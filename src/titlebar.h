#ifndef TITLEBAR_H
#define TITLEBAR_H

#include <QWidget>

class QLabel;
class QPushButton;

// ============================================================================
// titlebar.h —— 顶部标题栏（原 titlebar.ui 改为纯代码构建）
//
// 【为什么自己画标题栏，不用系统自带的？】
//   播放器这类"全屏沉浸式"应用通常希望整块界面风格统一（深色、无系统边框）。
//   本工程保留系统边框，但把标题栏做成一个普通控件放进顶部 Dock ——
//   好处是还能显示"当前文件名的信息"，而且按钮样式可控。
//
// 【设计上值得注意的一点：它只管发信号，不管干什么】
//   最小化/最大化/关闭这三个动作最终要落到主窗口（MainWind）上，
//   但 TitleBar 并不持有主窗口指针 —— 它只 emit 三个信号，
//   由 MainWind 决定怎么响应。
//   这就是"子控件不反向依赖父窗口"的做法，控件可以独立复用和测试。
// ============================================================================

class TitleBar : public QWidget
{
    Q_OBJECT
public:
    explicit TitleBar(QWidget* parent = nullptr);

    void setTitle(const QString& title);

signals:
    void minimizeRequested();
    void maximizeToggleRequested();
    void closeRequested();

private:
    QLabel*      titleLabel_ = nullptr;
    QPushButton* minBtn_     = nullptr;
    QPushButton* maxBtn_     = nullptr;
    QPushButton* closeBtn_   = nullptr;
};

#endif // TITLEBAR_H
