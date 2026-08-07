/**
 * Forest-OS X11 Server
 *
 * Implements a minimal but functional X11R6 server that lets Xlib clients
 * (xterm, xclock, …) connect, create windows, and render to them.
 *
 * Architecture
 * ============
 * Because Forest-OS does not yet have a POSIX socket layer with blocking
 * accept(), the server uses an in-kernel IPC model:
 *
 *   userspace shim / syscall
 *       |
 *       v
 *   x11_client_write()  ──► server-side recv ring ──► request parser
 *   x11_client_read()   ◄── server-side send ring ◄── reply builder
 *
 * Each "client slot" owns two x11_ipc_ring_t rings (rx = data from client,
 * tx = data to client).  The kernel task or poll thread calls
 * x11_server_pump() periodically to process pending data.
 *
 * Rendering
 * =========
 * Requests that draw (PolyFillRectangle, PutImage, ImageText8 …) operate on
 * the window's WM surface via the graphics_manager and window_manager APIs.
 * After any draw the window is marked dirty and compositor_update() is called.
 *
 * Events
 * ======
 * x11_input_event_callback() is registered with input_mux so that keyboard
 * and pointer events from device drivers flow into per-client event queues
 * and are converted to X11 wire format.
 */

#include "include/x11_server.h"
#include "include/memory.h"
#include "include/util.h"
#include "include/string.h"
#include "include/debuglog.h"
#include "include/spinlock.h"
#include "include/input_mux.h"
#include "include/graphics/graphics_manager.h"
#include "include/graphics/window_manager.h"
#include "include/graphics/font8x8.h"
#include "include/gfx_config.h"

#if HAS_GRAPHICS

/* =========================================================
 *  Tunables
 * ========================================================= */
#define MAX_X11_CLIENTS     16
#define MAX_X11_WINDOWS     64
#define MAX_X11_GCS         64
#define MAX_X11_PIXMAPS     32
#define MAX_X11_ATOMS       64
#define MAX_EVENTS_PER_WIN  32   /* per-window pending event queue depth */

/* =========================================================
 *  X11 wire-format helpers
 * ========================================================= */
static uint16 x11_u16le(const uint8 *p) {
    return (uint16)((uint16)p[0] | ((uint16)p[1] << 8));
}
static uint32 x11_u32le(const uint8 *p) {
    return ((uint32)p[0])       |
           ((uint32)p[1] <<  8) |
           ((uint32)p[2] << 16) |
           ((uint32)p[3] << 24);
}
static int32 x11_i32le(const uint8 *p) { return (int32)x11_u32le(p); }

static void x11_put16le(uint8 *p, uint16 v) {
    p[0] = (uint8)(v);
    p[1] = (uint8)(v >> 8);
}
static void x11_put32le(uint8 *p, uint32 v) {
    p[0] = (uint8)(v);
    p[1] = (uint8)(v >>  8);
    p[2] = (uint8)(v >> 16);
    p[3] = (uint8)(v >> 24);
}

/* =========================================================
 *  IPC ring helpers
 * ========================================================= */
static uint32 ipc_ring_used(const x11_ipc_ring_t *r) {
    return (r->head - r->tail) % X11_IPC_BUF_SIZE;
}
__attribute__((unused)) static uint32 ipc_ring_free(const x11_ipc_ring_t *r) {
    return X11_IPC_BUF_SIZE - 1 - ipc_ring_used(r);
}
__attribute__((unused)) static bool ipc_ring_empty(const x11_ipc_ring_t *r) {
    return r->head == r->tail;
}

/* Push bytes into a ring; returns bytes actually written */
static uint32 ipc_ring_write(x11_ipc_ring_t *r, const uint8 *src, uint32 len) {
    uint32 i;
    uint32 written = 0;
    for (i = 0; i < len; i++) {
        uint32 next = (r->head + 1) % X11_IPC_BUF_SIZE;
        if (next == r->tail) break;  /* full */
        r->data[r->head] = src[i];
        r->head = next;
        written++;
    }
    return written;
}

/* Pop bytes from a ring; returns bytes actually read */
static uint32 ipc_ring_read(x11_ipc_ring_t *r, uint8 *dst, uint32 len) {
    uint32 i;
    uint32 read_cnt = 0;
    for (i = 0; i < len; i++) {
        if (r->tail == r->head) break;  /* empty */
        dst[i] = r->data[r->tail];
        r->tail = (r->tail + 1) % X11_IPC_BUF_SIZE;
        read_cnt++;
    }
    return read_cnt;
}

/* Peek at ring without consuming */
static bool ipc_ring_peek(const x11_ipc_ring_t *r, uint8 *dst, uint32 len) {
    uint32 i;
    uint32 pos = r->tail;
    if (ipc_ring_used(r) < len) return false;
    for (i = 0; i < len; i++) {
        dst[i] = r->data[pos];
        pos = (pos + 1) % X11_IPC_BUF_SIZE;
    }
    return true;
}

/* Drop bytes from ring */
static void ipc_ring_drop(x11_ipc_ring_t *r, uint32 len) {
    uint32 i;
    for (i = 0; i < len && r->tail != r->head; i++) {
        r->tail = (r->tail + 1) % X11_IPC_BUF_SIZE;
    }
}

/* =========================================================
 *  X11 pending-event queue (per window)
 * ========================================================= */
typedef struct {
    uint8  wire[32];   /* X11 event wire bytes (always 32 bytes) */
    bool   used;
} x11_pending_event_t;

/* =========================================================
 *  GC (Graphics Context)
 * ========================================================= */
typedef struct {
    bool   used;
    uint32 gc_id;
    uint32 foreground;   /* ARGB pixel */
    uint32 background;   /* ARGB pixel */
    uint8  function;     /* GX_COPY=3, etc. */
    uint32 plane_mask;
    uint16 line_width;
    uint8  font;         /* unused for now – we use font8x8 always */
} x11_gc_t;

/* =========================================================
 *  Pixmap
 * ========================================================= */
typedef struct {
    bool             used;
    uint32           pix_id;
    uint16           width;
    uint16           height;
    uint8            depth;
    graphics_surface_t *surface;
} x11_pixmap_t;

/* =========================================================
 *  Atom table
 * ========================================================= */
typedef struct {
    bool   used;
    uint32 atom_id;
    char   name[64];
} x11_atom_entry_t;

/* =========================================================
 *  Window
 * ========================================================= */
typedef struct {
    bool             used;
    uint32           x11_id;       /* X11 resource id from client */
    uint32           parent_x11;
    window_handle_t  wm_handle;    /* WM window handle (0 = no WM window) */
    int16            x, y;
    uint16           width, height;
    uint16           border_width;
    uint32           event_mask;
    uint32           bg_pixel;
    bool             mapped;
    bool             override_redirect;
    int              owner_client;  /* client slot index */
    char             title[128];

    /* Pending events for this window */
    x11_pending_event_t events[MAX_EVENTS_PER_WIN];
    uint8 ev_head, ev_tail;  /* indices into events[] */
} x11_window_t;

/* =========================================================
 *  Client slot
 * ========================================================= */
#define X11_CLIENT_STATE_FREE       0
#define X11_CLIENT_STATE_HANDSHAKE  1   /* waiting for setup bytes */
#define X11_CLIENT_STATE_READY      2   /* fully connected */

typedef struct {
    uint8           state;
    x11_ipc_ring_t  rx;     /* bytes from client */
    x11_ipc_ring_t  tx;     /* bytes to client   */
    uint32          next_seq;
    bool            byte_order_msb; /* client uses big-endian if true */
    uint32          base_resource;  /* first resource id hint from client */
} x11_client_t;

/* =========================================================
 *  Server global state
 * ========================================================= */
typedef struct {
    bool            initialized;
    spinlock_t      lock;

    x11_client_t    clients[MAX_X11_CLIENTS];
    x11_window_t    windows[MAX_X11_WINDOWS];
    x11_gc_t        gcs[MAX_X11_GCS];
    x11_pixmap_t    pixmaps[MAX_X11_PIXMAPS];
    x11_atom_entry_t atoms[MAX_X11_ATOMS];
    uint32          atom_next_id;

    /* Global pointer state for MotionNotify */
    int16  ptr_x, ptr_y;
    uint8  ptr_buttons;

    /* Focused window (for keyboard events) */
    uint32 focused_x11_id;

    /* Input mux consumer registration */
    input_consumer_t input_consumer;
    input_ring_t     input_ring;
} x11_server_t;

static x11_server_t g_x11;

/* =========================================================
 *  Lookup helpers
 * ========================================================= */
static x11_window_t *find_window(uint32 x11_id) {
    int i;
    for (i = 0; i < MAX_X11_WINDOWS; i++) {
        if (g_x11.windows[i].used && g_x11.windows[i].x11_id == x11_id)
            return &g_x11.windows[i];
    }
    return NULL;
}

static x11_gc_t *find_gc(uint32 gc_id) {
    int i;
    for (i = 0; i < MAX_X11_GCS; i++) {
        if (g_x11.gcs[i].used && g_x11.gcs[i].gc_id == gc_id)
            return &g_x11.gcs[i];
    }
    return NULL;
}

static x11_pixmap_t *find_pixmap(uint32 pix_id) {
    int i;
    for (i = 0; i < MAX_X11_PIXMAPS; i++) {
        if (g_x11.pixmaps[i].used && g_x11.pixmaps[i].pix_id == pix_id)
            return &g_x11.pixmaps[i];
    }
    return NULL;
}

/* =========================================================
 *  Atom management
 * ========================================================= */
static void atoms_init(void) {
    /* Pre-populate well-known atoms */
    int idx = 0;

#define ADD_ATOM(id, str) do {                          \
    g_x11.atoms[idx].used    = true;                    \
    g_x11.atoms[idx].atom_id = (id);                   \
    strncpy(g_x11.atoms[idx].name, (str),              \
            sizeof(g_x11.atoms[idx].name) - 1);        \
    idx++;                                              \
} while(0)

    ADD_ATOM(X11_ATOM_STRING,           "STRING");
    ADD_ATOM(X11_ATOM_WM_PROTOCOLS,     "WM_PROTOCOLS");
    ADD_ATOM(X11_ATOM_WM_DELETE_WINDOW, "WM_DELETE_WINDOW");
    ADD_ATOM(X11_ATOM_NET_WM_NAME,      "_NET_WM_NAME");
    ADD_ATOM(X11_ATOM_UTF8_STRING,      "UTF8_STRING");

