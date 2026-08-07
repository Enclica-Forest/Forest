#include "ConfigModel.h"
#include "Schema.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QSaveFile>
#include <QRegularExpression>
#include <QTextStream>

// ===========================================================================
//  helpers
// ===========================================================================
static QString colorHex(QRgb c) {
    return QString("0x%1").arg((quint32)(c & 0x00FFFFFFu), 6, 16, QChar('0')).toUpper()
               .replace("0X", "0x");
}
// The firmware's icon= resolver (uefi/config.c icon_resolve): a value with a
// path separator or a .tga/.bmp suffix is a literal path; a bare name becomes
// /forebo/icons/<name>.tga. Mirrored so a parsed-then-emitted file is stable.
static QString iconResolve(const QString &val) {
    if (val.isEmpty()) return val;
    if (val.contains('/') || val.contains('\\')) return val;
    QString low = val.toLower();
    if (low.endsWith(".tga") || low.endsWith(".bmp")) return val;
    return "/forebo/icons/" + val + ".tga";
}
// Match tools/forebo-install sanitize_title(): '/'->U+2215, '"'->'\''.
void ConfigModel::sanitizeTitleInPlace(QString &t) {
    t.replace('/', QChar(0x2215)).replace('"', '\'');
}
// Canonicalize a type= token to the string we store/emit.
static QString canonType(const QString &v) {
    QString s = v.trimmed().toLower();
    if (s == "chain") return "chainload";
    if (s == "firmware") return "setup";
    if (s == "theme") return "settings";
    static const QStringList known = Schema::entryTypes();
    return known.contains(s) ? s : "forest";
}
static bool toBool(const QString &s) {
    QString v = s.trimmed().toLower();
    if (v=="1"||v=="on"||v=="yes"||v=="true") return true;
    if (v=="0"||v=="off"||v=="no"||v=="false") return false;
    return s.toInt() != 0;
}
static QRgb toColor(const QString &s) {
    QString v = s.trimmed();
    if (v.startsWith("#")) v = v.mid(1);
    else if (v.startsWith("0x") || v.startsWith("0X")) v = v.mid(2);
    bool ok = false;
    quint32 c = v.toUInt(&ok, 16);
    if (!ok) c = (quint32)s.toInt();
    return (QRgb)(c & 0x00FFFFFFu);
}

// ===========================================================================
//  Theme equality (round-trip test relies on this)
// ===========================================================================
bool Theme::operator==(const Theme &o) const {
    return preset==o.preset &&
        colorBg==o.colorBg && colorFg==o.colorFg && colorAccent==o.colorAccent &&
        colorSelBg==o.colorSelBg && colorSelFg==o.colorSelFg &&
        colorTitlebar==o.colorTitlebar && colorWindow==o.colorWindow &&
        colorCursor==o.colorCursor &&
        cursorPath==o.cursorPath && cursorEnabled==o.cursorEnabled &&
        mouseEnabled==o.mouseEnabled && animations==o.animations &&
        doubleBuffer==o.doubleBuffer && windowSkin==o.windowSkin &&
        menuStyle==o.menuStyle && menuPos==o.menuPos && menuAlign==o.menuAlign &&
        menuSelection==o.menuSelection && menuBorder==o.menuBorder &&
        menuCorner==o.menuCorner && menuIconSide==o.menuIconSide &&
        menuX==o.menuX && menuY==o.menuY && menuW==o.menuW && menuH==o.menuH &&
        menuEntryH==o.menuEntryH && menuPad==o.menuPad &&
        menuAccentStrip==o.menuAccentStrip && menuDividers==o.menuDividers &&
        menuGradient==o.menuGradient && menuShadow==o.menuShadow &&
        menuTitleBar==o.menuTitleBar && menuShowTitle==o.menuShowTitle &&
        menuShowFooter==o.menuShowFooter && menuShowTimer==o.menuShowTimer &&
        menuShowIcons==o.menuShowIcons && menuScrollbar==o.menuScrollbar &&
        menuCaret==o.menuCaret &&
        btnStyle==o.btnStyle && btnCorner==o.btnCorner && uiWindowCorner==o.uiWindowCorner &&
        btnBorder==o.btnBorder && btnPadX==o.btnPadX && btnPadY==o.btnPadY &&
        uiWindowBorder==o.uiWindowBorder && uiPanelAlpha==o.uiPanelAlpha &&
        uiScrollbarW==o.uiScrollbarW && uiFocusWidth==o.uiFocusWidth &&
        uiFontScale==o.uiFontScale &&
        btnGradient==o.btnGradient && btnShadow==o.btnShadow && btnGlow==o.btnGlow &&
        btnFill==o.btnFill && btnFillHover==o.btnFillHover && btnFillActive==o.btnFillActive &&
        btnFillDisabled==o.btnFillDisabled && btnText==o.btnText &&
        btnTextHover==o.btnTextHover && btnTextActive==o.btnTextActive &&
        btnBorderColor==o.btnBorderColor && btnFocusColor==o.btnFocusColor &&
        uiSeparator==o.uiSeparator && uiScrollbarColor==o.uiScrollbarColor &&
        uiFocusColor==o.uiFocusColor &&
        fxGlass==o.fxGlass && fxBlur==o.fxBlur && fxOpacity==o.fxOpacity &&
        fxVignette==o.fxVignette && fxScanlines==o.fxScanlines &&
        pcspeaker==o.pcspeaker && audioVolume==o.audioVolume &&
        toneNav==o.toneNav && toneSelect==o.toneSelect && toneOpen==o.toneOpen &&
        toneError==o.toneError && toneBack==o.toneBack &&
        winTitleH==o.winTitleH && winTitleFill==o.winTitleFill && winTitleFg==o.winTitleFg &&
        winBorderColor==o.winBorderColor && winCloseColor==o.winCloseColor &&
        winBorderW==o.winBorderW && winCorner==o.winCorner &&
        winButtonStyle==o.winButtonStyle && winShadow==o.winShadow &&
        imgBackground==o.imgBackground && imgPanel==o.imgPanel &&
        imgWindow==o.imgWindow && imgButton==o.imgButton &&
        imgTitlebar==o.imgTitlebar && imgCursor==o.imgCursor;
}

// ===========================================================================
ConfigModel::ConfigModel(QObject *parent) : QObject(parent) { resetDefaults(); }

void ConfigModel::resetDefaults() {
    g = Global();
    th = Theme();
    roots.clear();
}

// ===========================================================================
//  Serializer  (ConfigModel -> forebo.cfg text)
// ===========================================================================
static void emitEntry(const EntryNode &e, const QString &ind, QStringList &out);

static void emitNodes(const QVector<EntryNode> &nodes, const QString &ind, QStringList &out) {
    for (const auto &n : nodes) {
        if (n.isSubmenu) {
            out << QString("%1submenu \"%2\" {").arg(ind, n.title);
            if (!n.icon.isEmpty()) out << QString("%1    icon=%2").arg(ind, n.icon);
            emitNodes(n.children, ind + "    ", out);
            if (!out.isEmpty() && out.last().isEmpty()) out.removeLast();
            out << ind + "}";
            out << "";
        } else {
            emitEntry(n, ind, out);
        }
    }
}

static void emitEntry(const EntryNode &e, const QString &ind, QStringList &out) {
    out << QString("%1menuentry \"%2\" {").arg(ind, e.title);
    out << QString("%1    type=%2").arg(ind, e.type);
    if (e.type == "forest") {
        if (!e.kernel.isEmpty()) out << QString("%1    kernel=%2").arg(ind, e.kernel);
        for (const auto &m : e.modules) out << QString("%1    module=%2").arg(ind, m);
    } else if (e.type == "linux") {
        if (!e.vmlinuz.isEmpty()) out << QString("%1    vmlinuz=%2").arg(ind, e.vmlinuz);
        if (!e.initrd.isEmpty())  out << QString("%1    initrd=%2").arg(ind, e.initrd);
    } else if (e.type == "chainload") {
        if (!e.chain.isEmpty())   out << QString("%1    chain=%2").arg(ind, e.chain);
    }
    if (e.type == "forest" || e.type == "linux" || !e.cmdline.isEmpty())
        out << QString("%1    cmdline=\"%2\"").arg(ind, e.cmdline);
    if (!e.background.isEmpty()) out << QString("%1    background=%2").arg(ind, e.background);
    if (!e.icon.isEmpty())      out << QString("%1    icon=%2").arg(ind, e.icon);
    out << ind + "}";
    out << "";
}

