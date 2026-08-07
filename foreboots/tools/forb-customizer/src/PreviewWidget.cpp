// PreviewWidget.cpp - a 1:1 recreation of the ForeB boot menu, ported from the
// firmware renderer (uefi/ui.c: ui_background / ui_menu_layout / ui_panel_frame /
// ui_sel_bg). Renders into an off-screen image at the chosen framebuffer size,
// then letterbox-scales it into the widget so proportions match the real screen.
#include "PreviewWidget.h"
#include "ConfigModel.h"
#include "Schema.h"

#include <QPainter>
#include <QImage>
#include <QRadialGradient>
#include <QMouseEvent>
#include <QFile>
#include <QFileInfo>
#include <cstring>
#include <vector>
#include "font8x16.h"   // the exact VGA 8x16 bitmap the firmware renders with

// ---------------------------------------------------------------------------
// Colour helpers (ui.c stores 0x00RRGGBB; ui_lerp blends with t in 0..256).
// ---------------------------------------------------------------------------
static inline QColor col(unsigned c){ return QColor((c>>16)&0xFF,(c>>8)&0xFF,c&0xFF); }
static inline unsigned lerpu(unsigned a, unsigned b, int t){
    int ar=(a>>16)&0xFF, ag=(a>>8)&0xFF, ab=a&0xFF;
    int br=(b>>16)&0xFF, bg=(b>>8)&0xFF, bb=b&0xFF;
    int r=ar+((br-ar)*t)/256, g=ag+((bg-ag)*t)/256, l=ab+((bb-ab)*t)/256;
    return ((unsigned)r<<16)|((unsigned)g<<8)|(unsigned)l;
}
static inline QColor lerpc(unsigned a, unsigned b, int t){ return col(lerpu(a,b,t)); }

// ---------------------------------------------------------------------------
// The 18-field palette per named theme (verbatim from uefi/ui.c g_themes).
// bg,bg_top,bg_bottom,panel,border,select,title,text,dim,timer,white,shadow,
// tree1,tree2,tree3,prog_track,prog_fill,accent
// ---------------------------------------------------------------------------
struct Pal18 { unsigned bg,bg_top,bg_bottom,panel,border,select,title,text,dim,
                        timer,white,shadow,tree1,tree2,tree3,prog_track,prog_fill,accent; };
static Pal18 palFor(const QString &name){
    struct Row { const char*n; Pal18 p; };
    static const Row R[] = {
    {"forest",{0x182D18,0x102010,0x1E3A1E,0x1C351C,0x285128,0x146514,0x51CA3D,0xB6DFB6,0x658265,0xDFA214,0xFFFFFF,0x040804,0x3D1C08,0x1C791C,0x3DB63D,0x285128,0x51CA3D,0x51CA3D}},
    {"midnight",{0x0B1020,0x070B18,0x131C33,0x131B2E,0x2B3B5C,0x1E3A66,0x6AA9FF,0xC7D6EE,0x6A7C99,0xFFC24B,0xFFFFFF,0x030509,0x1C2A3A,0x274A6E,0x4A82C0,0x2B3B5C,0x6AA9FF,0x6AA9FF}},
    {"nord",{0x2E3440,0x272C36,0x3B4252,0x343B49,0x4C566A,0x434C5E,0x88C0D0,0xECEFF4,0x818C9C,0xEBCB8B,0xECEFF4,0x191C22,0x4C566A,0x5E81AC,0x81A1C1,0x4C566A,0xA3BE8C,0x88C0D0}},
    {"dracula",{0x282A36,0x21222C,0x343746,0x31333F,0x44475A,0x454863,0xBD93F9,0xF8F8F2,0x6272A4,0xFFB86C,0xFFFFFF,0x121319,0x343746,0x6272A4,0xBD93F9,0x44475A,0x50FA7B,0xFF79C6}},
    {"gruvbox",{0x282828,0x1D2021,0x323030,0x323028,0x504945,0x453C30,0xFABD2F,0xEBDBB2,0xA89984,0xFE8019,0xFBF1C7,0x120F0F,0x3C3836,0x689D6A,0xB8BB26,0x504945,0xB8BB26,0xFABD2F}},
    {"solarized",{0x002B36,0x001F27,0x073642,0x073642,0x586E75,0x094A56,0x268BD2,0x93A1A1,0x657B83,0xB58900,0xFDF6E3,0x001015,0x073642,0x2AA198,0x859900,0x586E75,0x2AA198,0x268BD2}},
    {"amber",{0x120A00,0x0A0600,0x1E1200,0x1A1200,0x4A3300,0x3B2600,0xFFB000,0xFFCC55,0xA87A20,0xFF7818,0xFFE0A0,0x080400,0x201400,0x805000,0xFFB000,0x4A3300,0xFFB000,0xFFB000}},
    {"matrix",{0x001200,0x000A00,0x002200,0x001A00,0x105010,0x073807,0x00FF41,0x90FFA0,0x309040,0x00FF41,0xD0FFD8,0x000600,0x003000,0x008820,0x00FF41,0x105010,0x00FF41,0x00FF41}},
    {"rose",{0x191724,0x12101B,0x232135,0x232135,0x403D52,0x2A2740,0xEBBCBA,0xE0DEF4,0x908CAA,0xF6C177,0xFFFFFF,0x0C0B12,0x232135,0x524F67,0xC4A7E7,0x403D52,0xEBBCBA,0xEB6F92}},
    {"ocean",{0x0A1E24,0x061418,0x113038,0x102A32,0x285561,0x13414C,0x33C5D8,0xCDECEF,0x5F8A92,0xFFC24B,0xFFFFFF,0x030A0C,0x123840,0x1C6E78,0x33C5D8,0x285561,0x40D0A0,0x33C5D8}},
    {"mono",{0x141414,0x0E0E0E,0x202020,0x1E1E1E,0x404040,0x343434,0xE0E0E0,0xC8C8C8,0x808080,0xE0E0E0,0xFFFFFF,0x060606,0x303030,0x707070,0xB0B0B0,0x404040,0xE0E0E0,0xE0E0E0}},
    };
    QString n = name.isEmpty() ? "forest" : name;
    for (auto &r : R) if (n == r.n) return r.p;
    return R[0].p;
}

