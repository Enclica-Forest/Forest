// ConfigModel.h - the single source of truth for the whole editor.
//
// Holds a Global block, a Theme block (colours + toggles + menu style + button
// / UI style + effects + audio + Track-3 window-skin/image keys) and the
// menuentry/submenu tree. Every OPTIONAL key is an Opt<T> so unset => omitted
// on save (matches the firmware's -1 / FOREB_COLOR_UNSET inherit sentinels).
//
// Serialize / parse / Limine-import all live here (CfgSerializer + LimineImporter
// folded in for a compact, dependency-light build). Emitting matches the order
// and escaping of tools/forebo-install so generated files are byte-compatible
// with what the firmware and the installer already accept.
#ifndef FORB_CONFIGMODEL_H
#define FORB_CONFIGMODEL_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QRgb>
#include "Opt.h"

// ---- one menu row: a menuentry OR a submenu (submenus carry children) -------
struct EntryNode {
    bool        isSubmenu = false;
    QString     title;
    QString     type = "forest";   // forest|linux|chainload|shell|recovery|tools|setup|settings|reboot
    QString     kernel, vmlinuz, initrd, chain, cmdline, icon, background;
    QStringList modules;
    QVector<EntryNode> children;   // only meaningful when isSubmenu

    bool operator==(const EntryNode &o) const {
        return isSubmenu == o.isSubmenu && title == o.title && type == o.type &&
               kernel == o.kernel && vmlinuz == o.vmlinuz && initrd == o.initrd &&
               chain == o.chain && cmdline == o.cmdline && icon == o.icon &&
               background == o.background && modules == o.modules &&
               children == o.children;
    }
    bool operator!=(const EntryNode &o) const { return !(*this == o); }
};

struct Global {
    Opt<int>     timeout;
    QString      defaultRef;       // raw default= value (index string or title-path)
    Opt<bool>    rememberLast;
    Opt<QString> background;

    bool operator==(const Global &o) const {
        return timeout == o.timeout && defaultRef == o.defaultRef &&
               rememberLast == o.rememberLast && background == o.background;
    }
};

struct Tone { Opt<int> freq, ms; bool operator==(const Tone &o) const { return freq==o.freq && ms==o.ms; } };

struct Theme {
    // named preset (theme=) - "" means forest
    QString       preset;

    // colours (color_*)
    Opt<QRgb>     colorBg, colorFg, colorAccent, colorSelBg, colorSelFg,
                  colorTitlebar, colorWindow, colorCursor;

    // input / compositor toggles
    Opt<QString>  cursorPath;
    Opt<bool>     cursorEnabled, mouseEnabled, animations, doubleBuffer;
    QString       windowSkin;      // flat|beveled|glass ("" inherit)

    // menu layout (menu_*)
    QString       menuStyle;       // menu_style= preset name ("" inherit)
    QString       menuPos, menuAlign, menuSelection, menuBorder, menuCorner, menuIconSide;
    Opt<int>      menuX, menuY, menuW, menuH, menuEntryH, menuPad;
    Opt<bool>     menuAccentStrip, menuDividers, menuGradient, menuShadow,
                  menuTitleBar, menuShowTitle, menuShowFooter, menuShowTimer,
                  menuShowIcons, menuScrollbar, menuCaret;

    // buttons / UI (btn_* / ui_*)
    QString       btnStyle, btnCorner, uiWindowCorner;
    Opt<int>      btnBorder, btnPadX, btnPadY, uiWindowBorder, uiPanelAlpha,
                  uiScrollbarW, uiFocusWidth, uiFontScale;
    Opt<bool>     btnGradient, btnShadow, btnGlow;
    Opt<QRgb>     btnFill, btnFillHover, btnFillActive, btnFillDisabled,
                  btnText, btnTextHover, btnTextActive, btnBorderColor, btnFocusColor,
                  uiSeparator, uiScrollbarColor, uiFocusColor;

    // effects (fx_*)
    Opt<bool>     fxGlass;
    Opt<int>      fxBlur, fxOpacity, fxVignette, fxScanlines;

    // audio (pcspeaker / audio_*)
    Opt<bool>     pcspeaker;
    Opt<int>      audioVolume;
    Tone          toneNav, toneSelect, toneOpen, toneError, toneBack;

    // Track-3 window skin (win_*)
    Opt<int>      winTitleH;        // -1 == auto (kept as an explicit set value)
    Opt<QRgb>     winTitleFill, winTitleFg, winBorderColor, winCloseColor;
    Opt<int>      winBorderW;
    QString       winCorner, winButtonStyle;
    Opt<bool>     winShadow;

    // Track-3 image slots (img_*)
    Opt<QString>  imgBackground, imgPanel, imgWindow, imgButton, imgTitlebar, imgCursor;

    bool operator==(const Theme &o) const;
};

class ConfigModel : public QObject {
    Q_OBJECT
public:
    explicit ConfigModel(QObject *parent = nullptr);

    Global             g;
    Theme              th;
    QVector<EntryNode> roots;

    // WYSIWYG spine: any value edit -> touch() -> changed() -> preview repaint.
    void touch()            { emit changed(); }
    void touchStructure()   { emit structureChanged(); emit changed(); }
    // Ask every bound control to re-read the model (after load/import/preset).
    void requestRefresh()   { emit refreshRequested(); emit changed(); }

    void resetDefaults();          // built-in Forest defaults, no entries

    // ---- persistence -------------------------------------------------------
    QString serialize() const;     // ConfigModel -> forebo.cfg text
    bool    parseText(const QString &text);   // forebo.cfg text -> ConfigModel
    bool    loadFile(const QString &path, QString *err = nullptr);
    bool    saveFile(const QString &path, QString *err = nullptr); // timestamped .bak + atomic

    // ---- Limine migration --------------------------------------------------
    bool    importLimine(const QString &path, QString *err = nullptr);

    // ---- presets -----------------------------------------------------------
    void    applyPreset(int index);           // apply gallery preset i into the model

    // flatten helper for the default= combo and preview
    struct FlatRef { QString label; const EntryNode *node; };
    QVector<FlatRef> flatten() const;

signals:
    void changed();            // any edit (preview repaint)
    void structureChanged();   // entry add/remove/reorder
    void refreshRequested();   // controls should re-sync from the model

private:
    static void sanitizeTitleInPlace(QString &t);
};

#endif // FORB_CONFIGMODEL_H