QString ConfigModel::serialize() const {
    QStringList o;
    o << "# " + QString(74, '=');
    o << "#  forebo.cfg - generated by forb-customizer";
    o << "#  Generated     : " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    o << "#  Unknown keys are ignored by the ForeB firmware parser.";
    o << "# " + QString(74, '=');
    o << "";

    // ---- globals ----
    if (g.timeout.isSet())      o << QString("timeout=%1").arg(g.timeout.v);
    if (!g.defaultRef.isEmpty())o << QString("default=%1").arg(g.defaultRef);
    if (g.rememberLast.isSet()) o << QString("remember_last=%1").arg(g.rememberLast.v ? 1 : 0);
    if (g.background.isSet())   o << QString("background=%1").arg(g.background.v);
    o << "";

    // ---- theme / colours ----
    if (!th.preset.isEmpty())        o << QString("theme=%1").arg(th.preset);
    #define C(key, field) if (th.field.isSet()) o << QString(key "=%1").arg(colorHex(th.field.v));
    C("color_bg", colorBg) C("color_fg", colorFg) C("color_accent", colorAccent)
    C("color_sel_bg", colorSelBg) C("color_sel_fg", colorSelFg)
    C("color_titlebar", colorTitlebar) C("color_window", colorWindow) C("color_cursor", colorCursor)
    #undef C
    if (th.cursorPath.isSet())    o << QString("cursor=%1").arg(th.cursorPath.v);
    #define B(key, field) if (th.field.isSet()) o << QString(key "=%1").arg(th.field.v ? 1 : 0);
    B("cursor_enabled", cursorEnabled) B("mouse_enabled", mouseEnabled)
    B("animations", animations) B("double_buffer", doubleBuffer)
    #undef B
    if (!th.windowSkin.isEmpty()) o << QString("window_skin=%1").arg(th.windowSkin);

    // ---- menu layout ----
    o << "";
    if (!th.menuStyle.isEmpty())     o << QString("menu_style=%1").arg(th.menuStyle);
    if (!th.menuPos.isEmpty())       o << QString("menu_pos=%1").arg(th.menuPos);
    #define I(key, field) if (th.field.isSet()) o << QString(key "=%1").arg(th.field.v);
    I("menu_x", menuX) I("menu_y", menuY) I("menu_w", menuW) I("menu_h", menuH)
    I("menu_entry_h", menuEntryH) I("menu_pad", menuPad)
    #undef I
    if (!th.menuAlign.isEmpty())     o << QString("menu_align=%1").arg(th.menuAlign);
    if (!th.menuSelection.isEmpty()) o << QString("menu_selection=%1").arg(th.menuSelection);
    if (!th.menuBorder.isEmpty())    o << QString("menu_border=%1").arg(th.menuBorder);
    if (!th.menuCorner.isEmpty())    o << QString("menu_corner=%1").arg(th.menuCorner);
    #define B(key, field) if (th.field.isSet()) o << QString(key "=%1").arg(th.field.v ? 1 : 0);
    B("menu_accent_strip", menuAccentStrip) B("menu_dividers", menuDividers)
    B("menu_gradient", menuGradient) B("menu_shadow", menuShadow)
    B("menu_title_bar", menuTitleBar) B("menu_show_title", menuShowTitle)
    B("menu_show_footer", menuShowFooter) B("menu_show_timer", menuShowTimer)
    B("menu_show_icons", menuShowIcons) B("menu_scrollbar", menuScrollbar)
    B("menu_caret", menuCaret)
    #undef B
    if (!th.menuIconSide.isEmpty())  o << QString("menu_icon_side=%1").arg(th.menuIconSide);

    // ---- buttons / UI ----
    o << "";
    if (!th.btnStyle.isEmpty())      o << QString("btn_style=%1").arg(th.btnStyle);
    if (!th.btnCorner.isEmpty())     o << QString("btn_corner=%1").arg(th.btnCorner);
    #define I(key, field) if (th.field.isSet()) o << QString(key "=%1").arg(th.field.v);
    I("btn_border", btnBorder) I("btn_pad_x", btnPadX) I("btn_pad_y", btnPadY)
    #undef I
    #define B(key, field) if (th.field.isSet()) o << QString(key "=%1").arg(th.field.v ? 1 : 0);
    B("btn_gradient", btnGradient) B("btn_shadow", btnShadow) B("btn_glow", btnGlow)
    #undef B
    #define C(key, field) if (th.field.isSet()) o << QString(key "=%1").arg(colorHex(th.field.v));
    C("btn_fill", btnFill) C("btn_fill_hover", btnFillHover) C("btn_fill_active", btnFillActive)
    C("btn_fill_disabled", btnFillDisabled) C("btn_text", btnText)
    C("btn_text_hover", btnTextHover) C("btn_text_active", btnTextActive)
    C("btn_border_color", btnBorderColor) C("btn_focus_color", btnFocusColor)
    #undef C
    if (!th.uiWindowCorner.isEmpty()) o << QString("ui_window_corner=%1").arg(th.uiWindowCorner);
    #define I(key, field) if (th.field.isSet()) o << QString(key "=%1").arg(th.field.v);
    I("ui_window_border", uiWindowBorder) I("ui_panel_alpha", uiPanelAlpha)
    I("ui_scrollbar_w", uiScrollbarW) I("ui_focus_width", uiFocusWidth)
    I("ui_font_scale", uiFontScale)
    #undef I
    #define C(key, field) if (th.field.isSet()) o << QString(key "=%1").arg(colorHex(th.field.v));
    C("ui_separator", uiSeparator) C("ui_scrollbar_color", uiScrollbarColor)
    C("ui_focus_color", uiFocusColor)
    #undef C

    // ---- effects ----
    o << "";
    #define B(key, field) if (th.field.isSet()) o << QString(key "=%1").arg(th.field.v ? 1 : 0);
    B("fx_glass", fxGlass)
    #undef B
    #define I(key, field) if (th.field.isSet()) o << QString(key "=%1").arg(th.field.v);
    I("fx_blur", fxBlur) I("fx_opacity", fxOpacity) I("fx_vignette", fxVignette)
    I("fx_scanlines", fxScanlines)
    #undef I

    // ---- audio ----
    o << "";
    if (th.pcspeaker.isSet())   o << QString("pcspeaker=%1").arg(th.pcspeaker.v ? 1 : 0);
    if (th.audioVolume.isSet()) o << QString("audio_volume=%1").arg(th.audioVolume.v);
    struct { const char *fk, *mk; const Tone *t; } tones[] = {
        {"audio_nav_freq","audio_nav_ms",&th.toneNav},
        {"audio_select_freq","audio_select_ms",&th.toneSelect},
        {"audio_open_freq","audio_open_ms",&th.toneOpen},
        {"audio_error_freq","audio_error_ms",&th.toneError},
        {"audio_back_freq","audio_back_ms",&th.toneBack},
    };
    for (auto &tt : tones) {
        if (tt.t->freq.isSet()) o << QString("%1=%2").arg(tt.fk).arg(tt.t->freq.v);
        if (tt.t->ms.isSet())   o << QString("%1=%2").arg(tt.mk).arg(tt.t->ms.v);
    }

    // ---- Track-3 window skin + images ----
    o << "";
    #define I(key, field) if (th.field.isSet()) o << QString(key "=%1").arg(th.field.v);
    I("win_title_h", winTitleH) I("win_border_w", winBorderW)
    #undef I
    #define C(key, field) if (th.field.isSet()) o << QString(key "=%1").arg(colorHex(th.field.v));
    C("win_title_fill", winTitleFill) C("win_title_fg", winTitleFg)
    C("win_border_color", winBorderColor) C("win_close_color", winCloseColor)
    #undef C
    if (!th.winCorner.isEmpty())      o << QString("win_corner=%1").arg(th.winCorner);
    if (!th.winButtonStyle.isEmpty()) o << QString("win_button_style=%1").arg(th.winButtonStyle);
    if (th.winShadow.isSet())         o << QString("win_shadow=%1").arg(th.winShadow.v ? 1 : 0);
    #define S(key, field) if (th.field.isSet()) o << QString(key "=%1").arg(th.field.v);
    S("img_background", imgBackground) S("img_panel", imgPanel) S("img_window", imgWindow)
    S("img_button", imgButton) S("img_titlebar", imgTitlebar) S("img_cursor", imgCursor)
    #undef S

    // ---- entries ----
    o << "";
    emitNodes(roots, "", o);

    QString text = o.join("\n");
    while (text.endsWith("\n")) text.chop(1);
    return text + "\n";
}

