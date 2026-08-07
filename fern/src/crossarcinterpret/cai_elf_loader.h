/*
 * cai_elf_loader.h - ELF32 / ELF64 loader for the cross-architecture interpreter
 *
 * Parses an in-memory ELF image, validates its architecture against the
 * interpreter context, maps each PT_LOAD segment into the guest address space,
 * zeroes BSS, and sets up an initial stack with argc / argv / envp in the
 * ABI-correct layout for the target architecture.
 *
 * Supported guest ELF types
 * -------------------------
 *  CAI_ARCH_X86_32  : e_machine == EM_386    (ELF32, LE)
 *  CAI_ARCH_X86_64  : e_machine == EM_X86_64 (ELF64, LE)
 *  CAI_ARCH_ARM32   : e_machine == EM_ARM     (ELF32, LE)
 *  CAI_ARCH_AARCH64 : e_machine == EM_AARCH64 (ELF64, LE)
 *
 * Only ET_EXEC (statically linked executables) are supported; ET_DYN
 * (position-independent) is accepted but the load bias is forced to 0.
 */

#ifndef CAI_ELF_LOADER_H
#define CAI_ELF_LOADER_H

#include <stddef.h>
#include <stdint.h>
#include "crossarcinterpret.h"
#include "cai_memory.h"

/* =========================================================================
 * ELF identification constants (subset used by the loader)
 * ========================================================================= */

/* e_ident indices */
#define CAI_EI_MAG0    0
#define CAI_EI_MAG1    1
#define CAI_EI_MAG2    2
#define CAI_EI_MAG3    3
#define CAI_EI_CLASS   4   /* 1 = ELFCLASS32, 2 = ELFCLASS64 */
#define CAI_EI_DATA    5   /* 1 = ELFDATA2LSB (little-endian) */
#define CAI_EI_VERSION 6
#define CAI_EI_NIDENT  16

#define CAI_ELFCLASS32  1
#define CAI_ELFCLASS64  2
#define CAI_ELFDATA2LSB 1

/* e_type */
#define CAI_ET_EXEC 2
#define CAI_ET_DYN  3

/* e_machine */
#define CAI_EM_386     3
#define CAI_EM_ARM     40
#define CAI_EM_X86_64  62
#define CAI_EM_AARCH64 183

/* Program header type */
#define CAI_PT_LOAD 1

/* Program header flags */
#define CAI_PF_X 0x1
#define CAI_PF_W 0x2
#define CAI_PF_R 0x4

/* =========================================================================
 * Minimal packed ELF structures
 *
 * We define our own rather than including the kernel elf.h to avoid type
 * conflicts (elf.h uses Fern uint8/uint32 typedefs; we use stdint here).
 * ========================================================================= */

typedef struct __attribute__((packed)) {
    uint8_t  e_ident[CAI_EI_NIDENT];
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
} cai_elf32_ehdr_t;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} cai_elf32_phdr_t;

typedef struct __attribute__((packed)) {
    uint8_t  e_ident[CAI_EI_NIDENT];
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
} cai_elf64_ehdr_t;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} cai_elf64_phdr_t;

/* =========================================================================
 * Loader result
 * ========================================================================= */

typedef struct {
    uint64_t entry_point;   /* Guest virtual entry point                       */
    uint64_t stack_top;     /* Guest virtual address of initial stack top      */
    uint64_t load_base;     /* Lowest guest virtual address of any PT_LOAD seg */
    uint64_t load_end;      /* One past the highest byte of any PT_LOAD seg    */
} cai_elf_load_result_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/*
 * cai_elf_detect_arch - Identify the target architecture from an ELF header.
 *
 * @elf_data  : Pointer to the start of the ELF image.
 * @elf_size  : Size of the image in bytes.
 * @arch_out  : Populated with the detected cai_arch_t on success.
 *
 * Returns CAI_OK or a negative CAI_E* code.
 */
int cai_elf_detect_arch(const uint8_t *elf_data, size_t elf_size,
                        cai_arch_t *arch_out);

/*
 * cai_elf_load - Parse an ELF binary and populate a guest address space.
 *
 * Loads all PT_LOAD segments into @as (allocating via cai_as_map), zeroes BSS,
 * sets up an initial stack region, and writes argc/argv/envp onto the stack in
 * the correct ABI layout for @target_arch.
 *
 * @elf_data    : Raw ELF image bytes.
 * @elf_size    : Image size.
 * @target_arch : Architecture expected / to emulate.
 * @as          : Destination guest address space (must be freshly created).
 * @argc        : Argument count.
 * @argv        : Argument vector (host pointers to NUL-terminated strings).
 * @result      : Populated on success with entry point and stack top.
 *
 * Returns CAI_OK on success, negative CAI_E* on failure.
 */
int cai_elf_load(const uint8_t *elf_data, size_t elf_size,
                 cai_arch_t target_arch, cai_address_space_t *as,
                 int argc, char **argv,
                 cai_elf_load_result_t *result);

#endif /* CAI_ELF_LOADER_H */
