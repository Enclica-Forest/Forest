#include "sys.h"
#include "mem.h"
#include "x11_atoms.h"
#include "x11_protocol.h"
#include "x11_socket.h"
#include "x11_window.h"
#include "x11_gc.h"
#include "x11_pixmap.h"
#include "x11_events.h"
#include "x11_draw.h"
#include "x11_property.h"
#include "x11_font.h"
#include "x11_color.h"
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <forestos/syscalls.h>

#define REQ_CREATE_WINDOW        1
#define REQ_CHANGE_WINDOW_ATTR   2
#define REQ_GET_WINDOW_ATTR      3
#define REQ_DESTROY_WINDOW       4
#define REQ_MAP_WINDOW           8
#define REQ_MAP_SUBWINDOWS       9
#define REQ_UNMAP_WINDOW         10
#define REQ_CONFIGURE_WINDOW     12
#define REQ_GET_GEOMETRY         14
#define REQ_QUERY_TREE           15
#define REQ_INTERN_ATOM          16
#define REQ_GET_ATOM_NAME        17
#define REQ_CHANGE_PROPERTY      18
#define REQ_DELETE_PROPERTY      19
#define REQ_GET_PROPERTY         20
#define REQ_LIST_PROPERTIES      21
#define REQ_SET_INPUT_FOCUS      42
#define REQ_GET_INPUT_FOCUS      43
#define REQ_OPEN_FONT            45
#define REQ_CLOSE_FONT           46
#define REQ_QUERY_FONT           47
#define REQ_LIST_FONTS           48
#define REQ_LIST_FONTS_WITH_INFO 49
#define REQ_CREATE_GC            55
#define REQ_CHANGE_GC            56
#define REQ_FREE_GC              60
#define REQ_CLEAR_AREA           61
#define REQ_COPY_AREA            62
#define REQ_COPY_PLANE           63
#define REQ_POLY_POINT           64
#define REQ_POLY_LINE            65
#define REQ_POLY_SEGMENT         66
#define REQ_POLY_ARC             67
#define REQ_FILL_POLY            69
#define REQ_POLY_FILL_RECTANGLE  70
#define REQ_POLY_FILL_ARC        71
#define REQ_PUT_IMAGE            72
#define REQ_GET_IMAGE            73
#define REQ_POLY_TEXT8           74
#define REQ_IMAGE_TEXT8          76
#define REQ_CREATE_PIXMAP        53
#define REQ_FREE_PIXMAP          54
#define REQ_ALLOC_COLOR          84
#define REQ_ALLOC_NAMED_COLOR    85
#define REQ_ALLOC_COLORS         86
#define REQ_FREE_COLORS          88
#define REQ_QUERY_COLORS         89
#define REQ_LOOKUP_COLOR         90
#define REQ_SET_SELECTION_OWNER  92
#define REQ_GET_SELECTION_OWNER  93
#define REQ_CONVERT_SELECTION    94
#define REQ_NO_OPERATION         127

#define FB_FORMAT_BGRA_8888  7
#define FB_FORMAT_RGBA_8888  5

static x11_fb_t g_fb;
static uint8_t* g_backbuf = NULL;
static int g_running = 1;

static void composite_all(void) {
    if (!g_backbuf) return;
    uint32_t bg = 0xFF1A1A2E;
    uint32_t* pixels = (uint32_t*)g_backbuf;
    uint32_t npix = g_fb.width * g_fb.height;
    for (uint32_t i = 0; i < npix; i++) pixels[i] = bg;

    int sorted[X11_MAX_WINDOWS];
    int sc = 0;
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (g_x11_windows[i].used && g_x11_windows[i].mapped && g_x11_windows[i].surface)
            sorted[sc++] = i;
    }
    for (int a = 0; a < sc - 1; a++)
        for (int b = a + 1; b < sc; b++)
            if (g_x11_windows[sorted[a]].z_order > g_x11_windows[sorted[b]].z_order) {
                int t = sorted[a]; sorted[a] = sorted[b]; sorted[b] = t;
            }
    for (int s = 0; s < sc; s++) {
        x11_window_t* w = &g_x11_windows[sorted[s]];
        int sx = w->x, sy = w->y;
        uint16_t sw = w->width, sh = w->height;
        for (uint32_t wy = 0; wy < sh; wy++) {
            int dy = sy + (int)wy;
            if (dy < 0 || dy >= (int)g_fb.height) continue;
            for (uint32_t wx = 0; wx < sw; wx++) {
                int dx = sx + (int)wx;
                if (dx < 0 || dx >= (int)g_fb.width) continue;
                uint32_t c = ((uint32_t*)w->surface)[wy * sw + wx];
                pixels[dy * g_fb.width + dx] = c;
            }
        }
    }

    for (uint32_t y = 0; y < g_fb.height; y++) {
        uint32_t* src_row = pixels + y * g_fb.width;
        uint8_t* dst_row = g_backbuf + y * g_fb.pitch;
        for (uint32_t x = 0; x < g_fb.width; x++) {
            uint32_t c = src_row[x];
            uint8_t cb = c & 0xFF;
            uint8_t cg = (c >> 8) & 0xFF;
            uint8_t cr = (c >> 16) & 0xFF;
            uint8_t ca = (c >> 24) & 0xFF;
            if (g_fb.format == FB_FORMAT_BGRA_8888 || g_fb.format == 7) {
                dst_row[x * 4 + 0] = cb;
                dst_row[x * 4 + 1] = cg;
                dst_row[x * 4 + 2] = cr;
                dst_row[x * 4 + 3] = ca;
            } else {
                dst_row[x * 4 + 0] = cr;
                dst_row[x * 4 + 1] = cg;
                dst_row[x * 4 + 2] = cb;
                dst_row[x * 4 + 3] = ca;
            }
        }
    }
}