// ===========================================================================
//  Reader  (forebo.cfg text -> ConfigModel). Tokenizer mirrors uefi/config.c.
// ===========================================================================
namespace {
enum { TOK_EOF=0, TOK_WORD, TOK_STRING, TOK_EQ, TOK_LBRACE, TOK_RBRACE };
struct Lexer {
    const QString &s; int p; int n;
    Lexer(const QString &str) : s(str), p(0), n(str.size()) {}
    static bool space(QChar c){ return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\v'||c=='\f'; }
    static bool wordEnd(QChar c){ return space(c)||c=='='||c=='{'||c=='}'||c=='"'||c=='#'; }
    int next(QString &out) {
        out.clear();
        for (;;) {
            while (p<n && space(s[p])) p++;
            if (p>=n) return TOK_EOF;
            if (s[p]=='#') { while (p<n && s[p]!='\n') p++; continue; }
            break;
        }
        QChar c = s[p];
        if (c=='=') { p++; return TOK_EQ; }
        if (c=='{') { p++; return TOK_LBRACE; }
        if (c=='}') { p++; return TOK_RBRACE; }
        if (c=='"') {
            p++;
            while (p<n && s[p]!='"' && s[p]!='\n') out.append(s[p++]);
            if (p<n && s[p]=='"') p++;
            return TOK_STRING;
        }
        while (p<n && !wordEnd(s[p])) out.append(s[p++]);
        return TOK_WORD;
    }
    void skipLine(){ while (p<n && s[p]!='\n') p++; }
};
} // namespace

// Assign one per-entry key inside a menuentry block (mirrors entry_set()).
static void entrySet(EntryNode &e, const QString &key, const QString &val) {
    QString k = key.toLower();
    if (k=="type")          e.type = canonType(val);
    else if (k=="kernel")   { e.kernel = val; if (val.toLower()=="reboot") e.type = "reboot"; }
    else if (k=="vmlinuz")  { e.vmlinuz = val; if (e.type=="forest") e.type = "linux"; }
    else if (k=="initrd")   e.initrd = val;
    else if (k=="chain")    { e.chain = val; if (e.type=="forest") e.type = "chainload"; }
    else if (k=="module" || k=="module2") { if (!val.isEmpty() && e.modules.size()<8) e.modules << val; }
    else if (k=="cmdline")  e.cmdline = val;
    else if (k=="icon")     e.icon = iconResolve(val);
    else if (k=="background") e.background = val;
}

// Assign one global/theme key (mirrors theme_set/style_set/widget_set/audio_set
// plus the Track-3 win_/img_ keys). Returns nothing; unknown keys are ignored.
static void globalSet(ConfigModel *m, const QString &key, const QString &val) {
    QString k = key.toLower();
    Global &g = m->g; Theme &t = m->th;
    // globals
    if      (k=="timeout")       g.timeout.assign(val.toInt());
    else if (k=="default")       g.defaultRef = val;
    else if (k=="remember_last") g.rememberLast.assign(toBool(val));
    else if (k=="background")    g.background.assign(val);
    // colours
    else if (k=="color_bg")       t.colorBg.assign(toColor(val));
    else if (k=="color_fg")       t.colorFg.assign(toColor(val));
    else if (k=="color_accent")   t.colorAccent.assign(toColor(val));
    else if (k=="color_sel_bg")   t.colorSelBg.assign(toColor(val));
    else if (k=="color_sel_fg")   t.colorSelFg.assign(toColor(val));
    else if (k=="color_titlebar") t.colorTitlebar.assign(toColor(val));
    else if (k=="color_window")   t.colorWindow.assign(toColor(val));
    else if (k=="color_cursor")   t.colorCursor.assign(toColor(val));
    else if (k=="cursor")         t.cursorPath.assign(val);
    else if (k=="cursor_enabled") t.cursorEnabled.assign(toBool(val));
    else if (k=="mouse_enabled")  t.mouseEnabled.assign(toBool(val));
    else if (k=="animations")     t.animations.assign(toBool(val));
    else if (k=="double_buffer")  t.doubleBuffer.assign(toBool(val));
    else if (k=="window_skin")    t.windowSkin = val.trimmed().toLower();
    else if (k=="theme")          t.preset = val.trimmed().toLower();
    // menu layout
    else if (k=="menu_style")     t.menuStyle = val.trimmed().toLower();
    else if (k=="menu_pos")       t.menuPos = val.trimmed().toLower();
    else if (k=="menu_x")         t.menuX.assign(val.toInt());
    else if (k=="menu_y")         t.menuY.assign(val.toInt());
    else if (k=="menu_w")         t.menuW.assign(val.toInt());
    else if (k=="menu_h")         t.menuH.assign(val.toInt());
    else if (k=="menu_entry_h")   t.menuEntryH.assign(val.toInt());
    else if (k=="menu_pad")       t.menuPad.assign(val.toInt());
    else if (k=="menu_align")     t.menuAlign = val.trimmed().toLower();
    else if (k=="menu_selection") t.menuSelection = val.trimmed().toLower();
    else if (k=="menu_border")    t.menuBorder = val.trimmed().toLower();
    else if (k=="menu_corner")    t.menuCorner = val.trimmed().toLower();
    else if (k=="menu_accent_strip") t.menuAccentStrip.assign(toBool(val));
    else if (k=="menu_dividers")  t.menuDividers.assign(toBool(val));
    else if (k=="menu_gradient")  t.menuGradient.assign(toBool(val));
    else if (k=="menu_shadow")    t.menuShadow.assign(toBool(val));
    else if (k=="menu_title_bar") t.menuTitleBar.assign(toBool(val));
    else if (k=="menu_show_title")  t.menuShowTitle.assign(toBool(val));
    else if (k=="menu_show_footer") t.menuShowFooter.assign(toBool(val));
    else if (k=="menu_show_timer")  t.menuShowTimer.assign(toBool(val));
    else if (k=="menu_show_icons")  t.menuShowIcons.assign(toBool(val));
    else if (k=="menu_icon_side")   t.menuIconSide = (val.trimmed().toLower()=="left") ? "left" : "right";
    else if (k=="menu_scrollbar")   t.menuScrollbar.assign(toBool(val));
    else if (k=="menu_caret")       t.menuCaret.assign(toBool(val));
    // buttons / UI
    else if (k=="btn_style")       t.btnStyle = val.trimmed().toLower();
    else if (k=="btn_corner")      t.btnCorner = val.trimmed().toLower();
    else if (k=="btn_border")      t.btnBorder.assign(val.toInt());
    else if (k=="btn_pad_x")       t.btnPadX.assign(val.toInt());
    else if (k=="btn_pad_y")       t.btnPadY.assign(val.toInt());
    else if (k=="btn_gradient")    t.btnGradient.assign(toBool(val));
    else if (k=="btn_shadow")      t.btnShadow.assign(toBool(val));
    else if (k=="btn_glow")        t.btnGlow.assign(toBool(val));
    else if (k=="btn_fill")        t.btnFill.assign(toColor(val));
    else if (k=="btn_fill_hover")  t.btnFillHover.assign(toColor(val));
    else if (k=="btn_fill_active") t.btnFillActive.assign(toColor(val));
    else if (k=="btn_fill_disabled") t.btnFillDisabled.assign(toColor(val));
    else if (k=="btn_text")        t.btnText.assign(toColor(val));
    else if (k=="btn_text_hover")  t.btnTextHover.assign(toColor(val));
    else if (k=="btn_text_active") t.btnTextActive.assign(toColor(val));
    else if (k=="btn_border_color")t.btnBorderColor.assign(toColor(val));
    else if (k=="btn_focus_color") t.btnFocusColor.assign(toColor(val));
    else if (k=="ui_window_corner")t.uiWindowCorner = val.trimmed().toLower();
    else if (k=="ui_window_border")t.uiWindowBorder.assign(val.toInt());
    else if (k=="ui_panel_alpha")  t.uiPanelAlpha.assign(qBound(0, val.toInt(), 255));
    else if (k=="ui_separator")    t.uiSeparator.assign(toColor(val));
    else if (k=="ui_scrollbar_w")  t.uiScrollbarW.assign(val.toInt());
    else if (k=="ui_scrollbar_color") t.uiScrollbarColor.assign(toColor(val));
    else if (k=="ui_focus_color")  t.uiFocusColor.assign(toColor(val));
    else if (k=="ui_focus_width")  t.uiFocusWidth.assign(val.toInt());
    else if (k=="ui_font_scale")   t.uiFontScale.assign(qBound(25, val.toInt(), 800));
    // effects
    else if (k=="fx_glass")     t.fxGlass.assign(toBool(val));
    else if (k=="fx_blur")      t.fxBlur.assign(qBound(0, val.toInt(), 32));
    else if (k=="fx_opacity")   t.fxOpacity.assign(qBound(0, val.toInt(), 255));
    else if (k=="fx_vignette")  t.fxVignette.assign(qBound(0, val.toInt(), 255));
    else if (k=="fx_scanlines") t.fxScanlines.assign(qBound(0, val.toInt(), 255));
    // audio
    else if (k=="pcspeaker")    t.pcspeaker.assign(toBool(val));
    else if (k=="audio_volume") t.audioVolume.assign(qBound(0, val.toInt(), 100));
    else if (k=="audio_nav_freq")    t.toneNav.freq.assign(val.toInt());
    else if (k=="audio_nav_ms")      t.toneNav.ms.assign(val.toInt());
    else if (k=="audio_select_freq") t.toneSelect.freq.assign(val.toInt());
    else if (k=="audio_select_ms")   t.toneSelect.ms.assign(val.toInt());
    else if (k=="audio_open_freq")   t.toneOpen.freq.assign(val.toInt());
    else if (k=="audio_open_ms")     t.toneOpen.ms.assign(val.toInt());
    else if (k=="audio_error_freq")  t.toneError.freq.assign(val.toInt());
    else if (k=="audio_error_ms")    t.toneError.ms.assign(val.toInt());
    else if (k=="audio_back_freq")   t.toneBack.freq.assign(val.toInt());
    else if (k=="audio_back_ms")     t.toneBack.ms.assign(val.toInt());
    // Track-3 window skin + images
    else if (k=="win_title_h")     t.winTitleH.assign(val.toInt());
    else if (k=="win_title_fill")  t.winTitleFill.assign(toColor(val));
    else if (k=="win_title_fg")    t.winTitleFg.assign(toColor(val));
    else if (k=="win_border_color")t.winBorderColor.assign(toColor(val));
    else if (k=="win_close_color") t.winCloseColor.assign(toColor(val));
    else if (k=="win_border_w")    t.winBorderW.assign(val.toInt());
    else if (k=="win_corner")      t.winCorner = val.trimmed().toLower();
    else if (k=="win_button_style")t.winButtonStyle = val.trimmed().toLower();
    else if (k=="win_shadow")      t.winShadow.assign(toBool(val));
    else if (k=="img_background")  t.imgBackground.assign(val);
    else if (k=="img_panel")       t.imgPanel.assign(val);
    else if (k=="img_window")      t.imgWindow.assign(val);
    else if (k=="img_button")      t.imgButton.assign(val);
    else if (k=="img_titlebar")    t.imgTitlebar.assign(val);
    else if (k=="img_cursor")      t.imgCursor.assign(val);
    // unknown -> ignored
}

// A parse frame: pointer to the vector we append rows to, plus submenu flag.
struct Frame { QVector<EntryNode> *bucket; EntryNode *node; bool isSubmenu; };

bool ConfigModel::parseText(const QString &text) {
    resetDefaults();
    Lexer lx(text);
    QString tok, val;

    // Stack of open blocks. Because EntryNode children live inside their parent
    // by value, we build submenu subtrees on the heap first, then splice them in
    // on close so pointers stay stable while a block is open.
    struct Open { EntryNode node; bool isSubmenu; int parentIdx; };
    QVector<Open> stack;

    auto commit = [&](Open blk) {
        // Attach a finished block to its parent (or roots).
        if (blk.parentIdx < 0) roots.append(blk.node);
        else stack[blk.parentIdx].node.children.append(blk.node);
    };

    for (;;) {
        int t = lx.next(tok);
        if (t == TOK_EOF) break;

        if (t == TOK_RBRACE) {
            if (!stack.isEmpty()) { Open b = stack.takeLast(); commit(b); }
            continue;
        }

        if (t == TOK_WORD && (tok.compare("menuentry",Qt::CaseInsensitive)==0 ||
                              tok.compare("submenu",Qt::CaseInsensitive)==0)) {
            bool wantSub = (tok.compare("submenu",Qt::CaseInsensitive)==0);
            // block openers invalid inside a menuentry
            if (!stack.isEmpty() && !stack.last().isSubmenu) { lx.skipLine(); continue; }

            QString title;
            int tt = lx.next(title);
            if (tt == TOK_EOF) break;
            Open blk; blk.isSubmenu = wantSub; blk.parentIdx = stack.size()-1;
            blk.node = EntryNode();
            blk.node.isSubmenu = wantSub;
            if (tt == TOK_STRING || tt == TOK_WORD) blk.node.title = title;

            if (tt != TOK_LBRACE) {
                int tb = lx.next(val);
                if (tb == TOK_EOF) break;
                if (tb != TOK_LBRACE) { // malformed header - resync to a '}'
                    for (;;) { int ts = lx.next(val); if (ts==TOK_EOF||ts==TOK_RBRACE) break; }
                    continue;
                }
            }
            stack.append(blk);
            continue;
        }

        if (t == TOK_WORD) {
            int te = lx.next(val);
            if (te == TOK_EOF) break;
            if (te == TOK_RBRACE) { if (!stack.isEmpty()){Open b=stack.takeLast();commit(b);} continue; }
            if (te != TOK_EQ) continue;
            int tv = lx.next(val);
            if (tv == TOK_EOF) break;
            if (tv == TOK_RBRACE) { if (!stack.isEmpty()){Open b=stack.takeLast();commit(b);} continue; }
            if (tv != TOK_WORD && tv != TOK_STRING) continue;

            if (!stack.isEmpty()) {
                Open &top = stack.last();
                if (!top.isSubmenu) entrySet(top.node, tok, val);
                else if (tok.compare("icon",Qt::CaseInsensitive)==0) top.node.icon = iconResolve(val);
            } else {
                globalSet(this, tok, val);
            }
            continue;
        }
    }
    // close any still-open blocks (tolerant, like the firmware)
    while (!stack.isEmpty()) { Open b = stack.takeLast(); commit(b); }
    return true;
}

// ===========================================================================
//  File I/O
// ===========================================================================
bool ConfigModel::loadFile(const QString &path, QString *err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (err) *err = f.errorString(); return false; }
    QByteArray data = f.readAll();
    QString text = QString::fromUtf8(data);
    text.replace("\r\n", "\n").replace("\r", "\n");
    parseText(text);
    requestRefresh();
    return true;
}

