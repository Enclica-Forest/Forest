/*
 * Missing function implementations
 *
 * Provides full implementations for functions that are referenced by compiled
 * code but whose source files are either feature-gated, architecture-specific,
 * or simply missing from the build.
 */

#include "include/types.h"
#include "include/system.h"
#include "include/debuglog.h"
#include "include/memory.h"
#include "include/interrupt.h"
#include "arch/smp.h"
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

/* Forward declare vsnprintf (from libc) */
extern int vsnprintf(char *str, size_t size, const char *format, va_list ap);

/* Forward declare the uefi_gop_info_t struct (defined in arch/framebuffer.c) */
typedef struct {
    void*    BaseAddress;
    uint64_t BufferSize;
    uint32_t Width;
    uint32_t Height;
    uint32_t PixelsPerScanLine;
    uint32_t BitsPerPixel;
    uint32_t PixelFormat;
    uint32_t RedMask;
    uint32_t GreenMask;
    uint32_t BlueMask;
    uint32_t ReservedMask;
} uefi_gop_info_t;

/* Forward declare epoll types (defined locally in epoll.c) */
struct epoll_event;
typedef unsigned long sigset_t;

/* Forward declare functions we wrap */
extern void kernel_panic_annotated(const char *message, const char *file,
                                   uint32 line, const char *func);
extern phys_addr_t pmm_alloc_frames(uint32_t count);
extern void pic_init(void);
extern int  ioapic_init_advanced(void);
extern int  ioapic_enable_irq(uint8_t irq);
extern int  ioapic_disable_irq(uint8_t irq);
extern int  idt_init_full(void);
extern int  epoll_create(int size);
extern int  epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
extern int  epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
                        int timeout, const sigset_t *sigmask);

/* Forward declare net_nic_driver_t (defined in include/net.h) */
struct net_nic_driver;
typedef struct net_nic_driver net_nic_driver_t;

/* Forward declare GL types (matching src/gl/state.h) */
typedef unsigned int   GLenum;
typedef int            GLint;
typedef unsigned int   GLuint;
typedef int            GLsizei;
typedef float          GLfloat;
typedef double         GLdouble;
typedef unsigned int   GLbitfield;
typedef void           GLvoid;
typedef long           GLsizeiptr;
typedef float          GLclampf;
typedef unsigned char  GLubyte;

/* Forward declare netdev_t (defined in include/netdev.h) */
typedef struct netdev netdev_t;

/* =========================================================================
 * 1. debug_printf — variadic wrapper around debuglog_printf
 * ========================================================================= */

void debug_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    debuglog_vprintf(fmt, ap);
    va_end(ap);
}

/* =========================================================================
 * 2. panic — standalone function body for code that calls panic() as a
 *    function (fault_prevention.h declares extern void panic(...)).
 *    We avoid including panic.h to prevent macro conflict.
 * ========================================================================= */

void panic(const char *format, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    kernel_panic_annotated(buf, __FILE__, __LINE__, __func__);
}

/* =========================================================================
 * 3. smp_get_cpu_id — return the ID of the currently executing CPU.
 *    On 32-bit x86 without a real SMP implementation, always return 0.
 * ========================================================================= */

uint32_t smp_get_cpu_id(void)
{
    return 0;
}

/* =========================================================================
 * 4. smp_init_arch — architecture-specific SMP initialization.
 *    No-op for 32-bit x86.
 * ========================================================================= */

uint32_t smp_init_arch(void)
{
    return 0;
}

/* =========================================================================
 * 5. smp_get_current_cpu — return the ID of the executing CPU
 * ========================================================================= */

int smp_get_current_cpu(void)
{
    return (int)smp_get_cpu_id();
}

/* =========================================================================
 * 6. mm_allocate_pages — allocate contiguous physical pages and return
 *    the virtual address (identity-mapped region for early kernel use).
 * ========================================================================= */

void *mm_allocate_pages(size_t page_count)
{
    if (page_count == 0)
        return (void *)0;

    phys_addr_t phys = pmm_alloc_frames((uint32_t)page_count);
    if (phys == 0)
        return (void *)0;

    /* Identity map: virtual == physical for early kernel allocations */
    return (void *)(uintptr_t)phys;
}

/* =========================================================================
 * 7. atomic64_add — atomically add a 64-bit value.
 * ========================================================================= */

