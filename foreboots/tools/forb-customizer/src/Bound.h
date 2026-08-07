// Bound.h - reusable controls that wire two-way to a ConfigModel field.
//
// Each control holds a pointer to its model field (stable: the Global/Theme
// structs are members of the single ConfigModel). On edit it writes the field
// and calls model->touch() (=> live preview repaint). On model->refreshRequested
// (file load / preset / inspector edit) it re-reads the field, guarded so the
// refresh never re-triggers an edit. "(inherit)" / an unchecked box means the
// key is UNSET and therefore omitted from the saved file.
#ifndef FORB_BOUND_H
#define FORB_BOUND_H

#include <QWidget>
#include <QRgb>
#include "Opt.h"
class ConfigModel;
class QCheckBox;
class QSpinBox;
class QSlider;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;

// Opt<int>: [set] checkbox + spinbox (or slider). Unchecked => unset.
class OptIntWidget : public QWidget {
    Q_OBJECT
public:
    OptIntWidget(Opt<int> *f, ConfigModel *m, int lo, int hi,
                 const QString &suffix = QString(), bool slider = false,
                 QWidget *parent = nullptr);
    void refresh();
private:
    void write();
    Opt<int> *field; ConfigModel *model; bool syncing = false;
    QCheckBox *chk; QSpinBox *spin; QSlider *sld; QLabel *num = nullptr;
};

// Opt<bool>: tri-state combo (inherit / off / on).
class OptBoolWidget : public QWidget {
    Q_OBJECT
public:
    OptBoolWidget(Opt<bool> *f, ConfigModel *m, QWidget *parent = nullptr);
    void refresh();
private:
    void write();
    Opt<bool> *field; ConfigModel *model; bool syncing = false; QComboBox *combo;
};

// Opt<QRgb>: [set] checkbox + colour swatch button (QColorDialog).
class OptColorWidget : public QWidget {
    Q_OBJECT
public:
    OptColorWidget(Opt<QRgb> *f, ConfigModel *m, QWidget *parent = nullptr);
    void refresh();
private:
    void write(); void updateSwatch();
    Opt<QRgb> *field; ConfigModel *model; bool syncing = false;
    QCheckBox *chk; QPushButton *btn; QRgb cur = 0x3FB56B;
};

// Opt<QString> path: [set] checkbox + line edit + Browse (accepts file drops).
class OptPathWidget : public QWidget {
    Q_OBJECT
public:
    OptPathWidget(Opt<QString> *f, ConfigModel *m, QWidget *parent = nullptr);
    void refresh();
protected:
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;
private:
    void write();
    Opt<QString> *field; ConfigModel *model; bool syncing = false;
    QCheckBox *chk; QLineEdit *edit; QPushButton *browse;
};

// QString enum: combo with "(inherit)" first item. Empty string => inherit.
class EnumComboWidget : public QWidget {
    Q_OBJECT
public:
    EnumComboWidget(QString *f, ConfigModel *m, const QStringList &opts,
                    QWidget *parent = nullptr);
    void refresh();
private:
    void write();
    QString *field; ConfigModel *model; bool syncing = false; QComboBox *combo;
};

#endif // FORB_BOUND_H
