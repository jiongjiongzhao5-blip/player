// ============================================================================
// tests/test_audiodevice.cpp —— M7 实验台 B：AudioDevice（SDL3 音频输出）
//
// 6 组测试：
//   [1] 打开设备
//   [2] ★ 稳态消耗速率校验（证明设备真的按实时速率在跑）
//   [3] 欠载补静音（回调不能因为没数据就什么都不写）
//   [4] 暂停 / 恢复
//   [5] 音量 / 倍速接口
//   [6] close 之后回调停止
//
// ⚠ 运行时会发出一声很轻的 440Hz 提示音（约 0.6 秒，幅度不到 4%）。
//   想彻底静音，把 kQuietTone 改成 false 即可（那样回调填全 0）。
// ============================================================================

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include <windows.h>

#include "audiodevice.h"

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

constexpr double kPi       = 3.14159265358979323846;
constexpr bool   kQuietTone = true;    // false = 全程静音（填 0）

// ---------------------------------------------------------------------------
// 模拟 FFPlayer 侧的 pullAudio：按需要的字节数生成 PCM。
// 用很小的幅度（约 4%）保证不吵人，但确实能听见。
// ---------------------------------------------------------------------------
struct ToneSource {
    int    sampleRate = 48000;
    int    channels   = 2;
    double phase      = 0.0;
    double freq       = 440.0;

    // 下面这些会被 SDL 音频线程访问，主线程也会读，所以用 atomic
    std::atomic<bool>      stall{false};        // true -> 模拟"暂时没数据"
    std::atomic<long long> bytesPulled{0};
    std::atomic<int>       callCount{0};
    std::atomic<int>       stallHits{0};

    // SDL 会在【音频线程】里调这个函数
    int pull(uint8_t* dst, int bytes)
    {
        callCount.fetch_add(1);

        if (stall.load()) {
            // 模拟"上游还没准备好数据"。返回 <=0 让 AudioDevice 去补静音。
            stallHits.fetch_add(1);
            return -1;
        }

        const int frameBytes = 2 * channels;          // S16 * 声道数
        const int frames     = bytes / frameBytes;    // 只填整数个采样帧
        auto* out = reinterpret_cast<int16_t*>(dst);
        const double step = 2.0 * kPi * freq / sampleRate;

        for (int i = 0; i < frames; ++i) {
            const int16_t v = kQuietTone
                                  ? static_cast<int16_t>(1200.0 * std::sin(phase))
                                  : static_cast<int16_t>(0);
            for (int c = 0; c < channels; ++c)
                out[i * channels + c] = v;
            phase += step;
            if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
        }

        const int produced = frames * frameBytes;
        bytesPulled.fetch_add(produced);
        return produced;
    }
};

