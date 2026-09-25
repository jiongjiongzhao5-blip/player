// ============================================================================
// tests/test_ffplayer_video.cpp —— M10 实验台：视频链路 + 音视频同步
//
// ★ 这个实验台的观察手段是【正式公共接口】setFrameCallback，
//   不需要任何沙盒后门 —— 因为"什么时候显示了哪一帧"本来就该由上层知道。
//
// 7 组测试：
//   [1] prepare 并注册帧回调
//   [2] ★ 视频帧统计：帧数 / pts 单调 / 显示间隔 ≈ 40ms
//   [3] ★ 同步质量：(主时钟 − 帧PTS) 的分布
//   [4] seek 后视频跟随
//   [5] ★ 纯视频降级：没有音频流时的外部时钟路径
//   [6] ★ 丢帧逻辑：回调故意变慢，验证迟到帧被丢弃而不是越拖越远
//   [7] close
//
// ⚠ 会播放音频（音量 20%）。
// 用法：test_ffplayer_video.exe [带音频的素材] [纯视频素材]
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
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

static double nowSec()
{
    return av_gettime_relative() / 1000000.0;
}

static bool waitUntil(FFPlayer& p, FFMsg stopAt, int timeoutMs)
{
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        if (el > timeoutMs) return false;
        Message m;
        const int r = p.messages().get(m, false);
        if (r == 1) { if (m.what == stopAt) return true; }
        else if (r < 0) return false;
        else msleep(5);
    }
}

// ---------------------------------------------------------------------------
// 帧回调里采集的数据
// ---------------------------------------------------------------------------
struct FrameRec {
    double pts    = 0.0;   // 帧的媒体时间
    double master = 0.0;   // 显示瞬间的主时钟读数
    double wall   = 0.0;   // 显示瞬间的墙钟（相对测试开始）
};

struct Collector {
    std::mutex           mtx;
    std::vector<FrameRec> recs;
    std::atomic<int>     slowFrames{0};   // 前 N 帧故意睡一会儿（模拟渲染跟不上）
    std::atomic<int>     slowMs{0};
    double               t0 = 0.0;
};