#undef ADD_ATOM

    g_x11.atom_next_id = 2000;  /* dynamic atoms start here */
}

static uint32 atom_intern(const char *name, bool only_if_exists) {
    int i;
    size_t len;

    for (i = 0; i < MAX_X11_ATOMS; i++) {
        if (g_x11.atoms[i].used &&
            strcmp(g_x11.atoms[i].name, name) == 0)
            return g_x11.atoms[i].atom_id;
    }
    if (only_if_exists) return X11_ATOM_NONE;

    /* Create new atom */
    for (i = 0; i < MAX_X11_ATOMS; i++) {
        if (!g_x11.atoms[i].used) {
            g_x11.atoms[i].used    = true;
            g_x11.atoms[i].atom_id = g_x11.atom_next_id++;
            len = strlen(name);
            if (len >= sizeof(g_x11.atoms[i].name))
                len = sizeof(g_x11.atoms[i].name) - 1;
            memcpy(g_x11.atoms[i].name, name, len);
            g_x11.atoms[i].name[len] = '\0';
            return g_x11.atoms[i].atom_id;
        }
    }
    return X11_ATOM_NONE;
}

static const char *atom_name(uint32 atom_id) {
    int i;
    for (i = 0; i < MAX_X11_ATOMS; i++) {
        if (g_x11.atoms[i].used && g_x11.atoms[i].atom_id == atom_id)
            return g_x11.atoms[i].name;
    }
    return NULL;
}

/* =========================================================
 *  Surface / pixel helpers
 * ========================================================= */

/* Convert a 32-bit ARGB (0xAARRGGBB) value to the native surface pixel. */
static uint32 argb_to_native(uint32 argb, pixel_format_t fmt) {
    uint8 r = (uint8)((argb >> 16) & 0xFF);
    uint8 g = (uint8)((argb >>  8) & 0xFF);
    uint8 b = (uint8)((argb)       & 0xFF);
    uint8 a = (uint8)((argb >> 24) & 0xFF);
    graphics_color_t col;
    col.r = r; col.g = g; col.b = b; col.a = a;
    return graphics_color_to_pixel(col, fmt);
}

/* Draw a single pixel on a surface (bounds-checked). */
static void surf_put_pixel(graphics_surface_t *s, int32 x, int32 y, uint32 native_pixel) {
    uint32 *row;
    if (!s || !s->pixels) return;
    if (x < 0 || y < 0 || (uint32)x >= s->width || (uint32)y >= s->height) return;
    row = (uint32 *)((uint8 *)s->pixels + (uint32)y * s->pitch);
    row[x] = native_pixel;
}

/* Fill an axis-aligned rectangle on a surface. */
static void surf_fill_rect(graphics_surface_t *s,
                           int32 rx, int32 ry, int32 rw, int32 rh,
                           uint32 native_pixel) {
    int32 x, y;
    if (!s || !s->pixels || rw <= 0 || rh <= 0) return;
    for (y = ry; y < ry + rh; y++) {
        uint32 *row;
        if (y < 0 || (uint32)y >= s->height) continue;
        row = (uint32 *)((uint8 *)s->pixels + (uint32)y * s->pitch);
        for (x = rx; x < rx + rw; x++) {
            if (x < 0 || (uint32)x >= s->width) continue;
            row[x] = native_pixel;
        }
    }
}

/* Render one 8x8 glyph onto a surface. */
static void surf_draw_char(graphics_surface_t *s,
                           int32 cx, int32 cy,
                           uint8 ch,
                           uint32 fg_native, uint32 bg_native,
                           bool transparent_bg) {
    const char *glyph;
    int row, col;

    if (!s || !s->pixels) return;
    glyph = font8x8_get_glyph((uint32)ch);
    if (!glyph) return;

    for (row = 0; row < 8; row++) {
        uint8 bits = (uint8)glyph[row];
        for (col = 0; col < 8; col++) {
            int32 px = cx + col;
            int32 py = cy + row;
            if (px < 0 || py < 0 ||
                (uint32)px >= s->width || (uint32)py >= s->height) continue;
            if (bits & (1 << col)) {
                surf_put_pixel(s, px, py, fg_native);
            } else if (!transparent_bg) {
                surf_put_pixel(s, px, py, bg_native);
            }
        }
    }
}

/* Blit src_surface region onto dst_surface. */
static void surf_blit(graphics_surface_t *dst, int32 dx, int32 dy,
                      graphics_surface_t *src,
                      int32 sx, int32 sy, int32 sw, int32 sh) {
    int32 x, y;
    if (!dst || !src || !dst->pixels || !src->pixels) return;
    for (y = 0; y < sh; y++) {
        int32 sy2 = sy + y, dy2 = dy + y;
        uint32 *srow, *drow;
        if (sy2 < 0 || (uint32)sy2 >= src->height) continue;
        if (dy2 < 0 || (uint32)dy2 >= dst->height) continue;
        srow = (uint32 *)((uint8 *)src->pixels + (uint32)sy2 * src->pitch);
        drow = (uint32 *)((uint8 *)dst->pixels + (uint32)dy2 * dst->pitch);
        for (x = 0; x < sw; x++) {
            int32 sx2 = sx + x, dx2 = dx + x;
            if (sx2 < 0 || (uint32)sx2 >= src->width) continue;
            if (dx2 < 0 || (uint32)dx2 >= dst->width) continue;
            drow[dx2] = srow[sx2];
        }
    }
}

/* =========================================================
 *  Transmit helpers – write bytes into the client's TX ring
 * ========================================================= */
static void tx_write(x11_client_t *cl, const uint8 *buf, uint32 len) {
    uint32 written = ipc_ring_write(&cl->tx, buf, len);
    if (written < len) {
        debuglog(DEBUG_WARN, "[X11] TX ring overflow: lost %u bytes\n", len - written);
    }
}

/* Send a 32-byte zero-padded reply.  seq is filled in automatically. */
static void tx_reply(x11_client_t *cl, uint8 *buf32) {
    /* byte 0 = 1 (reply), bytes 2-3 = sequence number */
    buf32[0] = 1;
    x11_put16le(buf32 + 2, (uint16)cl->next_seq);
    tx_write(cl, buf32, 32);
}

/* Send a 32-byte X11 error packet. */
static void tx_error(x11_client_t *cl,
                     uint8 error_code, uint8 opcode,
                     uint32 bad_resource) {
    uint8 buf[32];
    memset(buf, 0, 32);
    buf[0] = 0;                    /* error marker */
    buf[1] = error_code;
    x11_put16le(buf + 2, (uint16)cl->next_seq);
    x11_put32le(buf + 4, bad_resource);
    x11_put16le(buf + 8, (uint16)opcode);
    tx_write(cl, buf, 32);
}

/* Queue a 32-byte event into a window's pending event ring. */
static void window_queue_event(x11_window_t *w, const uint8 *ev32) {
    uint8 next = (uint8)((w->ev_head + 1) % MAX_EVENTS_PER_WIN);
    if (next == w->ev_tail) {
        debuglog(DEBUG_DETAIL, "[X11] event queue full for window %u\n", w->x11_id);
        return;
    }
    memcpy(w->events[w->ev_head].wire, ev32, 32);
    w->events[w->ev_head].used = true;
    w->ev_head = next;
}

/* Flush pending events from all of a client's windows into the TX ring. */
static void flush_events_for_client(int cid) {
    x11_client_t *cl = &g_x11.clients[cid];
    int i;
    if (cl->state != X11_CLIENT_STATE_READY) return;
    for (i = 0; i < MAX_X11_WINDOWS; i++) {
        x11_window_t *w = &g_x11.windows[i];
        if (!w->used || w->owner_client != cid) continue;
        while (w->ev_tail != w->ev_head) {
            tx_write(cl, w->events[w->ev_tail].wire, 32);
            w->events[w->ev_tail].used = false;
            w->ev_tail = (uint8)((w->ev_tail + 1) % MAX_EVENTS_PER_WIN);
        }
    }
}

/* =========================================================
 *  Build standard X11 events (32 bytes each)
 * ========================================================= */
static void build_expose_event(uint8 *ev, uint32 wid, uint16 x, uint16 y,
                               uint16 w, uint16 h, uint16 count) {
    memset(ev, 0, 32);
    ev[0] = X11_EVENT_EXPOSE;
    x11_put32le(ev + 4,  wid);
    x11_put16le(ev + 8,  x);
    x11_put16le(ev + 10, y);
    x11_put16le(ev + 12, w);
    x11_put16le(ev + 14, h);
    x11_put16le(ev + 16, count);
}

static void build_key_event(uint8 *ev, uint8 event_code, uint8 keycode,
                            uint16 state, uint32 wid, uint32 root,
                            int16 ptr_x, int16 ptr_y, uint32 time) {
    memset(ev, 0, 32);
    ev[0] = event_code;
    ev[1] = keycode;
    x11_put32le(ev + 4,  time);
    x11_put32le(ev + 8,  root);        /* root window */
    x11_put32le(ev + 12, wid);        /* event window */
    x11_put32le(ev + 16, wid);        /* child */
    x11_put16le(ev + 20, (uint16)ptr_x);
    x11_put16le(ev + 22, (uint16)ptr_y);
    x11_put16le(ev + 24, (uint16)ptr_x);
    x11_put16le(ev + 26, (uint16)ptr_y);
    x11_put16le(ev + 28, state);
    ev[30] = 1;  /* same-screen */
}

static void build_button_event(uint8 *ev, uint8 event_code, uint8 button,
                               uint16 state, uint32 wid,
                               int16 px, int16 py, uint32 time) {
    memset(ev, 0, 32);
    ev[0] = event_code;
    ev[1] = button;
    x11_put32le(ev + 4,  time);
    x11_put32le(ev + 8,  1);          /* root window id */
    x11_put32le(ev + 12, wid);
    x11_put32le(ev + 16, wid);
    x11_put16le(ev + 20, (uint16)px);
    x11_put16le(ev + 22, (uint16)py);
    x11_put16le(ev + 24, (uint16)px);
    x11_put16le(ev + 26, (uint16)py);
    x11_put16le(ev + 28, state);
    ev[30] = 1;
}

