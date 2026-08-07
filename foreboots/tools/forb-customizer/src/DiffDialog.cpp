#include "DiffDialog.h"
#include "ConfigModel.h"
#include "Schema.h"

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QTreeWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>
#include <QFont>

// ---------- helpers to extract key=value snapshots for comparison ----------
static QString optColorStr(const Opt<QRgb> &o) {
    if (!o.isSet()) return "(unset)";
    return QString("0x%1").arg((quint32)(o.v & 0x00FFFFFFu), 6, 16, QChar('0')).toUpper();
}
static QString optIntStr(const Opt<int> &o) {
    return o.isSet() ? QString::number(o.v) : "(unset)";
}
static QString optBoolStr(const Opt<bool> &o) {
    if (!o.isSet()) return "(unset)";
    return o.v ? "true" : "false";
}
static QString optStr(const Opt<QString> &o) {
    return o.isSet() ? o.v : "(unset)";
}
static QString strOrEmpty(const QString &s) {
    return s.isEmpty() ? "(unset)" : s;
}

struct KV { QString key; QString value; };

static QVector<KV> extractGlobal(const Global &g) {
    QVector<KV> kvs;
    kvs.append({"timeout", optIntStr(g.timeout)});
    kvs.append({"default", g.defaultRef.isEmpty() ? "(unset)" : g.defaultRef});
    kvs.append({"remember_last", optBoolStr(g.rememberLast)});
    kvs.append({"background", optStr(g.background)});
    return kvs;
}

