#ifndef DECODER_H
#define DECODER_H

#include <functional>
#include <thread>

#include "av_utils.h"
#include "frame_queue.h"
#include "packet_queue.h"

// ============================================================================
// decoder.h —— 单路流（音频或视频）的解码器封装
//
// 【在项目里的位置】
//   它是流水线中间那一段：把"压缩数据包"变成"原始帧"。
//   一条流一个实例 —— 本项目里有两个：audDec_（音频）和 vidDec_（视频）。
//
//     PacketQueue ──get()──> [Decoder] ──产出帧──> FFPlayer 把它写进 FrameQueue
//       (压缩)                 (解压)                    (原始)
//
//   ⚠ 注意职责边界：Decoder 【不碰 FrameQueue】。
//     它只负责"给我一个 AVFrame，我解一帧出来给你"（decodeFrame 的契约）。
//     把帧放进队列、计算 pts/duration 这些事，是 FFPlayer 的解码线程做的
//     （M9/M10 会讲）。这样 Decoder 足够简单，也方便单独测试。
//     唯一的例外是 abort() 要往 FrameQueue 发信号 —— 因为那是"唤醒"而不是"入队"，
//     属于退出流程的一部分。
//
// 【相对原工程的现代化改造】
//   * AVCodecContext 用 unique_ptr 管理，异常/析构不会泄漏；
//   * 解码统一走【新的 send_packet / receive_frame API】
//     （旧的 avcodec_decode_video2 / avcodec_decode_audio4 在 FFmpeg 4 已废弃、
//       FFmpeg 5 起彻底移除，所以这是必答题而不是选择题）；
//   * 线程对象用 std::thread 值成员持有，join 后自动回收，不再 new/delete；
//   * 识别 PacketQueue 中的 flush 标记并调用 avcodec_flush_buffers，
//      从而正确支持 seek。
// ============================================================================

class Decoder
{
public:
    Decoder() = default;
    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // 依据一路流（AVStream）创建并打开对应的解码器。
    //
    // 【为什么传 AVStream* 而不是 AVCodecParameters*？】
    //   因为我们除了需要参数，还需要 stream->time_base（见下面 pkt_timebase 的说明）。
    //   传 AVStream* 一次拿全，调用方也少写代码。
    //
    // 返回 0 成功（负数为 FFmpeg 错误码）。
    int open(AVStream* stream);

    // 绑定包队列并启动解码工作线程。
    //
    // ★ 调用契约：调用本方法【之前】必须先 queue.start()。
    //   原因见 .cpp 里的详细说明（队列的生死不该绑在 Decoder 上，
    //   而且 PacketQueue::start() 不幂等）。FFPlayer 在 openComponent
    //   里启动队列。
    //
    // worker 是一个 std::function<void()> —— 也就是"解码线程的主体函数"。
    // 为什么不把主循环直接写进 Decoder？
    //   因为主循环里要做的事（算 pts、算 duration、往哪个 FrameQueue 写）
    //   是【调用方（FFPlayer）的知识】，不是 Decoder 的知识。
    //   让调用方把循环体传进来，Decoder 只负责"开线程 + 提供 decodeFrame 工具"，
    //   职责就干净了 —— 这是"策略注入"的用法。
    void start(PacketQueue& queue, std::function<void()> worker);

    // 请求解码线程退出。
    //
    // ★ 这是【两步】操作，少一步就会永久卡死（M3 实验台 [6] 专门验证过）：
    //     ① queue_->abort()      置标志位，让阻塞在 get() 的线程醒来
    //     ② frameQueue.signal()  叫醒睡在帧队列上的线程
    //   abort() 只是"把标志位设成 true"，它不会自动唤醒卡在【别的对象】
    //   的条件变量上睡觉的线程 —— 那需要那个对象自己 notify。
    //
    // 随后 join 线程、清空包队列。
    void abort(FrameQueue& frameQueue);

    // 取一帧解码结果。这是本类唯一的核心方法。
    //
    // 返回值三态（刻意与 PacketQueue::get 保持同样的约定，降低心智负担）：
    //    1 = 取到一帧（frame 已被填好）
    //    0 = 解码已到 EOF（输入耗尽且解码器已排空）
    //   -1 = 请求退出（上游队列被 abort）
    //
    // 注意：本方法【可能阻塞】—— 当解码器需要新数据时，它会阻塞在
    //       queue_->get(&pkt, true) 上等包。
    int decodeFrame(AVFrame* frame);

    // ---- 观察用访问器 ----
    AVCodecContext* ctx() const { return avctx_.get(); }
    AVMediaType     type() const { return type_; }
    // 最近一次取到的包所属的播放序列。
    // ⚠ 本工程存了它但没有任何地方拿它做判断 —— 详见 M3 讲的 serial 遗留问题。
    int             pktSerial() const { return pktSerial_; }

private:
    AVCodecContextPtr avctx_;              // 解码器上下文（RAII）
    PacketQueue*      queue_ = nullptr;    // 上游包队列（不持有所有权，只借用）
    AVMediaType       type_  = AVMEDIA_TYPE_UNKNOWN;
    int               pktSerial_ = 0;
    std::thread       thread_;             // 值成员：join 后自动回收
};

#endif // DECODER_H
