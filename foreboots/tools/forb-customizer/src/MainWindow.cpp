#include "MainWindow.h"
#include "ConfigModel.h"
#include "Schema.h"
#include "Bound.h"
#include "PreviewWidget.h"
#include "EntriesTab.h"
#include "PresetGallery.h"
#include "Inspector.h"

#include <QTabWidget>
#include <QDockWidget>
#include <QSplitter>
#include <QToolBar>
#include <QMenuBar>
#include <QMenu>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QGroupBox>
#include <QComboBox>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QStatusBar>
#include <QProcess>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QImage>
#include <QFile>
#include <QDir>
#include <QVector>
#include <QPair>
#include <QSet>

// ---- small form-row helpers (create bound control + add to a QFormLayout) ---
static QWidget *addColor(QFormLayout *f, const QString &lbl, Opt<QRgb> *fld, ConfigModel *m) {
    auto *w = new OptColorWidget(fld, m); f->addRow(lbl, w); return w; }
static QWidget *addInt(QFormLayout *f, const QString &lbl, Opt<int> *fld, ConfigModel *m,
                       int lo, int hi, const QString &suf = QString(), bool slider = false) {
    auto *w = new OptIntWidget(fld, m, lo, hi, suf, slider); f->addRow(lbl, w); return w; }
static QWidget *addBool(QFormLayout *f, const QString &lbl, Opt<bool> *fld, ConfigModel *m) {
    auto *w = new OptBoolWidget(fld, m); f->addRow(lbl, w); return w; }
static QWidget *addEnum(QFormLayout *f, const QString &lbl, QString *fld, ConfigModel *m,
                        const QStringList &opts) {
    auto *w = new EnumComboWidget(fld, m, opts); f->addRow(lbl, w); return w; }
static QWidget *addPath(QFormLayout *f, const QString &lbl, Opt<QString> *fld, ConfigModel *m) {
    auto *w = new OptPathWidget(fld, m); f->addRow(lbl, w); return w; }

