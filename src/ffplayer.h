#ifndef FFPLAYER_H
#define FFPLAYER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "av_utils.h"
#include "decoder.h"
#include "message_queue.h"
#include "packet_queue.h"

// ============================================================================
// ffplayer.h —— 与 Qt 解耦的播放器内核（对标原 ff_ffplay.cpp 的 FFPlayer）
//
// 【本文件当前进度：M8（解复用与流打开）】
//   只实现"把文件拆成包、按流分发到两个队列"这一段：
//     prepare() / close() / readThread() / handleSeekRequest() / openComponent()
//   解码线程、音频输出、视频渲染、播放控制会在 M9~M11 逐步长进来。
//
// 【线程模型（当前只有 1 条线程，最终会有 5 条）】
//   ✅ readThread          解复用：av_read_frame，按流分发到 audioQ_/videoQ_
//   ⏳ audioDecodeThread   M9：packet -> frame，入 sampQ_
//   ⏳ videoDecodeThread   M10：packet -> frame，入 pictQ_
//   ⏳ SDL3 音频线程        M9：经 AudioDevice 回调 pullAudio()
//   ⏳ videoRefreshThread  M10：以音频主时钟为基准到点渲染
//
// 【与上层（Qt）的关系】
//   完全解耦：本类不含任何 Qt 头文件，也不调用任何 Qt API。
//   对外只有两个出口：
//     ① MessageQueue  msgQ_  —— 投递状态事件（Prepared / Completed / Error…）
//     ② 帧回调（M9/M10 加入）—— 把解码好的视频帧抛给上层
//   这样 M8~M11 我们都能脱离 GUI 单独测试内核。
// ============================================================================

class FFPlayer
{
public:
    FFPlayer();
    ~FFPlayer();

    FFPlayer(const FFPlayer&) = delete;
    FFPlayer& operator=(const FFPlayer&) = delete;

    // 异步准备：启动解复用线程后【立刻返回】。
    //
    // 【为什么是异步的？为什么返回值不能判断成败？】
    //   avformat_open_input 可能阻塞很久（网络流、慢磁盘、甚至挂住），
    //   如果同步执行就会把调用方（GUI 主线程）卡死。
    //   所以这里只负责"起线程"，真正打开文件、找流、开解码器都在
    //   readThread 里做，结果通过 msgQ_ 投递事件来通知：
    //     成功 -> FFMsg::Prepared
    //     失败 -> FFMsg::Error（arg1 是 FFmpeg 错误码）
    //   ★ 代价：调用方不能靠返回值判断成败，必须监听消息。
    //     这是"异步接口"的典型代价 —— 换来的是 UI 永不卡顿。
    //
    // 返回值 0 表示"线程启动成功"，不代表文件打开成功。
    int prepare(const std::string& url);

    // 停止并释放全部资源，可重复调用。
    // 这是本类唯一需要小心的"顺序敏感"函数，见 .cpp 里的详细说明。
    void close();

    // 请求 seek 到指定毫秒位置。异步 —— 只是置一个请求标志，
    // 真正执行 av_seek_frame 的是 readThread（见 handleSeekRequest）。
    //
    // 【为什么要绕这么一圈？】因为 av_seek_frame 必须和 av_read_frame
    // 在同一个线程里调用 —— 它们共享 AVFormatContext 的内部状态
    //（读缓冲、当前位置），跨线程并发调用是未定义行为。
    // 所以用"请求标志 + 由读线程执行"的模式，天然避免了加锁竞争。
    void seekMs(int64_t ms);

    // 总时长（毫秒）。未知返回 -1。
    // 从 prepare 起就可用（readThread 打开文件后会填），线程安全。
    int64_t durationMs() const;

    MessageQueue& messages() { return msgQ_; }

    // ---- 以下两个访问器是【学习沙盒为可测试性加的】，最终工程里没有 ----
    // 有了它们，实验台才能直接观察"包有没有被正确分发到两路队列"。
    // 顺带说一句，这类"缓冲水位"接口对做 UI 也有用（比如显示缓冲进度条）。
    PacketQueue& audioQueue() { return audioQ_; }
    PacketQueue& videoQueue() { return videoQ_; }

private:
    // ---- 线程函数 ----
    void readThread();
    // 由 readThread 调用：把待处理的 seek 请求真正执行掉
    void handleSeekRequest();

    // 打开一路流（创建并打开解码器，记录索引与时基）。
    // M9 会在这里加音频输出与解码线程，M10 加视频解码线程。
    int openComponent(AVStream* stream, AVMediaType type);

    // ---- 消息 / 队列 / 解码器 ----
    MessageQueue msgQ_;
    PacketQueue  audioQ_;
    PacketQueue  videoQ_;
    Decoder      audDec_;
    Decoder      vidDec_;

    // ---- 解复用相关 ----
    AVFormatContextPtr ic_;                 // RAII，close() 时自动 avformat_close_input
    AVStream* audioSt_ = nullptr;           // 裸指针：生命周期由 ic_ 管，我们只借用
    AVStream* videoSt_ = nullptr;
    int       audioIdx_ = -1;
    int       videoIdx_ = -1;
    AVRational videoTb_{};                  // 视频流时间基（M10 算 pts 用）
    AVRational frameRate_{};                // 视频帧率（M10 算帧时长用）

    std::thread readThr_;
    std::string url_;

    // 这些标志会被多条线程读写，必须用 atomic。
    // 只是 bool 读写的场景用 relaxed 语义就够（不需要同步别的内存），
    // 但默认的 seq_cst 更安全，性能差异在这个使用频率下可忽略。
    std::atomic<bool>    abort_{true};      // 初值 true：未 prepare 时就是"终止态"
    std::atomic<bool>    eof_{false};
    std::atomic<int64_t> durationUs_{AV_NOPTS_VALUE};

    // ---- seek 请求（只在 readThread 里真正执行 av_seek_frame）----
    std::mutex ctlMtx_;
    bool       seekReq_   = false;
    int64_t    seekPosUs_ = 0;
};

#endif // FFPLAYER_H
