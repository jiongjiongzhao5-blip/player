#ifndef MEDIAPLAYER_H
#define MEDIAPLAYER_H

#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "ffplayer.h"
#include "imagescaler.h"

// ============================================================================
// mediaplayer.h —— 面向 Qt UI 的播放器门面
//
// 【这一层存在的唯一理由：翻译】
//   内核（FFPlayer）的世界：没有 Qt、只有一个 std::function 帧回调和一个
//     异步消息队列（post 进去就不管了，没有返回值）。
//   Qt 程序员的世界：信号槽 —— 声明式、跨线程安全、可多播。
//   两者的语义模型完全不同，中间必须有个翻译官。
//   ★ 这一层【不含任何播放逻辑】：不决定什么时候显示哪一帧、不算同步、
//     不碰 FFmpeg。它只做三件事：
//       ① 把内核的消息翻译成 Qt 信号
//       ② 把内核丢出来的 AVFrame 翻译成 QImage
//       ③ 把 Qt 侧的调用转发成内核的方法调用
//
// 【三条线程，和它们各自的规矩】
//
//   线程 A：GUI 线程（Qt 主线程）
//     · 所有 public 槽（play/pause/seek…）都在这里被调用
//     · positionTimer_ 的 timeout 也在这里
//     · 只有这里能碰 Qt 控件
//
//   线程 B：事件线程（本类自己的 std::thread 跑 eventLoop）
//     · 阻塞在 FFPlayer 的 MessageQueue 上等事件
//     · 收到事件后【必须切回 GUI 线程】才能改状态、动 QTimer
//       —— 用 QMetaObject::invokeMethod(..., Qt::QueuedConnection)
//
//   线程 C：内核的视频刷新线程
//     · 通过 videoCb_ 调进 onKernelFrame()
//     · 在这里做 AVFrame -> QImage 的转换（把 CPU 活留在非 GUI 线程）
//     · 转换完 emit frameReady —— 这是【跨线程信号】，
//       Qt 会自动走 QueuedConnection，槽在 GUI 线程执行
//
// 【相对参考工程的改造】
//   · FFPlayer/ImageScaler 用 unique_ptr 管理；
//   · 加了 teardown() 统一"停旧实例"的流程（修掉一处线程生命周期竞态，
//     详见 .cpp 里 play() 的说明）；
//   · 状态迁移集中在 setState() 一处，避免漏发 stateChanged。
// ============================================================================

enum class PlaybackState {
    Idle,          // 未加载
    Preparing,     // 已调用 prepare，等内核的 Prepared 事件
    Playing,
    Paused,
    Completed,     // 播放到末尾（注意：是"播完"，不是"读完"——见 M10）
    Error
};

class MediaPlayer : public QObject
{
    Q_OBJECT
public:
    explicit MediaPlayer(QObject* parent = nullptr);
    ~MediaPlayer() override;

    void setDataSource(const QString& url);
    QString dataSource() const { return source_; }

    bool isPlaying() const { return state_ == PlaybackState::Playing; }
    PlaybackState state() const { return state_; }

public slots:
    // 统一入口：空闲/完成态就"准备并播放"，暂停态就恢复。
    void play();
    void togglePlayPause();
    void pause();
    void resume();
    void stop();
    void seek(qint64 ms);

    // 音量用 0~100 的整数 —— 界面滑块天然就是这个范围。
    // 换算成 AudioDevice 要的 0.0~1.0 在这一层做，界面不需要知道。
    void setVolume(int percent0to100);
    void setSpeed(float ratio);

signals:
    void prepared();
    void completed();
    void errorOccurred(const QString& message);
    void frameReady(const QImage& frame);      // 跨线程：在 GUI 线程被投递
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void stateChanged(PlaybackState state);

private slots:
    void onPositionTick();                     // GUI 线程：200ms 定时上报位置

private:
    void eventLoop();                          // 事件线程主体
    void onKernelFrame(const Frame& frame);    // 刷新线程：AVFrame -> QImage
    void setState(PlaybackState s);
    void teardown();                           // 停掉当前实例（含 join 事件线程）

    std::unique_ptr<FFPlayer> player_;
    std::thread eventThread_;                  // 事件线程
    ImageScaler scaler_;
    // 保护 scaler_ / rgbCache_。
    // ⚠ 说实话：当前 onKernelFrame 只被【刷新线程】一条线程调用，
    //   所以这把锁并不解决任何实际问题，属于防御性冗余。
    //   保留它的理由是"万一将来多一个帧消费方"；但你要知道它现在不起作用，
    //   别误以为"加了锁所以这里一定线程安全"。
    std::mutex  convertMtx_;
    std::vector<uint8_t> rgbCache_;            // RGB24 缓冲，逐帧复用

    QString source_;
    PlaybackState state_ = PlaybackState::Idle;
    QTimer* positionTimer_ = nullptr;
    int   volume_ = 100;
    float speed_  = 1.0f;
};

#endif // MEDIAPLAYER_H