static void build_motion_event(uint8 *ev, uint32 wid,
                               int16 px, int16 py, uint32 time) {
    memset(ev, 0, 32);
    ev[0] = X11_EVENT_MOTION_NOTIFY;
    x11_put32le(ev + 4,  time);
    x11_put32le(ev + 8,  1);
    x11_put32le(ev + 12, wid);
    x11_put32le(ev + 16, wid);
    x11_put16le(ev + 20, (uint16)px);
    x11_put16le(ev + 22, (uint16)py);
    x11_put16le(ev + 24, (uint16)px);
    x11_put16le(ev + 26, (uint16)py);
    ev[30] = 1;
}

/* =========================================================
 *  Connection setup reply (X11R6 wire format)
 * ========================================================= */
#define SCREEN_WIDTH    1024
#define SCREEN_HEIGHT   768
#define ROOT_WINDOW_ID  1
#define ROOT_DEPTH      24
#define ROOT_VISUAL_ID  0x22

static void send_setup_success(x11_client_t *cl) {
    /*
     * Fixed-size header = 8 bytes
     * Vendor string = 0 bytes (padded to 4)
     * pixmap-formats = 1 × 8 bytes
     * screen info    = 1 screen × (40 + depth_info_size)
     *   depth 24: 2 visuals × 24 bytes = 48; depth header = 8; total = 56
     * Total additional data = 0+8+8+56 = 72 bytes  → 18 uint32 words
     *
     * X11 field "length of additional data in units of 4 bytes after 8-byte header"
     * = (total_bytes - 8) / 4
     */

    uint8 buf[256];
    uint16 add_len;
    uint32 off;

    memset(buf, 0, sizeof(buf));

    /* Success byte */
    buf[0] = 1;
    buf[1] = 0;  /* unused */

    /* Protocol version */
    x11_put16le(buf + 2, 11);   /* major */
    x11_put16le(buf + 4, 0);    /* minor */

    /* Placeholder for 'additional data length in 4-byte units' at offset 6 */
    off = 8;  /* start of additional data */

    /* --- Resource id base / mask --- */
    x11_put32le(buf + off, 0x00200000); off += 4;  /* resource-id-base  */
    x11_put32le(buf + off, 0x001FFFFF); off += 4;  /* resource-id-mask  */
    x11_put32le(buf + off, 0);          off += 4;  /* motion-buffer-size */

    /* Vendor length (0) */
    x11_put16le(buf + off, 0); off += 2;

    /* max-request-length */
    x11_put16le(buf + off, 0xFFFF); off += 2;

    /* number of roots = 1 */
    buf[off] = 1; off += 1;

    /* number of pixmap formats = 1 */
    buf[off] = 1; off += 1;

    /* image-byte-order: 0=LSBFirst */
    buf[off] = 0; off += 1;
    /* bitmap-bit-order: 0=LeastSignificant */
    buf[off] = 0; off += 1;

    /* bitmap-scanline-unit */
    buf[off] = 32; off += 1;
    /* bitmap-scanline-pad */
    buf[off] = 32; off += 1;
    /* min-keycode */
    buf[off] = 8;  off += 1;
    /* max-keycode */
    buf[off] = 255; off += 1;

    /* padding */
    off += 4;

    /* --- Pixmap format (8 bytes): depth=24, bits-per-pixel=32, scanline-pad=32 --- */
    buf[off] = ROOT_DEPTH; off += 1;   /* depth */
    buf[off] = 32;         off += 1;   /* bits-per-pixel */
    buf[off] = 32;         off += 1;   /* scanline-pad */
    off += 5;                          /* pad */

    /* --- Screen info (40 bytes fixed + depths) --- */
    x11_put32le(buf + off, ROOT_WINDOW_ID); off += 4;  /* root window */
    x11_put32le(buf + off, 0x00FFFFFF);     off += 4;  /* default colormap */
    x11_put32le(buf + off, 0x00FFFFFF);     off += 4;  /* white-pixel */
    x11_put32le(buf + off, 0x00000000);     off += 4;  /* black-pixel */

    x11_put32le(buf + off, 0); off += 4;  /* current-input-masks */

    x11_put16le(buf + off, SCREEN_WIDTH);  off += 2;
    x11_put16le(buf + off, SCREEN_HEIGHT); off += 2;
    x11_put16le(buf + off, 338);           off += 2;  /* width  in mm  */
    x11_put16le(buf + off, 190);           off += 2;  /* height in mm  */

    x11_put16le(buf + off, 1);  off += 2;  /* min-installed-maps */
    x11_put16le(buf + off, 1);  off += 2;  /* max-installed-maps */
    x11_put32le(buf + off, ROOT_VISUAL_ID); off += 4;  /* root-visual */

    buf[off] = 2; off += 1;  /* backing-stores: WhenMapped */
    buf[off] = 0; off += 1;  /* save-unders */
    buf[off] = ROOT_DEPTH; off += 1;  /* root-depth */
    buf[off] = 1; off += 1;  /* number of allowed depths */

    /* Depth 24 info (8 bytes + visuals) */
    buf[off] = ROOT_DEPTH; off += 1;    /* depth */
    buf[off] = 0;          off += 1;    /* unused */
    x11_put16le(buf + off, 1); off += 2; /* number of visuals */
    off += 4;                            /* unused */

    /* Visual (24 bytes) */
    x11_put32le(buf + off, ROOT_VISUAL_ID); off += 4;
    buf[off] = 4;  off += 1;   /* class: TrueColor */
    buf[off] = 8;  off += 1;   /* bits-per-rgb-value */
    x11_put16le(buf + off, 256); off += 2;  /* colormap-entries */
    x11_put32le(buf + off, 0x00FF0000); off += 4; /* red-mask   */
    x11_put32le(buf + off, 0x0000FF00); off += 4; /* green-mask */
    x11_put32le(buf + off, 0x000000FF); off += 4; /* blue-mask  */
    off += 4;  /* unused */

    /* Fill in 'additional data in 4-byte units' at offset 6 */
    add_len = (uint16)((off - 8) / 4);
    x11_put16le(buf + 6, add_len);

    tx_write(cl, buf, off);
    debuglog(DEBUG_INFO, "[X11] Sent setup-success (%u bytes, addlen=%u)\n", off, add_len);
}

/* =========================================================
 *  Request handlers
 * ========================================================= */

static void handle_create_window(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    x11_window_t *w;
    uint32 x11_id, parent_id;
    int16  wx, wy;
    uint16 ww, wh, bw;
    uint32 value_mask, voff;
    int i;

    if (len < 32) { tx_error(cl, 16, X11_REQ_CREATE_WINDOW, 0); return; }

    /* byte 1 = depth (ignored for now) */
    x11_id    = x11_u32le(req + 4);
    parent_id = x11_u32le(req + 8);
    wx = (int16)x11_u16le(req + 12);
    wy = (int16)x11_u16le(req + 14);
    ww = x11_u16le(req + 16);
    wh = x11_u16le(req + 18);
    bw = x11_u16le(req + 20);
    /* class = req+22, visual = req+24 */
    value_mask = x11_u32le(req + 28);
    voff       = 32;

    /* Find a free window slot */
    w = NULL;
    for (i = 0; i < MAX_X11_WINDOWS; i++) {
        if (!g_x11.windows[i].used) { w = &g_x11.windows[i]; break; }
    }
    if (!w) { tx_error(cl, 11, X11_REQ_CREATE_WINDOW, x11_id); return; }

    memset(w, 0, sizeof(*w));
    w->used         = true;
    w->x11_id       = x11_id;
    w->parent_x11   = parent_id;
    w->x            = wx;
    w->y            = wy;
    w->width        = (ww > 0) ? ww : 100;
    w->height       = (wh > 0) ? wh : 100;
    w->border_width = bw;
    w->bg_pixel     = 0x00C0C0C0;
    w->event_mask   = 0;
    w->mapped       = false;
    w->wm_handle    = INVALID_WINDOW_HANDLE;
    w->owner_client = cid;
    w->ev_head      = 0;
    w->ev_tail      = 0;

    /* Parse value list */
    {
        uint32 bit;
        for (bit = 0; bit < 32; bit++) {
            if (!(value_mask & (1U << bit))) continue;
            if (voff + 4 > len) break;
            switch (bit) {
                case 1:  w->bg_pixel   = x11_u32le(req + voff); break;
                case 11: w->event_mask = x11_u32le(req + voff); break;
                case 9:  /* override-redirect */
                    w->override_redirect = (x11_u32le(req + voff) != 0);
                    break;
                default: break;
            }
            voff += 4;
        }
    }

    debuglog(DEBUG_INFO,
             "[X11] CreateWindow id=%u parent=%u %dx%d+%d+%d em=0x%x\n",
             x11_id, parent_id, (int)w->width, (int)w->height,
             (int)wx, (int)wy, w->event_mask);

    /* No reply – CreateWindow is a void request */
    (void)cl;
}

static void handle_destroy_window(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    x11_window_t *w;
    uint32 x11_id;

    if (len < 8) { tx_error(cl, 16, X11_REQ_DESTROY_WINDOW, 0); return; }
    x11_id = x11_u32le(req + 4);
    w = find_window(x11_id);
    if (!w) return;

    if (w->wm_handle != INVALID_WINDOW_HANDLE) {
        window_destroy(w->wm_handle);
    }
    if (g_x11.focused_x11_id == x11_id)
        g_x11.focused_x11_id = 0;
    memset(w, 0, sizeof(*w));
}

static void handle_map_window(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    x11_window_t *w;
    uint32 x11_id;
    uint8  ev[32];

    if (len < 8) { tx_error(cl, 16, X11_REQ_MAP_WINDOW, 0); return; }
    x11_id = x11_u32le(req + 4);
    w = find_window(x11_id);
    if (!w) return;

    if (!w->mapped) {
        w->mapped = true;

        /* Create a real WM window if not already */
        if (w->wm_handle == INVALID_WINDOW_HANDLE && window_manager_is_initialized()) {
            w->wm_handle = window_create(
                w->x, w->y, w->width, w->height,
                (w->title[0] ? w->title : "X11 Window"),
                WINDOW_FLAGS_DEFAULT);
        }

        /* Send Expose event */
        if (w->event_mask & X11_MASK_EXPOSURE) {
            build_expose_event(ev, x11_id, 0, 0, w->width, w->height, 0);
            window_queue_event(w, ev);
        }

        /* Fill window background */
        if (w->wm_handle != INVALID_WINDOW_HANDLE) {
            graphics_surface_t *surf = NULL;
            if (window_get_surface(w->wm_handle, &surf) == GRAPHICS_SUCCESS && surf) {
                video_mode_t mode;
                uint32 bgpix = 0;
                if (graphics_get_current_mode(&mode) == GRAPHICS_SUCCESS)
                    bgpix = argb_to_native(w->bg_pixel, mode.format);
                surf_fill_rect(surf, 0, 0, (int32)surf->width, (int32)surf->height, bgpix);
                window_invalidate(w->wm_handle);
                compositor_update();
            }
        }

        debuglog(DEBUG_INFO, "[X11] MapWindow id=%u wm=%u\n", x11_id, w->wm_handle);
    }
    (void)cl;
}

