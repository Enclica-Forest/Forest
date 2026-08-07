/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/input.c - pointer (mouse / touch) polling + software cursor sprite.
 * =============================================================================
 * Freestanding (no libc). Draws the cursor via ui.c primitives (put_pixel) so
 * it composites into the same off-screen back buffer as everything else. See
 * input.h for the contract.
 * ========================================================================== */
#include "input.h"
#include "../efi_ext.h"
#include "../ui.h"

static EFI_GUID sp_guid  = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
static EFI_GUID abs_guid = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;

/* -----------------------------------------------------------------------------
 * COM1 serial log (private copy; the helpers in bootx64.c are static there, so
 * chainload.c / boot_linux.c each keep their own tiny copy and so do we). Lets
 * us confirm how many pointer devices bound + the first few tracked coords on
 * QEMU's `-serial stdio`. The 16550 is already initialised by bootx64's
 * serial_init(); we only need to push bytes.
 * -------------------------------------------------------------------------- */
static inline void in_outb(UINT16 port, UINT8 v)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
#else
    (void)port; (void)v;   /* no 16550 on aarch64/riscv virt; log is a no-op */
#endif
}
static inline UINT8 in_inb(UINT16 port)
{
#if defined(__x86_64__) || defined(__i386__)
    UINT8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
#else
    (void)port; return 0;
#endif
}
static void in_putc(char c) { if (c == '\n') in_outb(0x3F8, '\r'); in_outb(0x3F8, (UINT8)c); }
static void in_puts(const char *s) { while (s && *s) in_putc(*s++); }
static void in_putd(long v)
{
    char buf[24]; int i = 0; unsigned long u;
    if (v < 0) { in_putc('-'); u = (unsigned long)(-v); } else u = (unsigned long)v;
    if (u == 0) { in_putc('0'); return; }
    while (u) { buf[i++] = (char)('0' + (u % 10)); u /= 10; }
    while (i) in_putc(buf[--i]);
}
static void in_puthex(UINT64 v)
{
    static const char hx[] = "0123456789ABCDEF";
    int started = 0;
    in_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        int d = (int)((v >> i) & 0xF);
        if (d || started || i == 0) { in_putc(hx[d]); started = 1; }
    }
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void zero(void *p, unsigned n)
{
    unsigned char *b = (unsigned char *)p;
    for (unsigned i = 0; i < n; i++) b[i] = 0;
}

/* First few successful device reads get logged so we can watch tracking. */
static int g_log_budget = 12;

static int handle_known(void *const *list, int n, void *h)
{
    for (int i = 0; i < n; i++)
        if (list[i] == h) return 1;
    return 0;
}

/* -----------------------------------------------------------------------------
 * Force-connect every controller so firmware actually BINDS its pointer drivers.
 * -----------------------------------------------------------------------------
 * Root cause of "cursor never moves": OVMF only auto-connects the handful of
 * controllers it needs to reach the boot device + console. A USB tablet / mouse
 * (and the i8042 PS/2 aux port) frequently never get their driver bound, so the
 * ONLY thing carrying the pointer GUIDs is the edk2 ConSplitter *aggregate* -
 * which has no child device behind it and whose GetState never reports motion.
 * Binding succeeds, yet every poll returns EFI_NOT_READY -> a frozen cursor.
 *
 * rEFInd/GRUB avoid this by walking ALL handles and calling ConnectController
 * recursively, which drives UsbMouseAbsolutePointer / UsbMouse / Ps2Mouse to
 * attach and start delivering. We do the same. Already-connected controllers
 * just return EFI_ALREADY_STARTED (cheap), so this is safe to repeat while we
 * wait out OVMF's asynchronous USB enumeration. Bounded so it can't spin. */
static int g_connect_budget = 3;   /* ~ first few rescans; a bound pointer stops it */

