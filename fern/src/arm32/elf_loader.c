#include "elf_loader.h"
#include "arm32.h"

/* All ELF32 types (elf32_ehdr_t, elf32_phdr_t, PF_X, PT_LOAD, etc.)
 * are now provided via the unified cross-architecture ELF header
 * included through elf_loader.h -> ../arch/elf.h */

/* ========================================================================
 * Physical page bump allocator
 *
 * The ARM32 kernel runs from 0x40008000 with heap starting after BSS.
 * Free physical pages start at _heap_start (page-aligned upward).
 * Each call to page_alloc() returns the next 4 KB page.
 * ======================================================================== */

extern char _heap_start[];

static uint32_t page_alloc_next = 0;  /* next free physical page address */
static uint32_t page_alloc_end  = 0;  /* end of available physical memory */

void arm32_elf_init(void)
{
    /* Align _heap_start up to the next 16 KB boundary.
     * The L1 translation table requires 16 KB alignment (TTBR0 bits
     * [13:0] must be zero when TTBCR.N = 0).  By starting the bump
     * allocator at a 16 KB boundary, every subsequent allocation (which
     * advances by 4 KB) will produce addresses where the first 4 pages
     * of any 16 KB region are naturally aligned. */
    uint32_t start = (uint32_t)(uintptr_t)_heap_start;
    page_alloc_next = (start + 0x3FFFU) & ~0x3FFFU;

    /*
     * RAM is 128 MB starting at 0x40000000 (from link.ld).
     * Leave 64 KB of headroom at the top of RAM for initial stacks.
     */
    page_alloc_end = 0x40000000U + (128U * 1024U * 1024U) - 0x10000U;
}

/**
 * page_alloc - Allocate a single 4 KB physical page.
 *
 * Returns the physical address of the page, or 0 on failure.
 * The page is not zeroed; callers that need zeroed pages must do so.
 */
static uint32_t page_alloc(void)
{
    if (page_alloc_next == 0 || page_alloc_next >= page_alloc_end) {
        return 0;
    }

    uint32_t page = page_alloc_next;
    page_alloc_next += 0x1000U;  /* advance by 4 KB */
    return page;
}

/* ========================================================================
 * ELF loading helpers
 * ======================================================================== */

static inline uint32_t elf_align_down(uint32_t val, uint32_t align)
{
    return val & ~(align - 1U);
}

static inline uint32_t elf_align_up(uint32_t val, uint32_t align)
{
    return (val + align - 1U) & ~(align - 1U);
}

static bool elf_validate_arm32(const uint8_t *elf_data, uint32_t size)
{
    if (size < sizeof(elf32_ehdr_t)) {
        return false;
    }

    const elf32_ehdr_t *eh = (const elf32_ehdr_t *)elf_data;

    /* Magic: \x7fELF */
    if (eh->e_ident[EI_MAG0] != 0x7F ||
        eh->e_ident[EI_MAG1] != 'E'  ||
        eh->e_ident[EI_MAG2] != 'L'  ||
        eh->e_ident[EI_MAG3] != 'F') {
        return false;
    }

    /* 32-bit, little-endian */
    if (eh->e_ident[EI_CLASS] != ELF_CLASS_32) {
        return false;
    }
    if (eh->e_ident[EI_DATA] != ELF_DATA_2LSB) {
        return false;
    }

    /* Must be an executable */
    if (eh->e_type != ELF_TYPE_EXEC) {
        return false;
    }

    /* ARM machine */
    if (eh->e_machine != ELF_MACHINE_ARM) {
        return false;
    }

    /* Current ELF version */
    if (eh->e_version != ELF_VERSION_CURRENT) {
        return false;
    }

    /* Sanity-check program header sizes */
    if (eh->e_ehsize != sizeof(elf32_ehdr_t)) {
        return false;
    }
    if (eh->e_phentsize != sizeof(elf32_phdr_t)) {
        return false;
    }
    if (eh->e_phnum == 0 || eh->e_phnum > 256) {
        return false;
    }

    return true;
}

/**
 * elf_map_segment - Map one PT_LOAD segment into the L1 page table.
 *
 * For each page in the segment:
 *   1. Allocate a physical page.
 *   2. Zero it (to handle BSS/padding).
 *   3. Map it via arm_map_page() with user-accessible permissions.
 *   4. Copy the segment data from the ELF buffer.
 *
 * @l1        Target L1 translation table.
 * @elf_data  Base of the ELF file in memory.
 * @phdr      The PT_LOAD program header.
 * @elf_size  Total size of the ELF file (for bounds checking).
 *
 * Returns true on success, false on allocation failure.
 */