static void flip_to_fb(void) {
    if (!g_backbuf || !g_fb.addr) return;
    uint8_t* dst = (uint8_t*)g_fb.addr;
    for (uint32_t y = 0; y < g_fb.height; y++) {
        memcpy(dst + y * g_fb.pitch, g_backbuf + y * g_fb.pitch, g_fb.width * 4);
    }
}

static void handle_create_window(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 24) return;
    uint32_t id = x11_u32le(body + 0);
    uint32_t parent = x11_u32le(body + 4);
    int16_t x = (int16_t)x11_u16le(body + 8);
    int16_t y = (int16_t)x11_u16le(body + 10);
    uint16_t w = x11_u16le(body + 12);
    uint16_t h = x11_u16le(body + 14);
    uint16_t bw = x11_u16le(body + 16);
    uint16_t class = x11_u16le(body + 18);
    uint32_t visual = x11_u32le(body + 20);
    (void)parent; (void)class; (void)visual;
    uint32_t bg = 0;
    if (body_len >= 28) {
        uint32_t vmask = x11_u32le(body + 24);
        int off = 28;
        if (vmask & 0x01) { bg = x11_u32le(body + off); off += 4; }
    }
    x11_window_create(id, parent, x, y, w, h, 0, bg);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_reply_put16(&r, &ro, seq);
    x11_reply_put32(&r, &ro, id);
    x11_reply_put16(&r, &ro, x);
    x11_reply_put16(&r, &ro, y);
    x11_reply_put16(&r, &ro, w);
    x11_reply_put16(&r, &ro, h);
    x11_reply_put16(&r, &ro, bw);
    x11_reply_put16(&r, &ro, 0);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_destroy_window(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 4) return;
    uint32_t id = x11_u32le(body);
    x11_send_destroy_notify(client_fd, id, id);
    x11_window_destroy(id);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_map_window(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 4) return;
    uint32_t id = x11_u32le(body);
    x11_window_map(id);
    x11_send_map_notify(client_fd, id);
    x11_send_expose(client_fd, id, 0, 0, 0, 0);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_unmap_window(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 4) return;
    uint32_t id = x11_u32le(body);
    x11_window_unmap(id);
    x11_send_unmap_notify(client_fd, id);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_configure_window(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 8) return;
    uint32_t id = x11_u32le(body);
    uint32_t vmask = x11_u16le(body + 4);
    int off = 8;
    x11_window_t* w = x11_window_get(id);
    if (!w) return;
    int16_t nx = w->x, ny = w->y;
    uint16_t nw = w->width, nh = w->height;
    if (vmask & 0x01) { nx = (int16_t)x11_u16le(body + off); off += 2; }
    if (vmask & 0x02) { ny = (int16_t)x11_u16le(body + off); off += 2; }
    if (vmask & 0x04) { nw = x11_u16le(body + off); off += 2; }
    if (vmask & 0x08) { nh = x11_u16le(body + off); off += 2; }
    x11_window_configure(id, nx, ny, nw, nh);
    x11_send_configure_notify(client_fd, id, nx, ny, nw, nh, 0, 0);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_get_geometry(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    (void)body; (void)body_len;
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_reply_put32(&r, &ro, 0);
    x11_reply_put16(&r, &ro, 0);
    x11_reply_put16(&r, &ro, 0);
    x11_reply_put16(&r, &ro, (uint16_t)g_fb.width);
    x11_reply_put16(&r, &ro, (uint16_t)g_fb.height);
    x11_reply_put16(&r, &ro, 0);
    x11_reply_put32(&r, &ro, 0);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_query_tree(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    (void)body; (void)body_len;
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_reply_put32(&r, &ro, 0);
    x11_reply_put32(&r, &ro, 0);
    x11_reply_put32(&r, &ro, 0);
    x11_reply_put16(&r, &ro, 0);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_intern_atom(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 4) return;
    uint8_t only = body[0];
    uint16_t namelen = x11_u16le(body + 2);
    const char* name = (const char*)(body + 4);
    (void)only;
    uint32_t atom = x11_intern_atom(name, namelen);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_reply_put32(&r, &ro, atom);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_get_atom_name(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 4) return;
    uint32_t atom = x11_u32le(body);
    const char* name = x11_get_atom_name(atom);
    uint16_t nlen = strlen(name);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_reply_put16(&r, &ro, nlen);
    x11_reply_put32(&r, &ro, 0);
    x11_write_reply(client_fd, r.data, 32);
    if (nlen > 0) x11_socket_write(client_fd, (const uint8_t*)name, nlen);
    int pad = (4 - (nlen % 4)) % 4;
    uint8_t z[4] = {0};
    if (pad) x11_socket_write(client_fd, z, pad);
}

static void handle_change_property(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 16) return;
    uint32_t window = x11_u32le(body + 0);
    uint32_t atom = x11_u32le(body + 4);
    uint32_t type = x11_u32le(body + 8);
    uint8_t format = body[12];
    uint32_t dlen = x11_u32le(body + 13);
    (void)dlen;
    const uint8_t* data = body + 17;
    int dlen_actual = body_len - 17;
    if (format == 8) {
        x11_window_set_property(window, atom, type, 8, data, dlen_actual);
    } else if (format == 16) {
        x11_window_set_property(window, atom, type, 16, data, dlen_actual);
    } else if (format == 32) {
        x11_window_set_property(window, atom, type, 32, data, dlen_actual);
    }
    if (atom == X11_ATOM_WM_NAME || atom == X11_ATOM_NET_WM_NAME) {
        x11_window_set_title(window, (const char*)data, dlen_actual);
    }
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_get_property(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 8) return;
    uint8_t del = body[0];
    uint32_t window = x11_u32le(body + 1);
    uint32_t atom = x11_u32le(body + 5);
    (void)del;
    uint32_t type = 0, format = 0, len = 256;
    uint8_t data[256];
    memset(data, 0, sizeof(data));
    x11_window_get_property(window, atom, &type, &format, data, &len);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_reply_put32(&r, &ro, type);
    x11_reply_put32(&r, &ro, 0);
    x11_reply_put32(&r, &ro, len);
    x11_write_reply(client_fd, r.data, 32);
    if (len > 0) x11_socket_write(client_fd, data, len);
    int pad = (4 - (len % 4)) % 4;
    uint8_t z[4] = {0};
    if (pad) x11_socket_write(client_fd, z, pad);
}

