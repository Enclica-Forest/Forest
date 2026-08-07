/*
 * Fern - Cross-Architecture Kernel Heap Interface
 * kheap.h
 *
 * Defines the portable kernel heap API and per-architecture heap layout
 * constants.  Every architecture maps the heap at a fixed virtual range
 * backed by PMM frames.
 *
 * Supported architectures:
 *   x86_64  - 0xFFFF800000000000, 256 MB
 *   AArch64 - 0xFFFFFF8000000000, 256 MB
 *   ARM32   - 0xC0000000,          64 MB
 *   RISC-V  - 0xFFFFFFC040000000, 256 MB
 */

#ifndef FOREST_KHEAP_H
#define FOREST_KHEAP_H

#include "arch.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * 1. Per-Architecture Heap Layout
 * ========================================================================= */

#if ARCH_X86_64
#   define KHEAP_START       0xFFFF800000000000ULL
#   define KHEAP_MAX_SIZE    (256U * 1024U * 1024U)   /* 256 MB */
#   define KHEAP_INITIAL_SIZE (16U * 1024U * 1024U)   /* 16 MB */

#elif ARCH_X86_32
#   define KHEAP_START       0xC0000000UL
#   define KHEAP_MAX_SIZE    (64U * 1024U * 1024U)    /* 64 MB */
#   define KHEAP_INITIAL_SIZE (16U * 1024U * 1024U)   /* 16 MB */

#elif ARCH_ARM64
#   define KHEAP_START       0xFFFFFF8000000000ULL
#   define KHEAP_MAX_SIZE    (256U * 1024U * 1024U)   /* 256 MB */
#   define KHEAP_INITIAL_SIZE (16U * 1024U * 1024U)   /* 16 MB */

#elif ARCH_ARM32
#   define KHEAP_START       0xC0000000UL
#   define KHEAP_MAX_SIZE    (64U * 1024U * 1024U)    /* 64 MB */
#   define KHEAP_INITIAL_SIZE (16U * 1024U * 1024U)   /* 16 MB */

#elif ARCH_RISCV64
#   define KHEAP_START       0xFFFFFFC040000000ULL
#   define KHEAP_MAX_SIZE    (256U * 1024U * 1024U)   /* 256 MB */
#   define KHEAP_INITIAL_SIZE (16U * 1024U * 1024U)   /* 16 MB */

#else
#   error "kheap.h: unsupported architecture"
#endif

/* Page-aligned heap start (compile-time check) */
#if (KHEAP_START & 0xFFF) != 0
#   error "kheap.h: KHEAP_START must be page-aligned"
#endif

/* =========================================================================
 * 2. Memory Result Type (if not already defined via memory.h)
 * ========================================================================= */

#ifndef FOREST_MEMORY_RESULT_DEFINED
#define FOREST_MEMORY_RESULT_DEFINED
typedef enum {
    MEMORY_OK = 0,
    MEMORY_ERROR_OUT_OF_MEMORY,
    MEMORY_ERROR_INVALID_ADDR,
    MEMORY_ERROR_INVALID_SIZE,
    MEMORY_ERROR_ALREADY_MAPPED,
    MEMORY_ERROR_NOT_MAPPED,
    MEMORY_ERROR_PERMISSION,
    MEMORY_ERROR_ALIGNMENT,
    MEMORY_ERROR_UNKNOWN
} memory_result_t;
#endif

/* =========================================================================
 * 3. Kernel Heap API
 * ========================================================================= */

/**
 * kheap_init - Initialize the kernel heap.
 *
 * Must be called after the PMM and VMM are operational.  Sets up the
 * virtual mapping for the heap region and creates the initial free pool.
 *
 * @param start     Virtual start address of the heap (use KHEAP_START)
 * @param size      Initial size in bytes (use KHEAP_INITIAL_SIZE)
 *
 * @return MEMORY_OK on success, or an error code.
 */
memory_result_t kheap_init(uintptr_t start, size_t size);

/**
 * kmalloc - Allocate kernel memory.
 *
 * @param size  Number of bytes to allocate.
 *
 * @return Pointer to allocated memory, or NULL on failure.
 *         The returned pointer is aligned to at least 8 bytes.
 */
void *kmalloc(size_t size);

/**
 * kzalloc - Allocate zeroed kernel memory.
 *
 * Equivalent to kmalloc() followed by memset to zero.
 *
 * @param size  Number of bytes to allocate.
 *
 * @return Pointer to zeroed memory, or NULL on failure.
 */
void *kzalloc(size_t size);

/**
 * kmalloc_aligned - Allocate aligned kernel memory.
 *
 * @param size       Number of bytes to allocate.
 * @param alignment  Required alignment in bytes (must be power of 2).
 *
 * @return Pointer to aligned memory, or NULL on failure.
 */
void *kmalloc_aligned(size_t size, uint32_t alignment);

/**
 * kfree - Free kernel memory.
 *
 * @param ptr  Pointer previously returned by kmalloc/kzalloc/kmalloc_aligned.
 *             NULL is safely ignored.
 */
void kfree(void *ptr);

/**
 * krealloc - Reallocate kernel memory.
 *
 * If @p ptr is NULL, behaves like kmalloc(@p new_size).
 * If @p new_size is 0, frees @p ptr and returns NULL.
 *
 * @param ptr       Pointer previously returned by kmalloc (or NULL).
 * @param new_size  New size in bytes.
 *
 * @return Pointer to reallocated memory, or NULL on failure.
 *         The original @p ptr is always freed on success.
 */
void *krealloc(void *ptr, size_t new_size);

/**
 * kheap_get_free - Return the number of free bytes in the kernel heap.
 */
size_t kheap_get_free(void);

/**
 * kheap_check_pressure - Check if at least @p needed bytes are free.
 *
 * @return true if enough memory is available, false otherwise.
 */
bool kheap_check_pressure(size_t needed);

#endif /* FOREST_KHEAP_H */