// wrap a form in a scroll area so long tabs stay usable
static QWidget *scrollWrap(QWidget *inner) {
    auto *sa = new QScrollArea; sa->setWidgetResizable(true); sa->setWidget(inner);
    sa->setFrameShape(QFrame::NoFrame);
    return sa;
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    model = new ConfigModel(this);

    // ---- central: tabs | preview ----
    auto *tabs = new QTabWidget;
    preview  = new PreviewWidget(model);
    entries  = new EntriesTab(model);
    gallery  = new PresetGallery(model);
    inspector = new Inspector(model);

    tabs->addTab(scrollWrap(buildGeneralTab()), "General");
    tabs->addTab(scrollWrap(buildThemeTab()), "Theme && Colours");
    tabs->addTab(scrollWrap(buildMenuTab()), "Menu Layout");
    tabs->addTab(scrollWrap(buildButtonsTab()), "Buttons && UI");
    tabs->addTab(scrollWrap(buildEffectsTab()), "Effects");
    tabs->addTab(scrollWrap(buildAudioTab()), "Audio");
    tabs->addTab(scrollWrap(buildWindowSkinTab()), "Window Skin");
    tabs->addTab(scrollWrap(buildImagesTab()), "Images");
    tabs->addTab(entries, "Entries");
    tabs->addTab(gallery, "Presets");

    auto *split = new QSplitter(Qt::Horizontal);
    split->addWidget(tabs);
    auto *pvBox = new QGroupBox("Live preview");
    auto *pvl = new QVBoxLayout(pvBox);
    auto *fbRow = new QComboBox; fbRow->addItems({"640x480","800x600","1024x768","1920x1080"});
    fbRow->setCurrentIndex(3);
    connect(fbRow, &QComboBox::currentTextChanged, this, [this](const QString &s){
        auto parts = s.split('x'); if (parts.size()==2) preview->setFbSize(parts[0].toInt(), parts[1].toInt());
    });
    pvl->addWidget(fbRow);
    pvl->addWidget(preview, 1);
    split->addWidget(pvBox);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    setCentralWidget(split);

    // ---- inspector dock ----
    auto *dock = new QDockWidget("Inspector", this);
    dock->setWidget(inspector);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    connect(preview, &PreviewWidget::elementClicked, inspector, &Inspector::showElement);
    connect(preview, &PreviewWidget::elementClicked, this, [this](const QString &k, int i){
        if (k == "entry") entries->selectFlatIndex(i);
    });
    connect(gallery, &PresetGallery::presetApplied, this, [this]{ entries->reloadFromModel(); });
    connect(model, &ConfigModel::structureChanged, this, [this]{
        if (defaultSyncing) return; // repopulate default combo when entries change
        if (defaultCombo) { defaultSyncing = true;
            QString cur = defaultCombo->currentText();
            defaultCombo->clear();
            for (const auto &fr : model->flatten()) defaultCombo->addItem(fr.label);
            defaultCombo->setCurrentText(cur.isEmpty()? model->g.defaultRef : cur);
            defaultSyncing = false;
        }
    });

    // ---- menus / toolbar ----
    auto *tb = addToolBar("Main");
    auto *mFile = menuBar()->addMenu("&File");
    auto actNew    = mFile->addAction("&New", this, &MainWindow::newFile);
    auto actOpen   = mFile->addAction("&Open forebo.cfg…", this, &MainWindow::openFile);
    auto actImport = mFile->addAction("&Import Limine…", this, &MainWindow::importLimine);
    auto actImpBoot = mFile->addAction("Import from &/boot…", this, &MainWindow::importFromBoot);
    mFile->addSeparator();
    auto actSave   = mFile->addAction("&Save", this, &MainWindow::saveFile);
    auto actSaveAs = mFile->addAction("Save &As…", this, &MainWindow::saveFileAs);
    auto actApply  = mFile->addAction("&Apply to /boot…", this, &MainWindow::applyToBoot);
    mFile->addSeparator();
    mFile->addAction("E&xit", this, &QWidget::close);
    tb->addAction(actNew); tb->addAction(actOpen); tb->addAction(actImport);
    tb->addAction(actImpBoot);
    tb->addSeparator(); tb->addAction(actSave); tb->addAction(actSaveAs);
    tb->addAction(actApply);

    setWindowTitle("ForeB Customizer");
    resize(1180, 760);

    autoLoad();   // find + load an existing /boot forebo.cfg if present
    { QByteArray sv = qgetenv("FORB_SAVE");   // test/scripting: save on startup
      if (!sv.isEmpty()) doSave(QString::fromLocal8Bit(sv)); }

    // Only seed sample entries when nothing was auto-loaded (keeps the preview
    // alive on a fresh start without clobbering a real config).
    if (currentPath.isEmpty()) {
        model->parseText("menuentry \"Forest OS\" { type=forest kernel=/forebo/kernel.elf icon=os }\n"
                         "menuentry \"Linux\" { type=linux vmlinuz=/forebo/vmlinuz initrd=/forebo/initrd.img icon=tux }\n"
                         "menuentry \"Reboot\" { type=reboot icon=reboot }\n");
        model->requestRefresh();
        entries->reloadFromModel();
    }
}

// ---------------------------------------------------------------- tabs
void MainWindow::buildDefaultCombo(QFormLayout *form) {
    defaultCombo = new QComboBox; defaultCombo->setEditable(true);
    for (const auto &fr : model->flatten()) defaultCombo->addItem(fr.label);
    defaultCombo->setCurrentText(model->g.defaultRef);
    connect(defaultCombo, &QComboBox::currentTextChanged, this, [this](const QString &s){
        if (defaultSyncing) return;
        model->g.defaultRef = s; model->touch();
    });
    connect(model, &ConfigModel::refreshRequested, this, [this]{
        // repopulate AND resync: loads/imports replace the whole entry list
        defaultSyncing = true;
        if (defaultCombo) { defaultCombo->clear();
            for (const auto &fr : model->flatten()) defaultCombo->addItem(fr.label);
            defaultCombo->setCurrentText(model->g.defaultRef); }
        defaultSyncing = false;
    });
    form->addRow("default (index or Title/Path)", defaultCombo);
}

QWidget *MainWindow::buildGeneralTab() {
    auto *w = new QWidget; auto *f = new QFormLayout(w);
    addInt(f, "timeout (s)", &model->g.timeout, model, 0, 3600, "s");
    buildDefaultCombo(f);
    addBool(f, "remember_last", &model->g.rememberLast, model);
    addPath(f, "background", &model->g.background, model);
    return w;
}

