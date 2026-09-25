#include "displaywind.h"

#include <QDebug>
#include <QPainter>
#include <QPalette>

DisplayWind::DisplayWind(QWidget* parent)
    : QWidget(parent)
{
    // 纯绘制区域：黑底 + 最小尺寸。
    // setAutoFillBackground(true) 让 Qt 用调色板里的 Window 色填背景，
    // 而 paintEvent 里我们没有填满整块区域（只画了居中的图），
    // 所以背景色必须设成黑色，否则边缘会出现默认灰。
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);
    setMinimumSize(320, 180);
}

void DisplayWind::presentFrame(const QImage& image)
{
    {
        QMutexLocker lock(&mutex_);
        image_ = image;         // QImage 是隐式共享的，这次赋值只是引用计数 +1
    }

    // 每 25 帧打一条日志（约每秒一次）。
    // 用途很实际：排查"有声音没画面"时，看一眼这里有没有帧进来，
    // 立刻就能分辨是"视频链路断了"还是"绘制出了问题"。
    if (++framesSinceLog_ >= 25) {
        framesSinceLog_ = 0;
        qDebug() << "[DisplayWind] 已显示" << ++frameLogCount_ * 25
                 << "帧左右，尺寸" << image.size();
    }

    update();                   // 请求重绘（不是立即绘制）
}

void DisplayWind::clear()
{
    {
        QMutexLocker lock(&mutex_);
        image_ = QImage();
    }
    update();
}

void DisplayWind::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);

    // 先铺黑底：视频宽高比和控件不一致时，边缘就是黑边
    painter.fillRect(rect(), Qt::black);

    QImage img;
    {
        QMutexLocker lock(&mutex_);
        img = image_;
    }
    if (img.isNull())
        return;                 // 还没有画面，保持全黑

    // ★ 缩放 + 居中，一句话解决：
    //   KeepAspectRatio   —— 保持宽高比，缩放到能放进控件为止
    //   SmoothTransformation —— 双线性滤波，缩小视频时明显更干净
    //   （代价是比 FastTransformation 慢一点。视频显示的场景下
    //     这点开销换来画质提升很划算。）
    const QImage scaled = img.scaled(size(), Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);

    // 居中偏移：多出来的宽度/高度各分一半，两边就一样宽的黑边
    const int x = (width()  - scaled.width())  / 2;
    const int y = (height() - scaled.height()) / 2;
    painter.drawImage(x, y, scaled);
}

// ---------------------------------------------------------------------------
// 关于 mutex_ 的诚实说明
// ---------------------------------------------------------------------------
// presentFrame() 是槽，通过 M12 那套机制被投递到 GUI 线程执行；
// paintEvent() 也永远在 GUI 线程。也就是说这两个函数【本来就在同一条线程里】，
// 这把锁在当前代码路径下不会发生任何竞争。
//
// 保留它的理由：如果将来有人直接从别的线程调 presentFrame（比如绕过信号槽），
// 这把锁能挡住数据撕裂。但你要知道它现在不起作用 ——
// 不要因为"看到锁"就以为这里天然线程安全，真正的保证来自"都用 GUI 线程"。
