/*
 * Fern - AArch64 ELF64 Loader
 *
 * Loads statically-linked ELF64 executables into user address space.
 * Maps PT_LOAD segments into a per-process page table (pgd_t), copies
 * segment data, zeros BSS, and returns the entry point and user stack.
 */
#ifndef AARCH64_ELF_LOADER_H
#define AARCH64_ELF_LOADER_H

#include <stdint.h>
#include <stddef.h>
#include "mmu.h"

/* Use the unified cross-architecture ELF types from arch/elf.h.
 * ELF64 structure definitions, magic constants, and program header types
 * are all provided there. */
#include "../arch/elf.h"

/* ------------------------------------------------------------------ */
/* User virtual address constants                                      */
/* ------------------------------------------------------------------ */

#define USER_STACK_TOP   0x00007FFFFFFF0000UL
#define USER_STACK_SIZE  (16 * 1024)  /* 16 KB */
#define USER_STACK_BOTTOM (USER_STACK_TOP - USER_STACK_SIZE)

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/*
 * aarch64_elf_load - Parse an ELF64 binary and map it into a new user
 * address space.
 *
 * @elf_data:  Pointer to the ELF image in memory (kernel-mapped).
 * @size:      Size of the ELF image in bytes.
 * @user_pgd:  Pointer to the user process page table (pre-allocated).
 * @entry_out: Receives the ELF entry point virtual address.
 * @sp_out:    Receives the user stack pointer (top of user stack).
 *
 * Returns 0 on success, negative errno on failure.
 */
int aarch64_elf_load(const uint8_t *elf_data, uint64_t size,
                     pgd_t *user_pgd, uint64_t *entry_out, uint64_t *sp_out);

#endif /* AARCH64_ELF_LOADER_H */
