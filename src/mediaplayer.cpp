#include "mediaplayer.h"

#include <QDebug>

#include <algorithm>

extern "C" {
#include <libavutil/error.h>
}

MediaPlayer::MediaPlayer(QObject* parent)
    : QObject(parent)
{
    positionTimer_ = new QTimer(this);
    positionTimer_->setInterval(200);      // 200ms 上报一次位置：够平滑也不浪费
    connect(positionTimer_, &QTimer::timeout, this, &MediaPlayer::onPositionTick);
}

MediaPlayer::~MediaPlayer()
{
    stop();
}

void MediaPlayer::setDataSource(const QString& url)
{
    source_ = url;
}

void MediaPlayer::setState(PlaybackState s)
{
    if (state_ == s)
        return;                            // 状态没变就别发信号，减少 UI 无谓刷新
    state_ = s;
    emit stateChanged(s);
}

// ---------------------------------------------------------------------------
// ★ teardown —— 停掉当前实例
// ---------------------------------------------------------------------------
// 把"关闭内核 + join 事件线程 + 销毁实例"这三步绑在一起，顺序不能乱：
//
//   ① player_->close()          停掉内核的全部线程（含刷新线程，它是
//                               唯一会调 onKernelFrame 的地方）
//   ② eventThread_.join()       等事件线程退出
//   ③ player_.reset()           这时才销毁 FFPlayer
//
// ★★ 为什么必须有这个函数（而不是像参考工程那样在 play() 里直接
//    给 player_ 重新赋值）：
//
//   参考工程的 play() 是这样写的：
//       player_ = std::make_unique<FFPlayer>();   // ← 旧的 FFPlayer 在这里被析构
//       ...
//       if (eventThread_.joinable()) eventThread_.join();   // ← 之后才 join
//
//   问题在于：旧的 eventThread_ 此刻正阻塞在
//       player_->messages().get(msg, true)
//   上。而 `player_ = make_unique(...)` 会先析构旧对象 ——
//   MessageQueue 的 mutex / condition_variable 跟着被销毁，
//   而事件线程还在那个 condition_variable 上睡着。
//
//   析构里的 msgQ_.abort() 会 notify 它，但"被唤醒后重新获取锁"与
//   "锁被销毁"之间是一个真实的竞态窗口，属于未定义行为。
//   更糟的是 eventLoop 的循环条件 `while (player_)` 也在读那个
//   正被改写的 unique_ptr —— 而它并不是原子的。
//
//   复现路径很短：**播完之后（状态 Completed）再点一次播放**。
//   这时 player_ 非空、事件线程还活着，play() 就会踩进这个窗口。
//
//   修法就是"先彻底停干净，再建新的"。
void MediaPlayer::teardown()
{
    if (player_) {
        player_->close();                  // ① 停内核（内部会 join 它的 4 条线程）
        if (eventThread_.joinable())
            eventThread_.join();           // ② 等事件线程退出
        player_.reset();                   // ③ 现在销毁才安全
    }
    positionTimer_->stop();
}

// ---------------------------------------------------------------------------
// play
// ---------------------------------------------------------------------------
void MediaPlayer::play()
{
    if (state_ == PlaybackState::Playing || state_ == PlaybackState::Preparing)
        return;
    if (state_ == PlaybackState::Paused) {
        resume();
        return;
    }
    if (source_.isEmpty()) {
        emit errorOccurred(tr("尚未选择要播放的文件"));
        return;
    }

    // ★ 先彻底停掉上一次（见 teardown 的说明）
    teardown();

    player_ = std::make_unique<FFPlayer>();
    // 把内核的帧回调接到本类的转换函数上。
    // 注意这个 lambda 会在【刷新线程】里被调用。
    player_->setFrameCallback([this](const Frame& f) { onKernelFrame(f); });
    player_->setVolume(volume_ / 100.0f);
    player_->setSpeed(speed_);
    player_->prepare(source_.toUtf8().constData());

    setState(PlaybackState::Preparing);

    // 起事件线程。★ 这里不需要再 join 旧线程了 —— teardown() 已经做过。
    eventThread_ = std::thread(&MediaPlayer::eventLoop, this);
}

void MediaPlayer::togglePlayPause()
{
    switch (state_) {
    case PlaybackState::Playing:  pause(); break;
    case PlaybackState::Paused:   resume(); break;
    default:                      play(); break;
    }
}

void MediaPlayer::pause()
{
    if (player_ && state_ == PlaybackState::Playing) {
        player_->setPaused(true);
        setState(PlaybackState::Paused);
    }
}

void MediaPlayer::resume()
{
    if (player_ && state_ == PlaybackState::Paused) {
        player_->setPaused(false);
        setState(PlaybackState::Playing);
    }
}

void MediaPlayer::stop()
{
    teardown();
    setState(PlaybackState::Idle);
}

void MediaPlayer::seek(qint64 ms)
{
    if (player_)
        player_->seekMs(static_cast<int64_t>(ms));
}

void MediaPlayer::setVolume(int percent0to100)
{
    volume_ = std::clamp(percent0to100, 0, 100);
    if (player_)
        player_->setVolume(volume_ / 100.0f);
}