QWidget *MainWindow::buildThemeTab() {
    auto *w = new QWidget; auto *f = new QFormLayout(w);
    addEnum(f, "theme preset", &model->th.preset, model, Schema::themes());
    addColor(f, "color_bg", &model->th.colorBg, model);
    addColor(f, "color_fg", &model->th.colorFg, model);
    addColor(f, "color_accent", &model->th.colorAccent, model);
    addColor(f, "color_sel_bg", &model->th.colorSelBg, model);
    addColor(f, "color_sel_fg", &model->th.colorSelFg, model);
    addColor(f, "color_titlebar", &model->th.colorTitlebar, model);
    addColor(f, "color_window", &model->th.colorWindow, model);
    addColor(f, "color_cursor", &model->th.colorCursor, model);
    addPath(f, "cursor (tga)", &model->th.cursorPath, model);
    addBool(f, "cursor_enabled", &model->th.cursorEnabled, model);
    addBool(f, "mouse_enabled", &model->th.mouseEnabled, model);
    addBool(f, "animations", &model->th.animations, model);
    addBool(f, "double_buffer", &model->th.doubleBuffer, model);
    addEnum(f, "window_skin", &model->th.windowSkin, model, Schema::windowSkins());
    return w;
}

QWidget *MainWindow::buildMenuTab() {
    auto *w = new QWidget; auto *f = new QFormLayout(w);
    addEnum(f, "menu_style", &model->th.menuStyle, model, Schema::menuStyles());
    addEnum(f, "menu_pos", &model->th.menuPos, model, Schema::menuPos());
    addInt(f, "menu_x (permille)", &model->th.menuX, model, 0, 1000);
    addInt(f, "menu_y (permille)", &model->th.menuY, model, 0, 1000);
    addInt(f, "menu_w (permille)", &model->th.menuW, model, 0, 1000);
    addInt(f, "menu_h (permille)", &model->th.menuH, model, 0, 1000);
    addInt(f, "menu_entry_h (permille)", &model->th.menuEntryH, model, 0, 1000);
    addInt(f, "menu_pad (px)", &model->th.menuPad, model, 0, 200, "px");
    addEnum(f, "menu_align", &model->th.menuAlign, model, Schema::menuAlign());
    addEnum(f, "menu_selection", &model->th.menuSelection, model, Schema::selStyles());
    addEnum(f, "menu_border", &model->th.menuBorder, model, Schema::borderStyles());
    addEnum(f, "menu_corner", &model->th.menuCorner, model, Schema::corners());
    addEnum(f, "menu_icon_side", &model->th.menuIconSide, model, Schema::iconSides());
    addBool(f, "menu_accent_strip", &model->th.menuAccentStrip, model);
    addBool(f, "menu_dividers", &model->th.menuDividers, model);
    addBool(f, "menu_gradient", &model->th.menuGradient, model);
    addBool(f, "menu_shadow", &model->th.menuShadow, model);
    addBool(f, "menu_title_bar", &model->th.menuTitleBar, model);
    addBool(f, "menu_show_title", &model->th.menuShowTitle, model);
    addBool(f, "menu_show_footer", &model->th.menuShowFooter, model);
    addBool(f, "menu_show_timer", &model->th.menuShowTimer, model);
    addBool(f, "menu_show_icons", &model->th.menuShowIcons, model);
    addBool(f, "menu_scrollbar", &model->th.menuScrollbar, model);
    addBool(f, "menu_caret", &model->th.menuCaret, model);
    return w;
}

