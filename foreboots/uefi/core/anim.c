/*
 * anim.c - GOP framebuffer animation helpers for the ForeB UEFI loader.
 *
 * Self-contained, freestanding (no libc). Writes 32bpp pixels straight to the
 * GOP linear framebuffer, handling BGRX (x86 default) and RGBX byte orders
 * exactly like ui.c. See anim.h for the API contract.
 */
#include "anim.h"
#include "../ui.h"                       /* ui_width/height, ui_scale, ui_progress */
#include "../../include/forebo_theme.h"  /* forest palette                          */

/* ------------------------------------------------------------------ */
/*  Framebuffer state (owned, mirrors ui.c)                            */
/* ------------------------------------------------------------------ */
static volatile UINT8 *a_fb    = 0;   /* framebuffer base as byte ptr  */
static UINT32 a_pitch          = 0;   /* bytes per scanline            */
static UINT32 a_w              = 0;   /* width  in pixels              */
static UINT32 a_h              = 0;   /* height in pixels              */
static int    a_swap_rb        = 0;   /* 1 => framebuffer is RGBX      */
static EFI_BOOT_SERVICES *a_bs = 0;   /* for Stall + AllocatePool      */

/* Full-screen snapshot of the static background (native fb words, tight
 * w*h stride). NULL when the allocation failed => fade/particles degrade. */
static UINT32 *a_snap          = 0;

/* PixelRedGreenBlueReserved8BitPerColor is enum value 0 in efi.h. */
#define A_PIXFMT_RGBX 0u

/* Convert a logical 0x00RRGGBB color to the framebuffer's byte order. */
static inline UINT32 a_pack(UINT32 c)
{
    if (a_swap_rb) {
        return (c & 0x0000FF00u)
             | ((c & 0x00FF0000u) >> 16)
             | ((c & 0x000000FFu) << 16);
    }
    return c & 0x00FFFFFFu;
}

static inline void a_put(int x, int y, UINT32 native)
{
    if (!a_fb) return;
    if ((UINT32)x >= a_w || (UINT32)y >= a_h) return;   /* negatives wrap huge */
    *(volatile UINT32 *)(a_fb + (UINTN)y * a_pitch + (UINTN)x * 4u) = native;
}

static inline UINT32 a_get(int x, int y)
{
    if (!a_fb) return 0;
    if ((UINT32)x >= a_w || (UINT32)y >= a_h) return 0;
    return *(volatile UINT32 *)(a_fb + (UINTN)y * a_pitch + (UINTN)x * 4u);
}

/* Alpha-composite a solid source over a rectangle of the framebuffer. The
 * source is constant across the whole rect, so the caller pre-packs it to
 * native order (channels s0/s1/s2) and pre-computes the clamped alpha and its
 * complement ia = 255-alpha once; this routine only reads/writes the fb. The
 * rect is clipped to the framebuffer here (like a_restore), then walked with a
 * per-row base pointer so the address math is a single add per pixel and each
 * pixel does exactly one bounds-free read + write. Bit-identical to the old
 * per-pixel a_blend()/a_pack()/a_get()/a_put() path. */