// ---------------------------------------------------------------------------
// Resolved menu style (mirrors uefi/ui.c STYLE_BASE + style_preset_apply +
// ui_apply_style). Only the fields the preview needs.
// ---------------------------------------------------------------------------
enum { POS_CENTER,POS_LEFT,POS_RIGHT,POS_TOP,POS_BOTTOM,POS_FULL,POS_CUSTOM };
enum { AL_LEFT,AL_CENTER,AL_RIGHT };
// selection: bar,doublebar,box,outline,underline,arrow,bracket,invert,pill,gradient,glow,none
// border: none,thin,thick,double,shadow,glow,dashed ; corner: square,round,cut
struct RStyle {
    int pos=POS_CENTER, align=AL_LEFT;
    int px=-1,py=-1,pw=-1,ph=-1, entryH=-1, pad=14;
    QString sel="doublebar", border="thick", corner="square";
    int iconRight=1;
    int accentStrip=1,dividers=0,gradient=1,shadow=1,titleBar=1,
        showTitle=1,showFooter=1,showTimer=1,showIcons=1,scrollbar=1,caret=1;
};
static int idx(const QStringList&l,const QString&s){int i=l.indexOf(s);return i<0?0:i;}

static RStyle resolveStyle(const Theme &t){
    RStyle r;
    QString p = t.menuStyle.isEmpty() ? "classic" : t.menuStyle;
    auto S=[&](const char*sel,const char*bd,const char*cn){ r.sel=sel;r.border=bd;r.corner=cn; };
    // preset deltas (ported from style_preset_apply in ui.c)
    if(p=="minimal"){ r.border="none";r.accentStrip=0;r.gradient=0;r.shadow=0;r.sel="arrow";r.showIcons=0;r.titleBar=0;r.showTitle=0; }
    else if(p=="terminal"){ r.align=AL_LEFT;r.sel="bracket";r.border="thin";r.gradient=0;r.accentStrip=0;r.dividers=1;r.showIcons=0; }
    else if(p=="flat"){ r.gradient=0;r.shadow=0;r.border="thin";r.sel="bar";r.accentStrip=0; }
    else if(p=="modern"){ r.sel="pill";r.corner="round";r.border="none";r.gradient=1;r.shadow=1;r.accentStrip=1; }
    else if(p=="card"){ r.corner="round";r.border="thick";r.shadow=1;r.sel="box"; }
    else if(p=="neon"){ r.sel="glow";r.border="glow";r.accentStrip=1;r.gradient=1; }
    else if(p=="outline"){ r.border="thin";r.sel="outline";r.gradient=0;r.accentStrip=0; }
    else if(p=="underline"){ r.sel="underline";r.border="none";r.dividers=1;r.gradient=0; }
    else if(p=="invert"){ r.sel="invert";r.border="thin";r.gradient=0; }
    else if(p=="brackets"){ r.sel="bracket";r.align=AL_CENTER;r.border="thin"; }
    else if(p=="sidebar-left"){ r.pos=POS_LEFT;r.align=AL_LEFT;r.iconRight=0;r.showTitle=0; }
    else if(p=="sidebar-right"){ r.pos=POS_RIGHT;r.align=AL_LEFT;r.showTitle=0; }
    else if(p=="banner-top"){ r.pos=POS_TOP;r.showTitle=0; }
    else if(p=="dock-bottom"){ r.pos=POS_BOTTOM; }
    else if(p=="fullscreen"){ r.pos=POS_FULL;r.align=AL_LEFT;r.showTitle=0; }
    else if(p=="centered"){ r.pos=POS_CENTER;r.align=AL_CENTER;r.sel="bar";r.showIcons=0; }
    else if(p=="compact"){ r.entryH=42;r.pad=8;r.gradient=0;r.accentStrip=0; }
    else if(p=="spacious"){ r.entryH=78;r.pad=20; }
    else if(p=="retro"){ r.border="double";r.sel="bracket";r.align=AL_LEFT;r.corner="cut";r.gradient=0; }
    else if(p=="glass"){ r.gradient=1;r.border="thin";r.sel="gradient";r.accentStrip=1; }
    else if(p=="hacker"||p=="matrix"){ r.align=AL_LEFT;r.sel="none";r.dividers=1;r.showIcons=0;r.gradient=0;r.border="thin";r.accentStrip=0;r.showTitle=0; }
    else if(p=="ribbon"){ r.accentStrip=1;r.sel="bar";r.border="none";r.gradient=1; }
    else if(p=="framed"){ r.border="double";r.corner="square";r.sel="bar"; }
    else if(p=="dashed"){ r.border="dashed";r.sel="outline";r.gradient=0; }
    else if(p=="spotlight"){ r.sel="glow";r.accentStrip=0;r.border="none";r.gradient=1; }
    else if(p=="pill"){ r.sel="pill";r.corner="round";r.border="thin"; }
    else if(p=="boxed"){ r.sel="box";r.border="thick";r.gradient=0; }
    else if(p=="ghost"){ r.border="none";r.gradient=0;r.shadow=0;r.sel="outline";r.accentStrip=0; }
    else if(p=="elegant"){ r.gradient=1;r.sel="underline";r.accentStrip=1;r.align=AL_LEFT;r.showIcons=0;r.corner="round"; }
    (void)S;
    // explicit config overrides (menu_*)
    if(!t.menuPos.isEmpty())      r.pos=idx(Schema::menuPos(),t.menuPos);
    if(!t.menuAlign.isEmpty())    r.align=idx(Schema::menuAlign(),t.menuAlign);
    if(!t.menuSelection.isEmpty())r.sel=t.menuSelection;
    if(!t.menuBorder.isEmpty())   r.border=t.menuBorder;
    if(!t.menuCorner.isEmpty())   r.corner=t.menuCorner;
    if(!t.menuIconSide.isEmpty()) r.iconRight = (t.menuIconSide=="left")?0:1;
    if(t.menuX.isSet())r.px=t.menuX.v; if(t.menuY.isSet())r.py=t.menuY.v;
    if(t.menuW.isSet())r.pw=t.menuW.v; if(t.menuH.isSet())r.ph=t.menuH.v;
    if(t.menuEntryH.isSet())r.entryH=t.menuEntryH.v; if(t.menuPad.isSet())r.pad=t.menuPad.v;
    auto B=[&](const Opt<bool>&o,int&f){ if(o.isSet()) f=o.v?1:0; };
    B(t.menuAccentStrip,r.accentStrip); B(t.menuDividers,r.dividers); B(t.menuGradient,r.gradient);
    B(t.menuShadow,r.shadow); B(t.menuTitleBar,r.titleBar); B(t.menuShowTitle,r.showTitle);
    B(t.menuShowFooter,r.showFooter); B(t.menuShowTimer,r.showTimer); B(t.menuShowIcons,r.showIcons);
    B(t.menuScrollbar,r.scrollbar); B(t.menuCaret,r.caret);
    return r;
}

