#ifndef CLOCK_H
#define CLOCK_H

#include <cmath>
#include <mutex>

// av_gettime_relative() 在这里。注意它在本机返回的是纪元量级的绝对秒数，
// 并非"开机以来秒数"，也不保证单调 —— 详见下方文件头说明。
extern "C" {
#include <libavutil/time.h>
}

// ============================================================================
// clock.h —— 音视频同步时钟（对标 ffplay 的 Clock）
//
// 【★ 先说清楚时间源这件事（本机实测结论，务必先读）】
//
//   本类用 av_gettime_relative() 读"现在几点"。名字听起来是"相对时间"，
//   但实测（本机 FFmpeg 9 / Windows）它返回的是【纪元时间量级的绝对秒数】：
//
//       av_gettime_relative() = 1789786012.518 秒   （≈ 2026-09-17 的 Unix 时间）
//       av_gettime()          = 1789634812.518 秒   （与上面相差 42 小时）
//       QueryPerformanceCounter = 12966.8 秒        （自开机，才是真正的单调源）
//
//   所以：
//     * 不要假设它是"自开机以来的秒数"，也不要假设它和 av_gettime() 相等；
//     * 更要紧的是【不要假设它单调】。它带着纪元时间量级，说明它是锚在
//       墙钟上的。若系统时间被 NTP 校正或被手工修改，读它会跳变。
//       （要绝对保证单调，应改用 std::chrono::steady_clock 或
//         QueryPerformanceCounter，这里保持与 ffplay/原工程一致，仅作记录。）
//     * 好消息：这些【完全不影响漂移补偿的正确性】—— 因为本类只使用
//       "两个时刻的差值"，绝对量级会被减掉。真正需要留意的是 double 的
//       有效位：在 1.79e9 量级上，double 的分辨率约 0.4 微秒，相对毫秒级的
//       音频/视频同步误差可以忽略。
//
// 【在项目里的位置】
//   它是"当前播放到第几秒"这个问题的【唯一权威答案】。
//   项目里只实例化了一个 Clock —— audClk_（音频主时钟），它是整条
//   同步链的基准：音频回调负责推进它，视频刷新线程读它来决定"这一帧
//   现在该不该显示"。
//
//     SDL 音频回调  ──set()──>  [audClk_]  ──get()──>  视频刷新线程
//                                              └─────>  进度条 / 位置显示
//
// 【它解决什么问题】
//   PTS（时间戳）是【离散的】—— 音频每 20ms 才来一帧、视频每 40ms 才来一帧。
//   但"当前播放位置"是【连续的】—— 用户拖进度条、界面显示时间、视频线程
//   判断该不该显示，任何时候都需要立刻问出"现在到哪了"。
//
//   最朴素的做法是"记住最近一帧的 PTS 直接当位置"，但这会出错：
//   两次帧到达之间，位置会【冻结不动】。视频按 40ms 一帧算，位置就会
//   一跳一跳地跳 40ms，最大误差达到【一整帧的时长】。
//
// 【漂移补偿 —— 这个类的核心思想】
//   关键洞察：在两次"帧到达"之间，播放位置的变化量【恰好等于流逝的系统时间】
//   （1 倍速时）。所以不必存储"位置"这个会变的量，而是存储
//   "位置与系统时间的偏差"这个在两次锚定之间【恒定】的量。
//
//   推导（设 pts = 锚定时的媒体时间戳，t = 锚定时刻的系统时间，
//             now = 查询时刻的系统时间， s = 播放倍速）：
//
//       从锚定到查询，流逝了 (now - t) 秒的系统时间
//       1 倍速时媒体位置也前进 (now - t)
//       s 倍速时媒体位置前进 (now - t) * s
//
//       所以   clock = pts + (now - t) * s
//
//   把常量部分合并，就得到本类使用的形式：
//
//        clock = pts_drift + now * s          （get() 只需一次乘加）
//        其中  pts_drift = pts - t * s        （锚定时算一次，之后是常量）
//
//   验证：把 now = t 代入 → clock = (pts - t*s) + t*s = pts ✓
//
//   对照 ffplay：它写的是
//        pts_drift + time - (time - last_updated) * (1.0 - speed)
//   代数上完全等价（展开后同样是 pts + (now - t) * s），
//   但我们的形式直接把补偿量算出来，少几项运算，也更容易看懂。
//
// 【相比老工程（裸 struct + 一堆全局函数，且完全无锁）的改进】
//   * 封装成类：数据与操作在一起，改动不会漏掉某个全局函数；
//   * std::mutex 保护：原工程里音频线程写、视频线程读、UI 线程读，
//     三线程并发访问四个 double 而【没有任何保护】。x86 上单个 double 的
//     读写确实是原子的，所以"看起来能用"，但 pts_drift_ / last_updated_ /
//     speed_ 不是一起更新的 —— 读线程可能读到"半新半旧"的组合，
//     导致时钟瞬间跳变。这在标准里是数据竞争（UB），必须用锁消除。
//   * 支持倍速（speed），且切换倍速时保证读数【连续不跳变】。
// ============================================================================

class Clock
{
public:
    Clock() = default;

