/*
 * Fern - AArch64 ELF64 Loader
 *
 * Parses an ELF64 binary and maps its segments into a user page table.
 * Provides a simple physical page frame allocator for user-space pages.
 */

#include "elf_loader.h"
#include "mmu.h"
#include "uart.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Physical-to-kernel-virtual conversion                               */
/*                                                                     */
/* After the MMU is enabled, physical addresses in the upper memory    */
/* range (>64MB) are not identity-mapped.  We access them through the  */
/* kernel's higher-half mapping: VA = PA | 0xFFFF000000000000.         */
/* ------------------------------------------------------------------ */
static inline uint64_t phys_to_kva(uint64_t pa)
{
    return pa | 0xFFFF000000000000UL;
}

/* ------------------------------------------------------------------ */
/* Simple physical page frame allocator                                */
/*                                                                     */
/* Allocates 4KB frames from a region above the kernel image.          */
/* Used only for user-space segment backing pages during early boot.   */
/* ------------------------------------------------------------------ */

/* These symbols are provided by the linker script (link.ld) */
extern uint64_t _kernel_end;

/* Pool: 512 frames = 2 MB of user page memory */
#define USER_PAGE_POOL_FRAMES  512
#define USER_FRAME_SIZE        4096

static uint64_t page_pool_base;
static uint64_t page_pool_next;
static uint64_t page_pool_end;

static void page_pool_init(void)
{
    /* Start allocation just above _kernel_end, page-aligned */
    uint64_t base = ((uint64_t)&_kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1UL);
    page_pool_base = base;
    page_pool_next = base;
    page_pool_end  = base + (USER_PAGE_POOL_FRAMES * USER_FRAME_SIZE);
}

static uint64_t alloc_frame(void)
{
    if (page_pool_next >= page_pool_end)
        return 0; /* out of frames */

    uint64_t pa = page_pool_next;
    page_pool_next += USER_FRAME_SIZE;

    /* Zero the frame via the kernel's higher-half mapping */
    uint64_t *p = (uint64_t *)phys_to_kva(pa);
    for (int i = 0; i < (USER_FRAME_SIZE / 8); i++)
        p[i] = 0;

    return pa;
}

/* ------------------------------------------------------------------ */
/* Static page table pool for user address spaces                      */
/*                                                                     */
/* Each user process needs its own L0 (PGD). Intermediate tables are   */
/* allocated from the static pool inside mmu.c. Here we maintain a     */
/* small pool of full L0 tables for task_create_elf().                 */
/* ------------------------------------------------------------------ */

static pgd_t user_pgd_pool[4] __attribute__((aligned(PAGE_SIZE)));
static int user_pgd_next = 0;

pgd_t *aarch64_elf_create_user_pgd(void)
{
    if (user_pgd_next >= 4)
        return NULL;

    pgd_t *pgd = &user_pgd_pool[user_pgd_next++];
    for (int i = 0; i < TABLE_ENTRIES; i++)
        (*pgd)[i] = PTE_TYPE_FAULT;
    return pgd;
}

/* ------------------------------------------------------------------ */
/* ELF64 loader                                                        */
/* ------------------------------------------------------------------ */

static inline uint64_t align_up(uint64_t val, uint64_t align)
{
    return (val + align - 1) & ~(align - 1);
}

static inline uint64_t align_down(uint64_t val, uint64_t align)
{
    return val & ~(align - 1);
}