static void handle_set_input_focus(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 4) return;
    uint32_t id = x11_u32le(body);
    x11_window_set_focus(id);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_get_input_focus(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    (void)body; (void)body_len;
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_reply_put32(&r, &ro, x11_window_get_focus());
    x11_reply_put32(&r, &ro, 0);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_create_gc(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 8) return;
    uint32_t id = x11_u32le(body);
    uint32_t drawable = x11_u32le(body + 4);
    (void)drawable;
    x11_gc_create(id, body + 8, body_len - 8);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_change_gc(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 4) return;
    uint32_t id = x11_u32le(body);
    x11_gc_change(id, body + 4, body_len - 4);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_free_gc(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 4) return;
    uint32_t id = x11_u32le(body);
    x11_gc_free(id);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_create_pixmap(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 12) return;
    uint32_t id = x11_u32le(body);
    uint32_t drawable = x11_u32le(body + 4);
    uint16_t w = x11_u16le(body + 8);
    uint16_t h = x11_u16le(body + 10);
    uint8_t depth = body[12];
    (void)drawable;
    x11_pixmap_create(id, w, h, depth);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_free_pixmap(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 4) return;
    uint32_t id = x11_u32le(body);
    x11_pixmap_free(id);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_poly_fill_rect(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 8) return;
    uint32_t drawable = x11_u32le(body);
    uint32_t gcid = x11_u32le(body + 4);
    x11_gc_t* gc = x11_gc_get(gcid);
    x11_window_t* w = x11_window_get(drawable);
    if (!w || !w->surface || !gc) goto ack;
    uint32_t color = gc->fg | 0xFF000000;
    int num = (body_len - 8) / 8;
    const uint8_t* p = body + 8;
    for (int i = 0; i < num; i++) {
        int16_t rx = (int16_t)x11_u16le(p);
        int16_t ry = (int16_t)x11_u16le(p + 2);
        uint16_t rw = x11_u16le(p + 4);
        uint16_t rh = x11_u16le(p + 6);
        x11_draw_rect(w->surface, w->width, w->height, rx, ry, rw, rh, color);
        p += 8;
    }
    w->dirty = 1;
ack:;
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_poly_line(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 8) return;
    uint32_t drawable = x11_u32le(body);
    uint32_t gcid = x11_u32le(body + 4);
    x11_gc_t* gc = x11_gc_get(gcid);
    x11_window_t* w = x11_window_get(drawable);
    if (!w || !w->surface || !gc) goto ack;
    uint32_t color = gc->fg | 0xFF000000;
    int num = (body_len - 8) / 4;
    if (num < 1) goto ack;
    int prev_x = (int16_t)x11_u16le(body + 8);
    int prev_y = (int16_t)x11_u16le(body + 10);
    for (int i = 1; i < num; i++) {
        int cx = (int16_t)x11_u16le(body + 8 + i * 4);
        int cy = (int16_t)x11_u16le(body + 10 + i * 4);
        x11_draw_line(w->surface, w->width, w->height, prev_x, prev_y, cx, cy, color);
        prev_x = cx; prev_y = cy;
    }
    w->dirty = 1;
ack:;
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_poly_segment(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 8) return;
    uint32_t drawable = x11_u32le(body);
    uint32_t gcid = x11_u32le(body + 4);
    x11_gc_t* gc = x11_gc_get(gcid);
    x11_window_t* w = x11_window_get(drawable);
    if (!w || !w->surface || !gc) goto ack;
    uint32_t color = gc->fg | 0xFF000000;
    int num = (body_len - 8) / 8;
    const uint8_t* p = body + 8;
    for (int i = 0; i < num; i++) {
        int x0 = (int16_t)x11_u16le(p);
        int y0 = (int16_t)x11_u16le(p + 2);
        int x1 = (int16_t)x11_u16le(p + 4);
        int y1 = (int16_t)x11_u16le(p + 6);
        x11_draw_line(w->surface, w->width, w->height, x0, y0, x1, y1, color);
        p += 8;
    }
    w->dirty = 1;
ack:;
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_poly_arc(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 8) return;
    uint32_t drawable = x11_u32le(body);
    uint32_t gcid = x11_u32le(body + 4);
    x11_gc_t* gc = x11_gc_get(gcid);
    x11_window_t* w = x11_window_get(drawable);
    if (!w || !w->surface || !gc) goto ack;
    uint32_t color = gc->fg | 0xFF000000;
    int num = (body_len - 8) / 12;
    const uint8_t* p = body + 8;
    for (int i = 0; i < num; i++) {
        int cx = (int16_t)x11_u16le(p) + (int16_t)x11_u16le(p + 4) / 2;
        int cy = (int16_t)x11_u16le(p + 2) + (int16_t)x11_u16le(p + 6) / 2;
        int rx = (int16_t)x11_u16le(p + 4) / 2;
        int ry = (int16_t)x11_u16le(p + 6) / 2;
        int a1 = x11_u16le(p + 8);
        int a2 = x11_u16le(p + 10);
        x11_draw_arc(w->surface, w->width, w->height, cx, cy, rx, ry, a1, a2, color);
        p += 12;
    }
    w->dirty = 1;
ack:;
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_poly_fill_arc(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    handle_poly_arc(client_fd, body, body_len, seq);
}

static void handle_fill_poly(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 16) return;
    uint32_t drawable = x11_u32le(body);
    uint32_t gcid = x11_u32le(body + 4);
    (void)gcid;
    x11_window_t* w = x11_window_get(drawable);
    x11_gc_t* gc = x11_gc_get(gcid);
    if (!w || !w->surface || !gc) goto ack;
    int npts = (body_len - 16) / 4;
    if (npts > 0) {
        int pts[128];
        int np = npts > 32 ? 32 : npts;
        for (int i = 0; i < np; i++) {
            pts[i * 2] = (int16_t)x11_u16le(body + 16 + i * 4);
            pts[i * 2 + 1] = (int16_t)x11_u16le(body + 18 + i * 4);
        }
        x11_fill_polygon(w->surface, w->width, w->height, pts, np, gc->fg | 0xFF000000);
    }
    w->dirty = 1;
ack:;
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_put_image(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 16) return;
    uint8_t format = body[0];
    uint32_t drawable = x11_u32le(body + 4);
    uint16_t w = x11_u16le(body + 8);
    uint16_t h = x11_u16le(body + 10);
    int16_t dstx = (int16_t)x11_u16le(body + 12);
    int16_t dsty = (int16_t)x11_u16le(body + 14);
    (void)format;
    x11_window_t* win = x11_window_get(drawable);
    if (win && win->surface) {
        int data_off = 16;
        int data_len = body_len - data_off;
        if (data_len >= (int)w * h * 4) {
            x11_put_image(win->surface, win->width, win->height,
                          dstx, dsty, w, h, body + data_off, 24);
            win->dirty = 1;
        }
    }
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_copy_area(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 28) return;
    uint32_t src_draw = x11_u32le(body + 4);
    uint32_t dst_draw = x11_u32le(body + 8);
    uint32_t gcid = x11_u32le(body + 12);
    int16_t sx = (int16_t)x11_u16le(body + 16);
    int16_t sy = (int16_t)x11_u16le(body + 18);
    uint16_t w = x11_u16le(body + 20);
    uint16_t h = x11_u16le(body + 22);
    int16_t dx = (int16_t)x11_u16le(body + 24);
    int16_t dy = (int16_t)x11_u16le(body + 26);
    (void)gcid;
    x11_window_t* src = x11_window_get(src_draw);
    x11_window_t* dst = x11_window_get(dst_draw);
    if (src && dst && src->surface && dst->surface) {
        x11_copy_area(dst->surface, dst->width, dst->height, dx, dy,
                      src->surface, src->width, src->height, sx, sy, w, h);
        dst->dirty = 1;
    }
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_image_text8(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 8) return;
    uint32_t drawable = x11_u32le(body);
    uint32_t gcid = x11_u32le(body + 4);
    int16_t x = (int16_t)x11_u16le(body + 8);
    int16_t y = (int16_t)x11_u16le(body + 10);
    int tlen = body_len - 12;
    if (tlen > 255) tlen = 255;
    x11_gc_t* gc = x11_gc_get(gcid);
    x11_window_t* w = x11_window_get(drawable);
    if (w && w->surface && gc) {
        x11_draw_text(w->surface, w->width, w->height, x, y,
                      (const char*)(body + 12), tlen, gc->fg | 0xFF000000, 8);
        w->dirty = 1;
    }
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_alloc_color(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 8) return;
    uint16_t r = x11_u16le(body);
    uint16_t g = x11_u16le(body + 2);
    uint16_t b = x11_u16le(body + 4);
    uint32_t pixel = x11_alloc_color(r, g, b);
    x11_reply_t rep;
    int ro = 0;
    x11_reply_init(&rep, 1, seq);
    x11_reply_put8(&rep, &ro, 1);
    x11_reply_put32(&rep, &ro, pixel);
    x11_reply_put16(&rep, &ro, r);
    x11_reply_put16(&rep, &ro, g);
    x11_reply_put16(&rep, &ro, b);
    x11_reply_put16(&rep, &ro, 0);
    x11_write_reply(client_fd, rep.data, 32);
}

