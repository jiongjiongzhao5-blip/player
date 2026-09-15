#include "packet_queue.h"

// 析构时清空队列。这是"最后一道保险"：
// 如果谁忘了在退出前 flush，队列里残留的包（每个都持有一块 buffer）
// 就会被 unique_ptr 逐个析构，而 Item 的析构会触发 AVPacket 的析构...
// 等等 —— AVPacket 是 C 结构体，没有析构函数！所以这里必须显式 unref。
// 这正是 clearLocked() 存在的意义。
PacketQueue::~PacketQueue()
{
    flush();
}

bool PacketQueue::overLimitLocked() const
{
    return size_bytes_ > kMaxQueueBytes || nb_packets_ > kMaxQueuePackets;
}

// 调用时必须已持有 mutex_
void PacketQueue::clearLocked()
{
    while (!queue_.empty()) {
        auto& front = queue_.front();
        // 只有真实数据包才需要 unref；flush 标记没有 buffer，unref 也无害，
        // 但显式跳过分得更清楚。
        if (!front->flush)
            av_packet_unref(&front->pkt);
        queue_.pop_front();          // unique_ptr 析构 -> 释放 Item 本身
    }
    nb_packets_ = 0;
    duration_   = 0;
    size_bytes_ = 0;
}

// 调用时必须已持有 mutex_
int PacketQueue::pushFlushLocked()
{
    ++serial_;                                  // ★ 只有 flush 包推进播放序列
    auto item    = std::make_unique<Item>();
    item->serial = serial_;
    item->flush  = true;
    queue_.push_back(std::move(item));
    ++nb_packets_;
    cond_.notify_all();
    return 0;
}

void PacketQueue::start()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_request_ = false;
        pushFlushLocked();                      // serial: 0 -> 1
    }
}

void PacketQueue::abort()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_request_ = true;
        // 必须 notify_all 而不是 notify_one：
        // 此刻可能同时在等待的有【两类】线程 —— 生产者卡在"队列满"，
        // 消费者卡在"队列空"。只叫醒一个，另一个就会永远睡下去。
        cond_.notify_all();
    }
}

void PacketQueue::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);
    clearLocked();
    // 清空后队列必然不满，所以这里叫醒的是被背压卡住的生产者。
    cond_.notify_all();
}

void PacketQueue::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    clearLocked();
    pushFlushLocked();                          // serial +1
}

int PacketQueue::put(AVPacket* pkt)
{
    {
        std::unique_lock<std::mutex> lock(mutex_);

        // 已终止：不再收包。注意要 unref —— 否则这块 buffer 就泄漏了，
        // 因为 move_ref 还没发生，所有权还在调用方（也就是我们）手上。
        if (abort_request_) {
            av_packet_unref(pkt);
            return -1;
        }

        // ★ 背压：队列超限时阻塞在这里，把"内存压力"反向传导给读线程。
        //   wait 的谓词必须同时检查 abort_request_，否则退出时会死等。
        cond_.wait(lock, [this] {
            return abort_request_ || !overLimitLocked();
        });
        if (abort_request_) {                   // 被 abort 叫醒的，放弃入队
            av_packet_unref(pkt);
            return -1;
        }

        auto item    = std::make_unique<Item>();
        item->serial = serial_;                 // ★ 普通包继承当前序列，绝不自增
        item->flush  = false;
        size_bytes_ += pkt->size;
        duration_   += pkt->duration;
        // 关键：转移底层 buffer 的所有权，而不是 memcpy。
        // move_ref 之后 pkt 变成空包，我们不需要（也不能）再 unref 它。
        av_packet_move_ref(&item->pkt, pkt);
        ++nb_packets_;
        queue_.push_back(std::move(item));
    }
    // 在锁外 notify：被叫醒的线程不用等我们放锁，少一次上下文切换的等待。
    cond_.notify_one();                         // 唤醒一个消费者
    return 0;
}

int PacketQueue::putFlush()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (abort_request_)
            return -1;
        pushFlushLocked();
    }
    cond_.notify_one();
    return 0;
}

int PacketQueue::putNullPacket(int streamIndex)
{
    // 零初始化即为"空包"：data==nullptr、size==0。
    // put() 内部会 move_ref 一个空包 —— 结果就是队列里多了一个"没有数据的包"，
    // 解码线程看到 size==0 就知道这一路流到底了。
    AVPacket pkt{};
    pkt.stream_index = streamIndex;
    return put(&pkt);
}

int PacketQueue::get(AVPacket* pkt, bool block, int* serial, bool* isFlush)
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (abort_request_)
            return -1;                          // 终止优先级最高，哪怕队列里还有货

        if (!queue_.empty()) {
            auto item = std::move(queue_.front());   // 取出并持有所有权
            queue_.pop_front();
            --nb_packets_;
            duration_   -= item->pkt.duration;
            size_bytes_ -= item->pkt.size;

            if (serial)  *serial  = item->serial;
            if (isFlush) *isFlush = item->flush;

            if (item->flush) {
                // flush 标记没有真实数据。但调用方传进来的 pkt 可能是上一次
                // 用剩下的（还持有 buffer），所以这里要 unref 一下让它变干净。
                av_packet_unref(pkt);
            } else {
                av_packet_move_ref(pkt, &item->pkt);
            }

            // 腾出空间了，叫醒可能正被背压卡住的生产者。
            cond_.notify_all();
            return 1;
        }

        if (!block)
            return 0;                           // 非阻塞且无数据

        // 阻塞等待。注意这里用的是不带谓词的 wait()，靠外层 for(;;) 重新检查条件。
        // 这样写的好处：无论被谁唤醒（新数据 / abort / flush），都会回到循环
        // 开头重新判断，不会漏掉任何一种情况。
        cond_.wait(lock);
    }
}

bool PacketQueue::isAborted() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return abort_request_;
}

int PacketQueue::serial() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return serial_;
}

int PacketQueue::nbPackets() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return nb_packets_;
}

int64_t PacketQueue::duration() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return duration_;
}
