// ============================================================================
// tests/test_clock.cpp —— M5 实验台：验证 Clock 漂移补偿时钟
//
// 8 组测试：
//   [1] 时钟源探针：av_gettime_relative() 到底是什么
//   [2] 缺陷复现：原工程 init() 之后 get() 不是 NaN
//   [3] 修复后的行为：init() → NaN
//   [4] set/get 基本行为
//   [5] ★ 漂移补偿实测对比（朴素做法 vs Clock）—— 本模块的灵魂
//   [6] setAt 公式验证 + 两次锚定之间读数严格递增
//   [7] ★ setSpeed 的读数连续性（含"不重算补偿量会怎样"的反例）
//   [8] 并发压力：1 写 3 读
// ============================================================================

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include <windows.h>

#include "clock.h"

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

// ---------------------------------------------------------------------------
// 用来做对比的两个"错误实现"
// ---------------------------------------------------------------------------

// ① 复现原工程 init() 的写法：只把 pts 置 NaN，漏掉了真正参与计算的 pts_drift_
struct OldInitClock {
    double pts_drift = 0.0;
    double speed     = 1.0;
    void   init()          { pts_drift = 0.0; }
    double get()  const    { return pts_drift + nowSec() * speed; }
};

// ② 字段语义与 Clock 相同，但改倍速时【只改系数，不重算补偿量】。
//    这是绝大多数人第一次写漂移补偿时钟时会犯的错。
struct NaiveClock {
    double pts_drift = 0.0;
    double speed     = 1.0;
    void   set(double pts) { pts_drift = pts - nowSec() * speed; }
    double get()  const    { return pts_drift + nowSec() * speed; }
};

// ===========================================================================
// [1] 时钟源探针
// ===========================================================================
static void testClockSource()
{
    section("[1] 时钟源探针：av_gettime_relative() 返回的到底是什么");

    const double rel  = av_gettime_relative() / 1000000.0;
    const double abs_ = av_gettime() / 1000000.0;

    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    const double qpc = double(c.QuadPart) / double(f.QuadPart);

    std::printf("        av_gettime_relative()   = %.3f 秒\n", rel);
    std::printf("        av_gettime()            = %.3f 秒\n", abs_);
    std::printf("        两者之差                = %.3f 秒\n", rel - abs_);
    std::printf("        QueryPerformanceCounter = %.3f 秒\n", qpc);

    check(rel > 1e6,
          "av_gettime_relative() 返回纪元量级的绝对秒数，不是'开机以来秒数'");
    check(std::fabs(rel - abs_) > 1.0,
          "它和 av_gettime() 并不相等 —— 不要假设两者等价");
    check(qpc < 1e6,
          "QueryPerformanceCounter 才是真正的单调计时源（自开机）");

    std::printf("        结论：本类只用【两个时刻的差值】做漂移补偿，绝对量级会被\n");
    std::printf("              减掉，所以不影响正确性；但也别假设它单调。\n");
}

// ===========================================================================
// [2] 缺陷复现
// ===========================================================================
static void testOldInitBug()
{
    section("[2] 缺陷复现：原工程 init() 之后 get() 不是 NaN");

    OldInitClock oc;
    oc.init();
    const double v = oc.get();

    std::printf("        原写法 init() 后 get() = %.3f 秒\n", v);
    check(v > 1e6, "返回的是绝对时间（天文数字），而不是 NaN");

    std::printf("        根因：init() 把 pts_ 置成了 NaN，但 get() 真正用的是\n");
    std::printf("              pts_drift_ + now * speed_，而 pts_drift_ 被留成了 0.0，\n");
    std::printf("              于是返回值退化成 now（当前绝对时间）。\n");
    std::printf("        后果：上层那句 std::isnan(clock) 的保护形同虚设，\n");
    std::printf("              第一次锚定之前查询播放位置会拿到这个荒谬值。\n");
}

// ===========================================================================
// [3] 修复后的行为
// ===========================================================================
static void testFixedInit()
{
    section("[3] 修复后：本沙盒版本的 init() → get() 返回 NaN");

    Clock c;
    c.set(42.0);
    check(!std::isnan(c.get()), "set() 之后不再是 NaN");

    c.init();
    const double v = c.get();
    std::printf("        修复后 init() 后 get() = %s\n",
                std::isnan(v) ? "NaN" : "仍然不是 NaN");
    check(std::isnan(v), "init() 之后 get() 返回 NaN —— NaN 语义真正成立了");

    std::printf("        修复只需一行：init() 里把 pts_drift_ 也置成 std::nan(\"\")。\n");
}