static void handle_unmap_window(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    x11_window_t *w;
    uint32 x11_id;

    if (len < 8) { tx_error(cl, 16, X11_REQ_UNMAP_WINDOW, 0); return; }
    x11_id = x11_u32le(req + 4);
    w = find_window(x11_id);
    if (!w) return;

    w->mapped = false;
    if (w->wm_handle != INVALID_WINDOW_HANDLE) {
        window_hide(w->wm_handle);
        compositor_update();
    }
    (void)cl;
}

static void handle_configure_window(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    x11_window_t *w;
    uint32 x11_id, value_mask, voff, bit;

    if (len < 12) { tx_error(cl, 16, X11_REQ_CONFIGURE_WINDOW, 0); return; }
    x11_id     = x11_u32le(req + 4);
    value_mask = x11_u32le(req + 8);  /* actually 16-bit per spec, but we read 32 */
    voff       = 12;

    w = find_window(x11_id);
    if (!w) { tx_error(cl, 3, X11_REQ_CONFIGURE_WINDOW, x11_id); return; }

    for (bit = 0; bit < 7; bit++) {
        if (!(value_mask & (1U << bit))) continue;
        if (voff + 4 > len) break;
        int32 val = x11_i32le(req + voff);
        switch (bit) {
            case 0: w->x = (int16)val;  break;
            case 1: w->y = (int16)val;  break;
            case 2: if (val > 0) w->width  = (uint16)val; break;
            case 3: if (val > 0) w->height = (uint16)val; break;
            default: break;
        }
        voff += 4;
    }

    if (w->wm_handle != INVALID_WINDOW_HANDLE) {
        window_set_position(w->wm_handle, w->x, w->y);
        compositor_update();
    }
    (void)cl;
}

static void handle_get_geometry(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    x11_window_t *w;
    x11_pixmap_t *pm;
    uint32 res_id;
    uint8  reply[32];
    uint16 rw = 0, rh = 0;
    int16  rx = 0, ry = 0;
    uint16 rbw = 0;
    uint8  rdepth = ROOT_DEPTH;

    if (len < 8) { tx_error(cl, 16, X11_REQ_GET_GEOMETRY, 0); return; }
    res_id = x11_u32le(req + 4);

    w  = find_window(res_id);
    pm = find_pixmap(res_id);

    if (w) {
        rx = w->x; ry = w->y;
        rw = w->width; rh = w->height;
        rbw = w->border_width;
    } else if (pm) {
        rw = pm->width; rh = pm->height;
        rdepth = pm->depth;
    } else if (res_id == ROOT_WINDOW_ID) {
        rw = SCREEN_WIDTH; rh = SCREEN_HEIGHT;
    } else {
        tx_error(cl, 3, X11_REQ_GET_GEOMETRY, res_id); return;
    }

    memset(reply, 0, 32);
    reply[1] = rdepth;
    x11_put32le(reply + 4, 0);         /* length of additional data = 0 */
    x11_put32le(reply + 8, ROOT_WINDOW_ID); /* root */
    x11_put16le(reply + 12, (uint16)rx);
    x11_put16le(reply + 14, (uint16)ry);
    x11_put16le(reply + 16, rw);
    x11_put16le(reply + 18, rh);
    x11_put16le(reply + 20, rbw);
    tx_reply(cl, reply);
}

static void handle_query_tree(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    uint32 x11_id;
    uint8  reply[32];

    if (len < 8) { tx_error(cl, 16, X11_REQ_QUERY_TREE, 0); return; }
    x11_id = x11_u32le(req + 4);

    memset(reply, 0, 32);
    x11_put32le(reply + 4,  0);                 /* additional data length */
    x11_put32le(reply + 8,  ROOT_WINDOW_ID);    /* root */
    x11_put32le(reply + 12, (x11_id == ROOT_WINDOW_ID) ? 0 : ROOT_WINDOW_ID); /* parent */
    x11_put16le(reply + 16, 0);                 /* number of children */
    tx_reply(cl, reply);
}

static void handle_intern_atom(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    bool only_if;
    uint16 name_len;
    uint32 atom_id;
    uint8  reply[32];
    char   name_buf[64];
    uint32 copy_len;

    if (len < 8) { tx_error(cl, 16, X11_REQ_INTERN_ATOM, 0); return; }
    only_if  = (req[1] != 0);
    name_len = x11_u16le(req + 4);

    if (name_len == 0 || (uint32)(8 + name_len) > len)
        { tx_error(cl, 16, X11_REQ_INTERN_ATOM, 0); return; }

    copy_len = name_len;
    if (copy_len >= sizeof(name_buf)) copy_len = sizeof(name_buf) - 1;
    memcpy(name_buf, req + 8, copy_len);
    name_buf[copy_len] = '\0';

    atom_id = atom_intern(name_buf, only_if);

    memset(reply, 0, 32);
    x11_put32le(reply + 4,  0);      /* additional data length */
    x11_put32le(reply + 8,  atom_id);
    tx_reply(cl, reply);
}

static void handle_get_atom_name(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    uint32 atom_id;
    const char *name;
    uint16 name_len;
    uint8  reply[32];
    uint32 extra_words;
    uint8  pad[4] = {0,0,0,0};
    uint8  padded_len;

    if (len < 8) { tx_error(cl, 16, X11_REQ_GET_ATOM_NAME, 0); return; }
    atom_id = x11_u32le(req + 4);
    name    = atom_name(atom_id);
    if (!name) { tx_error(cl, 5, X11_REQ_GET_ATOM_NAME, atom_id); return; }

    name_len    = (uint16)strlen(name);
    padded_len  = (uint8)((name_len + 3) & ~3u);
    extra_words = padded_len / 4;

    memset(reply, 0, 32);
    x11_put32le(reply + 4,  extra_words);
    x11_put16le(reply + 8,  name_len);
    tx_reply(cl, reply);
    tx_write(cl, (const uint8 *)name, name_len);
    if ((name_len % 4) != 0)
        tx_write(cl, pad, 4 - (name_len % 4));
}

static void handle_change_property(int cid, const uint8 *req, uint32 len) {
    /* We accept but largely ignore property changes (not a full prop store). */
    uint32 x11_id;
    uint32 prop_atom;
    uint16 name_len;
    char   title_buf[128];
    x11_window_t *w;

    if (len < 24) return;

    x11_id    = x11_u32le(req + 4);
    prop_atom = x11_u32le(req + 8);

    w = find_window(x11_id);
    if (!w) return;

    /* If setting WM_NAME / _NET_WM_NAME, update title */
    if (prop_atom == X11_ATOM_NET_WM_NAME ||
        strcmp(atom_name(prop_atom) ? atom_name(prop_atom) : "", "WM_NAME") == 0) {
        uint32 data_len = x11_u32le(req + 20);
        name_len = (uint16)((data_len < 127) ? data_len : 127);
        if ((uint32)(24 + name_len) <= len) {
            memcpy(title_buf, req + 24, name_len);
            title_buf[name_len] = '\0';
            strncpy(w->title, title_buf, sizeof(w->title) - 1);
            if (w->wm_handle != INVALID_WINDOW_HANDLE)
                window_set_title(w->wm_handle, w->title);
        }
    }
    (void)cid;
}

static void handle_get_property(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    uint8 reply[32];

    (void)req; (void)len;
    /* Return an empty property for everything we don't track. */
    memset(reply, 0, 32);
    x11_put32le(reply + 4, 0);   /* additional data length */
    x11_put32le(reply + 8, 0);   /* type = None */
    x11_put32le(reply + 12, 0);  /* bytes-after */
    x11_put32le(reply + 16, 0);  /* length of value */
    tx_reply(cl, reply);
}

static void handle_set_input_focus(int cid, const uint8 *req, uint32 len) {
    uint32 wid;
    if (len < 12) return;
    wid = x11_u32le(req + 4);
    g_x11.focused_x11_id = wid;
    debuglog(DEBUG_DETAIL, "[X11] SetInputFocus wid=%u\n", wid);
    (void)cid;
}

static void handle_get_input_focus(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    uint8 reply[32];
    (void)req; (void)len;
    memset(reply, 0, 32);
    x11_put32le(reply + 4, 0);
    x11_put32le(reply + 8, g_x11.focused_x11_id);
    x11_put32le(reply + 12, 0); /* revert-to */
    tx_reply(cl, reply);
}

static void handle_create_pixmap(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    x11_pixmap_t *pm;
    uint32 pix_id;
    uint16 pw, ph;
    uint8  depth;
    int i;
    video_mode_t mode;
    graphics_surface_t *surf = NULL;

    if (len < 16) { tx_error(cl, 16, X11_REQ_CREATE_PIXMAP, 0); return; }
    depth  = req[1];
    pix_id = x11_u32le(req + 4);
    /* drawable = req+8 (ignored) */
    pw = x11_u16le(req + 12);
    ph = x11_u16le(req + 14);

    pm = NULL;
    for (i = 0; i < MAX_X11_PIXMAPS; i++) {
        if (!g_x11.pixmaps[i].used) { pm = &g_x11.pixmaps[i]; break; }
    }
    if (!pm) { tx_error(cl, 11, X11_REQ_CREATE_PIXMAP, pix_id); return; }

    if (graphics_get_current_mode(&mode) == GRAPHICS_SUCCESS)
        graphics_create_surface(pw, ph, mode.format, &surf);

    pm->used    = true;
    pm->pix_id  = pix_id;
    pm->width   = pw;
    pm->height  = ph;
    pm->depth   = (depth != 0) ? depth : ROOT_DEPTH;
    pm->surface = surf;

    debuglog(DEBUG_DETAIL, "[X11] CreatePixmap id=%u %dx%d depth=%u\n",
             pix_id, (int)pw, (int)ph, (int)pm->depth);
    (void)cl;
}

