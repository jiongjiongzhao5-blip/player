#include <QApplication>
#include <QDebug>
#include <QFileInfo>

#include "mainwind.h"

// ============================================================================
// main.cpp —— 程序入口
//
// 【M13 起这里不再是一个空窗口，换成真正的 MainWind】
//   而 main() 本身依然只有十几行。这是好设计的标志：
//   **入口的职责永远只有"创建、配置、启动"**，所有逻辑都在别的类里。
//
// 【顺带支持命令行参数】
//   voice_player_qt6 D:\movie.mp4
//   —— 也可以直接把文件拖到 exe 图标上。
//   这不是为了炫技：它是运行期的第一条"冒烟测试"路径，
//   让我们可以不点任何按钮就把整条链路（解复用→解码→音频→视频→界面）跑一遍。
// ============================================================================

int main(int argc, char* argv[])
{
    // QApplication 必须是第一个被构造的 Qt 对象（M1 讲过），
    // 它负责初始化事件循环、平台插件、字体、样式。
    QApplication app(argc, argv);

    QApplication::setApplicationName("voice_player_qt6");
    QApplication::setOrganizationName("0voice");

    MainWind window;
    window.show();                       // 先显示窗口，再开始播放

    // 命令行带了文件就自动打开并播放。
    // QString::fromLocal8Bit：Windows 下 argv 是本地编码（中文系统=GBK），
    // 直接用 QString(argv[1]) 会把中文路径变成乱码。
    // 这是中文 Windows 上一个很容易漏的细节。
    if (argc > 1) {
        const QString path = QString::fromLocal8Bit(argv[1]);
        if (QFileInfo::exists(path)) {
            qDebug() << "[main] 命令行打开:" << path;
            window.openUrl(path);
        } else {
            qDebug() << "[main] 命令行参数不是有效文件，忽略:" << path;
        }
    } else {
        qDebug() << "[main] 未指定文件，可通过“文件 -> 打开”选择媒体";
    }

    return app.exec();
}
