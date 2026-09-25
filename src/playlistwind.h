#ifndef PLAYLISTWIND_H
#define PLAYLISTWIND_H

#include <QWidget>

class QListWidget;

// ============================================================================
// playlistwind.h —— 播放列表侧栏
//
// 【本工程里它是最"轻"的一个控件】，只有三个方法。这是有意的：
//   真实的播放列表要管"当前项、播放模式、下一首/上一首、拖拽排序"……
//   那些都属于"播放器逻辑"，本工程的播放列表暂时只做"显示播过哪些文件"。
//
//   它的价值在于演示 Dock 的用法：MainWind 把它塞进一个 QDockWidget，
//   于是它自动获得了"可浮动/可停靠/可隐藏"的能力 —— 不用自己写一行布局代码。
// ============================================================================

class PlayListWind : public QWidget
{
    Q_OBJECT
public:
    explicit PlayListWind(QWidget* parent = nullptr);

    void addItem(const QString& name);
    void clearItems();

private:
    QListWidget* list_ = nullptr;
};

#endif // PLAYLISTWIND_H