static void handle_free_pixmap(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    x11_pixmap_t *pm;
    uint32 pix_id;
    if (len < 8) return;
    pix_id = x11_u32le(req + 4);
    pm = find_pixmap(pix_id);
    if (!pm) { tx_error(cl, 9, X11_REQ_FREE_PIXMAP, pix_id); return; }
    if (pm->surface) graphics_destroy_surface(pm->surface);
    memset(pm, 0, sizeof(*pm));
    (void)cl;
}

static void handle_create_gc(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    x11_gc_t *gc;
    uint32 gc_id, value_mask, voff, bit;
    int i;

    if (len < 16) { tx_error(cl, 16, X11_REQ_CREATE_GC, 0); return; }
    gc_id      = x11_u32le(req + 4);
    /* drawable = req+8 */
    value_mask = x11_u32le(req + 12);
    voff       = 16;

    gc = NULL;
    for (i = 0; i < MAX_X11_GCS; i++) {
        if (!g_x11.gcs[i].used) { gc = &g_x11.gcs[i]; break; }
    }
    if (!gc) { tx_error(cl, 11, X11_REQ_CREATE_GC, gc_id); return; }

    memset(gc, 0, sizeof(*gc));
    gc->used       = true;
    gc->gc_id      = gc_id;
    gc->function   = 3;   /* GXcopy */
    gc->plane_mask = 0xFFFFFFFF;
    gc->foreground = 0x00000000;  /* black */
    gc->background = 0x00FFFFFF;  /* white */
    gc->line_width = 0;

    for (bit = 0; bit < 23; bit++) {
        if (!(value_mask & (1U << bit))) continue;
        if (voff + 4 > len) break;
        uint32 val = x11_u32le(req + voff);
        switch (bit) {
            case 0:  gc->function   = (uint8)val;  break;
            case 1:  gc->plane_mask = val;          break;
            case 2:  gc->foreground = val;          break;
            case 3:  gc->background = val;          break;
            case 4:  gc->line_width = (uint16)val;  break;
            default: break;
        }
        voff += 4;
    }
    debuglog(DEBUG_DETAIL, "[X11] CreateGC id=%u fg=0x%x bg=0x%x\n",
             gc_id, gc->foreground, gc->background);
    (void)cl;
}

static void handle_change_gc(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    x11_gc_t *gc;
    uint32 gc_id, value_mask, voff, bit;

    if (len < 12) return;
    gc_id      = x11_u32le(req + 4);
    value_mask = x11_u32le(req + 8);
    voff       = 12;

    gc = find_gc(gc_id);
    if (!gc) { tx_error(cl, 13, X11_REQ_CHANGE_GC, gc_id); return; }

    for (bit = 0; bit < 23; bit++) {
        if (!(value_mask & (1U << bit))) continue;
        if (voff + 4 > len) break;
        uint32 val = x11_u32le(req + voff);
        switch (bit) {
            case 0:  gc->function   = (uint8)val;  break;
            case 1:  gc->plane_mask = val;          break;
            case 2:  gc->foreground = val;          break;
            case 3:  gc->background = val;          break;
            case 4:  gc->line_width = (uint16)val;  break;
            default: break;
        }
        voff += 4;
    }
    (void)cid;
}

static void handle_free_gc(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    x11_gc_t *gc;
    uint32 gc_id;
    if (len < 8) return;
    gc_id = x11_u32le(req + 4);
    gc = find_gc(gc_id);
    if (!gc) { tx_error(cl, 13, X11_REQ_FREE_GC, gc_id); return; }
    memset(gc, 0, sizeof(*gc));
    (void)cl;
}

/* Resolve a drawable (window or pixmap) to a surface + offset */
static graphics_surface_t *resolve_drawable(uint32 drawable_id,
                                             int32 *off_x, int32 *off_y) {
    x11_window_t *w;
    x11_pixmap_t *pm;

    *off_x = 0; *off_y = 0;

    if (drawable_id == ROOT_WINDOW_ID) {
        /* Draw to the desktop framebuffer – not implemented fully;
           return NULL so callers degrade gracefully */
        return NULL;
    }

    w = find_window(drawable_id);
    if (w && w->wm_handle != INVALID_WINDOW_HANDLE) {
        graphics_surface_t *s = NULL;
        window_get_surface(w->wm_handle, &s);
        return s;
    }

    pm = find_pixmap(drawable_id);
    if (pm) return pm->surface;

    return NULL;
}

static void invalidate_drawable(uint32 drawable_id) {
    x11_window_t *w = find_window(drawable_id);
    if (w && w->wm_handle != INVALID_WINDOW_HANDLE) {
        window_invalidate(w->wm_handle);
        compositor_update();
    }
}

static void handle_clear_area(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    x11_window_t *w;
    uint32 x11_id;
    int16  rx, ry;
    uint16 rw, rh;
    graphics_surface_t *surf;
    video_mode_t mode;
    uint32 bgpix;
    int32 ox, oy;

    (void)cl;
    if (len < 16) return;
    x11_id = x11_u32le(req + 4);
    rx = (int16)x11_u16le(req + 8);
    ry = (int16)x11_u16le(req + 10);
    rw = x11_u16le(req + 12);
    rh = x11_u16le(req + 14);

    w = find_window(x11_id);
    surf = resolve_drawable(x11_id, &ox, &oy);
    if (!surf) return;

    if (rw == 0) rw = w ? w->width  : (uint16)surf->width;
    if (rh == 0) rh = w ? w->height : (uint16)surf->height;

    bgpix = 0;
    if (graphics_get_current_mode(&mode) == GRAPHICS_SUCCESS)
        bgpix = argb_to_native(w ? w->bg_pixel : 0x00C0C0C0, mode.format);

    surf_fill_rect(surf, rx, ry, rw, rh, bgpix);
    invalidate_drawable(x11_id);
}

static void handle_copy_area(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    uint32 src_id, dst_id;
    int16  sx, sy, dx2, dy2;
    uint16 cw, ch;
    graphics_surface_t *ssrc, *sdst;
    int32 sox, soy, dox, doy;

    (void)cl;
    if (len < 28) return;
    src_id = x11_u32le(req + 4);
    dst_id = x11_u32le(req + 8);
    /* gc = req+12 */
    sx  = (int16)x11_u16le(req + 16);
    sy  = (int16)x11_u16le(req + 18);
    dx2 = (int16)x11_u16le(req + 20);
    dy2 = (int16)x11_u16le(req + 22);
    cw  = x11_u16le(req + 24);
    ch  = x11_u16le(req + 26);

    ssrc = resolve_drawable(src_id, &sox, &soy);
    sdst = resolve_drawable(dst_id, &dox, &doy);
    if (!ssrc || !sdst) return;

    surf_blit(sdst, dx2, dy2, ssrc, sx, sy, (int32)cw, (int32)ch);
    invalidate_drawable(dst_id);
}

static void handle_poly_fill_rectangle(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    uint32 drawable_id, gc_id;
    x11_gc_t *gc;
    graphics_surface_t *surf;
    video_mode_t mode;
    uint32 fgpix;
    uint32 off;
    int32  ox, oy;

    (void)cl;
    if (len < 12) return;
    drawable_id = x11_u32le(req + 4);
    gc_id       = x11_u32le(req + 8);
    gc          = find_gc(gc_id);
    surf        = resolve_drawable(drawable_id, &ox, &oy);
    if (!surf) return;

    fgpix = 0;
    if (gc && graphics_get_current_mode(&mode) == GRAPHICS_SUCCESS)
        fgpix = argb_to_native(gc->foreground, mode.format);

    off = 12;
    while (off + 8 <= len) {
        int16  rx = (int16)x11_u16le(req + off);
        int16  ry = (int16)x11_u16le(req + off + 2);
        uint16 rw = x11_u16le(req + off + 4);
        uint16 rh = x11_u16le(req + off + 6);
        surf_fill_rect(surf, rx, ry, (int32)rw, (int32)rh, fgpix);
        off += 8;
    }
    invalidate_drawable(drawable_id);
}

static void handle_put_image(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    uint32 drawable_id;
    uint16 iw, ih;
    int16  dx, dy;
    uint8  depth, format, left_pad;
    graphics_surface_t *surf;
    video_mode_t mode;
    int32  ox, oy;
    uint32 data_off = 24;
    uint32 row;

    (void)cl;
    if (len < 24) return;
    format      = req[1];
    drawable_id = x11_u32le(req + 4);
    /* gc = req+8 */
    iw       = x11_u16le(req + 12);
    ih       = x11_u16le(req + 14);
    dx       = (int16)x11_u16le(req + 16);
    dy       = (int16)x11_u16le(req + 18);
    left_pad = req[20];
    depth    = req[21];

    surf = resolve_drawable(drawable_id, &ox, &oy);
    if (!surf || !surf->pixels) return;

    if (graphics_get_current_mode(&mode) != GRAPHICS_SUCCESS) return;

    if (format == 2 /* ZPixmap */ && depth == ROOT_DEPTH) {
        /* 32bpp BGRA or BGRX row data */
        uint32 src_pitch = (uint32)iw * 4;
        for (row = 0; row < (uint32)ih; row++) {
            uint32 src_off = data_off + row * src_pitch;
            int32  py = dy + (int32)row;
            uint32 col;
            uint32 *drow;
            if (py < 0 || (uint32)py >= surf->height) continue;
            drow = (uint32 *)((uint8 *)surf->pixels + (uint32)py * surf->pitch);
            for (col = 0; col < (uint32)iw; col++) {
                int32 px = dx + (int32)col + (int32)left_pad;
                uint32 soff = src_off + col * 4;
                uint32 raw;
                if (soff + 4 > len) break;
                if (px < 0 || (uint32)px >= surf->width) continue;
                raw = x11_u32le(req + soff);   /* BGRX from client */
                /* Convert: Xlib sends BGRX (little-endian ARGB = BGRA) */
                {
                    uint8 b = (uint8)(raw);
                    uint8 g2 = (uint8)(raw >> 8);
                    uint8 r = (uint8)(raw >> 16);
                    graphics_color_t col_c;
                    col_c.r = r; col_c.g = g2; col_c.b = b; col_c.a = 0xFF;
                    drow[px] = graphics_color_to_pixel(col_c, mode.format);
                }
            }
        }
    }
    /* XYBitmap and XYPixmap formats: fall through (not implemented) */

    invalidate_drawable(drawable_id);
    (void)left_pad; (void)depth;
}

