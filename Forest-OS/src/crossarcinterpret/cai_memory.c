/*
 * cai_memory.c - Guest memory management for the cross-architecture interpreter
 *
 * Implementation notes
 * --------------------
 * Each mapped guest region is backed by a separate kmalloc() allocation so
 * that regions can be independently freed when the address space is torn down
 * (cai_as_destroy) without needing a slab or buddy allocator for the guest
 * pool.  The region list is kept unsorted; translation walks the list linearly,
 * which is acceptable for the small number of PT_LOAD segments typical of ELF
 * binaries (usually 2-4).
 *
 * All multi-byte accesses use explicit byte-at-a-time encoding to avoid any
 * unaligned-access UB and to remain endian-correct for LE guests.
 *
 * The existing cai_context_t flat-pool functions (cai_mem_gva_to_host, etc.)
 * in the original cai_memory.c are preserved in that translation unit; this
 * file provides a complementary standalone cai_address_space_t API used by
 * the new ELF loader and syscall bridge.
 */

#include "cai_memory.h"
#include "crossarcinterpret.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/debuglog.h"

/* =========================================================================
 * Address-space lifecycle
 * ========================================================================= */

cai_address_space_t *cai_as_create(void)
{
    cai_address_space_t *as = (cai_address_space_t *)kzalloc(sizeof(*as));
    if (!as) {
        debuglog(DEBUG_ERROR, "cai_as_create: kzalloc failed\n");
        return NULL;
    }
    as->regions    = NULL;
    as->total_size = 0;
    return as;
}

void cai_as_destroy(cai_address_space_t *as)
{
    if (!as)
        return;

    cai_mem_region_t *r = as->regions;
    while (r) {
        cai_mem_region_t *next = r->next;
        if (r->host_ptr)
            kfree(r->host_ptr);
        kfree(r);
        r = next;
    }
    kfree(as);
}

/* =========================================================================
 * Region mapping
 * ========================================================================= */

int cai_as_map(cai_address_space_t *as, uint64_t guest_addr,
               size_t size, uint32_t flags)
{
    if (!as || size == 0) {
        debuglog(DEBUG_WARN, "cai_as_map: invalid arguments (as=%p size=%u)\n",
                 (void *)as, (unsigned)size);
        return -1;
    }

    /* Allocate the region descriptor */
    cai_mem_region_t *r = (cai_mem_region_t *)kzalloc(sizeof(*r));
    if (!r) {
        debuglog(DEBUG_ERROR, "cai_as_map: kzalloc for region descriptor failed\n");
        return -1;
    }

    /* Allocate the backing host memory */
    r->host_ptr = (uint8_t *)kzalloc(size);
    if (!r->host_ptr) {
        debuglog(DEBUG_ERROR,
                 "cai_as_map: kzalloc for host backing (%u bytes) failed\n",
                 (unsigned)size);
        kfree(r);
        return -1;
    }

    r->guest_base = guest_addr;
    r->size       = size;
    r->flags      = flags;

    /* Prepend to the region list (order does not matter for lookup) */
    r->next      = as->regions;
    as->regions  = r;
    as->total_size += size;

    debuglog(DEBUG_DETAIL,
             "cai_as_map: gva=0x%llx size=0x%x flags=0x%x host=%p\n",
             (unsigned long long)guest_addr, (unsigned)size, flags,
             (void *)r->host_ptr);

    return 0;
}

/* =========================================================================
 * Address translation
 * ========================================================================= */

void *cai_as_translate(cai_address_space_t *as, uint64_t guest_addr,
                       size_t access_size)
{
    if (!as || access_size == 0)
        return NULL;

    for (cai_mem_region_t *r = as->regions; r != NULL; r = r->next) {
        if (guest_addr >= r->guest_base) {
            uint64_t offset = guest_addr - r->guest_base;
            /* Ensure the entire access fits within this region */
            if (offset <= r->size - access_size) {
                return (void *)(r->host_ptr + offset);
            }
        }
    }

    debuglog(DEBUG_WARN,
             "cai_as_translate: gva=0x%llx size=%u not mapped\n",
             (unsigned long long)guest_addr, (unsigned)access_size);
    return NULL;
}

