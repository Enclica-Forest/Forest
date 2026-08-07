/*
 * Fern - Cross-Architecture Kernel Heap Wrapper
 * kheap.c
 *
 * Provides the portable kheap_init() / kmalloc() / kfree() / krealloc()
 * API declared in kheap.h.  The heavy lifting (block allocator, VMM
 * mapping, PMM backing) lives in the original kheap.c; this file is
 * a thin, architecture-agnostic glue layer.
 *
 * Build: compiled for every architecture; links against kheap.o.
 */

#include "include/memory.h"
#include "include/string.h"
#include "include/spinlock.h"
#include "include/debuglog.h"
#include "kheap.h"

/* =========================================================================
 * 1. Declarations from the original kheap.c (architecture-specific
 *    implementations live there; they use uint32 internally which is
 *    fine on 32-bit archs and on x86_64 where kernel VA fits in 32 bits).
 * ========================================================================= */

/* Existing public symbols from kheap.c */
extern memory_result_t heap_init(uint32_t start_addr, uint32_t initial_size);
extern void *kmalloc(size_t size);
extern void *kzalloc(size_t size);
extern void *kmalloc_aligned(size_t size, uint32_t alignment);
extern void  kfree(void *ptr);
extern void *krealloc(void *ptr, size_t new_size);
extern void  heap_get_stats(uint32_t *total_size, uint32_t *used_size, uint32_t *free_size);
extern uint32_t kheap_get_free_memory(void);

/* =========================================================================
 * 2. Cross-architecture kheap_init
 * ========================================================================= */

memory_result_t kheap_init(uintptr_t start, size_t size)
{
    /* Validate parameters */
    if (start == 0 || (start & 0xFFF) != 0) {
        return MEMORY_ERROR_INVALID_ADDR;
    }
    if (size == 0) {
        size = KHEAP_INITIAL_SIZE;
    }

    /* Ensure initial size is page-aligned */
    size = (size + 0xFFF) & ~(size_t)0xFFF;

    /* Clamp to maximum */
    if (size > KHEAP_MAX_SIZE) {
        size = KHEAP_MAX_SIZE;
    }

    debuglog(DEBUG_INFO, "[KHEAP] kheap_init: start=0x%lx size=0x%lx max=0x%lx\n",
             (unsigned long)start, (unsigned long)size, (unsigned long)KHEAP_MAX_SIZE);

    /*
     * The original heap_init() takes uint32 arguments.  On architectures
     * where uintptr_t is wider than 32 bits, we rely on the caller
     * (memory_init) to ensure the heap falls within the low 4 GB of
     * virtual address space (as it does for all current arch targets).
     * If KHEAP_START is above 4 GB, the underlying VMM must be updated
     * to support 64-bit virtual addresses first.
     */
    if (start > 0xFFFFFFFFUL || size > 0xFFFFFFFFUL) {
        debuglog(DEBUG_ERROR,
                 "[KHEAP] ERROR: heap address 0x%lx or size 0x%lx exceeds 32-bit range\n",
                 (unsigned long)start, (unsigned long)size);
        return MEMORY_ERROR_INVALID_ADDR;
    }

    return heap_init((uint32_t)start, (uint32_t)size);
}

/* =========================================================================
 * 3. Convenience: initialise with compile-time arch defaults
 * ========================================================================= */

memory_result_t kheap_init_default(void)
{
    return kheap_init(KHEAP_START, KHEAP_INITIAL_SIZE);
}

/* =========================================================================
 * 4. Query helpers
 * ========================================================================= */

size_t kheap_get_free(void)
{
    return (size_t)kheap_get_free_memory();
}

bool kheap_check_pressure(size_t needed)
{
    return kheap_get_free() >= needed;
}