static void handle_get_image(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    uint32 drawable_id;
    int16  gx, gy;
    uint16 gw, gh;
    graphics_surface_t *surf;
    int32  ox, oy;
    uint32 data_words, row;
    uint8  reply[32];
    video_mode_t mode;

    if (len < 20) { tx_error(cl, 16, X11_REQ_GET_IMAGE, 0); return; }
    drawable_id = x11_u32le(req + 4);
    gx = (int16)x11_u16le(req + 8);
    gy = (int16)x11_u16le(req + 10);
    gw = x11_u16le(req + 12);
    gh = x11_u16le(req + 14);
    /* plane-mask = req+16 */

    surf = resolve_drawable(drawable_id, &ox, &oy);
    if (!surf || !surf->pixels)
        { tx_error(cl, 3, X11_REQ_GET_IMAGE, drawable_id); return; }
    if (graphics_get_current_mode(&mode) != GRAPHICS_SUCCESS)
        { tx_error(cl, 3, X11_REQ_GET_IMAGE, drawable_id); return; }

    data_words = ((uint32)gw * (uint32)gh);  /* 32bpp → 1 word per pixel */

    memset(reply, 0, 32);
    reply[1] = ROOT_DEPTH;
    x11_put32le(reply + 4,  data_words);
    x11_put32le(reply + 8,  0x00FFFFFF);  /* visual */
    tx_reply(cl, reply);

    /* Stream pixels */
    for (row = 0; row < (uint32)gh; row++) {
        uint32 col;
        uint32 *srow;
        int32 py = gy + (int32)row;
        if (py < 0 || (uint32)py >= surf->height) {
            /* Fill with zeros for out-of-bounds rows */
            uint32 z = 0;
            for (col = 0; col < (uint32)gw; col++)
                tx_write(cl, (const uint8 *)&z, 4);
            continue;
        }
        srow = (uint32 *)((uint8 *)surf->pixels + (uint32)py * surf->pitch);
        for (col = 0; col < (uint32)gw; col++) {
            int32 px = gx + (int32)col;
            uint32 pix = 0;
            uint8 pbuf[4];
            if (px >= 0 && (uint32)px < surf->width)
                pix = srow[px];
            /* Convert native pixel back to BGRA */
            {
                graphics_color_t c = graphics_pixel_to_color(pix, mode.format);
                pbuf[0] = c.b; pbuf[1] = c.g; pbuf[2] = c.r; pbuf[3] = 0;
            }
            tx_write(cl, pbuf, 4);
        }
    }
}

static void handle_image_text8(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    uint8  nchars;
    uint32 drawable_id, gc_id;
    int16  tx2, ty;
    x11_gc_t *gc;
    graphics_surface_t *surf;
    video_mode_t mode;
    uint32 fgpix, bgpix;
    uint32 i;
    int32  ox, oy;

    (void)cl;
    if (len < 16) return;
    nchars      = req[1];
    drawable_id = x11_u32le(req + 4);
    gc_id       = x11_u32le(req + 8);
    tx2 = (int16)x11_u16le(req + 12);
    ty  = (int16)x11_u16le(req + 14);

    gc   = find_gc(gc_id);
    surf = resolve_drawable(drawable_id, &ox, &oy);
    if (!surf || !gc) return;

    fgpix = 0; bgpix = 0xFFFFFFFF;
    if (graphics_get_current_mode(&mode) == GRAPHICS_SUCCESS) {
        fgpix = argb_to_native(gc->foreground, mode.format);
        bgpix = argb_to_native(gc->background, mode.format);
    }

    /* Text in X11: ty is baseline.  Our font is 8px tall, baseline at row 7. */
    int32 cy = (int32)ty - 7;

    for (i = 0; i < (uint32)nchars; i++) {
        uint32 char_off = 16 + i;
        if (char_off >= len) break;
        surf_draw_char(surf, tx2 + (int32)(i * 8), cy,
                       req[char_off], fgpix, bgpix, false);
    }
    invalidate_drawable(drawable_id);
}

static void handle_poly_text8(int cid, const uint8 *req, uint32 len) {
    /*
     * PolyText8: sequence of text items.
     * Each item: 1-byte count, 1-byte delta, then 'count' chars.
     * Count == 255 means a font-change item (4 bytes); we skip it.
     */
    x11_client_t *cl = &g_x11.clients[cid];
    uint32 drawable_id, gc_id;
    int16  cx, cy;
    x11_gc_t *gc;
    graphics_surface_t *surf;
    video_mode_t mode;
    uint32 fgpix, bgpix;
    uint32 off;
    int32  ox, oy;

    (void)cl;
    if (len < 16) return;
    drawable_id = x11_u32le(req + 4);
    gc_id       = x11_u32le(req + 8);
    cx  = (int16)x11_u16le(req + 12);
    cy  = (int16)x11_u16le(req + 14);

    gc   = find_gc(gc_id);
    surf = resolve_drawable(drawable_id, &ox, &oy);
    if (!surf || !gc) return;

    fgpix = 0; bgpix = 0xFFFFFFFF;
    if (graphics_get_current_mode(&mode) == GRAPHICS_SUCCESS) {
        fgpix = argb_to_native(gc->foreground, mode.format);
        bgpix = argb_to_native(gc->background, mode.format);
    }

    int32 draw_y = (int32)cy - 7;

    off = 16;
    while (off + 2 <= len) {
        uint8 count = req[off];
        int8  delta = (int8)req[off + 1];
        off += 2;

        if (count == 255) { off += 4; continue; }  /* font change */
        if (count == 0)   break;

        cx = (int16)((int32)cx + (int32)delta);

        uint32 i;
        for (i = 0; i < (uint32)count; i++) {
            if (off >= len) break;
            surf_draw_char(surf, (int32)cx + (int32)(i * 8), draw_y,
                           req[off], fgpix, bgpix, true);
            off++;
        }
        cx = (int16)((int32)cx + (int32)count * 8);
    }
    invalidate_drawable(drawable_id);
}

static void handle_query_extension(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    uint8 reply[32];
    /* We claim to support no extensions for now */
    (void)req; (void)len;
    memset(reply, 0, 32);
    x11_put32le(reply + 4, 0);
    /* present=0, major-opcode=0, first-event=0, first-error=0 */
    tx_reply(cl, reply);
}

static void handle_list_extensions(int cid, const uint8 *req, uint32 len) {
    x11_client_t *cl = &g_x11.clients[cid];
    uint8 reply[32];
    (void)req; (void)len;
    memset(reply, 0, 32);
    x11_put32le(reply + 4, 0);  /* no additional data */
    reply[1] = 0;               /* num extensions = 0 */
    tx_reply(cl, reply);
}

/* =========================================================
 *  Request dispatcher
 * ========================================================= */
static void dispatch_request(int cid, const uint8 *req, uint32 req_bytes) {
    x11_client_t *cl = &g_x11.clients[cid];
    uint8 opcode = req[0];

    cl->next_seq++;

    switch (opcode) {
        case X11_REQ_CREATE_WINDOW:       handle_create_window(cid, req, req_bytes); break;
        case X11_REQ_CHANGE_WINDOW_ATTR:  /* silently accept */ break;
        case X11_REQ_GET_WINDOW_ATTR:
        {
            /* Minimal GetWindowAttributes reply */
            uint8 reply[44];
            memset(reply, 0, 44);
            reply[1] = 2;  /* backing-store: WhenMapped */
            x11_put32le(reply + 4, (44-32)/4);  /* additional data words */
            x11_put32le(reply + 8, ROOT_VISUAL_ID);
            reply[12] = 1;  /* class: InputOutput */
            reply[13] = 1;  /* bit-gravity: NorthWest */
            reply[14] = 1;  /* win-gravity: NorthWest */
            x11_put32le(reply + 24, X11_MASK_EXPOSURE); /* your-event-mask */
            reply[30] = 1;  /* map-state: Viewable */
            tx_reply(cl, reply);
            tx_write(cl, reply + 32, 12);  /* extra 12 bytes */
            break;
        }
        case X11_REQ_DESTROY_WINDOW:      handle_destroy_window(cid, req, req_bytes); break;
        case X11_REQ_MAP_WINDOW:          handle_map_window(cid, req, req_bytes); break;
        case X11_REQ_UNMAP_WINDOW:        handle_unmap_window(cid, req, req_bytes); break;
        case X11_REQ_CONFIGURE_WINDOW:    handle_configure_window(cid, req, req_bytes); break;
        case X11_REQ_GET_GEOMETRY:        handle_get_geometry(cid, req, req_bytes); break;
        case X11_REQ_QUERY_TREE:          handle_query_tree(cid, req, req_bytes); break;
        case X11_REQ_INTERN_ATOM:         handle_intern_atom(cid, req, req_bytes); break;
        case X11_REQ_GET_ATOM_NAME:       handle_get_atom_name(cid, req, req_bytes); break;
        case X11_REQ_CHANGE_PROPERTY:     handle_change_property(cid, req, req_bytes); break;
        case X11_REQ_GET_PROPERTY:        handle_get_property(cid, req, req_bytes); break;
        case X11_REQ_SET_INPUT_FOCUS:     handle_set_input_focus(cid, req, req_bytes); break;
        case X11_REQ_GET_INPUT_FOCUS:     handle_get_input_focus(cid, req, req_bytes); break;
        case X11_REQ_CREATE_PIXMAP:       handle_create_pixmap(cid, req, req_bytes); break;
        case X11_REQ_FREE_PIXMAP:         handle_free_pixmap(cid, req, req_bytes); break;
        case X11_REQ_CREATE_GC:           handle_create_gc(cid, req, req_bytes); break;
        case X11_REQ_CHANGE_GC:           handle_change_gc(cid, req, req_bytes); break;
        case X11_REQ_FREE_GC:             handle_free_gc(cid, req, req_bytes); break;
        case X11_REQ_CLEAR_AREA:          handle_clear_area(cid, req, req_bytes); break;
        case X11_REQ_COPY_AREA:           handle_copy_area(cid, req, req_bytes); break;
        case X11_REQ_POLY_FILL_RECTANGLE: handle_poly_fill_rectangle(cid, req, req_bytes); break;
        case X11_REQ_PUT_IMAGE:           handle_put_image(cid, req, req_bytes); break;
        case X11_REQ_GET_IMAGE:           handle_get_image(cid, req, req_bytes); break;
        case X11_REQ_POLY_TEXT8:          handle_poly_text8(cid, req, req_bytes); break;
        case X11_REQ_IMAGE_TEXT8:         handle_image_text8(cid, req, req_bytes); break;
        case X11_REQ_QUERY_EXTENSION:     handle_query_extension(cid, req, req_bytes); break;
        case X11_REQ_LIST_EXTENSIONS:     handle_list_extensions(cid, req, req_bytes); break;
        default:
            debuglog(DEBUG_DETAIL, "[X11] Unknown opcode %u len=%u\n", opcode, req_bytes);
            break;
    }
}