static void handle_alloc_named_color(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 4) return;
    uint16_t namelen = x11_u16le(body + 2);
    const char* name = (const char*)(body + 4);
    uint16_t r, g, b;
    if (x11_alloc_named_color(name, namelen, &r, &g, &b) == 0) {
        uint32_t pixel = x11_alloc_color(r, g, b);
        x11_reply_t rep;
        int ro = 0;
        x11_reply_init(&rep, 1, seq);
        x11_reply_put8(&rep, &ro, 1);
        x11_reply_put32(&rep, &ro, pixel);
        x11_reply_put16(&rep, &ro, r);
        x11_reply_put16(&rep, &ro, g);
        x11_reply_put16(&rep, &ro, b);
        x11_reply_put16(&rep, &ro, r);
        x11_reply_put16(&rep, &ro, g);
        x11_reply_put16(&rep, &ro, b);
        x11_write_reply(client_fd, rep.data, 32);
    } else {
        x11_reply_t rep;
        int ro = 0;
        x11_reply_init(&rep, 0, seq);
        x11_reply_put8(&rep, &ro, 0);
        x11_reply_put16(&rep, &ro, 1);
        x11_reply_put32(&rep, &ro, 0);
        x11_write_reply(client_fd, rep.data, 32);
    }
}

static void handle_query_colors(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 4) return;
    int npix = x11_u16le(body + 2);
    x11_reply_t rep;
    int ro = 0;
    x11_reply_init(&rep, 1, seq);
    x11_reply_put8(&rep, &ro, 1);
    x11_reply_put16(&rep, &ro, (uint16_t)npix);
    x11_write_reply(client_fd, rep.data, 32);
    for (int i = 0; i < npix && body_len >= 8 + i * 4; i++) {
        uint8_t buf[8];
        x11_put16le(buf + 0, 0);
        x11_put16le(buf + 2, 0);
        x11_put16le(buf + 4, 0);
        x11_put16le(buf + 6, 0);
        x11_socket_write(client_fd, buf, 8);
    }
}

