// ============================================================================
// tests/test_ffplayer_demux.cpp —— M8 实验台：FFPlayer 解复用阶段
//
// 6 组测试：
//   [1] 错误路径：prepare 一个不存在的文件（异步报错）
//   [2] 正常路径：事件序列 OpenInput -> FindStreamInfo -> ComponentOpen -> Prepared
//   [3] 包分发统计：两路队列拿到的包数是否与素材一致
//   [4] ★ EOF 信号验证：队列末尾必须有"空包"（M6 修复的直接证据）
//   [5] seek：serial 递增 + 队列被重置 + 落点正确
//   [6] close：干净退出、可重复调用
//
// 用法：test_ffplayer.exe [媒体文件路径]
// ============================================================================

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "ffplayer.h"

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
// 参考信息：自己解复用一遍素材，拿到"标准答案"用来对照 FFPlayer 的结果
// ---------------------------------------------------------------------------
struct RefInfo {
    int    videoIdx = -1, audioIdx = -1;
    int    videoPkts = 0, audioPkts = 0;
    AVRational videoTb{};
    int64_t durationMs = -1;
};

static bool buildRef(const std::string& path, RefInfo& r)
{
    AVFormatContext* ic = nullptr;
    if (avformat_open_input(&ic, path.c_str(), nullptr, nullptr) < 0) return false;
    avformat_find_stream_info(ic, nullptr);

    r.videoIdx = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    r.audioIdx = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (r.videoIdx >= 0) r.videoTb = ic->streams[r.videoIdx]->time_base;
    if (ic->duration != AV_NOPTS_VALUE) r.durationMs = ic->duration / (AV_TIME_BASE / 1000);

    AVPacket* p = av_packet_alloc();
    while (av_read_frame(ic, p) >= 0) {
        if (p->stream_index == r.videoIdx)      ++r.videoPkts;
        else if (p->stream_index == r.audioIdx) ++r.audioPkts;
        av_packet_unref(p);
    }
    av_packet_free(&p);
    avformat_close_input(&ic);
    return true;
}

// ---------------------------------------------------------------------------
// 轮询消息队列直到出现 stopAt（或超时）。把途经的所有消息记进 out。
// 用非阻塞 get + 轮询，是为了避免"消息不来就永远挂住"导致测试卡死。
// ---------------------------------------------------------------------------
struct Rec { FFMsg what; int arg1; };

static bool waitUntil(FFPlayer& p, FFMsg stopAt, int timeoutMs, std::vector<Rec>* out)
{
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0).count();
        if (elapsed > timeoutMs)
            return false;

        Message m;
        const int r = p.messages().get(m, false);
        if (r == 1) {
            if (m.what != FFMsg::FLUSH)          // FLUSH 是哨兵，不算业务事件
                out->push_back(Rec{m.what, m.arg1});
            if (m.what == stopAt)
                return true;
        } else if (r < 0) {
            return false;                        // 队列被 abort
        } else {
            msleep(5);
        }
    }
}

static const char* msgName(FFMsg m)
{
    switch (m) {
    case FFMsg::OpenInput:      return "OpenInput";
    case FFMsg::FindStreamInfo: return "FindStreamInfo";
    case FFMsg::ComponentOpen:  return "ComponentOpen";
    case FFMsg::Prepared:       return "Prepared";
    case FFMsg::Completed:      return "Completed";
    case FFMsg::Error:          return "Error";
    default:                    return "?";
    }
}

