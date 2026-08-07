#include "sys.h"
#include <forestos/syscalls.h>
#include <string.h>

#define SYS_MMAP_FB       471
#define SYS_MUNMAP_FB     472
#define SYS_GET_FB_INFO   473
#define SYS_FB_FLUSH      478
#define SYS_READ_KBD      479
#define SYS_READ_MOUSE    480
#define SYS_POLL_INPUT    481
#define SYS_FB_DIRTY_RECT 496

typedef struct {
    void*  addr;
    uint32_t phys_addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t size;
    uint32_t format;
    uint32_t flags;
} fb_info_t;

typedef struct {
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} __attribute__((packed)) input_event_t;

#define EV_SYN   0x00
#define EV_KEY   0x01
#define EV_REL   0x02

#define REL_X    0x00
#define REL_Y    0x01
#define REL_WHEEL 0x08

#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

#define KEY_ESC       1
#define KEY_LEFTCTRL  29
#define KEY_LEFTSHIFT 42
#define KEY_RIGHTSHIFT 54
#define KEY_LEFTALT   56
#define KEY_CAPSLOCK  58

static x11_fb_t g_fb;
static x11_mouse_t g_mouse;
static x11_kbd_t g_kbd;

int sys_fb_init(x11_fb_t* fb) {
    fb_info_t info;
    int r = syscall1(SYS_GET_FB_INFO, (int)&info);
    if (r < 0) return -1;

    long addr = syscall0(SYS_MMAP_FB);
    if (addr < 0) return -1;

    fb->addr = (void*)addr;
    fb->width = info.width;
    fb->height = info.height;
    fb->pitch = info.pitch;
    fb->bpp = info.bpp;
    fb->size = info.size;
    fb->format = info.format;
    fb->bytes_per_pixel = (info.bpp + 7) / 8;

    g_fb = *fb;
    return 0;
}

void sys_fb_flush(void) {
    syscall0(SYS_FB_FLUSH);
}

void sys_fb_dirty_rect(int x, int y, int w, int h) {
    syscall4(SYS_FB_DIRTY_RECT, x, y, w, h);
}

int sys_poll_input(void) {
    return syscall0(SYS_POLL_INPUT);
}

int sys_read_kbd(void* buf, int max) {
    (void)max;
    return syscall1(SYS_READ_KBD, (int)buf);
}

int sys_read_mouse(void* buf, int max) {
    (void)max;
    return syscall1(SYS_READ_MOUSE, (int)buf);
}

int x11_input_init(void) {
    memset(&g_mouse, 0, sizeof(g_mouse));
    memset(&g_kbd, 0, sizeof(g_kbd));
    return 0;
}

void x11_input_poll(x11_mouse_t* mouse, x11_kbd_t* kbd) {
    int avail = sys_poll_input();
    input_event_t ev;

    while (avail & 2) {
        if (sys_read_mouse(&ev, sizeof(ev)) <= 0) break;
        if (ev.type == EV_REL) {
            if (ev.code == REL_X) g_mouse.dx += ev.value;
            if (ev.code == REL_Y) g_mouse.dy += ev.value;
            if (ev.code == REL_WHEEL) g_mouse.wheel += ev.value;
        } else if (ev.type == EV_KEY) {
            if (ev.code == BTN_LEFT) {
                if (ev.value) g_mouse.buttons |= 1;
                else g_mouse.buttons &= ~1;
            }
            if (ev.code == BTN_RIGHT) {
                if (ev.value) g_mouse.buttons |= 2;
                else g_mouse.buttons &= ~2;
            }
            if (ev.code == BTN_MIDDLE) {
                if (ev.value) g_mouse.buttons |= 4;
                else g_mouse.buttons &= ~4;
            }
        }
        avail = sys_poll_input();
    }

    g_mouse.x += g_mouse.dx;
    g_mouse.y += g_mouse.dy;
    if (g_mouse.x < 0) g_mouse.x = 0;
    if (g_mouse.y < 0) g_mouse.y = 0;
    if ((uint32_t)g_mouse.x >= g_fb.width) g_mouse.x = g_fb.width - 1;
    if ((uint32_t)g_mouse.y >= g_fb.height) g_mouse.y = g_fb.height - 1;

    while (avail & 1) {
        if (sys_read_kbd(&ev, sizeof(ev)) <= 0) break;
        if (ev.type == EV_KEY) {
            uint8_t code = (uint8_t)ev.code;
            if (code < 256) g_kbd.keys[code] = (uint8_t)(ev.value ? 1 : 0);
            if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT)
                g_kbd.shift = ev.value ? 1 : 0;
            if (code == KEY_LEFTCTRL) g_kbd.ctrl = ev.value ? 1 : 0;
            if (code == KEY_LEFTALT) g_kbd.alt = ev.value ? 1 : 0;
            if (code == KEY_CAPSLOCK && ev.value)
                g_kbd.caps_lock = !g_kbd.caps_lock;
        }
        avail = sys_poll_input();
    }

    *mouse = g_mouse;
    *kbd = g_kbd;
    g_mouse.dx = 0;
    g_mouse.dy = 0;
    g_mouse.wheel = 0;
}
