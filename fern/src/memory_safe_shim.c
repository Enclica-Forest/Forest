/*
 * Memory safe shim - bridges memory_safe.h API to actual implementations
 *
 * This file provides the memory_heap_* functions that the memory_safe.h
 * macros (kmalloc/kfree) expand to. We call the actual implementations
 * from kheap.c via function pointers to avoid macro expansion issues.
 */

#include "include/memory.h"
#include "include/timer.h"
#include "include/types.h"

/* Declare the actual heap functions from kheap.c */
extern void* kmalloc(size_t size);
extern void* kmalloc_aligned(size_t size, uint32 alignment);
extern void kfree(void* ptr);

/* Save function pointers before macros potentially override them */
static void* (*const impl_kmalloc)(size_t) = kmalloc;
static void* (*const impl_kmalloc_aligned)(size_t, uint32) = kmalloc_aligned;
static void (*const impl_kfree)(void*) = kfree;

/* Now include memory_safe.h */
#include "include/memory_safe.h"

extern uint64_t tsc_frequency_hz;

static memory_validation_result_t convert_memory_result(memory_result_t result) {
    switch (result) {
        case MEMORY_OK: return MEMORY_VALIDATION_SUCCESS;
        case MEMORY_ERROR_NULL_PTR: return MEMORY_VALIDATION_NULL_POINTER;
        case MEMORY_ERROR_INVALID_ADDR: return MEMORY_VALIDATION_OUT_OF_BOUNDS;
        case MEMORY_ERROR_OUT_OF_MEMORY: return MEMORY_VALIDATION_SIZE_OVERFLOW;
        case MEMORY_ERROR_ALREADY_MAPPED: return MEMORY_VALIDATION_INVALID_STATE_TRANSITION;
        case MEMORY_ERROR_NOT_MAPPED: return MEMORY_VALIDATION_INVALID_STATE_TRANSITION;
        case MEMORY_ERROR_INVALID_SIZE: return MEMORY_VALIDATION_SIZE_OVERFLOW;
        case MEMORY_ERROR_NOT_INITIALIZED: return MEMORY_VALIDATION_INVALID_STATE_TRANSITION;
        default: return MEMORY_VALIDATION_CORRUPTED_METADATA;
    }
}

void* memory_heap_alloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    return impl_kmalloc(size);
}

void* memory_heap_alloc_aligned(size_t size, uint32 alignment) {
    if (alignment == 0) {
        return memory_heap_alloc(size);
    }
    return impl_kmalloc_aligned(size, alignment);
}

memory_validation_result_t memory_heap_free(void* ptr) {
    if (!ptr) {
        return MEMORY_VALIDATION_NULL_POINTER;
    }
    impl_kfree(ptr);
    return MEMORY_VALIDATION_SUCCESS;
}

uint32 memory_pmm_alloc_frames(uint32 count) {
    return pmm_alloc_frames(count);
}

memory_validation_result_t memory_pmm_free_frames(uint32 frame_addr, uint32 count) {
    memory_result_t res = pmm_free_frames(frame_addr, count);
    return convert_memory_result(res);
}

memory_validation_result_t memory_vmm_map_page(memory_page_directory_t* dir,
                                               uint32 virtual_addr,
                                               uint32 physical_addr,
                                               memory_page_flags_t flags) {
    page_directory_t* target_dir = dir ? (page_directory_t*)dir : vmm_get_current_page_directory();
    if (!target_dir) {
        return MEMORY_VALIDATION_NULL_POINTER;
    }
    memory_result_t res = vmm_map_page(target_dir, virtual_addr, physical_addr, (uint32)flags);
    return convert_memory_result(res);
}

uint64_t time_get_cpu_frequency(void) {
    if (tsc_frequency_hz != 0) {
        return tsc_frequency_hz;
    }
    return timer_get_frequency();
}

uint64_t time_get_uptime_ms(void) {
    uint64_t freq = timer_get_frequency();
    if (freq == 0) {
        return 0;
    }
    return (timer_get_ticks() * 1000ULL) / freq;
}
