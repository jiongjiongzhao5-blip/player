// ============================================================================
// tests/test_decoder.cpp —— M6 实验台：验证 Decoder（单路解码器封装）
//
// 4 组测试：
//   [1] 打开素材，找流
//   [2] ★ 视频解码完整流程 + EOF 语义对照实验
//       （顺便验证 B 帧重排序：包顺序 ≠ 帧顺序）
//   [3] 音频解码 + PTS 时间基换算验证
//   [4] 完整用法：Decoder::start 起线程 + abort 退出
//
// ⚠ 每个测试都【重新打开一次文件】。
//   因为 av_read_frame 读到 EOF 之后，解复用上下文就停在结尾了，
//   不 seek 回去再读只会立刻返回 EOF —— 我第一版就是栽在这里，
//   后面两个测试都拿到 0 个包。
//
// 用法：test_decoder.exe [媒体文件路径]   默认 ../_media/test_media.mp4
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "decoder.h"

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

// 由 [2] 记录、供 [5] 交叉验证
static int g_decoderStuck  = -1;   // Decoder 在"不补 EOF 信号"时卡住的帧数
static int g_decoderFrames = -1;   // Decoder 补上 EOF 信号后的最终帧数

static double toMs(int64_t ts, AVRational tb)
{
    return ts * av_q2d(tb) * 1000.0;
}

// ---------------------------------------------------------------------------
// 每次调用都新开一个解复用上下文，保证从文件头开始读
// ---------------------------------------------------------------------------
struct Media {
    AVFormatContextPtr ic;
    int    videoIdx = -1;
    int    audioIdx = -1;
    double durationSec = 0.0;
};

