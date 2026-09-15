#ifndef FRAME_QUEUE_H
#define FRAME_QUEUE_H

#include <condition_variable>
#include <mutex>
#include <vector>

#include "av_utils.h"

// ============================================================================
// frame_queue.h —— 解码后帧队列（"解码线程 → 播放/渲染线程"之间的血管）
//
// 【和 PacketQueue 的分工】
//   PacketQueue 传的是【压缩数据包】（AVPacket，一包可能几 KB 也可能几百 KB）；
//   FrameQueue  传的是【解码后的帧】（AVFrame，视频帧动辄几 MB）。
//   两者一个是"压缩态"、一个是"原始态"，尺寸和数量级完全不同，
//   所以数据结构、容量策略、接口设计都不一样。
//
// 【为什么用环形缓冲（定长数组 + 绕圈）而不是 deque？】
//   关键在于视频帧队列只开 3 个槽（ffplay 的 VIDEO_PICTURE_QUEUE_SIZE）！
//   原因有三：
//     1) 内存：一帧 1080p 的 YUV 就有 3MB，囤 100 帧就是 300MB，没必要 ——
//        视频是"边解边播"的流式消费，不需要大缓冲。
//     2) 槽位复用：AVFrame 对象本身可以反复使用，只有它"挂着的"那块像素
//        buffer 需要换。所以预先分配好固定几个 AVFrame，每次 next() 只
//        av_frame_unref（放掉数据、保留对象），下次直接复用 —— 避免了
//        频繁 alloc/free 带来的堆碎片和开销。
//     3) 定长 + 取模绕圈 = 索引运算，没有节点分配，缓存友好。
//
//   而 PacketQueue 的包大小差异极大、数量多，所以用 deque 动态增长更合适。
//   两个队列用不同的数据结构，不是随意选择，是被数据特征决定的。
//
// 【索引约定】（与 ffplay 完全一致，看懂这两个下标就看懂了这个类）
//   windex —— 下一个可写槽位（解码线程：peekWritable() 拿槽位，填完 push()）
//   rindex —— 下一个可读槽位（播放线程：peek()/peekReadable() 读，读完 next()）
//   两者都在 [0, maxSize) 之间绕圈，靠 size 区分"空"和"满"。
//
//   ⚠ 必须用 size 而不能用 "rindex == windex" 判断空/满 ——
//     绕圈之后这个等式在"空"和"满"两种情况下都成立，是经典陷阱。
// ============================================================================

class PacketQueue;   // 前向声明即可：这里只用到它的指针和 isAborted()

// 一个槽位。AVFrame 本身由 FrameQueue 预分配并持有，业务代码
// 只负责往 slot->frame 里填数据、往 slot 里写元信息。
struct Frame
{
    AVFramePtr frame;            // RAII 持有 AVFrame（FrameQueue 构造时分配，全程复用）
    double pts      = 0.0;       // 显示时间戳（秒，已从 time_base 换算过）
    double duration = 0.0;       // 这一帧该显示多久（秒）
    int    width    = 0;
    int    height   = 0;
    int    format   = 0;         // 视频是 AVPixelFormat，音频是 AVSampleFormat
};

class FrameQueue
{
public:
    // maxSize 由调用方决定：视频给 3，音频给 9（音频帧小、可以多囤一点）
    explicit FrameQueue(int maxSize);
    ~FrameQueue() = default;

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    // ★ 绑定"上游"的包队列。绑定之后，peekWritable/peekReadable 在阻塞等待时
    //   会顺带检查上游是否已 abort —— 这样 abort 一次就能把整条流水线上的
    //   线程全叫醒，不用逐个通知。
    void setPacketQueue(PacketQueue* pktq) { pktq_ = pktq; }

    // 手动唤醒所有阻塞的读/写线程。
    // 【为什么需要它】abort 只是"把标志位置成 true"，它不会自动唤醒
    // 卡在本对象 cond_ 上睡觉的线程 —— 那需要本对象自己 notify。
    // 所以完整流程是两步：上游 abort() 置标志 + 本对象 signal() 叫醒。
    void signal();

    // ---- 只读访问（不消费、不推进索引）----
    Frame* peek();                 // 当前待播放帧（要求 size>0）
    Frame* peekNext();             // 下一帧（要求 size>=2）
    Frame* peekLast();             // 最近读过的一帧

    // ---- 生产（解码线程）----
    // 阻塞直到有空槽位；若关联的 PacketQueue 已 abort，返回 nullptr。
    // 返回 nullptr 而不是抛异常 —— 内核代码用返回值表达失败，简单可控。
    Frame* peekWritable();
    void   push();                 // 填完了，推进 windex

    // ---- 消费（播放/渲染线程）----
    Frame* peekReadable();         // 阻塞直到有数据；abort 时返回 nullptr
    void   next();                 // 用完当前帧：释放其数据并推进 rindex

    // ---- 查询 ----
    int  nbRemaining() const;      // 未消费帧数（渲染线程用它判断"还有没有画面"）
    void flush();                  // 清空全部帧数据（seek 时用）

private:
    std::vector<Frame> queue_;     // 定长环形缓冲，长度 = maxSize_
    int rindex_  = 0;              // 下一个可读槽位
    int windex_  = 0;              // 下一个可写槽位
    int size_    = 0;              // 当前已填充的槽位数（判断空/满的唯一依据）
    int maxSize_ = 0;

    PacketQueue* pktq_ = nullptr;  // 上游，可为空（则不响应 abort）
    mutable std::mutex      mutex_;
    std::condition_variable cond_;
};

#endif // FRAME_QUEUE_H