static void connect_all_controllers(EFI_BOOT_SERVICES *bs)
{
    if (g_connect_budget <= 0) return;
    g_connect_budget--;

    UINTN nh = 0; EFI_HANDLE *handles = NULL;
    EFI_STATUS st = bs->LocateHandleBuffer(AllHandles, NULL, NULL, &nh, &handles);
    if (EFI_ERROR(st) || !handles) return;
    int connected = 0;
    for (UINTN i = 0; i < nh; i++) {
        /* recursive=TRUE so child buses (xHCI -> hub -> HID) fully attach */
        if (!EFI_ERROR(foreb_ConnectController(bs, handles[i], NULL, NULL, TRUE)))
            connected++;
    }
    bs->FreePool(handles);
    in_puts("[input] connect-all pass: handles="); in_putd((long)nh);
    in_puts(" newly-connected="); in_putd(connected);
    in_puts(" (budget left "); in_putd(g_connect_budget); in_puts(")\n");
}

/* =============================================================================
 * Direct i8042 PS/2 mouse driver (x86 only) - the firmware-independent fallback.
 * -----------------------------------------------------------------------------
 * Ports: 0x60 data, 0x64 status/command.
 *   status: 0x01 output-buffer-full  0x02 input-buffer-full  0x20 aux(mouse) byte
 *   cmd(0x64): 0xA8 enable-aux  0x20 read-cmd-byte  0x60 write-cmd-byte
 *              0xD4 "next data byte goes to the mouse"
 *   mouse replies 0xFA (ACK). We POLL (no IRQs) and only ever consume bytes the
 *   controller tags as aux (status bit 0x20), so keyboard bytes are left in the
 *   buffer for whoever owns the keyboard (here: USB / firmware). */
#if defined(__x86_64__) || defined(__i386__)
#define I8042_DATA 0x60
#define I8042_STAT 0x64
#define I8042_CMD  0x64

/* Spin caps kept small: on a machine with no i8042 the status port floats and
 * these never satisfy, so a big cap would stall boot ~1s per command. 20000
 * PIO reads still comfortably covers a real controller's turnaround (a few ms)
 * while making an absent one fail in a fraction of that. */
static int ps2_wait_write(void)            /* input buffer must be empty first */
{
    for (int i = 0; i < 20000; i++)
        if (!(in_inb(I8042_STAT) & 0x02)) return 1;
    return 0;
}
static int ps2_wait_read(void)             /* output buffer must be full        */
{
    for (int i = 0; i < 20000; i++)
        if (in_inb(I8042_STAT) & 0x01) return 1;
    return 0;
}
/* Return whether the controller drained its input buffer, so ps2_mouse_init()
 * can bail the instant an absent i8042 fails to accept the first command. */
static int  ps2_cmd(UINT8 c)   { int ok = ps2_wait_write(); in_outb(I8042_CMD, c);  return ok; }
static int  ps2_write(UINT8 d) { int ok = ps2_wait_write(); in_outb(I8042_DATA, d); return ok; }
static int  ps2_read(void)     { return ps2_wait_read() ? in_inb(I8042_DATA) : -1; }

/* Send one byte to the mouse device and return its ACK (0xFA) or -1. */
static int ps2_mouse_cmd(UINT8 d)
{
    ps2_cmd(0xD4);
    ps2_write(d);
    return ps2_read();
}
static void ps2_flush(void)
{
    for (int i = 0; i < 32 && (in_inb(I8042_STAT) & 0x01); i++) (void)in_inb(I8042_DATA);
}

