#ifndef FFPLAYER_H
#define FFPLAYER_H

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audiodevice.h"
#include "av_utils.h"
#include "clock.h"
#include "decoder.h"
#include "frame_queue.h"
#include "message_queue.h"
#include "packet_queue.h"

// ============================================================================
// ffplayer.h —— 与 Qt 解耦的播放器内核（对标原 ff_ffplay.cpp 的 FFPlayer）
//
// 【本文件当前进度：M9（音频链路 + 音频主时钟）】
//   ✅ readThread            解复用：av_read_frame，按流分发到 audioQ_/videoQ_
//   ✅ audioDecodeThread     音频解码：packet -> frame，入 sampQ_
//   ✅ SDL3 音频线程          经 AudioDevice 回调 pullAudio()：取 sampQ_、
//                            重采样为 S16、推进【音频主时钟】
//   ⏳ videoDecodeThread     M10
//   ⏳ videoRefreshThread    M10：以音频主时钟为基准到点渲染
//
// 【线程模型（当前 3 条，最终 5 条）】
//
//   ┌──────────────┐  put()   ┌──────────┐  get()  ┌───────────────────┐
//   │  readThread  │ ───────> │ audioQ_  │ ──────> │ audioDecodeThread │
//   └──────────────┘          └──────────┘         └─────────┬─────────┘
//                                                            │ push()
//                                                            ▼
//                                                    ┌──────────────┐
//                    SDL 音频线程  ── peekReadable ─> │   sampQ_     │
//                    (pullAudio)    <── next() ─────  └──────────────┘
//                          │
//                          ├── swresample 转成 S16 ──> 声卡
//                          └── audClk_.set() 推进主时钟
//
// 【★ 本模块最重要的并发设计：用"单线程独占"代替加锁】
//   下面这些成员【只被 SDL 音频线程】访问（pullAudio 及其调用链）：
//       swr_ / srcLayout_ / tgtLayout_ / srcFreq_ / srcFmt_ / tgtFreq_
//       resampleBuf_ / audioBuf_ / audioBufSize_ / audioBufIndex_ / audioClock_
//   因为只有一条线程碰它们，所以【一把锁都不需要】。
//   这是 M6/M7 反复强调"回调跑在 SDL 音频线程里"的真正用处 ——
//   加锁是最贵的同步手段，而"让数据只属于一个线程"是免费的。
//
//   对比：audClk_ 被音频线程 set、被视频线程/UI get（M10 会用到），
//        所以 Clock 内部自带锁；sampQ_ 被两条线程访问，队列自带锁。
//        ——"谁需要跨线程，谁才付锁的成本"，这才是正确的粒度。
// ============================================================================

class FFPlayer
{
public:
    FFPlayer();
    ~FFPlayer();

    FFPlayer(const FFPlayer&) = delete;
    FFPlayer& operator=(const FFPlayer&) = delete;

    // 异步准备：启动解复用线程后【立刻返回】，成败通过 msgQ_ 通知。
    int prepare(const std::string& url);

    // 停止并释放全部资源，可重复调用。
    void close();

    // 请求 seek 到指定毫秒位置（异步，由 readThread 执行）。
    void seekMs(int64_t ms);

    // ---- 音频输出控制（都是对 AudioDevice 的薄封装）----
    // 注意 setPaused 里有一处"时钟重锚定"，是同步正确性的关键，见 .cpp。
    void setPaused(bool paused);
    void setVolume(float linear01);
    void setSpeed(float ratio);

    // 当前播放位置（秒）。数据来源就是音频主时钟。
    // 未锚定时返回 0（而不是 NaN）—— 上层界面不需要知道 NaN 这个概念。
    double  positionSeconds() const;

    // 总时长（毫秒）。未知返回 -1。
    int64_t durationMs() const;

    MessageQueue& messages() { return msgQ_; }

    // ---- 以下为【学习沙盒为可测试性加的】，最终工程里没有 ----
    PacketQueue& audioQueue() { return audioQ_; }
    PacketQueue& videoQueue() { return videoQ_; }