/* =========================================================
 *  Per-client pump: consume from RX ring, produce to TX ring
 * ========================================================= */
#define SETUP_HEADER_LEN  12   /* client sends 12 bytes of connection setup */
#define REQ_TMP_BUF       512  /* max request we buffer (extend if needed) */

static void pump_client(int cid) {
    x11_client_t *cl = &g_x11.clients[cid];

    if (cl->state == X11_CLIENT_STATE_FREE) return;

    /* --- Handshake phase --- */
    if (cl->state == X11_CLIENT_STATE_HANDSHAKE) {
        uint8 hdr[12];
        if (ipc_ring_used(&cl->rx) < SETUP_HEADER_LEN) return;
        ipc_ring_read(&cl->rx, hdr, SETUP_HEADER_LEN);

        /* hdr[0]: byte order 'B' or 'l' */
        cl->byte_order_msb = (hdr[0] == 'B');

        uint16 proto_major = x11_u16le(hdr + 2);
        uint16 proto_minor = x11_u16le(hdr + 4);
        uint16 auth_proto_len = x11_u16le(hdr + 6);
        uint16 auth_data_len  = x11_u16le(hdr + 8);
        uint32 skip = (uint32)((auth_proto_len + 3) & ~3u) +
                      (uint32)((auth_data_len  + 3) & ~3u);

        (void)proto_major; (void)proto_minor;

        /* Drain auth data */
        if (ipc_ring_used(&cl->rx) < skip) return;
        ipc_ring_drop(&cl->rx, skip);

        cl->state    = X11_CLIENT_STATE_READY;
        cl->next_seq = 0;
        send_setup_success(cl);
        debuglog(DEBUG_INFO, "[X11] Client %d connected (msb=%d)\n",
                 cid, (int)cl->byte_order_msb);
        return;
    }

    /* --- Normal request processing --- */
    if (cl->state == X11_CLIENT_STATE_READY) {
        uint8 tmp[REQ_TMP_BUF];

        for (;;) {
            uint8 hdr4[4];
            uint32 req_bytes;

            /* Need at least 4 bytes to know the size */
            if (!ipc_ring_peek(&cl->rx, hdr4, 4)) break;

            uint16 length_units = x11_u16le(hdr4 + 2);
            if (length_units == 0) {
                /* BigRequests encoding: next uint32 is real length */
                uint8 hdr8[8];
                if (!ipc_ring_peek(&cl->rx, hdr8, 8)) break;
                uint32 big_len = x11_u32le(hdr8 + 4);
                req_bytes = big_len * 4;
            } else {
                req_bytes = (uint32)length_units * 4u;
            }

            if (req_bytes == 0 || req_bytes > ipc_ring_used(&cl->rx)) break;

            /* Cap at our tmp buffer; oversized requests get truncated */
            uint32 copy_len = (req_bytes <= REQ_TMP_BUF) ? req_bytes : REQ_TMP_BUF;
            ipc_ring_read(&cl->rx, tmp, req_bytes < REQ_TMP_BUF ? req_bytes : req_bytes);
            /* If request > buf, we already read it but only copied part.
               Use what we have and drop the rest (already consumed). */
            dispatch_request(cid, tmp, copy_len);
        }

        /* Deliver pending events */
        flush_events_for_client(cid);
    }
}

/* =========================================================
 *  Input event dispatch
 * ========================================================= */

/* Simple PS/2 scancode → X11 keycode mapping (set-1 make codes) */
static uint8 scancode_to_x11_keycode(uint8 sc) {
    /* Offset by +8 per X11 convention (min-keycode=8) */
    if (sc < 0x59) return sc + 8;
    return 0;
}

void x11_input_event_callback(const input_event_t *event, void *ctx) {
    int i;
    uint8 ev[32];
    uint32 time_ms;

    (void)ctx;
    if (!g_x11.initialized) return;

    time_ms = event->tv_sec * 1000 + event->tv_usec / 1000;

    if (event->type == EV_KEY) {
        uint8 keycode = scancode_to_x11_keycode((uint8)event->code);
        bool is_mouse_btn = (event->code >= BTN_LEFT && event->code <= BTN_MIDDLE);

        if (is_mouse_btn) {
            uint8 x11_btn = (uint8)(event->code - BTN_LEFT + 1);
            uint8 ev_code = (event->value) ? X11_EVENT_BUTTON_PRESS
                                           : X11_EVENT_BUTTON_RELEASE;
            /* Deliver to window under pointer */
            for (i = 0; i < MAX_X11_WINDOWS; i++) {
                x11_window_t *w = &g_x11.windows[i];
                if (!w->used || !w->mapped) continue;
                uint32 mask = event->value ? X11_MASK_BUTTON_PRESS
                                           : X11_MASK_BUTTON_RELEASE;
                if (!(w->event_mask & mask)) continue;
                if (g_x11.ptr_x < w->x || g_x11.ptr_x >= w->x + (int16)w->width) continue;
                if (g_x11.ptr_y < w->y || g_x11.ptr_y >= w->y + (int16)w->height) continue;
                build_button_event(ev, ev_code, x11_btn, 0, w->x11_id,
                                   g_x11.ptr_x, g_x11.ptr_y, time_ms);
                window_queue_event(w, ev);
            }
        } else if (keycode != 0) {
            /* Keyboard event → focused window */
            x11_window_t *fw = find_window(g_x11.focused_x11_id);
            if (!fw) {
                /* Fall back to first mapped window */
                for (i = 0; i < MAX_X11_WINDOWS; i++) {
                    if (g_x11.windows[i].used && g_x11.windows[i].mapped) {
                        fw = &g_x11.windows[i];
                        break;
                    }
                }
            }
            if (fw) {
                uint8 ev_code = event->value ? X11_EVENT_KEY_PRESS
                                             : X11_EVENT_KEY_RELEASE;
                uint32 mask = event->value ? X11_MASK_KEY_PRESS
                                           : X11_MASK_KEY_RELEASE;
                if (fw->event_mask & mask) {
                    build_key_event(ev, ev_code, keycode, 0, fw->x11_id,
                                    ROOT_WINDOW_ID,
                                    g_x11.ptr_x, g_x11.ptr_y, time_ms);
                    window_queue_event(fw, ev);
                }
            }
        }
    } else if (event->type == EV_REL) {
        if      (event->code == REL_X) g_x11.ptr_x = (int16)(g_x11.ptr_x + event->value);
        else if (event->code == REL_Y) g_x11.ptr_y = (int16)(g_x11.ptr_y + event->value);

        /* Clamp */
        if (g_x11.ptr_x < 0)              g_x11.ptr_x = 0;
        if (g_x11.ptr_x >= SCREEN_WIDTH)   g_x11.ptr_x = SCREEN_WIDTH  - 1;
        if (g_x11.ptr_y < 0)              g_x11.ptr_y = 0;
        if (g_x11.ptr_y >= SCREEN_HEIGHT)  g_x11.ptr_y = SCREEN_HEIGHT - 1;

        /* MotionNotify to windows under pointer */
        for (i = 0; i < MAX_X11_WINDOWS; i++) {
            x11_window_t *w = &g_x11.windows[i];
            if (!w->used || !w->mapped) continue;
            if (!(w->event_mask & X11_MASK_POINTER_MOTION)) continue;
            if (g_x11.ptr_x < w->x || g_x11.ptr_x >= w->x + (int16)w->width) continue;
            if (g_x11.ptr_y < w->y || g_x11.ptr_y >= w->y + (int16)w->height) continue;
            build_motion_event(ev, w->x11_id, g_x11.ptr_x, g_x11.ptr_y, time_ms);
            window_queue_event(w, ev);
        }
    }
}

/* =========================================================
 *  Public API
 * ========================================================= */

void x11_server_init(void) {
    memset(&g_x11, 0, sizeof(g_x11));
    spinlock_init(&g_x11.lock, "x11_server");
    g_x11.initialized = true;
    g_x11.focused_x11_id = 0;
    atoms_init();

    /* Register with input multiplexer */
    if (input_mux_is_initialized()) {
        input_ring_init(&g_x11.input_ring, "x11_input");
        input_consumer_init(&g_x11.input_consumer, "x11_server",
                            INPUT_PRIORITY_FOCUSED,
                            INPUT_MASK_ALL_INPUT);
        g_x11.input_consumer.event_callback  = x11_input_event_callback;
        g_x11.input_consumer.ring            = &g_x11.input_ring;
        g_x11.input_consumer.has_keyboard_focus = true;
        g_x11.input_consumer.has_mouse_focus    = true;
        input_mux_register_consumer(&g_x11.input_consumer);
        debuglog(DEBUG_INFO, "[X11] Registered input consumer\n");
    }

    debuglog(DEBUG_INFO, "[X11] Server initialized\n");
}

void x11_server_shutdown(void) {
    int i;
    spinlock_acquire(&g_x11.lock);

    if (input_mux_is_initialized())
        input_mux_unregister_consumer(&g_x11.input_consumer);

    for (i = 0; i < MAX_X11_PIXMAPS; i++) {
        if (g_x11.pixmaps[i].used && g_x11.pixmaps[i].surface)
            graphics_destroy_surface(g_x11.pixmaps[i].surface);
    }
    for (i = 0; i < MAX_X11_WINDOWS; i++) {
        x11_window_t *w = &g_x11.windows[i];
        if (w->used && w->wm_handle != INVALID_WINDOW_HANDLE)
            window_destroy(w->wm_handle);
    }

    memset(&g_x11.clients,  0, sizeof(g_x11.clients));
    memset(&g_x11.windows,  0, sizeof(g_x11.windows));
    memset(&g_x11.gcs,      0, sizeof(g_x11.gcs));
    memset(&g_x11.pixmaps,  0, sizeof(g_x11.pixmaps));
    g_x11.initialized = false;

    spinlock_release(&g_x11.lock);
    debuglog(DEBUG_INFO, "[X11] Server shutdown\n");
}

