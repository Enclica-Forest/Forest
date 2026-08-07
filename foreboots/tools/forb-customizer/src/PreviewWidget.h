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
    void setConfigDir(const QString &d) { configDir = d; imgCache.clear(); update(); }
    QImage loadImg(const QString &field);
    void zoomIn();
    void zoomOut();
    void zoomReset();
    int selectedEntryIndex() const { return selectedEntry; }
    void selectEntry(int idx);

signals:
    void elementClicked(const QString &kind, int index);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

private:
    ConfigModel *model;
    int fbW = 1920, fbH = 1080;
    QRect panelRect, buttonRect, windowRect;
    QVector<QRect> entryRects;
    double lastScale = 1.0; int lastOx = 0, lastOy = 0;
    QString configDir;
    int selectedEntry = -1;
    double zoomLevel = 1.0;
    // Resolved host path -> image, validated by mtime so edited/re-staged files
    // reload. Failed lookups are NEVER cached (a missing file may appear later).
    struct ImgEnt { QImage img; qint64 mtime = 0; };
    QHash<QString, ImgEnt> imgCache;
};

#endif // FORB_PREVIEWWIDGET_H
