#ifndef X11_FONT_H
#define X11_FONT_H

#include <stdint.h>

typedef struct {
    int    id;
    char   name[64];
    int    height;
    int    width;
    int    ascent;
    int    descent;
} x11_font_t;

void     x11_font_init(void);
int      x11_font_open(int id, const char* name, int namelen);
void     x11_font_close(int id);
int      x11_font_query(int id, int* ascent, int* descent, int* height, int* width);
int      x11_font_list(const char* pattern, int patlen, char* buf, int buflen);

#endif
