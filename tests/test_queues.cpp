// ============================================================================
// tests/test_queues.cpp —— M3 实验台：验证 PacketQueue / FrameQueue
//
// 队列的正确性靠"读代码"是看不出来的，必须真跑并发场景。
// 这里 6 组测试覆盖：基本往返、serial 语义、背压、abort 唤醒、
//                    环形缓冲绕圈、以及 abort 的"两步唤醒"机制。
//
// 注意：所有阻塞类测试都配了"先确认它确实卡住了"的断言，
//       否则一个永远不会阻塞的实现也能骗过测试。
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>          // SetConsoleOutputCP

#include "frame_queue.h"
#include "packet_queue.h"

// ---------------------------------------------------------------------------
// 测试框架（极简）
// ---------------------------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* desc, const std::string& detail = {})
{
    if (cond) {
        ++g_pass;
        std::printf("  [PASS] %s\n", desc);
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s%s%s\n", desc,
                    detail.empty() ? "" : "  -> ", detail.c_str());
    }
}

static void section(const char* title)
{
    std::printf("\n==================================================\n");
    std::printf(" %s\n", title);
    std::printf("==================================================\n");
}

static void msleep(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// 造一个"带真实数据"的 AVPacket。
// av_new_packet 会分配一块引用计数的 buffer 并挂到 pkt 上，
// 这样 av_packet_move_ref 才有东西可转移（空包的转移没有意义）。
static AVPacketPtr makeFilledPacket(int size, uint8_t fill)
{
    AVPacketPtr pkt = make_packet();
    if (!pkt || av_new_packet(pkt.get(), size) < 0)
        return AVPacketPtr();
    std::memset(pkt->data, fill, size);
    pkt->duration = size;      // 随便给个值，便于观察 duration 统计
    return pkt;
}

// ===========================================================================
// [1] 基本往返 + flush 标记语义
// ===========================================================================
static void test_basicRoundTrip()
{
    section("[1] PacketQueue 基本往返 + flush 标记语义");

    PacketQueue q;
    q.start();                       // serial 0 -> 1，并压入一个 flush 标记

    AVPacketPtr pkt = make_packet();
    int  serial  = -1;
    bool isFlush = false;

    int r = q.get(pkt.get(), true, &serial, &isFlush);
    check(r == 1, "start() 之后能取到第一条记录");
    check(isFlush, "第一条确实是 flush 标记");
    check(serial == 1, "start() 让 serial 从 0 变成 1",
          "实际 serial=" + std::to_string(serial));
    check(pkt->size == 0, "flush 标记不携带数据");

    // 放入一个带数据的包，验证 move_ref 语义
    auto one = makeFilledPacket(64, 0xAB);
    check(one->size == 64, "准备好一个 64 字节的包");
    q.put(one.get());
    check(one->size == 0, "put() 之后原 packet 被 move_ref 清空（所有权已转移）");

    // 再放 2 个，验证顺序与内容
    for (int i = 0; i < 2; ++i) {
        auto p = makeFilledPacket(32, static_cast<uint8_t>(0xA0 + i));
        q.put(p.get());
    }
    check(q.nbPackets() == 3, "队列里有 3 个包",
          "实际 " + std::to_string(q.nbPackets()));

    bool allOk = true;
    int expect[3] = {0xAB, 0xA0, 0xA1};
    int expectSize[3] = {64, 32, 32};
    for (int i = 0; i < 3; ++i) {
        r = q.get(pkt.get(), true, &serial, &isFlush);
        if (r != 1 || isFlush || pkt->size != expectSize[i]
            || pkt->data[0] != static_cast<uint8_t>(expect[i]))
            allOk = false;
    }
    check(allOk, "3 个包的顺序 / 大小 / 内容全部正确（证明 buffer 转移成功）");
    check(q.nbPackets() == 0, "取完后队列为空");
    check(q.duration() == 0, "duration 统计也归零（没有漏减）");
}

// ===========================================================================
// [2] serial 播放序列 —— seek 正确性的命根子
// ===========================================================================
static void testSerial()
{
    section("[2] serial 播放序列语义（seek 正确性的关键）");

    PacketQueue q;
    q.start();
    check(q.serial() == 1, "start() 后 serial = 1",
          "实际 " + std::to_string(q.serial()));

    AVPacketPtr pkt = make_packet();
    int  serial  = -1;
    bool isFlush = false;

    q.get(pkt.get(), true, &serial, &isFlush);     // 先吃掉 start 的 flush 标记

    for (int i = 0; i < 3; ++i) {
        auto p = makeFilledPacket(16, 1);
        q.put(p.get());
    }

    q.get(pkt.get(), true, &serial, &isFlush);
    check(serial == 1 && !isFlush, "普通包继承当前 serial(=1)，不自增",
          "实际 serial=" + std::to_string(serial));

    q.get(pkt.get(), true, &serial, &isFlush);
    check(serial == 1, "第 2 个普通包依然是 serial=1（老工程的 bug 就在这里）");

    std::printf("  --- 模拟一次 seek ---\n");
    q.reset();                                     // 清空 + 压入新 flush 标记
    check(q.nbPackets() == 1, "reset() 丢掉了队列里剩余的旧包");
    check(q.serial() == 2, "reset() 让 serial 变成 2",
          "实际 " + std::to_string(q.serial()));

    q.get(pkt.get(), true, &serial, &isFlush);
    check(isFlush && serial == 2, "reset() 后第一条是 serial=2 的 flush 标记");

    auto p = makeFilledPacket(16, 2);
    q.put(p.get());
    q.get(pkt.get(), true, &serial, &isFlush);
    check(serial == 2, "seek 之后的新包带 serial=2 —— 与旧数据可区分",
          "实际 serial=" + std::to_string(serial));
}

// ===========================================================================
// [3] 背压 —— 队列满时把压力反向传导给生产者
// ===========================================================================
static void testBackpressure()
{
    section("[3] 背压：生产者被内存上限挡住");

    constexpr int kTotal = 40000;
    constexpr int kSize  = 1024;    // 1024B * 8000 = 8MB，会先撞上"包数上限"

    PacketQueue q;
    q.start();

    std::atomic<int>  produced{0};
    std::atomic<bool> producerDone{false};

    std::thread producer([&] {
        for (int i = 0; i < kTotal; ++i) {
            auto p = makeFilledPacket(kSize, 0);
            if (q.put(p.get()) < 0)      // 被 abort 会返回 -1
                break;
            ++produced;
        }
        producerDone = true;
    });

    msleep(300);
    const int depth = q.nbPackets();
    check(!producerDone.load(), "300ms 后生产者仍在阻塞 —— 背压生效");
    check(depth >= 7000, "队列深度已逼近上限 8000",
          "实际 depth=" + std::to_string(depth));
    std::printf("        队列深度 = %d 个包，约 %.1f MB（上限 16MB / 8000 包）\n",
                depth, depth * kSize / 1024.0 / 1024.0);
    std::printf("        已生产 %d / %d 个包 —— 生产者被卡在中途\n",
                produced.load(), kTotal);

    // 开始消费，生产者应能继续推进
    std::atomic<int> consumed{0};
    std::thread consumer([&] {
        AVPacketPtr pkt = make_packet();
        bool isFlush = false;
        while (consumed.load() < kTotal) {
            if (q.get(pkt.get(), true, nullptr, &isFlush) < 0)
                break;
            if (!isFlush)
                ++consumed;
        }
    });

    producer.join();
    consumer.join();

    check(produced.load() == kTotal, "消费者开工后，生产者跑完全部",
          "实际 " + std::to_string(produced.load()));
    check(consumed.load() == kTotal, "消费者一个不漏地收到全部包",
          "实际 " + std::to_string(consumed.load()));
    check(q.nbPackets() == 0, "结束时队列已空");
}

// ===========================================================================
// [4] abort 唤醒阻塞中的消费者
// ===========================================================================
static void testAbortWakesConsumer()
{
    section("[4] abort() 能叫醒阻塞中的消费者");

    PacketQueue q;
    q.start();

    AVPacketPtr pkt = make_packet();
    bool isFlush = false;
    q.get(pkt.get(), true, nullptr, &isFlush);   // 吃掉 flush，队列变空

    std::atomic<bool> returned{false};
    std::atomic<int>  retVal{123};

    std::thread t([&] {
        AVPacketPtr p = make_packet();
        retVal = q.get(p.get(), true);           // 队列空且阻塞 -> 睡下
        returned = true;
    });

    msleep(200);
    check(!returned.load(), "消费者确实阻塞在 get() 上（不是立刻返回）");

    q.abort();
    t.join();                                    // 若唤醒失败，这里会永久卡住
    check(returned.load(), "abort() 之后消费者立刻返回");
    check(retVal.load() == -1, "get() 返回 -1，表示队列已终止",
          "实际 " + std::to_string(retVal.load()));
}

// ===========================================================================
// [5] FrameQueue 环形缓冲 —— 索引绕圈与槽位复用
// ===========================================================================
static void testFrameQueueRing()
{
    section("[5] FrameQueue 环形缓冲：索引绕圈 + 槽位复用");

    PacketQueue pktq;
    pktq.start();
    FrameQueue fq(3);                 // 模拟视频帧队列（真实工程里就是 3）
    fq.setPacketQueue(&pktq);

    check(fq.nbRemaining() == 0, "初始 0 帧");

    // 写 2 帧
    std::vector<Frame*> slots;
    for (int i = 0; i < 2; ++i) {
        Frame* s = fq.peekWritable();
        s->pts      = i * 0.04;
        s->duration = 0.04;
        s->width    = 1920;
        s->height   = 1080;
        slots.push_back(s);
        fq.push();
    }
    check(fq.nbRemaining() == 2, "写入 2 帧后 size=2");
    std::printf("        槽位地址: [0]=%p  [1]=%p\n",
                static_cast<void*>(slots[0]), static_cast<void*>(slots[1]));

    Frame* head = fq.peek();
    check(head == slots[0], "peek() 指向最先写入的槽位（rindex=0）");
    check(head->pts == 0.0, "槽位里的元信息可读，pts=0.0");

    fq.next();                        // 消费第一帧
    check(fq.nbRemaining() == 1, "next() 后 size=1");
    check(fq.peek() == slots[1], "rindex 前进到第 2 个槽位");
    check(fq.peek()->pts == 0.04, "此时读取到的是第 2 帧，pts=0.04");

    // 再写 2 帧，验证绕圈回到槽位 0
    fq.peekWritable();
    fq.push();
    Frame* wrapped = fq.peekWritable();
    fq.push();

    check(fq.nbRemaining() == 3, "此时队列已满 size=3",
          "实际 " + std::to_string(fq.nbRemaining()));
    check(wrapped == slots[0], "windex 绕回到槽位 0 —— 环形缓冲复用成功");
    std::printf("        绕回后写入地址 = %p（与最初 [0] 相同）\n",
                static_cast<void*>(wrapped));
}

// ===========================================================================
// [6] abort 其实是【两步】机制 —— 最容易漏的一步
// ===========================================================================
static void testAbortTwoStep()
{
    section("[6] FrameQueue 阻塞：揭示 abort 为什么是两步的");

    PacketQueue pktq;
    pktq.start();
    FrameQueue fq(2);                 // 故意只开 2 个槽，方便填满
    fq.setPacketQueue(&pktq);

    for (int i = 0; i < 2; ++i) {     // 填满
        fq.peekWritable()->pts = i;
        fq.push();
    }
    check(fq.nbRemaining() == 2, "队列已填满 2 个槽");

    std::atomic<bool> returned{false};
    std::atomic<bool> gotNull{false};

    std::thread t([&] {
        Frame* s = fq.peekWritable();     // 满 -> 阻塞
        returned = true;
        gotNull  = (s == nullptr);
    });

    msleep(200);
    check(!returned.load(), "生产者阻塞在 peekWritable() 上");

    // ---- 第一步：只置标志位 ----
    pktq.abort();
    msleep(200);
    check(!returned.load(),
          "只调 pktq.abort() 还不够 —— 线程仍在睡（这一步最容易漏！）");

    // ---- 第二步：叫醒它 ----
    fq.signal();
    t.join();
    check(returned.load(), "补上 fq.signal() 之后线程才被唤醒");
    check(gotNull.load(), "peekWritable() 返回 nullptr，通知解码线程该退出了");

    std::printf("\n        结论：Decoder::abort() 必须写成两行——\n");
    std::printf("          queue_->abort();      // 置标志位\n");
    std::printf("          frameQueue.signal();  // 叫醒睡在帧队列上的线程\n");
    std::printf("        少任何一行，退出时就会永久卡死。\n");
}

// ===========================================================================
int main()
{
    SetConsoleOutputCP(CP_UTF8);

    std::printf("==================================================\n");
    std::printf(" M3 实验台：PacketQueue / FrameQueue（线程安全队列）\n");
    std::printf("==================================================\n");

    test_basicRoundTrip();
    testSerial();
    testBackpressure();
    testAbortWakesConsumer();
    testFrameQueueRing();
    testAbortTwoStep();

    std::printf("\n==================================================\n");
    std::printf(" 测试结束：通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf("==================================================\n");
    return g_fail == 0 ? 0 : 1;
}
