/*
 * Fern - Cross-Architecture ELF Loader
 *
 * Provides a single entry point for loading ELF binaries.  The architecture-
 * specific loaders are dispatched at compile time via the ARCH_* macros
 * defined in arch.h.
 */
#include "elf.h"

/* Forward declarations for architecture-specific ELF loaders */
#if ARCH_X86_32 || ARCH_X86_64
#include "../include/memory.h"
int x86_elf_load(const void *elf_data, uint32_t size,
                 uintptr_t *entry_out, uintptr_t *sp_out);
#elif ARCH_ARM32
#include "../arm32/elf_loader.h"
#elif ARCH_ARM64
#include "../aarch64/elf_loader.h"
#elif ARCH_RISCV64
#include "../riscv64/elf_loader.h"
#endif

/* =========================================================================
 * Unified Validation
 * ========================================================================= */

int elf_validate(const void *data, uint32_t size)
{
    if (!data) {
        return -1;
    }

    const elf_header_t *eh = (const elf_header_t *)data;

    if (size < sizeof(elf_header_t)) {
        return -1;
    }

    /* ELF magic: 0x7F 'E' 'L' 'F' */
    if (eh->e_ident[EI_MAG0] != ELF_MAGIC_0 ||
        eh->e_ident[EI_MAG1] != ELF_MAGIC_1 ||
        eh->e_ident[EI_MAG2] != ELF_MAGIC_2 ||
        eh->e_ident[EI_MAG3] != ELF_MAGIC_3) {
        return -2;
    }

    /* Check ELF class matches our pointer width */
#if ARCH_IS_64BIT
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
        return -3;
    }
#else
    if (eh->e_ident[EI_CLASS] != ELFCLASS32) {
        return -3;
    }
#endif

    /* Little-endian only */
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        return -4;
    }

    return 0;
}

/* =========================================================================
 * Header Field Accessors
 * ========================================================================= */

uintptr_t elf_get_entry(const void *data)
{
    const elf_header_t *eh = (const elf_header_t *)data;
#if ARCH_IS_64BIT
    return (uintptr_t)eh->e_entry;
#else
    return (uintptr_t)eh->e_entry;
#endif
}

uint16_t elf_get_type(const void *data)
{
    const elf_header_t *eh = (const elf_header_t *)data;
    return eh->e_type;
}

uint16_t elf_get_machine(const void *data)
{
    const elf_header_t *eh = (const elf_header_t *)data;
    return eh->e_machine;
}

/* =========================================================================
 * Architecture-Specific ELF Loaders
 * ========================================================================= */

#if ARCH_X86_32 || ARCH_X86_64
/* ------------------------------------------------------------------
 * x86 ELF loader (wraps the existing x86 VMM/PMM-based loader)
 * ------------------------------------------------------------------ */
int x86_elf_load(const void *elf_data, uint32_t size,
                 uintptr_t *entry_out, uintptr_t *sp_out)
{
    if (!elf_data || !entry_out || !sp_out) {
        return -1;
    }

    const elf32_ehdr_t *eh = (const elf32_ehdr_t *)elf_data;

    if (size < sizeof(elf32_ehdr_t)) {
        return -2;
    }

    if (elf_validate(elf_data, size) != 0) {
        return -3;
    }

    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        return -4;
    }

    if (eh->e_machine != ELF_MACHINE_386) {
        return -5;
    }

    /* Create a new page directory */
    page_directory_t *new_dir = vmm_create_page_directory();
    if (!new_dir) {
        return -6;
    }

    /* Load each PT_LOAD segment */
    const elf32_phdr_t *ph = (const elf32_phdr_t *)(elf_data + eh->e_phoff);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) {
            continue;
        }

        if (ph[i].p_offset + ph[i].p_filesz > size) {
            goto fail;
        }

        uint32_t seg_start = ph[i].p_vaddr & ~(MEMORY_PAGE_SIZE - 1);
        uint32_t seg_end   = (ph[i].p_vaddr + ph[i].p_memsz + MEMORY_PAGE_SIZE - 1)
                             & ~(MEMORY_PAGE_SIZE - 1);

        /* Determine page flags */
        uint32_t flags = PAGE_PRESENT | PAGE_USER;
        if (ph[i].p_flags & PF_W) {
            flags |= PAGE_WRITABLE;
        }

        /* Map each page */
        for (uint32_t va = seg_start; va < seg_end; va += MEMORY_PAGE_SIZE) {
            uint32_t frame = pmm_alloc_frame();
            if (!frame) {
                goto fail;
            }

            memory_result_t res = vmm_map_page(new_dir, va, frame, flags);
            if (res != MEMORY_OK && res != MEMORY_ERROR_ALREADY_MAPPED) {
                goto fail;
            }

            /* Copy segment data via temp-map window */
            uint32_t page_offset = 0;
            if (va < ph[i].p_vaddr) {
                page_offset = ph[i].p_vaddr - va;
            }

            uint32_t copy_size = MEMORY_PAGE_SIZE - page_offset;
            uint32_t src_offset = (va > ph[i].p_vaddr) ? (va - ph[i].p_vaddr) : 0;
            if (src_offset < ph[i].p_filesz) {
                uint32_t remaining = ph[i].p_filesz - src_offset;
                if (copy_size > remaining) {
                    copy_size = remaining;
                }
                if (copy_size > 0) {
                    void *kmap = vmm_temp_map_page(frame);
                    if (!kmap) {
                        goto fail;
                    }
                    const uint8_t *src = (const uint8_t *)elf_data + ph[i].p_offset + src_offset;
                    uint8_t *dst = (uint8_t *)kmap + page_offset;
                    for (uint32_t b = 0; b < copy_size; b++) {
                        dst[b] = src[b];
                    }
                    vmm_temp_unmap_page(kmap);
                }
            }
            /* BSS is zeroed by pmm_alloc_frame() (frame is zeroed on allocation) */
        }
    }

    /* Set up user stack at the top of user virtual address space.
     * USER_STACK_TOP = 0xBFFFF000, USER_STACK_SIZE = 256KB */
    {
        uint32_t stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;
        for (uint32_t va = stack_bottom; va < USER_STACK_TOP; va += MEMORY_PAGE_SIZE) {
            uint32_t frame = pmm_alloc_frame();
            if (!frame) {
                goto fail;
            }
            memory_result_t res = vmm_map_page(new_dir, va, frame,
                                               PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
            if (res != MEMORY_OK && res != MEMORY_ERROR_ALREADY_MAPPED) {
                goto fail;
            }
        }
    }

    *entry_out = (uintptr_t)eh->e_entry;
    *sp_out    = (uintptr_t)USER_STACK_TOP;
    return 0;

fail:
    vmm_destroy_page_directory(new_dir);
    return -6;
}

