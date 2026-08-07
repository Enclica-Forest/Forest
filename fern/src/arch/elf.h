/*
 * Fern - Cross-Architecture ELF Loader Interface
 *
 * Provides unified ELF types and a single entry point for loading ELF
 * binaries across all supported architectures:
 *   x86 (32-bit), x86_64, ARM32, AArch64, RISC-V 64
 *
 * ELF32 is used on 32-bit architectures; ELF64 on 64-bit architectures.
 * The unified header selects the correct format at compile time based on
 * pointer size (ARCH_BITS from arch.h).
 */
#ifndef FOREST_ARCH_ELF_H
#define FOREST_ARCH_ELF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "arch.h"

/* =========================================================================
 * ELF Identification Constants
 * ========================================================================= */

#define ELF_MAGIC_0  0x7F
#define ELF_MAGIC_1  'E'
#define ELF_MAGIC_2  'L'
#define ELF_MAGIC_3  'F'

#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define EI_CLASS    4
#define EI_DATA     5
#define EI_VERSION  6
#define EI_NIDENT   16

#define ELFCLASS32  1
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EV_CURRENT  1

/* =========================================================================
 * ELF File Types
 * ========================================================================= */

#define ET_NONE  0
#define ET_REL   1
#define ET_EXEC  2
#define ET_DYN   3
#define ET_CORE  4

/* =========================================================================
 * ELF Machine Types
 * ========================================================================= */

#define ELF_MACHINE_NONE      0
#define ELF_MACHINE_386       3
#define ELF_MACHINE_ARM       40
#define ELF_MACHINE_X86_64    62
#define ELF_MACHINE_AARCH64   183
#define ELF_MACHINE_RISCV     243

/* =========================================================================
 * ELF Program Header Types
 * ========================================================================= */

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4

/* =========================================================================
 * ELF Program Header Flags
 * ========================================================================= */

#define PF_X  0x1
#define PF_W  0x2
#define PF_R  0x4

/* =========================================================================
 * ELF Header Structures
 *
 * The unified header type selects ELF32 or ELF64 based on pointer width.
 * Fields shared by both formats (e_ident, e_type, e_machine) are accessed
 * via the first common fields; entry point and program header offset use
 * the pointer-width-appropriate field.
 * ========================================================================= */

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf32_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) elf32_phdr_t;

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

/* =========================================================================
 * Unified ELF Types (pointer-width selected)
 * ========================================================================= */

#if ARCH_IS_64BIT
typedef elf64_ehdr_t elf_header_t;
typedef elf64_phdr_t elf_program_header_t;
#else
typedef elf32_ehdr_t elf_header_t;
typedef elf32_phdr_t elf_program_header_t;
#endif

/* =========================================================================
 * ELF Load Result
 * ========================================================================= */

typedef struct {
    uintptr_t entry_point;   /* ELF entry point virtual address */
    uintptr_t stack_top;     /* Top of initial user stack       */
    bool      valid;         /* True on successful load         */
} elf_load_result_t;

/* =========================================================================
 * Unified ELF Loader API
 * ========================================================================= */

/**
 * elf_validate - Validate an ELF image's magic, class, and endianness.
 *
 * @data  Pointer to the ELF image in memory.
 * @size  Size of the image in bytes.
 *
 * Returns 0 on success, negative error code on failure:
 *   -1  NULL data or too small for header
 *   -2  Bad ELF magic
 *   -3  Wrong ELF class (32/64 mismatch)
 *   -4  Wrong data encoding (not little-endian)
 */
int elf_validate(const void *data, uint32_t size);

/**
 * elf_get_entry - Get the ELF entry point address.
 *
 * @data  Validated ELF image pointer.
 *
 * Returns the entry point virtual address.
 */
uintptr_t elf_get_entry(const void *data);

/**
 * elf_get_type - Get the ELF file type (ET_EXEC, ET_DYN, etc.).
 *
 * @data  Validated ELF image pointer.
 *
 * Returns the e_type field value.
 */
uint16_t elf_get_type(const void *data);

/**
 * elf_get_machine - Get the ELF target machine.
 *
 * @data  Validated ELF image pointer.
 *
 * Returns the e_machine field value.
 */
uint16_t elf_get_machine(const void *data);

/**
 * elf_load - Load an ELF binary into the current address space.
 *
 * Dispatches to the architecture-specific loader for the compile target.
 *
 * @data       Pointer to the ELF image in memory.
 * @size       Size of the image in bytes.
 * @entry_out  Receives the entry point virtual address.
 * @sp_out     Receives the top of the initial user stack.
 *
 * Returns 0 on success, negative error code on failure.
 */
int elf_load(const void *data, uint32_t size,
             uintptr_t *entry_out, uintptr_t *sp_out);

#endif /* FOREST_ARCH_ELF_H */