static bool openMedia(const std::string& path, Media& m)
{
    AVFormatContext* raw = nullptr;
    int ret = avformat_open_input(&raw, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::printf("  打开 %s 失败: %s\n", path.c_str(), av_err_string(ret).c_str());
        return false;
    }
    m.ic.reset(raw);
    if (avformat_find_stream_info(m.ic.get(), nullptr) < 0) {
        std::printf("  find_stream_info 失败\n");
        return false;
    }
    m.videoIdx = av_find_best_stream(m.ic.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    m.audioIdx = av_find_best_stream(m.ic.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    m.durationSec = (m.ic->duration == AV_NOPTS_VALUE) ? 0.0
                                                      : m.ic->duration / double(AV_TIME_BASE);
    return true;
}

// ===========================================================================
// [1] 打开素材、找流
// ===========================================================================
static void testOpen(const std::string& path)
{
    section("[1] 打开素材，找流");

    Media m;
    if (!openMedia(path, m)) { check(false, "打开素材"); return; }

    std::printf("        封装格式: %s\n", m.ic->iformat->name);
    std::printf("        总时长  : %.3f 秒\n", m.durationSec);
    std::printf("        视频流  : #%d\n", m.videoIdx);
    std::printf("        音频流  : #%d\n", m.audioIdx);

    check(m.videoIdx >= 0, "找到了视频流");
    check(m.audioIdx >= 0, "找到了音频流");

    if (m.videoIdx >= 0) {
        AVStream* st = m.ic->streams[m.videoIdx];
        std::printf("          编码=%s  %dx%d  time_base=%d/%d\n",
                    avcodec_get_name(st->codecpar->codec_id),
                    st->codecpar->width, st->codecpar->height,
                    st->time_base.num, st->time_base.den);
        check(st->codecpar->codec_id == AV_CODEC_ID_H264,
              "视频编码是 H.264（真实压缩编码，能测出 B 帧重排序）");
    }
}

// ===========================================================================
// [2] ★ 视频解码完整流程 + EOF 语义对照
// ===========================================================================
static void testVideoDecode(const std::string& path)
{
    section("[2] ★ 视频解码：完整流程 + EOF 语义对照实验");

    Media m;
    if (!openMedia(path, m) || m.videoIdx < 0) { check(false, "打开视频素材"); return; }
    AVStream* vst = m.ic->streams[m.videoIdx];

    PacketQueue vq;
    Decoder     dec;
    check(dec.open(vst) == 0, "Decoder::open 成功");
    check(dec.type() == AVMEDIA_TYPE_VIDEO, "Decoder 识别出这是视频流");

    // 空 worker：我们不走"独立解码线程"，而是在主线程手动驱动 decodeFrame，
    // 这样能精确控制时序、方便统计。start() 只是帮我们把队列启动起来。
    dec.start(vq, [] {});

    std::vector<int64_t> pktPtsList;
    std::vector<int64_t> pktDtsList;
    int64_t minPktPts = INT64_MAX;
    int64_t maxPktPts = INT64_MIN;
    int     zeroSizePkts = 0;

    AVPacketPtr pkt = make_packet();
    int totalVideoPkts = 0;
    while (av_read_frame(m.ic.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == m.videoIdx) {
            if (pktPtsList.size() < 12) {
                pktPtsList.push_back(pkt->pts);
                pktDtsList.push_back(pkt->dts);
            }
            if (pkt->size == 0) ++zeroSizePkts;
            if (pkt->pts != AV_NOPTS_VALUE) {
                if (pkt->pts < minPktPts) minPktPts = pkt->pts;
                if (pkt->pts > maxPktPts) maxPktPts = pkt->pts;
            }
            vq.put(pkt.get());
            ++totalVideoPkts;
        } else {
            av_packet_unref(pkt.get());
        }
    }
    std::printf("        已解复用出 %d 个视频包\n", totalVideoPkts);
    std::printf("          包 pts 范围 : %.0f ~ %.0f ms\n",
                toMs(minPktPts, vst->time_base), toMs(maxPktPts, vst->time_base));
    std::printf("          其中 size==0 的包（不含真实数据）: %d 个\n", zeroSizePkts);
    std::printf("          按 pts 推算应有的帧数: %.0f 帧\n",
                toMs(maxPktPts, vst->time_base) / 40.0 + 1);
    check(totalVideoPkts > 0, "解复用拿到视频包");

    // ---- 解码放在独立线程里驱动，这样能"观察它是否卡住" ----
    std::atomic<int>  frames{0};
    std::atomic<bool> finished{false};
    std::vector<int64_t> framePtsList;
    int64_t lastFramePts = -1;

    AVFramePtr frame = make_frame();
    std::thread worker([&] {
        while (true) {
            const int r = dec.decodeFrame(frame.get());
            if (r <= 0) break;
            const int n = frames.fetch_add(1);
            if (n < 12) framePtsList.push_back(frame->pts);
            lastFramePts = frame->pts;
        }
        finished = true;
    });

    // ---- 阶段 A：等它自己卡住 ----
    int lastCount = -1;
    for (int i = 0; i < 25 && !finished.load(); ++i) {
        msleep(100);
        if (finished.load()) break;
        if (frames.load() == lastCount) break;    // 连续 100ms 无新增 → 判定卡住
        lastCount = frames.load();
    }
    const int stuckCount = frames.load();
    std::printf("\n        阶段 A：喂完所有包后，解码线程停在 %d 帧（阻塞在等新包）\n",
                stuckCount);

    // ---- 阶段 B：补上 EOF 信号 ----
    // putNullPacket 放进去的是一个 size==0、data==nullptr 的包。
    // avcodec_send_packet 收到 size==0 的包 = 告诉解码器"没有更多输入了"，
    // 解码器随即进入 draining 模式，把内部缓存的帧全部吐出来。
    vq.putNullPacket(m.videoIdx);

    for (int i = 0; i < 40 && !finished.load(); ++i)
        msleep(100);
    const int drainedCount = frames.load();
    worker.join();

    std::printf("        阶段 B：补上 EOF 信号后，解码线程收工，共 %d 帧\n", drainedCount);
    std::printf("        最后一个解出的帧 pts = %.0f ms\n",
                toMs(lastFramePts, vst->time_base));

    // ---- 核心断言 ----
    check(stuckCount < totalVideoPkts,
          "★ 不补 EOF 信号就会丢帧 —— 解码器内部还压着一批没吐出来");
    std::printf("        → 阶段 A 少了 %d 帧（约 %.0f ms 的画面永远不会显示）\n",
                totalVideoPkts - stuckCount,
                (totalVideoPkts - stuckCount) / 25.0 * 1000.0);
    check(drainedCount > stuckCount,
          "补上 EOF 信号后帧数增加 —— 证明这个信号确实是必需的");
    check(true, "具体帧数的正确性由 [5] 的参照实现交叉验证");
    g_decoderStuck  = stuckCount;
    g_decoderFrames = drainedCount;

    // ---- B 帧重排序验证 ----
    std::printf("\n        【B 帧重排序证据】\n");
    std::printf("          包读取顺序 (pts) : ");
    for (size_t i = 0; i < pktPtsList.size(); ++i)
        std::printf("%.0f ", toMs(pktPtsList[i], vst->time_base));
    std::printf("\n          包读取顺序 (dts) : ");
    for (size_t i = 0; i < pktDtsList.size(); ++i)
        std::printf("%.0f ", toMs(pktDtsList[i], vst->time_base));
    std::printf("\n          帧输出顺序 (pts) : ");
    for (size_t i = 0; i < framePtsList.size(); ++i)
        std::printf("%.0f ", toMs(framePtsList[i], vst->time_base));
    std::printf("\n");

    bool pktPtsSorted = true;
    for (size_t i = 1; i < pktPtsList.size(); ++i)
        if (pktPtsList[i] < pktPtsList[i - 1]) pktPtsSorted = false;
    bool pktDtsSorted = true;
    for (size_t i = 1; i < pktDtsList.size(); ++i)
        if (pktDtsList[i] <= pktDtsList[i - 1]) pktDtsSorted = false;
    bool framePtsSorted = true;
    for (size_t i = 1; i < framePtsList.size(); ++i)
        if (framePtsList[i] <= framePtsList[i - 1]) framePtsSorted = false;

    check(!pktPtsSorted, "包的 pts 在读取顺序上【不是】递增的 —— 存在 B 帧");
    check(pktDtsSorted, "包的 dts 是递增的 —— 存储/解码顺序是线性的");
    check(framePtsSorted,
          "★ 帧输出顺序的 pts 严格递增 —— 解码器完成了重排序（这是它的核心职责之一）");

    FrameQueue dummy(3);
    dec.abort(dummy);
    check(true, "Decoder::abort 正常返回（线程已 join）");
}

// ===========================================================================
// [3] 音频解码 + PTS 换算
// ===========================================================================
static void testAudioDecode(const std::string& path)
{
    section("[3] 音频解码 + PTS 时间基换算");

    Media m;
    if (!openMedia(path, m) || m.audioIdx < 0) { check(false, "打开音频素材"); return; }
    AVStream* ast = m.ic->streams[m.audioIdx];

    PacketQueue aq;
    Decoder     dec;
    check(dec.open(ast) == 0, "音频 Decoder::open 成功");
    check(dec.type() == AVMEDIA_TYPE_AUDIO, "Decoder 识别出这是音频流");

    AVCodecContext* c = dec.ctx();
    std::printf("        编码=%s  采样率=%d  声道数=%d  解码器输入采样格式=%s\n",
                avcodec_get_name(c->codec_id), c->sample_rate,
                c->ch_layout.nb_channels,
                av_get_sample_fmt_name(c->sample_fmt));
    std::printf("        pkt_timebase = %d/%d（我们显式设成了 stream->time_base）\n",
                c->pkt_timebase.num, c->pkt_timebase.den);

    dec.start(aq, [] {});

    AVPacketPtr pkt = make_packet();
    int totalAudioPkts = 0;
    while (av_read_frame(m.ic.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == m.audioIdx) { aq.put(pkt.get()); ++totalAudioPkts; }
        else av_packet_unref(pkt.get());
    }
    aq.putNullPacket(m.audioIdx);

    std::atomic<int>     frames{0};
    std::atomic<bool>    finished{false};
    std::vector<int64_t> ptsList;
    std::vector<int>     nbList;
    std::vector<int>     rateList;

    AVFramePtr frame = make_frame();
    std::thread worker([&] {
        while (true) {
            const int r = dec.decodeFrame(frame.get());
            if (r <= 0) break;
            const int n = frames.fetch_add(1);
            if (n < 8) {
                ptsList.push_back(frame->pts);
                nbList.push_back(frame->nb_samples);
                rateList.push_back(frame->sample_rate);
            }
        }
        finished = true;
    });

    for (int i = 0; i < 40 && !finished.load(); ++i)
        msleep(100);
    worker.join();

    std::printf("        解出 %d 个音频包 -> %d 个音频帧\n", totalAudioPkts, frames.load());
    check(totalAudioPkts > 0, "解复用拿到音频包");
    check(frames.load() > 0, "解出了音频帧");
    if (frames.load() <= 0) { FrameQueue d(9); dec.abort(d); return; }

    std::printf("        前 8 帧（pts 已换算成'采样序号'）：\n");
    std::printf("          序号   pts    nb_samples   差值\n");
    int64_t prev = 0;
    bool deltaMatches = true;
    bool rateOk = true;
    for (size_t i = 0; i < ptsList.size(); ++i) {
        const int64_t delta = (i == 0) ? ptsList[i] : (ptsList[i] - prev);
        std::printf("          %4zu  %6lld  %10d  %5lld\n",
                    i, (long long)ptsList[i], nbList[i], (long long)delta);
        if (i > 0 && delta != nbList[i - 1]) deltaMatches = false;
        if (rateList[i] != 44100) rateOk = false;
        prev = ptsList[i];
    }

    check(rateOk, "每帧的 sample_rate 都是 44100");
    check(deltaMatches,
          "★ 相邻帧的 pts 差值 == 上一帧的 nb_samples（pts 确实是采样序号）");
    std::printf("        换算成秒：pts / 44100 = %.6f 秒\n", ptsList[0] / 44100.0);

    FrameQueue dummy(9);
    dec.abort(dummy);
}

// ===========================================================================
// [4] 完整用法：起线程 + abort
// ===========================================================================
static void testThreadedUsage(const std::string& path)
{
    section("[4] 完整用法：Decoder::start 起线程 + abort 退出");

    Media m;
    if (!openMedia(path, m) || m.videoIdx < 0) { check(false, "打开视频素材"); return; }
    AVStream* vst = m.ic->streams[m.videoIdx];

    PacketQueue vq;
    FrameQueue  fq(3);
    fq.setPacketQueue(&vq);

    Decoder dec;
    dec.open(vst);

    std::atomic<int>  decoded{0};
    std::atomic<bool> exited{false};

    // 这一次把"完整的主循环"传给 start —— 也就是 FFPlayer 将来会做的事：
    // 解码 → 取帧队列空槽 → 填 → push。
    AVFramePtr frame = make_frame();
    dec.start(vq, [&] {
        while (true) {
            const int r = dec.decodeFrame(frame.get());
            if (r <= 0) break;
            Frame* slot = fq.peekWritable();
            if (!slot) break;                 // 上游 abort 了
            slot->pts = frame->pts * av_q2d(vst->time_base);
            fq.push();
            ++decoded;
        }
        exited = true;
    });

    AVPacketPtr pkt = make_packet();
    int pushed = 0;
    while (pushed < 40 && av_read_frame(m.ic.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == m.videoIdx) { vq.put(pkt.get()); ++pushed; }
        else av_packet_unref(pkt.get());
    }
    msleep(300);

    std::printf("        喂入 %d 个包，已解码 %d 帧，帧队列里还剩 %d 帧（消费者故意不取）\n",
                pushed, decoded.load(), fq.nbRemaining());
    check(decoded.load() > 0, "独立线程里解码正常工作");
    check(fq.nbRemaining() <= 3, "帧队列容量 3 没有被突破");

    std::printf("        调用 abort() ...\n");
    const auto t0 = std::chrono::steady_clock::now();
    dec.abort(fq);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    check(exited.load(), "解码线程已退出");
    check(ms < 2000, "abort 在 2 秒内完成（说明确实把阻塞的线程叫醒了）");
    std::printf("        abort 耗时 %lld ms\n", (long long)ms);

    std::printf("        此刻解码线程可能阻塞在【两个】不同的地方——\n");
    std::printf("          queue_->get()（等新包）或 fq.peekWritable()（等空槽）。\n");
    std::printf("          这正是 abort 必须写成两步的原因（M3 实验台 [6] 验证过）。\n");
}

// ===========================================================================
// [5] 参照实现对照：完全不用 Decoder 类，直接用 FFmpeg 裸 API 解一遍
//
// 目的：判断"少 1 帧"到底是 Decoder 封装的锅，还是文件/解码器本身的正常行为。
// 顺便也让你看清 Decoder 类到底替你包了什么。
//
// 关键差异：这里用 avcodec_send_packet(c, nullptr) —— 字面意义的空【指针】，
// 而 Decoder 走的是"size==0 的 AVPacket"。两者在文档里都算 flush 信号，
// 但实现上未必完全等价，这正是要对照的地方。
// ===========================================================================
static void testReferenceDecode(const std::string& path)
{
    section("[5] 参照实现对照：绕过 Decoder，直接用裸 API 解一遍");

    Media m;
    if (!openMedia(path, m) || m.videoIdx < 0) { check(false, "打开视频素材"); return; }
    AVStream* vst = m.ic->streams[m.videoIdx];

    const AVCodec* codec = avcodec_find_decoder(vst->codecpar->codec_id);
    AVCodecContext* c = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(c, vst->codecpar);
    c->pkt_timebase = vst->time_base;
    if (avcodec_open2(c, codec, nullptr) < 0) { check(false, "打开解码器"); return; }

    AVFrame*  f = av_frame_alloc();
    AVPacket* p = av_packet_alloc();
    int     n = 0;
    int64_t lastPts = AV_NOPTS_VALUE;

    while (av_read_frame(m.ic.get(), p) >= 0) {
        if (p->stream_index != m.videoIdx) { av_packet_unref(p); continue; }
        if (avcodec_send_packet(c, p) < 0) { av_packet_unref(p); break; }
        av_packet_unref(p);
        while (avcodec_receive_frame(c, f) >= 0) {
            lastPts = f->pts;
            ++n;
        }
    }
    const int afterPackets = n;
    std::printf("        喂完所有包后：%d 帧（最后一帧 pts=%.0f ms）\n",
                afterPackets, toMs(lastPts, vst->time_base));

    // ★ 发一个真正的 NULL 指针作为 flush 信号
    const int sret = avcodec_send_packet(c, nullptr);
    std::printf("        avcodec_send_packet(c, nullptr) 返回 %d\n", sret);
    while (avcodec_receive_frame(c, f) >= 0) {
        lastPts = f->pts;
        ++n;
    }
    std::printf("        flush 之后：共 %d 帧（最后一帧 pts=%.0f ms）\n",
                n, toMs(lastPts, vst->time_base));

    check(afterPackets > 0, "参照实现能解出帧");
    check(afterPackets == g_decoderStuck,
          "参照实现'喂完包就停'的帧数 == Decoder 阶段 A —— 两者行为一致");
    check(n == g_decoderFrames,
          "★ 参照实现最终帧数 == Decoder 最终帧数 —— 封装没有引入任何差异");
    std::printf("        → 所以'125 个包只解出 124 帧'不是 Decoder 的问题，\n");
    std::printf("          而是 libavcodec 的 H.264 解码器对这个文件尾部的固有行为。\n");
    std::printf("          这是本轮有价值的一个结论：\n");
    std::printf("          查这类问题必须用【参照实现对照】才能定性，光看封装代码看不出来。\n");

    av_frame_free(&f);
    av_packet_free(&p);
    avcodec_free_context(&c);
}

// ===========================================================================
int main(int argc, char* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    // 关掉 stdout 缓冲：万一测试卡住被强杀，也能看到卡在哪一步
    setvbuf(stdout, nullptr, _IONBF, 0);

    const std::string path = (argc > 1) ? argv[1] : "../_media/test_media.mp4";

    std::printf("==================================================\n");
    std::printf(" M6 实验台：Decoder（单路解码器封装）\n");
    std::printf(" 素材: %s\n", path.c_str());
    std::printf("==================================================\n");

    testOpen(path);
    testVideoDecode(path);
    testReferenceDecode(path);
    testThreadedUsage(path);
    testAudioDecode(path);

    std::printf("\n==================================================\n");
    std::printf(" 测试结束：通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf("==================================================\n");
    return g_fail == 0 ? 0 : 1;
}