static void a_blend_rect(int x, int y, int w, int h,
                         UINT32 s0, UINT32 s1, UINT32 s2,
                         UINT32 alpha, UINT32 ia)
{
    int yy, xx;
    if (!a_fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if ((UINT32)(x + w) > a_w) w = (int)a_w - x;
    if ((UINT32)(y + h) > a_h) h = (int)a_h - y;
    if (w <= 0 || h <= 0) return;
    for (yy = 0; yy < h; yy++) {
        volatile UINT32 *row =
            (volatile UINT32 *)(a_fb + (UINTN)(y + yy) * a_pitch + (UINTN)x * 4u);
        for (xx = 0; xx < w; xx++) {
            UINT32 d = row[xx];
            UINT32 d0 = d & 0xFF, d1 = (d >> 8) & 0xFF, d2 = (d >> 16) & 0xFF;
            /* Exact /255 without a divide: for t in 0..65025 (s,d,alpha,ia all
             * 0..255) this is bit-identical to t/255u. */
            UINT32 t0 = s0 * alpha + d0 * ia;
            UINT32 t1 = s1 * alpha + d1 * ia;
            UINT32 t2 = s2 * alpha + d2 * ia;
            UINT32 o0 = (t0 + 0x80u + ((t0 + 0x80u) >> 8)) >> 8;
            UINT32 o1 = (t1 + 0x80u + ((t1 + 0x80u) >> 8)) >> 8;
            UINT32 o2 = (t2 + 0x80u + ((t2 + 0x80u) >> 8)) >> 8;
            row[xx] = (o2 << 16) | (o1 << 8) | o0;
        }
    }
}

/* Copy a rectangle of the captured snapshot back to the framebuffer. */
static void a_restore(int x, int y, int w, int h)
{
    int yy, xx;
    if (!a_snap) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if ((UINT32)(x + w) > a_w) w = (int)a_w - x;
    if ((UINT32)(y + h) > a_h) h = (int)a_h - y;
    if (w <= 0 || h <= 0) return;
    if (!a_fb) return;
    /* Rect is fully clipped to the framebuffer now: walk snapshot + fb row
     * pointers with no per-pixel multiply or bounds recheck. */
    for (yy = 0; yy < h; yy++) {
        const UINT32 *src = a_snap + (UINTN)(y + yy) * a_w + (UINT32)x;
        volatile UINT32 *dst =
            (volatile UINT32 *)(a_fb + (UINTN)(y + yy) * a_pitch + (UINTN)x * 4u);
        for (xx = 0; xx < w; xx++)
            dst[xx] = src[xx];
    }
}

/* ------------------------------------------------------------------ */
/*  Tiny PRNG (xorshift32, seeded from the TSC)                        */
/* ------------------------------------------------------------------ */
static UINT32 a_rng = 0x1234567u;

static inline UINT64 a_rdtsc(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    UINT32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((UINT64)hi << 32) | lo;
#else
    /* No rdtsc outside x86. This only seeds a PRNG for particle jitter, so a
     * self-incrementing counter (golden-ratio step) is a fine, trap-free source
     * on aarch64/riscv. */
    static UINT64 c = 0x9E3779B97F4A7C15ull;
    c += 0x9E3779B97F4A7C15ull;
    return c;
#endif
}

static UINT32 a_rand(void)
{
    UINT32 x = a_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    a_rng = x;
    return x;
}

/* ------------------------------------------------------------------ */
/*  init                                                               */
/* ------------------------------------------------------------------ */
void anim_init(UINT64 fb_base, UINT32 pitch, UINT32 width, UINT32 height,
               UINT32 pixfmt, EFI_BOOT_SERVICES *bs)
{
    a_fb      = (volatile UINT8 *)(UINTN)fb_base;
    a_pitch   = pitch;
    a_w       = width;
    a_h       = height;
    a_swap_rb = (pixfmt == A_PIXFMT_RGBX) ? 1 : 0;
    a_bs      = bs;
    a_rng     = (UINT32)a_rdtsc() | 1u;

    if (!a_snap && a_bs && a_w && a_h) {
        VOID *p = 0;
        UINTN bytes = (UINTN)a_w * (UINTN)a_h * 4u;
        if (!EFI_ERROR(a_bs->AllocatePool(EfiLoaderData, bytes, &p)) && p)
            a_snap = (UINT32 *)p;
    }
}

void anim_capture(void)
{
    UINT32 y, x;
    if (!a_snap || !a_fb) return;
    /* Walk row base pointers: one add per pixel, no per-pixel bounds branch or
     * multiply (x,y are always in range here). */
    for (y = 0; y < a_h; y++) {
        UINT32 *dst = a_snap + (UINTN)y * a_w;
        volatile UINT32 *src = (volatile UINT32 *)(a_fb + (UINTN)y * a_pitch);
        for (x = 0; x < a_w; x++)
            dst[x] = src[x];
    }
}

/* ------------------------------------------------------------------ */
/*  fade-in                                                            */
/* ------------------------------------------------------------------ */
void anim_fade_in(int frames, int step_ms)
{
    int k;
    UINT32 y, x;
    if (frames < 1) frames = 1;
    if (!a_snap || !a_fb) {
        /* No snapshot: just hold for the same wall time so the caller's flow
         * is unchanged. */
        if (a_bs) for (k = 0; k < frames; k++) a_bs->Stall((UINTN)step_ms * 1000u);
        return;
    }
    for (k = 1; k <= frames; k++) {
        /* One 256-entry brightness table for this step, then a bounds-free
         * row-pointer walk: no per-pixel divide, bounds check, or a_put(). */
        UINT8 lut[256];
        int v;
        for (v = 0; v < 256; v++)
            lut[v] = (UINT8)((UINT32)v * (UINT32)k / (UINT32)frames);
        for (y = 0; y < a_h; y++) {
            const UINT32 *src = a_snap + (UINTN)y * a_w;
            volatile UINT32 *dst = (volatile UINT32 *)(a_fb + (UINTN)y * a_pitch);
            for (x = 0; x < a_w; x++) {
                UINT32 w = src[x];
                dst[x] = ((UINT32)lut[(w >> 16) & 0xFFu] << 16)
                       | ((UINT32)lut[(w >> 8)  & 0xFFu] << 8)
                       |  (UINT32)lut[ w        & 0xFFu];
            }
        }
        /* Flip this fade step to the screen (no-op when not double-buffered,
         * since we already wrote VRAM directly in that case). These writes
         * bypass the primitives, so force a whole-screen flip. */
        ui_mark_all();
        ui_present();
        if (a_bs) a_bs->Stall((UINTN)step_ms * 1000u);
    }
    /* Snap to the exact captured image via row pointers. */
    for (y = 0; y < a_h; y++) {
        const UINT32 *src = a_snap + (UINTN)y * a_w;
        volatile UINT32 *dst = (volatile UINT32 *)(a_fb + (UINTN)y * a_pitch);
        for (x = 0; x < a_w; x++)
            dst[x] = src[x];
    }
    ui_mark_all();
    ui_present();
}

/* ------------------------------------------------------------------ */
/*  fade-out                                                           */
/* ------------------------------------------------------------------ */
void anim_fade_out(int frames, int step_ms)
{
    int k;
    UINT32 y, x;
    if (frames < 1) frames = 1;
    if (!a_snap || !a_fb) {
        /* No snapshot/back buffer: hold briefly for the same wall time so the
         * caller's flow is unchanged, then slam the visible screen to black. */
        if (a_bs) a_bs->Stall((UINTN)step_ms * 1000u);
        if (a_fb) {
            for (y = 0; y < a_h; y++)
                for (x = 0; x < a_w; x++)
                    a_put((int)x, (int)y, 0u);
            ui_mark_all();
            ui_present();
        }
        return;
    }
    /* Snapshot the CURRENT framebuffer contents (menu/particles included), then
     * ramp those captured pixels toward 0x000000. This deliberately overwrites
     * the static-background snapshot, which is fine: fade-out is the last thing
     * that runs before a boot handoff. */
    anim_capture();
    for (k = frames - 1; k >= 1; k--) {
        /* One brightness table for this step, then a bounds-free row-pointer
         * walk: no per-pixel divide, bounds check, or a_put(). */
        UINT8 lut[256];
        int v;
        for (v = 0; v < 256; v++)
            lut[v] = (UINT8)((UINT32)v * (UINT32)k / (UINT32)frames);
        for (y = 0; y < a_h; y++) {
            const UINT32 *src = a_snap + (UINTN)y * a_w;
            volatile UINT32 *dst = (volatile UINT32 *)(a_fb + (UINTN)y * a_pitch);
            for (x = 0; x < a_w; x++) {
                UINT32 w = src[x];
                dst[x] = ((UINT32)lut[(w >> 16) & 0xFFu] << 16)
                       | ((UINT32)lut[(w >> 8)  & 0xFFu] << 8)
                       |  (UINT32)lut[ w        & 0xFFu];
            }
        }
        /* These writes bypass the ui.c primitives, so force a whole-screen flip
         * to make each darkening step visible. */
        ui_mark_all();
        ui_present();
        if (a_bs) a_bs->Stall((UINTN)step_ms * 1000u);
    }
    /* Snap to a fully black screen via row pointers. */
    for (y = 0; y < a_h; y++) {
        volatile UINT32 *dst = (volatile UINT32 *)(a_fb + (UINTN)y * a_pitch);
        for (x = 0; x < a_w; x++)
            dst[x] = 0u;
    }
    ui_mark_all();
    ui_present();
}

/* ------------------------------------------------------------------ */
/*  integer easing helper                                              */
/* ------------------------------------------------------------------ */
/* Quadratic ease-out interpolation from `from` to `to` at `step` of `steps`
 * (both inclusive endpoints: step==0 -> from, step>=steps -> to). Used by the
 * menu highlight slide (see GUI_TOOLS.md). */
int anim_lerp(int from, int to, int step, int steps)
{
    int t, ease;
    if (steps < 1) return to;
    if (step <= 0) return from;
    if (step >= steps) return to;
    /* Ease-out quad on a 0..steps parameter: 1-(1-t)^2, kept in integers by
     * scaling by steps*steps. ease = steps*steps - (steps-step)^2. */
    t = steps - step;
    ease = steps * steps - t * t;                 /* 0..steps*steps */
    return from + (to - from) * ease / (steps * steps);
}

/* ------------------------------------------------------------------ */
/*  particle layer                                                     */
/* ------------------------------------------------------------------ */
#define ANIM_MAX_PARTICLES 96

typedef struct {
    int    x, y;      /* current position (top-left of the drawn square) */
    int    vy;        /* vertical speed (px/tick)                        */
    int    drift;     /* horizontal drift (px/tick, small)              */
    int    size;      /* square side in px                              */
    int    alpha;     /* blend alpha 0..255                             */
    UINT32 color;     /* 0x00RRGGBB                                     */
    UINT32 pcolor;    /* color pre-packed to native fb order (a_pack)    */
} a_particle;

static a_particle a_parts[ANIM_MAX_PARTICLES];
static int a_nparts = 0;

/* Optional exclusion rect (px, screen space): particles are never seeded into
 * nor advanced across it, so an overlaid panel is not dirtied every tick. A
 * zero/empty rect (w<=0 || h<=0) disables the exclusion. */
static int a_excl_x = 0, a_excl_y = 0, a_excl_w = 0, a_excl_h = 0;

void anim_particles_set_exclude(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) { a_excl_w = 0; a_excl_h = 0; return; }
    a_excl_x = x; a_excl_y = y; a_excl_w = w; a_excl_h = h;
}

