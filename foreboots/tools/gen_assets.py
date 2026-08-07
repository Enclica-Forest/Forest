#!/usr/bin/env python3
"""
gen_assets.py - Generate ForeB's default UEFI menu assets with the stdlib only.

Writes raw BMP/TGA bytes (no PIL, no external deps) so the build has no binary
asset checked in and forebo_theme.h stays the single source of truth for color.

Outputs (under assets/, matching forebo.cfg paths):
    assets/bg.bmp                 1920x1080 24-bit forest-night background
    assets/icons/os.tga           32x32 32-bit TGA icons with 8-bit alpha
    assets/icons/gear.tga
    assets/icons/shield.tga
    assets/icons/reboot.tga
    assets/icons/text.tga         (alias used by forebo.cfg: console/text entry)
    assets/icons/safe.tga         (alias used by forebo.cfg: safe-mode entry)

The decoders in uefi/image.c consume both formats (24/32-bit BMP bottom-up,
type-2 TGA top-left origin with alpha).
"""

import math
import os
import struct

# --- forebo_theme.h palette (RGB888) ---------------------------------------
TITLE   = (81, 202, 61)     # leaf green   (FOREB_TITLE)
TEXT    = (182, 223, 182)   # mint         (FOREB_TEXT)
TREE1   = (61, 28, 8)       # trunk brown  (FOREB_TREE1)
TREE2   = (28, 121, 28)     # foliage mid  (FOREB_TREE2)
TREE3   = (61, 182, 61)     # foliage lit  (FOREB_TREE3)
TIMER   = (223, 162, 20)    # amber        (FOREB_TIMER)
WHITE   = (255, 255, 255)
BORDER  = (40, 81, 40)      # FOREB_BORDER

# Background gradient endpoints (night sky -> forest floor).
SKY_TOP    = (9, 13, 30)     # deep night blue
SKY_BOTTOM = (22, 50, 27)    # dim forest green


# ===========================================================================
#  BMP writer (24-bit, bottom-up)
# ===========================================================================
def write_bmp(path, width, height, rows_top_down):
    """rows_top_down: list of `height` bytearrays, each width*3 bytes B,G,R."""
    row_bytes = width * 3
    pad = (4 - (row_bytes % 4)) % 4
    padding = b"\x00" * pad
    image_size = (row_bytes + pad) * height
    file_size = 14 + 40 + image_size

    with open(path, "wb") as f:
        # BITMAPFILEHEADER
        f.write(b"BM")
        f.write(struct.pack("<IHHI", file_size, 0, 0, 14 + 40))
        # BITMAPINFOHEADER
        f.write(struct.pack("<IiiHHIIiiII",
                            40, width, height, 1, 24, 0,
                            image_size, 2835, 2835, 0, 0))
        # Pixel data is bottom-up: write last row first.
        for y in range(height - 1, -1, -1):
            f.write(rows_top_down[y])
            if pad:
                f.write(padding)


# ===========================================================================
#  Background: night -> forest vertical gradient + moon + stars + pine layers
# ===========================================================================
def lerp(a, b, t):
    return int(round(a + (b - a) * t))


