#include <QApplication>
#include <QDebug>
#include <QLabel>
#include <QMainWindow>

// ============================================================================
// main.cpp —— 程序入口（M1 版）
//
// 这个文件的职责极其单一：创建 QApplication、创建主窗口、进入事件循环。
// 真正的播放逻辑一行都不在这里——它只是把"操作系统"和"我们的窗口"接上线。
//
// 后面的模块会做两件事：
//   M13 把这里的 QMainWindow 换成 MainWind（我们自己的主窗口类）；
//   其余逻辑仍然留在各自的模块里，main.cpp 永远保持这么短。
// ============================================================================

int main(int argc, char* argv[])
{
    // 1) QApplication 必须是程序中第一个被构造的 Qt 对象。
    //    它负责初始化 Qt 的运行时（事件循环、平台插件、字体、样式等）。
    //    argc/argv 必须原样传进去——Qt 要从中解析 -platform、-style 等参数。
    QApplication app(argc, argv);

    // 2) 应用标识。看着像可选项，其实有实际作用：
    //    QSettings / QStandardPaths 会用它们拼出配置目录，
    //    setApplicationName 还会影响部分平台下的任务管理器显示名。
    QApplication::setApplicationName(QStringLiteral("voice_player_qt6"));
    QApplication::setOrganizationName(QStringLiteral("ZJJ"));

    // 3) 主窗口建在栈上（不是 new）。
    //    QApplication 退出后自动析构，不需要 delete，也不会泄漏。
    //    窗口尺寸 1280x800 是给后面的视频画面留的默认大小。
    QMainWindow window;
    window.resize(1280, 800);
    window.setWindowTitle(QStringLiteral("0VoicePlayer (Qt6 + FFmpeg + SDL3)"));

    // 4) 中央区域先放一个占位标签。
    //    等到 M13，这两行会被 DisplayWind + CtrlBar 的布局替换掉。
    //    注意这里给了父对象 &window：Qt 的对象树会自动负责它的释放。
    auto* placeholder = new QLabel(QStringLiteral("M1 骨架已跑通 —— 等待接入内核"), &window);
    placeholder->setAlignment(Qt::AlignCenter);
    window.setCentralWidget(placeholder);

    // 5) 验证信息打到控制台。
    //    因为 CMake 里暂时没写 WIN32 关键字，这个程序带控制台，
    //    所以你在 Qt Creator 的 "应用程序输出" 面板能直接看到这一行。
    //    以后每一轮模块我们都靠这种方式验证，直到 M13 才关掉控制台。
    qDebug() << "[M1] Qt 运行时版本:" << qVersion();
    qDebug() << "[M1] 主窗口已创建，即将进入事件循环。";

    // 6) show() 只是把窗口标记为"要显示"，真正的绘制由事件循环驱动。
    window.show();

    // 7) exec() 进入事件循环：程序在这里"停住"，不断处理鼠标/键盘/绘制/定时器事件，
    //    直到最后一个窗口关闭才返回。返回值作为进程退出码。
    return app.exec();
}