bool ConfigModel::saveFile(const QString &path, QString *err) {
    // Timestamped .bak of an existing file, then atomic write.
    QFileInfo fi(path);
    if (fi.exists()) {
        QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
        QString bak = path + "." + stamp + ".bak";
        QFile::remove(bak);
        if (!QFile::copy(path, bak)) { if (err) *err = "could not create backup " + bak; return false; }
    }
    QSaveFile sf(path);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = sf.errorString(); return false;
    }
    sf.write(serialize().toUtf8());
    if (!sf.commit()) { if (err) *err = sf.errorString(); return false; }
    return true;
}

// ===========================================================================
//  Template save / load
// ===========================================================================

// Template header marker used to identify template files.
static const char *kTemplateHeader = "# ForeB Template:";

bool ConfigModel::saveTemplate(const QString &path, const QString &name,
                               const QString &description, const QString &author) {
    QStringList o;
    // header block
    QString descLine = description.isEmpty() ? name : name + " - " + description;
    o << QString("%1 %2").arg(kTemplateHeader, descLine);
    if (!author.isEmpty()) o << "# Author: " + author;
    o << "# Date: " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    o << "";
    o << serialize();
    QString text = o.join("\n");

    QSaveFile sf(path);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    sf.write(text.toUtf8());
    return sf.commit();
}

bool ConfigModel::loadTemplate(const QString &path, bool themeOnly, QString *err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (err) *err = f.errorString(); return false; }
    QString text = QString::fromUtf8(f.readAll());
    text.replace("\r\n", "\n").replace("\r", "\n");

    // Strip template header lines (lines starting with # before the first
    // non-comment, non-empty line).
    QStringList lines = text.split('\n');
    int start = 0;
    for (int i = 0; i < lines.size(); ++i) {
        QString l = lines[i].trimmed();
        if (l.isEmpty()) { start = i + 1; continue; }
        if (l.startsWith('#')) { start = i + 1; continue; }
        break;
    }
    QString cfgText = lines.mid(start).join('\n');

    if (themeOnly) {
        // Save globals + entries, apply only theme keys from the template.
        Global savedG = g;
        QVector<EntryNode> savedRoots = roots;
        // Parse the template fully into a temp model state, then copy only theme.
        Theme savedTh = th;
        resetDefaults();
        parseText(cfgText);
        Theme templTh = th;
        // Restore globals and entries, apply template theme.
        g = savedG;
        roots = savedRoots;
        th = templTh;
    } else {
        parseText(cfgText);
    }
    requestRefresh();
    return true;
}

// ===========================================================================
//  Limine importer (ports tools/forebo-install parse_limine/translate).
//  ESP-relative resolution only: guid()/uuid() schemes are treated as the ESP.
// ===========================================================================
static QString limineStripPath(QString v) {
    // strip a leading scheme():  boot():/x -> /x ; guid(..):/y -> /y
    QRegularExpression scheme("^(\\w+)\\(([^)]*)\\):(.*)$");
    auto mm = scheme.match(v);
    if (mm.hasMatch()) v = mm.captured(3);
    // strip a trailing #<hex> content hash (>=32 hex chars)
    QRegularExpression hash("#[0-9a-fA-F]{32,}$");
    v.remove(hash);
    v = v.trimmed().replace('\\', '/');
    if (!v.startsWith('/')) v = "/" + v;
    return v;
}
// installer guess_icon keyword table
static QString guessIcon(const QString &text, const QString &type) {
    static const QVector<QPair<QString,QString>> kw = {
        {"windows boot","windows"},{"efi fallback","usb"},{"windows","windows"},
        {"grub","grub"},{"ubuntu","ubuntu"},{"debian","debian"},{"fedora","fedora"},
        {"mint","mint"},{"arch","arch"},{"cachyos","arch"},{"endeavour","arch"},
        {"manjaro","arch"},{"snapshot","safe"},{"fallback","safe"},{"safe","safe"},
        {"usb","usb"},{"removable","usb"},{"shell","terminal"},
    };
    QString hay = text.toLower();
    for (auto &p : kw) if (hay.contains(p.first)) return p.second;
    if (type=="linux") return "tux";
    if (type=="chainload") return "usb";
    if (type=="forest") return "os";
    return "";
}