// ===========================================================================
// [4] 基本行为
// ===========================================================================
static void testSetAndGet()
{
    section("[4] set/get 基本行为");

    Clock c;
    c.set(10.0);
    const double v = c.get();
    check(std::fabs(v - 10.0) < 0.002, "set(10.0) 后立刻 get() ≈ 10.0");
    std::printf("        实测 get() = %.6f（差 %.6f 秒 = 调用开销）\n", v, v - 10.0);

    check(std::fabs(c.pts() - 10.0) < 1e-9, "pts() 能读回最近一次锚定的 PTS");
    check(std::fabs(c.speed() - 1.0) < 1e-9, "默认倍速为 1.0");

    c.set(0.0);
    check(std::fabs(c.get() - 0.0) < 0.002,
          "可以往回设（seek 到开头），不是只能单调前进");
}

// ===========================================================================
// [5] ★ 漂移补偿实测
// ===========================================================================
static void testDriftCompensation()
{
    section("[5] ★ 漂移补偿实测：朴素做法 vs Clock");

    constexpr int kFrames  = 6;
    constexpr int kFrameMs = 40;      // 25fps
    constexpr int kSamples = 8;

    Clock  c;
    double pts         = 0.0;
    double maxErrNaive = 0.0;
    double maxErrClock = 0.0;
    double sampleGap   = 0.0;

    for (int i = 0; i < kFrames; ++i) {
        c.set(pts);                        // 一帧到达，锚定时钟
        const double anchor = nowSec();    // 记下锚定时刻
        const double naive  = pts;         // 朴素做法：直接用这帧的 pts 当位置

        for (int k = 0; k < kSamples; ++k) {
            msleep(kFrameMs / kSamples);

            // 局部真值：从锚定时刻起，真实播放位置应该 = pts + 流逝时间
            // （注意：必须用"锚定时刻"当基准，不能用进程启动时刻 ——
            //   否则会把 sleep 粒度误差算进来，那是我上一版测试的 bug。）
            const double truth = pts + (nowSec() - anchor);

            maxErrNaive = std::max(maxErrNaive, std::fabs(naive - truth));
            maxErrClock = std::max(maxErrClock, std::fabs(c.get() - truth));
            sampleGap   = std::max(sampleGap, nowSec() - anchor);
        }
        pts += kFrameMs / 1000.0;
    }

    std::printf("        模拟 %d 帧 @ %dms，与真实时间轴对比：\n", kFrames, kFrameMs);
    std::printf("          单个帧间隔内实际流逝          : %6.1f ms\n", sampleGap * 1000.0);
    std::printf("          朴素做法（直接用最近一帧 pts）最大误差: %6.1f ms\n",
                maxErrNaive * 1000.0);
    std::printf("          漂移补偿（本类的 Clock）      最大误差: %6.3f ms\n",
                maxErrClock * 1000.0);

    check(maxErrNaive > 0.030,
          "朴素做法的误差 = 整个锚定间隔（位置在这段时间里完全冻结）");
    check(maxErrClock < 0.003,
          "Clock 的误差只有零点几毫秒（纯粹是两次取时间的调用间隔）");
    check(maxErrClock * 10.0 < maxErrNaive,
          "Clock 的误差比朴素做法小一个数量级以上 —— 漂移补偿确实有效");

    std::printf("        → 这就是'进度条一跳一跳'的量化来源：\n");
    std::printf("          朴素做法每次要等下一帧到达才更新，位置呈台阶状；\n");
    std::printf("          Clock 靠系统时间线性外推，位置是平滑的。\n");
    std::printf("        （注：Windows 默认 sleep 粒度约 15.6ms，所以单个间隔\n");
    std::printf("          被拉长到了上百毫秒，不影响结论的可比性。）\n");
}

