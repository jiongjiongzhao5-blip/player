// ============================================================================
// tools/make_test_media.cpp —— 测试素材生成器（沙盒工具，不属于最终工程）
//
// 【为什么需要它】
//   课程目录里没有任何音视频素材，vcpkg 也没有带 ffmpeg.exe。
//   而 M6 之后每个模块都需要真实文件来验证：解码、解复用、音频输出、
//   音视频同步、界面播放……全部依赖"有一个能播的文件"。
//
//   所以这里用 FFmpeg 的【编码】API 现场造一个：
//     视频：约 5 秒、640x480、25fps、带移动亮块（保证帧间有差异 → 产生 I/P 帧）
//     音频：约 5 秒、44100Hz 立体声、左右声道不同频率的正弦波
//     封装：mp4
//   编解码器优先选 H.264（更贴近真实场景），不可用则回退到 FFmpeg
//   内置的 MPEG-4 Part 2 —— 两者都是真实压缩编码，足够验证解码链路。
//
//   ⚠ 编码 API 比解码 API 复杂（要自己管帧缓冲、时基换算、交错写包），
//     这份代码只是工具，不必逐行深究；但它顺便演示了 FFmpeg 的
//     "对称性"：编码是解码的逆过程。
//
// 用法：make_test_media.exe [输出文件路径]
// ============================================================================

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>          // av_opt_set
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>

namespace {

constexpr int kWidth      = 640;
constexpr int kHeight     = 480;
constexpr int kFps        = 25;
constexpr int kSeconds    = 5;
constexpr int kSampleRate = 44100;
constexpr int kChannels   = 2;
constexpr double kPi      = 3.14159265358979323846;

constexpr int kVideoFrames = kFps * kSeconds;              // 125 帧
constexpr int kAudioTotal  = kSampleRate * kSeconds;       // 220500 个采样/声道

// ---------------------------------------------------------------------------
// 生成第 idx 帧的画面：一条随帧号平移的竖向渐变 + 一个移动的亮块。
// 关键是"每帧都不一样" —— 否则编码器会把它压成静止画面，解码测试就没意义了。
// ---------------------------------------------------------------------------
void fillVideoFrame(AVFrame* f, int idx)
{
    // Y 平面：竖向渐变，整体随帧号平移
    const int shift = idx * 4;
    for (int y = 0; y < kHeight; ++y) {
        uint8_t* row = f->data[0] + static_cast<ptrdiff_t>(y) * f->linesize[0];
        for (int x = 0; x < kWidth; ++x)
            row[x] = static_cast<uint8_t>((x + shift) & 0xFF);
    }

    // 移动的亮块（每帧右移 7px、下移 5px，到边界回绕）
    const int bx = (idx * 7) % (kWidth - 80);
    const int by = (idx * 5) % (kHeight - 80);
    for (int y = by; y < by + 80; ++y) {
        uint8_t* row = f->data[0] + static_cast<ptrdiff_t>(y) * f->linesize[0];
        std::memset(row + bx, 235, 80);
    }

    // U/V 平面：整体偏蓝（YUV420P 下 U/V 是 Y 的一半分辨率）
    for (int y = 0; y < kHeight / 2; ++y) {
        std::memset(f->data[1] + static_cast<ptrdiff_t>(y) * f->linesize[1], 160, kWidth / 2);
        std::memset(f->data[2] + static_cast<ptrdiff_t>(y) * f->linesize[2],  90, kWidth / 2);
    }
}

// ---------------------------------------------------------------------------
// 收包的通用小工具：把编码器当前攒下的所有包都写进输出文件
// ---------------------------------------------------------------------------
int drainEncoder(AVFormatContext* oc, AVCodecContext* c, AVStream* st)
{
    AVPacket* pkt = av_packet_alloc();
    for (;;) {
        const int ret = avcodec_receive_packet(c, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            av_packet_free(&pkt);
            return ret;
        }
        pkt->stream_index = st->index;
        // 把包的时间戳从"编码器时基"换算到"流时基"
        av_packet_rescale_ts(pkt, c->time_base, st->time_base);
        pkt->time_base = st->time_base;
        const int wret = av_interleaved_write_frame(oc, pkt);
        if (wret < 0) {
            av_packet_free(&pkt);
            return wret;
        }
    }
    av_packet_free(&pkt);
    return 0;
}

// ---------------------------------------------------------------------------
// 打开一个编码器并挂到新的流上
// ---------------------------------------------------------------------------
AVCodecContext* openStream(AVFormatContext* oc, AVStream** outStream,
                           AVCodecID wantId, const char* fallbackName)
{
    const AVCodec* codec = avcodec_find_encoder(wantId);
    if (!codec)
        codec = avcodec_find_encoder_by_name(fallbackName);
    if (!codec) {
        std::fprintf(stderr, "找不到编码器 %s\n", fallbackName);
        return nullptr;
    }
    std::printf("  使用编码器: %s\n", codec->name);

    AVStream* st = avformat_new_stream(oc, nullptr);
    if (!st)
        return nullptr;

    AVCodecContext* c = avcodec_alloc_context3(codec);
    if (!c)
        return nullptr;

    // mp4 这类容器需要把 SPS/PPS 等全局头写进文件头
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    *outStream = st;
    return c;
}

} // namespace

