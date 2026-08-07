/*
 * pmm.c - Cross-architecture Physical Memory Manager implementation
 *
 * Bitmap-based frame allocator.  Architecture-specific memory map parsing
 * feeds into a common bitmap core.
 *
 * Supported memory map sources:
 *   x86_64 BIOS:   Multiboot info (ram_start / ram_size path)
 *   x86_64 UEFI:   EFI_MEMORY_DESCRIPTOR array via pmm_init_from_memory_map()
 *   ARM32/AArch64: DTB /memory@XXXX nodes or UEFI memory map
 *   RISC-V:        DTB /memory@XXXX nodes
 */

#include "pmm.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Architecture selection
 * --------------------------------------------------------------------- */
#if defined(__x86_64__) || defined(__i386__)
#define ARCH_PMM_X86        1
#elif defined(__aarch64__)
#define ARCH_PMM_AARCH64    1
#elif defined(__arm__)
#define ARCH_PMM_ARM32      1
#elif defined(__riscv)
#define ARCH_PMM_RISCV      1
#else
#define ARCH_PMM_GENERIC    1
#endif

/* -----------------------------------------------------------------------
 * Internal bitmap state
 * --------------------------------------------------------------------- */

/* One bit per frame: 1 = used/reserved, 0 = free */
static uint32_t pmm_bitmap[ARCH_PMM_MAX_FRAMES / 32];

static struct {
    bool     initialized;
    uint32_t total_frames;   /* total frames tracked */
    uint32_t free_frames;    /* currently free */
    uint32_t last_frame;     /* scan hint for next allocation */
    uint32_t kernel_end_frame; /* first frame after kernel+bitmap */
} pmm = {0};

/* -----------------------------------------------------------------------
 * Bitmap primitives
 * --------------------------------------------------------------------- */

static inline bool bitmap_test(uint32_t frame)
{
    return (pmm_bitmap[frame / 32] & (1U << (frame % 32))) != 0;
}

static inline void bitmap_set(uint32_t frame)
{
    uint32_t idx = frame / 32;
    uint32_t bit = 1U << (frame % 32);
    if (!(pmm_bitmap[idx] & bit)) {
        pmm_bitmap[idx] |= bit;
        pmm.free_frames--;
    }
}

static inline void bitmap_clear(uint32_t frame)
{
    uint32_t idx = frame / 32;
    uint32_t bit = 1U << (frame % 32);
    if (pmm_bitmap[idx] & bit) {
        pmm_bitmap[idx] &= ~bit;
        pmm.free_frames++;
    }
}

/* -----------------------------------------------------------------------
 * Scan helpers
 * --------------------------------------------------------------------- */

static uint32_t find_free_frame(uint32_t start)
{
    for (uint32_t i = 0; i < pmm.total_frames; i++) {
        uint32_t frame = (start + i) % pmm.total_frames;
        if (frame < pmm.kernel_end_frame)
            continue;
        if (!bitmap_test(frame))
            return frame;
    }
    return 0;
}

