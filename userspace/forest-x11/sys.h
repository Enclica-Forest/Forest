#ifndef X11_SYS_H
#define X11_SYS_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    void*   addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t size;
    uint32_t format;
    uint32_t bytes_per_pixel;
} x11_fb_t;

typedef struct {
    int32_t  x, y;
    uint32_t buttons;
    int32_t  dx, dy;
    int32_t  wheel;
} x11_mouse_t;

typedef struct {
    uint8_t  keys[256];
    int      shift;
    int      ctrl;
    int      alt;
    int      caps_lock;
} x11_kbd_t;

int         sys_fb_init(x11_fb_t* fb);
void        sys_fb_flush(void);
void        sys_fb_dirty_rect(int x, int y, int w, int h);
int         sys_poll_input(void);
int         sys_read_kbd(void* buf, int max);
int         sys_read_mouse(void* buf, int max);

int         x11_input_init(void);
void        x11_input_poll(x11_mouse_t* mouse, x11_kbd_t* kbd);

#endif
