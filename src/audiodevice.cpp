#include "audiodevice.h"
#include "av_utils.h"

#include <algorithm>
#include <cstring>

#include <SDL3/SDL.h>

AudioDevice::~AudioDevice()
{
    close();
}

// ---------------------------------------------------------------------------
// SDL3 音频回调 —— 整个音频链路被驱动的地方
// ---------------------------------------------------------------------------
// 参数含义（SDL3 的新签名，容易理解错）：
//   userdata         我们注册进去的 this 指针
//   stream           设备绑定着的那条 AudioStream，往它里面塞数据即可
//   additionalAmount 【本流输入格式】下还需要多少字节
//   totalAmount      当前 stream 里排队的字节总数（本实现用不到）
//
// ⚠ 这个函数运行在【SDL 自己的音频线程】里，不是主线程、也不是解码线程。
//   这决定了一件事：FFPlayer 侧的 pullAudio() 里访问的那些成员
//   （重采样器、PCM 缓冲、音频时钟）只被这一个线程碰，
//   所以它们【不需要加锁】。这是后面 M9 设计的关键前提，先记住。
// ---------------------------------------------------------------------------
void SDLCALL AudioDevice::onPull(void* userdata, SDL_AudioStream* stream,
                                 int additionalAmount, int /*totalAmount*/)
{
    auto* self = static_cast<AudioDevice*>(userdata);

    // 栈上 4KB 的小块。为什么不按 additionalAmount 一次性分配？
    //   因为回调里不该做堆分配（实时性要求，不能阻塞）。
    uint8_t chunk[4096];

    // ---- 循环分块 ----
    // additionalAmount 可能远大于 4096（比如设备一次要 16KB），
    // 所以要循环若干次，每次最多搬 4096 字节。
    while (additionalAmount > 0) {
        const int want = std::min(additionalAmount, static_cast<int>(sizeof(chunk)));

        int got = -1;
        if (self->pull_)
            got = self->pull_(chunk, want);

        // ---- ★ 取不到数据就补静音 ----
        // 这是音频回调里最容易踩的坑：如果"没数据就什么都不写"，
        // 会出两种事故：
        //   ① 爆音：设备缓冲区里残留着上一次的旧样本，会被重复播放；
        //   ② 卡死：SDL 认为流断供，音频线程可能挂住，表现为整个播放停住。
        // 补静音（全 0）是唯一正确的兜底 —— 静音在听感上是"没声音"，
        // 而不是噪音，用户几乎察觉不到，但流水线保持连续。
        if (got <= 0) {
            std::memset(chunk, 0, want);
            got = want;
        }

        if (got > 0)
            SDL_PutAudioStreamData(stream, chunk, got);

        // 注意这里减的是 got 而不是 want：pull_ 可能只给了一部分
        //（比如它内部只够凑出一帧），那就下一轮继续要。
        additionalAmount -= got;
    }
}