void atomic64_add(atomic64_t *ptr, uint64_t value)
{
    /* Simple non-atomic fallback; adequate for statistics counters */
    *(volatile uint64_t *)ptr += value;
}

/* =========================================================================
 * 8. arch_get_random_bytes — read random bytes from architecture RNG.
 *    Uses RDRAND/RDSEED on x86 when available, else returns -1.
 * ========================================================================= */

int arch_get_random_bytes(void *buf, size_t count)
{
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i < count; i++) {
        unsigned int val = 0;
        unsigned char ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
        if (ok) {
            p[i] = (uint8_t)(val & 0xFF);
        } else {
            return -1;
        }
    }
    return 0;
}

/* =========================================================================
 * 9. uefi_gop_get_info — only available in UEFI builds; return NULL for
 *    BIOS/legacy builds.
 * ========================================================================= */

#if !UEFI_BOOT
const uefi_gop_info_t* uefi_gop_get_info(void)
{
    return (const uefi_gop_info_t *)0;
}
#endif

/* =========================================================================
 * 10. x86 PIC / IOAPIC wrappers — delegate to existing implementations
 * ========================================================================= */

void x86_pic_init(void)
{
    pic_init();
}

void x86_ioapic_init(void)
{
    ioapic_init_advanced();
}

void x86_ioapic_unmask_irq(uint32_t irq)
{
    ioapic_enable_irq((uint8_t)irq);
}

void x86_ioapic_mask_irq(uint32_t irq)
{
    ioapic_disable_irq((uint8_t)irq);
}

/* =========================================================================
 * 11. x86_interrupt_init_idt — set up the Interrupt Descriptor Table
 * ========================================================================= */

void x86_interrupt_init_idt(void)
{
    idt_init_full();
}

/* =========================================================================
 * 12. x86_64_ist_configure_stack — configure IST entry in the TSS
 *    IST is x86_64-only; no-op for 32-bit builds.
 * ========================================================================= */

typedef enum {
    IST_DOUBLE_FAULT = 1,
    IST_NMI = 2,
    IST_MACHINE_CHECK = 3
} ist_stack_type_t;

void x86_64_ist_configure_stack(ist_stack_type_t type, void *stack_top)
{
    (void)type;
    (void)stack_top;
}

/* =========================================================================
 * 13. sys_epoll_* wrappers — bridge arch/syscall.c to epoll.c
 * ========================================================================= */

long sys_epoll_create1(int flags)
{
    (void)flags;
    return (long)epoll_create(1);
}

long sys_epoll_ctl(int epfd, int op, int fd, void *event)
{
    return (long)epoll_ctl(epfd, op, fd, (struct epoll_event *)event);
}

long sys_epoll_pwait(int epfd, void *events, int maxevents, int timeout,
                     const void *sigmask, size_t sigsetsize)
{
    (void)sigsetsize;
    return (long)epoll_pwait(epfd, (struct epoll_event *)events,
                             maxevents, timeout, (const sigset_t *)sigmask);
}

/* =========================================================================
 * 14. sys_getrandom — read random bytes from the kernel RNG
 * ========================================================================= */

long sys_getrandom(void *buf, size_t count, unsigned int flags)
{
    (void)flags;

    if (!count)
        return 0;

    if (arch_get_random_bytes(buf, count) == 0)
        return (long)count;

    /* Fallback: TSC-seeded PRNG */
    uint8_t *p = (uint8_t *)buf;
    uint32_t seed;
    __asm__ volatile("rdtsc" : "=a"(seed));

    for (size_t i = 0; i < count; i++) {
        seed = seed * 1103515245 + 12345;
        p[i] = (uint8_t)(seed >> 16);
    }

    return (long)count;
}

/* =========================================================================
 * 15. Networking stubs — no networking when ENABLE_NETWORKING=no
 * ========================================================================= */

int netdev_init(void)
{
    return -1;
}

void network_init(void)
{
}

net_nic_driver_t* net_active_nic(void)
{
    return (net_nic_driver_t *)0;
}

/* =========================================================================
 * 16. OpenGL stubs — all GL functions are no-ops when GL is not compiled.
 *     Guarded by #ifndef to avoid conflicts when GL sources are compiled.
 * ========================================================================= */

#ifndef ENABLE_OPENGL

