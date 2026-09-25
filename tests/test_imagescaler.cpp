// ============================================================================
// tests/test_imagescaler.cpp —— M7 实验台 A：ImageScaler（YUV -> RGB24）
//
// 4 组测试：
//   [1] 用 Decoder 解出若干视频帧
//   [2] 逐帧转换：尺寸/大小正确 + 输出缓冲被复用（不反复分配）
//   [3] ★ 通道顺序验证：转换结果应该整体偏蓝（源是 U=160/V=90）
//       —— 这是抓"R/B 搞反"这类经典 bug 的手段
//   [4] 导出 PNG 快照，供人眼确认
//   [5] 错误路径：空帧 / 非法尺寸
//
// 用法：test_imagescaler.exe [媒体文件路径] [PNG 输出路径]
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "decoder.h"
#include "imagescaler.h"

extern "C" {
#include <libavutil/imgutils.h>
}

// ---------------------------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* desc)
{
    if (cond) { ++g_pass; std::printf("  [PASS] %s\n", desc); }
    else      { ++g_fail; std::printf("  [FAIL] %s\n", desc); }
}

static void section(const char* title)
{
    std::printf("\n==================================================\n");
    std::printf(" %s\n", title);
    std::printf("==================================================\n");
}

static void msleep(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ---------------------------------------------------------------------------
// 把 RGB24 紧凑缓冲写成 JPEG。
//
// 【为什么不是 PNG】这个方法我原本写的是 PNG，结果发现
//   avcodec_find_encoder(AV_CODEC_ID_PNG) 返回空 ——
//   这个 FFmpeg 构建里【有 PNG 解码器但没有 PNG 编码器】（PNG 编码需要 zlib，
//   vcpkg 的这个构建没带）。实测可用的图像编码器只有：
//     bmp / ppm / tiff（能直接吃 rgb24）、mjpeg（只吃 YUV）
//   为了产物"到处都能打开"，选 MJPEG（.jpg）—— 代价是要先做一次
//   RGB24 -> YUVJ420P 的转换。
//
// 【顺带说一句】这次转换用 sws 做，方向正好和 ImageScaler 相反，
//   可以顺便看到 sws 本身是双向的：它只是"格式转换器"，
//   YUV→RGB 还是 RGB→YUV 由你给的参数决定。
// ---------------------------------------------------------------------------
static bool saveJpeg(const std::string& path, const std::vector<uint8_t>& rgb,
                     int w, int h)
{
    // ① RGB24 -> YUVJ420P（MJPEG 只接受 YUV，且要 JPEG 全范围）
    SwsContext* sws = sws_getContext(w, h, AV_PIX_FMT_RGB24,
                                     w, h, AV_PIX_FMT_YUVJ420P,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) { std::printf("        建转换上下文失败\n"); return false; }

    AVFrame* yuv = av_frame_alloc();
    yuv->format = AV_PIX_FMT_YUVJ420P;
    yuv->width  = w;
    yuv->height = h;
    if (av_frame_get_buffer(yuv, 0) < 0) {
        sws_freeContext(sws); av_frame_free(&yuv); return false;
    }
    const uint8_t* srcData[1] = { rgb.data() };
    int            srcStride[1] = { w * 3 };
    sws_scale(sws, srcData, srcStride, 0, h, yuv->data, yuv->linesize);
    sws_freeContext(sws);

    // ② MJPEG 编码
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) { std::printf("        找不到 MJPEG 编码器\n"); av_frame_free(&yuv); return false; }

    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width       = w;
    c->height      = h;
    c->pix_fmt     = AV_PIX_FMT_YUVJ420P;
    c->time_base   = AVRational{1, 25};
    c->color_range = AVCOL_RANGE_JPEG;
    int ret = avcodec_open2(c, codec, nullptr);
    if (ret < 0) {
        std::printf("        avcodec_open2 失败: %s\n", av_err_string(ret).c_str());
        avcodec_free_context(&c); av_frame_free(&yuv); return false;
    }
    yuv->pts = 0;

    bool ok = false;
    AVPacket* p = av_packet_alloc();
    ret = avcodec_send_frame(c, yuv);
    if (ret < 0)
        std::printf("        send_frame 失败: %s\n", av_err_string(ret).c_str());

    // 图像类编码器（intra-only）有时要等 flush 才吐包，收不到就补一次
    ret = avcodec_receive_packet(c, p);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        avcodec_send_frame(c, nullptr);
        ret = avcodec_receive_packet(c, p);
    }
    if (ret < 0) {
        std::printf("        receive_packet 失败: %s\n", av_err_string(ret).c_str());
    } else {
        FILE* fp = std::fopen(path.c_str(), "wb");
        if (!fp) {
            std::printf("        文件打不开: %s\n", path.c_str());
        } else {
            std::fwrite(p->data, 1, static_cast<size_t>(p->size), fp);
            std::fclose(fp);
            std::printf("        JPEG 大小 %d 字节\n", p->size);
            ok = true;
        }
    }

    av_packet_free(&p);
    av_frame_free(&yuv);
    avcodec_free_context(&c);
    return ok;
}

