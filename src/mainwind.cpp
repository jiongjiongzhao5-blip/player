#include "mainwind.h"

#include <algorithm>

#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

#include "ctrlbar.h"
#include "displaywind.h"
#include "playlistwind.h"
#include "titlebar.h"

MainWind::MainWind(QWidget* parent)
    : QMainWindow(parent)
{
    // ★ 播放器实例由主窗口持有（父对象是 this）。
    //   MediaPlayer 构造时会建 QTimer，析构时 stop()；交给 Qt 的对象树管理
    //   生命周期，比自己 delete 安全。
    mp_ = new MediaPlayer(this);

    buildUi();
    buildMenu();
    wireSignals();

    resize(1280, 800);
    setWindowTitle(QStringLiteral("0VoicePlayer (Qt6 + FFmpeg + SDL3)"));
}

MainWind::~MainWind()
{
    // 显式停一次：虽然 MediaPlayer 的析构也会 stop()，但那时 Qt 可能在
    // 拆对象树，早一点停能让"内核线程"在主窗口销毁前就结束 —— 更可控。
    mp_->stop();
}

// ---------------------------------------------------------------------------
// buildUi —— 摆放控件
// ---------------------------------------------------------------------------
void MainWind::buildUi()
{
    // ---- 中央区域：视频显示 + 底部控制条 ----
    // 用 QVBoxLayout 的拉伸因子让"显示区吃掉所有剩余高度、控制条固定"：
    //   addWidget(display_, 1)  拉伸因子 1
    //   addWidget(ctrl_, 0)     拉伸因子 0 = 不拉伸 = 保持固定高度
    display_ = new DisplayWind(this);
    ctrl_    = new CtrlBar(this);

    auto* central = new QWidget(this);
    auto* layout  = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);   // 视频画面贴边，不要留白
    layout->setSpacing(0);
    layout->addWidget(display_, 1);
    layout->addWidget(ctrl_, 0);
    setCentralWidget(central);

    // ---- 顶部标题栏：塞进一个 Dock ----
    // ★ 为什么用 Dock 而不是直接 addToolBar 或放进布局？
    //   Dock 自带"可浮动/可停靠/可隐藏"，省掉一堆布局代码；
    //   下面 setTitleBarWidget(new QWidget()) 把 Dock 自带的标题条换成空白，
    //   于是看起来就是一个普通的顶部条。
    title_ = new TitleBar(this);
    auto* titleDock = new QDockWidget(this);
    titleDock->setFeatures(QDockWidget::NoDockWidgetFeatures);   // 禁止拖动这个 Dock
    titleDock->setTitleBarWidget(new QWidget());
    titleDock->setWidget(title_);
    addDockWidget(Qt::TopDockWidgetArea, titleDock);

    // ---- 左侧播放列表：Dock 里放 PlayListWind ----
    playlist_ = new PlayListWind(this);
    playlistDock_ = new QDockWidget(QStringLiteral("播放列表"), this);
    playlistDock_->setWidget(playlist_);
    playlistDock_->setMinimumWidth(180);
    addDockWidget(Qt::LeftDockWidgetArea, playlistDock_);

    statusBar()->showMessage(QStringLiteral("就绪，可通过“文件 -> 打开”选择视频"));
}

// ---------------------------------------------------------------------------
// buildMenu
// ---------------------------------------------------------------------------
void MainWind::buildMenu()
{
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("文件"));
    // QKeySequence::Open 会自动用当前平台的快捷键（Windows 上是 Ctrl+O）
    fileMenu->addAction(QStringLiteral("打开..."), QKeySequence::Open,
                        this, &MainWind::openFile);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("退出"), QKeySequence::Quit,
                        this, &QWidget::close);

    auto* viewMenu = menuBar()->addMenu(QStringLiteral("视图"));
    // ★ 一行拿到"显示/隐藏播放列表"的菜单项：Dock 自带一个可勾选的 action，
    //   它和 Dock 的可见性双向同步。这是 Dock 最省事的地方。
    viewMenu->addAction(playlistDock_->toggleViewAction());
}

// ---------------------------------------------------------------------------
// wireSignals —— 全部接线集中在这里（见头文件的说明）
// ---------------------------------------------------------------------------
void MainWind::wireSignals()
{
    // ---------- 控件 -> MainWind ----------
    connect(ctrl_, &CtrlBar::playOrPauseClicked, this, &MainWind::onPlayOrPause);
    connect(ctrl_, &CtrlBar::stopClicked,        this, &MainWind::onStop);
    connect(ctrl_, &CtrlBar::seekRequested,      this, &MainWind::onSeek);
    connect(ctrl_, &CtrlBar::seekOffsetRequested, this, &MainWind::onSeekOffset);
    connect(ctrl_, &CtrlBar::volumeChanged,      this, &MainWind::onVolume);
    connect(ctrl_, &CtrlBar::speedChanged,       this, &MainWind::onSpeed);
    connect(ctrl_, &CtrlBar::playlistToggled,    this,
            [this] { playlistDock_->setVisible(!playlistDock_->isVisible()); });

    // ---------- 标题栏 -> 窗口动作 ----------
    connect(title_, &TitleBar::minimizeRequested, this, &QWidget::showMinimized);
    connect(title_, &TitleBar::maximizeToggleRequested, this, [this] {
        isMaximized() ? showNormal() : showMaximized();
    });
    connect(title_, &TitleBar::closeRequested, this, &QWidget::close);

    // ---------- 播放器 -> 界面 ----------
    // ★ frameReady 是跨线程信号（在内核刷新线程里 emit），
    //   这里传了接收者 display_（活在 GUI 线程），所以连接类型会自动是
    //   QueuedConnection —— 槽会在 GUI 线程执行。这正是 M12 讲的
    //   "connect 必须带 context" 的实际应用。
    connect(mp_, &MediaPlayer::frameReady,      display_, &DisplayWind::presentFrame);

    connect(mp_, &MediaPlayer::positionChanged, this, &MainWind::onPosition);
    connect(mp_, &MediaPlayer::durationChanged, this, &MainWind::onDuration);
    connect(mp_, &MediaPlayer::prepared,        this, &MainWind::onPrepared);
    connect(mp_, &MediaPlayer::completed,       this, &MainWind::onCompleted);
    connect(mp_, &MediaPlayer::errorOccurred,   this, &MainWind::onError);
}