namespace {
struct LNode {
    QString name; int depth=0;
    QMap<QString,QString> keys;
    QStringList modulePaths;
    QVector<LNode*> children;
    bool isEntry() const {
        return keys.contains("protocol")||keys.contains("path")||
               keys.contains("kernel_path")||keys.contains("image_path");
    }
};
}

bool ConfigModel::importLimine(const QString &path, QString *err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (err) *err = f.errorString(); return false; }
    QString text = QString::fromUtf8(f.readAll());
    text.replace("\r\n","\n").replace("\r","\n");

    QVector<LNode*> roots_;
    QVector<LNode*> stack;
    QVector<LNode*> all;   // for cleanup
    QMap<QString,QString> globals;

    const QStringList lines = text.split('\n');
    QRegularExpression header("^(/+)\\+?(.*)$");
    QRegularExpression kvre("^([A-Za-z0-9_]+)\\s*:\\s*(.*)$");
    for (const QString &raw : lines) {
        QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (line.startsWith('/')) {
            auto m = header.match(line);
            if (!m.hasMatch()) continue;
            int depth = m.captured(1).size();
            LNode *node = new LNode; all.append(node);
            node->name = m.captured(2).trimmed();
            node->depth = depth;
            while (!stack.isEmpty() && stack.last()->depth >= depth) stack.removeLast();
            while (!stack.isEmpty() && stack.last()->isEntry()) stack.removeLast();
            if (!stack.isEmpty()) stack.last()->children.append(node);
            else roots_.append(node);
            stack.append(node);
            continue;
        }
        auto m = kvre.match(line);
        if (!m.hasMatch()) continue;
        QString key = m.captured(1).toLower(), value = m.captured(2).trimmed();
        if (key == "comment") continue;
        if (!stack.isEmpty()) {
            LNode *node = stack.last();
            if (key == "module_path") node->modulePaths.append(value);
            else node->keys[key] = value;
        } else globals[key] = value;
    }

    resetDefaults();
    // globals
    if (globals.contains("timeout")) g.timeout.assign((int)qRound(globals["timeout"].toDouble()));
    if (globals.contains("default_entry")) g.defaultRef = QString::number(globals["default_entry"].toInt());
    if (globals.contains("remember_last_entry")) {
        QString v = globals["remember_last_entry"].toLower();
        g.rememberLast.assign(v=="yes"||v=="1"||v=="true"||v=="on");
    }
    if (globals.contains("wallpaper") && !globals["wallpaper"].isEmpty())
        g.background.assign(limineStripPath(globals["wallpaper"]));

    std::function<EntryNode*(LNode*, QVector<EntryNode>&)> translate =
        [&](LNode *node, QVector<EntryNode> &bucket) -> EntryNode* {
        if (!node->isEntry()) {
            EntryNode grp; grp.isSubmenu = true;
            grp.title = node->name; sanitizeTitleInPlace(grp.title);
            grp.icon = guessIcon(node->name, "");
            for (LNode *c : node->children) translate(c, grp.children);
            if (grp.children.isEmpty()) return nullptr;
            bucket.append(grp);
            return &bucket.last();
        }
        QString proto = node->keys.value("protocol").trimmed().toLower();
        QString title = node->name; sanitizeTitleInPlace(title);
        QString cmd = node->keys.value("cmdline", node->keys.value("kernel_cmdline")).trimmed();
        EntryNode e; e.title = title; e.cmdline = cmd;
        auto rp = [&](const QString &v){ return limineStripPath(v); };
        QStringList mods; for (const QString &m : node->modulePaths) mods << rp(m);

        bool ok = true;
        if (proto == "linux") {
            QString kp = node->keys.value("kernel_path", node->keys.value("path"));
            if (kp.isEmpty()) ok=false;
            else { e.type="linux"; e.vmlinuz=rp(kp); if(!mods.isEmpty()) e.initrd=mods.first(); }
        } else if (proto=="multiboot"||proto=="multiboot1"||proto=="multiboot2") {
            QString kp = node->keys.value("kernel_path", node->keys.value("path"));
            if (kp.isEmpty()) ok=false;
            else { e.type="forest"; e.kernel=rp(kp); e.modules=mods; }
        } else if (proto=="efi_chainload") {
            QString img = node->keys.value("image_path", node->keys.value("path"));
            if (img.isEmpty()) ok=false; else { e.type="chainload"; e.chain=rp(img); }
        } else if (proto=="efi"||proto=="limine") {
            QString p = node->keys.value("path");
            if (p.isEmpty()) ok=false; else { e.type="chainload"; e.chain=rp(p); }
        } else if (proto=="chainload") {
            ok=false; // legacy BIOS sector chainload unsupported
        } else {
            QString img = node->keys.value("image_path");
            QString p = node->keys.value("path");
            if (!img.isEmpty() || (!p.isEmpty() && p.toLower().endsWith(".efi")))
                { e.type="chainload"; e.chain=rp(img.isEmpty()?p:img); }
            else if (!p.isEmpty()) { e.type="linux"; e.vmlinuz=rp(p); if(!mods.isEmpty()) e.initrd=mods.first(); }
            else ok=false;
        }
        if (!ok) return nullptr;
        QString base = e.vmlinuz.isEmpty() ? (e.kernel.isEmpty()?e.chain:e.kernel) : e.vmlinuz;
        e.icon = iconResolve(guessIcon(node->name + " " + QFileInfo(base).fileName(), e.type));
        bucket.append(e);
        return &bucket.last();
    };

    for (LNode *r : roots_) translate(r, roots);
    qDeleteAll(all);
    requestRefresh();
    return true;
}

// ===========================================================================
//  Syslinux importer (ports tools/forebo-install parse_syslinux/translate).
// ===========================================================================
static QString guessSyslinuxIcon(const QString &text, const QString &type) {
    static const QMap<QString,QString> &m = []{
        static QMap<QString,QString> mm; if (!mm.isEmpty()) return mm;
        mm["windows"]="windows"; mm["win"]="windows"; mm["grub"]="grub";
        mm["ubuntu"]="ubuntu"; mm["debian"]="debian"; mm["fedora"]="fedora";
        mm["mint"]="mint"; mm["arch"]="arch"; mm["cachyos"]="arch";
        mm["endeavour"]="arch"; mm["manjaro"]="arch"; mm["safe"]="safe";
        mm["fallback"]="safe"; mm["snapshot"]="safe"; mm["usb"]="usb";
        mm["removable"]="usb"; mm["shell"]="terminal"; mm["recovery"]="shield";
        mm["memtest"]="gear"; mm["hd"]="disk";
        return mm;
    }();
    QString hay = text.toLower();
    for (auto it = m.constBegin(); it != m.constEnd(); ++it)
        if (hay.contains(it.key())) return it.value();
    if (type == "linux") return "tux";
    if (type == "chainload") return "usb";
    if (type == "forest") return "os";
    return "";
}

