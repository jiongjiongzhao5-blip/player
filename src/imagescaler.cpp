#include "imagescaler.h"

// libavutil/pixdesc.h 提供 av_get_pix_fmt_name()，把像素格式枚举转成可读名字
extern "C" {
#include <libavutil/pixdesc.h>
}

// ---------------------------------------------------------------------------
// ensureContext —— SwsContext 的缓存与重建
// ---------------------------------------------------------------------------
// 【这是本类最需要想清楚的地方】
//
// sws_getContext() 不便宜（要算滤波器系数、分配内部缓冲），绝不能逐帧调用。
// 但源帧的参数也不是永远不变：分辨率中途改变、像素格式切换（比如某些流会
// 从 yuv420p 切到 yuvj420p）都要重建。
//
// 所以做一个"三要素缓存"：只有 (宽, 高, 像素格式) 三者都与上次相同才复用。
//
// ⚠ 一个容易忽略的扩展陷阱：
//   如果将来要支持"缩放到指定尺寸"，那么【目标尺寸也必须纳入比较】，
//   否则换了目标尺寸却复用了旧 context，会转出错误大小的画面。
//   当前实现之所以只比三样，是因为目标尺寸恒等于源尺寸、目标格式恒定。
// ---------------------------------------------------------------------------
bool ImageScaler::ensureContext(const AVFrame* frame)
{
    const int w   = frame->width;
    const int h   = frame->height;
    const int fmt = frame->format;      // 视频帧里这是 AVPixelFormat

    if (sws_ && w == srcWidth_ && h == srcHeight_ && fmt == srcFormat_)
        return true;                    // 命中缓存，直接复用

    sws_.reset(sws_getContext(w, h, static_cast<AVPixelFormat>(fmt),   // 源
                              w, h, AV_PIX_FMT_RGB24,                  // 目标（同尺寸，只换格式）
                              SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!sws_) {
        av_log(nullptr, AV_LOG_ERROR,
               "sws_getContext failed: %s %dx%d -> RGB24\n",
               av_get_pix_fmt_name(static_cast<AVPixelFormat>(fmt)), w, h);
        return false;
    }
    srcWidth_  = w;
    srcHeight_ = h;
    srcFormat_ = fmt;
    return true;
}

// ---------------------------------------------------------------------------
// toRgb24 —— 真正的转换
// ---------------------------------------------------------------------------
int ImageScaler::toRgb24(const AVFrame* frame, std::vector<uint8_t>& out,
                         int& outWidth, int& outHeight)
{
    // 防御：没解码成功的帧、或者尺寸异常的帧，直接拒绝。
    // 这种帧在真实播放中确实会出现（比如损坏的流），不能让它带着
    // 0 或负数尺寸一路走到 sws_scale 里去。
    if (!frame || frame->width <= 0 || frame->height <= 0)
        return AVERROR(EINVAL);
    if (!ensureContext(frame))
        return AVERROR(ENOMEM);

    outWidth  = frame->width;
    outHeight = frame->height;

    // resize 而不是重新构造：尺寸不变时 vector 不会重新分配，
    // 只改 size 计数。这样逐帧调用也不会反复 malloc/free ——
    // "复用缓冲"就是靠这一句实现的。
    out.resize(static_cast<size_t>(outWidth) * outHeight * 3);

    // 组装目标平面指针数组。
    //   * 源帧用 frame->data / frame->linesize —— YUV420P 是 3 个平面、
    //     NV12 是 2 个平面，sws 会按源格式自己决定读几个；
    //   * 目标只有一个平面，因为 RGB24 是 packed（每像素 3 字节连续排列）。
    //
    // 【为什么 dstLinesize 直接写 outWidth * 3，不做对齐？】
    //   原工程用 av_image_alloc() 分配，它会按 32 字节对齐每行，
    //   于是每行末尾有 padding —— 那样的缓冲不能直接交给 QImage
    //   （QImage 的 bytesPerLine 得单独传，且拷贝会变复杂）。
    //   我们用紧凑排列，代价是 sws 写入时可能略微降低 SIMD 效率，
    //   收益是下游零转换。对 1080p 逐帧传输这个取舍是划算的。
    uint8_t* dstData[1]     = { out.data() };
    int      dstLinesize[1] = { outWidth * 3 };

    // sws_scale 的参数顺序容易记错，背下来：
    //   (context, 源data, 源linesize, 源起始行, 源行数, 目标data, 目标linesize)
    // 返回"实际输出的行数"，正常等于 frame->height。
    const int scaled = sws_scale(sws_.get(), frame->data, frame->linesize,
                                 0, frame->height, dstData, dstLinesize);
    return scaled > 0 ? 0 : AVERROR_EXTERNAL;
}