static void ps2_mouse_init(mouse_state *m)
{
    m->ps2_ok = 0;
    m->ps2_idx = 0;
    m->ps2_bytes = 3;

    ps2_flush();
    /* enable the auxiliary (mouse) device. If the input buffer never drains the
     * i8042 is absent (status port floats to 0xFF) - bail immediately instead of
     * grinding every following command through a full timeout. */
    if (!ps2_cmd(0xA8)) return;

    /* Read controller command byte, clear "mouse clock disable" (0x20) so the
     * aux stream runs; leave the keyboard bits alone. We poll, so no IRQ bits.
     * A missing read here (port floated to 0x00) is likewise "no controller". */
    ps2_cmd(0x20);
    int cb = ps2_read();
    if (cb < 0) return;           /* no answer to read-cmd-byte -> no i8042       */
    cb &= ~0x20;                  /* enable mouse clock                          */
    ps2_cmd(0x60);
    ps2_write((UINT8)cb);

    if (ps2_mouse_cmd(0xF6) != 0xFA) { ps2_flush(); return; }  /* set defaults    */

    /* Try the IntelliMouse magic knock (sample 200,100,80) to unlock the 4-byte
     * packet with a scroll wheel. If the device id comes back 3, keep 4 bytes. */
    ps2_mouse_cmd(0xF3); ps2_mouse_cmd(200);
    ps2_mouse_cmd(0xF3); ps2_mouse_cmd(100);
    ps2_mouse_cmd(0xF3); ps2_mouse_cmd(80);
    if (ps2_mouse_cmd(0xF2) == 0xFA) {          /* get device id                  */
        int id = ps2_read();
        if (id == 3) m->ps2_bytes = 4;
    }
    ps2_mouse_cmd(0xF3); ps2_mouse_cmd(100);    /* 100 samples/s                  */

    if (ps2_mouse_cmd(0xF4) != 0xFA) { ps2_flush(); return; }  /* enable reporting */

    m->ps2_ok = 1;
    m->present++;
    in_puts("[input] + PS/2 mouse (i8042 direct) enabled, packet=");
    in_putd(m->ps2_bytes); in_puts(" bytes\n");
}

/* Drain queued aux bytes, integrate finished packets into dx/dy/buttons/wheel.
 * Returns 1 if any complete packet was applied. */
static int ps2_mouse_poll(mouse_state *m, int *pdx, int *pdy, int *pwheel,
                          int *pleft, int *pright)
{
    if (!m->ps2_ok) return 0;
    int got = 0;
    for (int guard = 0; guard < 128; guard++) {
        UINT8 st = in_inb(I8042_STAT);
        if (!(st & 0x01)) break;              /* output buffer empty            */
        if (!(st & 0x20)) break;              /* head byte is keyboard, not ours */
        UINT8 b = in_inb(I8042_DATA);

        if (m->ps2_idx == 0 && !(b & 0x08)) continue;  /* resync: byte0 has bit3 */
        m->ps2_pkt[m->ps2_idx++] = b;
        if (m->ps2_idx < m->ps2_bytes) continue;
        m->ps2_idx = 0;

        UINT8 f = m->ps2_pkt[0];
        if (f & 0xC0) continue;               /* X/Y overflow -> drop packet      */
        int dx = (int)m->ps2_pkt[1] - ((f & 0x10) ? 256 : 0);
        int dy = (int)m->ps2_pkt[2] - ((f & 0x20) ? 256 : 0);
        *pdx += dx;
        *pdy -= dy;                           /* PS/2 Y is up-positive            */
        *pleft  = (f & 0x01) ? 1 : 0;         /* last packet = current state      */
        *pright = (f & 0x02) ? 1 : 0;
        if (m->ps2_bytes == 4) {
            int z = (int)(m->ps2_pkt[3] & 0x0F);
            if (z & 0x08) z -= 16;            /* sign-extend 4-bit wheel          */
            if (z) *pwheel += (z > 0) ? 1 : -1;
        }
        got = 1;
    }
    return got;
}
#else
static void ps2_mouse_init(mouse_state *m) { (void)m; }
static int  ps2_mouse_poll(mouse_state *m, int *a, int *b, int *c, int *d, int *e)
{ (void)m;(void)a;(void)b;(void)c;(void)d;(void)e; return 0; }
#endif

