#ifndef MAINWIND_H
#define MAINWIND_H

#include <QMainWindow>
#include <QString>

#include "mediaplayer.h"

class DisplayWind;
class CtrlBar;
class TitleBar;
class PlayListWind;
class QDockWidget;

// ============================================================================
// mainwind.h —— 播放器主窗口（原 mainwind.ui 改为纯代码构建）
//
// 【它是"总装车间"，也是全工程唯一知道所有部件的地方】
//   前面 12 轮的东西在这里第一次被拼到一起：
//     CtrlBar / TitleBar / DisplayWind / PlayListWind  ← 四个自绘控件
//     MediaPlayer                                      ← M12 的门面
//   而 MainWind 自己【不碰 FFmpeg、不碰内核】，它只做三件事：
//     ① 摆放控件（buildUi）
//     ② 建菜单（buildMenu）
//     ③ 把控件信号 <-> 播放器信号接线（wireSignals）
//
// 【接线为什么全放在这里，而不是让控件之间互相连？】
//   如果 CtrlBar 直接连 MediaPlayer，那么 CtrlBar 就得知道 MediaPlayer 的存在，
//   两个控件从此耦合在一起，谁都不能单独复用/单独测试。
//   让唯一的"总装点"负责接线，各控件就都是"只发信号、只收槽"的独立部件。
//   这是 Qt 界面里最常见也最有用的组织方式：**星形接线，不是网状接线**。
//
// 【信号流向一览（wireSignals 里的全部内容）】
//   CtrlBar ──> MainWind 的槽 ──> MediaPlayer 的槽          （用户操作向下）
//   TitleBar ──> MainWind 的槽（窗口控制）
//   MediaPlayer ──> MainWind 的槽 ──> CtrlBar / DisplayWind （状态向上再分发）
// ============================================================================

class MainWind : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWind(QWidget* parent = nullptr);
    ~MainWind() override;

    // 打开并播放一个媒体文件。
    // 【为什么做成 public】两个用途：
    //   · 菜单"文件 -> 打开"和按钮的兜底逻辑会调它；
    //   · 支持命令行参数（把文件拖到 exe 上也能放），见 main.cpp。
    void openUrl(const QString& path);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void openFile();                        // 弹文件对话框
    void onPlayOrPause();
    void onStop();
    void onSeek(qint64 ms);
    void onSeekOffset(int seconds);
    void onVolume(int percent);
    void onSpeed(float ratio);

    // ---- 播放器 -> 界面 ----
    void onPrepared();
    void onCompleted();
    void onError(const QString& message);
    void onPosition(qint64 ms);
    void onDuration(qint64 ms);

private:
    void buildUi();
    void buildMenu();
    void wireSignals();

    MediaPlayer*   mp_       = nullptr;
    DisplayWind*   display_  = nullptr;
    CtrlBar*       ctrl_     = nullptr;
    TitleBar*      title_    = nullptr;
    PlayListWind*  playlist_ = nullptr;
    QDockWidget*   playlistDock_ = nullptr;

    QString currentFile_;
    // 记住"最近一次上报的播放位置"。
    // ★ 为什么要自己记？因为 ±10 秒按钮只知道"偏移多少"，
    //   要知道"从哪偏移"就得有个基准。界面上没人愿意维护这份状态，
    //   所以由 MainWind 统一持有 —— 它就是那个"知道全局"的地方。
    qint64  lastPositionMs_ = 0;
};

#endif // MAINWIND_H