static QVector<KV> extractTheme(const Theme &t) {
    QVector<KV> kvs;
    kvs.append({"theme", strOrEmpty(t.preset)});
    kvs.append({"color_bg", optColorStr(t.colorBg)});
    kvs.append({"color_fg", optColorStr(t.colorFg)});
    kvs.append({"color_accent", optColorStr(t.colorAccent)});
    kvs.append({"color_sel_bg", optColorStr(t.colorSelBg)});
    kvs.append({"color_sel_fg", optColorStr(t.colorSelFg)});
    kvs.append({"color_titlebar", optColorStr(t.colorTitlebar)});
    kvs.append({"color_window", optColorStr(t.colorWindow)});
    kvs.append({"color_cursor", optColorStr(t.colorCursor)});
    kvs.append({"cursor", optStr(t.cursorPath)});
    kvs.append({"cursor_enabled", optBoolStr(t.cursorEnabled)});
    kvs.append({"mouse_enabled", optBoolStr(t.mouseEnabled)});
    kvs.append({"animations", optBoolStr(t.animations)});
    kvs.append({"double_buffer", optBoolStr(t.doubleBuffer)});
    kvs.append({"window_skin", strOrEmpty(t.windowSkin)});
    // menu layout
    kvs.append({"menu_style", strOrEmpty(t.menuStyle)});
    kvs.append({"menu_pos", strOrEmpty(t.menuPos)});
    kvs.append({"menu_x", optIntStr(t.menuX)});
    kvs.append({"menu_y", optIntStr(t.menuY)});
    kvs.append({"menu_w", optIntStr(t.menuW)});
    kvs.append({"menu_h", optIntStr(t.menuH)});
    kvs.append({"menu_entry_h", optIntStr(t.menuEntryH)});
    kvs.append({"menu_pad", optIntStr(t.menuPad)});
    kvs.append({"menu_align", strOrEmpty(t.menuAlign)});
    kvs.append({"menu_selection", strOrEmpty(t.menuSelection)});
    kvs.append({"menu_border", strOrEmpty(t.menuBorder)});
    kvs.append({"menu_corner", strOrEmpty(t.menuCorner)});
    kvs.append({"menu_accent_strip", optBoolStr(t.menuAccentStrip)});
    kvs.append({"menu_dividers", optBoolStr(t.menuDividers)});
    kvs.append({"menu_gradient", optBoolStr(t.menuGradient)});
    kvs.append({"menu_shadow", optBoolStr(t.menuShadow)});
    kvs.append({"menu_title_bar", optBoolStr(t.menuTitleBar)});
    kvs.append({"menu_show_title", optBoolStr(t.menuShowTitle)});
    kvs.append({"menu_show_footer", optBoolStr(t.menuShowFooter)});
    kvs.append({"menu_show_timer", optBoolStr(t.menuShowTimer)});
    kvs.append({"menu_show_icons", optBoolStr(t.menuShowIcons)});
    kvs.append({"menu_icon_side", strOrEmpty(t.menuIconSide)});
    kvs.append({"menu_scrollbar", optBoolStr(t.menuScrollbar)});
    kvs.append({"menu_caret", optBoolStr(t.menuCaret)});
    // buttons / UI
    kvs.append({"btn_style", strOrEmpty(t.btnStyle)});
    kvs.append({"btn_corner", strOrEmpty(t.btnCorner)});
    kvs.append({"btn_border", optIntStr(t.btnBorder)});
    kvs.append({"btn_pad_x", optIntStr(t.btnPadX)});
    kvs.append({"btn_pad_y", optIntStr(t.btnPadY)});
    kvs.append({"btn_gradient", optBoolStr(t.btnGradient)});
    kvs.append({"btn_shadow", optBoolStr(t.btnShadow)});
    kvs.append({"btn_glow", optBoolStr(t.btnGlow)});
    kvs.append({"btn_fill", optColorStr(t.btnFill)});
    kvs.append({"btn_fill_hover", optColorStr(t.btnFillHover)});
    kvs.append({"btn_fill_active", optColorStr(t.btnFillActive)});
    kvs.append({"btn_fill_disabled", optColorStr(t.btnFillDisabled)});
    kvs.append({"btn_text", optColorStr(t.btnText)});
    kvs.append({"btn_text_hover", optColorStr(t.btnTextHover)});
    kvs.append({"btn_text_active", optColorStr(t.btnTextActive)});
    kvs.append({"btn_border_color", optColorStr(t.btnBorderColor)});
    kvs.append({"btn_focus_color", optColorStr(t.btnFocusColor)});
    kvs.append({"ui_window_corner", strOrEmpty(t.uiWindowCorner)});
    kvs.append({"ui_window_border", optIntStr(t.uiWindowBorder)});
    kvs.append({"ui_panel_alpha", optIntStr(t.uiPanelAlpha)});
    kvs.append({"ui_separator", optColorStr(t.uiSeparator)});
    kvs.append({"ui_scrollbar_w", optIntStr(t.uiScrollbarW)});
    kvs.append({"ui_scrollbar_color", optColorStr(t.uiScrollbarColor)});
    kvs.append({"ui_focus_color", optColorStr(t.uiFocusColor)});
    kvs.append({"ui_focus_width", optIntStr(t.uiFocusWidth)});
    kvs.append({"ui_font_scale", optIntStr(t.uiFontScale)});
    // effects
    kvs.append({"fx_glass", optBoolStr(t.fxGlass)});
    kvs.append({"fx_blur", optIntStr(t.fxBlur)});
    kvs.append({"fx_opacity", optIntStr(t.fxOpacity)});
    kvs.append({"fx_vignette", optIntStr(t.fxVignette)});
    kvs.append({"fx_scanlines", optIntStr(t.fxScanlines)});
    // audio
    kvs.append({"pcspeaker", optBoolStr(t.pcspeaker)});
    kvs.append({"audio_volume", optIntStr(t.audioVolume)});
    kvs.append({"audio_nav_freq", optIntStr(t.toneNav.freq)});
    kvs.append({"audio_nav_ms", optIntStr(t.toneNav.ms)});
    kvs.append({"audio_select_freq", optIntStr(t.toneSelect.freq)});
    kvs.append({"audio_select_ms", optIntStr(t.toneSelect.ms)});
    kvs.append({"audio_open_freq", optIntStr(t.toneOpen.freq)});
    kvs.append({"audio_open_ms", optIntStr(t.toneOpen.ms)});
    kvs.append({"audio_error_freq", optIntStr(t.toneError.freq)});
    kvs.append({"audio_error_ms", optIntStr(t.toneError.ms)});
    kvs.append({"audio_back_freq", optIntStr(t.toneBack.freq)});
    kvs.append({"audio_back_ms", optIntStr(t.toneBack.ms)});
    // window skin
    kvs.append({"win_title_h", optIntStr(t.winTitleH)});
    kvs.append({"win_title_fill", optColorStr(t.winTitleFill)});
    kvs.append({"win_title_fg", optColorStr(t.winTitleFg)});
    kvs.append({"win_border_color", optColorStr(t.winBorderColor)});
    kvs.append({"win_close_color", optColorStr(t.winCloseColor)});
    kvs.append({"win_border_w", optIntStr(t.winBorderW)});
    kvs.append({"win_corner", strOrEmpty(t.winCorner)});
    kvs.append({"win_button_style", strOrEmpty(t.winButtonStyle)});
    kvs.append({"win_shadow", optBoolStr(t.winShadow)});
    // images
    kvs.append({"img_background", optStr(t.imgBackground)});
    kvs.append({"img_panel", optStr(t.imgPanel)});
    kvs.append({"img_window", optStr(t.imgWindow)});
    kvs.append({"img_button", optStr(t.imgButton)});
    kvs.append({"img_titlebar", optStr(t.imgTitlebar)});
    kvs.append({"img_cursor", optStr(t.imgCursor)});
    return kvs;
}