// ===========================================================================
int main(int argc, char* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const std::string path = (argc > 1) ? argv[1] : "../_media/test_media.mp4";

    std::printf("==================================================\n");
    std::printf(" M8 实验台：FFPlayer 解复用阶段\n");
    std::printf(" 素材: %s\n", path.c_str());
    std::printf("==================================================\n");

    RefInfo ref;
    if (!buildRef(path, ref)) { std::printf("  参考解复用失败\n"); return 1; }
    std::printf("  [参考] 视频包 %d 个，音频包 %d 个，总时长 %lld ms\n",
                ref.videoPkts, ref.audioPkts, (long long)ref.durationMs);

    // -----------------------------------------------------------------------
    section("[1] 错误路径：prepare 一个不存在的文件");
    // -----------------------------------------------------------------------
    {
        FFPlayer p;
        const int r = p.prepare("no_such_file_12345.mp4");
        check(r == 0, "prepare 返回 0（只表示线程启动成功，不代表文件打开成功）");

        std::vector<Rec> msgs;
        const bool gotError = waitUntil(p, FFMsg::Error, 3000, &msgs);
        check(gotError, "★ 通过【消息】收到了 Error —— 印证了 prepare 是异步的");
        if (gotError) {
            int code = 0;
            for (const auto& m : msgs) if (m.what == FFMsg::Error) code = m.arg1;
            std::printf("        错误码 %d -> %s\n", code, av_err_string(code).c_str());
            check(code < 0, "Error 消息的 arg1 是负的 FFmpeg 错误码");
        }
        std::printf("        （对比：如果 prepare 是同步的，返回值就能表达失败，\n");
        std::printf("          但代价是打开慢文件时 UI 会卡住 —— 这是刻意的取舍）\n");
        p.close();
    }

    // -----------------------------------------------------------------------
    section("[2] 正常路径：事件序列");
    // -----------------------------------------------------------------------
    FFPlayer player;
    {
        const int r = player.prepare(path);
        check(r == 0, "prepare 返回 0");

        std::vector<Rec> msgs;
        const bool gotPrepared = waitUntil(player, FFMsg::Prepared, 5000, &msgs);
        check(gotPrepared, "收到 Prepared（文件打开 + 解码器就绪）");

        std::printf("        事件序列: ");
        for (const auto& m : msgs) std::printf("%s ", msgName(m.what));
        std::printf("\n");

        const std::vector<FFMsg> expect = {
            FFMsg::OpenInput, FFMsg::FindStreamInfo,
            FFMsg::ComponentOpen, FFMsg::Prepared
        };
        bool seqOk = (msgs.size() == expect.size());
        if (seqOk)
            for (size_t i = 0; i < expect.size(); ++i)
                if (msgs[i].what != expect[i]) seqOk = false;
        check(seqOk, "★ 四个事件严格按 OpenInput/FindStreamInfo/ComponentOpen/Prepared 顺序到达");

        std::printf("        durationMs() = %lld（参考值 %lld）\n",
                    (long long)player.durationMs(), (long long)ref.durationMs);
        check(player.durationMs() > 4000 && player.durationMs() < 6000,
              "durationMs 在 4~6 秒之间，符合 5 秒素材");

        // 诊断：队列有没有被 start 过？看 serial 就知道（未 start 则恒为 0）
        std::printf("        [诊断] 参考流索引: 视频 #%d 音频 #%d\n",
                    ref.videoIdx, ref.audioIdx);
        std::printf("        [诊断] 音频队列 serial=%d 条目=%d | 视频队列 serial=%d 条目=%d\n",
                    player.audioQueue().serial(), player.audioQueue().nbPackets(),
                    player.videoQueue().serial(), player.videoQueue().nbPackets());
    }

    // -----------------------------------------------------------------------
    section("[3] 包分发统计");
    // -----------------------------------------------------------------------
    {
        // ★ M10 改了 Completed 的语义：它现在表示【播放真正结束】，
        //   而不是"解复用读完"。所以这里不能再等它 —— 本实验台关心的是
        //   "包有没有被分发完"，用固定等待更直接：126KB 的素材，
        //   readThread 几十毫秒就能全部读完并塞进队列。给 1.5 秒绰绰有余。
        msleep(1500);
        std::printf("        （等待 1.5 秒，让 readThread 把整个文件读完并分发）\n");

        // ★★ 观察方式变了（M10 修正）★★
        //   M8 时我们靠"队列里躺着多少条"来验证分发总量，那在【还没有消费者】
        //   的时候成立。但 M9/M10 加了音频、视频解码线程之后，两个队列都变成
        //   了"流水线上的一小段"，残留量随时序波动 —— 刚读到的数字下次跑就
        //   不一样了（实测从 127 变成 81）。
        //   所以改用【分发计数器】：它记录 readThread 一共往外投了多少包，
        //   与消费速度完全无关，什么时候读都准确。
        const auto d = player.demuxInfo();
        std::printf("        分发给音频队列: %d 个真实包（参考 %d）\n",
                    d.audioPkts, ref.audioPkts);
        std::printf("        分发给视频队列: %d 个真实包（参考 %d）\n",
                    d.videoPkts, ref.videoPkts);
        std::printf("        当前队列残留  : 音频 %d，视频 %d（仅是瞬时值）\n",
                    player.audioQueue().nbPackets(), player.videoQueue().nbPackets());

        check(d.audioPkts == ref.audioPkts, "★ 音频包一个不多一个不少地分发完了");
        check(d.videoPkts == ref.videoPkts, "★ 视频包一个不多一个不少地分发完了");
        check(player.audioQueue().serial() >= 1, "音频队列已被启动（serial >= 1）");
        check(player.videoQueue().serial() >= 1, "视频队列已被启动（serial >= 1）");

        msleep(300);
        const auto info = player.audioOutInfo();
        std::printf("        音频解码线程已解出 %d 帧\n", info.decodedFrames);
        check(info.decodedFrames > 0,
              "★ 下游消费者确实在工作 —— 分发出去的数据被实时消化了");
    }

    // -----------------------------------------------------------------------
    section("[4] ★ EOF 信号验证（M6 修复的直接证据）");
    // -----------------------------------------------------------------------
    {
        // ★ 用计数器验证 EOF 信号（不依赖队列残留 —— 空包可能在
        //   视频解码线程读到它的那一刻就被取走了）
        const auto d = player.demuxInfo();
        std::printf("        readThread 投递的 EOF 空包数: %d（音频 1 + 视频 1 = 2）\n",
                    d.nullPkts);
        check(d.nullPkts == 2, "★★ 两路各投了一个【空包】—— 这就是 EOF 信号");

        // 顺便看看队列里还剩下什么（可能已被消费者取走一部分）
        AVPacketPtr pkt = make_packet();
        int  total = 0, flushMarkers = 0, nullPackets = 0;
        int64_t lastSize = -1;
        int  serial = -1;
        bool isFlush = false;
        while (player.videoQueue().get(pkt.get(), false, &serial, &isFlush) == 1) {
            ++total;
            if (isFlush)            ++flushMarkers;
            else if (pkt->size == 0) ++nullPackets;
            lastSize = isFlush ? 0 : pkt->size;
        }
        std::printf("        视频队列剩余 %d 条（其中 flush 标记 %d，空包 %d）\n",
                    total, flushMarkers, nullPackets);
        if (nullPackets > 0) {
            check(lastSize == 0, "剩下的那个空包位于队尾 —— 顺序正确");
        } else {
            std::printf("        （空包已被视频解码线程取走，并在解码器里触发了 draining）\n");
        }
        std::printf("        → 没有这个空包，解码器就不会进入 draining 模式，\n");
        std::printf("          尾部约 2 帧画面永远不显示，解码线程还会一直阻塞。\n");
        std::printf("          这就是 M6 发现、M8 修好的缺陷。\n");
    }

    // -----------------------------------------------------------------------
    section("[5] seek");
    // -----------------------------------------------------------------------
    {
        const int serialBefore = player.videoQueue().serial();
        std::printf("        seek 前 serial = %d\n", serialBefore);

        player.seekMs(2000);
        msleep(500);                                   // 给 readThread 时间执行

        const int serialAfter = player.videoQueue().serial();
        std::printf("        seek 后 serial = %d\n", serialAfter);
        check(serialAfter > serialBefore,
              "★ seek 让 serial 递增（reset() 压入了新的 flush 标记）");

        AVPacketPtr pkt = make_packet();
        int64_t minPts = INT64_MAX, maxPts = INT64_MIN;
        int  n = 0, serial2 = -1;
        bool isFlush2 = false;
        while (player.videoQueue().get(pkt.get(), false, &serial2, &isFlush2) == 1) {
            if (isFlush2 || pkt->size == 0) continue;
            if (pkt->pts != AV_NOPTS_VALUE) {
                if (pkt->pts < minPts) minPts = pkt->pts;
                if (pkt->pts > maxPts) maxPts = pkt->pts;
            }
            ++n;
        }
        check(n > 0, "seek 后队列重新被填充了新数据");

        if (n > 0) {
            const double minMs = minPts * av_q2d(ref.videoTb) * 1000.0;
            const double maxMs = maxPts * av_q2d(ref.videoTb) * 1000.0;
            std::printf("        队列里视频包 pts 范围 %.0f ~ %.0f ms（目标是 2000ms）\n",
                        minMs, maxMs);
            // ★ 精确的落点校验放在 M9 的实验台（那里看音频主时钟，不受
            //   消费影响）。这里只验证"队列里已经全是新位置的数据"，
            //   因为队头可能已被视频解码线程取走，精确下界不可靠。
            check(minMs > 1500,
                  "★ 队列里都是 seek 之后的新数据（0 秒附近的旧包已被清掉）");
            check(serial2 == serialAfter, "新数据带的是 seek 之后的 serial");
        }
    }

    // -----------------------------------------------------------------------
    section("[6] close");
    // -----------------------------------------------------------------------
    {
        const auto t0 = std::chrono::steady_clock::now();
        player.close();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        std::printf("        close 耗时 %lld ms\n", (long long)ms);
        check(ms < 1000, "★ close 在 1 秒内完成 —— 说明读线程被正确唤醒并 join");

        player.close();                                 // 第二次
        check(true, "重复调用 close() 安全（幂等）");

        // close 之后队列应该被清空了
        check(player.videoQueue().nbPackets() == 0, "close 后视频队列已清空");
        check(player.audioQueue().nbPackets() == 0, "close 后音频队列已清空");
    }

    std::printf("\n==================================================\n");
    std::printf(" 测试结束：通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf("==================================================\n");
    return g_fail == 0 ? 0 : 1;
}