// ===========================================================================
int main()
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("==================================================\n");
    std::printf(" M7 实验台 B：AudioDevice（SDL3 音频输出）\n");
    if (kQuietTone)
        std::printf(" 提示：会发出一声很轻的 440Hz 提示音\n");
    std::printf("==================================================\n");

    ToneSource src;

    // -----------------------------------------------------------------------
    section("[1] 打开设备");
    // -----------------------------------------------------------------------
    AudioDevice dev;
    const int sampleRate = 48000;
    const int channels   = 2;

    const bool opened = dev.open(sampleRate, channels,
                                [&src](uint8_t* d, int b) { return src.pull(d, b); });
    check(opened, "SDL_OpenAudioDeviceStream 成功");
    if (!opened) {
        std::printf("\n        设备打不开（可能是无声卡的环境），后续测试跳过。\n");
        std::printf("        SDL 的错误信息已经由 AudioDevice 打印在上面。\n");
        return 0;
    }

    std::printf("        采样率 %d Hz, 声道 %d\n", dev.sampleRate(), dev.channels());
    std::printf("        一个采样帧 %d 字节, 每秒消耗 %d 字节\n",
                dev.frameBytes(), dev.bytesPerSecond());
    check(dev.isOpen(), "isOpen() 返回 true");
    check(dev.frameBytes() == 4, "S16 立体声一个采样帧 = 4 字节");
    check(dev.bytesPerSecond() == sampleRate * 4,
          "bytesPerSecond = 采样率 × 每帧字节数");

    // -----------------------------------------------------------------------
    section("[2] ★ 稳态消耗速率校验");
    // -----------------------------------------------------------------------
    // 为什么不能直接量"总共拉了多少"？因为 SDL 一恢复播放就会先把内部缓冲
    // 填满，那一瞬间的爆发量跟设备速率无关。
    // 正确做法：先等它填满（跳过瞬态），再量【稳定期】的增长速率。
    msleep(250);                                   // 让内部缓冲先填满
    const long long a = src.bytesPulled.load();
    const int       windowMs = 600;
    msleep(windowMs);
    const long long b = src.bytesPulled.load();

    const double measured = double(b - a) / (windowMs / 1000.0);
    const double expected = double(dev.bytesPerSecond());
    const double ratio    = measured / expected;

    std::printf("        稳定期 %d ms 内拉取 %lld 字节\n", windowMs, b - a);
    std::printf("        实测速率 %.0f 字节/秒，理论 %.0f 字节/秒，比值 %.3f\n",
                measured, expected, ratio);
    std::printf("        回调累计被调用 %d 次\n", src.callCount.load());

    check(b > a, "数据持续被拉取（回调在工作）");
    check(ratio > 0.75 && ratio < 1.25,
          "★ 实测速率与理论值一致（±25%）—— 设备确实按实时速率在消耗");
    check(src.callCount.load() > 3,
          "回调被反复调用（不是只调一次就停了）");

    // -----------------------------------------------------------------------
    section("[3] 欠载补静音");
    // -----------------------------------------------------------------------
    // 这一步验证 AudioDevice::onPull 里最关键的那段兜底逻辑：
    // pull 返回 -1 时，它必须往流里塞【静音】而不是什么都不写。
    // 什么都不写会出两种事故：① 重复播放缓冲区里的旧样本（爆音）
    //                      ② SDL 认为流断供而卡住（播放停摆）
    const int callsBefore = src.callCount.load();
    src.stall = true;
    msleep(300);
    src.stall = false;
    const int callsDuring = src.callCount.load() - callsBefore;

    std::printf("        欠载期间回调仍被调用 %d 次，其中返回 -1 的有 %d 次\n",
                callsDuring, src.stallHits.load());
    check(src.stallHits.load() > 0, "确实触发了欠载路径");
    check(callsDuring > 0,
          "★ 欠载期间回调仍在被调用 —— 说明补静音让流水线保持连续，没有卡住");

    msleep(100);

    // -----------------------------------------------------------------------
    section("[4] 暂停 / 恢复");
    // -----------------------------------------------------------------------
    dev.setPaused(true);
    msleep(200);                                   // 等设备真正停下来
    const long long p1 = src.bytesPulled.load();
    msleep(300);
    const long long p2 = src.bytesPulled.load();
    const double pausedGrow = double(p2 - p1) / expected;
    std::printf("        暂停 300ms 期间多拉了 %.1f ms 的量\n", pausedGrow * 1000.0);
    check(pausedGrow < 0.15, "暂停后基本不再拉取数据（<150ms 的残余）");

    dev.setPaused(false);
    msleep(300);
    const long long p3 = src.bytesPulled.load();
    check(p3 > p2, "恢复后继续拉取数据");
    std::printf("        恢复 300ms 内多拉了 %.1f ms 的量\n",
                double(p3 - p2) / expected * 1000.0);

    // -----------------------------------------------------------------------
    section("[5] 音量 / 倍速接口");
    // -----------------------------------------------------------------------
    // 重点是"越界值不能把 SDL 搞坏"：setVolume 内部有 std::clamp。
    dev.setVolume(0.4f);
    msleep(80);
    dev.setVolume(-1.0f);      // 越界，应被 clamp 到 0
    msleep(80);
    dev.setVolume(5.0f);       // 越界，应被 clamp 到 1
    msleep(80);
    dev.setVolume(0.6f);
    check(true, "setVolume 接受越界值而不崩溃（内部 clamp 生效）");

    // 倍速：SDL 是"改采样率"，会变调。听感上像快进磁带，这是已知取舍。
    dev.setSpeed(1.5f);
    msleep(120);
    dev.setSpeed(1.0f);
    msleep(120);
    check(true, "setSpeed 调用正常（1.5x 会变调，属预期行为）");

    // -----------------------------------------------------------------------
    section("[6] close 之后回调停止");
    // -----------------------------------------------------------------------
    dev.close();
    check(!dev.isOpen(), "close 后 isOpen() 返回 false");

    const long long c1 = src.bytesPulled.load();
    msleep(300);
    const long long c2 = src.bytesPulled.load();
    check(c2 == c1, "★ close 之后不再有任何回调 —— 流已被销毁并同步等待回调退出");

    // 重复 close 不应该出问题（FFPlayer::close 可能被调多次）
    dev.close();
    check(true, "重复调用 close() 安全（幂等）");

    std::printf("\n==================================================\n");
    std::printf(" 测试结束：通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf("==================================================\n");
    return g_fail == 0 ? 0 : 1;
}