// ---------- flatten entries to a title->EntryNode map ----------
static void collectEntries(const QVector<EntryNode> &nodes, QMap<QString, const EntryNode*> &map) {
    for (const auto &n : nodes) {
        map[n.title] = &n;
        if (n.isSubmenu) collectEntries(n.children, map);
    }
}

// ---------- apply a single setting back into ConfigModel via key/value string ----------
static void applySetting(ConfigModel *m, const QString &key, const QString &val) {
    // Re-use the same parser path the main loader uses.
    // We call globalSet which is static in ConfigModel.cpp, so we re-implement
    // a minimal version here that covers all known keys.
    Global &g = m->g; Theme &t = m->th;
    QString k = key.toLower();
    auto toBool = [](const QString &s){
        QString v = s.trimmed().toLower();
        if (v=="1"||v=="on"||v=="yes"||v=="true") return true;
        if (v=="0"||v=="off"||v=="no"||v=="false") return false;
        return s.toInt() != 0;
    };
    auto toColor = [](const QString &s) -> QRgb {
        QString v = s.trimmed();
        if (v.startsWith("#")) v = v.mid(1);
        else if (v.startsWith("0x") || v.startsWith("0X")) v = v.mid(2);
        bool ok = false;
        quint32 c = v.toUInt(&ok, 16);
        if (!ok) c = (quint32)s.toInt();
        return (QRgb)(c & 0x00FFFFFFu);
    };
    // globals
    if (k=="timeout") g.timeout.assign(val.toInt());
    else if (k=="default") g.defaultRef = val;
    else if (k=="remember_last") g.rememberLast.assign(toBool(val));
    else if (k=="background") g.background.assign(val);
    // colours
    else if (k=="color_bg") t.colorBg.assign(toColor(val));
    else if (k=="color_fg") t.colorFg.assign(toColor(val));
    else if (k=="color_accent") t.colorAccent.assign(toColor(val));
    else if (k=="color_sel_bg") t.colorSelBg.assign(toColor(val));
    else if (k=="color_sel_fg") t.colorSelFg.assign(toColor(val));
    else if (k=="color_titlebar") t.colorTitlebar.assign(toColor(val));
    else if (k=="color_window") t.colorWindow.assign(toColor(val));
    else if (k=="color_cursor") t.colorCursor.assign(toColor(val));
    else if (k=="cursor") t.cursorPath.assign(val);
    else if (k=="cursor_enabled") t.cursorEnabled.assign(toBool(val));
    else if (k=="mouse_enabled") t.mouseEnabled.assign(toBool(val));
    else if (k=="animations") t.animations.assign(toBool(val));
    else if (k=="double_buffer") t.doubleBuffer.assign(toBool(val));
    else if (k=="window_skin") t.windowSkin = val.trimmed().toLower();
    else if (k=="theme") t.preset = val.trimmed().toLower();
    // menu layout
    else if (k=="menu_style") t.menuStyle = val.trimmed().toLower();
    else if (k=="menu_pos") t.menuPos = val.trimmed().toLower();
    else if (k=="menu_x") t.menuX.assign(val.toInt());
    else if (k=="menu_y") t.menuY.assign(val.toInt());
    else if (k=="menu_w") t.menuW.assign(val.toInt());
    else if (k=="menu_h") t.menuH.assign(val.toInt());
    else if (k=="menu_entry_h") t.menuEntryH.assign(val.toInt());
    else if (k=="menu_pad") t.menuPad.assign(val.toInt());
    else if (k=="menu_align") t.menuAlign = val.trimmed().toLower();
    else if (k=="menu_selection") t.menuSelection = val.trimmed().toLower();
    else if (k=="menu_border") t.menuBorder = val.trimmed().toLower();
    else if (k=="menu_corner") t.menuCorner = val.trimmed().toLower();
    else if (k=="menu_accent_strip") t.menuAccentStrip.assign(toBool(val));
    else if (k=="menu_dividers") t.menuDividers.assign(toBool(val));
    else if (k=="menu_gradient") t.menuGradient.assign(toBool(val));
    else if (k=="menu_shadow") t.menuShadow.assign(toBool(val));
    else if (k=="menu_title_bar") t.menuTitleBar.assign(toBool(val));
    else if (k=="menu_show_title") t.menuShowTitle.assign(toBool(val));
    else if (k=="menu_show_footer") t.menuShowFooter.assign(toBool(val));
    else if (k=="menu_show_timer") t.menuShowTimer.assign(toBool(val));
    else if (k=="menu_show_icons") t.menuShowIcons.assign(toBool(val));
    else if (k=="menu_icon_side") t.menuIconSide = (val.trimmed().toLower()=="left") ? "left" : "right";
    else if (k=="menu_scrollbar") t.menuScrollbar.assign(toBool(val));
    else if (k=="menu_caret") t.menuCaret.assign(toBool(val));
    // buttons / UI
    else if (k=="btn_style") t.btnStyle = val.trimmed().toLower();
    else if (k=="btn_corner") t.btnCorner = val.trimmed().toLower();
    else if (k=="btn_border") t.btnBorder.assign(val.toInt());
    else if (k=="btn_pad_x") t.btnPadX.assign(val.toInt());
    else if (k=="btn_pad_y") t.btnPadY.assign(val.toInt());
    else if (k=="btn_gradient") t.btnGradient.assign(toBool(val));
    else if (k=="btn_shadow") t.btnShadow.assign(toBool(val));
    else if (k=="btn_glow") t.btnGlow.assign(toBool(val));
    else if (k=="btn_fill") t.btnFill.assign(toColor(val));
    else if (k=="btn_fill_hover") t.btnFillHover.assign(toColor(val));
    else if (k=="btn_fill_active") t.btnFillActive.assign(toColor(val));
    else if (k=="btn_fill_disabled") t.btnFillDisabled.assign(toColor(val));
    else if (k=="btn_text") t.btnText.assign(toColor(val));
    else if (k=="btn_text_hover") t.btnTextHover.assign(toColor(val));
    else if (k=="btn_text_active") t.btnTextActive.assign(toColor(val));
    else if (k=="btn_border_color") t.btnBorderColor.assign(toColor(val));
    else if (k=="btn_focus_color") t.btnFocusColor.assign(toColor(val));
    else if (k=="ui_window_corner") t.uiWindowCorner = val.trimmed().toLower();
    else if (k=="ui_window_border") t.uiWindowBorder.assign(val.toInt());
    else if (k=="ui_panel_alpha") t.uiPanelAlpha.assign(qBound(0, val.toInt(), 255));
    else if (k=="ui_separator") t.uiSeparator.assign(toColor(val));
    else if (k=="ui_scrollbar_w") t.uiScrollbarW.assign(val.toInt());
    else if (k=="ui_scrollbar_color") t.uiScrollbarColor.assign(toColor(val));
    else if (k=="ui_focus_color") t.uiFocusColor.assign(toColor(val));
    else if (k=="ui_focus_width") t.uiFocusWidth.assign(val.toInt());
    else if (k=="ui_font_scale") t.uiFontScale.assign(qBound(25, val.toInt(), 800));
    // effects
    else if (k=="fx_glass") t.fxGlass.assign(toBool(val));
    else if (k=="fx_blur") t.fxBlur.assign(qBound(0, val.toInt(), 32));
    else if (k=="fx_opacity") t.fxOpacity.assign(qBound(0, val.toInt(), 255));
    else if (k=="fx_vignette") t.fxVignette.assign(qBound(0, val.toInt(), 255));
    else if (k=="fx_scanlines") t.fxScanlines.assign(qBound(0, val.toInt(), 255));
    // audio
    else if (k=="pcspeaker") t.pcspeaker.assign(toBool(val));
    else if (k=="audio_volume") t.audioVolume.assign(qBound(0, val.toInt(), 100));
    else if (k=="audio_nav_freq") t.toneNav.freq.assign(val.toInt());
    else if (k=="audio_nav_ms") t.toneNav.ms.assign(val.toInt());
    else if (k=="audio_select_freq") t.toneSelect.freq.assign(val.toInt());
    else if (k=="audio_select_ms") t.toneSelect.ms.assign(val.toInt());
    else if (k=="audio_open_freq") t.toneOpen.freq.assign(val.toInt());
    else if (k=="audio_open_ms") t.toneOpen.ms.assign(val.toInt());
    else if (k=="audio_error_freq") t.toneError.freq.assign(val.toInt());
    else if (k=="audio_error_ms") t.toneError.ms.assign(val.toInt());
    else if (k=="audio_back_freq") t.toneBack.freq.assign(val.toInt());
    else if (k=="audio_back_ms") t.toneBack.ms.assign(val.toInt());
    // window skin + images
    else if (k=="win_title_h") t.winTitleH.assign(val.toInt());
    else if (k=="win_title_fill") t.winTitleFill.assign(toColor(val));
    else if (k=="win_title_fg") t.winTitleFg.assign(toColor(val));
    else if (k=="win_border_color") t.winBorderColor.assign(toColor(val));
    else if (k=="win_close_color") t.winCloseColor.assign(toColor(val));
    else if (k=="win_border_w") t.winBorderW.assign(val.toInt());
    else if (k=="win_corner") t.winCorner = val.trimmed().toLower();
    else if (k=="win_button_style") t.winButtonStyle = val.trimmed().toLower();
    else if (k=="win_shadow") t.winShadow.assign(toBool(val));
    else if (k=="img_background") t.imgBackground.assign(val);
    else if (k=="img_panel") t.imgPanel.assign(val);
    else if (k=="img_window") t.imgWindow.assign(val);
    else if (k=="img_button") t.imgButton.assign(val);
    else if (k=="img_titlebar") t.imgTitlebar.assign(val);
    else if (k=="img_cursor") t.imgCursor.assign(val);
}

