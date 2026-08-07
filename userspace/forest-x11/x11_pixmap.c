#include "x11_pixmap.h"
#include "mem.h"
#include <string.h>

static x11_pixmap_t g_pixmaps[X11_MAX_PIXMAPS];

void x11_pixmap_init(void) {
    memset(g_pixmaps, 0, sizeof(g_pixmaps));
}

int x11_pixmap_create(uint32_t id, uint16_t w, uint16_t h, uint8_t depth) {
    for (int i = 0; i < X11_MAX_PIXMAPS; i++) {
        if (!g_pixmaps[i].used) {
            g_pixmaps[i].used = 1;
            g_pixmaps[i].id = id;
            g_pixmaps[i].width = w;
            g_pixmaps[i].height = h;
            g_pixmaps[i].depth = depth;
            g_pixmaps[i].surface = x11_mem_alloc((uint32_t)w * h * 4);
            if (g_pixmaps[i].surface) {
                memset(g_pixmaps[i].surface, 0, (uint32_t)w * h * 4);
            }
            return 0;
        }
    }
    return -1;
}

void x11_pixmap_free(uint32_t id) {
    for (int i = 0; i < X11_MAX_PIXMAPS; i++) {
        if (g_pixmaps[i].used && g_pixmaps[i].id == id) {
            if (g_pixmaps[i].surface) x11_mem_free(g_pixmaps[i].surface);
            memset(&g_pixmaps[i], 0, sizeof(x11_pixmap_t));
            return;
        }
    }
}

x11_pixmap_t* x11_pixmap_get(uint32_t id) {
    for (int i = 0; i < X11_MAX_PIXMAPS; i++) {
        if (g_pixmaps[i].used && g_pixmaps[i].id == id)
            return &g_pixmaps[i];
    }
    return NULL;
}