void input_rescan(EFI_BOOT_SERVICES *bs, mouse_state *m)
{
    if (!m) return;
    if (!bs) bs = (EFI_BOOT_SERVICES *)m->bs;
    if (!bs) return;

    /* Make firmware bind USB/PS2 pointer drivers before we enumerate them.
     * Once ANY pointer has bound, the recursive ConnectController sweep is pure
     * wasted work every rescan, so gate it on "still nothing found". */
    if (!m->present)
        connect_all_controllers(bs);

    /* --- Absolute pointer devices (QEMU usb-tablet -> reliable OVMF path) --- *
     * Enumerate EVERY handle carrying the absolute-pointer GUID, bind and reset
     * each NEW one. Under OVMF the GUID is installed on more than one handle
     * (the physical USB device AND the ConSplitter virtual aggregate, which on
     * its own never delivers motion), and the physical device typically shows
     * up late (async USB enumeration) - hence the re-scan. */
    {
        UINTN nh = 0; EFI_HANDLE *handles = NULL;
        EFI_STATUS st = bs->LocateHandleBuffer(ByProtocol, &abs_guid, NULL, &nh, &handles);
        if (!EFI_ERROR(st) && handles) {
            for (UINTN i = 0; i < nh && m->n_abs < INPUT_MAX_PTR; i++) {
                EFI_ABSOLUTE_POINTER_PROTOCOL *ap = NULL;
                if (handle_known(m->abs_hnd, m->n_abs, (void *)handles[i]))
                    continue;                       /* already bound: leave it alone */
                if (EFI_ERROR(bs->HandleProtocol(handles[i], &abs_guid, (VOID **)&ap)) || !ap)
                    continue;
                if (ap->Reset) ap->Reset(ap, FALSE);
                UINT64 minx = 0, maxx = (UINT64)m->screen_w;
                UINT64 miny = 0, maxy = (UINT64)m->screen_h;
                if (ap->Mode) {
                    minx = ap->Mode->AbsoluteMinX; maxx = ap->Mode->AbsoluteMaxX;
                    miny = ap->Mode->AbsoluteMinY; maxy = ap->Mode->AbsoluteMaxY;
                }
                if (maxx <= minx) { minx = 0; maxx = (UINT64)m->screen_w; }
                if (maxy <= miny) { miny = 0; maxy = (UINT64)m->screen_h; }
                int k = m->n_abs++;
                m->abs_dev[k] = ap;
                m->abs_hnd[k] = (void *)handles[i];
                m->amin_x[k] = minx; m->amax_x[k] = maxx;
                m->amin_y[k] = miny; m->amax_y[k] = maxy;
                m->present++;
                in_puts("[input] + absolute pointer #"); in_putd(k);
                in_puts(" bound, handle="); in_puthex((UINT64)(UINTN)handles[i]);
                in_puts(" range x=["); in_putd((long)minx); in_putc(',');
                in_putd((long)maxx); in_puts("] y=["); in_putd((long)miny);
                in_putc(','); in_putd((long)maxy); in_puts("]\n");
            }
            bs->FreePool(handles);
        }
    }

    /* --- Relative pointer devices (QEMU usb-mouse -> SimplePointer) --------- */
    {
        UINTN nh = 0; EFI_HANDLE *handles = NULL;
        EFI_STATUS st = bs->LocateHandleBuffer(ByProtocol, &sp_guid, NULL, &nh, &handles);
        if (!EFI_ERROR(st) && handles) {
            for (UINTN i = 0; i < nh && m->n_simple < INPUT_MAX_PTR; i++) {
                EFI_SIMPLE_POINTER_PROTOCOL *sp = NULL;
                if (handle_known(m->simple_hnd, m->n_simple, (void *)handles[i]))
                    continue;
                if (EFI_ERROR(bs->HandleProtocol(handles[i], &sp_guid, (VOID **)&sp)) || !sp)
                    continue;
                if (sp->Reset) sp->Reset(sp, FALSE);
                int k = m->n_simple++;
                m->simple_dev[k] = sp;
                m->simple_hnd[k] = (void *)handles[i];
                m->present++;
                in_puts("[input] + simple pointer #"); in_putd(k);
                in_puts(" bound, handle="); in_puthex((UINT64)(UINTN)handles[i]);
                in_puts("\n");
            }
            bs->FreePool(handles);
        }
    }
}