// =====================================================================
DiffDialog::DiffDialog(ConfigModel *current, const QString &refPath, QWidget *parent)
    : QDialog(parent), m_current(current)
{
    setWindowTitle("Config Diff - Current vs Reference");
    resize(900, 600);

    m_reference = new ConfigModel(this);
    QString err;
    if (!m_reference->loadFile(refPath, &err)) {
        QMessageBox::warning(this, "Load failed", "Cannot load reference config:\n" + err);
        reject();
        return;
    }

    auto *top = new QVBoxLayout(this);

    // summary label
    auto *lbl = new QLabel("Comparing current config vs: " + refPath);
    lbl->setWordWrap(true);
    top->addWidget(lbl);

    // tree
    m_tree = new QTreeWidget;
    m_tree->setHeaderLabels({"Change", "Current (left)", "Reference (right)"});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::NoSelection);
    m_tree->setColumnCount(4);  // 0=checkbox state, 1=change, 2=current, 3=reference
    m_tree->setHeaderLabels({"", "Change", "Current (left)", "Reference (right)"});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_tree->header()->resizeSection(0, 30);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(3, QHeaderView::Stretch);

    computeDiff();
    populateTree();

    top->addWidget(m_tree, 1);

    // buttons
    auto *btnRow = new QHBoxLayout;
    auto *summaryLbl = new QLabel;
    int added = 0, removed = 0, modified = 0;
    for (const auto &d : m_diffs) {
        if (d.kind == DiffItem::Added) added++;
        else if (d.kind == DiffItem::Removed) removed++;
        else modified++;
    }
    summaryLbl->setText(QString("%1 added, %2 removed, %3 modified")
                        .arg(added).arg(removed).arg(modified));
    btnRow->addWidget(summaryLbl);
    btnRow->addStretch();

    auto *btnApplySel = new QPushButton("Apply Selected");
    btnApplySel->setEnabled(!m_diffs.isEmpty());
    connect(btnApplySel, &QPushButton::clicked, this, &DiffDialog::applySelected);
    btnRow->addWidget(btnApplySel);

    auto *btnApplyAll = new QPushButton("Apply All");
    btnApplyAll->setEnabled(!m_diffs.isEmpty());
    connect(btnApplyAll, &QPushButton::clicked, this, &DiffDialog::applyAll);
    btnRow->addWidget(btnApplyAll);

    auto *btnClose = new QPushButton("Close");
    connect(btnClose, &QPushButton::clicked, this, &QDialog::accept);
    btnRow->addWidget(btnClose);

    top->addLayout(btnRow);
}

