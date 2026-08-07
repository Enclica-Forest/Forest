#include "PresetGallery.h"
#include "ConfigModel.h"
#include "PreviewWidget.h"

#include <QGridLayout>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QPushButton>
#include <QPixmap>
#include <QIcon>

// Render a preset thumbnail by grabbing an off-screen PreviewWidget bound to a
// throwaway model that has the preset applied - identical to the live preview.
static QPixmap renderThumb(int presetIndex) {
    ConfigModel tmp;
    // give it a few sample rows so the menu looks populated
    EntryNode a; a.title="Forest OS"; a.type="forest";
    EntryNode b; b.title="Linux"; b.type="linux";
    EntryNode c; c.title="Reboot"; c.type="reboot";
    tmp.roots = {a,b,c};
    tmp.applyPreset(presetIndex);
    PreviewWidget pv(&tmp);
    pv.resize(240, 150);
    return pv.grab();
}

PresetGallery::PresetGallery(ConfigModel *m, QWidget *parent) : QWidget(parent), model(m) {
    auto *outer = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this); scroll->setWidgetResizable(true);
    auto *content = new QWidget;
    auto *grid = new QGridLayout(content);
    const int cols = 3;
    for (int i = 0; i < forbPresetCount(); ++i) {
        auto *btn = new QPushButton(forbPresetName(i), content);
        QPixmap pm = renderThumb(i);
        btn->setIcon(QIcon(pm));
        btn->setIconSize(pm.size());
        btn->setStyleSheet("text-align:center; padding:6px;");
        btn->setMinimumSize(pm.width()+16, pm.height()+40);
        connect(btn, &QPushButton::clicked, this, [this, i]{
            model->applyPreset(i);
            emit presetApplied();
        });
        grid->addWidget(btn, i / cols, i % cols);
    }
    scroll->setWidget(content);
    outer->addWidget(scroll);
}