void input_init(EFI_BOOT_SERVICES *bs, mouse_state *m, int screen_w, int screen_h)
{
    if (!m) return;
    zero(m, sizeof(*m));
    m->screen_w = screen_w > 0 ? screen_w : 640;
    m->screen_h = screen_h > 0 ? screen_h : 480;
    m->x = m->screen_w / 2;
    m->y = m->screen_h / 2;
    m->bs = (void *)bs;
    if (!bs) return;

    in_puts("[input] scanning pointer protocols (LocateHandleBuffer over ALL handles)\n");
    input_rescan(bs, m);

    /* Firmware-independent fallback: drive the i8042 PS/2 mouse ourselves. Most
     * OVMF builds expose no working EFI pointer (only the dead ConSplitter
     * aggregate), so this is frequently the ONLY thing that actually moves the
     * cursor - and it covers real laptop trackpads too. */
    ps2_mouse_init(m);

    in_puts("[input] absolute pointer devices = "); in_putd(m->n_abs);
    in_puts(", simple pointer devices = ");         in_putd(m->n_simple);
    in_puts(", screen = "); in_putd(m->screen_w); in_puts("x"); in_putd(m->screen_h);
    in_puts("\n");
    if (m->present == 0)
        in_puts("[input] NO pointer device exposed by firmware yet "
                "(OVMF enumerates USB asynchronously - re-scanning; "
                "PS/2 is never surfaced)\n");
}

int input_available(const mouse_state *m)
{
    return (m && m->present) ? 1 : 0;
}