/* True if the sz-square at (x,y) overlaps the active exclusion rect. */
static inline int a_hits_excl(int x, int y, int sz)
{
    if (a_excl_w <= 0 || a_excl_h <= 0) return 0;
    return x < a_excl_x + a_excl_w && a_excl_x < x + sz &&
           y < a_excl_y + a_excl_h && a_excl_y < y + sz;
}

/* Small palettes. Leaves = greens, embers = amber. */
static const UINT32 a_leaf_pal[4] = {
    FOREB_TREE3, FOREB_TITLE, FOREB_TEXT, 0x006FB63Du
};
static const UINT32 a_ember_pal[4] = {
    FOREB_TIMER, 0x00FF7818u, 0x00FFC040u, 0x00E08010u
};

/* Optional theme tint for particles (set by anim_set_tint). When active it
 * replaces the built-in leaf/ember palettes so the drifting motes match the
 * selected UI skin. */
static int    a_tinted = 0;
static UINT32 a_tint_pal[4];

void anim_set_tint(UINT32 accent, UINT32 title)
{
    if (!accent && !title) { a_tinted = 0; return; }
    if (!accent) accent = title;
    if (!title)  title  = accent;
    /* Build a 4-stop palette: accent, title, and two blends between them. */
    UINT32 a = accent & 0x00FFFFFFu, b = title & 0x00FFFFFFu;
    a_tint_pal[0] = a;
    a_tint_pal[3] = b;
    for (int k = 1; k <= 2; k++) {
        UINT32 t = (UINT32)k * 255u / 3u;
        UINT32 r = (((a >> 16) & 0xFF) * (255 - t) + ((b >> 16) & 0xFF) * t) / 255u;
        UINT32 g = (((a >> 8)  & 0xFF) * (255 - t) + ((b >> 8)  & 0xFF) * t) / 255u;
        UINT32 bl = (((a)      & 0xFF) * (255 - t) + ((b)       & 0xFF) * t) / 255u;
        a_tint_pal[k] = (r << 16) | (g << 8) | bl;
    }
    a_tinted = 1;
}

