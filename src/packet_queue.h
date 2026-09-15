#ifndef PACKET_QUEUE_H
#define PACKET_QUEUE_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include "av_utils.h"

// ============================================================================
// packet_queue.h —— 解复用后、解码前的 AVPacket 队列
//
// 【在项目里的位置】
//   它是"读线程 → 解码线程"之间的血管，位于整条流水线的第一段：
//
//     readThread  --put()-->  [PacketQueue]  --get()-->  decodeThread
//
//   一个包进、一个包出，所以队列本身不搬运数据，只做"寄存 + 排队 + 唤醒"。
//
// 【为什么必须要有它】
//   解复用（读文件/网络）比解码快得多，解码又比"按帧率播放"快得多。
//   如果读到一个包就立刻交给解码器、解码完立刻送显示，那么整条流水线会被
//   最慢的一环（播放）拖住，读线程大部分时间在睡觉 —— 一旦网络抖动或磁盘
//   卡顿，声音/画面立刻就断。
//   队列的作用就是把三段速度解耦：读线程可以预读一批数据囤在队列里，
//   后面即使临时读不到数据，解码和播放也能靠存货继续跑。
//
// 【相对老工程（ff_ffplay_def.cpp 的手写链表 + SDL_mutex/SDL_cond）的改造】
//   * std::deque + std::mutex + std::condition_variable，节点用 unique_ptr 管理，
//     不用再手写 next 指针和 malloc/free；
//   * AVPacket 一律通过 av_packet_move_ref 转移所有权，绝不浅拷贝结构体
//     （浅拷贝会让两个 AVPacket 指向同一块 buffer，unref 两次必崩）；
//   * 用显式的 bool flush 标记取代 ffplay 中"比较静态 flush_pkt 指针地址"
//     这种 C 技巧；
//   * ★ serial（播放序列）只在放入 flush 包时 +1，普通包继承当前值 ——
//     老工程半成品代码里每个包都 +1，直接让 seek 判定失效（详见 .cpp）；
//   * 新增背压上限，防止解复用线程把整个文件一次性读进内存。
//
// 【线程模型】
//   demux 线程  -> put() / putFlush() / putNullPacket()
//   解码线程    -> get()
//   控制线程    -> flush() / reset() / abort()
// ============================================================================

class PacketQueue
{
public:
    PacketQueue() = default;
    ~PacketQueue();

    // 队列不可拷贝：持有 mutex / condition_variable / 独占资源，
    // 拷贝它们毫无意义且危险，直接禁用。
    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    // ---- 生命周期 ----

    // 启用队列：清除 abort 标志，并放入一个 flush 包（serial +1）。
    // 注意：abort_request_ 初值是 true，所以队列创建后是"终止态"，
    //       必须先 start() 才能用。这样设计是为了让"忘了 start"变成
    //       "put/get 立刻失败"，而不是"静默地不工作"。
    void start();

    // 请求终止：唤醒所有阻塞的 get()/put()，之后两者立即失败。
    // 用于线程退出流程 —— 是"打破阻塞等待"的唯一手段。
    void abort();

    // 清空缓存包（seek 时使用，不改变 abort 状态，也不改变 serial）
    void flush();

    // seek 后重新对齐：清空缓存 + 放入新的 flush 包（serial +1）。
    // ★ 这才是 seek 时该调用的：既丢掉旧数据，又让解码线程知道"到边界了，
    //   把解码器内部缓存也刷掉"。
    void reset();

    // ---- 生产 ----

    // 放入普通包。内部 av_packet_move_ref 把 pkt 的底层 buffer 转移给队列，
    // 调用后 pkt 被清空（调用方不需要也不应该再 unref 它）。
    // 返回 0 成功，-1 表示队列已终止。
    // 注意：队列满时本函数会【阻塞】，等消费者取走数据再继续（背压）。
    int put(AVPacket* pkt);

    // 放入 flush 包（serial +1）。单独暴露这个接口，用于"强制解码器刷新
    // 内部缓存"的场景。
    int putFlush();

    // 放入空包（data==nullptr, size==0），用于标记"某一路流读到结尾"。
    // 沿用 ffplay 的约定：空包是流结束的信令，不是真实数据。
    int putNullPacket(int streamIndex);

    // ---- 消费 ----

    // 取包。返回值是三态：
    //    1 = 取到包（可能是真实包，也可能是 flush 标记包）
    //    0 = 非阻塞模式且当前无包
    //   -1 = 队列已终止（调用方应该退出线程）
    //   serial   ：可选出参，本次取到的包所属的播放序列
    //   isFlush  ：可选出参，本次取到的是否为 flush 标记（此时 pkt 被 unref）
    //
    // 为什么不用 bool？因为"没数据"和"要退出"是两件完全不同的事，
    // 调用方的处理方式也完全不同，bool 表达不了三态。
    int get(AVPacket* pkt, bool block, int* serial = nullptr, bool* isFlush = nullptr);

    // ---- 查询 ----

    bool    isAborted() const;
    int     serial() const;
    int     nbPackets() const;
    int64_t duration() const;

private:
    // 队列里的一项。用 struct 而不是直接存 AVPacket，是因为除了包本身，
    // 还要一起记住它属于哪个 serial、以及它是不是一个 flush 标记。
    struct Item {
        AVPacket pkt{};            // 零初始化：构造出来就是"空包"状态
        int  serial = 0;
        bool flush  = false;       // true 表示这是 flush 标记，不是真实数据包
    };

    // 下面三个都要求调用方【已经持有 mutex_】（约定用后缀 Locked 表示）
    int  pushFlushLocked();
    bool overLimitLocked() const;
    void clearLocked();

    mutable std::mutex      mutex_;
    std::condition_variable cond_;
    // deque 而不是 vector：队列两端都要出/入，且长度变化频繁，
    // 用 vector 会有大量搬移。deque 两端操作都是 O(1)。
    std::deque<std::unique_ptr<Item>> queue_;

    bool    abort_request_ = true;   // 见 start() 的说明：默认就是终止态
    int     serial_        = 0;
    int     nb_packets_    = 0;
    int64_t duration_      = 0;      // 所有包 duration 之和（微秒，供将来做缓冲估算）
    int64_t size_bytes_    = 0;

    // 背压上限：避免解复用线程把整个文件一次性读进内存。
    // 两个条件满足其一就认为"满了"，put() 会阻塞。
    static constexpr int64_t kMaxQueueBytes   = 16 * 1024 * 1024;  // 16 MiB
    static constexpr int     kMaxQueuePackets = 8000;
};

#endif // PACKET_QUEUE_H