QWidget *MainWindow::buildButtonsTab() {
    auto *w = new QWidget; auto *f = new QFormLayout(w);
    addEnum(f, "btn_style", &model->th.btnStyle, model, Schema::btnStyles());
    addEnum(f, "btn_corner", &model->th.btnCorner, model, Schema::corners());
    addInt(f, "btn_border (px)", &model->th.btnBorder, model, 0, 16, "px");
    addInt(f, "btn_pad_x (px)", &model->th.btnPadX, model, 0, 64, "px");
    addInt(f, "btn_pad_y (px)", &model->th.btnPadY, model, 0, 64, "px");
    addBool(f, "btn_gradient", &model->th.btnGradient, model);
    addBool(f, "btn_shadow", &model->th.btnShadow, model);
    addBool(f, "btn_glow", &model->th.btnGlow, model);
    addColor(f, "btn_fill", &model->th.btnFill, model);
    addColor(f, "btn_fill_hover", &model->th.btnFillHover, model);
    addColor(f, "btn_fill_active", &model->th.btnFillActive, model);
    addColor(f, "btn_fill_disabled", &model->th.btnFillDisabled, model);
    addColor(f, "btn_text", &model->th.btnText, model);
    addColor(f, "btn_text_hover", &model->th.btnTextHover, model);
    addColor(f, "btn_text_active", &model->th.btnTextActive, model);
    addColor(f, "btn_border_color", &model->th.btnBorderColor, model);
    addColor(f, "btn_focus_color", &model->th.btnFocusColor, model);
    addEnum(f, "ui_window_corner", &model->th.uiWindowCorner, model, Schema::corners());
    addInt(f, "ui_window_border (px)", &model->th.uiWindowBorder, model, 0, 16, "px");
    addInt(f, "ui_panel_alpha", &model->th.uiPanelAlpha, model, 0, 255, "", true);
    addColor(f, "ui_separator", &model->th.uiSeparator, model);
    addInt(f, "ui_scrollbar_w (px)", &model->th.uiScrollbarW, model, 0, 40, "px");
    addColor(f, "ui_scrollbar_color", &model->th.uiScrollbarColor, model);
    addColor(f, "ui_focus_color", &model->th.uiFocusColor, model);
    addInt(f, "ui_focus_width (px)", &model->th.uiFocusWidth, model, 0, 16, "px");
    addInt(f, "ui_font_scale (%)", &model->th.uiFontScale, model, 25, 800, "%");
    return w;
}

QWidget *MainWindow::buildEffectsTab() {
    auto *w = new QWidget; auto *f = new QFormLayout(w);
    addBool(f, "fx_glass", &model->th.fxGlass, model);
    addInt(f, "fx_blur (px)", &model->th.fxBlur, model, 0, 16, "", true);
    addInt(f, "fx_opacity", &model->th.fxOpacity, model, 0, 255, "", true);
    addInt(f, "fx_vignette", &model->th.fxVignette, model, 0, 255, "", true);
    addInt(f, "fx_scanlines", &model->th.fxScanlines, model, 0, 255, "", true);
    return w;
}

QWidget *MainWindow::buildAudioTab() {
    auto *w = new QWidget; auto *f = new QFormLayout(w);
    addBool(f, "pcspeaker", &model->th.pcspeaker, model);
    addInt(f, "audio_volume", &model->th.audioVolume, model, 0, 100, "", true);
    struct { const char *lbl; Tone *t; } tones[] = {
        {"nav", &model->th.toneNav}, {"select", &model->th.toneSelect},
        {"open", &model->th.toneOpen}, {"error", &model->th.toneError},
        {"back", &model->th.toneBack} };
    for (auto &tt : tones) {
        addInt(f, QString("audio_%1_freq (Hz)").arg(tt.lbl), &tt.t->freq, model, 0, 20000, "Hz");
        addInt(f, QString("audio_%1_ms (ms)").arg(tt.lbl), &tt.t->ms, model, 0, 2000, "ms");
    }
    return w;
}

QWidget *MainWindow::buildWindowSkinTab() {
    auto *w = new QWidget; auto *f = new QFormLayout(w);
    addInt(f, "win_title_h (-1 auto)", &model->th.winTitleH, model, -1, 128, "px");
    addColor(f, "win_title_fill", &model->th.winTitleFill, model);
    addColor(f, "win_title_fg", &model->th.winTitleFg, model);
    addColor(f, "win_border_color", &model->th.winBorderColor, model);
    addInt(f, "win_border_w (px)", &model->th.winBorderW, model, 0, 16, "px");
    addEnum(f, "win_corner", &model->th.winCorner, model, Schema::corners());
    addBool(f, "win_shadow", &model->th.winShadow, model);
    addColor(f, "win_close_color", &model->th.winCloseColor, model);
    addEnum(f, "win_button_style", &model->th.winButtonStyle, model, Schema::btnStyles());
    return w;
}

