// PresetGallery.h - a scrollable grid of ~30 preset cards. Each card renders a
// live thumbnail with the SAME painter as the main preview (an off-screen
// PreviewWidget grabbed to a pixmap), so a thumbnail always matches what the
// preset produces. Clicking a card applies it into the model.
#ifndef FORB_PRESETGALLERY_H
#define FORB_PRESETGALLERY_H

#include <QWidget>
class ConfigModel;

class PresetGallery : public QWidget {
    Q_OBJECT
public:
    explicit PresetGallery(ConfigModel *m, QWidget *parent = nullptr);
signals:
    void presetApplied();
private:
    ConfigModel *model;
};

int         forbPresetCount();
const char *forbPresetName(int i);

#endif // FORB_PRESETGALLERY_H
