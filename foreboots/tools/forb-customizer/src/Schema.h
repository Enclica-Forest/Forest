// Schema.h - single source of truth for the forebo.cfg enum vocabularies,
// clamp ranges and the named-theme palettes. Shared by the tabs (to build
// combos), the serializer/reader (to map strings), the preview (to resolve a
// concrete style) and the preset gallery. Keeping every table here means the
// UI, the writer and the reader can never disagree about the key vocabulary.
#ifndef FORB_SCHEMA_H
#define FORB_SCHEMA_H

#include <QStringList>
#include <QString>
#include <QMap>

namespace Schema {

// ---- enum vocabularies (order == the firmware enum order in forebo_cfg.h) ----
inline QStringList menuStyles() {
    return {"classic","minimal","terminal","flat","modern","card","neon",
            "outline","underline","invert","brackets","sidebar-left",
            "sidebar-right","banner-top","dock-bottom","fullscreen","centered",
            "compact","spacious","retro","glass","hacker","ribbon","framed",
            "dashed","spotlight","pill","boxed","ghost","elegant"};
}
inline QStringList menuPos()      { return {"center","left","right","top","bottom","full","custom"}; }
inline QStringList menuAlign()    { return {"left","center","right"}; }
inline QStringList selStyles()    { return {"bar","doublebar","box","outline","underline","arrow","bracket","invert","pill","gradient","glow","none"}; }
inline QStringList borderStyles() { return {"none","thin","thick","double","shadow","glow","dashed"}; }
inline QStringList corners()      { return {"square","round","cut"}; }
inline QStringList iconSides()    { return {"left","right"}; }
inline QStringList windowSkins()  { return {"flat","beveled","glass"}; }
inline QStringList btnStyles()    { return {"flat","raised","pill","outline","ghost","glass"}; }
inline QStringList themes()       { return {"forest","midnight","nord","dracula","gruvbox","solarized","amber","matrix","rose","ocean","mono"}; }
inline QStringList entryTypes()   { return {"forest","linux","chainload","shell","recovery","tools","setup","settings","reboot"}; }

// The 18 icon short-names shipped by tools/gen_assets.py (see forebo.cfg).
inline QStringList iconNames() {
    return {"os","text","safe","gear","shield","reboot","ubuntu","debian",
            "arch","fedora","mint","tux","windows","grub","usb","disk",
            "terminal","settings"};
}

// Syslinux icon mapping: keyword -> ForeB icon short-name (mirrors the
// syslinux LABEL/MENU LABEL to icon resolution that forebo-install does).
inline QMap<QString,QString> syslinuxIconMap() {
    return {
        {"windows", "windows"}, {"win", "windows"},
        {"grub", "grub"}, {"ubuntu", "ubuntu"}, {"debian", "debian"},
        {"fedora", "fedora"}, {"mint", "mint"}, {"arch", "arch"},
        {"cachyos", "arch"}, {"endeavour", "arch"}, {"manjaro", "arch"},
        {"safe", "safe"}, {"fallback", "safe"}, {"snapshot", "safe"},
        {"usb", "usb"}, {"removable", "usb"}, {"shell", "terminal"},
        {"recovery", "shield"}, {"memtest", "gear"}, {"hd", "disk"},
    };
}

// ---- a concrete 8-colour palette per named theme ----
// bg/fg/accent/sel_bg/sel_fg are taken verbatim from the firmware's g_themes
// (uefi/ui.c: bg, text, accent, select, white) so an applied preset renders the
// same on hardware as in the preview. titlebar=select, window=panel, cursor=white
// (the firmware has no per-theme WM colors; presets stamp these as explicit
// color_* overrides, which both sides honor identically).
struct Palette {
    unsigned bg, fg, accent, sel_bg, sel_fg, titlebar, window, cursor;
};
inline QMap<QString, Palette> themePalettes() {
    QMap<QString, Palette> m;
    m["forest"]    = {0x182D18,0xB6DFB6,0x51CA3D,0x146514,0xFFFFFF,0x146514,0x1C351C,0xFFFFFF};
    m["midnight"]  = {0x0B1020,0xC7D6EE,0x6AA9FF,0x1E3A66,0xFFFFFF,0x1E3A66,0x131B2E,0xFFFFFF};
    m["nord"]      = {0x2E3440,0xECEFF4,0x88C0D0,0x434C5E,0xECEFF4,0x434C5E,0x343B49,0xECEFF4};
    m["dracula"]   = {0x282A36,0xF8F8F2,0xFF79C6,0x454863,0xFFFFFF,0x454863,0x31333F,0xFF79C6};
    m["gruvbox"]   = {0x282828,0xEBDBB2,0xFABD2F,0x453C30,0xFBF1C7,0x453C30,0x323028,0xFBF1C7};
    m["solarized"] = {0x002B36,0x93A1A1,0x268BD2,0x094A56,0xFDF6E3,0x094A56,0x073642,0xFDF6E3};
    m["amber"]     = {0x120A00,0xFFCC55,0xFFB000,0x3B2600,0xFFE0A0,0x3B2600,0x1A1200,0xFFE0A0};
    m["matrix"]    = {0x001200,0x90FFA0,0x00FF41,0x073807,0xD0FFD8,0x073807,0x001A00,0xD0FFD8};
    m["rose"]      = {0x191724,0xE0DEF4,0xEB6F92,0x2A2740,0xFFFFFF,0x2A2740,0x232135,0xEB6F92};
    m["ocean"]     = {0x0A1E24,0xCDECEF,0x33C5D8,0x13414C,0xFFFFFF,0x13414C,0x102A32,0xFFFFFF};
    m["mono"]      = {0x141414,0xC8C8C8,0xE0E0E0,0x343434,0xFFFFFF,0x343434,0x1E1E1E,0xFFFFFF};
    return m;
}
inline Palette paletteFor(const QString &name) {
    auto m = themePalettes();
    return m.value(name.isEmpty() ? "forest" : name, m["forest"]);
}

} // namespace Schema

#endif // FORB_SCHEMA_H
