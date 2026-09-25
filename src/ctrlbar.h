#ifndef CTRLBAR_H
#define CTRLBAR_H

#include <QWidget>

class QPushButton;
class QSlider;
class QLabel;

// ============================================================================
// ctrlbar.h —— 底部播放控制条（原 ctrlbar.ui 改为纯代码构建）
//
// 【它是整个界面里"交互最复杂"的控件】，值得单独说三处设计：
//
// ① 进度条的"拖动中"状态
//    播放时 positionTimer 每 200ms 会把新位置推给进度条。如果用户正在拖，
//    这个推送会把滑块"拽回去"，手感极差。
//    所以用 sliderDragging_ 标志：拖动期间【不响应外部推送】，
//    松手时才发出 seekRequested。
//
// ② 倍速用"循环切换"而不是下拉框
//    倍速只有 4 个固定档位，用一个按钮循环点比下拉框少一次点击，
//    也少一个弹出控件。按钮文字直接显示当前倍速（"倍速1.5"）。
//
// ③ 时间显示自己做格式化
//    formatTime() 把毫秒转成 mm:ss 或 hh:mm:ss（超过一小时才显示小时段）——
//    两个 QLabel 固定宽度，避免数字跳动时布局抖动。
// ============================================================================

class CtrlBar : public QWidget
{
    Q_OBJECT
public:
    explicit CtrlBar(QWidget* parent = nullptr);

public slots:
    void setPositionMs(qint64 ms);     // 外部（定时器）推送播放位置
    void setDurationMs(qint64 ms);     // 总时长到了才能设置进度条范围
    void setPlaying(bool playing);     // 切换 播放/暂停 图标
    void reset();                      // 停止时清空

signals:
    void playOrPauseClicked();
    void stopClicked();
    void seekRequested(qint64 ms);          // 用户拖完进度条
    void seekOffsetRequested(int seconds);  // ±10 秒
    void volumeChanged(int percent);
    void speedChanged(float ratio);
    void playlistToggled();

private slots:
    void onSliderReleased();
    void cycleSpeed();

private:
    static QString formatTime(qint64 ms);

    QPushButton* playOrPauseBtn_ = nullptr;
    QPushButton* stopBtn_        = nullptr;
    QPushButton* backwardBtn_    = nullptr;
    QPushButton* forwardBtn_     = nullptr;
    QPushButton* speedBtn_       = nullptr;
    QPushButton* volumeBtn_      = nullptr;
    QPushButton* playListBtn_    = nullptr;
    QSlider*     playSlider_     = nullptr;
    QSlider*     volumeSlider_   = nullptr;
    QLabel*      currentLabel_   = nullptr;
    QLabel*      totalLabel_     = nullptr;

    qint64 durationMs_ = 0;
    bool   sliderDragging_ = false;     // 见头文件 ①
    int    speedIndex_ = 1;             // 默认停在 1.0x
    static constexpr float kSpeeds[] = {0.5f, 1.0f, 1.5f, 2.0f};
};

#endif // CTRLBAR_H
