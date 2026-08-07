/*
 * Fern - Cross-Architecture DMA Mapping Interface
 * dma.h
 *
 * Provides a unified DMA API for device drivers.  On architectures with
 * hardware-coherent DMA (x86, RISC-V on QEMU) the operations are no-ops.
 * On ARM/AArch64, proper cache maintenance is performed.
 *
 * All current targets use 32-bit or identity-mapped addressing, so DMA
 * addresses are physical addresses passed directly to devices.
 *
 * Supported architectures:
 *   x86_32/x86_64  - Cache-coherent DMA (no-ops)
 *   ARM32 (v7)     - CP15 cache clean/invalidate
 *   AArch64 (v8)   - DC CIVAC/DC CVAC instructions
 *   RISC-V 64      - Typically cache-coherent on QEMU (no-ops)
 */

#ifndef FOREST_ARCH_DMA_H
#define FOREST_ARCH_DMA_H

#include "arch.h"
#include "barrier.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * DMA transfer direction
 * ========================================================================= */

typedef enum {
    DMA_BIDIRECTIONAL = 0,  /* Memory-to-device and device-to-memory */
    DMA_TO_DEVICE     = 1,  /* Memory -> Device (clean caches) */
    DMA_FROM_DEVICE   = 2,  /* Device -> Memory (invalidate caches) */
} dma_dir_t;

/* =========================================================================
 * DMA allocation flags
 * ========================================================================= */

#define DMA_COHERENT    (1U << 0)  /* Allocate cache-coherent memory */

/* =========================================================================
 * DMA address type
 *
 * On all current targets this is the same as a physical address.
 * The typedef exists so drivers are not hard-coupled to phys addr width.
 * ========================================================================= */

typedef uint32_t dma_addr_t;

/* =========================================================================
 * DMA memory allocation
 * ========================================================================= */

/**
 * dma_alloc - Allocate DMA-capable memory.
 *
 * @size:       Number of bytes to allocate (rounded up to page boundary).
 * @coherent:   If true, memory is cache-coherent (no sync needed).
 *              If false, caller must use dma_sync_* before/after DMA.
 *
 * Returns a pointer to the allocated memory (virtual address), or NULL
 * on failure.  The returned memory is page-aligned and zeroed.
 *
 * The physical address for DMA programming is the same as the virtual
 * address on all current targets (identity mapping).
 */
void *dma_alloc(size_t size, bool coherent);

/**
 * dma_free - Free DMA memory previously allocated with dma_alloc().
 *
 * @vaddr:  Virtual address returned by dma_alloc().
 * @size:   Original size passed to dma_alloc().
 */
void dma_free(void *vaddr, size_t size);

/* =========================================================================
 * DMA mapping (physical address -> DMA address)
 * ========================================================================= */

/**
 * dma_map - Map a physical address range for DMA access.
 *
 * On identity-mapped targets this is a no-op (phys == DMA address).
 * Provided for API completeness and future non-identity-mapped support.
 *
 * @phys:       Physical address of the buffer.
 * @size:       Size of the buffer in bytes.
 * @direction:  Transfer direction (affects cache maintenance).
 *
 * Returns the DMA address to program into the device.
 */
dma_addr_t dma_map(dma_addr_t phys, size_t size, dma_dir_t direction);

/**
 * dma_unmap - Unmap a previously mapped DMA address range.
 *
 * @virt:       Virtual address of the buffer (for cache ops on ARM).
 * @size:       Size of the buffer in bytes.
 * @direction:  Transfer direction.
 */
void dma_unmap(void *virt, size_t size, dma_dir_t direction);

/* =========================================================================
 * DMA synchronisation (cache maintenance)
 *
 * Must be called when the CPU and device share memory and the region
 * is not allocated with DMA_COHERENT.
 *
 *   Before device reads (DMA_FROM_DEVICE):  dma_sync_for_device()
 *   After device writes (DMA_FROM_DEVICE):  dma_sync_for_cpu()
 *   Before device writes (DMA_TO_DEVICE):   dma_sync_for_device()
 *   After device reads (DMA_TO_DEVICE):     dma_sync_for_cpu()
 *   Bidirectional:                          both before and after.
 * ========================================================================= */

/**
 * dma_sync_for_device - Flush CPU caches so the device sees current data.
 *
 * Cleans (writes back) dirty cache lines to main memory so the device
 * can read the latest data.
 *
 * @phys:   Physical address of the buffer.
 * @size:   Size of the buffer in bytes.
 */
void dma_sync_for_device(dma_addr_t phys, size_t size);

/**
 * dma_sync_for_cpu - Invalidate CPU caches so the CPU sees device-written data.
 *
 * Invalidates cache lines so the CPU reads fresh data from main memory
 * (written by the device).
 *
 * @phys:   Physical address of the buffer.
 * @size:   Size of the buffer in bytes.
 */
void dma_sync_for_cpu(dma_addr_t phys, size_t size);

#endif /* FOREST_ARCH_DMA_H */