    // 音频输出的统计量，用来验证"重采样确实产出了正确的声音数据"。
    struct AudioOutInfo {
        int     targetSampleRate = 0;   // 送进声卡的采样率
        int     targetChannels   = 0;
        int     decodedFrames    = 0;   // 解码出的音频帧数
        int     swrRebuilds      = 0;   // swresample 上下文重建次数（理想是 1）
        int64_t pcmBytes         = 0;   // 累计送给声卡的字节数
        int     pcmPeak          = 0;   // 见过的最大采样绝对值（0 = 全是静音）
    };
    AudioOutInfo audioOutInfo() const;

private:
    // ---- 线程函数 ----
    void readThread();
    void handleSeekRequest();
    void audioDecodeThread();

    // 取一帧解码音频 -> swresample 重采样为 S16 -> 存进【自有缓冲】。
    // 返回本次可用的字节数；-1 表示暂时没有数据。
    int decodeOneAudioFrame();

    // 供 SDL 音频回调拉取 PCM。返回实际写入 dst 的字节数（<=0 表示没数据）。
    int pullAudio(uint8_t* dst, int bytes);

    int openComponent(AVStream* stream, AVMediaType type);

    // ---- 消息 / 队列 / 解码器 / 时钟 / 输出 ----
    MessageQueue msgQ_;
    PacketQueue  audioQ_;
    PacketQueue  videoQ_;
    FrameQueue   sampQ_;
    FrameQueue   pictQ_;            // M10 才会用到
    Decoder      audDec_;
    Decoder      vidDec_;           // M10 才会用到
    Clock        audClk_;           // ★ 音频主时钟：全项目的同步基准
    AudioDevice  audioDev_;

    // ---- 解复用 ----
    AVFormatContextPtr ic_;
    AVStream* audioSt_ = nullptr;
    AVStream* videoSt_ = nullptr;
    int       audioIdx_ = -1;
    int       videoIdx_ = -1;
    AVRational videoTb_{};
    AVRational frameRate_{};

    std::thread readThr_;
    std::thread refreshThr_;        // M10 才会用到
    std::string url_;
    std::atomic<bool>    abort_{true};
    std::atomic<bool>    paused_{false};
    std::atomic<bool>    eof_{false};
    std::atomic<int64_t> durationUs_{AV_NOPTS_VALUE};

    // ---- seek 请求（只在 readThread 里真正执行）----
    std::mutex ctlMtx_;
    bool       seekReq_   = false;
    int64_t    seekPosUs_ = 0;

    // ---- 音频重采样状态（★ 仅 SDL 音频线程访问，无需加锁）----
    SwrContextPtr   swr_;
    AVChannelLayout srcLayout_{};      // 解码器输出的声道布局
    AVChannelLayout tgtLayout_{};      // 输出设备的声道布局
    int             srcFreq_ = 0;      // 解码器输出的采样率
    AVSampleFormat  srcFmt_  = AV_SAMPLE_FMT_NONE;
    int             tgtFreq_ = 0;      // 输出设备的采样率

    std::vector<uint8_t> resampleBuf_; // 重采样后的 PCM 缓冲（自有、可复用）
    const uint8_t*       audioBuf_     = nullptr;  // 指向 resampleBuf_，
                                                   // 同时兼作"是否有效"的标志
    int                  audioBufSize_  = 0;       // 当前帧产出的总字节数
    int                  audioBufIndex_ = 0;       // 已被取走的字节数（读游标）
    double               audioClock_    = std::nan("");  // 当前帧的 PTS（秒）

    // ---- 沙盒统计量（最终工程里没有）----
    mutable std::mutex statMtx_;
    int                statDecodedFrames_ = 0;
    int                statSwrRebuilds_   = 0;
    int64_t            statPcmBytes_      = 0;
    int                statPcmPeak_       = 0;
};

#endif // FFPLAYER_H