// ---------------------------------------------------------------------------
// openUrl —— 打开并播放指定文件
// ---------------------------------------------------------------------------
void MainWind::openUrl(const QString& path)
{
    if (path.isEmpty())
        return;

    // 切换文件前先把上一次彻底停掉（M12 的 teardown 会 join 全部线程），
    // 并把界面恢复到"干净状态"。
    mp_->stop();
    display_->clear();
    ctrl_->reset();
    lastPositionMs_ = 0;

    currentFile_ = path;
    mp_->setDataSource(path);
    mp_->play();                       // 异步：等 prepared 信号回来才算真的开始

    // 界面上的"当前文件"信息立即更新 —— 不用等播放器准备好
    const QFileInfo fi(path);
    title_->setTitle(fi.fileName());
    playlist_->addItem(fi.fileName());
    statusBar()->showMessage(path);
}

void MainWind::openFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开媒体文件"), QString(),
        QStringLiteral("媒体文件 (*.mp4 *.mkv *.avi *.mov *.flv *.ts *.mp3 *.aac *.wav *.flac);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;                        // 用户取消了
    openUrl(path);
}

// ---------------------------------------------------------------------------
// 控件动作
// ---------------------------------------------------------------------------
void MainWind::onPlayOrPause()
{
    // ★ 还没打开过任何文件时，点"播放"的合理行为是"弹打开对话框"，
    //   而不是报错。这是体验上的小细节，但用户会明显感觉到差别。
    if (currentFile_.isEmpty()) {
        openFile();
        return;
    }
    mp_->togglePlayPause();
    ctrl_->setPlaying(mp_->isPlaying());
}

void MainWind::onStop()
{
    mp_->stop();
    ctrl_->reset();
    display_->clear();
    lastPositionMs_ = 0;
}

void MainWind::onSeek(qint64 ms)
{
    mp_->seek(ms);
    lastPositionMs_ = ms;              // 立即更新基准，否则紧接着点 ±10s 会算错
}

void MainWind::onSeekOffset(int seconds)
{
    // 用 max(0, ...) 挡住"往前 10 秒"越界到负数的情况
    mp_->seek(std::max<qint64>(0, lastPositionMs_ + seconds * 1000LL));
}

void MainWind::onVolume(int percent)
{
    mp_->setVolume(percent);           // 界面是 0~100，换算在 MediaPlayer 里做
}

void MainWind::onSpeed(float ratio)
{
    mp_->setSpeed(ratio);
    statusBar()->showMessage(QStringLiteral("播放倍速：%1x").arg(ratio, 0, 'f', 1), 1500);
}

// ---------------------------------------------------------------------------
// 播放器状态 -> 界面
// ---------------------------------------------------------------------------
void MainWind::onPrepared()
{
    ctrl_->setPlaying(true);           // 显示"暂停"图标 = 正在播放
}

void MainWind::onCompleted()
{
    // ★ 注意这个信号来自 M10 修正后的语义：**真的播完了**，
    //   而不是"文件读完了"。所以在这里把按钮复位、状态栏提示都是准确的。
    ctrl_->setPlaying(false);
    statusBar()->showMessage(QStringLiteral("播放完成"), 2000);
}

void MainWind::onError(const QString& message)
{
    ctrl_->setPlaying(false);
    QMessageBox::warning(this, QStringLiteral("播放出错"), message);
}

void MainWind::onPosition(qint64 ms)
{
    lastPositionMs_ = ms;              // 维护"当前位置"这份状态（±10s 要用）
    ctrl_->setPositionMs(ms);
}

void MainWind::onDuration(qint64 ms)
{
    ctrl_->setDurationMs(ms);
}

// ---------------------------------------------------------------------------
// closeEvent
// ---------------------------------------------------------------------------
// ★ 关窗时必须【先停播放器】。
//   因为 MediaPlayer 的内核有 5 条线程在跑，其中视频刷新线程会持续调用
//   帧回调。如果窗口先被销毁、而内核线程还在往 display_ 投递信号，
//   就会访问到已经析构的控件。
//   mp_->stop() 内部会 join 全部线程，所以之后再销毁窗口是安全的。
void MainWind::closeEvent(QCloseEvent* event)
{
    mp_->stop();
    QMainWindow::closeEvent(event);
}