static void handle_set_selection_owner(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    (void)body; (void)body_len;
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_get_selection_owner(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    (void)body; (void)body_len;
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_reply_put32(&r, &ro, 0);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_convert_selection(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 16) return;
    uint32_t requestor = x11_u32le(body);
    uint32_t selection = x11_u32le(body + 4);
    uint32_t target = x11_u32le(body + 8);
    uint32_t property = x11_u32le(body + 12);
    (void)selection; (void)target; (void)property;
    x11_event_t ev;
    x11_event_init(&ev, X11_EVENT_SELECTION_NOTIFY);
    x11_event_put32(&ev, 4, requestor);
    x11_event_put32(&ev, 8, selection);
    x11_event_put32(&ev, 12, target);
    x11_event_put32(&ev, 16, 0);
    x11_write_event(client_fd, &ev);
    (void)seq;
}

static void handle_list_properties(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 4) return;
    uint32_t window = x11_u32le(body);
    x11_window_t* w = x11_window_get(window);
    int n = w ? w->num_props : 0;
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_reply_put16(&r, &ro, (uint16_t)n);
    x11_reply_put32(&r, &ro, 0);
    x11_write_reply(client_fd, r.data, 32);
    for (int i = 0; i < n; i++) {
        uint8_t buf[4];
        x11_put32le(buf, w->props[i].atom);
        x11_socket_write(client_fd, buf, 4);
    }
}