int x11_client_connect(void) {
    int i;
    spinlock_acquire(&g_x11.lock);
    for (i = 0; i < MAX_X11_CLIENTS; i++) {
        if (g_x11.clients[i].state == X11_CLIENT_STATE_FREE) {
            memset(&g_x11.clients[i], 0, sizeof(g_x11.clients[i]));
            g_x11.clients[i].state = X11_CLIENT_STATE_HANDSHAKE;
            spinlock_release(&g_x11.lock);
            debuglog(DEBUG_INFO, "[X11] Client %d connected\n", i);
            return i;
        }
    }
    spinlock_release(&g_x11.lock);
    return -1;
}

void x11_client_disconnect(int client_id) {
    int i;
    if (client_id < 0 || client_id >= MAX_X11_CLIENTS) return;
    spinlock_acquire(&g_x11.lock);

    /* Destroy all windows belonging to this client */
    for (i = 0; i < MAX_X11_WINDOWS; i++) {
        x11_window_t *w = &g_x11.windows[i];
        if (!w->used || w->owner_client != client_id) continue;
        if (w->wm_handle != INVALID_WINDOW_HANDLE)
            window_destroy(w->wm_handle);
        if (g_x11.focused_x11_id == w->x11_id)
            g_x11.focused_x11_id = 0;
        memset(w, 0, sizeof(*w));
    }

    memset(&g_x11.clients[client_id], 0, sizeof(g_x11.clients[client_id]));
    spinlock_release(&g_x11.lock);
    debuglog(DEBUG_INFO, "[X11] Client %d disconnected\n", client_id);
}

int x11_client_write(int client_id, const uint8 *data, uint32 len) {
    uint32 written;
    if (client_id < 0 || client_id >= MAX_X11_CLIENTS) return -1;
    if (g_x11.clients[client_id].state == X11_CLIENT_STATE_FREE) return -1;
    written = ipc_ring_write(&g_x11.clients[client_id].rx, data, len);
    return (written == len) ? 0 : -1;
}

uint32 x11_client_read(int client_id, uint8 *buf, uint32 max_len) {
    if (client_id < 0 || client_id >= MAX_X11_CLIENTS) return 0;
    if (g_x11.clients[client_id].state == X11_CLIENT_STATE_FREE) return 0;
    return ipc_ring_read(&g_x11.clients[client_id].tx, buf, max_len);
}

void x11_server_pump(uint32 max_events) {
    int i;
    (void)max_events;
    if (!g_x11.initialized) return;

    /* Drain input ring from the mux */
    if (input_mux_is_initialized()) {
        input_event_t ev;
        while (input_ring_pop(&g_x11.input_ring, &ev))
            x11_input_event_callback(&ev, NULL);
    }

    for (i = 0; i < MAX_X11_CLIENTS; i++)
        pump_client(i);
}

void x11_server_loop(void) {
    debuglog(DEBUG_INFO, "[X11] Server loop started\n");
    for (;;) {
        x11_server_pump(64);
        /* In a real implementation this would yield / sleep */
    }
}

/* =========================================================
 *  Legacy direct-call API (kept for backward compatibility)
 * ========================================================= */
int x11_handle_connection(int client_fd) {
    /* Map client_fd to a client slot by its numeric value */
    if (client_fd < 0 || client_fd >= MAX_X11_CLIENTS) return -1;
    if (g_x11.clients[client_fd].state != X11_CLIENT_STATE_FREE) return 0;
    memset(&g_x11.clients[client_fd], 0, sizeof(g_x11.clients[client_fd]));
    g_x11.clients[client_fd].state = X11_CLIENT_STATE_HANDSHAKE;
    return 0;
}

int x11_handle_request(int client_fd, const uint8 *request, uint32 length) {
    if (client_fd < 0 || client_fd >= MAX_X11_CLIENTS) return -1;
    if (!request || length < 4) return -1;
    ipc_ring_write(&g_x11.clients[client_fd].rx, request, length);
    pump_client(client_fd);
    return 0;
}

/* =========================================================
 *  Window API (used internally and by shell / WM code)
 * ========================================================= */
uint32 x11_create_window(uint32 parent, int16 x, int16 y,
                         uint16 width, uint16 height, uint32 event_mask) {
    int i;
    for (i = 0; i < MAX_X11_WINDOWS; i++) {
        if (!g_x11.windows[i].used) {
            x11_window_t *w = &g_x11.windows[i];
            static uint32 next_id = 0x100;
            memset(w, 0, sizeof(*w));
            w->used         = true;
            w->x11_id       = next_id++;
            w->parent_x11   = parent;
            w->x            = x;
            w->y            = y;
            w->width        = (width  > 0) ? width  : 100;
            w->height       = (height > 0) ? height : 100;
            w->event_mask   = event_mask;
            w->bg_pixel     = 0x00C0C0C0;
            w->wm_handle    = INVALID_WINDOW_HANDLE;
            w->owner_client = -1;
            return w->x11_id;
        }
    }
    return 0;
}

int x11_map_window(uint32 window_id) {
    x11_window_t *w = find_window(window_id);
    if (!w) return -1;
    w->mapped = true;
    if (w->wm_handle == INVALID_WINDOW_HANDLE && window_manager_is_initialized()) {
        w->wm_handle = window_create(w->x, w->y, w->width, w->height,
                                     (w->title[0] ? w->title : "X11"),
                                     WINDOW_FLAGS_DEFAULT);
    }
    compositor_update();
    return 0;
}

int x11_unmap_window(uint32 window_id) {
    x11_window_t *w = find_window(window_id);
    if (!w) return -1;
    w->mapped = false;
    if (w->wm_handle != INVALID_WINDOW_HANDLE) window_hide(w->wm_handle);
    compositor_update();
    return 0;
}

int x11_destroy_window(uint32 window_id) {
    x11_window_t *w = find_window(window_id);
    if (!w) return -1;
    if (w->wm_handle != INVALID_WINDOW_HANDLE) window_destroy(w->wm_handle);
    if (g_x11.focused_x11_id == window_id) g_x11.focused_x11_id = 0;
    memset(w, 0, sizeof(*w));
    return 0;
}

int x11_configure_window(uint32 window_id, int16 x, int16 y,
                         uint16 width, uint16 height) {
    x11_window_t *w = find_window(window_id);
    if (!w) return -1;
    if (x >= 0) w->x = x;
    if (y >= 0) w->y = y;
    if (width  > 0) w->width  = width;
    if (height > 0) w->height = height;
    if (w->wm_handle != INVALID_WINDOW_HANDLE)
        window_set_position(w->wm_handle, w->x, w->y);
    return 0;
}

int x11_get_window_info(uint32 window_id, int16 *x, int16 *y,
                        uint16 *width, uint16 *height, bool *mapped) {
    x11_window_t *w = find_window(window_id);
    if (!w) return -1;
    if (x)      *x      = w->x;
    if (y)      *y      = w->y;
    if (width)  *width  = w->width;
    if (height) *height = w->height;
    if (mapped) *mapped = w->mapped;
    return 0;
}

void x11_send_expose(uint32 window_id) {
    x11_window_t *w = find_window(window_id);
    uint8 ev[32];
    if (!w || !w->mapped) return;
    if (!(w->event_mask & X11_MASK_EXPOSURE)) return;
    build_expose_event(ev, window_id, 0, 0, w->width, w->height, 0);
    window_queue_event(w, ev);
}

void x11_dispatch_keyboard(uint8 keycode, bool pressed) {
    input_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = EV_KEY;
    ev.code  = keycode;
    ev.value = pressed ? KEY_PRESS : KEY_RELEASE;
    x11_input_event_callback(&ev, NULL);
}

void x11_dispatch_pointer(int16 dx, int16 dy, uint8 buttons) {
    input_event_t evx, evy;
    (void)buttons;
    memset(&evx, 0, sizeof(evx));
    evx.type = EV_REL; evx.code = REL_X; evx.value = dx;
    x11_input_event_callback(&evx, NULL);
    memset(&evy, 0, sizeof(evy));
    evy.type = EV_REL; evy.code = REL_Y; evy.value = dy;
    x11_input_event_callback(&evy, NULL);
}

#else /* !HAS_GRAPHICS */

/* No-framebuffer X11 server stubs. The X11 server requires a framebuffer to
 * render into; without one it accepts no connections and does nothing. */

void x11_server_init(void)     {}
void x11_server_shutdown(void) {}
void x11_server_loop(void)     {}
void x11_server_pump(uint32 max_events) { (void)max_events; }

int  x11_client_connect(void)                       { return -1; }
void x11_client_disconnect(int client_id)           { (void)client_id; }
int  x11_client_write(int client_id, const uint8 *data, uint32 len) { (void)client_id; (void)data; (void)len; return -1; }
uint32 x11_client_read(int client_id, uint8 *buf, uint32 max_len)  { (void)client_id; (void)buf; (void)max_len; return 0; }

int  x11_handle_connection(int client_fd)                                { (void)client_fd; return -1; }
int  x11_handle_request(int client_fd, const uint8 *request, uint32 len) { (void)client_fd; (void)request; (void)len; return -1; }

uint32 x11_create_window(uint32 parent, int16 x, int16 y, uint16 width, uint16 height, uint32 event_mask) {
    (void)parent; (void)x; (void)y; (void)width; (void)height; (void)event_mask;
    return 0;
}
int  x11_map_window(uint32 window_id)                 { (void)window_id; return -1; }
int  x11_unmap_window(uint32 window_id)               { (void)window_id; return -1; }
int  x11_destroy_window(uint32 window_id)             { (void)window_id; return -1; }
int  x11_configure_window(uint32 window_id, int16 x, int16 y, uint16 width, uint16 height) {
    (void)window_id; (void)x; (void)y; (void)width; (void)height; return -1;
}
int  x11_get_window_info(uint32 window_id, int16 *x, int16 *y, uint16 *width, uint16 *height, bool *mapped) {
    (void)window_id; if (x) *x = 0; if (y) *y = 0; if (width) *width = 0; if (height) *height = 0; if (mapped) *mapped = false;
    return -1;
}
void x11_send_expose(uint32 window_id)                { (void)window_id; }
void x11_dispatch_keyboard(uint8 keycode, bool pressed) { (void)keycode; (void)pressed; }
void x11_dispatch_pointer(int16 dx, int16 dy, uint8 buttons) { (void)dx; (void)dy; (void)buttons; }
void x11_input_event_callback(const input_event_t *event, void *ctx) { (void)event; (void)ctx; }

#endif /* HAS_GRAPHICS */