static bool elf_map_segment(arm_l1_table_t *l1,
                            const uint8_t *elf_data,
                            const elf32_phdr_t *phdr,
                            uint32_t elf_size)
{
    /* Validate program header bounds */
    if (phdr->p_offset + phdr->p_filesz > elf_size) {
        return false;
    }
    if (phdr->p_filesz > phdr->p_memsz) {
        return false;
    }

    uint32_t seg_start = elf_align_down(phdr->p_vaddr, 0x1000U);
    uint32_t seg_end   = elf_align_up(phdr->p_vaddr + phdr->p_memsz, 0x1000U);

    if (seg_end <= seg_start) {
        return false;
    }

    /* Compute page-level permissions */
    uint32_t page_flags = ARM_PAGE_MEM_NORMAL_WB_WA;

    /* User access: AP[1:0] = 11 (full access PL0+PL1) */
    page_flags |= ARM_PAGE_AP_FULL_ACCESS;

    /* Execute Never: set XN if segment is NOT executable */
    if (!(phdr->p_flags & PF_X)) {
        page_flags |= ARM_PAGE_XN;
    }

    const uint8_t *src = elf_data + phdr->p_offset;
    uint32_t src_offset = 0;

    for (uint32_t va = seg_start; va < seg_end; va += 0x1000U) {
        uint32_t phys = page_alloc();
        if (!phys) {
            return false;
        }

        /* Zero the page (handles BSS/padding between filesz and memsz) */
        uint32_t *page_ptr = (uint32_t *)(uintptr_t)phys;
        for (uint32_t i = 0; i < 256; i++) {
            page_ptr[i] = 0;
        }

        /* Map the page */
        arm_map_page(l1, va, phys, page_flags);

        /* Copy segment data if we haven't exhausted p_filesz */
        uint32_t page_offset = 0;
        if (va < phdr->p_vaddr) {
            /* First page might start partway through the segment */
            page_offset = phdr->p_vaddr - va;
        }

        uint32_t copy_size = 0x1000U - page_offset;
        if (src_offset < phdr->p_filesz) {
            uint32_t remaining_in_file = phdr->p_filesz - src_offset;
            if (copy_size > remaining_in_file) {
                copy_size = remaining_in_file;
            }
            if (copy_size > 0) {
                /* Map phys temporarily to copy data */
                /* Since phys == linear address in the L2 pool region,
                 * we can access it directly */
                uint8_t *dst = (uint8_t *)(uintptr_t)phys + page_offset;
                const uint8_t *file_src = src + src_offset;
                for (uint32_t b = 0; b < copy_size; b++) {
                    dst[b] = file_src[b];
                }
            }
            src_offset += copy_size;
        }
    }

    return true;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

int arm32_elf_load(const uint8_t *elf_data, uint32_t size,
                   uint32_t *entry_out, uint32_t *sp_out,
                   arm_l1_table_t **l1_out)
{
    if (!elf_data || !entry_out || !sp_out) {
        return -1;
    }

    /* Validate the ELF header */
    if (!elf_validate_arm32(elf_data, size)) {
        return -2;
    }

    const elf32_ehdr_t *eh = (const elf32_ehdr_t *)elf_data;

    /* Allocate a new L1 translation table.
     * The L1 table is 4096 entries × 4 bytes = 16 KB and MUST be
     * 16 KB aligned (TTBR0 requirement).  Since page_alloc_next is
     * 16 KB aligned (from arm32_elf_init), the first allocation is
     * guaranteed to be properly aligned.  We allocate 4 consecutive
     * pages (16 KB) for the table. */
    uint32_t l1_phys = page_alloc();
    if (!l1_phys) {
        return -3;
    }
    /* Consume 3 more pages to get the full 16 KB */
    (void)page_alloc();
    (void)page_alloc();
    (void)page_alloc();
    arm_l1_table_t *l1 = (arm_l1_table_t *)(uintptr_t)l1_phys;

    /* Zero the L1 table (all entries = fault) */
    uint32_t *l1_raw = (uint32_t *)l1;
    for (uint32_t i = 0; i < 4096; i++) {
        l1_raw[i] = 0;
    }

    /* Copy kernel identity-mapping entries from the current L1 table.
     * arm_read_ttbr0() returns the active kernel page table, which
     * includes both the identity-mapped kernel regions and the device
     * MMIO mappings added by kernel_arm32.c. */
    uint32_t kernel_ttbr0 = arm_read_ttbr0();
    arm_l1_table_t *kernel_l1 = (arm_l1_table_t *)(uintptr_t)kernel_ttbr0;
    for (uint32_t i = 0; i < 4096; i++) {
        l1_raw[i] = ((uint32_t *)kernel_l1)[i];
    }

    /* Load each PT_LOAD segment */
    const elf32_phdr_t *phdrs =
        (const elf32_phdr_t *)(elf_data + eh->e_phoff);

    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) {
            continue;
        }
        if (phdrs[i].p_memsz == 0) {
            continue;
        }
        if (!elf_map_segment(l1, elf_data, &phdrs[i], size)) {
            return -4;
        }
    }

    /* Set up user stack: allocate physical pages and map them.
     * Stack grows downward on ARM, so sp_out points to the top
     * (highest usable address).  ARM32_USER_STACK_SIZE = 64 KB = 16 pages. */
    uint32_t stack_pages = ARM32_USER_STACK_SIZE / 0x1000U;
    uint32_t stack_top = ARM32_USER_STACK_VADDR + ARM32_USER_STACK_SIZE;

    /* Stack flags: user R/W, no-execute, normal memory */
    uint32_t stack_flags = ARM_PAGE_MEM_NORMAL_WB_WA
                         | ARM_PAGE_AP_FULL_ACCESS
                         | ARM_PAGE_XN;

    /* Map all stack pages */
    for (uint32_t i = 0; i < stack_pages; i++) {
        uint32_t stack_phys = page_alloc();
        if (!stack_phys) {
            return -5;
        }

        /* Zero the stack page */
        uint32_t *sp_ptr = (uint32_t *)(uintptr_t)stack_phys;
        for (uint32_t j = 0; j < 256; j++) {
            sp_ptr[j] = 0;
        }

        uint32_t stack_va = ARM32_USER_STACK_VADDR + (i * 0x1000U);
        arm_map_page(l1, stack_va, stack_phys, stack_flags);
    }

    /* Flush TLB to pick up all new mappings */
    arm_flush_tlb_all();

    /* Output parameters */
    *entry_out = eh->e_entry;
    *sp_out    = stack_top;  /* top of stack (SP grows down) */
    if (l1_out) {
        *l1_out = l1;
    }

    return 0;
}