QColor DiffDialog::colorForKind(DiffItem::Kind kind) const {
    switch (kind) {
    case DiffItem::Added:     return QColor(40, 167, 69);   // green
    case DiffItem::Removed:   return QColor(220, 53, 69);   // red
    case DiffItem::Modified:  return QColor(255, 193, 7);   // yellow/amber
    }
    return Qt::white;
}

void DiffDialog::highlightItem(QTreeWidgetItem *item, DiffItem::Kind kind) {
    QColor c = colorForKind(kind);
    for (int col = 0; col < 4; ++col)
        item->setBackground(col, c);
    // dark text on yellow, white on green/red
    QColor txt = (kind == DiffItem::Modified) ? Qt::black : Qt::white;
    for (int col = 0; col < 4; ++col)
        item->setForeground(col, txt);
}

void DiffDialog::computeDiff() {
    computeEntryDiff();
    computeGlobalDiff();
    computeThemeDiff();
}

void DiffDialog::computeEntryDiff() {
    QMap<QString, const EntryNode*> curMap, refMap;
    collectEntries(m_current->roots, curMap);
    collectEntries(m_reference->roots, refMap);

    // removed: in current but not reference
    for (auto it = curMap.constBegin(); it != curMap.constEnd(); ++it) {
        if (!refMap.contains(it.key())) {
            DiffItem d;
            d.kind = DiffItem::Removed;
            d.label = it.key();
            d.category = "Entry";
            d.leftValue = it.value()->type;
            d.rightValue = "-";
            d.checked = false; // removing from current not typical
            m_diffs.append(d);
        }
    }
    // added: in reference but not current
    for (auto it = refMap.constBegin(); it != refMap.constEnd(); ++it) {
        if (!curMap.contains(it.key())) {
            DiffItem d;
            d.kind = DiffItem::Added;
            d.label = it.key();
            d.category = "Entry";
            d.leftValue = "-";
            d.rightValue = it.value()->type;
            m_diffs.append(d);
        }
    }
    // modified: in both, but different
    for (auto it = refMap.constBegin(); it != refMap.constEnd(); ++it) {
        if (curMap.contains(it.key())) {
            const EntryNode *c = curMap[it.key()];
            const EntryNode *r = it.value();
            if (*c != *r) {
                DiffItem d;
                d.kind = DiffItem::Modified;
                d.label = it.key();
                d.category = "Entry";
                d.leftValue = c->type + (c->kernel.isEmpty() ? "" : " kernel=" + c->kernel)
                            + (c->vmlinuz.isEmpty() ? "" : " vmlinuz=" + c->vmlinuz);
                d.rightValue = r->type + (r->kernel.isEmpty() ? "" : " kernel=" + r->kernel)
                             + (r->vmlinuz.isEmpty() ? "" : " vmlinuz=" + r->vmlinuz);
                d.detail = "type/icon/kernel/vmlinuz/initrd/cmdline fields differ";
                m_diffs.append(d);
            }
        }
    }
}

