// ============================================================================
// tests/test_mediaplayer.cpp —— M12 实验台：MediaPlayer 门面层
//
// ★ 这是一个 Qt 控制台程序（QCoreApplication，不需要 QApplication）：
//   因为要验证信号槽、事件循环、跨线程投递，必须真的跑 Qt 的事件循环。
//
// 7 组测试：
//   [1] 信号序列与状态迁移
//   [2] ★ frameReady：QImage 数量 / 尺寸 / 内容非空
//   [3] ★★ 线程归属：所有信号都在 GUI 线程被投递（跨线程投递的正确性）
//   [4] positionChanged / durationChanged
//   [5] ★★ 重复播放：测 teardown() 修掉的那处线程生命周期竞态
//   [6] 错误路径
//   [7] stop 回到 Idle
//
// ⚠ 会播放音频（音量 20%）。
// 用法：test_mediaplayer.exe [媒体文件路径]
// ============================================================================

#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QThread>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "mediaplayer.h"

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

// 边跑 Qt 事件循环边等条件成立。超时返回 false。
static bool pumpUntil(const std::function<bool()>& pred, int timeoutMs)
{
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (pred())
            return true;
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        if (el > timeoutMs)
            return false;
        msleep(3);
    }
}

// ---------------------------------------------------------------------------
// 信号采集器
// ---------------------------------------------------------------------------
struct Recorder {
    QThread* guiThread = nullptr;

    std::atomic<int> wrongThreadSeen{0};    // 在非 GUI 线程被投递的次数
    std::atomic<int> frameCount{0};
    std::atomic<int> posCount{0};
    std::atomic<int> durCount{0};
    std::atomic<bool> gotPrepared{false};
    std::atomic<bool> gotCompleted{false};
    std::atomic<bool> gotError{false};

    // 这些只在 GUI 线程里被写（槽都投递到 GUI 线程），所以不用加锁
    std::vector<PlaybackState> states;
    std::vector<QString> errors;
    int  badSizeFrames = 0;
    int  blankFrames   = 0;
    int  firstW = 0, firstH = 0;
    qint64 lastDuration = -1;
    qint64 lastPosition = -1;

    void noteThread() {
        if (QThread::currentThread() != guiThread)
            wrongThreadSeen.fetch_add(1);
    }
};