// ===========================================================================
// [6] setAt 公式验证
// ===========================================================================
static void testSetAt()
{
    section("[6] setAt()：公式验证 + 连续性");

    Clock c;
    c.setAt(5.0, nowSec() - 2.0);         // 假装 2 秒前用 pts=5.0 锚定过
    const double v = c.get();
    check(std::fabs(v - 7.0) < 0.01,
          "setAt(5.0, now-2s) 后 get() ≈ 7.0（= pts + 流逝的 2 秒）");
    std::printf("        实测 %.6f —— 这就是 clock = pts + (now - t) * s 的验证\n", v);

    bool strictlyIncreasing = true;
    double prev = c.get();
    for (int i = 0; i < 10; ++i) {
        msleep(5);
        const double cur = c.get();
        if (cur <= prev)
            strictlyIncreasing = false;
        prev = cur;
    }
    check(strictlyIncreasing,
          "连续查询 10 次，读数严格递增（位置在平滑推进，不是等帧到达才跳）");
}

// ===========================================================================
// [7] ★ setSpeed 连续性
// ===========================================================================
static void testSetSpeed()
{
    section("[7] ★ 切换倍速：读数连续性（含反例对比）");

    {
        NaiveClock nc;
        nc.set(10.0);
        const double before = nc.get();
        nc.speed = 2.0;                       // ← 朴素改法：只改系数
        const double after = nc.get();
        std::printf("        反例（不重算补偿量）：%.3f  ->  %.3f   跳变 %.1f 秒\n",
                    before, after, after - before);
        check(std::fabs(after - before) > 0.05,
              "反例复现成功：读数剧烈跳变（补偿量里含绝对时间偏移）");
    }

    Clock c;
    c.set(10.0);
    const double before = c.get();
    c.setSpeed(2.0);
    const double after = c.get();
    std::printf("        本沙盒实现：          %.3f  ->  %.3f   跳变 %.4f 秒\n",
                before, after, after - before);
    check(std::fabs(after - before) < 0.003, "切换倍速瞬间读数连续、不跳变");

    msleep(300);
    const double advanced = c.get() - before;
    std::printf("        2 倍速下等待 300ms，读数推进 %.3f 秒\n", advanced);
    check(std::fabs(advanced - 0.6) < 0.05, "2 倍速下推进量 ≈ 系统时间 × 2");

    c.setSpeed(0.0);
    c.setSpeed(-1.0);
    check(std::fabs(c.speed() - 2.0) < 1e-9, "非法倍速（<=0）被忽略，不会产生负向时钟");
}

// ===========================================================================
// [8] 并发压力
// ===========================================================================
static void testConcurrency()
{
    section("[8] 并发压力：1 个写线程 + 3 个读线程");

    Clock c;
    c.set(0.0);

    std::atomic<bool>      stop{false};
    std::atomic<long long> reads{0};
    std::atomic<bool>      sawNaN{false};
    std::atomic<bool>      sawHuge{false};

    std::thread writer([&] {
        double pts = 0.0;
        while (!stop.load(std::memory_order_relaxed)) {
            c.set(pts);                       // 模拟音频回调不断重新锚定
            pts += 0.02;
            msleep(1);
        }
    });

    std::vector<std::thread> readers;
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const double v = c.get();
                if (std::isnan(v))       sawNaN  = true;
                if (std::fabs(v) > 1e12) sawHuge = true;
                ++reads;
            }
        });
    }

    msleep(300);
    stop = true;
    writer.join();
    for (auto& t : readers)
        t.join();

    std::printf("        3 个读线程共完成 %lld 次 get()\n", reads.load());
    check(reads.load() > 10000, "完成了大量并发读取，没有死锁");
    check(!sawNaN.load(),  "并发期间从未读到 NaN");
    check(!sawHuge.load(), "从未读到爆表值 —— 加锁消除了'半新半旧'的数据竞争");

    std::printf("        真实压力参考：音频回调约 50 次/秒、视频刷新约 100 次/秒、\n");
    std::printf("        进度条 5 次/秒 —— 加锁成本在这个量级下完全可忽略。\n");
}

// ===========================================================================
int main()
{
    SetConsoleOutputCP(CP_UTF8);

    std::printf("==================================================\n");
    std::printf(" M5 实验台：Clock（音视频同步时钟 / 漂移补偿）\n");
    std::printf("==================================================\n");

    testClockSource();
    testOldInitBug();
    testFixedInit();
    testSetAndGet();
    testDriftCompensation();
    testSetAt();
    testSetSpeed();
    testConcurrency();

    std::printf("\n==================================================\n");
    std::printf(" 测试结束：通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf("==================================================\n");
    return g_fail == 0 ? 0 : 1;
}