QWidget *MainWindow::buildImagesTab() {
    auto *w = new QWidget; auto *f = new QFormLayout(w);
    addPath(f, "img_background", &model->th.imgBackground, model);
    addPath(f, "img_panel", &model->th.imgPanel, model);
    addPath(f, "img_window", &model->th.imgWindow, model);
    addPath(f, "img_button", &model->th.imgButton, model);
    addPath(f, "img_titlebar", &model->th.imgTitlebar, model);
    addPath(f, "img_cursor", &model->th.imgCursor, model);
    return w;
}

// ---------------------------------------------------------------- file ops
void MainWindow::newFile() {
    model->resetDefaults();
    model->requestRefresh();
    entries->reloadFromModel();
    currentPath.clear();
    preview->setConfigDir(QString());   // drop the previous config's image root + cache
    setWindowTitle("ForeB Customizer");
    statusBar()->showMessage("New config");
}

void MainWindow::openFile() {
    QString p = QFileDialog::getOpenFileName(this, "Open forebo.cfg", QString(),
                                             "forebo.cfg (*.cfg);;All files (*)");
    if (p.isEmpty()) return;
    QString err;
    if (!model->loadFile(p, &err)) { QMessageBox::warning(this, "Open failed", err); return; }
    entries->reloadFromModel();
    currentPath = p;
    preview->setConfigDir(QFileInfo(p).absolutePath());
    setWindowTitle("ForeB Customizer - " + p);
    statusBar()->showMessage("Loaded " + p);
}

void MainWindow::importLimine() {
    QString p = QFileDialog::getOpenFileName(this, "Import limine.conf", QString(),
                                             "Limine (*.conf *.cfg);;All files (*)");
    if (p.isEmpty()) return;
    QString err;
    if (!model->importLimine(p, &err)) { QMessageBox::warning(this, "Import failed", err); return; }
    entries->reloadFromModel();
    if (!currentPath.isEmpty()) preview->setConfigDir(QFileInfo(currentPath).absolutePath());
    statusBar()->showMessage("Imported (migrated) " + p);
}

void MainWindow::saveFile() {
    if (currentPath.isEmpty()) { saveFileAs(); return; }
    doSave(currentPath);
}

void MainWindow::saveFileAs() {
    QString start = currentPath.isEmpty() ? "forebo.cfg" : currentPath;
    QString p = QFileDialog::getSaveFileName(this, "Save forebo.cfg", start,
                                             "forebo.cfg (*.cfg);;All files (*)");
    if (p.isEmpty()) return;
    currentPath = p;
    setWindowTitle("ForeB Customizer - " + p);
    doSave(p);
}

// Write a QImage as an uncompressed 32-bit top-down TGA (img_type 2, descriptor
// 0x28 = top origin + 8 alpha bits) - exactly what uefi/image.c decode_tga reads.
static bool writeTGA(const QImage &src, const QString &path) {
    QImage im = src.convertToFormat(QImage::Format_ARGB32);
    int w = im.width(), h = im.height();
    if (w <= 0 || h <= 0 || w > 0xFFFF || h > 0xFFFF) return false;
    QByteArray o; o.reserve(18 + w*h*4);
    unsigned char hdr[18] = {0};
    hdr[2] = 2;                          // uncompressed true-color
    hdr[12] = w & 0xFF; hdr[13] = (w>>8)&0xFF;
    hdr[14] = h & 0xFF; hdr[15] = (h>>8)&0xFF;
    hdr[16] = 32;                        // bpp
    hdr[17] = 0x28;                      // top-down + 8 alpha bits
    o.append((const char*)hdr, 18);
    for (int y = 0; y < h; ++y) {        // top-down rows
        const QRgb *row = (const QRgb*)im.constScanLine(y);
        for (int x = 0; x < w; ++x) { QRgb p = row[x];
            char px[4] = { (char)qBlue(p), (char)qGreen(p), (char)qRed(p), (char)qAlpha(p) };
            o.append(px, 4); }
    }
    QFile f(path); if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(o) == o.size();
}