int input_poll(mouse_state *m)
{
    if (!m || !m->present) return 0;

    int ox = m->x, oy = m->y;
    int newleft = m->left, newright = m->right;
    m->dx = m->dy = m->wheel = 0;
    m->moved = 0;

    /* --- Absolute devices win for positioning ------------------------------ *
     * Poll EVERY bound absolute device and pick the most trustworthy one:
     * firmwares may expose an aggregate that only ever reports (minX,minY)
     * (e.g. the edk2 ConSplitter virtual absolute pointer while it has no
     * physical device attached), so "first device that answered" is not good
     * enough. A device becomes LIVE once it reports a non-origin position;
     * the first live device is authoritative for the cursor. If no device is
     * live yet, the first one that returned a fresh state at all is used.
     * Buttons/wheel are taken from that same winning device (simple-pointer
     * buttons are still merged in below). */
    int abs_reported = 0;
    int abs_best = -1;
    EFI_ABSOLUTE_POINTER_STATE bs_state;   /* always overwritten before it is read */
    for (int d = 0; d < m->n_abs; d++) {
        EFI_ABSOLUTE_POINTER_PROTOCOL *ap = (EFI_ABSOLUTE_POINTER_PROTOCOL *)m->abs_dev[d];
        EFI_ABSOLUTE_POINTER_STATE s;
        if (!ap || !ap->GetState) continue;
        if (EFI_ERROR(ap->GetState(ap, &s))) continue;   /* EFI_NOT_READY */
        abs_reported = 1;
        if (s.CurrentX != m->amin_x[d] || s.CurrentY != m->amin_y[d])
            m->abs_live[d] = 1;                          /* real motion seen */
        if (abs_best < 0 || (m->abs_live[d] && !m->abs_live[abs_best])) {
            abs_best = d;
            bs_state = s;
        }
        /* Once the winner is live it is authoritative: no later device can
         * satisfy (live[d] && !live[abs_best]), so stop scanning. This also
         * short-circuits the dead ConSplitter aggregates, which never go live. */
        if (m->abs_live[abs_best]) break;
    }
    if (abs_best >= 0) {
        int d = abs_best;
        UINT64 rx = m->amax_x[d] - m->amin_x[d];
        UINT64 ry = m->amax_y[d] - m->amin_y[d];
        UINT64 cx = (bs_state.CurrentX < m->amin_x[d]) ? 0 : (bs_state.CurrentX - m->amin_x[d]);
        UINT64 cy = (bs_state.CurrentY < m->amin_y[d]) ? 0 : (bs_state.CurrentY - m->amin_y[d]);
        int nx = rx ? (int)((cx * (UINT64)(m->screen_w - 1)) / rx) : m->x;
        int ny = ry ? (int)((cy * (UINT64)(m->screen_h - 1)) / ry) : m->y;
        m->x = clampi(nx, 0, m->screen_w - 1);
        m->y = clampi(ny, 0, m->screen_h - 1);
        newleft  = (bs_state.ActiveButtons & EFI_ABSP_TouchActive)      ? 1 : 0;
        newright = (bs_state.ActiveButtons & EFI_ABS_POINTER_AltActive) ? 1 : 0;
        if (bs_state.CurrentZ) m->wheel += (bs_state.CurrentZ > 0) ? 1 : -1;
        if (g_log_budget > 0) {
            g_log_budget--;
            in_puts("[input] abs dev "); in_putd(d);
            in_puts(" -> x="); in_putd(m->x); in_puts(" y="); in_putd(m->y);
            in_puts(newleft ? " L" : ""); in_puts("\n");
        }
    }

    /* --- Relative devices -------------------------------------------------- *
     * If no absolute device reported this frame, integrate relative deltas from
     * every bound simple pointer (QEMU usb-mouse). Historic bug: RelativeMovement
     * was DIVIDED by Mode->Resolution ("counts per mm"), collapsing real motion
     * to ~0px. Reference loaders (rEFInd/GRUB) accumulate the raw deltas; we do
     * the same, with a light sensitivity shift so high-resolution mice are not
     * hypersonic. Buttons/wheel are always merged regardless of the absolute
     * path so a usb-mouse click still registers alongside a usb-tablet. */
    for (int d = 0; d < m->n_simple; d++) {
        EFI_SIMPLE_POINTER_PROTOCOL *sp = (EFI_SIMPLE_POINTER_PROTOCOL *)m->simple_dev[d];
        if (!sp || !sp->GetState) continue;

        /* Drain the WHOLE queued report burst this frame - mirrors the i8042 aux
         * drain above. A single GetState() only pops the oldest pending report,
         * so on a busy frame the cursor would lag one report per frame and feel
         * sluggish; loop until GetState returns EFI_NOT_READY and coalesce every
         * report into one accumulated delta, a summed wheel, and the most recent
         * button state. */
        int sdx = 0, sdy = 0, got = 0, lb = 0, rb = 0;
        for (;;) {
            EFI_SIMPLE_POINTER_STATE s;
            if (EFI_ERROR(sp->GetState(sp, &s))) break;  /* queue drained (EFI_NOT_READY) */
            got = 1;
            /* Accumulate raw device deltas as pixels. Mode->Resolution is NOT a
             * divisor here (that was the crawl bug); it only tells us relative
             * sensitivity between axes, which OVMF reports symmetric, so we keep
             * a 1:1 mapping. Guard against absurd single-poll spikes. */
            int rdx = (int)s.RelativeMovementX;
            int rdy = (int)s.RelativeMovementY;
            /* Some firmwares report large per-mm counts; damp only if huge. */
            if (rdx >  200 || rdx < -200) rdx /= 8;
            if (rdy >  200 || rdy < -200) rdy /= 8;
            sdx += rdx;
            sdy += rdy;
            if (s.RelativeMovementZ) m->wheel += (s.RelativeMovementZ > 0) ? 1 : -1;
            lb = s.LeftButton  ? 1 : 0;      /* newest report holds current state */
            rb = s.RightButton ? 1 : 0;
        }
        if (!got) continue;                              /* nothing queued this frame */

        if (!abs_reported && (sdx || sdy)) {
            m->x = clampi(m->x + sdx, 0, m->screen_w - 1);
            m->y = clampi(m->y + sdy, 0, m->screen_h - 1);
            if (g_log_budget > 0) {
                g_log_budget--;
                in_puts("[input] simple dev "); in_putd(d);
                in_puts(" d=("); in_putd(sdx); in_puts(","); in_putd(sdy);
                in_puts(") -> x="); in_putd(m->x); in_puts(" y="); in_putd(m->y);
                in_puts("\n");
            }
        }
        /* Merge buttons across devices (OR) so a usb-mouse click still registers
         * alongside a usb-tablet, using each device's newest report. */
        if (lb) newleft  = 1;
        if (rb) newright = 1;
    }

    /* --- Direct PS/2 mouse (i8042) ----------------------------------------- *
     * The firmware-independent source. Relative like a simple pointer, so we
     * only drive POSITION from it when no absolute device claimed the frame;
     * buttons/wheel always merge. On this OVMF it is the only live pointer. */
    {
        int pdx = 0, pdy = 0, pw = 0, pl = 0, pr = 0;
        if (ps2_mouse_poll(m, &pdx, &pdy, &pw, &pl, &pr)) {
            if (!abs_reported && (pdx || pdy)) {
                m->x = clampi(m->x + pdx, 0, m->screen_w - 1);
                m->y = clampi(m->y + pdy, 0, m->screen_h - 1);
                if (g_log_budget > 0) {
                    g_log_budget--;
                    in_puts("[input] ps2 d=("); in_putd(pdx); in_puts(",");
                    in_putd(pdy); in_puts(") -> x="); in_putd(m->x);
                    in_puts(" y="); in_putd(m->y); in_puts("\n");
                }
            }
            m->wheel += pw;
            /* Merge PS/2 buttons with absolute device buttons (OR) so a real
             * laptop trackpad tap (absolute pointer) and a PS/2 mouse click are
             * both seen even when the other source reports no buttons. */
            if (pl) newleft  = 1;
            if (pr) newright = 1;
        }
    }

    m->dx = m->x - ox;
    m->dy = m->y - oy;
    m->moved = (m->dx || m->dy) ? 1 : 0;

    m->left_pressed    = (!m->prev_left  && newleft ) ? 1 : 0;
    m->left_released   = ( m->prev_left  && !newleft) ? 1 : 0;
    m->right_pressed   = (!m->prev_right && newright) ? 1 : 0;
    m->right_released  = ( m->prev_right && !newright) ? 1 : 0;
    m->prev_left  = newleft;
    m->prev_right = newright;
    m->left  = newleft;
    m->right = newright;

    return (m->moved || m->wheel ||
            m->left_pressed || m->left_released ||
            m->right_pressed || m->right_released) ? 1 : 0;
}

