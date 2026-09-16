// ============================================================================
// tests/test_message_queue.cpp —— M4 实验台：验证 ffmsg.h + MessageQueue
//
// 6 组测试：
//   [1] 基本投递/取出 + 三态返回值
//   [2] start() 之前 post() 被静默丢弃（易错点）
//   [3] enum class 的数值分段设计
//   [4] remove()：撤销还没被处理的事件（消息队列相对回调的核心优势）
//   [5] abort() 唤醒阻塞的 get()
//   [6] 场景模拟：内核 readThread → UI 事件线程
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include <windows.h>

#include "message_queue.h"

// ---------------------------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* desc)
{
    if (cond) { ++g_pass; std::printf("  [PASS] %s\n", desc); }
    else      { ++g_fail; std::printf("  [FAIL] %s\n", desc); }
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

// ===========================================================================
// [1] 基本投递 / 取出
// ===========================================================================
static void testBasic()
{
    section("[1] 基本投递/取出 + 三态返回值");

    MessageQueue q;
    q.start();                                     // 队列默认是终止态，必须先 start

    Message m;
    int r = q.get(m, true);
    check(r == 1 && m.what == FFMsg::FLUSH, "start() 后第一条是 FLUSH 哨兵");

    q.post(FFMsg::Error, -13);                     // 带一个错误码
    q.post(FFMsg::Prepared);                       // 不带参数
    q.post(FFMsg::VideoSizeChanged, 1920, 1080);   // 带两个参数

    r = q.get(m, true);
    check(r == 1 && m.what == FFMsg::Error && m.arg1 == -13,
          "Error 消息把错误码带在 arg1 = -13");

    r = q.get(m, true);
    check(r == 1 && m.what == FFMsg::Prepared && m.arg1 == 0,
          "Prepared 无参数时 arg1 默认 0");

    r = q.get(m, true);
    check(r == 1 && m.what == FFMsg::VideoSizeChanged
          && m.arg1 == 1920 && m.arg2 == 1080,
          "两个参数正确传递（1920 x 1080）");

    check(q.get(m, false) == 0, "非阻塞 get 在空队列上返回 0（不是 -1）");
}

// ===========================================================================
// [2] start() 之前 post() 会被静默丢弃 —— 最容易忽略的行为
// ===========================================================================
static void testPostBeforeStart()
{
    section("[2] start() 之前 post() 会被静默丢弃");

    MessageQueue q;                    // 默认 abort_request_ = true（终止态）
    q.post(FFMsg::Prepared);           // 不报错、不抛异常，直接丢
    q.post(FFMsg::Error, -13);

    Message m;
    check(q.get(m, false) == -1, "队列处于终止态时 get() 返回 -1");

    q.start();                         // 清空 + 压 FLUSH，队列"活了"
    int r = q.get(m, false);
    check(r == 1 && m.what == FFMsg::FLUSH, "start() 后队列可用，第一条是 FLUSH");

    r = q.get(m, false);
    check(r == 0, "队列里再无其他消息 —— 之前那两条 post 确实被丢弃了");

    std::printf("        结论：post() 是'尽力而为'的通知，不返回错误。\n");
    std::printf("        所以「忘了 start」的表现是「界面永远没反应」，而不是崩溃。\n");
}

// ===========================================================================
// [3] enum class 的数值分段设计
// ===========================================================================
static void testEnumLayout()
{
    section("[3] enum class 的数值分段设计");

    struct Item { FFMsg m; const char* name; };
    const Item items[] = {
        {FFMsg::FLUSH,            "FLUSH"},
        {FFMsg::Error,            "Error"},
        {FFMsg::Prepared,         "Prepared"},
        {FFMsg::Completed,        "Completed"},
        {FFMsg::VideoSizeChanged, "VideoSizeChanged"},
        {FFMsg::ComponentOpen,    "ComponentOpen"},
        {FFMsg::ReqStart,         "ReqStart"},
        {FFMsg::ReqPause,         "ReqPause"},
        {FFMsg::ReqSeek,          "ReqSeek"},
    };

    std::printf("        枚举值一览（注意编号是稀疏分段留空隙的）：\n");
    int eventCount = 0, requestCount = 0;
    for (const auto& it : items) {
        const int v = static_cast<int>(it.m);
        std::printf("          %-20s = %d\n", it.name, v);
        if (v < 20000) ++eventCount; else ++requestCount;
    }

    check(eventCount == 6 && requestCount == 3,
          "可按数值区间粗分类：<20000 = 事件类（内核→上层），>=20000 = 请求类（上层→内核）");
    check(static_cast<int>(FFMsg::Error) < static_cast<int>(FFMsg::Prepared),
          "同段内编号递增，方便阅读和二分定位");
    std::printf("        注意 static_cast<int> 是必须的 —— enum class 禁止隐式转换，\n");
    std::printf("        这正是它比 #define 安全的地方：想当整数用必须明说。\n");
}

// ===========================================================================
// [4] remove() —— 撤销还没被处理的事件
// ===========================================================================
static void testRemove()
{
    section("[4] remove()：撤销还没被处理的事件");

    MessageQueue q;
    q.start();

    Message m;
    q.get(m, false);                        // 吃掉 FLUSH 哨兵

    // 模拟用户 1 秒内连点 5 次"暂停"
    for (int i = 0; i < 5; ++i)
        q.post(FFMsg::ReqPause);
    q.post(FFMsg::Completed);               // 期间播放真的结束了

    q.remove(FFMsg::ReqPause);              // 一把清掉所有积压的暂停请求

    int  n = 0;
    bool onlyCompleted = true;
    while (q.get(m, false) == 1) {
        ++n;
        if (m.what != FFMsg::Completed)
            onlyCompleted = false;
    }

    check(n == 1, "5 条 ReqPause 全部被撤销，只剩 1 条消息");
    check(onlyCompleted, "剩下的那条是 Completed，没有被误删");

    std::printf("        这就是消息队列相对回调的核心优势：\n");
    std::printf("        回调在产生的瞬间就执行完了，无法撤销；\n");
    std::printf("        而排队中的消息可以随时清掉。\n");
}

// ===========================================================================
// [5] abort() 唤醒阻塞的 get()
// ===========================================================================
static void testAbortWakesGetter()
{
    section("[5] abort() 唤醒阻塞的 get()");

    MessageQueue q;
    q.start();

    Message m;
    q.get(m, false);                        // 吃掉 FLUSH，队列变空

    std::atomic<bool> returned{false};
    std::atomic<int>  retVal{999};

    std::thread t([&] {
        Message msg;
        retVal = q.get(msg, true);          // 空 + 阻塞 -> 睡下
        returned = true;
    });

    msleep(200);
    check(!returned.load(), "事件线程确实阻塞在 get() 上（不是立刻返回）");

    q.abort();
    t.join();                               // 唤醒失败这里就会永久卡住
    check(returned.load(), "abort() 之后立刻返回");
    check(retVal.load() == -1, "返回 -1，调用方据此退出事件循环");
}

// ===========================================================================
// [6] 场景模拟：内核 readThread → UI 事件线程
// ===========================================================================
static void testKernelToUiScenario()
{
    section("[6] 场景模拟：内核 readThread → UI 事件线程");

    MessageQueue q;
    q.start();

    // 内核侧：模拟读线程在解复用过程中陆续投递的事件
    std::thread kernel([&] {
        q.post(FFMsg::OpenInput);                 // 打开文件
        q.post(FFMsg::FindStreamInfo);            // 探测流信息
        q.post(FFMsg::ComponentOpen);             // 打开解码器
        q.post(FFMsg::Prepared);                  // 就绪 —— UI 关心
        q.post(FFMsg::VideoSizeChanged, 1920, 1080);
        q.post(FFMsg::Completed);                 // 播完 —— UI 关心
    });
    kernel.join();

    // UI 侧：只关心 Prepared / Completed / Error 三种
    Message m;
    int consumed = 0;
    int ignored  = 0;

    while (q.get(m, false) == 1) {
        ++consumed;
        switch (m.what) {
        case FFMsg::Prepared:
            std::printf("          [UI] Prepared  -> 开始播放，启动进度条\n");
            break;
        case FFMsg::Completed:
            std::printf("          [UI] Completed -> 播放结束，按钮复位\n");
            break;
        case FFMsg::Error:
            std::printf("          [UI] Error code=%d -> 弹错误提示\n", m.arg1);
            break;
        default:
            ++ignored;                            // 落到这里的事件本工程暂不消费
            break;
        }
    }

    check(consumed == 7, "共消费 7 条（6 条投递 + 1 条 FLUSH 哨兵）");
    check(ignored == 5,
          "其中 5 条落入 default 被忽略（FLUSH + OpenInput + FindStreamInfo + "
          "ComponentOpen + VideoSizeChanged）");
    std::printf("        说明：内核投递的事件【可以多于上层消费的】——\n");
    std::printf("        多投递的事件不影响功能，将来 UI 想增强体验（比如加个\n");
    std::printf("        '正在解析...' 的提示）时，直接加分支即可，内核不用动。\n");
}

// ===========================================================================
int main()
{
    SetConsoleOutputCP(CP_UTF8);

    std::printf("==================================================\n");
    std::printf(" M4 实验台：ffmsg.h + MessageQueue（内核消息机制）\n");
    std::printf("==================================================\n");

    testBasic();
    testPostBeforeStart();
    testEnumLayout();
    testRemove();
    testAbortWakesGetter();
    testKernelToUiScenario();

    std::printf("\n==================================================\n");
    std::printf(" 测试结束：通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf("==================================================\n");
    return g_fail == 0 ? 0 : 1;
}