bool ConfigModel::importSyslinux(const QString &path, QString *err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (err) *err = f.errorString(); return false; }
    QString text = QString::fromUtf8(f.readAll());
    text.replace("\r\n", "\n").replace("\r", "\n");

    resetDefaults();
    const QStringList lines = text.split('\n');
    int globalTimeout = -1;
    QString globalDefault;

    // Current LABEL block state
    struct Label {
        QString name, menuLabel, kernel, append, initrd, localboot;
        bool hasLocalboot = false;
        int localbootIndex = -1;
    };
    QVector<Label> labels;
    Label cur;
    bool inLabel = false;

    auto flush = [&]{
        if (!inLabel) return;
        inLabel = false;
        if (cur.name.isEmpty() && cur.kernel.isEmpty() && !cur.hasLocalboot) return;
        labels.append(cur);
        cur = Label();
    };

    QRegularExpression reKeyValue("^([A-Za-z_][A-Za-z0-9_]*)\\s+(.*)$");

    for (const QString &raw : lines) {
        QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';')) continue;

        // LABEL keyword (syslinux uses "LABEL name" or "label name")
        QString low = line.toLower();
        if (low.startsWith("label ")) {
            flush();
            inLabel = true;
            cur.name = line.mid(6).trimmed();
            continue;
        }
        if (low == "label") {
            flush();
            inLabel = true;
            continue;
        }
        if (low.startsWith("label\t")) {
            flush();
            inLabel = true;
            cur.name = line.mid(6).trimmed();
            continue;
        }

        // Global keywords
        if (!inLabel) {
            if (low.startsWith("timeout ")) {
                // syslinux TIMEOUT is in 10ths of seconds
                bool ok; int t = line.mid(8).trimmed().toInt(&ok);
                if (ok) globalTimeout = t / 10;  // convert to whole seconds
            } else if (low.startsWith("default ")) {
                globalDefault = line.mid(8).trimmed();
            } else if (low.startsWith("ui ") || low.startsWith(" vesamenu") || low.startsWith(" gfxboot")) {
                // UI directives — skip
            }
            continue;
        }

        // Inside a LABEL block
        auto m = reKeyValue.match(line);
        if (m.hasMatch()) {
            QString key = m.captured(1).toLower();
            QString val = m.captured(2).trimmed();
            if (key == "kernel")       cur.kernel = val;
            else if (key == "append")  cur.append = val;
            else if (key == "initrd")  cur.initrd = val;
            else if (key == "menu" && val.toLower().startsWith("label ")) {
                cur.menuLabel = val.mid(6).trimmed();
            } else if (key == "menu" && val.toLower().startsWith("label\t")) {
                cur.menuLabel = val.mid(6).trimmed();
            } else if (key == "localboot") {
                cur.hasLocalboot = true;
                cur.localbootIndex = val.toInt();
            }
        } else if (line.toLower().startsWith("menu ")) {
            // "MENU LABEL xxx" handled above; "MENU COLOR", "MENU WIDTH", etc. — skip
            QString rest = line.mid(4).trimmed();
            if (rest.toLower().startsWith("label ")) {
                cur.menuLabel = rest.mid(6).trimmed();
            }
        }
    }
    flush();

    // Apply globals
    if (globalTimeout >= 0) g.timeout.assign(globalTimeout);

    int defaultIdx = globalDefault.isEmpty() ? -1 : -1;
    // Find default by label name
    if (!globalDefault.isEmpty()) {
        for (int i = 0; i < labels.size(); i++) {
            if (labels[i].name.toLower() == globalDefault.toLower()) {
                defaultIdx = i; break;
            }
        }
    }

    // Convert labels to EntryNodes
    auto translateEntry = [](const Label &lb) -> EntryNode {
        EntryNode e;
        QString title = lb.menuLabel.isEmpty() ? lb.name : lb.menuLabel;
        sanitizeTitleInPlace(title);
        e.title = title;

        if (lb.hasLocalboot) {
            e.type = "chainload";
            e.chain = QString("LOCALBOOT %1").arg(lb.localbootIndex);
            e.icon = iconResolve(guessSyslinuxIcon(title, "chainload"));
        } else {
            QString k = lb.kernel.toLower();
            // Strip leading / from kernel path (syslinux paths are relative to config dir)
            QString kernelPath = lb.kernel;
            if (!kernelPath.startsWith('/')) kernelPath = "/" + kernelPath;

            if (k.endsWith(".efi")) {
                e.type = "chainload";
                e.chain = kernelPath;
                e.icon = iconResolve(guessSyslinuxIcon(title + " " + QFileInfo(kernelPath).fileName(), "chainload"));
            } else {
                e.type = "linux";
                e.vmlinuz = kernelPath;
                // INITRD can be inline in APPEND or separate INITRD directive
                if (!lb.initrd.isEmpty()) {
                    QString initrdPath = lb.initrd.trimmed();
                    if (!initrdPath.startsWith('/')) initrdPath = "/" + initrdPath;
                    e.initrd = initrdPath;
                }
                // APPEND carries the kernel cmdline (and sometimes initrd= as a param)
                if (!lb.append.isEmpty()) {
                    QString cmdline = lb.append;
                    // Extract initrd= from append if no explicit INITRD was given
                    if (lb.initrd.isEmpty()) {
                        QRegularExpression initrdRe("\\binitrd=(\\S+)");
                        auto im = initrdRe.match(cmdline);
                        if (im.hasMatch()) {
                            QString ip = im.captured(1);
                            if (!ip.startsWith('/')) ip = "/" + ip;
                            e.initrd = ip;
                            cmdline.remove(im.capturedStart(), im.capturedLength());
                        }
                    }
                    // Extract root= from append (not meaningful to ForeB)
                    QRegularExpression rootRe("\\broot=\\S+");
                    cmdline.remove(rootRe);
                    e.cmdline = cmdline.trimmed();
                }
                e.icon = iconResolve(guessSyslinuxIcon(title + " " + QFileInfo(kernelPath).fileName(), "linux"));
            }
        }
        return e;
    };

    for (const auto &lb : labels) {
        roots.append(translateEntry(lb));
    }

    if (defaultIdx >= 0 && defaultIdx < roots.size()) {
        g.defaultRef = QString::number(defaultIdx);
    }

    requestRefresh();
    return true;
}

// ===========================================================================
//  rEFInd importer (ports tools/forebo-install parse_refind/translate).
// ===========================================================================
static QString guessRefindIcon(const QString &text, const QString &type) {
    static const QMap<QString,QString> &m = []{
        static QMap<QString,QString> mm; if (!mm.isEmpty()) return mm;
        mm["windows"]="windows"; mm["win"]="windows"; mm["grub"]="grub";
        mm["ubuntu"]="ubuntu"; mm["debian"]="debian"; mm["fedora"]="fedora";
        mm["mint"]="mint"; mm["arch"]="arch"; mm["cachyos"]="arch";
        mm["endeavour"]="arch"; mm["manjaro"]="arch"; mm["safe"]="safe";
        mm["fallback"]="safe"; mm["snapshot"]="safe"; mm["usb"]="usb";
        mm["removable"]="usb"; mm["shell"]="terminal"; mm["recovery"]="shield";
        mm["memtest"]="gear"; mm["hd"]="disk"; mm["mac"]="os"; mm["linux"]="tux";
        mm["tux"]="tux";
        return mm;
    }();
    QString hay = text.toLower();
    for (auto it = m.constBegin(); it != m.constEnd(); ++it)
        if (hay.contains(it.key())) return it.value();
    if (type == "linux") return "tux";
    if (type == "chainload") return "usb";
    if (type == "forest") return "os";
    return "";
}