#elif ARCH_ARM32
/* ------------------------------------------------------------------
 * ARM32 ELF loader
 * ------------------------------------------------------------------ */
static int arm32_load_wrapper(const void *elf_data, uint32_t size,
                              uintptr_t *entry_out, uintptr_t *sp_out)
{
    if (!elf_data || !entry_out || !sp_out) {
        return -1;
    }

    uint32_t entry = 0;
    uint32_t sp = 0;
    arm_l1_table_t *l1 = NULL;

    int rc = arm32_elf_load((const uint8_t *)elf_data, size,
                            &entry, &sp, &l1);
    if (rc != 0) {
        return rc;
    }

    *entry_out = (uintptr_t)entry;
    *sp_out    = (uintptr_t)sp;
    return 0;
}

#elif ARCH_ARM64
/* ------------------------------------------------------------------
 * AArch64 ELF loader
 * ------------------------------------------------------------------ */
static int aarch64_load_wrapper(const void *elf_data, uint32_t size,
                                uintptr_t *entry_out, uintptr_t *sp_out)
{
    if (!elf_data || !entry_out || !sp_out) {
        return -1;
    }

    /* Create a user page table */
    extern pgd_t *aarch64_elf_create_user_pgd(void);
    pgd_t *user_pgd = aarch64_elf_create_user_pgd();
    if (!user_pgd) {
        return -6;
    }

    uint64_t entry = 0;
    uint64_t sp = 0;

    int rc = aarch64_elf_load((const uint8_t *)elf_data, (uint64_t)size,
                              user_pgd, &entry, &sp);
    if (rc != 0) {
        return rc;
    }

    *entry_out = (uintptr_t)entry;
    *sp_out    = (uintptr_t)sp;
    return 0;
}

#elif ARCH_RISCV64
/* ------------------------------------------------------------------
 * RISC-V 64-bit ELF loader
 * ------------------------------------------------------------------ */
static int riscv64_load_wrapper(const void *elf_data, uint32_t size,
                                uintptr_t *entry_out, uintptr_t *sp_out)
{
    if (!elf_data || !entry_out || !sp_out) {
        return -1;
    }

    riscv64_elf_result_t result = {0};

    int rc = riscv64_elf_load((const uint8_t *)elf_data, (uint64_t)size, &result);
    if (rc != 0) {
        return rc;
    }

    *entry_out = (uintptr_t)result.entry_point;
    *sp_out    = (uintptr_t)result.stack_top;
    return 0;
}

#endif /* architecture dispatch */

/* =========================================================================
 * Unified ELF Load Dispatch
 * ========================================================================= */

int elf_load(const void *data, uint32_t size,
             uintptr_t *entry_out, uintptr_t *sp_out)
{
    if (!data || !entry_out || !sp_out) {
        return -1;
    }

    /* Validate common ELF header fields */
    int rc = elf_validate(data, size);
    if (rc != 0) {
        return rc;
    }

    /* Dispatch to the architecture-specific loader */
#if ARCH_X86_32 || ARCH_X86_64
    return x86_elf_load(data, size, entry_out, sp_out);
#elif ARCH_ARM32
    return arm32_load_wrapper(data, size, entry_out, sp_out);
#elif ARCH_ARM64
    return aarch64_load_wrapper(data, size, entry_out, sp_out);
#elif ARCH_RISCV64
    return riscv64_load_wrapper(data, size, entry_out, sp_out);
#else
    #error "No ELF loader implemented for this architecture"
#endif
}