// Convert every set image field (host PNG/JPG/BMP/TGA) to a ForeB TGA written
// into `dir`, rewrite the field to /forebo/<name>.tga, and return the staged
// (destName, hostPath) pairs so the caller can install them (locally or via
// pkexec to the ESP). ESP-relative paths with no host file are left untouched.
QVector<QPair<QString,QString>> MainWindow::stageImagesTGA(const QString &dir,
        QVector<QPair<Opt<QString>*,QString>> *pending) {
    QVector<QPair<QString,QString>> out;
    QSet<QString> used;   // avoid two sources colliding on one dest name
    auto handle = [&](Opt<QString> *o){
        if (!o || !o->isSet() || o->v.isEmpty()) return;
        QFileInfo fi(o->v);
        if (!fi.exists() || !fi.isFile()) return;       // already ESP path / missing
        QString base = fi.completeBaseName(); if (base.isEmpty()) base = "img";
        QString destName = base + ".tga";
        for (int n = 2; used.contains(destName); n++) destName = base + "-" + QString::number(n) + ".tga";
        used.insert(destName);
        QString host = dir + "/" + destName;
        bool ok;
        if (fi.suffix().toLower() == "tga") {           // copy branch: overwrite stale dest
            if (QFileInfo(o->v).absoluteFilePath() == QFileInfo(host).absoluteFilePath()) ok = true;
            else { QFile::remove(host); ok = QFile::copy(o->v, host); }
        } else { QImage im(o->v); ok = !im.isNull() && writeTGA(im, host); }
        if (!ok) { statusBar()->showMessage("Could not stage " + fi.fileName()); return; }
        if (pending) pending->append({o, "/forebo/" + destName});
        else       o->assign("/forebo/" + destName);    // ESP-relative for the firmware
        out.append({destName, host});
        statusBar()->showMessage("Converted " + fi.fileName() + " -> " + destName + " (TGA)");
    };
    handle(&model->g.background);
    handle(&model->th.imgBackground); handle(&model->th.imgPanel);
    handle(&model->th.imgWindow);     handle(&model->th.imgTitlebar);
    handle(&model->th.imgButton);     handle(&model->th.imgCursor);
    handle(&model->th.cursorPath);                      // legacy cursor= sprite path
    return out;
}

// Install (dest,src) file pairs to possibly root-owned locations in ONE polkit
// prompt: back up each existing dest, then install the new file (creating dirs).
bool MainWindow::installElevated(const QVector<QPair<QString,QString>> &destSrc, QString *err) {
    if (destSrc.isEmpty()) return true;
    static const char *script =
        "while [ \"$#\" -ge 2 ]; do d=\"$1\"; s=\"$2\"; shift 2; "
        "[ -f \"$d\" ] && cp -f \"$d\" \"$d.bak\"; install -Dm644 \"$s\" \"$d\"; done";
    QStringList args = { "sh", "-c", script, "forb" };
    for (const auto &pr : destSrc) { args << pr.first << pr.second; }
    QProcess pk; pk.start("pkexec", args);
    if (!pk.waitForStarted(5000)) { if(err)*err="pkexec not available"; return false; }
    if (!pk.waitForFinished(120000)) {          // polkit prompt left unanswered
        pk.kill(); pk.waitForFinished(3000);
        if(err)*err="timed out waiting for the password prompt"; return false;
    }
    if (pk.exitStatus()!=QProcess::NormalExit || pk.exitCode()!=0) {
        if(err)*err = pk.exitCode()==126?"cancelled":("pkexec failed: "+QString::fromUtf8(pk.readAllStandardError()));
        return false;
    }
    return true;
}

// First vfat/msdos mount from /proc/mounts (world-readable), preferring a
// boot/efi path. Returns the mount point, or "" if none.
QString MainWindow::detectEspMount() {
    QFile m("/proc/mounts"); if (!m.open(QIODevice::ReadOnly)) return {};
    QString best;
    for (const QByteArray &ln : m.readAll().split('\n')) {
        QList<QByteArray> f = ln.split(' ');
        if (f.size() < 3) continue;
        if (f[2] != "vfat" && f[2] != "msdos") continue;
        QString mp = QString::fromLocal8Bit(f[1]);
        if (mp.contains("boot") || mp.contains("efi")) return mp;   // ideal ESP
        if (best.isEmpty()) best = mp;
    }
    return best;
}

