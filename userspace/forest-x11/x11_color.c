#include "x11_color.h"
#include <string.h>

#define MAX_COLORS 256

static x11_color_t g_colors[MAX_COLORS];
static int g_num_colors = 0;

typedef struct { const char* name; uint16_t r, g, b; } named_color_t;

static const named_color_t named_colors[] = {
    {"black", 0, 0, 0}, {"white", 65535, 65535, 65535},
    {"red", 65535, 0, 0}, {"green", 0, 65535, 0}, {"blue", 0, 0, 65535},
    {"yellow", 65535, 65535, 0}, {"cyan", 0, 65535, 65535},
    {"magenta", 65535, 0, 65535}, {"gray", 32768, 32768, 32768},
    {"grey", 32768, 32768, 32768}, {"silver", 49152, 49152, 49152},
    {"maroon", 32768, 0, 0}, {"olive", 32768, 32768, 0},
    {"navy", 0, 0, 32768}, {"purple", 32768, 0, 32768},
    {"teal", 0, 32768, 32768}, {"orange", 65535, 32768, 0},
    {"pink", 65535, 19660, 19660}, {"brown", 32768, 16384, 0},
    {0, 0, 0, 0}
};

void x11_color_init(void) {
    g_num_colors = 0;
    memset(g_colors, 0, sizeof(g_colors));
}

uint32_t x11_alloc_color(uint16_t r, uint16_t g, uint16_t b) {
    for (int i = 0; i < g_num_colors; i++) {
        if (g_colors[i].r == r && g_colors[i].g == g && g_colors[i].b == b)
            return g_colors[i].pixel;
    }
    if (g_num_colors < MAX_COLORS) {
        int i = g_num_colors++;
        g_colors[i].r = r;
        g_colors[i].g = g;
        g_colors[i].b = b;
        g_colors[i].pixel = i;
        return i;
    }
    return 0;
}

int x11_alloc_named_color(const char* name, int len, uint16_t* r, uint16_t* g, uint16_t* b) {
    for (int i = 0; named_colors[i].name; i++) {
        if ((int)strlen(named_colors[i].name) == len &&
            strncmp(named_colors[i].name, name, len) == 0) {
            *r = named_colors[i].r;
            *g = named_colors[i].g;
            *b = named_colors[i].b;
            return 0;
        }
    }
    return -1;
}

uint32_t x11_get_pixel(uint32_t cmap, uint16_t r, uint16_t g, uint16_t b) {
    (void)cmap;
    uint8_t cr = (uint8_t)(r >> 8);
    uint8_t cg = (uint8_t)(g >> 8);
    uint8_t cb = (uint8_t)(b >> 8);
    return 0xFF000000 | ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | cb;
}
