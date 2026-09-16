#ifndef MESSAGE_QUEUE_H
#define MESSAGE_QUEUE_H

#include <condition_variable>
#include <deque>
#include <mutex>

#include "ffmsg.h"

// ============================================================================
// message_queue.h —— 内核事件队列（"内核线程 → 上层事件线程"的单向通道）
//
// 【它解决什么问题】
//   内核里 5 条线程（读/解码/音频回调/刷新）随时可能产生"状态事件"：
//   打开成功了、准备就绪了、播完了、出错了。这些事件必须让 UI 知道。
//
//   最直觉的做法是回调函数：
//       player->setOnPrepared([] { ... });   // 直接在解码线程里调用！
//   问题很严重：
//     1) 回调跑在【投递者的线程】上 —— 上层代码会在解码线程里执行。
//        如果上层在回调里更新界面、写日志、甚至只是加个锁，就会拖慢
//        解码线程，直接表现为卡顿；反过来上层若阻塞，整条流水线停摆。
//     2) 上层往往要遵守自己的线程规则（比如 Qt 的"只能在 GUI 线程碰控件"），
//        而回调根本不给你选择线程的余地。
//     3) 回调难以取消、难以去重、难以延迟处理 —— 而"连续点 3 次暂停，
//        前面的请求已经无意义"是真实需求。
//
//   消息队列把"产生事件"和"处理事件"彻底解耦：
//     产生方只管 post（不阻塞、不关心谁在处理）；
//     处理方在自己的线程里按自己的节奏 get。
//   这是标准的"异步事件通道"模式。
//
// 【为什么不让内核直接发 Qt 信号？】
//   那样内核就依赖 Qt 了。本项目的核心设计目标之一是"FFPlayer 与 Qt 完全解耦"
//   —— 只有这样，M8~M11 阶段我们才能脱离 GUI 单独测试内核（命令行跑播放器）。
//   而且 Qt 信号需要一个 QObject 作为发送者，内核对象不是 QObject，也不该是。
//
// 【和 M3 的 PacketQueue 对比：同样是队列，为什么这个简单这么多】
//   | 维度     | PacketQueue              | MessageQueue            |
//   | 携带内容 | AVPacket（持堆内存）      | 3 个 int（纯值语义）    |
//   | 元素大小 | 几 KB ~ 几百 KB           | 12 字节                 |
//   | 所有权   | 需要 move_ref 转移        | 直接拷贝就行            |
//   | 背压     | 必须（否则内存爆）        | 不需要（最多几十条）    |
//   | serial   | 需要（seek 判新旧）       | 不需要                  |
//   | 析构清理 | 必须 flush（否则泄漏）    | 不需要（没有堆资源）    |
//
//   ★ 记住这个判据：队列里放的对象"是否持有需要转移的堆资源"，
//     决定了这个队列的复杂度。持有 —— 就要 move 语义 + 析构清理；
//     不持有 —— 值语义拷贝即可。
//
//   尽管简单这么多，它仍然保留了 start/abort/post/get 这套和 PacketQueue
//   一致的骨架 —— 一致性本身就是价值：读代码的人不用学第二套并发模型。
// ============================================================================

// 一条消息。故意做得极小：
//   * 只有一个枚举 + 两个整数，可以按值拷贝，没有生命周期问题；
//   * 两个整型参数是 ijkplayer 的约定，实测够用 —— 错误码、宽高、
//     旋转角度、序号都是整数。
//   * 代价：传不了字符串或结构体。本项目的上层只需要"知道发生了什么"，
//     具体内容（比如错误详情）由上层自己用错误码去查，所以不需要。
//     真需要传复杂数据时，正确做法是加一个引用计数的 payload 字段，
//     而不是把结构体硬塞进来（那会重新引入所有权问题）。
struct Message
{
    FFMsg what = FFMsg::FLUSH;
    int   arg1 = 0;
    int   arg2 = 0;
};

class MessageQueue
{
public:
    MessageQueue() = default;
    // 注意：这里【没有】析构函数，和 PacketQueue 不一样。
    // 因为 Message 不持有任何堆资源，deque 析构时自己就收拾干净了。
    // PacketQueue 必须写析构去 av_packet_unref，否则泄漏 —— 差别就在这。

    MessageQueue(const MessageQueue&) = delete;
    MessageQueue& operator=(const MessageQueue&) = delete;

    // 启用队列：清除 abort 标志、清空残留消息、投入一条 FLUSH。
    // ⚠ 注意队列【创建后处于终止态】（abort_request_ 初值为 true），
    //   在调用 start() 之前 post() 会被静默丢弃。这样设计的好处是
    //   "忘了 start"会表现为"收不到任何事件"，比较容易发现；
    //   坏处是它不报错，排查时要想一想。
    void start();

    // 终止并唤醒所有阻塞在 get() 上的线程。退出流程必须调用。
    void abort();

    // 清空已排队但尚未处理的消息（不改变 abort 状态）。
    void flush();

    // ---- 投递（内核线程调用）----
    // 三个重载只是为了写起来方便，最终都走 postPrivate。
    void post(FFMsg what);
    void post(FFMsg what, int arg1);
    void post(FFMsg what, int arg1, int arg2);

    // ---- 取出（上层事件线程调用）----
    // 返回值三态（与 PacketQueue::get 保持一致）：
    //    1 = 取到消息    0 = 非阻塞且队列空    -1 = 已终止
    int get(Message& out, bool block);

    // 移除队列中某一类的所有待处理消息。
    //
    // 【这个接口很有价值，但本项目当前没有调用它】
    //   它体现了消息队列相对回调的一个关键优势：**可以撤销还没被处理的事件**。
    //   典型场景：用户 1 秒内连点了 5 次暂停，队列里积压了 5 条 ReqPause，
    //   而实际只需要处理最后一次 —— 用 remove(ReqPause) 一把清掉。
    //   回调模型做不到这件事，因为事件在产生的瞬间就已经执行完了。
    void remove(FFMsg what);

private:
    void postPrivate(Message m);

    mutable std::mutex      mutex_;
    std::condition_variable cond_;
    std::deque<Message>     queue_;
    bool abort_request_ = true;      // 见 start() 的说明
};

#endif // MESSAGE_QUEUE_H