// ---------------------------------------------------------------------------
PreviewWidget::PreviewWidget(ConfigModel *m, QWidget *parent)
    : QWidget(parent), model(m) {
    setMinimumSize(420, 320);
    connect(model, &ConfigModel::changed, this, [this]{ update(); });
}

// ---- minimal uncompressed/RLE TGA decoder (Qt has no TGA plugin by default) -
static QImage loadTGA(const QString &path) {
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return {};
    QByteArray d = f.readAll(); if(d.size()<18) return {};
    const unsigned char *p=(const unsigned char*)d.constData();
    int idlen=p[0], type=p[2];
    int w=p[12]|(p[13]<<8), h=p[14]|(p[15]<<8), bpp=p[16], desc=p[17];
    if(w<=0||h<=0||(bpp!=24&&bpp!=32)||(type!=2&&type!=10)) return {};
    int bytes=bpp/8; const unsigned char *s=p+18+idlen;
    const unsigned char *end=p+d.size();
    QImage img(w,h,QImage::Format_ARGB32); img.fill(Qt::black);
    int n=w*h, i=0; std::vector<unsigned char> px((size_t)n*bytes);
    if(type==2){ if(s+(size_t)n*bytes>end) return {}; memcpy(px.data(),s,(size_t)n*bytes); }
    else { // RLE
        size_t o=0;
        while(i<n && s<end){ unsigned char rc=*s++; int cnt=(rc&0x7F)+1;
            if(rc&0x80){ if(s+bytes>end)break; for(int k=0;k<cnt&&i<n;k++,i++){ memcpy(&px[o],s,bytes); o+=bytes; } s+=bytes; }
            else { for(int k=0;k<cnt&&i<n;k++,i++){ if(s+bytes>end)break; memcpy(&px[o],s,bytes); o+=bytes; s+=bytes; } } }
    }
    bool topOrigin = desc&0x20;
    for(int y=0;y<h;y++){ int sy = topOrigin? y : (h-1-y);
        QRgb *row=(QRgb*)img.scanLine(y);
        for(int x=0;x<w;x++){ const unsigned char *q=&px[((size_t)sy*w+x)*bytes];
            int b=q[0],g=q[1],r=q[2],a=bpp==32?q[3]:255; row[x]=qRgba(r,g,b,a); } }
    return img;
}

QImage PreviewWidget::loadImg(const QString &field) {
    if(field.isEmpty()) return {};
    // --- resolve the field to a real host file -----------------------------
    QString host;
    { QFileInfo direct(field);
      if(direct.exists() && direct.isFile() && direct.isAbsolute()) host = field; }
    if(host.isEmpty()){
        QStringList cands;
        if(!configDir.isEmpty()){
            QString espRoot = QFileInfo(configDir).absolutePath();   // parent of /forebo
            QString grand   = QFileInfo(espRoot).absolutePath();     // covers <esp>/EFI/forebo
            if(field.startsWith("/")){
                cands << espRoot + field << grand + field;
                if(field.startsWith("/forebo/"))
                    cands << configDir + field.mid(7);               // strip prefix, keep subdirs
                cands << configDir + field
                      << configDir + "/" + QFileInfo(field).fileName();
            } else {
                cands << configDir + "/" + field;                    // cfg-relative host path
            }
        }
        cands << field;                                              // legacy: CWD-relative
        for(const QString &c : cands){
            QFileInfo fi(c);
            if(fi.exists() && fi.isFile()){ host = fi.absoluteFilePath(); break; }
        }
    }
    if(host.isEmpty()) return {};              // not found: retry next repaint, never cached
    // --- decode, cached by host path + mtime -------------------------------
    qint64 mtime = QFileInfo(host).lastModified().toMSecsSinceEpoch();
    auto it = imgCache.constFind(host);
    if(it != imgCache.constEnd() && it->mtime == mtime) return it->img;
    QImage im = host.toLower().endsWith(".tga") ? loadTGA(host) : QImage(host);
    if(im.isNull()) return {};                 // decode failure: also never cached
    imgCache.insert(host, {im, mtime});
    return im;
}

// ---- separable box blur + darken over a region of the composed image ------
// (ports uefi/ui.c ui_blur_rect / fx_darken; used for the glass window skin).
static void blurRegion(QImage &img, int x, int y, int w, int h, int r){
    if(r<1) return;
    x=qMax(0,x); y=qMax(0,y);
    w=qMin(w,img.width()-x); h=qMin(h,img.height()-y);
    if(w<=0||h<=0) return;
    r=qMin(r,16);
    std::vector<int> tmp((size_t)qMax(w,h)*3);
    auto lane=[&](QRgb p,int c){ return (c==0)?qRed(p):(c==1?qGreen(p):qBlue(p)); };
    for(int row=0;row<h;row++){
        QRgb *line=(QRgb*)img.scanLine(y+row)+x;
        for(int c=0;c<3;c++){
            int sum=0,cnt=0;
            for(int i=0;i<=r&&i<w;i++){ sum+=lane(line[i],c); cnt++; }
            for(int i=0;i<w;i++){
                tmp[(size_t)i*3+c]=sum/cnt;
                int add=i+r+1, sub=i-r;
                if(add<w){ sum+=lane(line[add],c); cnt++; }
                if(sub>=0){ sum-=lane(line[sub],c); cnt--; }
            }
        }
        for(int i=0;i<w;i++) line[i]=qRgb(tmp[(size_t)i*3],tmp[(size_t)i*3+1],tmp[(size_t)i*3+2]);
    }
    for(int colx=0;colx<w;colx++){
        for(int c=0;c<3;c++){
            int sum=0,cnt=0;
            for(int i=0;i<=r&&i<h;i++){ sum+=lane(((QRgb*)img.scanLine(y+i))[x+colx],c); cnt++; }
            for(int i=0;i<h;i++){
                tmp[(size_t)i*3+c]=sum/cnt;
                int add=i+r+1, sub=i-r;
                if(add<h){ sum+=lane(((QRgb*)img.scanLine(y+add))[x+colx],c); cnt++; }
                if(sub>=0){ sum-=lane(((QRgb*)img.scanLine(y+sub))[x+colx],c); cnt--; }
            }
        }
        for(int i=0;i<h;i++){ QRgb *p=((QRgb*)img.scanLine(y+i))+x+colx;
            *p=qRgb(tmp[(size_t)i*3],tmp[(size_t)i*3+1],tmp[(size_t)i*3+2]); }
    }
}
static void darkenRegion(QImage &img, int x, int y, int w, int h, int amt){
    if(amt<=0) return; if(amt>255) amt=255;
    x=qMax(0,x); y=qMax(0,y); w=qMin(w,img.width()-x); h=qMin(h,img.height()-y);
    if(w<=0||h<=0) return;
    int keep=255-amt;
    for(int row=0;row<h;row++){ QRgb *line=(QRgb*)img.scanLine(y+row)+x;
        for(int i=0;i<w;i++){ QRgb p=line[i];
            line[i]=qRgb(qRed(p)*keep/255,qGreen(p)*keep/255,qBlue(p)*keep/255); } }
}

