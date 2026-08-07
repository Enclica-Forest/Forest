#include "Bound.h"
#include "ConfigModel.h"

#include <QHBoxLayout>
#include <QCheckBox>
#include <QSpinBox>
#include <QSlider>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QColorDialog>
#include <QFileDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>

static QColor toQColor(QRgb c) { return QColor((c>>16)&0xFF,(c>>8)&0xFF,c&0xFF); }

// NOTE: picked/dropped files are stored as their real host path. The preview
// can then load them immediately, and MainWindow::stageImagesTGA() converts
// them to /forebo/<name>.tga on save/apply. (Previously the host path was
// thrown away here, so the preview had nothing to load.)

// ---------------------------------------------------------------- OptIntWidget
OptIntWidget::OptIntWidget(Opt<int> *f, ConfigModel *m, int lo, int hi,
                           const QString &suffix, bool slider, QWidget *parent)
    : QWidget(parent), field(f), model(m) {
    auto *lay = new QHBoxLayout(this); lay->setContentsMargins(0,0,0,0);
    chk = new QCheckBox("set", this);
    lay->addWidget(chk);
    if (slider) {
        sld = new QSlider(Qt::Horizontal, this); sld->setRange(lo,hi);
        num = new QLabel(this); num->setMinimumWidth(36);
        lay->addWidget(sld,1); lay->addWidget(num);
        spin = nullptr;
        connect(sld,&QSlider::valueChanged,this,[this]{ if(num) num->setText(QString::number(sld->value())); write(); });
    } else {
        spin = new QSpinBox(this); spin->setRange(lo,hi);
        if (!suffix.isEmpty()) spin->setSuffix(" " + suffix);
        lay->addWidget(spin,1); sld = nullptr;
        connect(spin,qOverload<int>(&QSpinBox::valueChanged),this,[this]{ write(); });
    }
    connect(chk,&QCheckBox::toggled,this,[this](bool on){
        if (spin) spin->setEnabled(on); if (sld) sld->setEnabled(on);
        write();
    });
    connect(model,&ConfigModel::refreshRequested,this,&OptIntWidget::refresh);
    refresh();
}
void OptIntWidget::write() {
    if (syncing) return;
    if (chk->isChecked()) field->assign(sld ? sld->value() : spin->value());
    else field->unset();
    model->touch();
}
void OptIntWidget::refresh() {
    syncing = true;
    chk->setChecked(field->isSet());
    int v = field->isSet() ? field->v : (sld?sld->minimum():spin->minimum());
    if (sld) { sld->setValue(v); sld->setEnabled(field->isSet()); if(num) num->setText(QString::number(v)); }
    if (spin){ spin->setValue(v); spin->setEnabled(field->isSet()); }
    syncing = false;
}

// --------------------------------------------------------------- OptBoolWidget
OptBoolWidget::OptBoolWidget(Opt<bool> *f, ConfigModel *m, QWidget *parent)
    : QWidget(parent), field(f), model(m) {
    auto *lay = new QHBoxLayout(this); lay->setContentsMargins(0,0,0,0);
    combo = new QComboBox(this);
    combo->addItems({"(inherit)","off","on"});
    lay->addWidget(combo,1);
    connect(combo,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{ write(); });
    connect(model,&ConfigModel::refreshRequested,this,&OptBoolWidget::refresh);
    refresh();
}
void OptBoolWidget::write() {
    if (syncing) return;
    int i = combo->currentIndex();
    if (i == 0) field->unset(); else field->assign(i == 2);
    model->touch();
}
void OptBoolWidget::refresh() {
    syncing = true;
    combo->setCurrentIndex(field->isSet() ? (field->v ? 2 : 1) : 0);
    syncing = false;
}