static void handle_open_font(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 8) return;
    uint32_t id = x11_u32le(body);
    uint16_t namelen = x11_u16le(body + 4);
    x11_font_open(id, (const char*)(body + 8), namelen);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_close_font(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 4) return;
    uint32_t id = x11_u32le(body);
    x11_font_close(id);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_query_font(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 4) return;
    uint32_t id = x11_u32le(body);
    int ascent = 6, descent = 2, height = 8, width = 8;
    x11_font_query(id, &ascent, &descent, &height, &width);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_reply_put32(&r, &ro, id);
    x11_reply_put16(&r, &ro, (uint16_t)height);
    x11_reply_put16(&r, &ro, (uint16_t)ascent);
    x11_reply_put16(&r, &ro, (uint16_t)descent);
    x11_reply_put16(&r, &ro, 0);
    x11_reply_put16(&r, &ro, 0);
    x11_reply_put16(&r, &ro, (uint16_t)width);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_list_fonts(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 4) return;
    uint16_t maxnames = x11_u16le(body);
    uint16_t patlen = x11_u16le(body + 2);
    char buf[256];
    int total = x11_font_list((const char*)(body + 4), patlen, buf, sizeof(buf));
    int nfonts = total > 0 ? 1 : 0;
    if (nfonts > maxnames) nfonts = maxnames;
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_reply_put16(&r, &ro, (uint16_t)nfonts);
    x11_reply_put32(&r, &ro, 0);
    x11_write_reply(client_fd, r.data, 32);
    if (nfonts > 0) {
        uint16_t nl = total;
        x11_socket_write(client_fd, (const uint8_t*)&nl, 2);
        x11_socket_write(client_fd, (const uint8_t*)buf, nl);
        int pad = (4 - ((nl + 2) % 4)) % 4;
        uint8_t z[4] = {0};
        if (pad) x11_socket_write(client_fd, z, pad);
    }
}

static void handle_get_window_attr(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 4) return;
    uint32_t id = x11_u32le(body);
    x11_window_t* w = x11_window_get(id);
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_reply_put32(&r, &ro, 0);
    x11_reply_put16(&r, &ro, 1);
    x11_reply_put16(&r, &ro, 0);
    x11_reply_put16(&r, &ro, w ? (uint16_t)w->width : 0);
    x11_reply_put16(&r, &ro, w ? (uint16_t)w->height : 0);
    x11_reply_put16(&r, &ro, w ? (uint16_t)w->border_width : 0);
    x11_reply_put16(&r, &ro, w ? (uint16_t)(w->mapped ? 2 : 0) : 0);
    x11_reply_put32(&r, &ro, w ? w->event_mask : 0);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_change_window_attr(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 8) return;
    uint32_t id = x11_u32le(body);
    uint32_t vmask = x11_u32le(body + 4);
    int off = 8;
    x11_window_t* w = x11_window_get(id);
    if (!w) goto ack;
    if (vmask & 0x01) { w->bg_pixel = x11_u32le(body + off); off += 4; }
    if (vmask & 0x0800) { w->event_mask = x11_u32le(body + off); off += 4; }
ack:;
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_copy_plane(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    handle_copy_area(client_fd, body, body_len, seq);
}

