#ifndef ARM32_ELF_LOADER_H
#define ARM32_ELF_LOADER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "mmu.h"

/* Use the unified cross-architecture ELF types from arch/elf.h.
 * ELF_MACHINE_ARM, elf32_ehdr_t, elf32_phdr_t, and all common
 * ELF constants are provided there. */
#include "../arch/elf.h"

/* User-space address ranges */
#define ARM32_USER_STACK_VADDR  0x70000000U
#define ARM32_USER_STACK_SIZE   (64 * 1024U)   /* 64 KB */
#define ARM32_USER_HEAP_VADDR   0x60000000U
#define ARM32_USER_HEAP_SIZE    (256 * 1024U)  /* 256 KB */

/* Minimum free physical pages after loading an ELF */
#define ELF_MIN_FREE_PAGES 32

/**
 * arm32_elf_load - Load an ELF32 binary into mapped pages.
 *
 * @elf_data   Pointer to the ELF file in memory.
 * @size       Size of the ELF file in bytes.
 * @entry_out  Receives the ELF entry point address.
 * @sp_out     Receives the top of the user stack.
 * @l1_out     Receives the L1 translation table for this task (or NULL
 *             if the caller wants to use the kernel table).
 *
 * Returns 0 on success, negative error code on failure.
 */
int arm32_elf_load(const uint8_t *elf_data, uint32_t size,
                   uint32_t *entry_out, uint32_t *sp_out,
                   arm_l1_table_t **l1_out);

/**
 * arm32_elf_init - Initialise the physical page allocator for ELF loading.
 *
 * Must be called once before arm32_elf_load().  Uses the linker symbol
 * _heap_start to determine the base of free physical memory.
 */
void arm32_elf_init(void);

#endif /* ARM32_ELF_LOADER_H */
