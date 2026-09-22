#ifndef IMAGESCALER_H
#define IMAGESCALER_H

#include <cstdint>
#include <vector>

#include "av_utils.h"

// ============================================================================
// imagescaler.h —— 软件色彩空间/尺寸转换（libswscale 的 RAII 封装）
//
// 【在项目里的位置】
//   它是视频链路的【最后一道加工】，把解码器吐出来的原始帧转成"能直接
//   贴到界面上"的字节流：
//
//     解码器 → AVFrame(YUV420P/NV12/…) ──[ImageScaler]──> RGB24 字节流
//                                                              │
//                                              M12 包成 QImage ──┘
//
// 【它到底做了什么，没做什么】
//   做   ：像素格式转换（YUV → RGB24）。这是必须做的，因为 Qt 不认识 YUV。
//   不做 ：缩放。输出尺寸 = 源尺寸，交给 Qt 的 QPainter 去缩。
//
//   为什么不自己缩？三个理由：
//     ① 质量：sws 的 SWS_BILINEAR 是入门级滤波，Qt 的 SmoothTransformation
//        质量更好；
//     ② 稳定性：窗口尺寸随时会被用户拖动。如果按控件尺寸缩，每次 resize
//        都要重建 SwsContext（sws_getContext 不便宜）；按源尺寸转换则
//        SwsContext 只依赖"源参数"，一路稳定复用；
//     ③ 职责：显示尺寸是【界面层】的知识，不该渗进媒体处理层。
//   所以 sws_getContext 的源和目标分辨率【故意写成一样】，只换格式。
//
// 【为什么输出选 RGB24 而不是 BGRA/ARGB32】
//   QImage::Format_RGB888 的字节布局正好是"每像素 3 字节、行内紧凑无填充"，
//   和 AV_PIX_FMT_RGB24 一一对应，可以【零转换】直接构造 QImage。
//   选 32 位格式（ARGB32）的好处是 QPainter 绘制可能少一次内部转换，
//   代价是内存和带宽多 1/3 —— 对 1080p 逐帧传输，带宽是瓶颈，所以选 24 位。
//
// 【相对原工程的改造】
//   原 imagescaler.h 用裸 SwsContext* + 手工 malloc 缓存，且只在 DisplayWind
//   里用。现代化后：
//     * SwsContext 用 unique_ptr 管理；
//     * 输出固定为紧凑 RGB24；
//     * 源参数变化时自动（重）建上下文；
//     * 缓冲用 std::vector 复用（尺寸不变时不重新分配）；
//     * 与 Qt 彻底解耦 —— 本文件不包含任何 Qt 头文件。
// ============================================================================

class ImageScaler
{
public:
    ImageScaler() = default;
    ~ImageScaler() = default;

    // 把一帧转换为紧凑 RGB24（按源帧原始分辨率）。
    //
    // out       ：输出缓冲。会被自动扩容/复用；转换成功后
    //             out.size() == outWidth * outHeight * 3。
    // outWidth  /
    // outHeight ：出参，返回输出尺寸（当前实现必然等于源帧尺寸）。
    //
    // 返回 0 成功，负数为 FFmpeg 错误码（EINVAL / ENOMEM / EXTERNAL）。
    // 【为什么返回错误码而不是 bool】
    //   失败原因是可分的：参数非法、内存不足、sws 内部失败 ——
    //   排查时知道是哪一个很有用。
    int toRgb24(const AVFrame* frame, std::vector<uint8_t>& out,
                int& outWidth, int& outHeight);

private:
    // 检查并（必要时）重建 SwsContext。返回 false 表示建不出来。
    bool ensureContext(const AVFrame* frame);

    SwsContextPtr sws_;
    // 缓存"当前 sws_ 是按什么源参数建的"。只有这三个都相同才复用。
    int srcWidth_  = 0;
    int srcHeight_ = 0;
    int srcFormat_ = AV_PIX_FMT_NONE;
};

#endif // IMAGESCALER_H
