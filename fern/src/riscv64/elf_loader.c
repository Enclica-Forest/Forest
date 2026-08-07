/*
 * Fern - RISC-V 64-bit ELF64 Loader Implementation
 *
 * Loads ELF64 PT_LOAD segments into a new Sv39 page table with
 * user-mode permissions. Uses a bump allocator for physical pages.
 */
#include "elf_loader.h"
#include "mmu.h"
#include "uart.h"
#include <stdint.h>
#include <stddef.h>

/* Kernel end from linker script */
extern uint64_t _kernel_end;

/* ------------------------------------------------------------------ */
/* Physical page bump allocator                                        */
/* Allocates from kernel_end onward; no free support.                  */
/* ------------------------------------------------------------------ */

static uint64_t user_phys_next = 0;

static void user_phys_init(void)
{
    uint64_t ke_pa = (uint64_t)&_kernel_end - KERNEL_VIRT_BASE + KERNEL_PHYS_BASE;
    user_phys_next = (ke_pa + PAGE_SIZE - 1) & PAGE_MASK;
}

static uint64_t user_phys_alloc_page(void)
{
    if (user_phys_next == 0)
        return 0;

    uint64_t pa = user_phys_next;
    user_phys_next += PAGE_SIZE;

    /* Zero the page */
    uint64_t *p = (uint64_t *)(uintptr_t)pa;
    for (int i = 0; i < TABLE_ENTRIES; i++)
        p[i] = 0;

    return pa;
}

/* ------------------------------------------------------------------ */
/* Create a user page table with kernel high-half mappings copied      */
/* ------------------------------------------------------------------ */

static sv39_pgd_t *create_user_page_table(void)
{
    sv39_pgd_t *pgd = (sv39_pgd_t *)riscv64_alloc_page_table();
    if (!pgd)
        return NULL;

    /* Copy kernel L2 entries (high half: indices 256-511) so that
     * kernel code/data remains accessible in supervisor mode. */
    sv39_pgd_t *kpgd = riscv64_get_kernel_pgd();
    for (int i = 256; i < 512; i++)
        pgd[i] = kpgd[i];

    return pgd;
}

/* ------------------------------------------------------------------ */
/* ELF64 header validation                                             */
/* ------------------------------------------------------------------ */

static int validate_elf64(const elf64_ehdr_t *eh, uint64_t size)
{
    if (size < sizeof(elf64_ehdr_t))
        return -1;

    if (eh->e_ident[EI_MAG0] != ELF_MAGIC_0 ||
        eh->e_ident[EI_MAG1] != ELF_MAGIC_1 ||
        eh->e_ident[EI_MAG2] != ELF_MAGIC_2 ||
        eh->e_ident[EI_MAG3] != ELF_MAGIC_3)
        return -2;

    if (eh->e_ident[EI_CLASS] != ELFCLASS64)
        return -3;

    if (eh->e_ident[EI_DATA] != ELFDATA2LSB)
        return -4;

    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
        return -5;

    if (eh->e_machine != ELF_MACHINE_RISCV)
        return -6;

    if (eh->e_phnum == 0)
        return -7;

    if (eh->e_phentsize != sizeof(elf64_phdr_t))
        return -8;

    return 0;
}

/* ------------------------------------------------------------------ */
/* riscv64_elf_load                                                    */
/* ------------------------------------------------------------------ */

int riscv64_elf_load(const uint8_t *elf_data, uint64_t size,
                     riscv64_elf_result_t *result)
{
    if (!elf_data || !result)
        return -1;

    if (user_phys_next == 0)
        user_phys_init();

    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)elf_data;

    int err = validate_elf64(eh, size);
    if (err != 0) {
        riscv64_uart_printf("[ELF] Validation failed: %d\n", err);
        return err;
    }

    sv39_pgd_t *pgd = create_user_page_table();
    if (!pgd) {
        riscv64_uart_puts("[ELF] Page table alloc failed\n");
        return -10;
    }

    /* Map the user stack in the low half */
    uint64_t stack_top = RISCV64_USER_STACK_TOP;
    uint64_t stack_bot = stack_top - RISCV64_USER_STACK_SIZE;
    for (uint64_t va = stack_bot; va < stack_top; va += PAGE_SIZE) {
        uint64_t pa = user_phys_alloc_page();
        if (!pa) return -11;
        if (riscv64_map_page(pgd, va, pa,
                             PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D))
            return -12;
    }

    /* Load each PT_LOAD segment */
    const elf64_phdr_t *ph = (const elf64_phdr_t *)(elf_data + eh->e_phoff);
    int found_load = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *seg = &ph[i];

        if (seg->p_type != PT_LOAD || seg->p_memsz == 0)
            continue;

        found_load = 1;

        uint64_t seg_start = seg->p_vaddr & PAGE_MASK;
        uint64_t seg_end   = (seg->p_vaddr + seg->p_memsz + PAGE_SIZE - 1) & PAGE_MASK;

        uint64_t flags = PTE_V | PTE_U | PTE_A | PTE_D;
        if (seg->p_flags & PF_R) flags |= PTE_R;
        if (seg->p_flags & PF_W) flags |= PTE_W;
        if (seg->p_flags & PF_X) flags |= PTE_X;

        for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
            uint64_t pa = user_phys_alloc_page();
            if (!pa) return -13;

            if (riscv64_map_page(pgd, va, pa, flags))
                return -14;

            /* Copy file data into the page */
            uint64_t offset_in_page = seg->p_vaddr & (PAGE_SIZE - 1);
            uint64_t src_start = (va > seg->p_vaddr) ? va : seg->p_vaddr;
            uint64_t src_end_u = seg->p_vaddr + seg->p_filesz;
            if (src_start < src_end_u && offset_in_page < PAGE_SIZE) {
                uint64_t copy_off = src_start - seg->p_vaddr;
                uint64_t copy_sz  = src_end_u - src_start;
                uint64_t page_rem = PAGE_SIZE - offset_in_page;
                if (copy_sz > page_rem) copy_sz = page_rem;

                uint64_t file_off = seg->p_offset + copy_off;
                if (file_off + copy_sz <= size) {
                    uint8_t *dst = (uint8_t *)(uintptr_t)pa + offset_in_page;
                    const uint8_t *src = elf_data + file_off;
                    for (uint64_t j = 0; j < copy_sz; j++)
                        dst[j] = src[j];
                }
            }
            /* BSS is already zero from page alloc */
        }
    }

    if (!found_load) {
        riscv64_uart_puts("[ELF] No PT_LOAD segments\n");
        return -15;
    }

    result->entry_point = eh->e_entry;
    result->stack_top   = stack_top;
    result->page_table  = pgd;
    result->valid       = 1;

    riscv64_uart_printf("[ELF] Loaded entry=0x%lx sp=0x%lx\n",
                        eh->e_entry, stack_top);
    return 0;
}
