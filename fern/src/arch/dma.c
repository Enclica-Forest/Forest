/*
 * Fern - Cross-Architecture DMA Mapping Implementation
 * dma.c
 *
 * Implements the DMA API declared in dma.h.
 *
 * Cache maintenance strategy per architecture:
 *   x86_32/x86_64  - No-op: x86 uses a coherent shared-memory model
 *                     (write-combining for uncacheable MMIO only).
 *   ARM32 (v7)     - CP15 cache operations (DCCIMVAC, DCIMVAC, DCCMVAC)
 *                     via the existing arm32 cache infrastructure.
 *   AArch64 (v8)   - DC CIVAC (clean+invalidate) / DC CVAC (clean) /
 *                     DC IVAC (invalidate) by MVA to PoC.
 *   RISC-V 64      - No-op: QEMU virt provides coherent DMA; real
 *                     hardware may need cache flush via Zicbop/Zicbom
 *                     extensions (not yet implemented).
 *
 * Memory allocation uses the PMM for physical frames and identity-maps
 * them (virt == phys on all current targets).
 */

#include "dma.h"
#include "pmm.h"
#include <string.h>

/* Cache line size (used for alignment of DMA regions) */
#if ARCH_ARM32
#   define DMA_CACHE_LINE_SIZE  32U   /* Typical Cortex-A9 L1 D-cache line */
#elif ARCH_ARM64
#   define DMA_CACHE_LINE_SIZE  64U   /* Typical Cortex-A53/A72 L1 D-cache line */
#else
#   define DMA_CACHE_LINE_SIZE  64U   /* x86/RISC-V common line size */
#endif

/* Align up to the next multiple of the given alignment */
#define DMA_ALIGN_UP(val, align)  (((val) + (align) - 1) & ~((align) - 1))

/* =========================================================================
 * Internal: round up to page size
 * ========================================================================= */

static inline size_t dma_page_align(size_t size)
{
    return DMA_ALIGN_UP(size, ARCH_PMM_PAGE_SIZE);
}

/* =========================================================================
 * DMA memory allocation
 * ========================================================================= */

void *dma_alloc(size_t size, bool coherent)
{
    (void)coherent;  /* All current targets use identity mapping */

    if (size == 0) {
        return NULL;
    }

    size_t aligned = dma_page_align(size);
    uint32_t pages = (uint32_t)(aligned / ARCH_PMM_PAGE_SIZE);

    uint32_t phys = pmm_alloc_frames(pages);
    if (phys == 0) {
        return NULL;
    }

    /* Zero the allocation */
    memset((void *)(uintptr_t)phys, 0, aligned);

    /*
     * On all current targets the kernel runs with identity mapping for
     * DMA regions, so the virtual address equals the physical address.
     */
    return (void *)(uintptr_t)phys;
}

void dma_free(void *vaddr, size_t size)
{
    if (vaddr == NULL || size == 0) {
        return;
    }

    uint32_t phys = (uint32_t)(uintptr_t)vaddr;
    uint32_t pages = (uint32_t)(dma_page_align(size) / ARCH_PMM_PAGE_SIZE);

    pmm_free_frames(phys, pages);
}

/* =========================================================================
 * DMA mapping (identity-mapped: no-op)
 * ========================================================================= */

dma_addr_t dma_map(dma_addr_t phys, size_t size, dma_dir_t direction)
{
    (void)size;
    (void)direction;

    /* Identity mapping: physical address is the DMA address */
    return phys;
}

void dma_unmap(void *virt, size_t size, dma_dir_t direction)
{
    (void)virt;
    (void)size;
    (void)direction;

    /* No-op on identity-mapped targets */
}

/* =========================================================================
 * DMA synchronisation — per-architecture cache maintenance
 * ========================================================================= */

#if ARCH_IS_X86

/*
 * x86: DMA is cache-coherent.  No cache maintenance needed.
 * The "memory" clobber on compiler barriers is sufficient.
 */
void dma_sync_for_device(dma_addr_t phys, size_t size)
{
    (void)phys;
    (void)size;
    arch_compiler_barrier();
}

void dma_sync_for_cpu(dma_addr_t phys, size_t size)
{
    (void)phys;
    (void)size;
    arch_compiler_barrier();
}

#elif ARCH_ARM32

/*
 * ARM32: Use CP15 cache maintenance operations.
 *
 * For DMA_TO_DEVICE (CPU writes, device reads):
 *   dma_sync_for_device -> clean to PoC (DCCMVAC) so device sees data.
 *
 * For DMA_FROM_DEVICE (device writes, CPU reads):
 *   dma_sync_for_cpu -> invalidate to PoC (DCIMVAC) so CPU sees new data.
 *
 * For DMA_BIDIRECTIONAL:
 *   Both clean and invalidate (DCCIMVAC).
 */

/** Clean D-cache range to PoC (write-back to memory). */
static void arm32_dma_clean_range(dma_addr_t phys, size_t size)
{
    uint32_t line_mask = DMA_CACHE_LINE_SIZE - 1U;
    uint32_t addr = (uint32_t)phys & ~line_mask;
    uint32_t end  = DMA_ALIGN_UP((uint32_t)phys + size, DMA_CACHE_LINE_SIZE);

    while (addr < end) {
        /* DCCMVAC: Data Cache Clean by MVA to PoC */
        __asm__ volatile ("mcr p15, 0, %0, c7, c10, 1" :: "r"(addr) : "memory");
        addr += DMA_CACHE_LINE_SIZE;
    }
    arch_dsb();
}

