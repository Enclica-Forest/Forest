// Inspector.h - a dock that shows the style keys of the element the user
// clicked in the preview (panel / entry / button / window) and binds them
// two-way to the model, so "click element -> edit its keys" works.
#ifndef FORB_INSPECTOR_H
#define FORB_INSPECTOR_H

#include <QWidget>
class ConfigModel;
class QVBoxLayout;
class QLabel;

class Inspector : public QWidget {
    Q_OBJECT
public:
    explicit Inspector(ConfigModel *m, QWidget *parent = nullptr);
public slots:
    void showElement(const QString &kind, int index);
private:
    ConfigModel *model;
    QVBoxLayout *lay;
    QLabel *header;
    QWidget *body = nullptr;
};

#endif // FORB_INSPECTOR_H