/* =========================================================================
 * Typed read primitives – little-endian, byte-granularity, bounds-safe
 * ========================================================================= */

uint8_t cai_mem_r8(cai_address_space_t *as, uint64_t addr)
{
    const uint8_t *p = (const uint8_t *)cai_as_translate(as, addr, 1);
    if (!p) {
        debuglog(DEBUG_WARN, "cai_mem_r8: fault at gva=0x%llx\n",
                 (unsigned long long)addr);
        return 0;
    }
    return p[0];
}

uint16_t cai_mem_r16(cai_address_space_t *as, uint64_t addr)
{
    const uint8_t *p = (const uint8_t *)cai_as_translate(as, addr, 2);
    if (!p) {
        debuglog(DEBUG_WARN, "cai_mem_r16: fault at gva=0x%llx\n",
                 (unsigned long long)addr);
        return 0;
    }
    return (uint16_t)p[0] |
           ((uint16_t)p[1] << 8);
}

uint32_t cai_mem_r32(cai_address_space_t *as, uint64_t addr)
{
    const uint8_t *p = (const uint8_t *)cai_as_translate(as, addr, 4);
    if (!p) {
        debuglog(DEBUG_WARN, "cai_mem_r32: fault at gva=0x%llx\n",
                 (unsigned long long)addr);
        return 0;
    }
    return (uint32_t)p[0]         |
           ((uint32_t)p[1] << 8)  |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint64_t cai_mem_r64(cai_address_space_t *as, uint64_t addr)
{
    const uint8_t *p = (const uint8_t *)cai_as_translate(as, addr, 8);
    if (!p) {
        debuglog(DEBUG_WARN, "cai_mem_r64: fault at gva=0x%llx\n",
                 (unsigned long long)addr);
        return 0;
    }
    return (uint64_t)p[0]         |
           ((uint64_t)p[1] << 8)  |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

/* =========================================================================
 * Typed write primitives – little-endian, byte-granularity, bounds-safe
 * ========================================================================= */

void cai_mem_w8(cai_address_space_t *as, uint64_t addr, uint8_t v)
{
    uint8_t *p = (uint8_t *)cai_as_translate(as, addr, 1);
    if (!p) {
        debuglog(DEBUG_WARN, "cai_mem_w8: fault at gva=0x%llx\n",
                 (unsigned long long)addr);
        return;
    }
    p[0] = v;
}

void cai_mem_w16(cai_address_space_t *as, uint64_t addr, uint16_t v)
{
    uint8_t *p = (uint8_t *)cai_as_translate(as, addr, 2);
    if (!p) {
        debuglog(DEBUG_WARN, "cai_mem_w16: fault at gva=0x%llx\n",
                 (unsigned long long)addr);
        return;
    }
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

void cai_mem_w32(cai_address_space_t *as, uint64_t addr, uint32_t v)
{
    uint8_t *p = (uint8_t *)cai_as_translate(as, addr, 4);
    if (!p) {
        debuglog(DEBUG_WARN, "cai_mem_w32: fault at gva=0x%llx\n",
                 (unsigned long long)addr);
        return;
    }
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void cai_mem_w64(cai_address_space_t *as, uint64_t addr, uint64_t v)
{
    uint8_t *p = (uint8_t *)cai_as_translate(as, addr, 8);
    if (!p) {
        debuglog(DEBUG_WARN, "cai_mem_w64: fault at gva=0x%llx\n",
                 (unsigned long long)addr);
        return;
    }
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 32);
    p[5] = (uint8_t)(v >> 40);
    p[6] = (uint8_t)(v >> 48);
    p[7] = (uint8_t)(v >> 56);
}

/* =========================================================================
 * Region registration for cai_context_t (used by ELF loader)
 *
 * Adds a region to the context's fixed-size region table so that
 * cai_mem_gva_to_host() can serve it.  This is the companion to cai_as_map()
 * for the flat-pool ctx-based memory API declared in crossarcinterpret.h.
 * ========================================================================= */

int cai_mem_add_region(cai_context_t *ctx, uint64_t gva_base,
                       uint8_t *host_ptr, size_t size, uint32_t flags)
{
    if (!ctx || !host_ptr || size == 0)
        return CAI_EINVAL;
    if (ctx->n_regions >= CAI_MAX_MEM_REGIONS)
        return CAI_ENOMEM;

    cai_mem_region_t *r = &ctx->regions[ctx->n_regions++];
    r->gva_base = gva_base;
    r->host_ptr = host_ptr;
    r->size     = size;
    r->flags    = flags;

    debuglog(DEBUG_DETAIL,
             "cai_mem_add_region: gva=0x%llx size=0x%x flags=0x%x host=%p\n",
             (unsigned long long)gva_base, (unsigned)size, flags,
             (void *)host_ptr);
    return CAI_OK;
}

/* =========================================================================
 * cai_context_t flat-pool memory API
 *
 * These functions operate on the region table embedded in cai_context_t.
 * The architecture-specific step functions use these for all guest memory
 * accesses (instruction fetch, load, store, stack push/pop).
 * ========================================================================= */

uint8_t *cai_mem_gva_to_host(cai_context_t *ctx, uint64_t gva, size_t len,
                              uint32_t access_flags)
{
    if (!ctx || len == 0)
        return NULL;
    for (int i = 0; i < ctx->n_regions; i++) {
        cai_mem_region_t *r = &ctx->regions[i];
        if (gva >= r->gva_base &&
            (gva + (uint64_t)len) <= (r->gva_base + (uint64_t)r->size)) {
            if ((r->flags & access_flags) != access_flags)
                return NULL;
            return r->host_ptr + (gva - r->gva_base);
        }
    }
    return NULL;
}

/* ---- Read helpers ---- */

int cai_mem_read8(cai_context_t *ctx, uint64_t gva, uint8_t *out)
{
    uint8_t *p = cai_mem_gva_to_host(ctx, gva, 1, CAI_MEM_READ);
    if (!p) return CAI_EFAULT;
    *out = *p;
    return CAI_OK;
}

int cai_mem_read16(cai_context_t *ctx, uint64_t gva, uint16_t *out)
{
    uint8_t *p = cai_mem_gva_to_host(ctx, gva, 2, CAI_MEM_READ);
    if (!p) return CAI_EFAULT;
    *out = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    return CAI_OK;
}

int cai_mem_read32(cai_context_t *ctx, uint64_t gva, uint32_t *out)
{
    uint8_t *p = cai_mem_gva_to_host(ctx, gva, 4, CAI_MEM_READ);
    if (!p) return CAI_EFAULT;
    *out = (uint32_t)p[0]         |
           ((uint32_t)p[1] <<  8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
    return CAI_OK;
}

int cai_mem_read64(cai_context_t *ctx, uint64_t gva, uint64_t *out)
{
    uint8_t *p = cai_mem_gva_to_host(ctx, gva, 8, CAI_MEM_READ);
    if (!p) return CAI_EFAULT;
    *out = (uint64_t)p[0]         |
           ((uint64_t)p[1] <<  8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
    return CAI_OK;
}

/* ---- Write helpers ---- */

int cai_mem_write8(cai_context_t *ctx, uint64_t gva, uint8_t val)
{
    uint8_t *p = cai_mem_gva_to_host(ctx, gva, 1, CAI_MEM_WRITE);
    if (!p) return CAI_EFAULT;
    *p = val;
    return CAI_OK;
}

int cai_mem_write16(cai_context_t *ctx, uint64_t gva, uint16_t val)
{
    uint8_t *p = cai_mem_gva_to_host(ctx, gva, 2, CAI_MEM_WRITE);
    if (!p) return CAI_EFAULT;
    p[0] = (uint8_t)(val);
    p[1] = (uint8_t)(val >> 8);
    return CAI_OK;
}

int cai_mem_write32(cai_context_t *ctx, uint64_t gva, uint32_t val)
{
    uint8_t *p = cai_mem_gva_to_host(ctx, gva, 4, CAI_MEM_WRITE);
    if (!p) return CAI_EFAULT;
    p[0] = (uint8_t)(val);
    p[1] = (uint8_t)(val >>  8);
    p[2] = (uint8_t)(val >> 16);
    p[3] = (uint8_t)(val >> 24);
    return CAI_OK;
}

int cai_mem_write64(cai_context_t *ctx, uint64_t gva, uint64_t val)
{
    uint8_t *p = cai_mem_gva_to_host(ctx, gva, 8, CAI_MEM_WRITE);
    if (!p) return CAI_EFAULT;
    p[0] = (uint8_t)(val);
    p[1] = (uint8_t)(val >>  8);
    p[2] = (uint8_t)(val >> 16);
    p[3] = (uint8_t)(val >> 24);
    p[4] = (uint8_t)(val >> 32);
    p[5] = (uint8_t)(val >> 40);
    p[6] = (uint8_t)(val >> 48);
    p[7] = (uint8_t)(val >> 56);
    return CAI_OK;
}

/* ---- Stack push/pop helpers ---- */

int cai_stack_push32(cai_context_t *ctx, uint32_t val)
{
    switch (ctx->target_arch) {
    case CAI_ARCH_X86_32:
        ctx->cpu.x86_32.esp -= 4;
        return cai_mem_write32(ctx, (uint64_t)ctx->cpu.x86_32.esp, val);
    case CAI_ARCH_ARM32:
        ctx->cpu.arm32.r[13] -= 4;
        return cai_mem_write32(ctx, (uint64_t)ctx->cpu.arm32.r[13], val);
    default:
        return CAI_ENOTSUP;
    }
}

int cai_stack_pop32(cai_context_t *ctx, uint32_t *out)
{
    int rc;
    switch (ctx->target_arch) {
    case CAI_ARCH_X86_32:
        rc = cai_mem_read32(ctx, (uint64_t)ctx->cpu.x86_32.esp, out);
        if (rc == CAI_OK) ctx->cpu.x86_32.esp += 4;
        return rc;
    case CAI_ARCH_ARM32:
        rc = cai_mem_read32(ctx, (uint64_t)ctx->cpu.arm32.r[13], out);
        if (rc == CAI_OK) ctx->cpu.arm32.r[13] += 4;
        return rc;
    default:
        return CAI_ENOTSUP;
    }
}

int cai_stack_push64(cai_context_t *ctx, uint64_t val)
{
    switch (ctx->target_arch) {
    case CAI_ARCH_X86_64:
        ctx->cpu.x86_64.rsp -= 8;
        return cai_mem_write64(ctx, ctx->cpu.x86_64.rsp, val);
    case CAI_ARCH_AARCH64:
        ctx->cpu.aarch64.sp -= 8;
        return cai_mem_write64(ctx, ctx->cpu.aarch64.sp, val);
    default:
        return CAI_ENOTSUP;
    }
}

int cai_stack_pop64(cai_context_t *ctx, uint64_t *out)
{
    int rc;
    switch (ctx->target_arch) {
    case CAI_ARCH_X86_64:
        rc = cai_mem_read64(ctx, ctx->cpu.x86_64.rsp, out);
        if (rc == CAI_OK) ctx->cpu.x86_64.rsp += 8;
        return rc;
    case CAI_ARCH_AARCH64:
        rc = cai_mem_read64(ctx, ctx->cpu.aarch64.sp, out);
        if (rc == CAI_OK) ctx->cpu.aarch64.sp += 8;
        return rc;
    default:
        return CAI_ENOTSUP;
    }
}