    // 复位：把 PTS 置为 NaN。
    //
    // 【为什么用 NaN 当"未知"的哨兵，而不是 0 或 -1？】
    //   1) 检查起来自然：std::isnan() 一句话；
    //   2) 更安全：NaN 参与任何算术运算，结果【仍然是 NaN】。
    //      也就是说，如果你在时钟还没锚定时就拿它去算，错误会一路传播、
    //      立刻暴露；而用 0 当哨兵的话，算出来的 0 看起来像"合法值"，
    //      bug 会被悄悄吞掉。
    //   项目里的用法：「positionSeconds()」和「videoRefresh()」都先用
    //   std::isnan() 判一下再使用 —— 所以这个 NaN 语义是【必须成立】的。
    //
    // ★ 这一行是本沙盒相对原始工程的【唯一一处行为修正】，原因见下：
    //
    //   原工程 init() 写的是：
    //       pts_ = nan;  pts_drift_ = 0.0;  last_updated_ = 0.0;
    //   只把 pts_ 设成 NaN，却把真正参与 get() 计算的 pts_drift_ 留成 0.0。
    //   而 get() = pts_drift_ + now * speed_ = 0 + now = 【当前绝对时间】。
    //
    //   实测（本机）：init() 之后 get() 返回 1789786012.5 秒，
    //   也就是"纪元时间量级的绝对秒数"，而不是 NaN。
    //
    //   后果：
    //     * 上层那句 std::isnan(clock) 的保护永远不成立（它想挡的就是
    //       "还没锚定"这个状态），等于失效；
    //     * 在第一次锚定之前查询播放位置，会拿到一个天文数字。
    //   影响范围有限（正常播放几十毫秒内就会被音频回调锚定，且纯视频
    //   路径另有 videoClockInit_ 兜底），但这是个实打实的语义 bug。
    //
    //   修复：让 pts_drift_ 也是 NaN —— 这样 get() 自然返回 NaN，
    //         NaN 语义才真正成立。
    void init()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pts_          = std::nan("");
        pts_drift_    = std::nan("");   // ★ 修复点（原工程是 0.0）
        last_updated_ = 0.0;
    }

    // 以"当前系统时间"为基准锚定时钟 —— 最常用的入口，
    // 音频回调里每拉取一批数据就调一次，把时钟纠回真实 PTS。
    void set(double pts)
    {
        const double now = av_gettime_relative() / 1000000.0;
        setAt(pts, now);
    }

    // 以【指定的系统时间】为基准锚定时钟。
    // 单独暴露它是为了测试时能构造"虚拟时间线"，以及在需要批量补偿时
    // 一次性把多个时钟对齐到同一个时刻。
    void setAt(double pts, double time)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pts_          = pts;
        last_updated_ = time;
        // 把"倍速"预补偿进漂移量，这样 get() 只需要一次乘加
        pts_drift_    = pts - time * speed_;
    }

    // 读取当前时钟值（秒）—— 本类唯一的热点函数
    double get() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const double now = av_gettime_relative() / 1000000.0;
        return pts_drift_ + now * speed_;
    }

    // 设置播放倍速。
    //
    // 【难点】直接改 speed_ 会让 get() 的返回值【瞬间跳变】，因为
    //        pts_drift_ 是按旧倍速算出来的。举例：当前读到 10.0 秒，
    //        此时把倍速从 1.0 改成 2.0，若不处理，下一瞬间读数会变成
    //        ptr_drift + now*2 —— 而 ptr_drift 里含 -now*1，于是读数会
    //        突然偏移出 now 这一项，位置瞬间乱跳。
    //
    // 【解法】改倍速前先按旧倍速读出当前值，改完之后反算出新的补偿量，
    //        使得"此刻"的读数仍然等于原来的值。这样位置曲线在切换点是
    //        连续的，只是斜率变了。
    void setSpeed(double speed)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (speed <= 0.0)
            return;                       // 防御：非正倍速无意义，直接忽略
        const double now  = av_gettime_relative() / 1000000.0;
        const double curr = pts_drift_ + now * speed_;   // 旧倍速下的当前读数
        speed_            = speed;
        // 反算：让此刻 get() = curr，即 curr - now * 新倍速
        pts_drift_        = curr - now * speed_;
    }

    // ---- 以下两个是"观察用"访问器 ----
    // 注意：在本工程里【都没有被调用过】，属于为调试/扩展预留的接口。
    // 保留它们的价值：排查同步问题时，你会想知道"时钟最后一次是被哪个
    // PTS 锚定的、当时倍速是多少"，这两个 getter 就是那个窗口。
    double speed() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return speed_;
    }

    double pts() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return pts_;
    }

private:
    // mutable 是必须的：get()/speed()/pts() 都是 const 成员函数，
    // 但它们要加锁 —— 加锁会修改 mutex 的内部状态。
    // mutable 的语义就是"这个成员即使对象是 const 也允许被修改"。
    mutable std::mutex mutex_;

    double pts_          = std::nan("");  // 最近一次锚定的 PTS（秒）
    double pts_drift_    = 0.0;           // = pts - 锚定时刻 * speed（核心状态）
    double last_updated_ = 0.0;           // 最近一次锚定的系统时间
                                          // ⚠ 本工程里只写不读，属于"留档"字段
    double speed_        = 1.0;           // 播放倍速
};

#endif // CLOCK_H
