#include "frame_queue.h"
#include "packet_queue.h"

FrameQueue::FrameQueue(int maxSize)
    : maxSize_(maxSize)
{
    queue_.resize(maxSize_);
    // ★ 一次性把所有槽位的 AVFrame 都分配好，之后整个播放过程中不再
    //   new/delete —— 这是"槽位复用"设计的前提。
    //   后续 next() 只 av_frame_unref() 释放帧挂着的像素数据，
    //   AVFrame 对象本身一直活着，等下一次写入时复用。
    for (auto& f : queue_)
        f.frame = make_frame();
}

void FrameQueue::signal()
{
    std::lock_guard<std::mutex> lock(mutex_);
    cond_.notify_all();
}

// ---------------------------------------------------------------------------
// 只读访问：注意这三个都【不加锁】也不检查 size
// ---------------------------------------------------------------------------
// 设计取舍：这几个是"快速查看"，调用方（渲染线程）自己保证 size>0 才调。
//          加锁会让它和后面的 next() 之间出现"检查与使用不一致"的窗口，
//          反而不如"约定好单线程访问"清晰。渲染线程是唯一的消费者，
//          所以这个约定成立。
// ---------------------------------------------------------------------------

Frame* FrameQueue::peek()
{
    return &queue_[rindex_ % maxSize_];     // 当前待播放帧
}

Frame* FrameQueue::peekNext()
{
    return &queue_[(rindex_ + 1) % maxSize_];
}

Frame* FrameQueue::peekLast()
{
    // 【代码审查提示】这一句有问题，值得单独说：
    //
    //   在 ffplay 里，peekLast 返回的是"上一帧"（配合它的 rindex_shown /
    //   keep_last 机制，用于暂停时保留最后一帧画面不消失）。
    //   但我们的实现省掉了 rindex_shown，于是 rindex_ 始终已经是规范化过的
    //   下标，这里的 queue_[rindex_] 和上面 peek() 的 queue_[rindex_ % maxSize_]
    //   指向的是【同一个槽位】—— 也就是说 peekLast() 完全等价于 peek()，
    //   并没有"回到上一帧"。
    //
    //   目前的处理：保留（因为整个工程没有任何地方调用它，不影响功能），
    //   但你要知道它是抄接口抄出来的"死代码"，别照着它的注释去理解行为。
    //   真要做"暂停保留最后一帧"，得把 rindex_shown 机制补回来。
    return &queue_[rindex_];
}

// ---------------------------------------------------------------------------
// 生产端（解码线程）
// ---------------------------------------------------------------------------
Frame* FrameQueue::peekWritable()
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 两个能打破等待的条件：① 有空槽；② 上游已终止。
        // 少了 ② 的话，退出时解码线程会永远卡在这里。
        cond_.wait(lock, [this] {
            return size_ < maxSize_ || (pktq_ && pktq_->isAborted());
        });
    }
    // 出锁之后再判断 —— 因为 isAborted() 自己要加锁，不能在持有 mutex_ 时调用
    //（虽然它是别的对象的锁，不会死锁，但保持"不在锁内调外部函数"的好习惯）。
    if (pktq_ && pktq_->isAborted())
        return nullptr;
    return &queue_[windex_];
}

void FrameQueue::push()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (++windex_ == maxSize_)      // 绕圈：到末尾就回到 0
            windex_ = 0;
        ++size_;
        cond_.notify_all();             // 有新帧了，叫醒可能在等的消费者
    }
}

// ---------------------------------------------------------------------------
// 消费端（播放/渲染线程）
// ---------------------------------------------------------------------------
Frame* FrameQueue::peekReadable()
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] {
            return size_ > 0 || (pktq_ && pktq_->isAborted());
        });
    }
    if (pktq_ && pktq_->isAborted())
        return nullptr;
    return &queue_[rindex_ % maxSize_];
}

void FrameQueue::next()
{
    // ★ 先释放数据，再推进索引。
    //   next() 只 av_frame_unref（放掉像素/采样数据、把 AVFrame 清空成可复用状态），
    //   不销毁 AVFrame 对象 —— 这就是"槽位复用"。
    //
    //   ⚠ 这也是老工程的一个坑所在：调用方一旦 next()，当前帧的数据立刻失效。
    //     老代码里"先调 next() 再去读帧里的 data 指针"就是典型的 use-after-free。
    //     （我们在 M9 音频链路里会看到正确的处理顺序：先把数据拷进自有缓冲，
    //       再 next()。）
    av_frame_unref(queue_[rindex_].frame.get());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (++rindex_ == maxSize_)
            rindex_ = 0;
        --size_;
        cond_.notify_all();             // 腾出槽位，叫醒可能在等的生产者
    }
}

int FrameQueue::nbRemaining() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
}

void FrameQueue::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);
    // 把每个槽里挂着的帧数据都放掉（但保留 AVFrame 对象本身）
    for (auto& f : queue_)
        av_frame_unref(f.frame.get());
    // 索引全部归零。注意这里没有顺手清 pts/width/height 等元信息 ——
    // 不影响正确性，因为写入方每次都会重新赋值。
    rindex_ = windex_ = size_ = 0;
    cond_.notify_all();
}
