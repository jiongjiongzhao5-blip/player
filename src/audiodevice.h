#ifndef AUDIODEVICE_H
#define AUDIODEVICE_H

#include <cstdint>
#include <functional>

#include <SDL3/SDL_audio.h>

// ============================================================================
// audiodevice.h —— SDL3 音频输出封装
//
// 【在项目里的位置】
//   它是音频链路的【出口】，也是整条流水线的"节拍器"：
//
//     FrameQueue(音频帧) ──> FFPlayer::pullAudio() ──> [AudioDevice] ──> 声卡
//                                    │
//                              顺便推进音频主时钟
//
//   ★ 关键：音频主时钟是被【声卡的需求】驱动的。声卡要数据 → 回调被调用 →
//     我们从队列取帧、重采样、交给声卡，同时把时钟锚定到这一帧的 PTS。
//     所以音频的播放速度由硬件决定，视频去追它 —— 这就是 M5 那个 Clock 的
//     使用场景。这一层是"谁在驱动谁"的答案所在。
//
// 【相对原工程（SDL2）的彻底改写】
//   SDL3 把音频 API 整个重做了，不是改名字而是换模型：
//
//   | 维度       | SDL2                              | SDL3                          |
//   |-----------|-----------------------------------|-------------------------------|
//   | 打开设备   | SDL_OpenAudio(&want, &obtained)   | SDL_OpenAudioDeviceStream()   |
//   | 数据通路   | 回调里直接往设备 buffer 写         | 回调里往 SDL_AudioStream 塞     |
//   | 格式转换   | SDL_AudioCVT（要手工搭，很痛苦）   | 流自带（重采样/声道/格式）      |
//   | 回调签名   | (userdata, Uint8* stream, len)    | (userdata, stream, additional, total) |
//   | 暂停/恢复  | SDL_PauseAudio(int)               | SDL_PauseAudioStreamDevice()  |
//   | 关闭       | SDL_CloseAudio()                  | SDL_DestroyAudioStream()      |
//   | 采样格式   | AUDIO_S16SYS                      | SDL_AUDIO_S16                 |
//
//   ★ SDL3 改动的本质：把 SDL_AudioStream 从"可选工具"提升为【一等公民】。
//     在 SDL2 里，回调收到的就是要写进硬件的裸 buffer，所有转换都要自己用
//     SDL_AudioCVT 做；SDL3 改成"设备持有一条流"，回调只负责【往流里塞数据】，
//     转换（采样率、声道数、格式、增益、频率比）全部是流的属性。
//     好处：① 播放/录制/格式转换统一一套抽象；② 音量、倍速不用自己写 DSP；
//           ③ 回调里不用关心设备格式，只管塞我们自己的格式。
//
// 【本类的接口风格：只暴露一个"拉"回调】
//   对上层只暴露 PullCallback：SDL 需要多少字节，就向 FFPlayer 拉多少。
//   上层完全看不到 SDL 的类型（除了 include 那一行），换音频后端时
//   只需要重写这个 .cpp。
//
//   输出格式固定为【交织有符号 16bit】（SDL_AUDIO_S16）。转换由 FFPlayer
//   侧的 swresample 完成 —— 因为那里才知道源格式是什么。
// ============================================================================

class AudioDevice
{
public:
    // 向播放器拉取 bytes 字节的 S16 交织 PCM 写入 dst。
    // 返回实际写入的字节数；<=0 表示"暂时没有数据"（本类会补静音）。
    using PullCallback = std::function<int(uint8_t* dst, int bytes)>;

    AudioDevice() = default;
    ~AudioDevice();

    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // 打开默认播放设备并绑定一条 AudioStream。
    // 成功后设备【已经开始工作】，回调会持续被调用。
    // 失败返回 false（并已通过 av_log 打出 SDL 的错误信息）。
    bool open(int sampleRate, int channels, PullCallback pull);

    // 销毁流（同时关闭设备）并解绑回调。可重复调用。
    // ⚠ 必须在解绑回调之前确保回调不会再访问已销毁的资源 ——
    //   所以 FFPlayer::close() 里是"先 audioDev_.close()，再 join 其他线程"。
    void close();

    bool isOpen() const { return stream_ != nullptr; }

    void setPaused(bool paused);
    void setVolume(float linear01);   // 0.0 静音 ~ 1.0 原始音量
    void setSpeed(float ratio);       // 1.0 正常，>1 加速（会变调，见 .cpp 说明）
    float speed() const { return speed_; }

    int sampleRate() const { return spec_.freq; }
    int channels() const { return spec_.channels; }
    int frameBytes() const;           // 一个采样帧（所有声道各一个采样）的字节数
    // 每秒消耗多少字节 ★ 注意：它把【播放倍速】也算进去了 ——
    // 倍速下流是以 ratio 倍的速度消耗输入 PCM 的。
    // FFPlayer 用它把"还剩下多少字节"换算成"还有多少秒没播"，
    // 所以这个换算必须反映真实消耗速率，否则倍速下主时钟会偏。
    int bytesPerSecond() const;

private:
    // SDL 线程回调。SDLCALL 是平台调用约定宏（Windows 上是 __cdecl），
    // 必须带上，否则函数指针类型不匹配。
    static void SDLCALL onPull(void* userdata, SDL_AudioStream* stream,
                               int additionalAmount, int totalAmount);

    SDL_AudioStream* stream_ = nullptr;   // 流是裸指针：SDL 没有 RAII，我们在 close() 里管
    SDL_AudioSpec    spec_{};             // SDL3 里只剩 format / channels / freq 三个字段
    PullCallback     pull_;
    float            speed_  = 1.0f;      // 当前播放倍速（setSpeed 记下，供 bytesPerSecond 换算）
};

#endif // AUDIODEVICE_H