// -------------------------------------------------------------- OptColorWidget
OptColorWidget::OptColorWidget(Opt<QRgb> *f, ConfigModel *m, QWidget *parent)
    : QWidget(parent), field(f), model(m) {
    auto *lay = new QHBoxLayout(this); lay->setContentsMargins(0,0,0,0);
    chk = new QCheckBox("set", this);
    btn = new QPushButton(this); btn->setMinimumWidth(120);
    lay->addWidget(chk); lay->addWidget(btn,1);
    connect(chk,&QCheckBox::toggled,this,[this](bool on){ btn->setEnabled(on); write(); });
    connect(btn,&QPushButton::clicked,this,[this]{
        QColor c = QColorDialog::getColor(toQColor(cur), this, "Pick colour");
        if (c.isValid()) { cur = (QRgb)(c.rgb() & 0x00FFFFFF); updateSwatch(); write(); }
    });
    connect(model,&ConfigModel::refreshRequested,this,&OptColorWidget::refresh);
    refresh();
}
void OptColorWidget::updateSwatch() {
    QColor c = toQColor(cur);
    QString fg = (c.lightness() > 128) ? "#000" : "#fff";
    btn->setText(QString("0x%1").arg((quint32)cur,6,16,QChar('0')).toUpper());
    btn->setStyleSheet(QString("background:%1; color:%2; padding:4px;").arg(c.name(), fg));
}
void OptColorWidget::write() {
    if (syncing) return;
    if (chk->isChecked()) field->assign(cur); else field->unset();
    model->touch();
}
void OptColorWidget::refresh() {
    syncing = true;
    chk->setChecked(field->isSet());
    if (field->isSet()) cur = field->v;
    btn->setEnabled(field->isSet());
    updateSwatch();
    syncing = false;
}

// --------------------------------------------------------------- OptPathWidget
OptPathWidget::OptPathWidget(Opt<QString> *f, ConfigModel *m, QWidget *parent)
    : QWidget(parent), field(f), model(m) {
    setAcceptDrops(true);
    auto *lay = new QHBoxLayout(this); lay->setContentsMargins(0,0,0,0);
    chk = new QCheckBox("set", this);
    edit = new QLineEdit(this); edit->setPlaceholderText("/forebo/…");
    browse = new QPushButton("Browse…", this);
    lay->addWidget(chk); lay->addWidget(edit,1); lay->addWidget(browse);
    connect(chk,&QCheckBox::toggled,this,[this](bool on){ edit->setEnabled(on); browse->setEnabled(on); write(); });
    connect(edit,&QLineEdit::textEdited,this,[this]{ write(); });
    connect(browse,&QPushButton::clicked,this,[this]{
        QString p = QFileDialog::getOpenFileName(this,"Choose image");
        if (!p.isEmpty()) { chk->setChecked(true); edit->setText(p); write(); }
    });
    connect(model,&ConfigModel::refreshRequested,this,&OptPathWidget::refresh);
    refresh();
}
void OptPathWidget::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}
void OptPathWidget::dropEvent(QDropEvent *e) {
    const auto urls = e->mimeData()->urls();
    if (urls.isEmpty()) return;
    chk->setChecked(true);
    edit->setText(urls.first().toLocalFile());
    write();
}
void OptPathWidget::write() {
    if (syncing) return;
    if (chk->isChecked()) field->assign(edit->text()); else field->unset();
    model->touch();
}
void OptPathWidget::refresh() {
    syncing = true;
    chk->setChecked(field->isSet());
    edit->setText(field->isSet() ? field->v : QString());
    edit->setEnabled(field->isSet()); browse->setEnabled(field->isSet());
    syncing = false;
}

// ------------------------------------------------------------- EnumComboWidget
EnumComboWidget::EnumComboWidget(QString *f, ConfigModel *m, const QStringList &opts,
                                 QWidget *parent)
    : QWidget(parent), field(f), model(m) {
    auto *lay = new QHBoxLayout(this); lay->setContentsMargins(0,0,0,0);
    combo = new QComboBox(this);
    combo->addItem("(inherit)");
    combo->addItems(opts);
    lay->addWidget(combo,1);
    connect(combo,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{ write(); });
    connect(model,&ConfigModel::refreshRequested,this,&EnumComboWidget::refresh);
    refresh();
}
void EnumComboWidget::write() {
    if (syncing) return;
    *field = (combo->currentIndex() == 0) ? QString() : combo->currentText();
    model->touch();
}
void EnumComboWidget::refresh() {
    syncing = true;
    if (field->isEmpty()) combo->setCurrentIndex(0);
    else {
        int i = combo->findText(*field);
        combo->setCurrentIndex(i >= 0 ? i : 0);
    }
    syncing = false;
}