/** Invalidate D-cache range to PoC (discard, device will have written). */
static void arm32_dma_invalidate_range(dma_addr_t phys, size_t size)
{
    uint32_t line_mask = DMA_CACHE_LINE_SIZE - 1U;
    uint32_t addr = (uint32_t)phys & ~line_mask;
    uint32_t end  = DMA_ALIGN_UP((uint32_t)phys + size, DMA_CACHE_LINE_SIZE);

    while (addr < end) {
        /* DCIMVAC: Data Cache Invalidate by MVA to PoC */
        __asm__ volatile ("mcr p15, 0, %0, c7, c6, 1" :: "r"(addr) : "memory");
        addr += DMA_CACHE_LINE_SIZE;
    }
    arch_dsb();
}

/** Clean and invalidate D-cache range to PoC. */
static void arm32_dma_clean_invalidate_range(dma_addr_t phys, size_t size)
{
    uint32_t line_mask = DMA_CACHE_LINE_SIZE - 1U;
    uint32_t addr = (uint32_t)phys & ~line_mask;
    uint32_t end  = DMA_ALIGN_UP((uint32_t)phys + size, DMA_CACHE_LINE_SIZE);

    while (addr < end) {
        /* DCCIMVAC: Data Cache Clean and Invalidate by MVA to PoC */
        __asm__ volatile ("mcr p15, 0, %0, c7, c14, 1" :: "r"(addr) : "memory");
        addr += DMA_CACHE_LINE_SIZE;
    }
    arch_dsb();
}

void dma_sync_for_device(dma_addr_t phys, size_t size)
{
    arm32_dma_clean_range(phys, size);
}

void dma_sync_for_cpu(dma_addr_t phys, size_t size)
{
    arm32_dma_clean_invalidate_range(phys, size);
}

#elif ARCH_ARM64

/*
 * AArch64: Use DC instructions by MVA to PoC.
 *
 * DC CVAC  - Clean by VA to PoC (write-back dirty lines)
 * DC CIVAC - Clean and Invalidate by VA to PoC
 * DC IVAC  - Invalidate by VA to PoC (discard lines)
 */

/** Clean D-cache range to PoC. */
static void aarch64_dma_clean_range(dma_addr_t phys, size_t size)
{
    uint64_t line_mask = DMA_CACHE_LINE_SIZE - 1ULL;
    uint64_t addr = (uint64_t)phys & ~line_mask;
    uint64_t end  = DMA_ALIGN_UP((uint64_t)phys + size, DMA_CACHE_LINE_SIZE);

    while (addr < end) {
        __asm__ volatile ("dc cvac, %0" :: "r"(addr) : "memory");
        addr += DMA_CACHE_LINE_SIZE;
    }
    arch_dsb();
}

/** Clean and invalidate D-cache range to PoC. */
static void aarch64_dma_clean_invalidate_range(dma_addr_t phys, size_t size)
{
    uint64_t line_mask = DMA_CACHE_LINE_SIZE - 1ULL;
    uint64_t addr = (uint64_t)phys & ~line_mask;
    uint64_t end  = DMA_ALIGN_UP((uint64_t)phys + size, DMA_CACHE_LINE_SIZE);

    while (addr < end) {
        __asm__ volatile ("dc civac, %0" :: "r"(addr) : "memory");
        addr += DMA_CACHE_LINE_SIZE;
    }
    arch_dsb();
}

/** Invalidate D-cache range to PoC. */
static void aarch64_dma_invalidate_range(dma_addr_t phys, size_t size)
{
    uint64_t line_mask = DMA_CACHE_LINE_SIZE - 1ULL;
    uint64_t addr = (uint64_t)phys & ~line_mask;
    uint64_t end  = DMA_ALIGN_UP((uint64_t)phys + size, DMA_CACHE_LINE_SIZE);

    while (addr < end) {
        __asm__ volatile ("dc ivac, %0" :: "r"(addr) : "memory");
        addr += DMA_CACHE_LINE_SIZE;
    }
    arch_dsb();
}

void dma_sync_for_device(dma_addr_t phys, size_t size)
{
    aarch64_dma_clean_range(phys, size);
}

void dma_sync_for_cpu(dma_addr_t phys, size_t size)
{
    aarch64_dma_clean_invalidate_range(phys, size);
}

#elif ARCH_RISCV64

/*
 * RISC-V: Typically cache-coherent on QEMU virt.
 * Real hardware with non-coherent DMA would use Zicbop/Zicbom
 * cache-block management instructions (not yet widely available).
 */
void dma_sync_for_device(dma_addr_t phys, size_t size)
{
    (void)phys;
    (void)size;
    arch_compiler_barrier();
}

void dma_sync_for_cpu(dma_addr_t phys, size_t size)
{
    (void)phys;
    (void)size;
    arch_compiler_barrier();
}

#else
#error "DMA: unsupported architecture"
#endif
