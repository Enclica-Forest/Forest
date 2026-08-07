#include "x11_window.h"
#include "mem.h"
#include <string.h>

x11_window_t g_x11_windows[X11_MAX_WINDOWS];
#define g_windows g_x11_windows
static uint32_t g_focus_id = 0;
static int g_z_counter = 0;

void x11_windows_init(void) {
    memset(g_windows, 0, sizeof(g_windows));
    g_focus_id = 0;
    g_z_counter = 0;
}

int x11_window_create(uint32_t id, uint32_t parent, int16_t x, int16_t y,
                      uint16_t w, uint16_t h, uint32_t event_mask, uint32_t bg) {
    if (w == 0 || h == 0) return -1;
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (!g_windows[i].used) {
            memset(&g_windows[i], 0, sizeof(x11_window_t));
            g_windows[i].used = 1;
            g_windows[i].id = id;
            g_windows[i].parent = parent;
            g_windows[i].x = x;
            g_windows[i].y = y;
            g_windows[i].width = w;
            g_windows[i].height = h;
            g_windows[i].event_mask = event_mask;
            g_windows[i].bg_pixel = bg;
            g_windows[i].mapped = 0;
            g_windows[i].z_order = g_z_counter++;
            g_windows[i].border_width = 0;
            g_windows[i].surface = x11_mem_alloc((uint32_t)w * h * 4);
            if (g_windows[i].surface) {
                uint32_t argb = bg | 0xFF000000;
                uint32_t* px = (uint32_t*)g_windows[i].surface;
                for (uint32_t p = 0; p < (uint32_t)w * h; p++) px[p] = argb;
            }
            g_windows[i].num_props = 0;
            return 0;
        }
    }
    return -1;
}

void x11_window_destroy(uint32_t id) {
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (g_windows[i].used && g_windows[i].id == id) {
            if (g_windows[i].surface) x11_mem_free(g_windows[i].surface);
            memset(&g_windows[i], 0, sizeof(x11_window_t));
            return;
        }
    }
}

int x11_window_map(uint32_t id) {
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (g_windows[i].used && g_windows[i].id == id) {
            g_windows[i].mapped = 1;
            return 0;
        }
    }
    return -1;
}

int x11_window_unmap(uint32_t id) {
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (g_windows[i].used && g_windows[i].id == id) {
            g_windows[i].mapped = 0;
            return 0;
        }
    }
    return -1;
}

int x11_window_configure(uint32_t id, int16_t x, int16_t y, uint16_t w, uint16_t h) {
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (g_windows[i].used && g_windows[i].id == id) {
            x11_window_t* win = &g_windows[i];
            int resized = (w != win->width || h != win->height);
            win->x = x;
            win->y = y;
            if (resized && w > 0 && h > 0) {
                uint8_t* new_surf = x11_mem_alloc((uint32_t)w * h * 4);
                if (new_surf) {
                    uint32_t argb = win->bg_pixel | 0xFF000000;
                    uint32_t* px = (uint32_t*)new_surf;
                    for (uint32_t p = 0; p < (uint32_t)w * h; p++) px[p] = argb;
                    if (win->surface) x11_mem_free(win->surface);
                    win->surface = new_surf;
                    win->width = w;
                    win->height = h;
                }
            } else {
                win->width = w;
                win->height = h;
            }
            win->dirty = 1;
            return 0;
        }
    }
    return -1;
}

x11_window_t* x11_window_get(uint32_t id) {
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (g_windows[i].used && g_windows[i].id == id)
            return &g_windows[i];
    }
    return NULL;
}

x11_window_t* x11_window_at(int px, int py) {
    x11_window_t* best = NULL;
    int best_z = -1;
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (!g_windows[i].used || !g_windows[i].mapped) continue;
        x11_window_t* w = &g_windows[i];
        if (px >= w->x && px < w->x + (int)w->width &&
            py >= w->y && py < w->y + (int)w->height) {
            if (w->z_order > best_z) {
                best_z = w->z_order;
                best = w;
            }
        }
    }
    return best;
}

void x11_window_set_focus(uint32_t id) { g_focus_id = id; }
uint32_t x11_window_get_focus(void) { return g_focus_id; }

void x11_window_set_title(uint32_t id, const char* title, int len) {
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (g_windows[i].used && g_windows[i].id == id) {
            if (len > 63) len = 63;
            memcpy(g_windows[i].title, title, len);
            g_windows[i].title[len] = '\0';
            return;
        }
    }
}

void x11_window_set_zorder(uint32_t id, int z) {
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (g_windows[i].used && g_windows[i].id == id) {
            g_windows[i].z_order = z;
            return;
        }
    }
}

int x11_window_set_property(uint32_t id, uint32_t atom, uint32_t type,
                            uint32_t format, const uint8_t* data, uint32_t len) {
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (g_windows[i].used && g_windows[i].id == id) {
            x11_window_t* w = &g_windows[i];
            for (int j = 0; j < w->num_props; j++) {
                if (w->props[j].atom == atom) {
                    if (len > 256) len = 256;
                    w->props[j].type = type;
                    w->props[j].format = format;
                    w->props[j].len = len;
                    memcpy(w->props[j].data, data, len);
                    return 0;
                }
            }
            if (w->num_props < X11_MAX_PROPS) {
                x11_property_t* p = &w->props[w->num_props++];
                p->atom = atom;
                p->type = type;
                p->format = format;
                if (len > 256) len = 256;
                p->len = len;
                memcpy(p->data, data, len);
                return 0;
            }
        }
    }
    return -1;
}

int x11_window_get_property(uint32_t id, uint32_t atom, uint32_t* type,
                            uint32_t* format, uint8_t* data, uint32_t* len) {
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (g_windows[i].used && g_windows[i].id == id) {
            x11_window_t* w = &g_windows[i];
            for (int j = 0; j < w->num_props; j++) {
                if (w->props[j].atom == atom) {
                    *type = w->props[j].type;
                    *format = w->props[j].format;
                    uint32_t ml = w->props[j].len;
                    if (ml > *len) ml = *len;
                    memcpy(data, w->props[j].data, ml);
                    *len = ml;
                    return 0;
                }
            }
            *type = 0;
            *format = 0;
            *len = 0;
            return 0;
        }
    }
    return -1;
}
