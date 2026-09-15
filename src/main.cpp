#include <QApplication>
#include <QDebug>
#include <QLabel>
#include <QMainWindow>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    QApplication::setApplicationName(QStringLiteral("voice_player_qt6"));
    QApplication::setOrganizationName(QStringLiteral("ZJJ"));

    QMainWindow window;
    window.resize(1280, 800);
    window.setWindowTitle(QStringLiteral("0VoicePlayer (Qt6 + FFmpeg + SDL3)"));

    auto* placeholder = new QLabel(QStringLiteral("M1 骨架已跑通 —— 等待接入内核"), &window);
    placeholder->setAlignment(Qt::AlignCenter);
    window.setCentralWidget(placeholder);

    qDebug() << "[M1] Qt 运行时版本:" << qVersion();
    qDebug() << "[M1] 主窗口已创建，即将进入事件循环。";

    window.show();

    return app.exec();
}
