#include "x11_font.h"
#include "mem.h"
#include <string.h>

#define X11_MAX_FONTS 16

static x11_font_t g_fonts[X11_MAX_FONTS];

void x11_font_init(void) {
    memset(g_fonts, 0, sizeof(g_fonts));
}

int x11_font_open(int id, const char* name, int namelen) {
    for (int i = 0; i < X11_MAX_FONTS; i++) {
        if (g_fonts[i].id == 0) {
            g_fonts[i].id = id;
            if (namelen > 63) namelen = 63;
            memcpy(g_fonts[i].name, name, namelen);
            g_fonts[i].name[namelen] = '\0';
            g_fonts[i].height = 8;
            g_fonts[i].width = 8;
            g_fonts[i].ascent = 6;
            g_fonts[i].descent = 2;
            return 0;
        }
    }
    return -1;
}

void x11_font_close(int id) {
    for (int i = 0; i < X11_MAX_FONTS; i++) {
        if (g_fonts[i].id == id) {
            g_fonts[i].id = 0;
            return;
        }
    }
}

int x11_font_query(int id, int* ascent, int* descent, int* height, int* width) {
    for (int i = 0; i < X11_MAX_FONTS; i++) {
        if (g_fonts[i].id == id) {
            if (ascent) *ascent = g_fonts[i].ascent;
            if (descent) *descent = g_fonts[i].descent;
            if (height) *height = g_fonts[i].height;
            if (width) *width = g_fonts[i].width;
            return 0;
        }
    }
    return -1;
}

int x11_font_list(const char* pattern, int patlen, char* buf, int buflen) {
    (void)pattern; (void)patlen;
    const char* name = "fixed";
    int len = strlen(name);
    if (len + 1 > buflen) return 0;
    memcpy(buf, name, len + 1);
    return len + 1;
}