void MediaPlayer::setSpeed(float ratio)
{
    speed_ = ratio;
    if (player_)
        player_->setSpeed(ratio);
}

// ---------------------------------------------------------------------------
// onPositionTick —— GUI 线程，200ms 一次
// ---------------------------------------------------------------------------
// 读位置本身是安全的：Clock 内部有锁（M5 加的），而视频线程也在读它。
// ⚠ 但【不要】在这里做耗时操作 —— 它跑在 GUI 线程，卡住就是界面卡住。
void MediaPlayer::onPositionTick()
{
    if (!player_)
        return;
    emit positionChanged(static_cast<qint64>(player_->positionSeconds() * 1000.0));
    const int64_t total = player_->durationMs();
    if (total >= 0)
        emit durationChanged(static_cast<qint64>(total));
}

// ---------------------------------------------------------------------------
// onKernelFrame —— ★ 运行在【内核的视频刷新线程】里
// ---------------------------------------------------------------------------
// 它做的是"把内核的 AVFrame 变成 Qt 的 QImage"，然后 emit 出去。
//
// 【为什么转换放在这里，而不是放到 GUI 线程去做？】
//   YUV -> RGB 是逐像素的 CPU 活（640x480 就是 92 万个像素）。
//   放在 GUI 线程做，等于每帧都占用界面线程几毫秒 —— 界面会顿。
//   放在刷新线程做，界面线程只需处理"贴一张已经好的图"，几乎不花时间。
//   这就是"把计算留在后台线程、把呈现留给界面线程"的分工。
void MediaPlayer::onKernelFrame(const Frame& frame)
{
    int w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lock(convertMtx_);   // 见头文件里的说明
        if (scaler_.toRgb24(frame.frame.get(), rgbCache_, w, h) < 0)
            return;

        // ⚠ QImage 的这个构造函数【不拷贝数据】—— 它只是包装了 rgbCache_ 的指针。
        QImage image(rgbCache_.data(), w, h, w * 3, QImage::Format_RGB888);

        // ★★ .copy() 是必须的，两个理由：
        //   ① rgbCache_ 是【复用缓冲】—— 下一帧的转换会把它整块覆盖掉。
        //      不拷贝的话，接收方拿到的 QImage 会随着下一帧的到来而"变色"。
        //   ② 这个信号会跨线程投递（我在刷新线程 emit，槽在 GUI 线程执行）。
        //      不拷贝就等于两个线程同时读写同一块内存 —— 数据竞争。
        //   copy() 会把像素数据复制一份，让 QImage 自己持有，随信号一起传递。
        emit frameReady(image.copy());
    }
}

// ---------------------------------------------------------------------------
// eventLoop —— ★ 运行在独立的 std::thread 里
// ---------------------------------------------------------------------------
// 它阻塞等待内核投递的事件，然后【切回 GUI 线程】去处理。
void MediaPlayer::eventLoop()
{
    Message msg;
    while (player_) {                    // ← 循环条件：见 teardown() 的说明
        const int ret = player_->messages().get(msg, true);   // 阻塞取
        if (ret < 0)        // 消息队列被 abort（也就是 close 了）
            break;

        switch (msg.what) {
        case FFMsg::Prepared:
            // ★ 这里必须切回 GUI 线程，原因有两个：
            //   ① setState() 会改 state_ 成员，而它同时被 GUI 线程读
            //      （isPlaying() / state()）；
            //   ② positionTimer_->start() 操作 QTimer —— QTimer 属于
            //      GUI 线程，只能从它自己的线程启动。
            //
            //   顺带说明：【信号】本身其实不必包在这里 —— Qt 的自动连接
            //   会根据"接收者所在线程"决定是否排队，跨线程 emit 本来就是安全的。
            //   包进来是为了让"改状态 + 起定时器 + 发信号"这三件事原子地
            //   发生在 GUI 线程上，顺序也确定。
            QMetaObject::invokeMethod(this, [this] {
                setState(PlaybackState::Playing);
                emit prepared();
                positionTimer_->start();
                if (player_) {
                    const int64_t total = player_->durationMs();
                    if (total >= 0)
                        emit durationChanged(static_cast<qint64>(total));
                }
            }, Qt::QueuedConnection);
            break;

        case FFMsg::Completed:
            // ★ 注意：M10 修正后，这个事件表示【真正播完】，而不是
            //   "文件读完"。所以这里停掉定时器、切到 Completed 状态是准确的。
            QMetaObject::invokeMethod(this, [this] {
                setState(PlaybackState::Completed);
                positionTimer_->stop();
                emit completed();
            }, Qt::QueuedConnection);
            break;

        case FFMsg::Error: {
            // 内核只给错误码，转成可读文字是这一层的活
            char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(msg.arg1, buf, sizeof(buf));
            const QString text = QString::fromUtf8(buf);
            QMetaObject::invokeMethod(this, [this, text] {
                setState(PlaybackState::Error);
                emit errorOccurred(text);
            }, Qt::QueuedConnection);
            break;
        }

        default:
            // OpenInput / FindStreamInfo / ComponentOpen 等"日志型事件"
            // 当前界面用不到，忽略。将来要做"正在解析…"的提示，在这里加分支即可。
            break;
        }
    }
}
