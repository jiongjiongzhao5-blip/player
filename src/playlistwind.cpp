#include "playlistwind.h"

#include <QListWidget>
#include <QVBoxLayout>

PlayListWind::PlayListWind(QWidget* parent)
    : QWidget(parent)
{
    list_ = new QListWidget(this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->addWidget(list_);
}

void PlayListWind::addItem(const QString& name)
{
    list_->addItem(name);
}

void PlayListWind::clearItems()
{
    list_->clear();
}