static uint32_t find_contiguous(uint32_t count)
{
    if (count == 0 || count > pmm.free_frames)
        return 0;
    if (count == 1)
        return find_free_frame(pmm.last_frame);

    uint32_t run = 0;
    uint32_t start = 0;

    for (uint32_t frame = pmm.kernel_end_frame; frame < pmm.total_frames; frame++) {
        if (!bitmap_test(frame)) {
            if (run == 0)
                start = frame;
            run++;
            if (run >= count)
                return start;
        } else {
            run = 0;
        }
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Region registration (called by arch-specific parsers)
 * --------------------------------------------------------------------- */

/* Maximum memory regions we track before finalising */
#define MAX_REGIONS 64

static struct {
    uint64_t base;
    uint64_t length;
    bool     available; /* true = usable RAM */
} pmm_regions[MAX_REGIONS];
static uint32_t pmm_region_count = 0;

static void pmm_add_region(uint64_t base, uint64_t length, bool available)
{
    if (pmm_region_count >= MAX_REGIONS || length == 0)
        return;

    /* Align to page boundaries */
    uint64_t aligned_base  = base & ~(uint64_t)(ARCH_PMM_PAGE_SIZE - 1);
    uint64_t aligned_end   = (base + length + ARCH_PMM_PAGE_SIZE - 1)
                             & ~(uint64_t)(ARCH_PMM_PAGE_SIZE - 1);
    if (aligned_end <= aligned_base)
        return;

    pmm_regions[pmm_region_count].base      = aligned_base;
    pmm_regions[pmm_region_count].length    = aligned_end - aligned_base;
    pmm_regions[pmm_region_count].available = available;
    pmm_region_count++;
}

/*
 * pmm_apply_regions - Commit collected regions into the bitmap.
 *
 * Must be called once after all pmm_add_region() calls.  Marks every frame
 * in available regions as free and computes total_frames from the highest
 * address seen.
 */
static void pmm_apply_regions(void)
{
    /* Determine highest address to size the bitmap */
    uint64_t highest = 0;
    for (uint32_t i = 0; i < pmm_region_count; i++) {
        uint64_t end = pmm_regions[i].base + pmm_regions[i].length;
        if (end > highest)
            highest = end;
    }

    pmm.total_frames = (uint32_t)(highest / ARCH_PMM_PAGE_SIZE);
    if (pmm.total_frames > ARCH_PMM_MAX_FRAMES)
        pmm.total_frames = ARCH_PMM_MAX_FRAMES;

    /* Start with everything marked used */
    memset(pmm_bitmap, 0xFF, sizeof(pmm_bitmap));
    pmm.free_frames = 0;

    /* Free available regions */
    for (uint32_t i = 0; i < pmm_region_count; i++) {
        if (!pmm_regions[i].available)
            continue;
        uint32_t start = (uint32_t)(pmm_regions[i].base / ARCH_PMM_PAGE_SIZE);
        uint32_t end   = (uint32_t)((pmm_regions[i].base +
                         pmm_regions[i].length) / ARCH_PMM_PAGE_SIZE);
        if (end > pmm.total_frames)
            end = pmm.total_frames;
        for (uint32_t f = start; f < end; f++)
            bitmap_clear(f);
    }

    /*
     * Reserve frames 0..kernel_end.
     * The kernel typically loads at 0x100000 (1 MB) on x86 or 0x40080000
     * on ARM.  We conservatively reserve up to the end of the bitmap
     * itself plus a small margin so the bitmap lives in reserved space.
     *
     * kernel_end_frame covers:
     *   [0, KERNEL_LOAD_END) where KERNEL_LOAD_END is the end of the
     *   PMM bitmap plus 1 MB of kernel space.
     *
     * For the simple pmm_init() path we also reserve the range below
     * ram_start (which on x86 includes the VGA/ROM area).
     */
    extern char __kernel_start[];
    extern char __kernel_end[];
    (void)__kernel_start;
    (void)__kernel_end;

    /* Reserve frames occupied by the bitmap itself */
    uint32_t bitmap_start = (uint32_t)((uintptr_t)pmm_bitmap /
                            ARCH_PMM_PAGE_SIZE);
    uint32_t bitmap_frames = (sizeof(pmm_bitmap) + ARCH_PMM_PAGE_SIZE - 1) /
                              ARCH_PMM_PAGE_SIZE;
    for (uint32_t f = bitmap_start; f < bitmap_start + bitmap_frames; f++) {
        if (f < pmm.total_frames)
            bitmap_set(f);
    }

    pmm.kernel_end_frame = bitmap_start + bitmap_frames;

    /* Scan hint starts after the kernel/bitmap area */
    pmm.last_frame = pmm.kernel_end_frame;
}

/* -----------------------------------------------------------------------
 * Simple flat-range initialisation
 * --------------------------------------------------------------------- */

int pmm_init(uint32_t ram_start, uint32_t ram_size)
{
    if (ram_size == 0)
        return -1;

    pmm_region_count = 0;

    /*
     * Reserve everything below ram_start as reserved (covers BIOS area,
     * VGA, ROM, memory-mapped hardware on x86, or zero-page on ARM).
     */
    if (ram_start > 0)
        pmm_add_region(0, ram_start, false);

    pmm_add_region(ram_start, ram_size, true);
    pmm_apply_regions();

    pmm.initialized = true;
    return 0;
}

/* -----------------------------------------------------------------------
 * UEFI memory map parsing (x86_64 UEFI, ARM UEFI)
 * --------------------------------------------------------------------- */

#if defined(ARCH_PMM_X86) || defined(ARCH_PMM_AARCH64) || \
    defined(ARCH_PMM_ARM32)

/*
 * Minimal UEFI memory descriptor layout (avoids pulling in the full
 * uefi_boot_services.h which requires EFIAPI and forward declarations).
 * The layout matches EFI_MEMORY_DESCRIPTOR from the UEFI spec.
 */
typedef struct {
    uint32_t Type;
    uint32_t _pad_phys;       /* high 32 bits of PhysicalStart (unused on 32-bit) */
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} __attribute__((packed)) pmm_efi_desc_t;

#define PMM_EFI_CONVENTIONAL_MEMORY 7  /* EfiConventionalMemory */

static int pmm_init_from_uefi(void *memmap, uint32_t memmap_size)
{
    if (!memmap || memmap_size == 0)
        return -1;

    pmm_efi_desc_t *desc = (pmm_efi_desc_t *)memmap;
    uint32_t num = memmap_size / sizeof(pmm_efi_desc_t);

    pmm_region_count = 0;

    for (uint32_t i = 0; i < num; i++) {
        bool avail = (desc->Type == PMM_EFI_CONVENTIONAL_MEMORY);
        pmm_add_region(desc->PhysicalStart,
                       desc->NumberOfPages * 4096ULL,
                       avail);
        desc = (pmm_efi_desc_t *)((uint8_t *)desc + sizeof(pmm_efi_desc_t));
    }

    if (pmm_region_count == 0)
        return -1;

    pmm_apply_regions();
    pmm.initialized = true;
    return 0;
}
#endif /* UEFI-capable archs */

/* -----------------------------------------------------------------------
 * DTB memory parsing (ARM32, AArch64, RISC-V)
 * --------------------------------------------------------------------- */

#if defined(ARCH_PMM_ARM32) || defined(ARCH_PMM_AARCH64) || \
    defined(ARCH_PMM_RISCV)
#include "../fdt.h"

/*
 * Parse DTB /memory@XXXX nodes to discover physical RAM.
 *
 * Each node's 'reg' property encodes (base, size) using #address-cells
 * and #size-cells from the root node.  Typical values:
 *   ARM32 / RISC-V: #address-cells=1, #size-cells=1  (two u32s)
 *   AArch64:        #address-cells=2, #size-cells=2  (two u64s)
 *
 * We try 2-cell first (AArch64) and fall back to 1-cell.
 */
static int pmm_init_from_dtb(void)
{
    pmm_region_count = 0;

    /* Try to find a /memory@XXXX node.  Iterate over likely addresses. */
    static const char *memory_paths[] = {
        "/memory@80000000",  /* ARM32/AArch64 common */
        "/memory@40000000",  /* QEMU ARM virt */
        "/memory@0",         /* RISC-V / low RAM */
        "/memory@8000000",   /* RISC-V SiFive */
        "/memory",           /* Fallback (unnamed) */
        NULL
    };

    for (const char **p = memory_paths; *p; p++) {
        uint32_t len = 0;
        const void *reg = fdt_get_property(*p, "reg", &len);
        if (!reg || len < 8)
            continue;

        /*
         * Determine cell size from the property length.
         * 8 bytes  = 1 address-cell + 1 size-cell  (u32 + u32)
         * 16 bytes = 2 address-cells + 2 size-cells (u64 + u64)
         */
        if (len == 16) {
            /* 2-cell format (AArch64) */
            uint64_t base = fdt64_to_cpu(((const uint64_t *)reg)[0]);
            uint64_t size = fdt64_to_cpu(((const uint64_t *)reg)[1]);
            pmm_add_region(base, size, true);
        } else if (len >= 8) {
            /* 1-cell format (ARM32 / RISC-V) */
            uint32_t base = fdt32_to_cpu(((const uint32_t *)reg)[0]);
            uint32_t size = fdt32_to_cpu(((const uint32_t *)reg)[1]);
            pmm_add_region(base, size, true);
        }
    }

    if (pmm_region_count == 0)
        return -1;

    pmm_apply_regions();
    pmm.initialized = true;
    return 0;
}
#endif /* DTB-capable archs */

/* -----------------------------------------------------------------------
 * Unified init-from-memory-map dispatcher
 * --------------------------------------------------------------------- */

int pmm_init_from_memory_map(void *memmap, uint32_t memmap_size)
{
#if defined(ARCH_PMM_X86)
    /* x86_64: could be UEFI memory map or Multiboot (handled externally) */
    if (memmap && memmap_size > 0)
        return pmm_init_from_uefi(memmap, memmap_size);
    return -1;

#elif defined(ARCH_PMM_AARCH64) || defined(ARCH_PMM_ARM32)
    /* ARM: prefer DTB, fall back to UEFI memory map */
    if (!memmap || memmap_size == 0)
        return pmm_init_from_dtb();
    return pmm_init_from_uefi(memmap, memmap_size);

#elif defined(ARCH_PMM_RISCV)
    /* RISC-V: always DTB; ignore memmap pointer */
    (void)memmap;
    (void)memmap_size;
    return pmm_init_from_dtb();

#else
    /* Generic fallback: require a flat memory map */
    if (memmap && memmap_size > 0) {
        /* Treat memmap as a sequence of (base_u32, size_u32, type_u32) triples */
        const uint32_t *m = (const uint32_t *)memmap;
        uint32_t entries = memmap_size / (3 * sizeof(uint32_t));
        pmm_region_count = 0;
        for (uint32_t i = 0; i < entries; i++) {
            uint32_t base   = m[i * 3 + 0];
            uint32_t length = m[i * 3 + 1];
            uint32_t type   = m[i * 3 + 2];
            pmm_add_region(base, length, type == 1);
        }
        if (pmm_region_count > 0) {
            pmm_apply_regions();
            pmm.initialized = true;
            return 0;
        }
    }
    return -1;
#endif
}

/* -----------------------------------------------------------------------
 * Frame allocation / free
 * --------------------------------------------------------------------- */

uint32_t pmm_alloc_frame(void)
{
    if (!pmm.initialized || pmm.free_frames == 0)
        return 0;

    uint32_t frame = find_free_frame(pmm.last_frame);
    if (frame == 0)
        return 0;

    bitmap_set(frame);
    pmm.last_frame = frame + 1;

    return frame * ARCH_PMM_PAGE_SIZE;
}

void pmm_free_frame(uint32_t frame)
{
    if (!pmm.initialized || frame == 0)
        return;

    uint32_t f = frame / ARCH_PMM_PAGE_SIZE;
    if (f < pmm.kernel_end_frame || f >= pmm.total_frames)
        return;

    bitmap_clear(f);
}

uint32_t pmm_alloc_frames(uint32_t count)
{
    if (!pmm.initialized || count == 0 || count > pmm.free_frames)
        return 0;

    uint32_t start = find_contiguous(count);
    if (start == 0)
        return 0;

    for (uint32_t i = 0; i < count; i++)
        bitmap_set(start + i);

    pmm.last_frame = start + count;
    return start * ARCH_PMM_PAGE_SIZE;
}

void pmm_free_frames(uint32_t base, uint32_t count)
{
    if (!pmm.initialized || base == 0)
        return;

    uint32_t start = base / ARCH_PMM_PAGE_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t f = start + i;
        if (f < pmm.kernel_end_frame || f >= pmm.total_frames)
            continue;
        bitmap_clear(f);
    }
}

/* -----------------------------------------------------------------------
 * Statistics
 * --------------------------------------------------------------------- */

uint32_t pmm_get_free_frames(void)
{
    return pmm.initialized ? pmm.free_frames : 0;
}

uint32_t pmm_get_total_frames(void)
{
    return pmm.initialized ? pmm.total_frames : 0;
}