// ---------------------------------------------------------------------------
// 打开设备
// ---------------------------------------------------------------------------
bool AudioDevice::open(int sampleRate, int channels, PullCallback pull)
{
    // 音频子系统是【惰性初始化】的：只有本模块用得到，所以不放在 main()
    // 里全局初始化。SDL_WasInit 检查避免重复 Init（重复 Init 无害但会
    // 打乱引用计数，万一同进程还有别的地方也初始化音频就容易出问题）。
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) {
        if (!SDL_Init(SDL_INIT_AUDIO)) {
            av_log(nullptr, AV_LOG_FATAL, "SDL_Init(AUDIO) failed: %s\n", SDL_GetError());
            return false;
        }
    }

    // SDL3 的 SDL_AudioSpec 只剩三个字段（SDL2 里还有 samples/silence/
    // padding/size/callback/userdata 等一堆，SDL3 把回调挪到 open 的参数里了）
    spec_.format   = SDL_AUDIO_S16;     // 交织有符号 16bit —— 和 FFPlayer 侧
                                        // swresample 的输出格式必须一致
    spec_.channels = channels;
    spec_.freq     = sampleRate;
    pull_          = std::move(pull);

    // ★ 一步打开设备并绑定流（SDL2 要 SDL_OpenAudio + SDL_NewAudioStream 两步）
    //   传 SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK 表示用系统默认播放设备。
    //
    //   ⚠ SDL3 的设备打开后【初始是暂停状态】。这是刻意的设计：
    //     SDL2 时代是"打开就开始播"，容易在没准备好数据时先播出一段噪音。
    //     SDL3 让你先准备好，再显式恢复 —— 更安全。
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                        &spec_, &AudioDevice::onPull, this);
    if (!stream_) {
        av_log(nullptr, AV_LOG_FATAL,
               "SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        return false;
    }

    // 显式启动（等价于 SDL2 的 SDL_PauseAudio(0)）。
    // 从这一行起，onPull 就开始被音频线程调用了 —— 此时 pull_ 必须已经就绪。
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

// ---------------------------------------------------------------------------
// 关闭
// ---------------------------------------------------------------------------
void AudioDevice::close()
{
    if (stream_) {
        // SDL_DestroyAudioStream 会同时关闭它绑定的设备，
        // 并等待回调退出（SDL 内部会做同步），所以返回之后
        // 可以安全地认为 onPull 不会再被调用。
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    // 解绑回调。这一句放在最后，是为了让"流还活着时 pull_ 一定有效"这个
    // 不变量成立 —— 反过来写（先清 pull_ 再销毁流）会出现回调里 self->pull_
    // 为空、整段静音的时间窗。
    pull_ = nullptr;
}

// ---------------------------------------------------------------------------
// 播放控制
// ---------------------------------------------------------------------------

// 暂停：SDL3 里这是"流的属性"，不是"设备的状态"。
// 效果是音频线程停止向设备供数，但流里已排队的数据会保住，恢复时接着放。
void AudioDevice::setPaused(bool paused)
{
    if (!stream_)
        return;
    if (paused)
        SDL_PauseAudioStreamDevice(stream_);
    else
        SDL_ResumeAudioStreamDevice(stream_);
}

// 音量：SDL3 的 gain 是【线性】倍数，不是分贝。
// 所以界面上的 0~100 要除以 100 再传进来（这层换算在 MediaPlayer 里做）。
// clamp 是防御：上层传了越界值（比如界面滑块 bug 给了 150）不至于让
// SDL 收到无意义的参数。
void AudioDevice::setVolume(float linear01)
{
    if (stream_)
        SDL_SetAudioStreamGain(stream_, std::clamp(linear01, 0.0f, 1.0f));
}

// 倍速：改的是流的【频率比】。
//
// ★ 必须知道的局限：SDL_SetAudioStreamFrequencyRatio 做的是
//   "按比例重新采样"，也就是【改变音高】。ratio=2 时声音会变成
//   又高一倍又快的"花栗鼠音"。
//
//   真正好听的倍速播放需要"时间拉伸不变调"（phase vocoder 之类的 DSP），
//   那是另一个量级的工程。本项目选择这个廉价方案，是因为：
//     ① 实现成本为零（SDL 自带）；
//     ② 视频播放的倍速主要是为了"快速跳过/慢放看清"，音高变化可接受；
//     ③ 帧队列和时钟都不需要改 —— 音频输出速率变了，PTS 自然推进更快，
//        视频侧只是"更快地追上"，同步逻辑完全不用动。
//   这是一个典型的"用可接受的音质损失换取实现简单"的取舍。
void AudioDevice::setSpeed(float ratio)
{
    if (stream_)
        SDL_SetAudioStreamFrequencyRatio(stream_, ratio);
}

// 一个采样帧的字节数 = 每声道 2 字节（S16）× 声道数。
// SDL_AUDIO_FRAMESIZE 宏就是干这个的（等价于 SDL_AUDIO_BYTESIZE/8 × channels）。
int AudioDevice::frameBytes() const
{
    return SDL_AUDIO_FRAMESIZE(spec_);
}

// 每秒消耗多少字节。★ 这个数字很重要：
//   它把"还有多少字节没播"换算成"还有多少秒"，
//   正是 M9 里推进音频主时钟时那个 remain 的计算依据。
int AudioDevice::bytesPerSecond() const
{
    return spec_.freq * frameBytes();
}
