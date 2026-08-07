/*
 * pmm.h - Cross-architecture Physical Memory Manager interface
 *
 * Provides a unified PMM API that initializes from architecture-specific
 * memory maps:
 *   - x86_64 BIOS:  Multiboot memory map
 *   - x86_64 UEFI:  EFI memory descriptors
 *   - ARM32/AArch64: DTB /memory@XXXX or UEFI memory map
 *   - RISC-V:        DTB /memory@XXXX
 *
 * The bitmap-based allocator is architecture-independent; only the memory
 * map parsing differs per platform.
 */

#ifndef FOREST_ARCH_PMM_H
#define FOREST_ARCH_PMM_H

#include <stdint.h>
#include <stdbool.h>

/* Page frame size (4 KB, consistent across all supported architectures) */
#define ARCH_PMM_PAGE_SIZE      4096U
#define ARCH_PMM_PAGE_SHIFT     12U

/* Maximum physical memory the bitmap can track (4 GB) */
#define ARCH_PMM_MAX_MEMORY     (4ULL * 1024U * 1024U * 1024U)
#define ARCH_PMM_MAX_FRAMES     ((uint32_t)(ARCH_PMM_MAX_MEMORY / ARCH_PMM_PAGE_SIZE))

/* --- Initialization ---------------------------------------------------- */

/*
 * pmm_init - Simple initialization from a flat RAM range.
 *
 * @ram_start: Physical start address of available RAM (page-aligned).
 * @ram_size:  Size of available RAM in bytes.
 *
 * Marks the entire range as available, then reserves the kernel area.
 * Returns 0 on success, negative on error.
 */
int pmm_init(uint32_t ram_start, uint32_t ram_size);

/*
 * pmm_init_from_memory_map - Initialize from an architecture-specific
 *                            memory map provided by firmware.
 *
 * @memmap:      Pointer to the memory map (type depends on source):
 *                 UEFI: EFI_MEMORY_DESCRIPTOR array
 *                 DTB:  not used (NULL); DTB is parsed internally via fdt.h
 *                 Multiboot: not used (NULL); handled internally
 * @memmap_size: Size in bytes of the memory map, or 0 for DTB/Multiboot.
 *
 * Parses the firmware memory map, converts entries to internal regions,
 * and initializes the bitmap allocator.  Returns 0 on success.
 */
int pmm_init_from_memory_map(void *memmap, uint32_t memmap_size);

/* --- Frame allocation -------------------------------------------------- */

/*
 * pmm_alloc_frame - Allocate a single 4 KB physical frame.
 *
 * Returns the physical address of the allocated frame, or 0 on failure.
 */
uint32_t pmm_alloc_frame(void);

/*
 * pmm_free_frame - Return a previously allocated frame.
 *
 * @frame: Physical address of the frame to free (must be page-aligned).
 */
void pmm_free_frame(uint32_t frame);

/*
 * pmm_alloc_frames - Allocate 'count' contiguous physical frames.
 *
 * @count: Number of contiguous frames to allocate.
 *
 * Returns the physical address of the first frame, or 0 on failure.
 */
uint32_t pmm_alloc_frames(uint32_t count);

/*
 * pmm_free_frames - Free 'count' contiguous frames starting at 'base'.
 *
 * @base:  Physical address of the first frame (page-aligned).
 * @count: Number of frames to free.
 */
void pmm_free_frames(uint32_t base, uint32_t count);

/* --- Statistics -------------------------------------------------------- */

/*
 * pmm_get_free_frames - Return the current number of free frames.
 */
uint32_t pmm_get_free_frames(void);

/*
 * pmm_get_total_frames - Return the total number of tracked frames.
 */
uint32_t pmm_get_total_frames(void);

#endif /* FOREST_ARCH_PMM_H */
