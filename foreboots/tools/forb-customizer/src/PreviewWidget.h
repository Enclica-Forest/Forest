// PreviewWidget.h - the WYSIWYG canvas. paintEvent() renders the boot menu
// from the model, resolving preset -> theme palette -> explicit overrides the
// same way the firmware loader does, then painting in the loader's layer order.
// Repaints on model->changed(). Clicking an element selects it (Inspector).
#ifndef FORB_PREVIEWWIDGET_H
#define FORB_PREVIEWWIDGET_H

#include <QWidget>
#include <QRect>
#include <QVector>
#include <QHash>
#include <QImage>
#include <QString>
class ConfigModel;

class PreviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit PreviewWidget(ConfigModel *m, QWidget *parent = nullptr);
    QSize sizeHint() const override { return QSize(560, 440); }
    void setFbSize(int w, int h) { fbW = w; fbH = h; update(); }
    // Directory of the loaded forebo.cfg (the ESP /forebo dir) so ESP-relative
    // image paths like /forebo/bg.bmp resolve to a real host file to preview.
    void setConfigDir(const QString &d) { configDir = d; imgCache.clear(); update(); }
    // Resolve + load an image field (host path OR ESP-relative). BMP/PNG/JPG via
    // Qt, plus a built-in TGA decoder. Cached by path.
    QImage loadImg(const QString &field);

signals:
    void elementClicked(const QString &kind, int index); // "panel","entry","button","window"

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;

private:
    ConfigModel *model;
    int fbW = 1920, fbH = 1080;
    QRect panelRect, buttonRect, windowRect;
    QVector<QRect> entryRects;
    double lastScale = 1.0; int lastOx = 0, lastOy = 0; // native<->widget transform
    QString configDir;
    // Resolved host path -> image, validated by mtime so edited/re-staged files
    // reload. Failed lookups are NEVER cached (a missing file may appear later).
    struct ImgEnt { QImage img; qint64 mtime = 0; };
    QHash<QString, ImgEnt> imgCache;
};

#endif // FORB_PREVIEWWIDGET_H