void MainWindow::doSave(const QString &path) {
    QFileInfo fi(path);
    QString foreboDir = fi.absolutePath();
    bool writable = QDir(foreboDir).exists() && QFileInfo(foreboDir).isWritable();

    QTemporaryDir td;
    if (!td.isValid()) { QMessageBox::warning(this, "Save failed", "cannot create a temp dir"); return; }
    QString stageDir = writable ? foreboDir : td.path();
    // Convert -> TGA. The field rewrites (/forebo/<name>.tga) are applied BEFORE
    // serializing so the cfg references the staged files, and REVERTED if the
    // write fails/cancels - a failed apply keeps the original host paths.
    QVector<QPair<Opt<QString>*,QString>> pending;
    auto imgs = stageImagesTGA(stageDir, &pending);
    QStringList oldVals;
    for (const auto &rw : pending) oldVals << rw.first->v;
    auto applyPending  = [&]{ for (int i=0;i<pending.size();i++) pending[i].first->assign(pending[i].second); };
    auto revertPending = [&]{ for (int i=0;i<pending.size();i++) pending[i].first->assign(oldVals[i]); };
    preview->setConfigDir(foreboDir);

    applyPending();   // so serialization below emits the ESP-relative paths
    QString err;
    if (writable && model->saveFile(path, &err)) {
        currentPath = path;
        model->touch();
        statusBar()->showMessage("Saved " + path + (imgs.isEmpty()?"":QString(" (+%1 image(s) -> TGA)").arg(imgs.size())));
        return;
    }
    // Elevated: cfg + every staged image go to their ESP locations in one prompt.
    QString cfgTmp = td.path() + "/forebo.cfg";
    { QFile c(cfgTmp);
      if (!c.open(QIODevice::WriteOnly) || c.write(model->serialize().toUtf8()) < 0) {
          revertPending(); model->touch();
          QMessageBox::warning(this, "Save failed", "cannot stage the config for the elevated write"); return; } }
    QVector<QPair<QString,QString>> files; files.append({path, cfgTmp});
    for (const auto &pr : imgs) {
        QString dest = foreboDir + "/" + pr.first;
        if (QFileInfo(dest).absoluteFilePath() == QFileInfo(pr.second).absoluteFilePath())
            continue;                       // already staged in place (writable dir)
        files.append({dest, pr.second});
    }
    QString eerr;
    if (!installElevated(files, &eerr)) {
        revertPending(); model->touch();
        QMessageBox::warning(this, "Apply failed", eerr); return;
    }
    currentPath = path;
    model->touch();
    statusBar()->showMessage(QString("Applied to %1 (admin, %2 image(s))").arg(path).arg(imgs.size()));
}

// Toolbar: convert images -> TGA + write forebo.cfg to the ESP /forebo (pkexec).
void MainWindow::applyToBoot() {
    QString target = currentPath;
    if (target.isEmpty() || !(target.contains("/boot") || target.contains("/efi"))) {
        QString esp = detectEspMount();
        if (esp.isEmpty()) { QMessageBox::warning(this, "Apply to /boot",
            "No EFI System Partition mount found.\nMount your ESP (e.g. /boot/efi) or use Save As."); return; }
        target = esp + "/forebo/forebo.cfg";
    }
    if (QMessageBox::question(this, "Apply to /boot",
            "Convert images to TGA and write:\n  " + target +
            "\nto the ESP (asks for your password)?") != QMessageBox::Yes) return;
    doSave(target);
}