void anim_particles_init(int count, int style)
{
    int i;
    a_nparts = 0;
    if (!a_snap || !a_fb || a_w == 0 || a_h == 0) return;
    if (count < 0) count = 0;
    if (count > ANIM_MAX_PARTICLES) count = ANIM_MAX_PARTICLES;
    if (a_h < 720 && count > 48) count = 48;
    for (i = 0; i < count; i++) {
        int sz = 2 + (int)(a_rand() % 3u);            /* 2..4 px      */
        int px, py, tries;
        /* Reject seed positions whose sz-square would land on the excluded
         * panel; retry a few times, then give up (the particle will simply not
         * draw until it drifts clear, since step() also skips overlaps). */
        px = (int)(a_rand() % a_w);
        py = (int)(a_rand() % a_h);
        for (tries = 0; tries < 8 && a_hits_excl(px, py, sz); tries++) {
            px = (int)(a_rand() % a_w);
            py = (int)(a_rand() % a_h);
        }
        a_parts[i].size  = sz;
        a_parts[i].x     = px;
        a_parts[i].y     = py;
        a_parts[i].vy    = 1 + sz / 2;                 /* bigger => faster (parallax) */
        a_parts[i].drift = (int)(a_rand() % 3u) - 1;   /* -1,0,1       */
        a_parts[i].alpha = 70 + sz * 28;               /* ~126..182    */
        if (a_tinted)
            a_parts[i].color = a_tint_pal[a_rand() & 3u];
        else if (style)
            a_parts[i].color = a_ember_pal[a_rand() & 3u];
        else
            a_parts[i].color = a_leaf_pal[a_rand() & 3u];
        /* Pre-pack to native fb order once; step() reads pcolor each tick. */
        a_parts[i].pcolor = a_pack(a_parts[i].color);
    }
    a_nparts = count;
}