static double median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// ===========================================================================
int main(int argc, char* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const std::string avPath  = (argc > 1) ? argv[1] : "../_media/test_media.mp4";
    const std::string vPath   = (argc > 2) ? argv[2] : "../_media/test_video_only.mp4";

    std::printf("==================================================\n");
    std::printf(" M10 实验台：视频链路 + 音视频同步\n");
    std::printf(" 带音频素材: %s\n", avPath.c_str());
    std::printf(" 纯视频素材: %s\n", vPath.c_str());
    std::printf("==================================================\n");

    // =======================================================================
    section("[1] prepare 并注册帧回调（带音频素材）");
    // =======================================================================
    Collector col;
    col.t0 = nowSec();

    FFPlayer player;
    player.setVolume(0.2f);
    player.setFrameCallback([&col, &player](const Frame& f) {
        // 回调运行在 videoRefreshThread 里，所以采集要加锁
        const double master = player.positionSeconds();
        const double wall   = nowSec() - col.t0;

        // 模拟"渲染跟不上"：前 slowFrames 帧各睡 slowMs 毫秒
        if (col.slowFrames.load() > 0) {
            col.slowFrames.fetch_sub(1);
            msleep(col.slowMs.load());
        }

        std::lock_guard<std::mutex> lock(col.mtx);
        col.recs.push_back(FrameRec{f.pts, master, wall});
    });

    {
        check(player.prepare(avPath) == 0, "prepare 返回 0");
        check(waitUntil(player, FFMsg::Prepared, 5000), "收到 Prepared");

        // 诊断：每 500ms 打印一次流水线上游的状态，看是谁卡住了
        std::atomic<bool> monStop{false};
        std::thread monitor([&] {
            for (int i = 0; i < 12 && !monStop.load(); ++i) {
                msleep(500);
                std::printf("          [监控] t=%.1fs 主时钟=%.3f 视频队列剩包=%d "
                            "已丢帧=%d 已显示=%zu\n",
                            (nowSec() - col.t0), player.positionSeconds(),
                            player.videoQueue().nbPackets(),
                            player.droppedFrames(),
                            col.recs.size());
            }
        });

        // 等播放到结束（时长约 5 秒 + 余量）
        const bool done = waitUntil(player, FFMsg::Completed, 9000);
        check(done, "收到 Completed");
        msleep(600);   // 让刷新线程把队列里最后几帧也显示掉
        monStop = true;
        if (monitor.joinable()) monitor.join();
    }

    const size_t n = col.recs.size();
    std::printf("        共采集到 %zu 帧显示记录\n", n);
    check(n > 100, "显示了足够多的帧（素材可解 124 帧）");

    // 诊断：帧数异常时把原始记录全打出来，看时间跨度就明白卡在哪了
    if (n > 0 && n < 30) {
        std::printf("        [诊断] 全部显示记录：\n");
        for (const auto& r : col.recs)
            std::printf("          pts=%.3f  master=%.3f  wall=%.3f\n",
                        r.pts, r.master, r.wall);
    }

    // =======================================================================
    section("[2] ★ 视频帧统计");
    // =======================================================================
    if (n > 10) {
        bool ptsMono = true;
        std::vector<double> gaps;
        for (size_t i = 1; i < n; ++i) {
            if (col.recs[i].pts <= col.recs[i - 1].pts) ptsMono = false;
            gaps.push_back((col.recs[i].wall - col.recs[i - 1].wall) * 1000.0);
        }
        const double gapMed = median(gaps);
        const double gapMax = *std::max_element(gaps.begin(), gaps.end());

        std::printf("        首帧 pts=%.3f 秒，末帧 pts=%.3f 秒\n",
                    col.recs.front().pts, col.recs.back().pts);
        std::printf("        显示间隔：中位数 %.1f ms，最大 %.1f ms\n", gapMed, gapMax);

        check(ptsMono, "★ 显示的帧按 PTS 严格递增（没有倒序显示）");
        check(gapMed > 30.0 && gapMed < 50.0,
              "★ 显示间隔中位数接近 40ms —— 对应素材的 25fps");
        check(col.recs.back().pts > 4.8,
              "最后一帧的 pts 接近素材末尾（说明视频播完了）");
    }

    // =======================================================================
    section("[3] ★ 同步质量");
    // =======================================================================
    // 显示条件是 diff = pts − master <= 0，所以 master >= pts 恒成立。
    // "迟到量" = master − pts 就是同步误差：理想情况应该是 0~10ms
    // （受刷新节拍影响），远大于 100ms 就说明画面明显落后于声音了。
    if (n > 10) {
        std::vector<double> late;
        for (const auto& r : col.recs)
            late.push_back((r.master - r.pts) * 1000.0);

        const double med = median(late);
        const double mx  = *std::max_element(late.begin(), late.end());
        const double mn  = *std::min_element(late.begin(), late.end());

        std::printf("        迟到量(主时钟 − 帧PTS)：最小 %.1f ms，中位 %.1f ms，最大 %.1f ms\n",
                    mn, med, mx);
        check(mn >= -5.0, "没有早于主时钟就显示的帧（diff 判定生效）");
        check(med >= 0.0 && med < 40.0,
              "★ 中位迟到量在 0~40ms —— 画面紧跟声音，同步良好");
        check(mx < 200.0,
              "★ 最大迟到量 < 200ms —— 没有出现持续落后的情况");
        std::printf("        （人眼对音视频偏差的容忍度大致是 ±50ms 以内很自然）\n");
    }

    // =======================================================================
    section("[4] seek 后视频跟随");
    // =======================================================================
    {
        const size_t before = col.recs.size();
        player.seekMs(3000);
        msleep(900);

        std::lock_guard<std::mutex> lock(col.mtx);
        const size_t after = col.recs.size();
        check(after > before, "seek 之后又有新帧被显示");

        if (after > before) {
            const double firstAfterSeek = col.recs[before].pts;
            std::printf("        seek 到 3000ms 后第一帧的 pts = %.3f 秒\n", firstAfterSeek);
            check(firstAfterSeek > 2.0 && firstAfterSeek < 4.0,
                  "★ 视频跳到了新位置附近（不是从 0 重新开始）");
        }
    }
    player.close();
    check(true, "close 正常返回");

    // =======================================================================
    section("[5] ★ 纯视频降级：没有音频流时的外部时钟");
    // =======================================================================
    // 这条路径的关键是：没有音频来推进时钟，于是反过来用视频自己的 PTS
    // 去锚定外部时钟（videoRefresh 里 audioIdx_ < 0 的分支）。
    // 判据：画面仍然按 25fps 正常推进、并且能播到结尾 —— 说明外部时钟在工作。
    {
        Collector vcol;
        vcol.t0 = nowSec();

        FFPlayer vplayer;
        vplayer.setFrameCallback([&vcol](const Frame& f) {
            std::lock_guard<std::mutex> lock(vcol.mtx);
            vcol.recs.push_back(FrameRec{f.pts, 0.0, nowSec() - vcol.t0});
        });

        check(vplayer.prepare(vPath) == 0, "prepare 纯视频素材");
        check(waitUntil(vplayer, FFMsg::Prepared, 5000), "收到 Prepared");
        waitUntil(vplayer, FFMsg::Completed, 9000);
        msleep(600);

        const size_t vn = vcol.recs.size();
        std::printf("        纯视频共显示 %zu 帧\n", vn);
        check(vn > 100, "纯视频素材也显示出了足够多的帧");

        if (vn > 10) {
            bool mono = true;
            std::vector<double> gaps;
            for (size_t i = 1; i < vn; ++i) {
                if (vcol.recs[i].pts <= vcol.recs[i - 1].pts) mono = false;
                gaps.push_back((vcol.recs[i].wall - vcol.recs[i - 1].wall) * 1000.0);
            }
            const double gapMed = median(gaps);
            std::printf("        显示间隔中位数 %.1f ms（期望约 40ms）\n", gapMed);
            std::printf("        末帧 pts = %.3f 秒\n", vcol.recs.back().pts);

            check(mono, "纯视频路径下 PTS 也是严格递增的");
            check(gapMed > 30.0 && gapMed < 55.0,
                  "★ 外部时钟让画面维持了正确帧率 —— 降级路径生效");
            check(vcol.recs.back().pts > 4.6,
                  "★ 纯视频也播到了结尾（外部时钟没有跑偏或停摆）");
        }
        vplayer.close();
    }

    // =======================================================================
    section("[6] ★ 丢帧逻辑：回调故意变慢");
    // =======================================================================
    // 让前 5 帧的回调各睡 150ms —— 模拟"渲染跟不上"。这会让画面一下子
    // 落后主时钟 750ms 左右。如果只是逐帧补显，画面会一直落后；
    // 正确的做法是把迟到超过阈值的帧丢掉，快速追回来。
    {
        Collector scol;
        scol.t0 = nowSec();
        scol.slowFrames = 5;
        scol.slowMs     = 150;

        FFPlayer splayer;
        splayer.setVolume(0.2f);
        splayer.setFrameCallback([&scol](const Frame& f) {
            const double wall = nowSec() - scol.t0;
            if (scol.slowFrames.load() > 0) {
                scol.slowFrames.fetch_sub(1);
                msleep(scol.slowMs.load());
            }
            std::lock_guard<std::mutex> lock(scol.mtx);
            scol.recs.push_back(FrameRec{f.pts, 0.0, wall});
        });

        splayer.prepare(avPath);
        waitUntil(splayer, FFMsg::Prepared, 5000);
        msleep(4000);                       // 播 4 秒足够观察
        const int dropped = splayer.droppedFrames();
        splayer.close();

        std::printf("        丢帧数 = %d\n", dropped);
        check(dropped > 0, "★ 迟到的帧被丢弃了（参考工程没有这个逻辑）");

        // 后半段（慢回调结束之后）应该已经追回来：检查最后 20 帧的时间间隔
        std::lock_guard<std::mutex> lock(scol.mtx);
        if (scol.recs.size() > 30) {
            const size_t m = scol.recs.size();
            const double lastGap = (scol.recs[m - 1].wall - scol.recs[m - 20].wall)
                                   / 19.0 * 1000.0;
            std::printf("        最后 20 帧的平均显示间隔 = %.1f ms\n", lastGap);
            check(lastGap > 25.0 && lastGap < 60.0,
                  "★ 慢回调结束后显示间隔回到 40ms 附近 —— 说明成功追回了进度");
        } else {
            check(false, "采集到的帧太少，无法评估追赶效果");
        }
    }

    std::printf("\n==================================================\n");
    std::printf(" 测试结束：通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf("==================================================\n");
    return g_fail == 0 ? 0 : 1;
}