void DiffDialog::computeGlobalDiff() {
    auto curKVs = extractGlobal(m_current->g);
    auto refKVs = extractGlobal(m_reference->g);
    QMap<QString, QString> curMap, refMap;
    for (const auto &kv : curKVs) curMap[kv.key] = kv.value;
    for (const auto &kv : refKVs) refMap[kv.key] = kv.value;

    for (const auto &kv : refKVs) {
        QString cur = curMap.value(kv.key, "(unset)");
        if (cur != kv.value) {
            DiffItem d;
            d.kind = (cur == "(unset)") ? DiffItem::Added : DiffItem::Modified;
            d.label = kv.key;
            d.category = "Global";
            d.leftValue = cur;
            d.rightValue = kv.value;
            m_diffs.append(d);
        }
    }
}

void DiffDialog::computeThemeDiff() {
    auto curKVs = extractTheme(m_current->th);
    auto refKVs = extractTheme(m_reference->th);
    QMap<QString, QString> curMap, refMap;
    for (const auto &kv : curKVs) curMap[kv.key] = kv.value;
    for (const auto &kv : refKVs) refMap[kv.key] = kv.value;

    for (const auto &kv : refKVs) {
        QString cur = curMap.value(kv.key, "(unset)");
        if (cur != kv.value) {
            DiffItem d;
            d.kind = (cur == "(unset)") ? DiffItem::Added : DiffItem::Modified;
            d.label = kv.key;
            d.category = "Theme";
            d.leftValue = cur;
            d.rightValue = kv.value;
            m_diffs.append(d);
        }
    }
}