int main(int argc, char* argv[])
{
    SetConsoleOutputCP(CP_UTF8);

    const std::string outPath = (argc > 1) ? argv[1] : "test_media.mp4";

    std::printf("==================================================\n");
    std::printf(" 测试素材生成器\n");
    std::printf(" 目标: %s\n", outPath.c_str());
    std::printf(" 规格: %dx%d @ %dfps, %d 秒 | %dHz %d 声道\n",
                kWidth, kHeight, kFps, kSeconds, kSampleRate, kChannels);
    std::printf("==================================================\n");

    // -----------------------------------------------------------------------
    // 1. 输出上下文
    // -----------------------------------------------------------------------
    AVFormatContext* oc = nullptr;
    int ret = avformat_alloc_output_context2(&oc, nullptr, nullptr, outPath.c_str());
    if (ret < 0 || !oc) {
        std::fprintf(stderr, "无法根据扩展名推断封装格式\n");
        return 1;
    }
    std::printf("  封装格式: %s\n", oc->oformat->name);

    // -----------------------------------------------------------------------
    // 2. 视频流（优先 H.264，回退 MPEG-4 Part 2）
    // -----------------------------------------------------------------------
    AVStream* vst = nullptr;
    AVCodecContext* vc = openStream(oc, &vst, AV_CODEC_ID_H264, "mpeg4");
    if (!vc)
        return 1;
    // 注意：不要从 vst->codecpar 去读 codec_id —— 此刻它还是空的。
    // avcodec_alloc_context3(codec) 已经把 codec_id/type 填进上下文了。
    if (vc->codec_id == AV_CODEC_ID_NONE) {
        std::fprintf(stderr, "视频编码器上下文未正确初始化\n");
        return 1;
    }
    vc->width     = kWidth;
    vc->height    = kHeight;
    vc->time_base = AVRational{1, kFps};       // 视频以"帧"为时间单位最自然
    vc->framerate = AVRational{kFps, 1};
    vc->pix_fmt   = AV_PIX_FMT_YUV420P;
    vc->gop_size  = 12;                        // 每 12 帧一个关键帧
    vc->bit_rate  = 800000;
    // H.264 的 libx264 需要显式要求 yuv420p，否则可能选 yuv444p 导致容器不兼容
    if (vc->codec_id == AV_CODEC_ID_H264)
        av_opt_set(vc->priv_data, "preset", "veryfast", 0);

    ret = avcodec_open2(vc, nullptr, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "打开视频编码器失败\n");
        return 1;
    }
    avcodec_parameters_from_context(vst->codecpar, vc);
    vst->time_base = vc->time_base;

    // -----------------------------------------------------------------------
    // 3. 音频流（AAC，FFmpeg 内置编码器）
    // -----------------------------------------------------------------------
    AVStream* ast = nullptr;
    AVCodecContext* ac = openStream(oc, &ast, AV_CODEC_ID_AAC, "aac");
    if (!ac)
        return 1;
    ac->sample_rate = kSampleRate;
    av_channel_layout_default(&ac->ch_layout, kChannels);
    ac->time_base   = AVRational{1, kSampleRate};   // 音频以"采样"为时间单位
    ac->bit_rate    = 128000;

    // ★ FFmpeg 8/9 的破坏性变化：AVCodec 结构体已经私有化，
    //   原来直接读的 codec->sample_fmts / codec->pix_fmts / codec->priv_data_size
    //   等字段【全部移除】了（现在 AVCodec 只剩 name/type/id/capabilities 等 9 个字段）。
    //   要查询编码器支持哪些格式，必须改用 avcodec_get_supported_config()。
    //   这是 FFmpeg 的一贯趋势：把公开结构体的字段一步步藏到 getter API 后面，
    //   这样库内部改 ABI 时不会把用户代码一起打断。
    {
        ac->sample_fmt = AV_SAMPLE_FMT_NONE;
        const void* cfgList = nullptr;
        int cfgCount = 0;
        const int qret = avcodec_get_supported_config(ac, nullptr,
                                                     AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                                     0, &cfgList, &cfgCount);
        if (qret >= 0 && cfgList && cfgCount > 0) {
            const AVSampleFormat* fmts = static_cast<const AVSampleFormat*>(cfgList);
            ac->sample_fmt = fmts[0];
            std::printf("  编码器支持 %d 种采样格式，选用: %s\n",
                        cfgCount, av_get_sample_fmt_name(ac->sample_fmt));
        } else {
            ac->sample_fmt = AV_SAMPLE_FMT_FLTP;   // AAC 的常规选择（32 位浮点、分平面）
            std::printf("  查询采样格式失败，回退为 FLTP\n");
        }
    }
    // 注意：frame_size 要等 avcodec_open2 之后才由编码器填好，现在还是 0

    ret = avcodec_open2(ac, nullptr, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "打开音频编码器失败\n");
        return 1;
    }
    if (ac->frame_size <= 0) {
        std::fprintf(stderr, "音频编码器未给出 frame_size，无法分包\n");
        return 1;
    }
    std::printf("  音频采样格式: %s, 每帧 %d 个采样\n",
                av_get_sample_fmt_name(ac->sample_fmt), ac->frame_size);
    avcodec_parameters_from_context(ast->codecpar, ac);
    ast->time_base = ac->time_base;

    // -----------------------------------------------------------------------
    // 4. 打开输出文件、写文件头
    // -----------------------------------------------------------------------
    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&oc->pb, outPath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            std::fprintf(stderr, "无法创建输出文件: %s\n", outPath.c_str());
            return 1;
        }
    }
    ret = avformat_write_header(oc, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "写文件头失败\n");
        return 1;
    }

    // -----------------------------------------------------------------------
    // 5. 编码视频
    // -----------------------------------------------------------------------
    {
        AVFrame* frame = av_frame_alloc();
        frame->format = vc->pix_fmt;
        frame->width  = vc->width;
        frame->height = vc->height;
        av_frame_get_buffer(frame, 0);

        for (int i = 0; i < kVideoFrames; ++i) {
            av_frame_make_writable(frame);
            fillVideoFrame(frame, i);
            frame->pts = i;

            ret = avcodec_send_frame(vc, frame);
            if (ret < 0) {
                std::fprintf(stderr, "发送视频帧 %d 失败\n", i);
                break;
            }
            if (drainEncoder(oc, vc, vst) < 0) {
                std::fprintf(stderr, "写视频包失败\n");
                break;
            }
        }
        // flush 编码器：送入 nullptr 表示"没有更多输入了"
        avcodec_send_frame(vc, nullptr);
        drainEncoder(oc, vc, vst);
        av_frame_free(&frame);
        std::printf("  已编码视频 %d 帧\n", kVideoFrames);
    }

    // -----------------------------------------------------------------------
    // 6. 编码音频
    // -----------------------------------------------------------------------
    {
        AVFrame* frame = av_frame_alloc();
        frame->format      = ac->sample_fmt;
        frame->sample_rate = ac->sample_rate;
        frame->nb_samples  = ac->frame_size;
        av_channel_layout_copy(&frame->ch_layout, &ac->ch_layout);
        av_frame_get_buffer(frame, 0);

        int64_t written = 0;
        while (written < kAudioTotal) {
            av_frame_make_writable(frame);

            for (int ch = 0; ch < kChannels; ++ch) {
                float* p = reinterpret_cast<float*>(frame->data[ch]);
                // 左右声道用不同频率，解码后能听出"立体声"
                const double freq = 440.0 + 220.0 * ch;
                for (int n = 0; n < ac->frame_size; ++n) {
                    const int64_t idx = written + n;
                    if (idx >= kAudioTotal) {       // 最后不足一帧的部分补静音
                        p[n] = 0.0f;
                        continue;
                    }
                    const double t = idx / double(kSampleRate);
                    p[n] = static_cast<float>(0.3 * std::sin(2.0 * kPi * freq * t));
                }
            }
            frame->pts = written;

            ret = avcodec_send_frame(ac, frame);
            if (ret < 0) {
                std::fprintf(stderr, "发送音频帧失败\n");
                break;
            }
            if (drainEncoder(oc, ac, ast) < 0) {
                std::fprintf(stderr, "写音频包失败\n");
                break;
            }
            written += ac->frame_size;
        }
        avcodec_send_frame(ac, nullptr);
        drainEncoder(oc, ac, ast);
        av_frame_free(&frame);
        std::printf("  已编码音频 %lld 个采样（约 %.2f 秒）\n",
                    static_cast<long long>(written), written / double(kSampleRate));
    }

    // -----------------------------------------------------------------------
    // 7. 收尾：关闭输出句柄、释放上下文
    // -----------------------------------------------------------------------
    av_write_trailer(oc);

    if (!(oc->oformat->flags & AVFMT_NOFILE))
        avio_closep(&oc->pb);
    avcodec_free_context(&vc);
    avcodec_free_context(&ac);
    avformat_free_context(oc);

    // -----------------------------------------------------------------------
    // 8. 回读验证
    //    ★ 为什么不直接用上面那个输出上下文里的信息？
    //      因为 av_write_trailer 之后，输出上下文里的 st->duration 往往
    //      仍是 AV_NOPTS_VALUE（时长是写进文件头里的，不回填内存结构）。
    //      直接打印会得到 -7e14 这种垃圾数字。
    //      而且"回读一遍能解开"本身就是对文件合法性的最好验证 ——
    //      这一步等于顺便跑了一次解复用，正是下面 Decoder 要做的事。
    // -----------------------------------------------------------------------
    std::printf("\n  回读验证（确认文件合法可解）：\n");

    AVFormatContext* ic = nullptr;
    ret = avformat_open_input(&ic, outPath.c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "  ✗ 回读失败！文件可能已损坏\n");
        return 1;
    }
    avformat_find_stream_info(ic, nullptr);

    std::printf("    封装格式  : %s\n", ic->iformat->name);
    std::printf("    总时长    : %.3f 秒\n", ic->duration / double(AV_TIME_BASE));
    std::printf("    流数量    : %u\n", ic->nb_streams);

    for (unsigned i = 0; i < ic->nb_streams; ++i) {
        AVStream* st = ic->streams[i];
        const bool isVideo = (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);
        if (isVideo) {
            std::printf("      流 %u [视频] %s  %dx%d  time_base=%d/%d\n",
                        i, avcodec_get_name(st->codecpar->codec_id),
                        st->codecpar->width, st->codecpar->height,
                        st->time_base.num, st->time_base.den);
        } else {
            std::printf("      流 %u [音频] %s  %dHz %d声道  time_base=%d/%d\n",
                        i, avcodec_get_name(st->codecpar->codec_id),
                        st->codecpar->sample_rate,
                        st->codecpar->ch_layout.nb_channels,
                        st->time_base.num, st->time_base.den);
        }
    }
    avformat_close_input(&ic);

    std::printf("\n完成。文件已就绪，可以给 Decoder 测试用了。\n");
    return 0;
}