void anim_particles_step(void)
{
    int i;
    if (!a_snap || !a_fb || a_nparts == 0) return;
    for (i = 0; i < a_nparts; i++) {
        a_particle *p = &a_parts[i];
        int ox = p->x, oy = p->y, sz = p->size;
        int old_excl = a_hits_excl(ox, oy, sz);
        int new_excl;
        /* Erase where we drew last tick (exact region => no trails). Skip when
         * the old square sat on the excluded panel: it was never drawn there,
         * so restoring the background snapshot would dirty panel pixels. */
        if (!old_excl)
            a_restore(ox, oy, sz, sz);
        /* Advance. */
        p->y += p->vy;
        p->x += p->drift;
        if (p->y >= (int)a_h) { p->y = -(p->size); p->x = (int)(a_rand() % a_w); }
        if (p->x < 0)             p->x += (int)a_w;
        else if (p->x >= (int)a_w) p->x -= (int)a_w;
        {
            int nx = p->x, ny = p->y;
            new_excl = a_hits_excl(nx, ny, sz);
            /* Draw. The particle color + alpha are constant over the whole
             * square, so read the pre-packed color and clamp the alpha once
             * here (loop-invariant), leaving only the destination read+blend
             * inside a_blend_rect(). Skip entirely when the new square overlaps
             * the excluded panel so those pixels are never touched. */
            if (!new_excl) {
                UINT32 pc = p->pcolor;
                UINT32 s0 = pc & 0xFF, s1 = (pc >> 8) & 0xFF, s2 = (pc >> 16) & 0xFF;
                int alpha = p->alpha;
                if (alpha > 255) alpha = 255;
                if (alpha >= 200 && sz <= 4) {
                    for (int dy = 0; dy < sz; dy++) {
                        volatile UINT32 *row = (volatile UINT32 *)(a_fb + (UINTN)(ny + dy) * a_pitch + (UINTN)nx * 4u);
                        for (int dx = 0; dx < sz; dx++) {
                            if ((UINT32)(nx+dx) < a_w && (UINT32)(ny+dy) < a_h)
                                row[dx] = pc;
                        }
                    }
                    ui_mark_dirty(nx, ny, sz, sz);
                } else if (alpha > 0) {
                    a_blend_rect(nx, ny, sz, sz,
                                 s0, s1, s2, (UINT32)alpha, 255u - (UINT32)alpha);
                }
            }
            /* These writes bypass the ui.c primitives, so flag the erased and
             * freshly-drawn squares for the partial-present pass. vy>=1 so the
             * two squares usually overlap; mark their bounding union as one rect
             * in that case to avoid re-presenting the shared area twice. Never
             * mark a square that fell on the panel: those pixels stay owned by
             * the panel and must not be re-presented. */
            if (!old_excl && !new_excl &&
                ox < nx + sz && nx < ox + sz && oy < ny + sz && ny < oy + sz) {
                int ux  = (ox < nx) ? ox : nx;
                int uy  = (oy < ny) ? oy : ny;
                int ux2 = ((ox > nx) ? ox : nx) + sz;
                int uy2 = ((oy > ny) ? oy : ny) + sz;
                ui_mark_dirty(ux, uy, ux2 - ux, uy2 - uy);
            } else {
                if (!old_excl) ui_mark_dirty(ox, oy, sz, sz);
                if (!new_excl) ui_mark_dirty(nx, ny, sz, sz);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  spinner                                                            */
/* ------------------------------------------------------------------ */
/* Unit-circle offsets (x8) for the 8 dots, clockwise from the top. */
static const int a_spin_dx[8] = {  0,  5,  7,  5,  0, -5, -7, -5 };
static const int a_spin_dy[8] = { -7, -5,  0,  5,  7,  5,  0, -5 };

/* Scale a 0x00RRGGBB color's brightness by lvl/255. */
static UINT32 a_dim(UINT32 c, int lvl)
{
    UINT32 r = ((c >> 16) & 0xFFu) * (UINT32)lvl / 255u;
    UINT32 g = ((c >> 8) & 0xFFu) * (UINT32)lvl / 255u;
    UINT32 b = (c & 0xFFu) * (UINT32)lvl / 255u;
    return (r << 16) | (g << 8) | b;
}

void anim_spinner(int cx, int cy, int phase, UINT32 color, int scale)
{
    int k, dsz, ux, uy, ddx, ddy;
    if (!a_fb) return;
    if (scale < 1) scale = 1;
    dsz = 2 * scale;
    for (k = 0; k < 8; k++) {
        int rel = (phase - k) & 7;              /* 0 = bright head          */
        int lvl = 255 - rel * 30;
        UINT32 c;
        if (lvl < 40) lvl = 40;
        c = a_pack(a_dim(color, lvl));
        /* a_spin_* are a ~7px-radius unit ring; magnify by `scale`. */
        ux = cx + a_spin_dx[k] * scale;
        uy = cy + a_spin_dy[k] * scale;
        for (ddy = 0; ddy < dsz; ddy++)
            for (ddx = 0; ddx < dsz; ddx++)
                a_put(ux + ddx, uy + ddy, c);
    }
    /* Mark the spinner's bounding box dirty so ui_present() flips it to VRAM
     * without requiring the caller to force a full-screen flip. */
    ui_mark_dirty(cx - 7 * scale, cy - 7 * scale, 16 * scale, 16 * scale);
}

/* Snapshot buffer for the spinner region, so old dots are erased before each
 * new phase is drawn. Without this, previous frame dots accumulate as a static
 * ring of varying brightness. The snapshot is allocated lazily on first use. */
static UINT32 *a_spin_snap = 0;
static int     a_spin_snap_w = 0, a_spin_snap_h = 0;
static int     a_spin_snap_x = 0, a_spin_snap_y = 0;

static void a_spin_snap_free(void)
{
    if (a_spin_snap && a_bs) {
        a_bs->FreePool(a_spin_snap);
        a_spin_snap = 0;
    }
    a_spin_snap_w = a_spin_snap_h = 0;
}

static void a_spin_restore(void)
{
    if (!a_spin_snap || !a_fb) return;
    for (int yy = 0; yy < a_spin_snap_h; yy++) {
        const UINT32 *src = a_spin_snap + (UINTN)yy * (UINTN)a_spin_snap_w;
        volatile UINT32 *dst =
            (volatile UINT32 *)(a_fb + (UINTN)(a_spin_snap_y + yy) * a_pitch
                                     + (UINTN)a_spin_snap_x * 4u);
        for (int xx = 0; xx < a_spin_snap_w; xx++)
            dst[xx] = src[xx];
    }
}

/* Snapshot the spinner's bounding box before drawing the new phase, then draw. */
static void a_spin_draw(int cx, int cy, int phase, UINT32 color, int scale)
{
    int bx = cx - 7 * scale, by = cy - 7 * scale;
    int bw = 16 * scale,     bh = 16 * scale;

    /* Clip to framebuffer. */
    if (bx < 0) { bw += bx; bx = 0; }
    if (by < 0) { bh += by; by = 0; }
    if (bx + bw > (int)a_w) bw = (int)a_w - bx;
    if (by + bh > (int)a_h) bh = (int)a_h - by;
    if (bw <= 0 || bh <= 0) return;

    /* Allocate (or reallocate) snapshot buffer when size changed. */
    if (a_spin_snap_w != bw || a_spin_snap_h != bh) {
        a_spin_snap_free();
        VOID *p = 0;
        UINTN bytes = (UINTN)bw * (UINTN)bh * 4u;
        if (!EFI_ERROR(a_bs->AllocatePool(EfiLoaderData, bytes, &p)) && p)
            a_spin_snap = (UINT32 *)p;
        a_spin_snap_w = bw;
        a_spin_snap_h = bh;
    }

    /* Snapshot the region (erases previous frame's dots when restored). */
    if (a_spin_snap) {
        a_spin_snap_x = bx;
        a_spin_snap_y = by;
        for (int yy = 0; yy < bh; yy++) {
            const volatile UINT32 *src =
                (const volatile UINT32 *)(a_fb + (UINTN)(by + yy) * a_pitch
                                                + (UINTN)bx * 4u);
            UINT32 *dst = a_spin_snap + (UINTN)yy * (UINTN)bw;
            for (int xx = 0; xx < bw; xx++)
                dst[xx] = src[xx];
        }
    }

    anim_spinner(cx, cy, phase, color, scale);
}

void anim_load_spinner(int phase)
{
    int bx = (int)((UINTN)a_w * 220u / 1000u);
    int by = (int)((UINTN)a_h * 860u / 1000u);
    int bw = (int)((UINTN)a_w * 560u / 1000u);
    int bh = (int)((UINTN)a_h * 30u / 1000u);
    int sc = 1;
    int cx = bx + bw + 16;
    int cy = by + bh / 2;
    if (a_h >= 1080) sc = 2;
    /* Keep the spinner on-screen: fall back to the bar's left if the right
     * gutter is too tight. */
    if (cx + 10 * sc >= (int)a_w) cx = bx - 16;
    /* Restore previous frame's spinner dots, then snapshot + draw new phase.
     * This avoids accumulating stale dots as a static ring. */
    a_spin_restore();
    a_spin_draw(cx, cy, phase, FOREB_TITLE, sc);
}

/* ------------------------------------------------------------------ */
/*  eased progress                                                     */
/* ------------------------------------------------------------------ */
static int a_prog_last = 0;
static int a_prog_spin = 0;

static void a_delay(int use_stall, int ms)
{
    if (use_stall && a_bs) {
        a_bs->Stall((UINTN)ms * 1000u);
    } else {
        /* Post-ExitBootServices busy delay (~ms, imprecise but adequate). */
        volatile UINT32 i;
        UINT32 n = (UINT32)ms * 120000u;
        for (i = 0; i < n; i++)
#if defined(__x86_64__) || defined(_M_X64)
            __asm__ __volatile__("pause");
#elif defined(__aarch64__) || defined(_M_ARM64)
            __asm__ __volatile__("yield");
#else
            __asm__ __volatile__("" ::: "memory"); /* riscv: compiler barrier */
#endif
    }
}

void anim_progress_reset(void)
{
    a_prog_last = 0;
}

void anim_progress_to(const char *label, UINT64 cur, UINT64 total, int use_stall)
{
    int target;
    if (cur == 0) { a_prog_last = 0; a_spin_restore(); }
    if (total == 0) target = 100;
    else {
        if (cur > total) cur = total;
        target = (int)((cur * 100u) / total);
    }
    if (target < 0) target = 0;
    if (target > 100) target = 100;
    if (target < a_prog_last) a_prog_last = target;  /* never animate backwards */

    while (a_prog_last < target) {
        a_prog_last += 4;
        if (a_prog_last > target) a_prog_last = target;
        ui_progress(label, (UINT64)a_prog_last, 100);
        /* anim_load_spinner() restores old dots, snapshots + draws new phase,
         * and marks the spinner bbox dirty. ui_progress() marks the bar dirty.
         * Together these give ui_present() just the changed regions - no
         * full-screen flip needed. */
        anim_load_spinner(a_prog_spin++);
        ui_present();                 /* flip each eased step (no-op if !DB) */
        a_delay(use_stall, 10);
    }
    a_spin_restore();
    ui_progress(label, (UINT64)target, 100);
    ui_present();
}
