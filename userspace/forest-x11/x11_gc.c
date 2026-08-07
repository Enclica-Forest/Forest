#include "x11_gc.h"
#include "x11_protocol.h"
#include <string.h>

static x11_gc_t g_gcs[X11_MAX_GCS];

void x11_gc_init(void) {
    memset(g_gcs, 0, sizeof(g_gcs));
}

int x11_gc_create(uint32_t id, const uint8_t* vals, int num_vals) {
    for (int i = 0; i < X11_MAX_GCS; i++) {
        if (!g_gcs[i].used) {
            memset(&g_gcs[i], 0, sizeof(x11_gc_t));
            g_gcs[i].used = 1;
            g_gcs[i].id = id;
            g_gcs[i].fg = 0x00000000;
            g_gcs[i].bg = 0xFFFFFFFF;
            g_gcs[i].function = 3;
            g_gcs[i].plane_mask = 0xFFFFFFFF;
            if (vals && num_vals > 0) {
                int off = 0;
                uint32_t mask = x11_u32le(vals); off += 4;
                for (int b = 0; b < 32 && b < num_vals - 4; b++) {
                    if (!(mask & (1u << b))) continue;
                    switch (b) {
                    case 0: g_gcs[i].function = vals[off]; off += 4; break;
                    case 1: g_gcs[i].plane_mask = x11_u32le(vals + off); off += 4; break;
                    case 2: g_gcs[i].fg = x11_u32le(vals + off); off += 4; break;
                    case 3: g_gcs[i].bg = x11_u32le(vals + off); off += 4; break;
                    case 6: g_gcs[i].line_width = x11_u16le(vals + off); off += 4; break;
                    case 7: g_gcs[i].cap_style = x11_u32le(vals + off); off += 4; break;
                    case 8: g_gcs[i].join_style = x11_u32le(vals + off); off += 4; break;
                    case 10: g_gcs[i].fill_style = x11_u32le(vals + off); off += 4; break;
                    default: off += 4; break;
                    }
                }
            }
            return 0;
        }
    }
    return -1;
}

int x11_gc_change(uint32_t id, const uint8_t* vals, int num_vals) {
    x11_gc_t* gc = x11_gc_get(id);
    if (!gc) return -1;
    if (!vals || num_vals < 4) return -1;
    uint32_t mask = x11_u32le(vals);
    int off = 4;
    for (int b = 0; b < 32; b++) {
        if (!(mask & (1u << b))) continue;
        if (off + 4 > num_vals) break;
        switch (b) {
        case 0: gc->function = vals[off]; off += 4; break;
        case 1: gc->plane_mask = x11_u32le(vals + off); off += 4; break;
        case 2: gc->fg = x11_u32le(vals + off); off += 4; break;
        case 3: gc->bg = x11_u32le(vals + off); off += 4; break;
        case 6: gc->line_width = x11_u16le(vals + off); off += 4; break;
        default: off += 4; break;
        }
    }
    return 0;
}

void x11_gc_free(uint32_t id) {
    for (int i = 0; i < X11_MAX_GCS; i++) {
        if (g_gcs[i].used && g_gcs[i].id == id) {
            g_gcs[i].used = 0;
            return;
        }
    }
}

x11_gc_t* x11_gc_get(uint32_t id) {
    for (int i = 0; i < X11_MAX_GCS; i++) {
        if (g_gcs[i].used && g_gcs[i].id == id)
            return &g_gcs[i];
    }
    return NULL;
}