def gen_background(path, W=1920, H=1080):
    # Pre-blend the vertical gradient per scanline.
    grad = []
    for y in range(H):
        t = y / (H - 1)
        # ease so the horizon glow sits low; sky stays dark up top.
        te = t * t
        r = lerp(SKY_TOP[0], SKY_BOTTOM[0], te)
        g = lerp(SKY_TOP[1], SKY_BOTTOM[1], te)
        b = lerp(SKY_TOP[2], SKY_BOTTOM[2], te)
        grad.append((r, g, b))

    # Sky feature overrides: {(x,y): (r,g,b)}
    overrides = {}

    # Moon: soft disc with a faint halo, upper-right.
    mcx, mcy, mr = int(W * 0.78), int(H * 0.22), 70
    moon = (222, 230, 214)
    for yy in range(mcy - mr - 40, mcy + mr + 40):
        if yy < 0 or yy >= H:
            continue
        for xx in range(mcx - mr - 40, mcx + mr + 40):
            if xx < 0 or xx >= W:
                continue
            d = math.hypot(xx - mcx, yy - mcy)
            if d <= mr:
                overrides[(xx, yy)] = moon
            elif d <= mr + 40:
                # halo: blend moon into sky by falloff
                f = 1.0 - (d - mr) / 40.0
                f = max(0.0, f) * 0.5
                bg = grad[yy]
                overrides[(xx, yy)] = (lerp(bg[0], moon[0], f),
                                       lerp(bg[1], moon[1], f),
                                       lerp(bg[2], moon[2], f))

    # Stars: deterministic pseudo-random field in the upper sky.
    seed = 1337
    def rnd():
        nonlocal seed
        seed = (1103515245 * seed + 12345) & 0x7FFFFFFF
        return seed
    for _ in range(260):
        sx = rnd() % W
        sy = rnd() % int(H * 0.60)
        # keep stars off the moon disc
        if math.hypot(sx - mcx, sy - mcy) < mr + 6:
            continue
        bright = 150 + (rnd() % 106)
        col = (bright, bright, min(255, bright + 15))
        overrides[(sx, sy)] = col
        if rnd() % 3 == 0 and sx + 1 < W:      # a few brighter twinkles
            overrides[(sx + 1, sy)] = col

    # Layered pine silhouettes along the bottom. Back layers are higher and
    # lighter; front layers are lower, bigger and darker for depth.
    ground = H
    layers = [
        # (apex_frac_y, tree_h, half_w, spacing, color)
        (0.62, 150, 70,  150, (17, 46, 24)),   # far ridge
        (0.68, 210, 95,  200, (13, 37, 19)),   # mid ridge
        (0.75, 300, 130, 260, (9, 27, 13)),    # near ridge (darkest)
    ]
    # For each layer compute a per-column silhouette top (min apex over trees).
    layer_tops = []
    min_top = H
    for (afy, th, hw, spacing, col) in layers:
        base_y = int(H * afy) + th       # tree trunk base (ground of this row)
        top = [H] * W
        x = -hw
        while x < W + hw:
            apex_y = base_y - th
            for cx in range(max(0, x - hw), min(W, x + hw + 1)):
                dx = abs(cx - x)
                # triangular pine: top rises to apex at center
                ty = apex_y + int(th * (dx / hw))
                if ty < top[cx]:
                    top[cx] = ty
            x += spacing
        layer_tops.append((top, col, base_y))
        min_top = min(min_top, min(top))

    # Build rows.
    rows = []
    for y in range(H):
        base = grad[y]
        row = bytearray(bytes((base[2], base[1], base[0])) * W)
        # apply tree layers front-to-back so the nearest layer wins.
        if y >= min_top:
            for x in range(W):
                key = (x, y)
                # trees take priority over sky features they cover
                painted = False
                for (top, col, base_y) in reversed(layer_tops):
                    if top[x] <= y <= base_y:
                        o = x * 3
                        row[o] = col[2]
                        row[o + 1] = col[1]
                        row[o + 2] = col[0]
                        painted = True
                        break
                if not painted and key in overrides:
                    c = overrides[key]
                    o = x * 3
                    row[o] = c[2]
                    row[o + 1] = c[1]
                    row[o + 2] = c[0]
        else:
            # sky-only row: just apply moon/star overrides for this scanline.
            # (cheap: overrides dict is small.)
            pass
        rows.append(row)

    # Apply sky overrides (moon/stars) for rows above the tree line in bulk.
    for (x, y), c in overrides.items():
        if y < min_top:
            o = x * 3
            rows[y][o] = c[2]
            rows[y][o + 1] = c[1]
            rows[y][o + 2] = c[0]

    write_bmp(path, W, H, rows)