// ---------------------------------------------------------------------------
// Rendering into a native-resolution image, ported layer-for-layer from ui.c.
// ---------------------------------------------------------------------------
void PreviewWidget::paintEvent(QPaintEvent *) {
    const Theme &t = model->th;
    Pal18 P = palFor(t.preset);
    // Apply colour overrides EXACTLY like the firmware: bootx64.c only passes a
    // colour when it differs from the FOREB_DEF_* default (else the named theme
    // palette wins), and ui_pal_set additionally skips 0. Replicate both gates.
    if(t.colorBg.isSet()    && t.colorBg.v    && t.colorBg.v    != 0x0E1A12u) { P.bg=t.colorBg.v; P.bg_bottom=t.colorBg.v; }
    if(t.colorFg.isSet()    && t.colorFg.v    && t.colorFg.v    != 0xDDE7DEu)   P.text=t.colorFg.v;
    if(t.colorAccent.isSet()&& t.colorAccent.v&& t.colorAccent.v!= 0x3FB56Bu) { P.accent=t.colorAccent.v; P.title=t.colorAccent.v; P.prog_fill=t.colorAccent.v; }
    if(t.colorSelBg.isSet() && t.colorSelBg.v && t.colorSelBg.v != 0x1F5E3Au)   P.select=t.colorSelBg.v;
    if(t.colorSelFg.isSet() && t.colorSelFg.v && t.colorSelFg.v != 0xFFFFFFu)   P.white=t.colorSelFg.v;

    RStyle S = resolveStyle(t);

    const int W=fbW, H=fbH;
    QImage img(W,H,QImage::Format_RGB32);
    QPainter g(&img);
    g.setRenderHint(QPainter::Antialiasing,false);

    // integer-permille layout helpers (UI_FW/UI_FH)
    auto FW=[&](int pm){ return (int)((qint64)W*pm/1000); };
    auto FH=[&](int pm){ return (int)((qint64)H*pm/1000); };
    int uiscale = (H>=1080)?2:1;       // matches ui.c g_uiscale
    int gh = FONT8X16_H*uiscale;       // glyph cell height (8x16 * uiscale)
    int gw = FONT8X16_W*uiscale;       // glyph advance = 8 * uiscale
    // Blit a string with the real firmware font (MSB-first, cell top at y).
    auto text=[&](int x,int y,const QString&s,QColor c){
        g.setPen(Qt::NoPen); g.setBrush(c);
        for(int i=0;i<s.size();++i){
            int gi=(unsigned char)s.at(i).toLatin1(); if(gi<0||gi>255)gi='?';
            for(int row=0;row<16;row++){ unsigned bits=font8x16[gi][row];
                for(int col=0;col<8;col++) if(bits&(0x80u>>col))
                    g.fillRect(x+col*uiscale, y+row*uiscale, uiscale, uiscale, c); }
            x+=gw;
        } };
    auto textC=[&](int cx,int y,const QString&s,QColor c){ text(cx-s.size()*gw/2,y,s,c); };
    auto vgrad=[&](int x,int y,int w,int h,unsigned a,unsigned b){
        for(int i=0;i<h;i++) g.fillRect(x,y+i,w,1,lerpc(a,b, h?i*256/h:0)); };

    // ---- 1. background: real image / wallpaper stand-in / drawn scene -------
    QString bgField = t.imgBackground.isSet()&&!t.imgBackground.v.isEmpty() ? t.imgBackground.v
                    : (model->g.background.isSet()? model->g.background.v : QString());
    // A background is "configured" only when the config actually sets one; with
    // no background key the firmware draws ui_background() (tree scene).
    bool bgConfigured = !bgField.isEmpty();
    QImage bgImg = loadImg(bgField);
    if(!bgImg.isNull()){
        // (a) resolvable image file -> blit it, exactly like the firmware.
        g.drawImage(QRect(0,0,W,H), bgImg);
    } else if(bgConfigured){
        // (b) an image is configured but not resolvable on this host: show the
        //     shipped-wallpaper look (navy sky, moon, mountains, stars).
        vgrad(0,0,W,H,0x0A1120u,0x14241Cu);
        { unsigned s=0x1234567u; QColor star(210,222,220); star.setAlpha(90);
          for(int i=0;i<150;i++){ s=s*1103515245u+12345u; int x=(s>>9)%W; s=s*1103515245u+12345u; int y=(s>>9)%(H*62/100);
            int sz=((s>>3)&1)+1; g.fillRect(x,y,sz,sz,star); } }
        { int mcx=FW(770), mcy=FH(210), mr=FW(150)/2; QRadialGradient rg(mcx,mcy,mr*2.4);
          QColor moon(236,239,228); rg.setColorAt(0.0,moon); rg.setColorAt(0.33,moon);
          QColor glow=moon; glow.setAlpha(55); rg.setColorAt(0.55,glow);
          QColor tr=moon; tr.setAlpha(0); rg.setColorAt(1.0,tr);
          g.setBrush(rg); g.setPen(Qt::NoPen); g.drawEllipse(QPoint(mcx,mcy),(int)(mr*2.4),(int)(mr*2.4)); }
        { auto layer=[&](int baseY,unsigned c,int n,int h){ g.setBrush(col(c)); g.setPen(Qt::NoPen); int step=W/n;
            for(int i=-1;i<=n;i++){ int cx=i*step+step/2; QPolygon tri; tri<<QPoint(cx-step,baseY)<<QPoint(cx,baseY-h)<<QPoint(cx+step,baseY); g.drawPolygon(tri);} };
          layer(H,0x122C1Eu,6,FH(300)); layer(H,0x18381Fu,8,FH(230)); layer(H,0x214A2Bu,11,FH(150)); }
    } else {
        // (c) NO background configured: the firmware draws ui_background() -
        //     theme gradient + centered tree logo + title/subtitle + rule.
        vgrad(0,0,W,H,P.bg_top,P.bg_bottom);
        // centered tree (ui_draw_tree): 3 foliage tiers + trunk.
        auto triUp=[&](int cx,int yt,int yb,int hb,unsigned c){ int span=yb-yt; if(span<=0)span=1;
            for(int y=yt;y<=yb;y++){ int half=hb*(y-yt)/span; g.fillRect(cx-half,y,half*2+1,1,col(c)); } };
        { int cx=FW(500), cy=FH(230), lw=FW(140), lh=FH(170);
          int half=lw/2, trunkW=lw/6, trunkH=lh/5, top=cy-lh/2, foliage=lh-trunkH, seg=foliage/3;
          if(half<3)half=3; if(trunkW<2)trunkW=2;
          triUp(cx,top,            top+seg+seg/2,   half/2,      P.tree3);
          triUp(cx,top+seg,        top+2*seg+seg/2, half*3/4,    P.tree2);
          triUp(cx,top+2*seg,      top+3*seg,       half,        P.tree3);
          g.fillRect(cx-trunkW/2, top+foliage, trunkW, trunkH, col(P.tree1)); }
        if(S.showTitle){ int mx=FW(20), tyy=FH(47);
          g.fillRect(mx,tyy,W-2*mx,1,col(P.border));
          // title at 2x, subtitle at 1x (firmware uses scale 2 / 1)
          int savescale=uiscale; (void)savescale;
          { int ts=uiscale*2, tw2=QString("ForeB - Forest Bootloader").size()*8*ts;
            int tx=(W-tw2)/2, tty=FH(100);
            QString s="ForeB - Forest Bootloader"; g.setPen(Qt::NoPen);
            for(int i=0;i<s.size();++i){ int gi=(unsigned char)s.at(i).toLatin1();
              for(int row=0;row<16;row++){ unsigned bits=font8x16[gi][row]; for(int c2=0;c2<8;c2++) if(bits&(0x80u>>c2)) g.fillRect(tx+ (i*8+c2)*ts, tty+row*ts, ts,ts, col(P.title)); } } }
          textC(W/2, FH(100)+22, "Forest OS Boot Manager", col(P.dim)); }
    }

    // ---- 2. menu panel geometry (ui_menu_layout) -----------------------
    int bx,by,bw,bh;
    switch(S.pos){
      case POS_LEFT:  bx=30;by=110;bw=380;bh=800;break;
      case POS_RIGHT: bx=590;by=110;bw=380;bh=800;break;
      case POS_TOP:   bx=100;by=40;bw=800;bh=320;break;
      case POS_BOTTOM:bx=100;by=560;bw=800;bh=400;break;
      case POS_FULL:  bx=40;by=40;bw=920;bh=920;break;
      default:        bx=200;by=360;bw=600;bh=420;break;
    }
    if(S.px>=0)bx=S.px; if(S.py>=0)by=S.py; if(S.pw>=0)bw=S.pw; if(S.ph>=0)bh=S.ph;
    int px=FW(bx),py=FH(by),pw=FW(bw),ph=FH(bh);
    int eh = (S.entryH>0)?FH(S.entryH):FH(55); if(eh<gh+8)eh=gh+8;
    // ui_menu_layout uses the RAW pad for vertical geometry; ui_menu clamps it
    // (>=6) only for the horizontal insets. Keep the two apart like the firmware.
    int padRaw = S.pad;
    int pad = S.pad<4?6:S.pad;
    int entriesTop = py + (S.titleBar?(8+gh+12):(padRaw+2));
    int bottom = py + ph - (S.showTimer?(gh+10):padRaw);
    int vis = (bottom-entriesTop)/eh; if(vis<1)vis=1;

    panelRect = QRect(px,py,pw,ph);

    // ---- 3. panel frame (ui_panel_frame) -------------------------------
    if(S.shadow){ g.fillRect(px+6,py+7,pw,ph,col(P.shadow));
                  g.fillRect(px+3,py+4,pw,ph,lerpc(P.shadow,P.panel,90)); }
    if(S.gradient) vgrad(px,py,pw,ph, lerpu(P.panel,P.white,16), lerpu(P.panel,P.shadow,46));
    else g.fillRect(px,py,pw,ph,col(P.panel));
    { QImage pim=loadImg(t.imgPanel.isSet()?t.imgPanel.v:QString());
      if(!pim.isNull()){ g.drawImage(QRect(px,py,pw,ph),pim);
        QColor tint=col(P.panel); tint.setAlpha(96); g.fillRect(px,py,pw,ph,tint); } }  // ui.c blends 96
    if(S.accentStrip) g.fillRect(px,py,pw,3,col(P.accent));
    auto outline=[&](int x,int y,int w,int h,int th,QColor c){
        g.fillRect(x,y,w,th,c); g.fillRect(x,y+h-th,w,th,c);
        g.fillRect(x,y,th,h,c); g.fillRect(x+w-th,y,th,h,c); };
    if(S.border=="thin")   outline(px-1,py-1,pw+2,ph+2,1,col(P.border));
    else if(S.border=="double"){ outline(px-3,py-3,pw+6,ph+6,1,col(P.border)); outline(px+1,py+1,pw-2,ph-2,1,col(P.border)); }
    else if(S.border=="shadow") outline(px-1,py-1,pw+2,ph+2,1,lerpc(P.panel,P.shadow,60));
    else if(S.border=="glow"){ outline(px-3,py-3,pw+6,ph+6,1,lerpc(P.panel,P.accent,90)); outline(px-1,py-1,pw+2,ph+2,1,col(P.accent)); }
    else if(S.border=="dashed"){ QColor c=col(P.border); for(int x=px;x<px+pw;x+=10){g.fillRect(x,py-1,6,1,c);g.fillRect(x,py+ph,6,1,c);} for(int y=py;y<py+ph;y+=10){g.fillRect(px-1,y,1,6,c);g.fillRect(px+pw,y,1,6,c);} }
    else if(S.border!="none")   outline(px-2,py-2,pw+4,ph+4,2,col(P.border)); // "thick" + firmware's default
    // corner notch
    if(S.corner!="square"){ int n=(S.corner=="round")?4:6; QColor c=col(P.shadow);
      for(int i=0;i<n;i++){int ww=n-i; g.fillRect(px,py+i,ww,1,c); g.fillRect(px+pw-ww,py+i,ww,1,c); g.fillRect(px,py+ph-1-i,ww,1,c); g.fillRect(px+pw-ww,py+ph-1-i,ww,1,c);} }
    // header
    if(S.titleBar){ int ly=py+8; text(px+pad,ly,"[ Boot Menu ]",col(P.title));
      g.fillRect(px+6,ly+gh+2,pw-12,1,col(P.accent)); g.fillRect(px+6,ly+gh+3,pw-12,1,col(P.border)); }

    // ---- 4. entries (ui_menu + ui_sel_bg) ------------------------------
    // collect top-level entry titles + icon names from the model
    QStringList labels, iconNames;
    for(const auto&e:model->roots){ labels<<(e.title.isEmpty()?(e.isSubmenu?"Submenu":"Entry"):e.title); iconNames<<e.icon; }
    if(labels.isEmpty()){ labels<<"Forest OS"<<"Linux (vmlinuz)"<<"Reboot"; iconNames<<"os"<<"tux"<<"reboot"; }
    int count = labels.size();
    if(count>0 && vis>count) vis=count;   // ui_menu_layout clamps vis to count

    int isz=eh-12; if(isz<10)isz=10;
    int caretw = S.caret?(gw+4):0;
    int gut = S.showIcons?(isz+10):0;
    int barx = px+pad + (S.iconRight?0:gut);
    bool hasBar = (count>vis) && S.scrollbar;
    int barw = pw-2*pad-gut-(hasBar?10:0);
    int cl = barx+caretw, cr = barx+barw;
    int sel=0;
    entryRects.clear();

    auto selbg=[&](int ix,int rowtop,int iw){
        int by2=rowtop, bh2=eh-2; QColor fg=col(P.white);
        if(S.sel=="none") fg=col(P.accent);
        else if(S.sel=="arrow"||S.sel=="bracket") fg=col(P.white);
        else if(S.sel=="outline") outline(ix,by2,iw,bh2,1,col(P.accent));
        else if(S.sel=="box") outline(ix,by2,iw,bh2,2,col(P.accent));
        else if(S.sel=="underline") g.fillRect(ix,by2+bh2-2,iw,2,col(P.accent));
        else if(S.sel=="invert"){ g.fillRect(ix,by2,iw,bh2,col(P.text)); fg=col(P.panel); }
        else if(S.sel=="gradient"){ for(int sx=0;sx<iw;sx++) g.fillRect(ix+sx,by2,1,bh2,lerpc(P.accent,P.panel, iw?sx*256/iw:0)); }
        else if(S.sel=="pill"){ unsigned st=lerpu(P.accent,P.white,20), sb=lerpu(P.accent,P.shadow,30);
            for(int sy=0;sy<bh2;sy++) g.fillRect(ix+3,by2+sy,iw-6,1,lerpc(st,sb, bh2?sy*256/bh2:0));
            for(int i=0;i<3;i++){   // rounded end caps (ui.c FSS_PILL)
                g.fillRect(ix+3-i,   by2+i+1, 1, bh2-2*(i+1), col(st));
                g.fillRect(ix+iw-4+i,by2+i+1, 1, bh2-2*(i+1), col(sb)); } }
        else { if(S.sel=="glow"){ g.fillRect(ix,by2-1,iw,1,lerpc(P.panel,P.accent,60)); g.fillRect(ix,by2+bh2,iw,1,lerpc(P.panel,P.accent,60)); }
            unsigned st=lerpu(P.select,P.white,14), sb=lerpu(P.select,P.shadow,24);
            for(int sy=0;sy<bh2;sy++) g.fillRect(ix,by2+sy,iw,1,lerpc(st,sb, bh2?sy*256/bh2:0));
            if(S.sel=="doublebar") g.fillRect(ix,by2,3,bh2,col(P.accent)); }
        return fg;
    };

    for(int row=0; row<vis && row<labels.size(); ++row){
        int rowtop=entriesTop+row*eh, ty=rowtop+(eh-gh)/2;
        entryRects<<QRect(px,rowtop,pw,eh);
        bool isSel=(row==sel);
        QColor fg=col(P.text);
        if(S.dividers && row>0) g.fillRect(px+pad,rowtop,pw-2*pad,1,lerpc(P.panel,P.border,120));
        if(isSel){ fg=selbg(barx,rowtop,barw);
                   if(S.caret && S.sel!="bracket") text(barx+2,ty,">",col(P.accent)); }
        QString lbl=labels[row]; int tw=lbl.size()*gw; int lx;
        if(S.align==AL_CENTER) lx=cl+(cr-cl-tw)/2; else if(S.align==AL_RIGHT) lx=cr-tw; else lx=cl;
        if(lx<cl)lx=cl;
        if(isSel && S.sel=="bracket"){ text(lx-gw,ty,"[",col(P.accent)); text(lx+tw,ty,"]",col(P.accent)); }
        text(lx,ty,lbl,fg);
        // per-entry icon: load the real TGA (icon= shorthand -> /forebo/icons/<n>.tga).
        // Right gutter shifts 8px left when a scrollbar shows (draw_icons uses
        // count>vis, ungated by the style), like bootx64.c draw_icons().
        if(S.showIcons){ int iy=rowtop+(eh-isz)/2;
            int ix=S.iconRight?(px+pw-isz-14-((count>vis)?8:0)):(px+12);
            QString icf=iconNames.value(row);
            QImage ic;
            if(!icf.isEmpty()){
                QString low=icf.toLower();
                bool literal = icf.contains('/')||icf.contains('\\')
                            || low.endsWith(".tga")||low.endsWith(".bmp");
                ic=loadImg(literal? icf : ("/forebo/icons/"+icf+".tga")); }
            if(!ic.isNull()) g.drawImage(QRect(ix,iy,isz,isz),ic);
            else if(!icf.isEmpty()){ g.fillRect(ix,iy,isz,isz,lerpc(P.panel,P.accent,60)); outline(ix,iy,isz,isz,1,col(P.border)); }
        }
    }

    // scrollbar (ui_menu_scrollbar): track + proportional thumb on the right edge
    if(hasBar){ int tx=px+pw-10, ty2=entriesTop, tw=6, th=vis*eh;
        int thh=th*vis/count; if(thh<12)thh=12; if(thh>th)thh=th;
        g.fillRect(tx,ty2,tw,th,col(P.shadow)); outline(tx,ty2,tw,th,1,col(P.border));
        g.fillRect(tx+1,ty2+1,tw-2,thh-2,col(P.accent)); }

    // ---- 5. countdown + footer -----------------------------------------
    if(S.showTimer){ QString msg="Auto-boot in 5 sec";
        if(S.gradient) for(int gy=0;gy<gh+2;gy++){   // sample the panel gradient at the strip's absolute row
            int trow=(py+ph-gh-6+gy)-py;
            g.fillRect(px+6,py+ph-gh-6+gy,pw-12,1,
                       lerpc(lerpu(P.panel,P.white,16),lerpu(P.panel,P.shadow,46), ph?trow*256/ph:0)); }
        else g.fillRect(px+6,py+ph-gh-6,pw-12,gh+2,col(P.panel));
        textC(px+pw/2,py+ph-gh-6,msg,col(P.timer)); }
    if(S.showFooter) textC(W/2,FH(940),"[Up/Down] Navigate  [Enter] Boot  [Esc] Reset",col(P.dim));

    // ---- 6. sample window (wm.c draw_one), honoring window_skin + win_* -----
    // Window chrome uses the RAW config colors (FOREB_DEF_* defaults), NOT the
    // named menu palette - wm.c never sees ui.c's g_pal.
    {
        QString skin = t.windowSkin.isEmpty()?"beveled":t.windowSkin;
        unsigned winCol = t.colorWindow.isSet()? t.colorWindow.v : 0x16241Bu;   // FOREB_DEF_COLOR_WINDOW
        unsigned tbCol  = t.winTitleFill.isSet()? t.winTitleFill.v
                        : (t.colorTitlebar.isSet()? t.colorTitlebar.v : 0x1F5E3Au);
        unsigned tbFg   = t.winTitleFg.isSet()? t.winTitleFg.v
                        : (t.colorSelFg.isSet()? t.colorSelFg.v : 0xFFFFFFu);
        unsigned winFg  = t.colorFg.isSet()? t.colorFg.v : 0xDDE7DEu;
        unsigned wacc   = t.colorAccent.isSet()? t.colorAccent.v : 0x3FB56Bu;   // wm accent = raw color_accent
        unsigned bord   = t.winBorderColor.isSet()? t.winBorderColor.v : wacc;
        unsigned closeC = t.winCloseColor.isSet()? t.winCloseColor.v : 0xB03030u;
        int bw = (t.winBorderW.isSet()&&t.winBorderW.v>=0)? t.winBorderW.v : 1;
        int titleH = (t.winTitleH.isSet()&&t.winTitleH.v>=0)? t.winTitleH.v : qMax(gh+8,22);
        int shadow = t.winShadow.isSet()? (t.winShadow.v?1:0) : 1;
        QString wcorner = t.winCorner.isEmpty()?"square":t.winCorner;
        if(skin=="glass") tbCol = lerpu(tbCol,winCol,96);   // wm_blend(tb,cli,96)

        int wx=FW(60),wy=FH(70),ww=FW(470),wh=FH(300);
        windowRect=QRect(wx,wy,ww,wh);
        if(shadow) g.fillRect(wx+4,wy+4,ww,wh,col(0x040804));
        // Client fill: glass skin + fx on -> frosted backdrop + translucent tint.
        if(skin=="glass" && t.fxGlass.value(false)){
            blurRegion(img, wx,wy,ww,wh, t.fxBlur.isSet()? t.fxBlur.v : 8);
            darkenRegion(img, wx,wy,ww,wh, t.fxOpacity.isSet()? t.fxOpacity.v : 72);
            QColor cc=col(winCol); cc.setAlpha(170); g.fillRect(wx,wy,ww,wh,cc);
        } else g.fillRect(wx,wy,ww,wh,col(winCol));
        { QImage wim=loadImg(t.imgWindow.isSet()?t.imgWindow.v:QString());
          if(!wim.isNull()) g.drawImage(QRect(wx,wy,ww,wh),wim); }
        if(skin=="beveled"){ g.fillRect(wx,wy,ww,1,col(0xA8C0AE)); g.fillRect(wx,wy,1,wh,col(0xA8C0AE));
            g.fillRect(wx,wy+wh-1,ww,1,col(0x060B08)); g.fillRect(wx+ww-1,wy,1,wh,col(0x060B08)); }
        else outline(wx,wy,ww,wh,bw>0?bw:1,col(bord));
        g.fillRect(wx,wy,ww,titleH,col(tbCol));
        { QImage tim=loadImg(t.imgTitlebar.isSet()?t.imgTitlebar.v:QString());
          if(!tim.isNull()) g.drawImage(QRect(wx,wy,ww,titleH),tim); }
        g.fillRect(wx,wy+titleH-1,ww,1,col(wacc));
        text(wx+8, wy+(titleH-gh)/2, "Settings", col(tbFg));
        // close box (top-right) with an 'x'
        int cs=titleH-8; if(cs<10)cs=10; int cbx=wx+ww-cs-4, cby=wy+(titleH-cs)/2;
        g.fillRect(cbx,cby,cs,cs,col(closeC));
        text(cbx+(cs-gw)/2, cby+(cs-gh)/2, "x", col(0xFFFFFF));
        // corner notch LAST, in the drop-shadow colour, so round/cut is visible
        if(wcorner!="square"){ int n=(wcorner=="round")?4:6; QColor c=col(0x040804);
            for(int i=0;i<n;i++){int wwd=n-i; g.fillRect(wx,wy+i,wwd,1,c); g.fillRect(wx+ww-wwd,wy+i,wwd,1,c);} }
        // client content: a couple of rows + a sample button
        text(wx+10, wy+titleH+8, "Live window preview", col(winFg));
        text(wx+10, wy+titleH+8+gh+6, "Buttons follow win_button_style", col(lerpu(winFg,winCol,80)));
        // --- sample button, EXACT port of wm.c wm_button_draw --------------
        { int btw=FW(90), bth=gh+8*uiscale, btx=wx+ww-btw-10, bty=wy+wh-bth-8;
          QString bs = t.winButtonStyle;   // wm.c consults winskin ONLY (no btn_style fallback)
          unsigned face = lerpu(winCol,0xFFFFFF,30);   // normal (raised) face
          g.fillRect(btx,bty,btw,bth,col(face));
          QImage bim=loadImg(t.imgButton.isSet()?t.imgButton.v:QString());
          if(!bim.isNull()) g.drawImage(QRect(btx,bty,btw,bth),bim);
          bool bevel = (bs.isEmpty() && skin=="beveled") || bs=="raised";
          if(bs=="flat"||bs=="ghost"){ /* no edge */ }
          else if(bs=="outline") outline(btx,bty,btw,bth,1,lerpu(winCol,winFg,100)); // normal state
          else if(bevel){ unsigned hi=0xA8C0AE, lo=0x060B08;
              g.fillRect(btx,bty,btw,1,col(hi)); g.fillRect(btx,bty,1,bth,col(hi));
              g.fillRect(btx,bty+bth-1,btw,1,col(lo)); g.fillRect(btx+btw-1,bty,1,bth,col(lo)); }
          else outline(btx,bty,btw,bth,1,lerpu(winCol,winFg,80));   // pill/glass/unset+non-beveled
          unsigned btfg = winFg;   // wm_button_draw labels in color_fg on the face
          text(btx+(btw-2*gw)/2, bty+(bth-gh)/2, "OK", col(btfg)); }
    }

    // ---- 7. full-screen effects: two sequential multiplicative passes ------
    // (bootx64 calls ui_vignette THEN ui_scanlines; each scales lanes by
    // (255-amt)/255 with truncating division, so composition is multiplicative).
    int vig = t.fxVignette.isSet()?t.fxVignette.v:0;
    int scan = t.fxScanlines.isSet()?t.fxScanlines.v:0;
    if(vig>0){
        int cx=W/2, cy=H/2; long maxd=(long)cx*cx+(long)cy*cy; if(maxd<1)maxd=1;
        for(int y=0;y<H;y++){ QRgb *row=(QRgb*)img.scanLine(y);
            for(int x=0;x<W;x++){ long dx=x-cx, dy=y-cy;
                int amt=(int)(((long long)(dx*dx+dy*dy)*vig)/maxd); if(amt<=0) continue;
                int keep=255-amt; QRgb p=row[x];
                row[x]=qRgb(qRed(p)*keep/255,qGreen(p)*keep/255,qBlue(p)*keep/255); } }
    }
    if(scan>0){ int keep=255-scan;
        for(int y=1;y<H;y+=2){ QRgb *row=(QRgb*)img.scanLine(y);
            for(int x=0;x<W;x++){ QRgb p=row[x];
                row[x]=qRgb(qRed(p)*keep/255,qGreen(p)*keep/255,qBlue(p)*keep/255); } }
    }

    // ---- 8. mouse cursor sprite (input.c CURSOR), color_cursor / img_cursor --
    // Firmware draws it only when cursor_enabled AND a mouse is present
    // (mouse_enabled=0 -> no pointer device -> no cursor).
    if(t.cursorEnabled.value(true) && t.mouseEnabled.value(true)){
        int curx=px+FW(40), cury=entriesTop+eh+eh/3;   // over the menu, like the real UI
        QString cf = (t.imgCursor.isSet() && !t.imgCursor.v.isEmpty()) ? t.imgCursor.v
                   : (t.cursorPath.isSet()? t.cursorPath.v : QString());
        QImage cim=loadImg(cf);
        // firmware rejects cursor images >64x64 and falls back to the arrow
        if(!cim.isNull() && cim.width()<=64 && cim.height()<=64)
            g.drawImage(QRect(curx,cury,cim.width()*uiscale,cim.height()*uiscale),cim);
        else {
            static const char *CUR[] = {
              "2           ","22          ","212         ","2112        ","21112       ",
              "211112      ","2111112     ","21111112    ","211111112   ","2111111112  ",
              "21111111112 ","211111122222","211112112   ","21112 2112  ","2112  2112  ",
              "212    2112 ","22     2112 ","2       212 ","         22 "};
            // live_cursor_col(): color_cursor 0/unset -> white
            unsigned body = (t.colorCursor.isSet() && t.colorCursor.v!=0) ? t.colorCursor.v : 0xFFFFFF;
            for(int r=0;r<19;r++){ const char*rowp=CUR[r];
              for(int c2=0;rowp[c2];c2++){ unsigned cc; if(rowp[c2]=='1')cc=body; else if(rowp[c2]=='2')cc=0x101010; else continue;
                g.fillRect(curx+c2*uiscale, cury+r*uiscale, uiscale, uiscale, col(cc)); } }
        }
    }

    g.end();

    // ---- letterbox-scale the native image into the widget --------------
    QPainter p(this);
    p.fillRect(rect(),QColor(20,22,26));
    double s = qMin((double)width()/W,(double)height()/H);
    int dw=(int)(W*s), dh=(int)(H*s), ox=(width()-dw)/2, oy=(height()-dh)/2;
    p.setRenderHint(QPainter::SmoothPixmapTransform,true);
    p.drawImage(QRect(ox,oy,dw,dh), img);
    // remember transform so clicks map back to native coords for hit-testing
    lastScale=s; lastOx=ox; lastOy=oy;
}

void PreviewWidget::mousePressEvent(QMouseEvent *e) {
    // map widget click -> native framebuffer coords
    int nx=(int)((e->pos().x()-lastOx)/(lastScale>0?lastScale:1));
    int ny=(int)((e->pos().y()-lastOy)/(lastScale>0?lastScale:1));
    QPoint np(nx,ny);
    if(windowRect.contains(np)){ emit elementClicked("window",-1); return; }
    for (int i=0;i<entryRects.size();++i)
        if(entryRects[i].contains(np)){ emit elementClicked("entry",i); return; }
    if(panelRect.contains(np)){ emit elementClicked("panel",-1); return; }
}
