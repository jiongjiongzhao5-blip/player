#include "titlebar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

TitleBar::TitleBar(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(44);

    // 左边是"品牌名"。做成扁平按钮将来可以点出"关于"对话框。
    auto* brand = new QPushButton(QStringLiteral("0VoicePlayer"), this);
    brand->setFlat(true);
    brand->setFixedWidth(150);
    QFont font = brand->font();
    font.setPointSize(12);
    font.setBold(true);
    brand->setFont(font);

    // 中间是当前文件名。stretch 会让它占据所有剩余宽度，
    // 所以右边的三个按钮会被顶到最右 —— 不用手工算位置。
    titleLabel_ = new QLabel(QStringLiteral("未打开文件"), this);
    titleLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

    // 右边三个窗口按钮。用文字符号而不是图标，省得再准备一套资源。
    minBtn_   = new QPushButton(QStringLiteral("—"), this);
    maxBtn_   = new QPushButton(QStringLiteral("▢"), this);
    closeBtn_ = new QPushButton(QStringLiteral("✕"), this);
    for (QPushButton* b : {minBtn_, maxBtn_, closeBtn_}) {
        b->setFixedSize(44, 44);        // 44×44 是常见的"易点"尺寸
        b->setFlat(true);
    }

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(brand);
    layout->addWidget(titleLabel_, 1);  // ← 1 = 拉伸因子，吃掉剩余空间
    layout->addWidget(minBtn_);
    layout->addWidget(maxBtn_);
    layout->addWidget(closeBtn_);

    // ★ 只发信号，不自己干活 —— 具体动作由 MainWind 决定
    connect(minBtn_,   &QPushButton::clicked, this, &TitleBar::minimizeRequested);
    connect(maxBtn_,   &QPushButton::clicked, this, &TitleBar::maximizeToggleRequested);
    connect(closeBtn_, &QPushButton::clicked, this, &TitleBar::closeRequested);
}

void TitleBar::setTitle(const QString& title)
{
    titleLabel_->setText(title);
}
