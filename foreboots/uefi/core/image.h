/*
 * image.h - Self-contained BMP/TGA image decode + GOP blit for ForeB UEFI.
 *
 * Freestanding (no libc). Decodes uncompressed BMP (24/32-bit,
 * BITMAPINFOHEADER, bottom-up or top-down) and TGA (type 2 uncompressed and
 * type 10 RLE true-color, 24/32-bit) into a linear 0xAARRGGBB pixel buffer
 * that blits straight to the GOP framebuffer used by ui.c.
 *
 * Pixel model: every decoded pixel is a logical 0xAARRGGBB word (alpha in the
 * high byte, then R,G,B). 24-bit sources decode with alpha = 0xFF (opaque).
 * The blitters convert to the framebuffer byte order (BGRX vs RGBX) exactly
 * like ui.c, so R/B are swapped only on RGBX framebuffers.
 *
 * Lifecycle:
 *     img_init(gBS, fb_base, pitch, w, h, pixfmt);   // once, mirror ui_init()
 *     struct img_image bg;
 *     if (img_load_file(root, L"\\forebo\\bg.bmp", &bg) == 0) {
 *         img_blit_scaled(&bg, 0, 0, ui_width(), ui_height()); // background
 *         img_free(&bg);
 *     }
 *     struct img_image icon;
 *     if (img_load_file(root, L"\\forebo\\icons\\os.tga", &icon) == 0) {
 *         img_blit_alpha(&icon, x, y);                          // icon w/ alpha
 *         img_free(&icon);
 *     }
 *
 * All decode/blit work is valid BEFORE ExitBootServices (allocation uses
 * BootServices AllocatePool). The blitters themselves are pure framebuffer
 * MMIO and remain valid after ExitBootServices; only img_free()/img_load_file
 * need live BootServices.
 */
#ifndef FOREB_UEFI_IMAGE_H
#define FOREB_UEFI_IMAGE_H

#include "../efi.h"
#include "../ui.h"

/* Decoded image: linear top-down rows of 0xAARRGGBB pixels. */
struct img_image {
    UINT32 *pixels;      /* w*h words, 0xAARRGGBB, row-major top-down; NULL=empty */
    int     w;
    int     h;
    UINTN   alloc_bytes; /* size of the pixels allocation (for FreePool)          */
};

/* Return codes for the decoders (img_load_file returns EFI_STATUS). */
#define IMG_OK              0
#define IMG_ERR_FORMAT     -1   /* not a recognizable BMP/TGA                    */
#define IMG_ERR_UNSUPPORTED -2  /* recognized but unsupported (RLE-BMP, palette) */
#define IMG_ERR_TRUNCATED  -3   /* pixel data runs past the buffer end          */
#define IMG_ERR_NOMEM      -4   /* AllocatePool failed                          */
#define IMG_ERR_ARG        -5   /* bad argument / zero-size                     */

/*
 * One-time init. Pass the same framebuffer parameters given to ui_init() plus
 * the BootServices table (used for AllocatePool/FreePool). Safe to call again
 * if the framebuffer geometry changes.
 */
void img_init(EFI_BOOT_SERVICES *bs, UINT64 fb_base, UINT32 pitch,
              UINT32 width, UINT32 height, UINT32 pixfmt);

/*
 * Decode an in-memory BMP/TGA file image. On success fills *out (allocating
 * out->pixels) and returns IMG_OK. On failure returns a negative IMG_ERR_*
 * and leaves *out zeroed. Free with img_free().
 */
int img_decode(const void *data, UINTN size, struct img_image *out);

/*
 * Open `path` on the given (already OpenVolume'd) root directory, read the
 * whole file into a temporary pool buffer, decode it, and free the temporary.
 * Returns EFI_SUCCESS on success (out filled), or an EFI error status.
 * Requires img_init() to have been called with a live BootServices table.
 */
EFI_STATUS img_load_file(EFI_FILE_PROTOCOL *root, const CHAR16 *path,
                         struct img_image *out);

/* Release a decoded image (FreePool) and zero the struct. NULL-safe. */
void img_free(struct img_image *img);

/*
 * Nearest-neighbor scale-blit `src` into the framebuffer rectangle
 * (x,y,dstw,dsth). Opaque: the source alpha channel is ignored (intended for
 * backgrounds). Clipped to the screen. dstw/dsth <= 0 are no-ops.
 */
void img_blit_scaled(const struct img_image *src, int x, int y,
                     int dstw, int dsth);

/*
 * Blit `src` 1:1 at (x,y) compositing each pixel over the current framebuffer
 * contents using its 8-bit alpha (0=transparent, 255=opaque). For icons.
 */
void img_blit_alpha(const struct img_image *src, int x, int y);

/*
 * Nearest-neighbor scale-blit with per-pixel alpha compositing into
 * (x,y,dstw,dsth). Combines img_blit_scaled + img_blit_alpha for scaled icons.
 */
void img_blit_alpha_scaled(const struct img_image *src, int x, int y,
                           int dstw, int dsth);

#endif /* FOREB_UEFI_IMAGE_H */
