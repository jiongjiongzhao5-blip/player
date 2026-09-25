#ifndef DISPLAYWIND_H
#define DISPLAYWIND_H

#include <QImage>
#include <QMutex>
#include <QWidget>

// ============================================================================
// displaywind.h —— 视频画面显示控件
//
// 【职责边界：它只负责"贴图"，不负责转换】
//   原工程在控件内部用 sws 缩放到控件尺寸、还自己 malloc RGB 缓存。
//   现在色彩转换已经前移到 MediaPlayer::onKernelFrame()（M12 讲的：
//   把 CPU 活留在内核线程），传给这里的是一张【已经可用的 QImage】。
//   本控件只剩下两件事：
//     ① 按宽高比居中绘制（留黑边）
//     ② 保证跨线程安全（写方在 GUI 线程，但读方 paintEvent 也在 GUI 线程…
//        其实同线程；这个 QMutex 是防御性的，说明见 .cpp）
//
// 【为什么缩放交给 QPainter 而不是自己算】
//   QImage::scaled(size, KeepAspectRatio, SmoothTransformation) 一句话就做完了：
//   它内部会自动算比例、选滤波、处理边界。这正好印证了 M7 的设计取舍 ——
//   ImageScaler 不做缩放，把这件事留给 Qt。
// ============================================================================

class DisplayWind : public QWidget
{
    Q_OBJECT
public:
    explicit DisplayWind(QWidget* parent = nullptr);

public slots:
    // GUI 线程槽：接收一帧并请求重绘
    void presentFrame(const QImage& image);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage image_;
    QMutex mutex_;      // 见 .cpp 里对"其实用不上"的说明

    // 日志计数（每 25 帧打一条），见 .cpp 里的说明
    int framesSinceLog_ = 0;
    int frameLogCount_  = 0;
};

#endif // DISPLAYWIND_H