void input_nudge(mouse_state *m, int dx, int dy)
{
    if (!m) return;
    m->x = clampi(m->x + dx, 0, m->screen_w - 1);
    m->y = clampi(m->y + dy, 0, m->screen_h - 1);
    m->dx = dx; m->dy = dy;
    m->moved = (dx || dy) ? 1 : 0;
}

/* A compact 12x19 arrow cursor. '1' = body, '2' = outline, ' ' = transparent. */
static const char *const CURSOR[] = {
    "2",
    "22",
    "212",
    "2112",
    "21112",
    "211112",
    "2111112",
    "21111112",
    "211111112",
    "2111111112",
    "21111111112",
    "211111122222",
    "211112112",
    "21112 2112",
    "2112  2112",
    "212    2112",
    "22     2112",
    "2       212",
    "         22",
};
#define CURSOR_ROWS ((int)(sizeof(CURSOR) / sizeof(CURSOR[0])))
#define CURSOR_OUTLINE 0x00101010u

void input_draw_cursor(const mouse_state *m, UINT32 fill)
{
    if (!m) return;
    /* Scale the tiny sprite up on hi-res panels so it stays visible. */
    int s = ui_scale();
    if (s < 1) s = 1;
    for (int r = 0; r < CURSOR_ROWS; r++) {
        const char *row = CURSOR[r];
        int ybase = m->y + r * s;                /* loop-invariant row origin */
        for (int c = 0; row[c]; ) {
            char g = row[c];
            if (g != '1' && g != '2') { c++; continue; }   /* transparent */
            /* Coalesce a maximal horizontal run of the same glyph into one
             * fill_rect - adjacent same-color cells tile contiguously, so the
             * pixels drawn are identical to emitting them one at a time. */
            int c0 = c;
            do { c++; } while (row[c] == g);
            UINT32 col = (g == '1') ? fill : CURSOR_OUTLINE;
            fill_rect(m->x + c0 * s, ybase, (c - c0) * s, s, col);
        }
    }
}