# ===========================================================================
#  TGA icon canvas (super-sampled then box-downscaled for clean alpha edges)
# ===========================================================================
class Canvas:
    def __init__(self, size):
        self.n = size
        # buf[y][x] = [r,g,b,a]
        self.buf = [[[0, 0, 0, 0] for _ in range(size)] for _ in range(size)]

    def set(self, x, y, col, a=255):
        if 0 <= x < self.n and 0 <= y < self.n:
            self.buf[y][x] = [col[0], col[1], col[2], a]

    def disc(self, cx, cy, r, col, a=255):
        for y in range(int(cy - r) - 1, int(cy + r) + 2):
            for x in range(int(cx - r) - 1, int(cx + r) + 2):
                if math.hypot(x - cx, y - cy) <= r:
                    self.set(x, y, col, a)

    def ring(self, cx, cy, r_out, r_in, col, a=255):
        for y in range(int(cy - r_out) - 1, int(cy + r_out) + 2):
            for x in range(int(cx - r_out) - 1, int(cx + r_out) + 2):
                d = math.hypot(x - cx, y - cy)
                if r_in <= d <= r_out:
                    self.set(x, y, col, a)

    def rect(self, x0, y0, x1, y1, col, a=255):
        for y in range(int(y0), int(y1) + 1):
            for x in range(int(x0), int(x1) + 1):
                self.set(x, y, col, a)

    def tri(self, p0, p1, p2, col, a=255):
        xs = [p0[0], p1[0], p2[0]]
        ys = [p0[1], p1[1], p2[1]]
        minx, maxx = int(min(xs)), int(max(xs))
        miny, maxy = int(min(ys)), int(max(ys))

        def edge(ax, ay, bx, by, px, py):
            return (bx - ax) * (py - ay) - (by - ay) * (px - ax)
        area = edge(*p0, *p1, p2[0], p2[1])
        if area == 0:
            return
        for y in range(miny, maxy + 1):
            for x in range(minx, maxx + 1):
                w0 = edge(p1[0], p1[1], p2[0], p2[1], x, y)
                w1 = edge(p2[0], p2[1], p0[0], p0[1], x, y)
                w2 = edge(p0[0], p0[1], p1[0], p1[1], x, y)
                if (w0 >= 0 and w1 >= 0 and w2 >= 0) or \
                   (w0 <= 0 and w1 <= 0 and w2 <= 0):
                    self.set(x, y, col, a)

    def arc_thick(self, cx, cy, r, thick, a0, a1, col):
        """Draw an arc band from angle a0..a1 (radians), thickness `thick`."""
        steps = int(abs(a1 - a0) * r) + 8
        for i in range(steps + 1):
            ang = a0 + (a1 - a0) * (i / steps)
            for t in range(-thick, thick + 1):
                rr = r + t
                x = cx + rr * math.cos(ang)
                y = cy + rr * math.sin(ang)
                self.disc(x, y, 1.2, col)

    def downscale(self, factor):
        out_n = self.n // factor
        out = [[[0, 0, 0, 0] for _ in range(out_n)] for _ in range(out_n)]
        f2 = factor * factor
        for oy in range(out_n):
            for ox in range(out_n):
                r = g = b = a = 0
                for dy in range(factor):
                    for dx in range(factor):
                        px = self.buf[oy * factor + dy][ox * factor + dx]
                        r += px[0]; g += px[1]; b += px[2]; a += px[3]
                out[oy][ox] = [r // f2, g // f2, b // f2, a // f2]
        return out, out_n


def write_tga(path, pixels, n):
    """pixels[y][x] = [r,g,b,a], top-down. Writes 32-bit type-2 TGA."""
    with open(path, "wb") as f:
        # id_len=0, cmap_type=0, img_type=2, cmap spec=0(5 bytes),
        # x=0,y=0, w,h, depth=32, descriptor=0x28 (top-left origin, 8 alpha bits)
        f.write(struct.pack("<BBBHHBHHHHBB",
                            0, 0, 2, 0, 0, 0, 0, 0, n, n, 32, 0x28))
        out = bytearray()
        for y in range(n):
            for x in range(n):
                r, g, b, a = pixels[y][x]
                out += bytes((b, g, r, a))   # TGA stores BGRA
        f.write(out)


# --- individual icon painters (drawn on a 4x super-sampled canvas) ----------
S = 128          # super-sampled canvas size
F = 4            # downscale factor -> 32x32
C = S / 2.0      # center


def icon_os(cv):
    # A pine tree emblem inside a soft green disc: the Forest OS mark.
    cv.disc(C, C, 58, (18, 40, 22), 235)
    cv.ring(C, C, 58, 54, TITLE, 255)
    # trunk
    cv.rect(C - 6, C + 20, C + 6, C + 42, TREE1)
    # three stacked foliage tiers
    cv.tri((C, C - 46), (C - 34, C - 6), (C + 34, C - 6), TREE2)
    cv.tri((C, C - 30), (C - 40, C + 14), (C + 40, C + 14), TREE2)
    cv.tri((C, C - 12), (C - 46, C + 30), (C + 46, C + 30), TREE3)


def icon_gear(cv):
    # Cog wheel (settings).
    teeth = 8
    r_out, r_in = 52, 40
    for i in range(teeth):
        ang = (i / teeth) * 2 * math.pi
        tx, ty = C + r_out * math.cos(ang), C + r_out * math.sin(ang)
        cv.disc(tx, ty, 12, TEXT)
    cv.disc(C, C, r_in, TEXT)
    cv.disc(C, C, 18, (18, 40, 22))     # hub hole
    cv.ring(C, C, 20, 16, BORDER)


def icon_shield(cv):
    # Rounded-top, pointed-bottom shield.
    col = TITLE
    top, wid = 18, 46
    for y in range(int(top), S):
        t = (y - top) / (S - top)
        if t < 0.62:
            half = wid
        else:
            half = wid * (1 - (t - 0.62) / 0.38)
        if half <= 0:
            break
        cv.rect(C - half, y, C + half, y, col)
    # inner emboss line
    cv.rect(C - 4, 34, C + 4, 92, (18, 40, 22))


def icon_check(cv, col):
    # a check mark stroke
    pts = [(C - 26, C + 2), (C - 6, C + 26), (C + 30, C - 26)]
    for i in range(len(pts) - 1):
        ax, ay = pts[i]; bx, by = pts[i + 1]
        steps = 64
        for s in range(steps + 1):
            x = ax + (bx - ax) * s / steps
            y = ay + (by - ay) * s / steps
            cv.disc(x, y, 6, col)


def icon_safe(cv):
    # Shield + check = safe mode.
    icon_shield(cv)
    icon_check(cv, WHITE)


def icon_reboot(cv):
    # Circular arrow (reboot / reset).
    col = TIMER
    cv.arc_thick(C, C, 40, 5, math.radians(-40), math.radians(250), col)
    # arrowhead at the start of the arc (top)
    ang = math.radians(-40)
    hx, hy = C + 40 * math.cos(ang), C + 40 * math.sin(ang)
    cv.tri((hx + 16, hy - 2), (hx - 8, hy - 20), (hx - 8, hy + 16), col)


def icon_text(cv):
    # A document/page with lines (console/text entry).
    cv.rect(C - 34, C - 44, C + 30, C + 44, TEXT)
    # folded corner
    cv.tri((C + 30, C - 44), (C + 30, C - 20), (C + 6, C - 44), (18, 40, 22))
    # text lines
    for i in range(5):
        y = C - 26 + i * 16
        cv.rect(C - 24, y, C + 18, y + 4, (28, 60, 34))


# ===========================================================================
#  Extended icon set: Linux distros + hardware/tool glyphs (32x32, alpha).
#
#  All are 32x32 32-bit TGA with 8-bit alpha, generated the same way (4x super-
#  sampled canvas -> box downscale). forebo.cfg / the tool registry reference
#  them by SHORT NAME; tools_icon_path() (uefi/tools.h) resolves a bare name to
#  "/forebo/icons/<name>.tga". Suggested menu-entry defaults:
#     Linux(vmlinuz) -> tux      Chainload GRUB -> grub    Boot removable -> usb
#     ForeB Shell    -> terminal Recovery/Tools -> gear    Disk tools    -> disk
#
#  Palette (approximate brand colors, kept simple + recognizable):
#     ubuntu  aubergine #2C001E + orange ring   #E95420
#     debian  swirl red  #D70A53 on white
#     arch    blue       #1793D1 (mountain/A)
#     fedora  blue       #294172 + white 'f' infinity bar
#     mint    green      #86BE43 + darker leaf  #3A6629
#     tux     black body #202020, white belly, yellow beak/feet #F5C518
#     windows four-pane  #00A4EF (single-color flag)
#     grub    dark gnu   #2A2A2A + amber prompt #DFA214
#     usb     steel      #B6DFB6 trident on #285128
#     disk    HDD body   #3A6629 + platter ring #B6DFB6
#     gear    settings   reuse cog shape (TEXT) - alias/companion to icon_gear
#     terminal shell     #101814 screen + green prompt  #51CA3D
# ===========================================================================

# Brand-ish colors (RGB888)
UB_PLUM   = (44, 0, 30)
UB_ORANGE = (233, 84, 32)
DEB_RED   = (215, 10, 83)
ARCH_BLUE = (23, 147, 209)
FED_BLUE  = (41, 65, 114)
MINT_GRN  = (134, 190, 67)
MINT_DK   = (58, 102, 41)
TUX_BODY  = (32, 32, 32)
TUX_BELLY = (245, 245, 245)
TUX_BEAK  = (245, 197, 24)
WIN_BLUE  = (0, 164, 239)
GRUB_BG   = (42, 42, 42)
GRUB_AMB  = (223, 162, 20)
USB_STEEL = (182, 223, 182)
TERM_SCR  = (16, 24, 20)
TERM_GRN  = (81, 202, 61)


def icon_ubuntu(cv):
    # Circle of Friends: orange ring with three dots.
    cv.disc(C, C, 50, UB_PLUM, 255)
    cv.ring(C, C, 50, 40, UB_ORANGE, 255)
    for ang_deg in (0, 120, 240):
        a = math.radians(ang_deg - 90)
        cv.disc(C + 45 * math.cos(a), C + 45 * math.sin(a), 9, UB_ORANGE)
        cv.disc(C + 22 * math.cos(a), C + 22 * math.sin(a), 8, UB_ORANGE)


def icon_debian(cv):
    # Red swirl approximated by an open ring with a comma tail.
    cv.disc(C, C, 50, WHITE, 235)
    cv.arc_thick(C, C, 30, 4, math.radians(-20), math.radians(250), DEB_RED)
    cv.disc(C + 30 * math.cos(math.radians(-20)),
            C + 30 * math.sin(math.radians(-20)), 6, DEB_RED)


def icon_arch(cv):
    # Blue mountain 'A' triangle.
    cv.tri((C, C - 46), (C - 40, C + 42), (C + 40, C + 42), ARCH_BLUE)
    # inner shadow ridge (a slightly darker blue down the centre)
    cv.tri((C, C - 30), (C - 8, C + 42), (C + 8, C + 42), (12, 90, 150))


def icon_fedora(cv):
    # Blue disc with a white infinity/'f' bar.
    cv.disc(C, C, 50, FED_BLUE, 255)
    cv.rect(C - 6, C - 30, C + 6, C + 34, WHITE)       # stem
    cv.rect(C - 6, C - 8, C + 26, C + 4, WHITE)        # crossbar
    cv.arc_thick(C - 6, C - 20, 12, 3,
                 math.radians(90), math.radians(270), WHITE)


def icon_mint(cv):
    # Green rounded square with a leaf.
    cv.rect(C - 44, C - 44, C + 44, C + 44, MINT_DK)
    cv.rect(C - 32, C - 32, C + 32, C + 32, MINT_GRN)
    # simple leaf
    cv.tri((C - 20, C + 20), (C + 24, C - 24), (C + 24, C + 20), MINT_DK)


def icon_tux(cv):
    # Generic Linux penguin: black body, white belly, yellow beak + feet.
    cv.disc(C, C - 26, 22, TUX_BODY)                    # head
    cv.disc(C, C + 8, 40, TUX_BODY)                     # body
    cv.disc(C, C + 14, 26, TUX_BELLY)                   # belly
    cv.disc(C - 8, C - 30, 5, WHITE)                    # eyes
    cv.disc(C + 8, C - 30, 5, WHITE)
    cv.disc(C - 8, C - 30, 2, TUX_BODY)
    cv.disc(C + 8, C - 30, 2, TUX_BODY)
    cv.tri((C - 8, C - 20), (C + 8, C - 20), (C, C - 8), TUX_BEAK)  # beak
    cv.tri((C - 20, C + 44), (C - 2, C + 44), (C - 11, C + 30), TUX_BEAK)  # feet
    cv.tri((C + 20, C + 44), (C + 2, C + 44), (C + 11, C + 30), TUX_BEAK)


def icon_windows(cv):
    # Four-pane flag (single brand blue).
    gap = 6
    cv.rect(C - 40, C - 40, C - gap, C - gap, WIN_BLUE)
    cv.rect(C + gap, C - 40, C + 40, C - gap, WIN_BLUE)
    cv.rect(C - 40, C + gap, C - gap, C + 40, WIN_BLUE)
    cv.rect(C + gap, C + gap, C + 40, C + 40, WIN_BLUE)


def icon_grub(cv):
    # Dark console box with an amber prompt.
    cv.rect(C - 46, C - 34, C + 46, C + 34, GRUB_BG)
    cv.rect(C - 46, C - 34, C + 46, C - 22, (20, 20, 20))   # title strip
    # amber '>' prompt
    cv.tri((C - 30, C - 8), (C - 30, C + 12), (C - 16, C + 2), GRUB_AMB)
    cv.rect(C - 10, C + 8, C + 20, C + 14, GRUB_AMB)        # cursor line


def icon_usb(cv):
    # USB trident on a green disc.
    cv.disc(C, C, 50, (40, 81, 40), 235)
    col = USB_STEEL
    cv.rect(C - 4, C - 42, C + 4, C + 40, col)             # shaft
    cv.disc(C, C + 40, 8, col)                             # base ball
    cv.tri((C, C - 48), (C - 12, C - 34), (C + 12, C - 34), col)  # arrow tip
    # two prongs
    cv.rect(C - 22, C - 6, C - 14, C + 2, col)
    cv.rect(C - 22, C - 6, C - 2, C - 2, col)
    cv.disc(C - 22, C - 12, 6, col)
    cv.rect(C + 14, C + 8, C + 22, C + 16, col)
    cv.rect(C + 2, C + 12, C + 22, C + 16, col)


def icon_disk(cv):
    # Hard-disk / platter: rounded square + platter ring + spindle.
    cv.rect(C - 44, C - 34, C + 44, C + 34, MINT_DK)
    cv.disc(C, C, 30, (24, 52, 28))
    cv.ring(C, C, 30, 24, USB_STEEL)
    cv.disc(C, C, 6, USB_STEEL)                            # spindle
    cv.rect(C + 8, C + 8, C + 34, C + 14, USB_STEEL)       # head arm


def icon_terminal(cv):
    # Terminal window: dark screen + green prompt.
    cv.rect(C - 46, C - 36, C + 46, C + 36, (30, 60, 34))  # bezel
    cv.rect(C - 40, C - 24, C + 40, C + 30, TERM_SCR)      # screen
    # green '>' prompt + cursor
    cv.tri((C - 30, C - 12), (C - 30, C + 6), (C - 18, C - 3), TERM_GRN)
    cv.rect(C - 12, C - 1, C + 8, C + 5, TERM_GRN)


def icon_settings(cv):
    # Settings cog: a green-tinted gear (companion/alias to icon_gear, referenced
    # by the Theme/Settings tool + the "Firmware Setup" menu entry as icon=settings).
    teeth = 8
    r_out = 52
    for i in range(teeth):
        ang = (i / teeth) * 2 * math.pi
        tx, ty = C + r_out * math.cos(ang), C + r_out * math.sin(ang)
        cv.disc(tx, ty, 12, TITLE)
    cv.disc(C, C, 40, TITLE)
    cv.disc(C, C, 20, (18, 40, 22))       # hub hole
    cv.ring(C, C, 22, 18, TEXT)


ICONS = {
    # --- original set ---
    "os": icon_os,
    "gear": icon_gear,
    "shield": icon_shield,
    "reboot": icon_reboot,
    "text": icon_text,
    "safe": icon_safe,
    # --- distro icons ---
    "ubuntu": icon_ubuntu,
    "debian": icon_debian,
    "arch": icon_arch,
    "fedora": icon_fedora,
    "mint": icon_mint,
    "tux": icon_tux,
    "windows": icon_windows,
    # --- tool / hardware icons ---
    "grub": icon_grub,
    "usb": icon_usb,
    "disk": icon_disk,
    "terminal": icon_terminal,
    "settings": icon_settings,
}


def gen_icon(path, painter):
    cv = Canvas(S)
    painter(cv)
    px, n = cv.downscale(F)
    write_tga(path, px, n)


# ===========================================================================
def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    assets = os.path.join(root, "assets")
    icons = os.path.join(assets, "icons")
    os.makedirs(icons, exist_ok=True)

    bg_path = os.path.join(assets, "bg.bmp")
    print("Generating", bg_path, "(1920x1080)...")
    gen_background(bg_path)

    for name, painter in ICONS.items():
        p = os.path.join(icons, name + ".tga")
        print("Generating", p)
        gen_icon(p, painter)

    print("Done. Assets under", assets)


if __name__ == "__main__":
    main()
