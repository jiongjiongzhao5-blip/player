#include "message_queue.h"

void MessageQueue::start()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_request_ = false;
        queue_.clear();
        // 投入一条 FLUSH 哨兵。
        //
        // 这是从 ijkplayer 继承的习惯：让消费者"一启动就有事可做"，
        // 有机会在循环第一轮做初始化（比如重置界面状态、清空上一首的残留）。
        //
        // 诚实地说：在本项目的实现里它没有实际作用 ——
        // MediaPlayer::eventLoop 的 switch 没有 FLUSH 分支，会走到 default
        // 被忽略。保留它主要是为了跟上游设计保持一致，也留个"复位"的锚点。
        queue_.push_back(Message{FFMsg::FLUSH, 0, 0});
        cond_.notify_all();
    }
}

void MessageQueue::abort()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_request_ = true;
        // notify_all 而非 notify_one：可能多个线程在等（虽然本类实际只有一个
        // 消费者，但保持和 PacketQueue 一致的习惯，避免将来加线程时踩坑）。
        cond_.notify_all();
    }
}

void MessageQueue::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
}

void MessageQueue::postPrivate(Message m)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 终止态直接丢弃。
        // 这是个刻意的"静默失败"：消息是"尽力而为"的通知，
        // 在队列没启动或已关闭时，通知本身已经没有意义了，
        // 所以不值得为它返回错误码、让每个调用点都去判断。
        if (abort_request_)
            return;
        queue_.push_back(m);
        cond_.notify_one();          // 只有一个消费者，叫醒一个就够了
    }
}

void MessageQueue::post(FFMsg what)
{
    postPrivate(Message{what, 0, 0});
}

void MessageQueue::post(FFMsg what, int arg1)
{
    postPrivate(Message{what, arg1, 0});
}

void MessageQueue::post(FFMsg what, int arg1, int arg2)
{
    postPrivate(Message{what, arg1, arg2});
}

int MessageQueue::get(Message& out, bool block)
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (abort_request_)
            return -1;                        // 终止优先级最高

        if (!queue_.empty()) {
            out = queue_.front();             // 值拷贝：Message 只有 12 字节，无所谓
            queue_.pop_front();
            return 1;
        }

        if (!block)
            return 0;                         // 非阻塞且无消息

        // 不带谓词的 wait，靠外层 for(;;) 重新判断 ——
        // 好处是无论被谁唤醒，都会回到循环开头重新检查所有条件。
        cond_.wait(lock);
    }
}

void MessageQueue::remove(FFMsg what)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // 用迭代器 erase 的标准写法：erase 返回下一个有效迭代器，
    // 所以"要删除"时不要 ++it，"保留"时才 ++it。
    // 写错成 it = queue_.erase(it) 之后再 ++it，就会跳过元素甚至迭代器失效。
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->what == what)
            it = queue_.erase(it);
        else
            ++it;
    }
}