static void handle_poly_point(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 8) return;
    uint32_t drawable = x11_u32le(body);
    uint32_t gcid = x11_u32le(body + 4);
    x11_gc_t* gc = x11_gc_get(gcid);
    x11_window_t* w = x11_window_get(drawable);
    if (w && w->surface && gc) {
        uint32_t color = gc->fg | 0xFF000000;
        int num = (body_len - 8) / 4;
        const uint8_t* p = body + 8;
        for (int i = 0; i < num; i++) {
            int px = (int16_t)x11_u16le(p);
            int py = (int16_t)x11_u16le(p + 2);
            x11_draw_rect(w->surface, w->width, w->height, px, py, 1, 1, color);
            p += 4;
        }
        w->dirty = 1;
    }
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_clear_area(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    if (body_len < 8) return;
    uint32_t drawable = x11_u32le(body);
    int16_t x = (int16_t)x11_u16le(body + 4);
    int16_t y = (int16_t)x11_u16le(body + 6);
    uint16_t w = x11_u16le(body + 8);
    uint16_t h = x11_u16le(body + 10);
    (void)x; (void)y; (void)w; (void)h;
    x11_window_t* win = x11_window_get(drawable);
    if (win && win->surface) {
        uint32_t bg = win->bg_pixel | 0xFF000000;
        uint32_t* px = (uint32_t*)win->surface;
        for (uint32_t p = 0; p < (uint32_t)win->width * win->height; p++) px[p] = bg;
        win->dirty = 1;
    }
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_delete_property(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    (void)body; (void)body_len;
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_no_op(int client_fd, const uint8_t* body, int body_len, uint16_t seq) {
    (void)body; (void)body_len;
    x11_reply_t r;
    int ro = 0;
    x11_reply_init(&r, 1, seq);
    x11_reply_put8(&r, &ro, 1);
    x11_write_reply(client_fd, r.data, 32);
}

static void handle_request(int client_fd, uint8_t opcode, const uint8_t* body,
                           int body_len, uint16_t seq) {
    switch (opcode) {
    case REQ_CREATE_WINDOW:
        handle_create_window(client_fd, body, body_len, seq); break;
    case REQ_CHANGE_WINDOW_ATTR:
        handle_change_window_attr(client_fd, body, body_len, seq); break;
    case REQ_GET_WINDOW_ATTR:
        handle_get_window_attr(client_fd, body, body_len, seq); break;
    case REQ_DESTROY_WINDOW:
        handle_destroy_window(client_fd, body, body_len, seq); break;
    case REQ_MAP_WINDOW:
    case REQ_MAP_SUBWINDOWS:
        handle_map_window(client_fd, body, body_len, seq); break;
    case REQ_UNMAP_WINDOW:
        handle_unmap_window(client_fd, body, body_len, seq); break;
    case REQ_CONFIGURE_WINDOW:
        handle_configure_window(client_fd, body, body_len, seq); break;
    case REQ_GET_GEOMETRY:
        handle_get_geometry(client_fd, body, body_len, seq); break;
    case REQ_QUERY_TREE:
        handle_query_tree(client_fd, body, body_len, seq); break;
    case REQ_INTERN_ATOM:
        handle_intern_atom(client_fd, body, body_len, seq); break;
    case REQ_GET_ATOM_NAME:
        handle_get_atom_name(client_fd, body, body_len, seq); break;
    case REQ_CHANGE_PROPERTY:
        handle_change_property(client_fd, body, body_len, seq); break;
    case REQ_DELETE_PROPERTY:
        handle_delete_property(client_fd, body, body_len, seq); break;
    case REQ_GET_PROPERTY:
        handle_get_property(client_fd, body, body_len, seq); break;
    case REQ_LIST_PROPERTIES:
        handle_list_properties(client_fd, body, body_len, seq); break;
    case REQ_SET_INPUT_FOCUS:
        handle_set_input_focus(client_fd, body, body_len, seq); break;
    case REQ_GET_INPUT_FOCUS:
        handle_get_input_focus(client_fd, body, body_len, seq); break;
    case REQ_OPEN_FONT:
        handle_open_font(client_fd, body, body_len, seq); break;
    case REQ_CLOSE_FONT:
        handle_close_font(client_fd, body, body_len, seq); break;
    case REQ_QUERY_FONT:
        handle_query_font(client_fd, body, body_len, seq); break;
    case REQ_LIST_FONTS:
    case REQ_LIST_FONTS_WITH_INFO:
        handle_list_fonts(client_fd, body, body_len, seq); break;
    case REQ_CREATE_GC:
        handle_create_gc(client_fd, body, body_len, seq); break;
    case REQ_CHANGE_GC:
        handle_change_gc(client_fd, body, body_len, seq); break;
    case REQ_FREE_GC:
        handle_free_gc(client_fd, body, body_len, seq); break;
    case REQ_CLEAR_AREA:
        handle_clear_area(client_fd, body, body_len, seq); break;
    case REQ_COPY_AREA:
        handle_copy_area(client_fd, body, body_len, seq); break;
    case REQ_COPY_PLANE:
        handle_copy_plane(client_fd, body, body_len, seq); break;
    case REQ_POLY_POINT:
        handle_poly_point(client_fd, body, body_len, seq); break;
    case REQ_POLY_LINE:
        handle_poly_line(client_fd, body, body_len, seq); break;
    case REQ_POLY_SEGMENT:
        handle_poly_segment(client_fd, body, body_len, seq); break;
    case REQ_POLY_ARC:
        handle_poly_arc(client_fd, body, body_len, seq); break;
    case REQ_FILL_POLY:
        handle_fill_poly(client_fd, body, body_len, seq); break;
    case REQ_POLY_FILL_RECTANGLE:
        handle_poly_fill_rect(client_fd, body, body_len, seq); break;
    case REQ_POLY_FILL_ARC:
        handle_poly_fill_arc(client_fd, body, body_len, seq); break;
    case REQ_PUT_IMAGE:
        handle_put_image(client_fd, body, body_len, seq); break;
    case REQ_POLY_TEXT8:
    case REQ_IMAGE_TEXT8:
        handle_image_text8(client_fd, body, body_len, seq); break;
    case REQ_CREATE_PIXMAP:
        handle_create_pixmap(client_fd, body, body_len, seq); break;
    case REQ_FREE_PIXMAP:
        handle_free_pixmap(client_fd, body, body_len, seq); break;
    case REQ_ALLOC_COLOR:
        handle_alloc_color(client_fd, body, body_len, seq); break;
    case REQ_ALLOC_NAMED_COLOR:
        handle_alloc_named_color(client_fd, body, body_len, seq); break;
    case REQ_QUERY_COLORS:
        handle_query_colors(client_fd, body, body_len, seq); break;
    case REQ_SET_SELECTION_OWNER:
        handle_set_selection_owner(client_fd, body, body_len, seq); break;
    case REQ_GET_SELECTION_OWNER:
        handle_get_selection_owner(client_fd, body, body_len, seq); break;
    case REQ_CONVERT_SELECTION:
        handle_convert_selection(client_fd, body, body_len, seq); break;
    case REQ_NO_OPERATION:
        handle_no_op(client_fd, body, body_len, seq); break;
    default:
        break;
    }
}

int g_client_fds[X11_MAX_CLIENTS];

static void route_input(x11_mouse_t* mouse, x11_kbd_t* kbd) {
    (void)kbd;
    static int prev_focused = -1;
    x11_window_t* focused = x11_window_at(mouse->x, mouse->y);
    int new_id = focused ? focused->id : 0;

    if ((uint32_t)new_id != (uint32_t)prev_focused) {
        if (prev_focused > 0) {
            for (int c = 0; c < X11_MAX_CLIENTS; c++) {
                if (g_client_fds[c] > 0) {
                    x11_send_focus_out(g_client_fds[c], prev_focused);
                }
            }
        }
        if (new_id > 0) {
            x11_window_set_focus(new_id);
            for (int c = 0; c < X11_MAX_CLIENTS; c++) {
                if (g_client_fds[c] > 0) {
                    x11_send_focus_in(g_client_fds[c], new_id);
                    x11_send_enter_notify(g_client_fds[c], new_id, mouse->x, mouse->y);
                }
            }
        }
        prev_focused = new_id;
    }

    if (new_id > 0 && focused) {
        for (int c = 0; c < X11_MAX_CLIENTS; c++) {
            if (g_client_fds[c] <= 0) continue;
            if (mouse->dx != 0 || mouse->dy != 0) {
                x11_send_motion_notify(g_client_fds[c], new_id,
                    mouse->x - focused->x, mouse->y - focused->y, 0);
            }
        }
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    x11_mem_init(2 * 1024 * 1024);

    if (sys_fb_init(&g_fb) < 0) return 1;
    g_backbuf = x11_mem_alloc(g_fb.pitch * g_fb.height);
    if (!g_backbuf) return 1;

    x11_atoms_init();
    x11_windows_init();
    x11_gc_init();
    x11_pixmap_init();
    x11_events_init();
    x11_property_init();
    x11_font_init();
    x11_color_init();
    x11_input_init();

    if (x11_socket_init() < 0) return 1;

    memset(g_client_fds, 0xFF, sizeof(g_client_fds));

    for (int i = 0; i < X11_MAX_CLIENTS; i++) g_client_fds[i] = -1;

    while (g_running) {
        int listen_fd = x11_socket_get_listen_fd();
        int new_fd = -1;
        if (listen_fd >= 0) {
            new_fd = accept(listen_fd, NULL, NULL);
            if (new_fd >= 0) {
                for (int i = 0; i < X11_MAX_CLIENTS; i++) {
                    if (g_client_fds[i] < 0) {
                        g_client_fds[i] = new_fd;
                        x11_socket_set_nonblock(new_fd);

                        uint8_t setup[32];
                        memset(setup, 0, 32);
                        setup[0] = 1;
                        setup[1] = 0;
                        x11_put16le(setup + 2, 11);
                        x11_put32le(setup + 4, 1);
                        x11_put32le(setup + 8, 0);
                        x11_put32le(setup + 12, 0);
                        x11_put16le(setup + 16, 0);
                        x11_put16le(setup + 18, 0);
                        x11_put16le(setup + 20, 1024);
                        x11_put16le(setup + 22, 0);
                        write(new_fd, setup, 32);

                        uint8_t setup2[32];
                        memset(setup2, 0, 32);
                        x11_put32le(setup2 + 0, 0);
                        x11_put32le(setup2 + 4, 0);
                        x11_put32le(setup2 + 8, 0);
                        x11_put32le(setup2 + 12, 0);
                        x11_put32le(setup2 + 16, 0);
                        x11_put32le(setup2 + 20, 0);
                        x11_put16le(setup2 + 24, 8);
                        x11_put16le(setup2 + 26, 8);
                        x11_put16le(setup2 + 28, 8);
                        write(new_fd, setup2, 32);
                        break;
                    }
                }
                if (new_fd >= 0) {
                    int found = 0;
                    for (int i = 0; i < X11_MAX_CLIENTS; i++)
                        if (g_client_fds[i] == new_fd) { found = 1; break; }
                    if (!found) close(new_fd);
                }
            }
        }

        x11_mouse_t mouse;
        x11_kbd_t kbd;
        x11_input_poll(&mouse, &kbd);
        route_input(&mouse, &kbd);

        uint8_t req_buf[4096];
        for (int c = 0; c < X11_MAX_CLIENTS; c++) {
            if (g_client_fds[c] < 0) continue;
            int fd = g_client_fds[c];

            uint8_t hdr[4];
            int n = read(fd, hdr, 4);
            if (n == 0) {
                close(fd);
                g_client_fds[c] = -1;
                continue;
            }
            if (n < 0) continue;
            if (n < 4) continue;

            uint8_t opcode = hdr[0];
            uint16_t rlen = x11_u16le(hdr + 2);
            int total = rlen * 4;
            if (total > (int)sizeof(req_buf)) total = sizeof(req_buf);
            if (total > 4) {
                int got = read(fd, req_buf + 4, total - 4);
                if (got < 0) got = 0;
            }
            memcpy(req_buf, hdr, 4);
            handle_request(fd, opcode, req_buf + 4, total - 4, x11_u16le(hdr + 2));
        }

        composite_all();
        flip_to_fb();
        sys_fb_flush();

        for (int i = 0; i < X11_MAX_WINDOWS; i++) {
            g_x11_windows[i].dirty = 0;
        }
    }

    return 0;
}