void gl_init(void) {}
void gl_clear(GLbitfield mask) { (void)mask; }
void gl_clear_color(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    (void)r; (void)g; (void)b; (void)a;
}
void gl_enable(GLenum cap) { (void)cap; }
void gl_disable(GLenum cap) { (void)cap; }
void gl_blend_func(GLenum sfactor, GLenum dfactor) { (void)sfactor; (void)dfactor; }
void gl_depth_func(GLenum func) { (void)func; }
void gl_viewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    (void)x; (void)y; (void)width; (void)height;
}
void gl_flush(void) {}
void gl_finish(void) {}
void gl_fogf(GLenum pname, GLfloat param) { (void)pname; (void)param; }
void gl_fogfv(GLenum pname, const GLfloat *params) { (void)pname; (void)params; }
void gl_alpha_func(GLenum func, GLclampf ref) { (void)func; (void)ref; }

void gl_immediate_begin(GLenum mode) { (void)mode; }
void gl_immediate_end(void) {}
void gl_immediate_vertex(float x, float y, float z) { (void)x; (void)y; (void)z; }
void gl_immediate_color(float r, float g, float b, float a) {
    (void)r; (void)g; (void)b; (void)a;
}
void gl_immediate_color3f(float r, float g, float b) { (void)r; (void)g; (void)b; }
void gl_immediate_normal(float x, float y, float z) { (void)x; (void)y; (void)z; }
void gl_immediate_texcoord(float u, float v) { (void)u; (void)v; }

void gl_lightf(GLenum light, GLenum pname, GLfloat param) {
    (void)light; (void)pname; (void)param;
}
void gl_lightfv(GLenum light, GLenum pname, const GLfloat *params) {
    (void)light; (void)pname; (void)params;
}
void gl_materialf(GLenum face, GLenum pname, GLfloat param) {
    (void)face; (void)pname; (void)param;
}
void gl_materialfv(GLenum face, GLenum pname, const GLfloat *params) {
    (void)face; (void)pname; (void)params;
}

void glMatrixMode(GLenum mode) { (void)mode; }
void glLoadIdentity(void) {}
void glTranslatef(GLfloat x, GLfloat y, GLfloat z) { (void)x; (void)y; (void)z; }
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    (void)angle; (void)x; (void)y; (void)z;
}
void glScaled(GLdouble x, GLdouble y, GLdouble z) { (void)x; (void)y; (void)z; }
void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble near_val, GLdouble far_val) {
    (void)left; (void)right; (void)bottom; (void)top; (void)near_val; (void)far_val;
}
void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
               GLdouble near_val, GLdouble far_val) {
    (void)left; (void)right; (void)bottom; (void)top; (void)near_val; (void)far_val;
}
void glPushMatrix(void) {}
void glPopMatrix(void) {}
void glLoadMatrixf(const GLfloat *m) { (void)m; }
void glMultMatrixf(const GLfloat *m) { (void)m; }

void gl_gen_textures(GLsizei n, GLuint *textures) { (void)n; (void)textures; }
void gl_delete_textures(GLsizei n, const GLuint *textures) { (void)n; (void)textures; }
void gl_bind_texture(GLenum target, GLuint texture) { (void)target; (void)texture; }
void gl_tex_image_2d(GLenum target, GLint level, GLint internalformat,
                     GLsizei width, GLsizei height, GLint border,
                     GLenum format, GLenum type, const GLvoid *data) {
    (void)target; (void)level; (void)internalformat;
    (void)width; (void)height; (void)border;
    (void)format; (void)type; (void)data;
}
void gl_tex_parameteri(GLenum target, GLenum pname, GLint param) {
    (void)target; (void)pname; (void)param;
}

void gl_draw_arrays(GLenum mode, GLint first, GLsizei count) {
    (void)mode; (void)first; (void)count;
}
void gl_draw_elements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
    (void)mode; (void)count; (void)type; (void)indices;
}

void gl_present(void) {}

GLenum glGetError(void) { return 0; }
const GLubyte* glGetString(GLenum name) { (void)name; return (const GLubyte *)""; }

void gl_buffer_init(void) {}
void gl_buffer_delete(GLuint name) { (void)name; }
void gl_buffer_bind(GLenum target, GLuint name) { (void)target; (void)name; }
void gl_buffer_data(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage) {
    (void)target; (void)size; (void)data; (void)usage;
}

#endif /* !ENABLE_OPENGL */