int aarch64_elf_load(const uint8_t *elf_data, uint64_t size,
                     pgd_t *user_pgd, uint64_t *entry_out, uint64_t *sp_out)
{
    if (!elf_data || !user_pgd || !entry_out || !sp_out)
        return -1;

    /* ---- Validate ELF64 header ------------------------------------ */
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)elf_data;

    if (size < sizeof(elf64_ehdr_t))
        return -2;

    if (eh->e_ident[0] != ELF_MAGIC_0 || eh->e_ident[1] != ELF_MAGIC_1 ||
        eh->e_ident[2] != ELF_MAGIC_2 || eh->e_ident[3] != ELF_MAGIC_3)
        return -3;

    if (eh->e_ident[4] != ELFCLASS64)
        return -4;

    if (eh->e_ident[5] != ELFDATA2LSB)
        return -5;

    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
        return -6;

    if (eh->e_machine != ELF_MACHINE_AARCH64)
        return -7;

    if (eh->e_phentsize != sizeof(elf64_phdr_t))
        return -8;

    if (eh->e_phnum == 0)
        return -9;

    /* ---- Initialise the page frame pool if not yet done ----------- */
    if (page_pool_base == 0)
        page_pool_init();

    /* ---- Load each PT_LOAD segment -------------------------------- */
    const elf64_phdr_t *ph = (const elf64_phdr_t *)(elf_data + eh->e_phoff);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;

        if (ph[i].p_memsz == 0)
            continue;

        uint64_t vaddr    = ph[i].p_vaddr;
        uint64_t filesz   = ph[i].p_filesz;
        uint64_t memsz    = ph[i].p_memsz;
        uint64_t offset   = ph[i].p_offset;

        /* Validate bounds */
        if (offset + filesz > size)
            return -10;

        if (memsz < filesz)
            return -11;

        /* Determine page-level flags */
        uint64_t map_flags = AARCH64_MAP_USER;
        if (ph[i].p_flags & PF_W)
            map_flags |= AARCH64_MAP_RW;
        else
            map_flags |= AARCH64_MAP_RO;

        /* Map each page of this segment */
        uint64_t seg_start = align_down(vaddr, PAGE_SIZE);
        uint64_t seg_end   = align_up(vaddr + memsz, PAGE_SIZE);

        for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
            uint64_t pa = alloc_frame();
            if (pa == 0) {
                uart_puts("[elf] out of physical frames\n");
                return -12;
            }

            if (aarch64_map_page(user_pgd, va, pa, map_flags) != 0) {
                uart_puts("[elf] aarch64_map_page failed\n");
                return -13;
            }
        }

        /* Copy segment data from the ELF image */
        uint64_t copy_src = (uint64_t)(uintptr_t)(elf_data + offset);
        uint64_t copy_dst = vaddr;
        uint64_t remaining = filesz;

        while (remaining > 0) {
            /* Since MMU is on and user_pgd maps this VA, we can write directly.
             * However, we need to handle the page-aligned copying carefully
             * since the user_pgd is not yet the active TTBR0.  Instead, copy
             * into the physical frame via the identity-mapped kernel address. */
            uint64_t page_offset = copy_dst & (PAGE_SIZE - 1);
            uint64_t phys_addr = aarch64_get_phys(user_pgd, copy_dst);
            uint64_t chunk = PAGE_SIZE - page_offset;
            if (chunk > remaining)
                chunk = remaining;

            /* Write to the physical frame via higher-half kernel mapping */
            uint8_t *dst = (uint8_t *)phys_to_kva(phys_addr);
            const uint8_t *src = (const uint8_t *)copy_src;
            for (uint64_t b = 0; b < chunk; b++)
                dst[b] = src[b];

            copy_src += chunk;
            copy_dst += chunk;
            remaining -= chunk;
        }

        /* BSS: the rest of memsz beyond filesz is already zeroed
         * by alloc_frame(). No additional work needed. */
    }

    /* ---- Set up user stack ---------------------------------------- */
    /* Map stack pages (already zeroed by alloc_frame) */
    uint64_t stack_flags = AARCH64_MAP_USER | AARCH64_MAP_RW;
    for (uint64_t va = USER_STACK_BOTTOM; va < USER_STACK_TOP; va += PAGE_SIZE) {
        uint64_t pa = alloc_frame();
        if (pa == 0) {
            uart_puts("[elf] out of frames for stack\n");
            return -14;
        }
        if (aarch64_map_page(user_pgd, va, pa, stack_flags) != 0) {
            uart_puts("[elf] stack map failed\n");
            return -15;
        }
    }

    *entry_out = eh->e_entry;
    *sp_out    = USER_STACK_TOP;

    uart_printf("[elf] loaded: entry=0x%lx sp=0x%lx\n", *entry_out, *sp_out);
    return 0;
}