bool ConfigModel::importRefind(const QString &path, QString *err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (err) *err = f.errorString(); return false; }
    QString text = QString::fromUtf8(f.readAll());
    text.replace("\r\n", "\n").replace("\r", "\n");

    resetDefaults();
    const QStringList lines = text.split('\n');

    // rEFInd directives at top level (not inside menuentry)
    int globalTimeout = -1;
    QString globalDefault;
    bool scanForEfi = true;

    struct RefindEntry {
        QString title;
        QString loader, icon, options, initrd;
        QVector<RefindEntry> submenu;
    };
    QVector<RefindEntry> entries;
    RefindEntry cur;
    bool inEntry = false;
    int entryDepth = 0;  // track submenuentry nesting

    auto flush = [&]{
        if (!inEntry) return;
        inEntry = false;
        if (cur.title.isEmpty() && cur.loader.isEmpty()) return;
        entries.append(cur);
        cur = RefindEntry();
    };

    QRegularExpression keyVal("^([A-Za-z_]\\w*)\\s+(.*)$");
    QRegularExpression menuEntry("^menuentry\\s+\"([^\"]*)\"\\s*\\{(.*)$",
                                  QRegularExpression::CaseInsensitiveOption);
    QRegularExpression menuEntrySingle("^menuentry\\s+(\\S+)\\s*\\{(.*)$",
                                        QRegularExpression::CaseInsensitiveOption);
    QRegularExpression subEntry("^submenuentry\\s+\"([^\"]*)\"\\s*\\{(.*)$",
                                 QRegularExpression::CaseInsensitiveOption);
    QRegularExpression subEntrySingle("^submenuentry\\s+(\\S+)\\s*\\{(.*)$",
                                       QRegularExpression::CaseInsensitiveOption);

    for (const QString &raw : lines) {
        QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;

        // Check for menuentry open
        auto me = menuEntry.match(line);
        auto me2 = (!me.hasMatch()) ? menuEntrySingle.match(line) : QRegularExpressionMatch();
        if (me.hasMatch() || me2.hasMatch()) {
            flush();
            inEntry = true;
            cur.title = me.hasMatch() ? me.captured(1) : me2.captured(1);
            // Inline "}" on the same line?
            QString rest = me.hasMatch() ? me.captured(2) : me2.captured(2);
            rest = rest.trimmed();
            if (rest == "}") { flush(); continue; }
            continue;
        }

        // submenuentry
        auto se = subEntry.match(line);
        auto se2 = (!se.hasMatch()) ? subEntrySingle.match(line) : QRegularExpressionMatch();
        if (se.hasMatch() || se2.hasMatch()) {
            // We don't nest deeper than one level; treat submenuentry as a child
            RefindEntry sub;
            sub.title = se.hasMatch() ? se.captured(1) : se2.captured(1);
            QString rest = (se.hasMatch() ? se.captured(2) : se2.captured(2)).trimmed();
            if (rest == "}") {
                if (inEntry) cur.submenu.append(sub);
                continue;
            }
            // Parse submenu body inline
            bool inSub = true;
            for (int i = lines.indexOf(raw) + 1; i < lines.size() && inSub; i++) {
                QString sl = lines[i].trimmed();
                if (sl == "}") { inSub = false; break; }
                auto skm = keyVal.match(sl);
                if (skm.hasMatch()) {
                    QString k = skm.captured(1).toLower();
                    QString v = skm.captured(2).trimmed();
                    // Strip quotes from value
                    if (v.startsWith('"') && v.endsWith('"') && v.size() >= 2)
                        v = v.mid(1, v.size() - 2);
                    if (k == "loader") sub.loader = v;
                    else if (k == "icon") sub.icon = v;
                    else if (k == "options") sub.options = v;
                    else if (k == "initrd") sub.initrd = v;
                    else if (k == "title") sub.title = v;  // rEFInd also allows title=
                }
            }
            if (inEntry) cur.submenu.append(sub);
            continue;
        }

        // Closing brace
        if (line == "}") {
            flush();
            continue;
        }

        // Top-level keywords
        if (!inEntry) {
            QString low = line.toLower();
            if (low.startsWith("timeout ")) {
                bool ok; int t = line.mid(8).trimmed().toInt(&ok);
                if (ok) globalTimeout = t;
            } else if (low.startsWith("default_selection ")) {
                globalDefault = line.mid(17).trimmed();
            } else if (low.startsWith("scanfor ")) {
                scanForEfi = line.mid(8).trimmed().toLower().contains("efi");
            } else if (low.startsWith("scan_manual ")) {
                // skip
            }
            continue;
        }

        // Inside a menuentry block: key value
        auto kv = keyVal.match(line);
        if (kv.hasMatch()) {
            QString key = kv.captured(1).toLower();
            QString val = kv.captured(2).trimmed();
            // Strip quotes
            if (val.startsWith('"') && val.endsWith('"') && val.size() >= 2)
                val = val.mid(1, val.size() - 2);
            if (key == "loader")   cur.loader = val;
            else if (key == "icon")    cur.icon = val;
            else if (key == "options") cur.options = val;
            else if (key == "initrd")  cur.initrd = val;
            else if (key == "title")   cur.title = val;
        }
    }
    flush();

    // Apply globals
    if (globalTimeout >= 0) g.timeout.assign(globalTimeout);

    // Convert rEFInd entries to ForeB EntryNodes
    for (const auto &re : entries) {
        EntryNode e;
        e.title = re.title;
        sanitizeTitleInPlace(e.title);

        QString loaderLow = re.loader.toLower();

        if (re.loader.isEmpty()) {
            // No loader — skip this entry
            continue;
        }

        if (loaderLow.endsWith(".efi")) {
            e.type = "chainload";
            QString loaderPath = re.loader;
            if (!loaderPath.startsWith('/')) loaderPath = "/" + loaderPath;
            e.chain = loaderPath;
            e.icon = iconResolve(re.icon.isEmpty()
                ? guessRefindIcon(e.title + " " + QFileInfo(loaderPath).fileName(), "chainload")
                : re.icon);
        } else if (loaderLow.contains("vmlinuz") || loaderLow.contains("bzimage") ||
                   loaderLow.contains("vmlinux")) {
            e.type = "linux";
            QString loaderPath = re.loader;
            if (!loaderPath.startsWith('/')) loaderPath = "/" + loaderPath;
            e.vmlinuz = loaderPath;
            if (!re.initrd.isEmpty()) {
                QString initrdPath = re.initrd;
                if (!initrdPath.startsWith('/')) initrdPath = "/" + initrdPath;
                e.initrd = initrdPath;
            }
            e.cmdline = re.options;
            e.icon = iconResolve(re.icon.isEmpty()
                ? guessRefindIcon(e.title + " " + QFileInfo(loaderPath).fileName(), "linux")
                : re.icon);
        } else {
            // Unknown loader — chainload it
            e.type = "chainload";
            QString loaderPath = re.loader;
            if (!loaderPath.startsWith('/')) loaderPath = "/" + loaderPath;
            e.chain = loaderPath;
            e.icon = iconResolve(re.icon.isEmpty()
                ? guessRefindIcon(e.title, "chainload")
                : re.icon);
        }

        // Append submenu entries as children
        if (!re.submenu.isEmpty()) {
            e.isSubmenu = false;  // entries with submenus stay flat in ForeB
            // rEFInd submenuentries are alternative boot options; add them after
        }

        roots.append(e);

        // Add submenuentry children as separate entries (ForeB doesn't nest rEFInd submenus)
        for (const auto &sub : re.submenu) {
            EntryNode se;
            se.title = sub.title;
            sanitizeTitleInPlace(se.title);
            if (sub.loader.endsWith(".efi", Qt::CaseInsensitive)) {
                se.type = "chainload";
                QString lp = sub.loader;
                if (!lp.startsWith('/')) lp = "/" + lp;
                se.chain = lp;
            } else if (sub.loader.contains("vmlinuz", Qt::CaseInsensitive) ||
                       sub.loader.contains("bzimage", Qt::CaseInsensitive)) {
                se.type = "linux";
                QString lp = sub.loader;
                if (!lp.startsWith('/')) lp = "/" + lp;
                se.vmlinuz = lp;
                if (!sub.initrd.isEmpty()) {
                    QString ip = sub.initrd;
                    if (!ip.startsWith('/')) ip = "/" + ip;
                    se.initrd = ip;
                }
                se.cmdline = sub.options;
            } else if (!sub.loader.isEmpty()) {
                se.type = "chainload";
                QString lp = sub.loader;
                if (!lp.startsWith('/')) lp = "/" + lp;
                se.chain = lp;
            } else {
                continue;  // no loader, skip
            }
            se.icon = iconResolve(sub.icon.isEmpty()
                ? guessRefindIcon(se.title, se.type)
                : sub.icon);
            roots.append(se);
        }
    }

    if (!globalDefault.isEmpty() && !roots.isEmpty()) {
        // Try matching by index or title
        bool ok;
        int idx = globalDefault.toInt(&ok);
        if (ok && idx >= 0 && idx < roots.size()) {
            g.defaultRef = QString::number(idx);
        } else {
            // Match by title
            for (int i = 0; i < roots.size(); i++) {
                if (roots[i].title.toLower() == globalDefault.toLower()) {
                    g.defaultRef = QString::number(i);
                    break;
                }
            }
        }
    }

    requestRefresh();
    return true;
}

// ===========================================================================
//  flatten (for the default= combo + preview list)
// ===========================================================================
static void flattenRec(const QVector<EntryNode> &nodes, const QString &prefix,
                       QVector<ConfigModel::FlatRef> &out) {
    for (const auto &n : nodes) {
        QString label = prefix.isEmpty() ? n.title : prefix + "/" + n.title;
        out.append({label, &n});
        if (n.isSubmenu) flattenRec(n.children, label, out);
    }
}
QVector<ConfigModel::FlatRef> ConfigModel::flatten() const {
    QVector<FlatRef> out;
    flattenRec(roots, QString(), out);
    return out;
}

// ===========================================================================
//  Entry validation
// ===========================================================================
static bool isValidEspPath(const QString &p) {
    if (p.isEmpty()) return true;
    if (!p.startsWith('/')) return false;
    if (p.contains("//")) return false;
    if (p.contains('\\')) return false;
    return true;
}

static bool isValidIconName(const QString &icon) {
    if (icon.isEmpty()) return true;
    if (icon.startsWith('/')) return true;
    if (icon.contains('/')) return true;
    return Schema::iconNames().contains(icon.toLower());
}

