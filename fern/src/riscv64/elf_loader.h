/*
 * Fern - RISC-V 64-bit ELF64 Loader
 *
 * Parses ELF64 executables and loads them into user address space
 * using Sv39 page tables with proper R/W/X permissions.
 */
#ifndef RISCV64_ELF_LOADER_H
#define RISCV64_ELF_LOADER_H

#include <stdint.h>
#include <stddef.h>
#include "mmu.h"

/* Use the unified cross-architecture ELF types from arch/elf.h.
 * ELF64 structure definitions, magic constants, and program header types
 * are all provided there. */
#include "../arch/elf.h"

/* User address space layout */
#define RISCV64_USER_STACK_TOP  0x0000003F80000000ULL
#define RISCV64_USER_STACK_SIZE (16 * PAGE_SIZE)

/* ELF load result */
typedef struct {
    uint64_t     entry_point;
    uint64_t     stack_top;
    sv39_pgd_t  *page_table;
    int          valid;
} riscv64_elf_result_t;

/**
 * riscv64_elf_load - Load an ELF64 executable into a new Sv39 address space.
 *
 * @elf_data: Pointer to the ELF file image in memory.
 * @size:     Size of the ELF image in bytes.
 * @result:   Receives entry point, stack top, and page table on success.
 *
 * Returns 0 on success, negative error code on failure.
 */
int riscv64_elf_load(const uint8_t *elf_data, uint64_t size,
                     riscv64_elf_result_t *result);

#endif /* RISCV64_ELF_LOADER_H */