void DiffDialog::populateTree() {
    m_tree->clear();
    for (int i = 0; i < m_diffs.size(); ++i) {
        const DiffItem &d = m_diffs[i];
        auto *item = new QTreeWidgetItem(m_tree);
        QString kindStr;
        switch (d.kind) {
        case DiffItem::Added:    kindStr = "+ " + d.category + ": " + d.label; break;
        case DiffItem::Removed:  kindStr = "- " + d.category + ": " + d.label; break;
        case DiffItem::Modified: kindStr = "~ " + d.category + ": " + d.label; break;
        }
        item->setText(0, d.checked ? "\u2611" : "\u2610");  // ballot box
        item->setText(1, kindStr);
        item->setText(2, d.leftValue);
        item->setText(3, d.rightValue);
        if (!d.detail.isEmpty())
            item->setToolTip(1, d.detail);
        highlightItem(item, d.kind);
        item->setData(0, Qt::UserRole, i);
    }
}

void DiffDialog::toggleCheckState(QTreeWidgetItem *item, int col) {
    Q_UNUSED(col);
    int idx = item->data(0, Qt::UserRole).toInt();
    if (idx < 0 || idx >= m_diffs.size()) return;
    m_diffs[idx].checked = !m_diffs[idx].checked;
    item->setText(0, m_diffs[idx].checked ? "\u2611" : "\u2610");
}

void DiffDialog::applySelected() {
    int count = 0;
    for (int i = 0; i < m_diffs.size(); ++i) {
        const DiffItem &d = m_diffs[i];
        if (!d.checked) continue;
        if (d.category == "Entry") {
            if (d.kind == DiffItem::Added) {
                // copy entry from reference
                QMap<QString, const EntryNode*> refMap;
                collectEntries(m_reference->roots, refMap);
                if (refMap.contains(d.label)) {
                    // append a copy to current roots
                    EntryNode copy = *refMap[d.label];
                    m_current->roots.append(copy);
                    count++;
                }
            } else if (d.kind == DiffItem::Modified) {
                // replace matching entry in current with reference version
                QMap<QString, const EntryNode*> refMap;
                collectEntries(m_reference->roots, refMap);
                for (int j = 0; j < m_current->roots.size(); ++j) {
                    if (m_current->roots[j].title == d.label) {
                        m_current->roots[j] = *refMap[d.label];
                        count++;
                        break;
                    }
                }
            }
            // Removed entries: we skip removing from current (user can do that manually)
        } else {
            // Global or Theme setting
            applySetting(m_current, d.label, d.rightValue);
            count++;
        }
    }
    if (count > 0) {
        m_current->touchStructure();
        m_current->requestRefresh();
        QMessageBox::information(this, "Applied", QString("%1 change(s) applied.").arg(count));
        accept();
    } else {
        QMessageBox::information(this, "Nothing to apply", "No changes were selected.");
    }
}

void DiffDialog::applyAll() {
    for (auto &d : m_diffs) d.checked = true;
    populateTree();
    applySelected();
}