QVector<EntryValidation> ConfigModel::validateEntry(const EntryNode &e) {
    QVector<EntryValidation> r;
    static const QStringList VALID_TYPES = Schema::entryTypes();

    // 1. Title length <= 63 chars
    if (e.title.size() > 63)
        r.append({EntryValidation::Error, "title",
                  QString("title is %1 chars (firmware max 63)").arg(e.title.size())});

    // 2. Valid type
    if (!e.type.isEmpty() && !VALID_TYPES.contains(e.type))
        r.append({EntryValidation::Warning, "type",
                  QString("unknown type '%1'").arg(e.type)});

    // 3. Type-specific path checks
    if (e.type == "forest") {
        if (e.kernel.isEmpty())
            r.append({EntryValidation::Warning, "kernel",
                      "forest entry has no kernel path"});
        if (!e.kernel.isEmpty() && !isValidEspPath(e.kernel))
            r.append({EntryValidation::Warning, "kernel",
                      QString("kernel path '%1' has invalid format").arg(e.kernel)});
        if (e.kernel.size() > 255)
            r.append({EntryValidation::Warning, "kernel",
                      QString("kernel path is %1 chars (firmware max 255)").arg(e.kernel.size())});
        for (int i = 0; i < e.modules.size(); ++i) {
            const QString &m = e.modules[i];
            if (m.size() > 255)
                r.append({EntryValidation::Warning, "modules",
                          QString("module[%1] path is %2 chars (firmware max 255)").arg(i).arg(m.size())});
            if (!isValidEspPath(m))
                r.append({EntryValidation::Warning, "modules",
                          QString("module[%1] path '%2' has invalid format").arg(i).arg(m)});
        }
    } else if (e.type == "linux") {
        if (e.vmlinuz.isEmpty())
            r.append({EntryValidation::Warning, "vmlinuz",
                      "linux entry has no vmlinuz path"});
        if (!e.vmlinuz.isEmpty() && !isValidEspPath(e.vmlinuz))
            r.append({EntryValidation::Warning, "vmlinuz",
                      QString("vmlinuz path '%1' has invalid format").arg(e.vmlinuz)});
        if (e.vmlinuz.size() > 255)
            r.append({EntryValidation::Warning, "vmlinuz",
                      QString("vmlinuz path is %1 chars (firmware max 255)").arg(e.vmlinuz.size())});
        if (!e.initrd.isEmpty() && !isValidEspPath(e.initrd))
            r.append({EntryValidation::Warning, "initrd",
                      QString("initrd path '%1' has invalid format").arg(e.initrd)});
        if (e.initrd.size() > 255)
            r.append({EntryValidation::Warning, "initrd",
                      QString("initrd path is %1 chars (firmware max 255)").arg(e.initrd.size())});
    } else if (e.type == "chainload") {
        if (!e.chain.isEmpty() && !isValidEspPath(e.chain))
            r.append({EntryValidation::Warning, "chain",
                      QString("chain path '%1' has invalid format").arg(e.chain)});
        if (e.chain.size() > 255)
            r.append({EntryValidation::Warning, "chain",
                      QString("chain path is %1 chars (firmware max 255)").arg(e.chain.size())});
    }

    // 4. Cmdline length <= 255 chars
    if (e.cmdline.size() > 255)
        r.append({EntryValidation::Warning, "cmdline",
                  QString("cmdline is %1 chars (firmware max 255)").arg(e.cmdline.size())});

    // 5. Icon name is valid
    if (!isValidIconName(e.icon))
        r.append({EntryValidation::Info, "icon",
                  QString("unknown icon '%1' (not in icon table)").arg(e.icon)});

    return r;
}

QVector<EntryValidation> ConfigModel::validateAll() const {
    QVector<EntryValidation> all;
    // Collect all flattened entries for duplicate detection
    QVector<const EntryNode*> flatEntries;
    std::function<void(const QVector<EntryNode>&)> collect =
        [&](const QVector<EntryNode> &nodes) {
        for (const auto &n : nodes) {
            if (!n.isSubmenu) flatEntries.append(&n);
            else collect(n.children);
        }
    };
    collect(roots);

    // Validate each entry
    for (const auto *e : flatEntries) {
        QVector<EntryValidation> ev = validateEntry(*e);
        for (auto &v : ev) {
            // Prefix message with entry title for context
            v.message = QString("[%1] %2").arg(e->title, v.message);
        }
        all.append(ev);
    }

    // Duplicate detection: same title + same kernel/vmlinuz
    struct DupKey { QString title, kernel; bool operator<(const DupKey &o) const {
        return title < o.title || (title == o.title && kernel < o.kernel);
    }};
    QMap<DupKey, int> seen;  // key -> first index
    for (int i = 0; i < flatEntries.size(); ++i) {
        const EntryNode *e = flatEntries[i];
        DupKey key;
        key.title = e->title;
        key.kernel = e->type == "linux" ? e->vmlinuz : e->kernel;
        if (seen.contains(key)) {
            all.append({EntryValidation::Warning, "title",
                        QString("duplicate entry: '%1' (kernel='%2') first at index %3, duplicated at %4")
                        .arg(key.title, key.kernel).arg(seen[key]).arg(i)});
        } else {
            seen[key] = i;
        }
    }

    // Row count check
    int rows = flatEntries.size();
    if (rows > 64)
        all.append({EntryValidation::Warning, "",
                    QString("%1 entries exceeds firmware cap of 64 rows").arg(rows)});

    return all;
}

// ===========================================================================
//  Presets (~30). Each applies explicit sets so they serialize.
// ===========================================================================
struct PresetDef {
    const char *name; const char *theme; const char *style; const char *sel;
    const char *border; const char *btn; int fxScan; int fxVig; bool glass;
};
static const PresetDef kPresets[] = {
    {"Forest Classic","forest","classic","doublebar","thick","raised",0,0,false},
    {"Midnight Modern","midnight","modern","bar","thin","flat",0,0,false},
    {"Nord Card","nord","card","box","thin","raised",0,0,false},
    {"Dracula Neon","dracula","neon","glow","glow","pill",0,0,false},
    {"Gruvbox Retro","gruvbox","retro","invert","thick","outline",0,40,false},
    {"Solarized Elegant","solarized","elegant","underline","thin","ghost",0,0,false},
    {"Amber Terminal","amber","terminal","bracket","none","flat",60,0,false},
    {"Matrix Hacker","matrix","hacker","glow","glow","ghost",120,0,false},
    {"Rose Glass","rose","glass","pill","shadow","glass",0,0,true},
    {"Ocean Dock","ocean","dock-bottom","bar","thin","raised",0,0,false},
    {"Mono Minimal","mono","minimal","none","none","flat",0,0,false},
    {"Nord Sidebar","nord","sidebar-left","bar","thin","flat",0,0,false},
    {"Dracula Spotlight","dracula","spotlight","glow","glow","pill",0,60,false},
    {"Gruvbox Framed","gruvbox","framed","box","double","outline",0,0,false},
    {"Forest Fullscreen","forest","fullscreen","doublebar","none","raised",0,0,false},
    {"Midnight Ribbon","midnight","ribbon","bar","shadow","flat",0,0,false},
    {"Ocean Pill","ocean","pill","pill","thin","pill",0,0,false},
    {"Amber Boxed","amber","boxed","box","thick","outline",40,0,false},
    {"Matrix Ghost","matrix","ghost","outline","none","ghost",90,0,false},
    {"Solarized Underline","solarized","underline","underline","thin","ghost",0,0,false},
    {"Rose Centered","rose","centered","pill","shadow","glass",0,0,true},
    {"Nord Compact","nord","compact","bar","thin","flat",0,0,false},
    {"Dracula Banner","dracula","banner-top","bar","thin","pill",0,0,false},
    {"Forest Spacious","forest","spacious","doublebar","thick","raised",0,0,false},
    {"Mono Outline","mono","outline","outline","thin","outline",0,0,false},
    {"Gruvbox Brackets","gruvbox","brackets","bracket","none","outline",0,30,false},
    {"Ocean Glass","ocean","glass","pill","shadow","glass",0,0,true},
    {"Amber Invert","amber","invert","invert","none","flat",30,0,false},
    {"Midnight Dashed","midnight","dashed","box","dashed","flat",0,0,false},
    {"Matrix Neon","matrix","neon","glow","glow","pill",120,0,false},
};
int forbPresetCount() { return (int)(sizeof(kPresets)/sizeof(kPresets[0])); }
const char *forbPresetName(int i) { return kPresets[i].name; }

void ConfigModel::applyPreset(int index) {
    if (index < 0 || index >= forbPresetCount()) return;
    const PresetDef &p = kPresets[index];
    th.preset = p.theme;
    th.menuStyle = p.style;
    th.menuSelection = p.sel;
    th.menuBorder = p.border;
    th.btnStyle = p.btn;
    // stamp the theme's concrete palette as explicit colour overrides so it
    // survives round-trip and the preview matches the gallery thumbnail.
    Schema::Palette pal = Schema::paletteFor(p.theme);
    th.colorBg.assign(pal.bg); th.colorFg.assign(pal.fg); th.colorAccent.assign(pal.accent);
    th.colorSelBg.assign(pal.sel_bg); th.colorSelFg.assign(pal.sel_fg);
    th.colorTitlebar.assign(pal.titlebar); th.colorWindow.assign(pal.window);
    th.colorCursor.assign(pal.cursor);
    if (p.fxScan) th.fxScanlines.assign(p.fxScan); else th.fxScanlines.unset();
    if (p.fxVig)  th.fxVignette.assign(p.fxVig);   else th.fxVignette.unset();
    th.fxGlass.assign(p.glass);
    requestRefresh();
}
