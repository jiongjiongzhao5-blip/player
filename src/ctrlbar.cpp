#include "ctrlbar.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace {
constexpr int kBtnSize = 32;

// 小工具：造一个扁平图标按钮。
// 抽出来是因为下面要用三次（播放/暂停、停止），重复的样式设置容易写漏。
QPushButton* makeIconButton(const QIcon& icon)
{
    auto* btn = new QPushButton;
    btn->setFixedSize(kBtnSize, kBtnSize);
    btn->setIcon(icon);
    btn->setFlat(true);
    btn->setCursor(Qt::PointingHandCursor);
    return btn;
}
}  // namespace

CtrlBar::CtrlBar(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(72);

    // ---- 第一行：进度条 ----
    playSlider_ = new QSlider(Qt::Horizontal, this);
    playSlider_->setRange(0, 0);        // 时长未知时范围就是 0..0（不可拖）
    // ⚠ 注意这里【没有】连 valueChanged —— 只有用户拖完（sliderReleased）才 seek。
    //   否则播放位置每 200ms 一推送，就会触发一次 seek，直接失控。

    // ---- 第二行：按钮 + 时间 + 音量 ----
    auto* bottomRow = new QHBoxLayout;
    bottomRow->setContentsMargins(8, 0, 8, 4);
    bottomRow->setSpacing(6);

    playOrPauseBtn_ = makeIconButton(QIcon(":/icons/play.png"));
    stopBtn_        = makeIconButton(QIcon(":/icons/stop.png"));
    backwardBtn_    = new QPushButton(QStringLiteral("-10s"), this);
    forwardBtn_     = new QPushButton(QStringLiteral("+10s"), this);
    speedBtn_       = new QPushButton(QStringLiteral("倍速1.0"), this);
    speedBtn_->setFixedWidth(88);

    currentLabel_ = new QLabel("00:00", this);
    auto* slash   = new QLabel("/", this);
    totalLabel_   = new QLabel("00:00", this);
    // 固定宽度：否则时间从 "9:59" 变成 "10:00" 时整行按钮会左右抖动
    for (QLabel* l : {currentLabel_, slash, totalLabel_})
        l->setFixedWidth(46);
    slash->setFixedWidth(12);
    slash->setAlignment(Qt::AlignCenter);

    volumeBtn_    = new QPushButton(QStringLiteral("音量"), this);
    volumeSlider_ = new QSlider(Qt::Horizontal, this);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(100);
    volumeSlider_->setFixedWidth(90);

    playListBtn_ = new QPushButton(QStringLiteral("列表"), this);

    bottomRow->addWidget(playOrPauseBtn_);
    bottomRow->addWidget(stopBtn_);
    bottomRow->addWidget(backwardBtn_);
    bottomRow->addWidget(forwardBtn_);
    bottomRow->addWidget(speedBtn_);
    bottomRow->addWidget(currentLabel_);
    bottomRow->addWidget(slash);
    bottomRow->addWidget(totalLabel_);
    bottomRow->addStretch();            // 把音量/列表推到右边
    bottomRow->addWidget(volumeBtn_);
    bottomRow->addWidget(volumeSlider_);
    bottomRow->addWidget(playListBtn_);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 2, 0, 0);
    root->setSpacing(2);
    root->addWidget(playSlider_);
    root->addLayout(bottomRow);

    // ---- 信号连接 ----
    connect(playOrPauseBtn_, &QPushButton::clicked, this, &CtrlBar::playOrPauseClicked);
    connect(stopBtn_,        &QPushButton::clicked, this, &CtrlBar::stopClicked);

    // ±10 秒：界面上只发"偏移几秒"，具体加到哪个位置由 MainWind 决定
    //（因为它才知道当前播放到哪了）。控件不该自己维护"当前位置"这份状态。
    connect(forwardBtn_,  &QPushButton::clicked, this, [this] { emit seekOffsetRequested(10); });
    connect(backwardBtn_, &QPushButton::clicked, this, [this] { emit seekOffsetRequested(-10); });

    connect(speedBtn_,    &QPushButton::clicked, this, &CtrlBar::cycleSpeed);
    connect(playListBtn_, &QPushButton::clicked, this, &CtrlBar::playlistToggled);

    // ★ 进度条的"拖动中"处理（见头文件 ①）
    connect(playSlider_, &QSlider::sliderPressed,  this, [this] { sliderDragging_ = true; });
    connect(playSlider_, &QSlider::sliderReleased, this, &CtrlBar::onSliderReleased);

    // 音量滑块：拖动过程中实时生效，这是符合直觉的
    connect(volumeSlider_, &QSlider::valueChanged, this, &CtrlBar::volumeChanged);
}

// 毫秒 -> "mm:ss" 或 "hh:mm:ss"（超过一小时才显示小时）
QString CtrlBar::formatTime(qint64 ms)
{
    if (ms < 0) ms = 0;
    const qint64 totalSec = ms / 1000;
    const qint64 h = totalSec / 3600;
    const qint64 m = (totalSec % 3600) / 60;
    const qint64 s = totalSec % 60;
    if (h > 0)
        return QString::asprintf("%02lld:%02lld:%02lld",
                                 static_cast<long long>(h),
                                 static_cast<long long>(m),
                                 static_cast<long long>(s));
    return QString::asprintf("%02lld:%02lld",
                             static_cast<long long>(m),
                             static_cast<long long>(s));
}

void CtrlBar::setPositionMs(qint64 ms)
{
    currentLabel_->setText(formatTime(ms));

    // ★ 用户正在拖的时候就别动了 —— 否则每 200ms 一次的推送会把滑块拽回去。
    //   ⚠ 这里还套了 QSignalBlocker：setValue 会触发 valueChanged，
    //     虽然我们没连它，但保险起见（将来有人连了也不该被"程序性更新"触发）。
    if (!sliderDragging_ && durationMs_ > 0) {
        QSignalBlocker blocker(playSlider_);
        playSlider_->setValue(static_cast<int>(ms));
    }
}

void CtrlBar::setDurationMs(qint64 ms)
{
    durationMs_ = ms;
    totalLabel_->setText(formatTime(ms));
    playSlider_->setRange(0, static_cast<int>(ms));
}

void CtrlBar::setPlaying(bool playing)
{
    playOrPauseBtn_->setIcon(QIcon(playing ? ":/icons/pause.png" : ":/icons/play.png"));
}

void CtrlBar::reset()
{
    durationMs_ = 0;
    playSlider_->setRange(0, 0);
    currentLabel_->setText("00:00");
    totalLabel_->setText("00:00");
    setPlaying(false);
}

void CtrlBar::onSliderReleased()
{
    sliderDragging_ = false;
    emit seekRequested(playSlider_->value());   // 到这一刻才真正 seek 一次
}

void CtrlBar::cycleSpeed()
{
    speedIndex_ = (speedIndex_ + 1) % 4;
    const float ratio = kSpeeds[speedIndex_];
    speedBtn_->setText(QStringLiteral("倍速%1").arg(ratio, 0, 'f', 1));
    emit speedChanged(ratio);
}