// ===========================================================================
int main(int argc, char* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    QCoreApplication app(argc, argv);

    const std::string path = (argc > 1) ? argv[1] : "../_media/test_media.mp4";

    std::printf("==================================================\n");
    std::printf(" M12 实验台：MediaPlayer 门面层（Qt 世界的第一层）\n");
    std::printf(" 素材: %s\n", path.c_str());
    std::printf("==================================================\n");

    Recorder rec;
    rec.guiThread = QThread::currentThread();
    std::printf("  GUI 线程 = %p\n", static_cast<void*>(rec.guiThread));

    MediaPlayer mp;
    mp.setVolume(20);

    // ★★ 这里有个 Qt 的经典坑，值得单独说：
    //
    //   QObject::connect(sender, signal, lambda)          ← 三参数，【没有接收者上下文】
    //   QObject::connect(sender, signal, context, lambda) ← 四参数，有上下文
    //
    //   三参数版本 Qt 无从判断"槽应该在哪个线程执行"，
    //   于是连接的默认类型是 DirectConnection ——
    //   **槽会在【发送信号的线程】里同步执行**。
    //   对我们来说，frameReady 是在内核的刷新线程里 emit 的，
    //   用三参数接就会让槽跑在刷新线程里 —— 一旦槽里碰了 UI 就是灾难。
    //   （我第一版就是这么写的，测试里 61 帧全部在非 GUI 线程被执行。）
    //
    //   四参数版本用 context 对象的线程来决定连接类型：
    //   context 活在 GUI 线程 + 发送发生在别的线程 → 自动变成 QueuedConnection。
    //
    //   所以下面统一用一个"活在 GUI 线程的空 QObject"当 context。
    //   真实工程里的写法是直接传接收控件：
    //       connect(mp_, &MediaPlayer::frameReady, display_, &DisplayWind::presentFrame)
    //   display_ 在 GUI 线程 → 自动排队 → 安全。
    QObject guiContext;
    std::printf("  GUI 线程 = %p（guiContext 也活在这个线程）\n",
                static_cast<void*>(rec.guiThread));

    // ---- 接上所有信号（都带 context，保证槽在 GUI 线程执行）----
    QObject::connect(&mp, &MediaPlayer::stateChanged, &guiContext, [&rec](PlaybackState s) {
        rec.noteThread();
        rec.states.push_back(s);
    });
    QObject::connect(&mp, &MediaPlayer::prepared, &guiContext, [&rec] {
        rec.noteThread();
        rec.gotPrepared = true;
    });
    QObject::connect(&mp, &MediaPlayer::completed, &guiContext, [&rec] {
        rec.noteThread();
        rec.gotCompleted = true;
    });
    QObject::connect(&mp, &MediaPlayer::errorOccurred, &guiContext,
                     [&rec](const QString& e) {
        rec.noteThread();
        rec.gotError = true;
        rec.errors.push_back(e);
    });
    QObject::connect(&mp, &MediaPlayer::frameReady, &guiContext,
                     [&rec](const QImage& img) {
        // ★ 这一路的 emit 发生在【内核的视频刷新线程】，
        //   能走到这里说明 Qt 帮我们排队切到 GUI 线程了。
        rec.noteThread();
        rec.frameCount.fetch_add(1);

        if (img.isNull() || img.width() <= 0 || img.height() <= 0) {
            ++rec.badSizeFrames;
            return;
        }
        if (rec.firstW == 0) { rec.firstW = img.width(); rec.firstH = img.height(); }
        if (img.width() != 640 || img.height() != 480)
            ++rec.badSizeFrames;

        // 抽样看内容：素材是蓝色调 + 一个亮方块，不该是纯黑
        int bright = 0;
        for (int y = 0; y < img.height(); y += 20)
            for (int x = 0; x < img.width(); x += 20) {
                const QRgb p = img.pixel(x, y);
                if (qRed(p) + qGreen(p) + qBlue(p) > 40) ++bright;
            }
        if (bright == 0) ++rec.blankFrames;
    });
    QObject::connect(&mp, &MediaPlayer::positionChanged, &guiContext, [&rec](qint64 ms) {
        rec.noteThread();
        rec.posCount.fetch_add(1);
        rec.lastPosition = ms;
    });
    QObject::connect(&mp, &MediaPlayer::durationChanged, &guiContext, [&rec](qint64 ms) {
        rec.noteThread();
        rec.durCount.fetch_add(1);
        rec.lastDuration = ms;
    });

    // =======================================================================
    section("[1] 信号序列与状态迁移");
    // =======================================================================
    {
        mp.setDataSource(QString::fromUtf8(path.c_str()));
        mp.play();

        const bool prepared = pumpUntil([&rec] { return rec.gotPrepared.load(); }, 6000);
        check(prepared, "收到 prepared 信号");
        check(rec.states.size() >= 2
              && rec.states[0] == PlaybackState::Preparing
              && rec.states[1] == PlaybackState::Playing,
              "★ 状态迁移序列正确：Preparing -> Playing");
        std::printf("        stateChanged 序列前 3 个: ");
        for (size_t i = 0; i < rec.states.size() && i < 3; ++i)
            std::printf("%d ", static_cast<int>(rec.states[i]));
        std::printf("\n");
    }

    // =======================================================================
    section("[2] ★ frameReady：QImage 的数量 / 尺寸 / 内容");
    // =======================================================================
    {
        // 让它播一会儿
        pumpUntil([&rec] { return rec.frameCount.load() > 60; }, 4000);

        std::printf("        已收到 %d 帧 QImage（首帧 %dx%d）\n",
                    rec.frameCount.load(), rec.firstW, rec.firstH);
        check(rec.frameCount.load() > 40, "★ 持续收到视频帧");
        check(rec.badSizeFrames == 0, "★ 每帧尺寸都是 640x480（转换结果正确）");
        check(rec.blankFrames == 0,
              "★ 没有全黑的帧 —— 说明 RGB 数据确实被拷进 QImage 了（不是空指针包装）");
    }

    // =======================================================================
    section("[3] ★★ 线程归属：信号必须在 GUI 线程被投递");
    // =======================================================================
    {
        std::printf("        在非 GUI 线程收到信号的次数: %d\n", rec.wrongThreadSeen.load());
        check(rec.wrongThreadSeen.load() == 0,
              "★★ 所有信号的槽都在 GUI 线程执行 —— 跨线程投递机制生效");
        std::printf("        （frameReady 是在内核刷新线程 emit 的，\n");
        std::printf("          stateChanged 是在事件线程里 invokeMethod 出来的，\n");
        std::printf("          两者如果没有正确切线程，这里就会 > 0）\n");
    }

    // =======================================================================
    section("[4] 位置与时长上报");
    // =======================================================================
    {
        std::printf("        位置信号 %d 次（最后一次 %lld ms），时长信号 %d 次（%lld ms）\n",
                    rec.posCount.load(), (long long)rec.lastPosition,
                    rec.durCount.load(), (long long)rec.lastDuration);
        check(rec.posCount.load() > 2, "周期性收到 positionChanged（200ms 定时器在工作）");
        check(rec.lastDuration > 4000 && rec.lastDuration < 6000,
              "★ durationChanged 上报的时长约 5 秒");
        check(rec.lastPosition > 0, "位置在推进");
    }

    // =======================================================================
    section("[5] ★★ 重复播放（测 teardown 修掉的那处竞态）");
    // =======================================================================
    // 参考工程的 play() 会先给 player_ 重新赋值（析构旧实例），
    // 之后才 join 事件线程 —— 而事件线程那时正睡在旧实例的
    // condition_variable 上。这是个真实的竞态窗口。
    // 复现路径很短：**播完（Completed）之后再点一次播放**。
    // 这里就把这条路径跑 12 遍。
    {
        // ★ 先让当前这次播放跑到 Completed —— 因为 play() 在 Playing 状态下
        //   会直接返回（它的语义是"开始播放"，不是"重新开始"）。
        //   我们要构造的正是"从 Completed 状态再次 play"这条路径。
        mp.seek(4600);
        pumpUntil([&rec] { return rec.gotCompleted.load(); }, 5000);
        rec.gotCompleted = false;

        int ok = 0;
        for (int i = 0; i < 12; ++i) {
            rec.gotPrepared  = false;
            rec.gotCompleted = false;

            mp.play();                                   // ← 此时 state 是 Completed
            if (!pumpUntil([&rec] { return rec.gotPrepared.load(); }, 4000))
                break;

            mp.seek(4600);                               // 跳到接近末尾，快点放完
            if (!pumpUntil([&rec] { return rec.gotCompleted.load(); }, 5000))
                break;

            ++ok;
            if ((i + 1) % 4 == 0 || i == 0)
                std::printf("          第 %2d 轮完成（帧总数 %d）\n", i + 1, rec.frameCount.load());
        }
        check(ok == 12, "★★ 连续 12 轮 play->完成 全部成功，没有崩溃或挂死");
        std::printf("        （参考工程在这条路径上有 use-after-free 风险：\n");
        std::printf("           play() 先析构旧 FFPlayer，后 join 事件线程）\n");
    }

    // =======================================================================
    section("[6] 错误路径");
    // =======================================================================
    {
        mp.stop();
        rec.gotError = false;
        rec.errors.clear();
        mp.setDataSource(QStringLiteral("no_such_file_12345.mp4"));
        mp.play();

        const bool gotErr = pumpUntil([&rec] { return rec.gotError.load(); }, 4000);
        check(gotErr, "★ 打开不存在的文件会发出 errorOccurred 信号");
        if (!rec.errors.empty())
            std::printf("        错误内容: %s\n", rec.errors[0].toUtf8().constData());
        check(mp.state() == PlaybackState::Error, "状态被置为 Error");
    }

    // =======================================================================
    section("[7] stop 回到 Idle");
    // =======================================================================
    {
        mp.stop();
        check(mp.state() == PlaybackState::Idle, "stop 之后状态是 Idle");
        check(!mp.isPlaying(), "isPlaying() 为 false");

        // stop 之后再来一次正常播放，确认实例还能用
        mp.setDataSource(QString::fromUtf8(path.c_str()));
        rec.gotPrepared = false;
        mp.play();
        check(pumpUntil([&rec] { return rec.gotPrepared.load(); }, 5000),
              "stop 之后还能重新播放");
        mp.stop();
        check(true, "再次 stop 安全");
    }

    std::printf("\n==================================================\n");
    std::printf(" 测试结束：通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf("==================================================\n");
    return g_fail == 0 ? 0 : 1;
}