// ---------------------------------------------------------------------------
// 统计 RGB 三通道均值 —— 用来验证通道顺序没搞反
// ---------------------------------------------------------------------------
struct RgbStats {
    double avgR = 0, avgG = 0, avgB = 0;
};

static RgbStats analyze(const std::vector<uint8_t>& rgb)
{
    RgbStats s;
    if (rgb.empty()) return s;
    uint64_t sumR = 0, sumG = 0, sumB = 0;
    const size_t n = rgb.size() / 3;
    for (size_t i = 0; i < n; ++i) {
        sumR += rgb[i * 3 + 0];
        sumG += rgb[i * 3 + 1];
        sumB += rgb[i * 3 + 2];
    }
    s.avgR = double(sumR) / n;
    s.avgG = double(sumG) / n;
    s.avgB = double(sumB) / n;
    return s;
}

// ===========================================================================
int main(int argc, char* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const std::string mediaPath = (argc > 1) ? argv[1] : "../_media/test_media.mp4";
    const std::string pngPath   = (argc > 2) ? argv[2] : "../_media/frame_preview.jpg";

    std::printf("==================================================\n");
    std::printf(" M7 实验台 A：ImageScaler（YUV -> RGB24）\n");
    std::printf(" 素材: %s\n", mediaPath.c_str());
    std::printf("==================================================\n");

    // -----------------------------------------------------------------------
    section("[1] 用 Decoder 解出若干视频帧");
    // -----------------------------------------------------------------------
    AVFormatContext* ic = nullptr;
    if (avformat_open_input(&ic, mediaPath.c_str(), nullptr, nullptr) < 0) {
        std::printf("  打不开素材\n"); return 1;
    }
    avformat_find_stream_info(ic, nullptr);
    const int vIdx = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vIdx < 0) { std::printf("  没有视频流\n"); return 1; }

    PacketQueue vq;
    Decoder dec;
    dec.open(ic->streams[vIdx]);
    vq.start();                 // ★ 调用契约：先启动队列
    dec.start(vq, [] {});

    AVPacket* pkt = av_packet_alloc();
    int pushed = 0;
    while (pushed < 60 && av_read_frame(ic, pkt) >= 0) {
        if (pkt->stream_index == vIdx) { vq.put(pkt); ++pushed; }
        else av_packet_unref(pkt);
    }
    std::printf("        喂入 %d 个视频包\n", pushed);
    check(pushed > 0, "拿到视频包");

    // ★★ 关键一步：必须补上 EOF 信号！
    //   M6 已经验证过：只喂包不发 flush 信号的话，解码器吐完能吐的就会
    //   一直阻塞在 queue_->get() 等新包 —— 表现为程序永久卡住。
    //   这里我亲身又踩了一次（第一版忘了写，测试直接超时）。
    //   正式工程里这一步该由 FFPlayer::readThread 在 EOF 时做（见 M6 的发现 4）。
    vq.putNullPacket(vIdx);

    // -----------------------------------------------------------------------
    section("[2] 逐帧转换：尺寸 / 大小 / 缓冲复用");
    // -----------------------------------------------------------------------
    ImageScaler scaler;
    std::vector<uint8_t> rgb;
    AVFrame* frame = av_frame_alloc();
    int  frames = 0;
    int  expectW = 0, expectH = 0;
    bool sizeOk = true, reuseOk = true;
    uint8_t* firstBuf = nullptr;
    std::vector<uint8_t> keepForPng;

    while (true) {
        const int r = dec.decodeFrame(frame);
        if (r <= 0) break;

        int w = 0, h = 0;
        if (scaler.toRgb24(frame, rgb, w, h) < 0) {
            std::printf("        toRgb24 失败\n");
            sizeOk = false;
            break;
        }

        if (frames == 0) {
            expectW = w; expectH = h;
            firstBuf = rgb.data();
            std::printf("        第一帧: %dx%d -> RGB24 %zu 字节（期望 %d）\n",
                        w, h, rgb.size(), w * h * 3);
        } else {
            // 尺寸必须始终一致
            if (w != expectW || h != expectH) sizeOk = false;
            // 缓冲地址必须不变 —— 证明 vector 没有重新分配
            if (rgb.data() != firstBuf) reuseOk = false;
        }
        if (rgb.size() != static_cast<size_t>(w) * h * 3) sizeOk = false;

        if (frames == 40)
            keepForPng = rgb;      // 留一帧给 PNG 导出（后面还会用）

        ++frames;
    }

    std::printf("        共转换 %d 帧\n", frames);
    check(frames > 40, "转换了 40 帧以上");
    check(sizeOk, "所有帧的输出大小都恰好等于 w * h * 3（紧凑无填充）");
    check(reuseOk,
          "★ 输出缓冲地址全程不变 —— vector 被复用，没有反复分配");

    // -----------------------------------------------------------------------
    section("[3] ★ 通道顺序验证：结果应该整体偏蓝");
    // -----------------------------------------------------------------------
    // 素材生成器里设的是 U(Cb)=160、V(Cr)=90。
    // YUV -> RGB 的近似公式：
    //     R = Y + 1.402*(V-128)      -> 因为 V<128，红色被压低
    //     G = Y - 0.344*(U-128) - 0.714*(V-128)
    //     B = Y + 1.772*(U-128)      -> 因为 U>128，蓝色被抬高
    // 所以理论上 B > G > R。
    // 如果谁把 RGB24 写成了 BGR24，这个不等式就会反过来 —— 这正是要抓的 bug。
    const RgbStats st = analyze(keepForPng);
    std::printf("        第 41 帧的三通道均值: R=%.1f  G=%.1f  B=%.1f\n",
                st.avgR, st.avgG, st.avgB);
    check(!keepForPng.empty(), "拿到了用于分析的一帧");
    check(st.avgB > st.avgR + 20.0,
          "★ B 通道明显高于 R 通道 —— 通道顺序正确（没把 RGB 写成 BGR）");
    check(st.avgG > st.avgR && st.avgB > st.avgG,
          "三通道大小关系符合 YUV(U=160,V=90) 的理论预期 B > G > R");

    // -----------------------------------------------------------------------
    section("[4] 导出快照（JPEG）");
    // -----------------------------------------------------------------------
    if (!keepForPng.empty()) {
        if (saveJpeg(pngPath, keepForPng, expectW, expectH)) {
            std::printf("        已写出: %s (%dx%d)\n", pngPath.c_str(), expectW, expectH);
            check(true, "JPEG 导出成功 —— 可以打开看看颜色和移动方块的位置对不对");
        } else {
            check(false, "JPEG 导出失败");
        }
    }

    // -----------------------------------------------------------------------
    section("[5] 错误路径");
    // -----------------------------------------------------------------------
    {
        int w = 0, h = 0;
        std::vector<uint8_t> tmp;
        check(scaler.toRgb24(nullptr, tmp, w, h) < 0, "传 nullptr 帧被拒绝（返回负错误码）");

        AVFrame* bad = av_frame_alloc();       // 全新帧：width/height 都是 0
        const int r = scaler.toRgb24(bad, tmp, w, h);
        std::printf("        尺寸为 0 的帧 -> 返回 %d (%s)\n",
                    r, av_err_string(r).c_str());
        check(r == AVERROR(EINVAL), "尺寸非法的帧返回 EINVAL");
        av_frame_free(&bad);
    }

    // -----------------------------------------------------------------------
    // 收尾
    // -----------------------------------------------------------------------
    av_frame_free(&frame);
    av_packet_free(&pkt);
    FrameQueue dummy(3);
    dec.abort(dummy);
    avformat_close_input(&ic);

    std::printf("\n==================================================\n");
    std::printf(" 测试结束：通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf("==================================================\n");
    return g_fail == 0 ? 0 : 1;
}
