#include "os.h"
#include "../arch/framebuffer.h"

extern void* kmalloc(size_t size);
extern void  kfree(void* ptr);

void* gl_malloc(size_t size) {
    return kmalloc(size);
}

void gl_free(void* ptr) {
    kfree(ptr);
}

void* gl_get_framebuffer_ptr(void) {
    return framebuffer_get_buffer();
}

uint32_t gl_get_framebuffer_width(void) {
    uint32_t w, h, p, b;
    framebuffer_get_info(&w, &h, &p, &b);
    return w;
}

uint32_t gl_get_framebuffer_height(void) {
    uint32_t w, h, p, b;
    framebuffer_get_info(&w, &h, &p, &b);
    return h;
}

uint32_t gl_get_framebuffer_pitch(void) {
    uint32_t w, h, p, b;
    framebuffer_get_info(&w, &h, &p, &b);
    return p;
}

uint32_t gl_get_framebuffer_bpp(void) {
    uint32_t w, h, p, b;
    framebuffer_get_info(&w, &h, &p, &b);
    return b;
}