// Toolbar: detect + load forebo.cfg (or migrate limine.conf) from the ESP,
// reading it as root in one pkexec prompt when the ESP dir is root-only.
void MainWindow::importFromBoot() {
    QStringList cands;
    QString esp = detectEspMount();
    QStringList roots = { esp, "/boot", "/boot/efi", "/efi", "/boot/EFI" };
    for (const QString &r : roots) if (!r.isEmpty()) {
        cands << r + "/forebo/forebo.cfg" << r + "/EFI/forebo/forebo.cfg";
    }
    // limine after forebo (forebo preferred when both exist)
    QStringList lim;
    for (const QString &r : roots) if (!r.isEmpty())
        lim << r + "/limine.conf" << r + "/limine.cfg" << r + "/limine/limine.conf"
            << r + "/EFI/limine/limine.conf";
    QStringList all = cands + lim;

    // One pkexec: find the first existing candidate (as root) and cat it.
    static const char *script =
        "for c in \"$@\"; do if [ -f \"$c\" ]; then echo \"PATH:$c\"; cat \"$c\"; exit 0; fi; done; exit 3";
    QStringList args = { "sh", "-c", script, "forb" }; args += all;
    QProcess pk; pk.start("pkexec", args);
    if (!pk.waitForStarted(5000)) { QMessageBox::warning(this,"Import from /boot","pkexec not available"); return; }
    if (!pk.waitForFinished(60000)) {           // polkit prompt left unanswered
        pk.kill(); pk.waitForFinished(3000);
        QMessageBox::warning(this,"Import from /boot","Timed out waiting for the password prompt."); return;
    }
    if (pk.exitCode() == 3) { QMessageBox::information(this,"Import from /boot",
        "No forebo.cfg or limine.conf found on the ESP."); return; }
    if (pk.exitStatus()!=QProcess::NormalExit || pk.exitCode()!=0) {
        QMessageBox::warning(this,"Import from /boot", pk.exitCode()==126?"Cancelled.":
            "Read failed: "+QString::fromUtf8(pk.readAllStandardError())); return; }
    QByteArray out = pk.readAllStandardOutput();
    int nl = out.indexOf('\n');
    if (nl < 0) { QMessageBox::warning(this,"Import from /boot","Malformed readback from the ESP."); return; }
    QString srcPath = QString::fromUtf8(out.left(nl)).mid(5);   // strip "PATH:"
    QByteArray content = out.mid(nl + 1);

    QTemporaryFile tf; tf.open(); tf.write(content); tf.flush();
    QString err;
    bool isLimine = srcPath.contains("limine");
    bool ok = isLimine ? model->importLimine(tf.fileName(), &err)
                       : model->loadFile(tf.fileName(), &err);
    if (!ok) { QMessageBox::warning(this,"Import from /boot", "Parse failed: "+err); return; }
    entries->reloadFromModel();
    if (!isLimine) { currentPath = srcPath; }
    else           { // migrated: target apply back at the ESP forebo dir
        currentPath = QFileInfo(srcPath).absolutePath() + "/forebo/forebo.cfg"; }
    preview->setConfigDir(QFileInfo(currentPath).absolutePath());   // both branches
    setWindowTitle("ForeB Customizer - " + srcPath);
    statusBar()->showMessage((isLimine?"Migrated ":"Imported ") + srcPath + " from /boot");
}

// Search the usual ESP mount points (and /proc/mounts vfat entries) for an
// existing forebo.cfg and load it on startup so edits target the real config.
void MainWindow::autoLoad() {
    // Explicit override for testing / scripting: FORB_OPEN=<forebo.cfg>.
    QByteArray envp = qgetenv("FORB_OPEN");
    if (!envp.isEmpty()) {
        QString p = QString::fromLocal8Bit(envp), err;
        if (QFileInfo::exists(p) && model->loadFile(p, &err)) {
            entries->reloadFromModel();
            currentPath = p;
            preview->setConfigDir(QFileInfo(p).absolutePath());
            setWindowTitle("ForeB Customizer - " + p);
            statusBar()->showMessage("Loaded " + p);
            return;
        }
    }
    QStringList cand = {
        "/boot/forebo/forebo.cfg", "/boot/efi/forebo/forebo.cfg",
        "/efi/forebo/forebo.cfg",  "/boot/EFI/forebo/forebo.cfg",
        "/boot/efi/EFI/forebo/forebo.cfg" };
    QFile mounts("/proc/mounts");
    if (mounts.open(QIODevice::ReadOnly)) {
        for (const QByteArray &ln : mounts.readAll().split('\n')) {
            QList<QByteArray> f = ln.split(' ');
            if (f.size() >= 3 && (f[2] == "vfat" || f[2] == "msdos")) {
                QString mp = QString::fromUtf8(f[1]);
                cand << mp + "/forebo/forebo.cfg" << mp + "/EFI/forebo/forebo.cfg";
            }
        }
    }
    for (const QString &p : cand) {
        if (!QFileInfo::exists(p)) continue;
        QString err;
        if (model->loadFile(p, &err)) {
            entries->reloadFromModel();
            currentPath = p;
            preview->setConfigDir(QFileInfo(p).absolutePath());
            setWindowTitle("ForeB Customizer - " + p);
            statusBar()->showMessage("Auto-loaded " + p);
            return;
        }
    }
    statusBar()->showMessage("Ready (no forebo.cfg found on /boot; File > Open or Import)");
}
