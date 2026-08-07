/*
 * image.c - Self-contained BMP/TGA decode + GOP framebuffer blit for ForeB.
 *
 * Freestanding, no libc. See image.h for the public contract. Decoders emit a
 * linear top-down 0xAARRGGBB buffer; blitters convert to the framebuffer byte
 * order (BGRX default, RGBX with R/B swapped) exactly like ui.c so rendering
 * matches the rest of the UEFI UI.
 */
#include "image.h"
#include "ui.h"                 /* ui_mark_dirty: feed blits into partial present */

/* ------------------------------------------------------------------ */
/*  Module state (framebuffer geometry + BootServices for allocation)  */
/* ------------------------------------------------------------------ */
static EFI_BOOT_SERVICES *g_bs    = 0;
static volatile UINT8    *g_fb    = 0;   /* framebuffer base as byte ptr */
static UINT32             g_pitch = 0;   /* bytes per scanline           */
static UINT32             g_w     = 0;   /* width  in pixels             */
static UINT32             g_h     = 0;   /* height in pixels             */
static int                g_swap  = 0;   /* 1 => framebuffer is RGBX     */

/* PixelRedGreenBlueReserved8BitPerColor is enum value 0 in efi.h. */
#define IMG_PIXFMT_RGBX 0u

void img_init(EFI_BOOT_SERVICES *bs, UINT64 fb_base, UINT32 pitch,
              UINT32 width, UINT32 height, UINT32 pixfmt)
{
    g_bs    = bs;
    g_fb    = (volatile UINT8 *)(UINTN)fb_base;
    g_pitch = pitch;
    g_w     = width;
    g_h     = height;
    g_swap  = (pixfmt == IMG_PIXFMT_RGBX) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Pixel-order conversion (logical 0xxxRRGGBB <-> framebuffer word)   */
/* ------------------------------------------------------------------ */
/* Swap R<->B; self-inverse, so used for both pack and unpack. */
static inline UINT32 img_swap_rb(UINT32 c)
{
    return (c & 0x0000FF00u)
         | ((c & 0x00FF0000u) >> 16)
         | ((c & 0x000000FFu) << 16);
}

/* Logical 0x00RRGGBB -> framebuffer word. */
static inline UINT32 img_pack(UINT32 rgb)
{
    return g_swap ? img_swap_rb(rgb) : rgb;
}

/* Framebuffer word -> logical 0x00RRGGBB. */
static inline UINT32 img_unpack(UINT32 raw)
{
    return g_swap ? img_swap_rb(raw & 0x00FFFFFFu) : (raw & 0x00FFFFFFu);
}

/* Raw framebuffer read/write (no clipping; callers must clip first). */
static inline UINT32 fb_get(int x, int y)
{
    volatile UINT32 *p =
        (volatile UINT32 *)(g_fb + (UINTN)y * g_pitch + (UINTN)x * 4u);
    return *p;
}
static inline void fb_put(int x, int y, UINT32 raw)
{
    volatile UINT32 *p =
        (volatile UINT32 *)(g_fb + (UINTN)y * g_pitch + (UINTN)x * 4u);
    *p = raw;
}

/* ------------------------------------------------------------------ */
/*  Little-endian scalar reads from a byte buffer                      */
/* ------------------------------------------------------------------ */
static inline UINT16 rd16(const UINT8 *p)
{
    return (UINT16)(p[0] | ((UINT16)p[1] << 8));
}
static inline UINT32 rd32(const UINT8 *p)
{
    return (UINT32)p[0] | ((UINT32)p[1] << 8)
         | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/*  Allocation helpers                                                 */
/* ------------------------------------------------------------------ */
static int img_alloc(struct img_image *out, int w, int h)
{
    out->pixels = 0;
    out->w = 0;
    out->h = 0;
    out->alloc_bytes = 0;
    if (!g_bs) return IMG_ERR_ARG;
    if (w <= 0 || h <= 0) return IMG_ERR_ARG;
    /* Guard against absurd dimensions overflowing the size computation. */
    if (w > 16384 || h > 16384) return IMG_ERR_UNSUPPORTED;

    UINTN bytes = (UINTN)w * (UINTN)h * 4u;
    VOID *buf = 0;
    EFI_STATUS st = g_bs->AllocatePool(EfiLoaderData, bytes, &buf);
    if (EFI_ERROR(st) || !buf) return IMG_ERR_NOMEM;

    out->pixels = (UINT32 *)buf;
    out->w = w;
    out->h = h;
    out->alloc_bytes = bytes;
    return IMG_OK;
}

void img_free(struct img_image *img)
{
    if (!img) return;
    if (img->pixels && g_bs) g_bs->FreePool(img->pixels);
    img->pixels = 0;
    img->w = 0;
    img->h = 0;
    img->alloc_bytes = 0;
}

/* ------------------------------------------------------------------ */
/*  BMP decode (BITMAPINFOHEADER, 24/32-bit, BI_RGB / BI_BITFIELDS)    */
/* ------------------------------------------------------------------ */
static int decode_bmp(const UINT8 *d, UINTN n, struct img_image *out)
{
    if (n < 54) return IMG_ERR_TRUNCATED;             /* 14 file + 40 info hdr */
    if (d[0] != 'B' || d[1] != 'M') return IMG_ERR_FORMAT;

    UINT32 off_bits   = rd32(d + 10);
    UINT32 hdr_size   = rd32(d + 14);
    if (hdr_size < 40) return IMG_ERR_UNSUPPORTED;    /* need BITMAPINFOHEADER */

    INT32  biw        = (INT32)rd32(d + 18);
    INT32  bih        = (INT32)rd32(d + 22);
    UINT16 bitcount   = rd16(d + 28);
    UINT32 compression= rd32(d + 30);

    if (compression != 0 /*BI_RGB*/ && compression != 3 /*BI_BITFIELDS*/)
        return IMG_ERR_UNSUPPORTED;                   /* no RLE / JPEG / PNG   */
    if (bitcount != 24 && bitcount != 32)
        return IMG_ERR_UNSUPPORTED;                   /* no palette / 16-bit   */
    if (biw <= 0 || biw > 16384) return IMG_ERR_UNSUPPORTED;

    int top_down = 0;
    int height = bih;
    if (height < 0) { top_down = 1; height = -height; }
    if (height <= 0 || height > 16384) return IMG_ERR_UNSUPPORTED;

    int width = (int)biw;
    UINTN bytes_pp = bitcount / 8u;
    /* Rows are padded to a 4-byte boundary. */
    UINTN row_stride = (((UINTN)width * bytes_pp) + 3u) & ~(UINTN)3u;

    if ((UINTN)off_bits > n) return IMG_ERR_TRUNCATED;
    if (row_stride != 0 && (n - off_bits) / row_stride < (UINTN)height)
        return IMG_ERR_TRUNCATED;

    int rc = img_alloc(out, width, height);
    if (rc != IMG_OK) return rc;

    const UINT8 *pix = d + off_bits;
    /* Alpha-presence accumulated during the primary decode (32-bit only) so a
     * separate read pass is not needed. bytes_pp is fixed for the whole image,
     * so the 24/32-bit loops are specialized to pick the alpha source once. */
    UINT32 amask = 0;
    for (int row = 0; row < height; row++) {
        /* Source row in file; bottom-up unless top_down. */
        int srow = top_down ? row : (height - 1 - row);
        const UINT8 *sp = pix + (UINTN)srow * row_stride;
        UINT32 *dp = out->pixels + (UINTN)row * (UINTN)width;
        if (bytes_pp == 4) {
            for (int x = 0; x < width; x++) {
                UINT32 v = ((UINT32)sp[3] << 24) | ((UINT32)sp[2] << 16)
                         | ((UINT32)sp[1] << 8) | (UINT32)sp[0];
                dp[x] = v;
                amask |= v & 0xFF000000u;
                sp += 4;
            }
        } else {
            for (int x = 0; x < width; x++) {
                dp[x] = 0xFF000000u | ((UINT32)sp[2] << 16)
                      | ((UINT32)sp[1] << 8) | (UINT32)sp[0];
                sp += 3;
            }
        }
    }

    /* If a 32-bit BMP had an entirely-zero alpha channel it usually means "no
     * alpha was authored"; force opaque so such images are not invisible
     * (common for tools that leave the reserved byte at 0). */
    if (bytes_pp == 4 && amask == 0) {
        UINTN total = (UINTN)width * (UINTN)height;
        for (UINTN i = 0; i < total; i++) out->pixels[i] |= 0xFF000000u;
    }
    return IMG_OK;
}

/* ------------------------------------------------------------------ */
/*  TGA decode (type 2 uncompressed, type 10 RLE; 24/32-bit)           */
/* ------------------------------------------------------------------ */
static int decode_tga(const UINT8 *d, UINTN n, struct img_image *out)
{
    if (n < 18) return IMG_ERR_TRUNCATED;

    UINT8  id_len     = d[0];
    UINT8  cmap_type  = d[1];
    UINT8  img_type   = d[2];
    UINT16 width      = rd16(d + 12);
    UINT16 height     = rd16(d + 14);
    UINT8  depth      = d[16];
    UINT8  descriptor = d[17];

    if (cmap_type != 0) return IMG_ERR_UNSUPPORTED;   /* color-mapped: no     */
    if (img_type != 2 && img_type != 10) return IMG_ERR_FORMAT;
    if (depth != 24 && depth != 32) return IMG_ERR_UNSUPPORTED;
    if (width == 0 || height == 0) return IMG_ERR_FORMAT;

    UINTN bytes_pp = depth / 8u;
    /* Data begins after the header + image-id (+ color map, absent here). */
    UINTN data_off = 18u + (UINTN)id_len;
    if (data_off > n) return IMG_ERR_TRUNCATED;

    /* Bit 5 of the descriptor: 1 => rows stored top-to-bottom. */
    int top_down = (descriptor & 0x20) ? 1 : 0;

    int rc = img_alloc(out, (int)width, (int)height);
    if (rc != IMG_OK) return rc;

    const UINT8 *sp = d + data_off;
    const UINT8 *end = d + n;
    UINTN total = (UINTN)width * (UINTN)height;

    if (img_type == 2) {
        /* Uncompressed: total*bytes_pp bytes, stored row order per origin. */
        if ((UINTN)(end - sp) < total * bytes_pp) {
            img_free(out);
            return IMG_ERR_TRUNCATED;
        }
        /* Incremental (px,py) instead of a per-pixel divide/modulo, and the
         * 24/32-bit alpha source is chosen once by specializing the loop. */
        int px = 0, py = 0;
        int drow = top_down ? 0 : (int)height - 1;
        UINT32 *dp = out->pixels + (UINTN)drow * width;
        if (bytes_pp == 4) {
            for (UINTN i = 0; i < total; i++) {
                dp[px] = ((UINT32)sp[3] << 24) | ((UINT32)sp[2] << 16)
                       | ((UINT32)sp[1] << 8) | (UINT32)sp[0];
                sp += 4;
                if (++px == (int)width) {
                    px = 0;
                    if (++py < (int)height) {
                        drow = top_down ? py : (int)height - 1 - py;
                        dp = out->pixels + (UINTN)drow * width;
                    }
                }
            }
        } else {
            for (UINTN i = 0; i < total; i++) {
                dp[px] = 0xFF000000u | ((UINT32)sp[2] << 16)
                       | ((UINT32)sp[1] << 8) | (UINT32)sp[0];
                sp += 3;
                if (++px == (int)width) {
                    px = 0;
                    if (++py < (int)height) {
                        drow = top_down ? py : (int)height - 1 - py;
                        dp = out->pixels + (UINTN)drow * width;
                    }
                }
            }
        }
    } else {
        /* Type 10: RLE true-color. Packets span rows freely. A running
         * (px,py) + destination pointer is carried across packets so no
         * per-pixel divide/modulo is needed to address the output. */
        UINTN produced = 0;
        int px = 0, py = 0;
        int drow = top_down ? 0 : (int)height - 1;
        UINT32 *dp = out->pixels + (UINTN)drow * width;
        while (produced < total) {
            if (sp >= end) { img_free(out); return IMG_ERR_TRUNCATED; }
            UINT8 hdr = *sp++;
            UINTN count = (UINTN)(hdr & 0x7F) + 1u;
            if (produced + count > total) count = total - produced; /* clamp */
            if (hdr & 0x80) {
                /* RLE packet: one pixel repeated `count` times. */
                if ((UINTN)(end - sp) < bytes_pp) {
                    img_free(out); return IMG_ERR_TRUNCATED;
                }
                UINT8 b = sp[0], g = sp[1], r = sp[2];
                UINT8 a = (bytes_pp == 4) ? sp[3] : 0xFF;
                sp += bytes_pp;
                UINT32 v = ((UINT32)a << 24) | ((UINT32)r << 16)
                         | ((UINT32)g << 8) | (UINT32)b;
                for (UINTN k = 0; k < count; k++, produced++) {
                    dp[px] = v;
                    if (++px == (int)width) {
                        px = 0;
                        if (++py < (int)height) {
                            drow = top_down ? py : (int)height - 1 - py;
                            dp = out->pixels + (UINTN)drow * width;
                        }
                    }
                }
            } else {
                /* Raw packet: `count` literal pixels; alpha source picked once. */
                if ((UINTN)(end - sp) < count * bytes_pp) {
                    img_free(out); return IMG_ERR_TRUNCATED;
                }
                if (bytes_pp == 4) {
                    for (UINTN k = 0; k < count; k++, produced++) {
                        dp[px] = ((UINT32)sp[3] << 24) | ((UINT32)sp[2] << 16)
                               | ((UINT32)sp[1] << 8) | (UINT32)sp[0];
                        sp += 4;
                        if (++px == (int)width) {
                            px = 0;
                            if (++py < (int)height) {
                                drow = top_down ? py : (int)height - 1 - py;
                                dp = out->pixels + (UINTN)drow * width;
                            }
                        }
                    }
                } else {
                    for (UINTN k = 0; k < count; k++, produced++) {
                        dp[px] = 0xFF000000u | ((UINT32)sp[2] << 16)
                               | ((UINT32)sp[1] << 8) | (UINT32)sp[0];
                        sp += 3;
                        if (++px == (int)width) {
                            px = 0;
                            if (++py < (int)height) {
                                drow = top_down ? py : (int)height - 1 - py;
                                dp = out->pixels + (UINTN)drow * width;
                            }
                        }
                    }
                }
            }
        }
    }
    return IMG_OK;
}

/* ------------------------------------------------------------------ */
/*  Public: format-sniffing decode                                     */
/* ------------------------------------------------------------------ */
int img_decode(const void *data, UINTN size, struct img_image *out)
{
    if (out) { out->pixels = 0; out->w = 0; out->h = 0; out->alloc_bytes = 0; }
    if (!data || !out || size < 18) return IMG_ERR_ARG;

    const UINT8 *d = (const UINT8 *)data;
    /* BMP is unambiguous by its "BM" magic. Everything else: try TGA (it has
     * no magic, so sniff by a plausible header). */
    if (d[0] == 'B' && d[1] == 'M')
        return decode_bmp(d, size, out);
    return decode_tga(d, size, out);
}

/* ------------------------------------------------------------------ */
/*  Public: load a file from an open root dir, then decode             */
/* ------------------------------------------------------------------ */
EFI_STATUS img_load_file(EFI_FILE_PROTOCOL *root, const CHAR16 *path,
                         struct img_image *out)
{
    static EFI_GUID fileInfoGuid = EFI_FILE_INFO_ID;
    EFI_STATUS st;

    if (out) { out->pixels = 0; out->w = 0; out->h = 0; out->alloc_bytes = 0; }
    if (!g_bs || !root || !path || !out) return EFI_INVALID_PARAMETER;

    EFI_FILE_PROTOCOL *f = 0;
    st = root->Open(root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !f) return EFI_ERROR(st) ? st : EFI_NOT_FOUND;

    /* Size via GetInfo(EFI_FILE_INFO). */
    UINT8 infobuf[512];
    UINTN infosz = sizeof(infobuf);
    st = f->GetInfo(f, &fileInfoGuid, &infosz, infobuf);
    if (EFI_ERROR(st)) { f->Close(f); return st; }
    UINTN fsize = (UINTN)((EFI_FILE_INFO *)infobuf)->FileSize;
    if (fsize == 0) { f->Close(f); return EFI_LOAD_ERROR; }

    VOID *buf = 0;
    st = g_bs->AllocatePool(EfiLoaderData, fsize, &buf);
    if (EFI_ERROR(st) || !buf) { f->Close(f); return EFI_OUT_OF_RESOURCES; }

    /* Chunked read (mirrors load_kernel_file); 256 KiB per Read call. */
    UINTN done = 0;
    UINT8 *dst = (UINT8 *)buf;
    const UINTN CHUNK = 256u * 1024u;
    while (done < fsize) {
        UINTN want = fsize - done;
        if (want > CHUNK) want = CHUNK;
        UINTN got = want;
        st = f->Read(f, &got, dst + done);
        if (EFI_ERROR(st)) { g_bs->FreePool(buf); f->Close(f); return st; }
        if (got == 0) break;
        done += got;
    }
    f->Close(f);

    int rc = img_decode(buf, done, out);
    g_bs->FreePool(buf);
    if (rc != IMG_OK) return EFI_LOAD_ERROR;
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Blitters                                                           */
/* ------------------------------------------------------------------ */
/* Composite one logical-ARGB source pixel over the framebuffer word *p.
 * `swap` is the framebuffer R/B swap flag; callers pass it as a compile-time
 * constant (0 or 1) from a g_swap-hoisted branch, so the pack/unpack decision
 * is folded away once per blit instead of re-tested per pixel. The single
 * framebuffer address is computed by the caller and read/written via *p. */
static inline void blend_pixel_at(volatile UINT32 *p, UINT32 argb, int swap)
{
    UINT32 a = (argb >> 24) & 0xFFu;
    if (a == 0) return;                          /* fully transparent */
    UINT32 rgb = argb & 0x00FFFFFFu;
    if (a != 0xFF) {
        UINT32 raw = *p & 0x00FFFFFFu;
        UINT32 dst = swap ? img_swap_rb(raw) : raw;
        UINT32 sr = (argb >> 16) & 0xFFu, sg = (argb >> 8) & 0xFFu, sb = argb & 0xFFu;
        UINT32 dr = (dst  >> 16) & 0xFFu, dg = (dst  >> 8) & 0xFFu, db = dst  & 0xFFu;
        UINT32 ia = 255u - a;
        /* (u*0x8081)>>23 == u/255 exactly for u < 65536; u <= 255*255+127 here. */
        UINT32 rr = ((sr * a + dr * ia + 127u) * 0x8081u) >> 23;
        UINT32 rg = ((sg * a + dg * ia + 127u) * 0x8081u) >> 23;
        UINT32 rb = ((sb * a + db * ia + 127u) * 0x8081u) >> 23;
        rgb = (rr << 16) | (rg << 8) | rb;
    }
    *p = swap ? img_swap_rb(rgb) : rgb;
}

/* Intersect a destination rect with the screen AND the active ui clip rect. */
static int blit_clip(int x, int y, int w, int h, int *cx0, int *cy0,
                     int *cx1, int *cy1)
{
    int kx, ky, kw, kh;
    ui_clip_get(&kx, &ky, &kw, &kh);
    int ax0 = x < 0 ? 0 : x,            ay0 = y < 0 ? 0 : y;
    int ax1 = x + w > (int)g_w ? (int)g_w : x + w;
    int ay1 = y + h > (int)g_h ? (int)g_h : y + h;
    if (ax0 < kx) ax0 = kx;
    if (ay0 < ky) ay0 = ky;
    if (ax1 > kx + kw) ax1 = kx + kw;
    if (ay1 > ky + kh) ay1 = ky + kh;
    if (ax0 >= ax1 || ay0 >= ay1) return 0;
    *cx0 = ax0; *cy0 = ay0; *cx1 = ax1; *cy1 = ay1;
    return 1;
}

void img_blit_scaled(const struct img_image *src, int x, int y,
                     int dstw, int dsth)
{
    if (!g_fb || !src || !src->pixels) return;
    if (dstw <= 0 || dsth <= 0 || src->w <= 0 || src->h <= 0) return;

    int cx0, cy0, cx1, cy1;
    if (!blit_clip(x, y, dstw, dsth, &cx0, &cy0, &cx1, &cy1)) return;
    int dx0 = x, dy0 = y;

    /* 1:1 fast path (skinned chrome at native size): pure row copies. */
    if (src->w == dstw && src->h == dsth) {
        for (int py = cy0; py < cy1; py++) {
            const UINT32 *srow = src->pixels + (UINTN)(py - dy0) * (UINTN)src->w
                               + (UINTN)(cx0 - dx0);
            volatile UINT32 *drow = (volatile UINT32 *)
                (g_fb + (UINTN)py * g_pitch + (UINTN)cx0 * 4u);
            int n = cx1 - cx0;
            /* Non-swap: copy the row wholesale. Scanout (and ui.c's back-buffer
             * FX, which read channels via low-byte masks) ignore the top byte,
             * so the &0x00FFFFFF per-pixel mask is unnecessary work here. */
            if (g_swap) { for (int i = 0; i < n; i++) drow[i] = img_swap_rb(srow[i] & 0x00FFFFFFu); }
            else        { for (int i = 0; i < n; i++) drow[i] = srow[i]; }
        }
        ui_mark_dirty(cx0, cy0, cx1 - cx0, cy1 - cy0);
        return;
    }

    /* Fixed-point 16.16 nearest-neighbor stepping: two divisions total per
     * blit instead of one 64-bit divide per pixel. */
    UINT32 xstep = (UINT32)(((UINT64)src->w << 16) / (UINT32)dstw);
    UINT32 ystep = (UINT32)(((UINT64)src->h << 16) / (UINT32)dsth);
    UINT64 sy_acc = (UINT64)(cy0 - dy0) * ystep;
    int n = cx1 - cx0;
    for (int py = cy0; py < cy1; py++) {
        int sy = (int)(sy_acc >> 16);
        if (sy >= src->h) sy = src->h - 1;
        const UINT32 *srow = src->pixels + (UINTN)sy * (UINTN)src->w;
        volatile UINT32 *drow = (volatile UINT32 *)
            (g_fb + (UINTN)py * g_pitch + (UINTN)cx0 * 4u);
        UINT64 sx_acc = (UINT64)(cx0 - dx0) * xstep;
        /* The blit_clip destination bound guarantees the highest column index
         * is <= dstw-1, so sx = (idx*xstep)>>16 is provably < src->w: the old
         * per-pixel clamp never fired. Dropped from the hot span.
         * g_swap is blit-invariant: pick the pack path once per row. */
        if (g_swap) {
            for (int i = 0; i < n; i++) {
                int sx = (int)(sx_acc >> 16);
                drow[i] = img_swap_rb(srow[sx] & 0x00FFFFFFu);
                sx_acc += xstep;
            }
        } else {
            for (int i = 0; i < n; i++) {
                int sx = (int)(sx_acc >> 16);
                drow[i] = srow[sx] & 0x00FFFFFFu;
                sx_acc += xstep;
            }
        }
        sy_acc += ystep;
    }
    ui_mark_dirty(cx0, cy0, cx1 - cx0, cy1 - cy0);
}

void img_blit_alpha(const struct img_image *src, int x, int y)
{
    if (!g_fb || !src || !src->pixels || src->w <= 0 || src->h <= 0) return;
    int cx0, cy0, cx1, cy1;
    if (!blit_clip(x, y, src->w, src->h, &cx0, &cy0, &cx1, &cy1)) return;
    int n = cx1 - cx0;
    for (int py = cy0; py < cy1; py++) {
        /* Source pointer walks the row; drow's byte offset is hoisted so only
         * +i*4 varies per pixel. g_swap is picked once per row. */
        const UINT32 *sp = src->pixels + (UINTN)(py - y) * (UINTN)src->w
                         + (UINTN)(cx0 - x);
        volatile UINT32 *drow = (volatile UINT32 *)
            (g_fb + (UINTN)py * g_pitch + (UINTN)cx0 * 4u);
        if (g_swap) { for (int i = 0; i < n; i++) blend_pixel_at(drow + i, sp[i], 1); }
        else        { for (int i = 0; i < n; i++) blend_pixel_at(drow + i, sp[i], 0); }
    }
    ui_mark_dirty(cx0, cy0, cx1 - cx0, cy1 - cy0);
}

void img_blit_alpha_scaled(const struct img_image *src, int x, int y,
                           int dstw, int dsth)
{
    if (!g_fb || !src || !src->pixels) return;
    if (dstw <= 0 || dsth <= 0 || src->w <= 0 || src->h <= 0) return;

    int cx0, cy0, cx1, cy1;
    if (!blit_clip(x, y, dstw, dsth, &cx0, &cy0, &cx1, &cy1)) return;
    int dx0 = x, dy0 = y;

    /* Fixed-point 16.16 stepping (see img_blit_scaled). */
    UINT32 xstep = (UINT32)(((UINT64)src->w << 16) / (UINT32)dstw);
    UINT32 ystep = (UINT32)(((UINT64)src->h << 16) / (UINT32)dsth);
    UINT64 sy_acc = (UINT64)(cy0 - dy0) * ystep;
    int n = cx1 - cx0;
    for (int py = cy0; py < cy1; py++) {
        int sy = (int)(sy_acc >> 16);
        if (sy >= src->h) sy = src->h - 1;
        const UINT32 *srow = src->pixels + (UINTN)sy * (UINTN)src->w;
        volatile UINT32 *drow = (volatile UINT32 *)
            (g_fb + (UINTN)py * g_pitch + (UINTN)cx0 * 4u);
        UINT64 sx_acc = (UINT64)(cx0 - dx0) * xstep;
        /* Per-pixel sx clamp dropped: blit_clip bounds the top column index at
         * dstw-1, so sx = (idx*xstep)>>16 is provably < src->w (see
         * img_blit_scaled). g_swap is picked once per row. */
        if (g_swap) {
            for (int i = 0; i < n; i++) {
                int sx = (int)(sx_acc >> 16);
                blend_pixel_at(drow + i, srow[sx], 1);
                sx_acc += xstep;
            }
        } else {
            for (int i = 0; i < n; i++) {
                int sx = (int)(sx_acc >> 16);
                blend_pixel_at(drow + i, srow[sx], 0);
                sx_acc += xstep;
            }
        }
        sy_acc += ystep;
    }
    ui_mark_dirty(cx0, cy0, cx1 - cx0, cy1 - cy0);
}
