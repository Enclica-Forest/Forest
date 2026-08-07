/*
 * cai_elf_loader.c - ELF32 / ELF64 loader for the cross-architecture interpreter
 *
 * Approach
 * --------
 * 1. Validate ELF magic, class, data encoding, and e_machine against the
 *    requested target architecture.
 * 2. Iterate PT_LOAD program headers.  For each:
 *    a. Allocate a guest region via cai_as_map().
 *    b. Copy file bytes into the host backing store.
 *    c. Zero the BSS tail (p_memsz > p_filesz).
 * 3. Allocate a stack region at a well-known guest virtual address.
 * 4. Push argc / argv pointers onto the stack in the Linux initial-process
 *    stack layout so that _start can read them without a C runtime.
 * 5. Return the entry point and initial stack pointer in the result struct.
 *
 * Stack layout (identical for 32-bit and 64-bit guests, widths differ)
 * -------------------------------------------------------------------
 *  HIGH ADDRESS (stack_top)
 *    arg strings (NUL-terminated, packed)
 *    environment strings (NUL-terminated, packed)
 *    AT_NULL auxv entry
 *    envp[0]  ...  envp[envc-1]  NULL
 *    argv[0]  ...  argv[argc-1]  NULL
 *    argc
 *  ← sp points here
 *  LOW ADDRESS
 *
 * For simplicity we provide an empty envp (just the NULL terminator) and no
 * auxiliary vector entries; a real libc needs only argc/argv/envp to start.
 */

#include "cai_elf_loader.h"
#include "cai_memory.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/debuglog.h"

/* =========================================================================
 * Guest stack configuration
 * ========================================================================= */

#define CAI_STACK_GUEST_TOP  0xBFFF0000ULL   /* Guest virtual top of stack    */
#define CAI_STACK_SIZE       (1024 * 1024)    /* 1 MiB initial stack           */
#define CAI_STACK_GUEST_BASE (CAI_STACK_GUEST_TOP - CAI_STACK_SIZE)

/* Page alignment helpers */
static inline uint64_t page_align_down(uint64_t v)
{
    return v & ~(uint64_t)(4096 - 1);
}
static inline uint64_t page_align_up(uint64_t v)
{
    return (v + 4095) & ~(uint64_t)(4095);
}

/* =========================================================================
 * ELF validation helpers
 * ========================================================================= */

static int check_elf_magic(const uint8_t *d)
{
    return (d[CAI_EI_MAG0] == 0x7F &&
            d[CAI_EI_MAG1] == 'E'  &&
            d[CAI_EI_MAG2] == 'L'  &&
            d[CAI_EI_MAG3] == 'F') ? 0 : -1;
}

/* =========================================================================
 * cai_elf_detect_arch
 * ========================================================================= */

int cai_elf_detect_arch(const uint8_t *elf_data, size_t elf_size,
                        cai_arch_t *arch_out)
{
    if (!elf_data || elf_size < CAI_EI_NIDENT + 4 || !arch_out)
        return CAI_EINVAL;

    if (check_elf_magic(elf_data) != 0) {
        debuglog(DEBUG_WARN, "cai_elf_detect_arch: bad magic\n");
        return CAI_EINVAL;
    }

    uint8_t  elfclass = elf_data[CAI_EI_CLASS];
    uint16_t machine;

    if (elfclass == CAI_ELFCLASS32) {
        if (elf_size < sizeof(cai_elf32_ehdr_t))
            return CAI_EINVAL;
        const cai_elf32_ehdr_t *eh = (const cai_elf32_ehdr_t *)elf_data;
        machine = eh->e_machine;
    } else if (elfclass == CAI_ELFCLASS64) {
        if (elf_size < sizeof(cai_elf64_ehdr_t))
            return CAI_EINVAL;
        const cai_elf64_ehdr_t *eh = (const cai_elf64_ehdr_t *)elf_data;
        machine = eh->e_machine;
    } else {
        debuglog(DEBUG_WARN, "cai_elf_detect_arch: unknown ELF class %u\n",
                 elfclass);
        return CAI_EINVAL;
    }

    switch (machine) {
    case CAI_EM_386:     *arch_out = CAI_ARCH_X86_32;  break;
    case CAI_EM_X86_64:  *arch_out = CAI_ARCH_X86_64;  break;
    case CAI_EM_ARM:     *arch_out = CAI_ARCH_ARM32;   break;
    case CAI_EM_AARCH64: *arch_out = CAI_ARCH_AARCH64; break;
    default:
        debuglog(DEBUG_WARN,
                 "cai_elf_detect_arch: unsupported e_machine=0x%x\n", machine);
        return CAI_ENOTSUP;
    }
    return CAI_OK;
}

/* =========================================================================
 * Protection flags: ELF PF_* → CAI_MEM_*
 * ========================================================================= */

static uint32_t pf_to_cai(uint32_t pf)
{
    uint32_t f = 0;
    if (pf & CAI_PF_R) f |= CAI_MEM_READ;
    if (pf & CAI_PF_W) f |= CAI_MEM_WRITE;
    if (pf & CAI_PF_X) f |= CAI_MEM_EXEC;
    return f;
}

/* =========================================================================
 * String length helper that is safe when running in the kernel
 * (avoids pulling in posix strlen)
 * ========================================================================= */

static size_t cai_slen(const char *s)
{
    if (!s) return 0;
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

/* =========================================================================
 * ELF32 loading path
 * ========================================================================= */

static int load_elf32(const uint8_t *elf_data, size_t elf_size,
                      uint16_t expected_machine,
                      cai_address_space_t *as,
                      cai_elf_load_result_t *result)
{
    if (elf_size < sizeof(cai_elf32_ehdr_t)) {
        debuglog(DEBUG_WARN, "cai_elf32: image too small (%u)\n",
                 (unsigned)elf_size);
        return CAI_EINVAL;
    }

    const cai_elf32_ehdr_t *eh = (const cai_elf32_ehdr_t *)elf_data;

    if (eh->e_ident[CAI_EI_DATA] != CAI_ELFDATA2LSB) {
        debuglog(DEBUG_WARN, "cai_elf32: not little-endian\n");
        return CAI_ENOTSUP;
    }
    if (eh->e_machine != expected_machine) {
        debuglog(DEBUG_WARN, "cai_elf32: e_machine mismatch (got %u want %u)\n",
                 eh->e_machine, expected_machine);
        return CAI_EINVAL;
    }
    if (eh->e_type != CAI_ET_EXEC && eh->e_type != CAI_ET_DYN) {
        debuglog(DEBUG_WARN, "cai_elf32: unsupported e_type %u\n", eh->e_type);
        return CAI_ENOTSUP;
    }
    if (eh->e_phnum == 0 || eh->e_phentsize < sizeof(cai_elf32_phdr_t)) {
        debuglog(DEBUG_WARN, "cai_elf32: no usable program headers\n");
        return CAI_EINVAL;
    }

    size_t phoff = eh->e_phoff;
    if (phoff + (size_t)eh->e_phnum * sizeof(cai_elf32_phdr_t) > elf_size) {
        debuglog(DEBUG_WARN, "cai_elf32: phdr table outside image\n");
        return CAI_EINVAL;
    }

    uint64_t load_base = (uint64_t)-1;
    uint64_t load_end  = 0;
    int segs_loaded = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const cai_elf32_phdr_t *ph =
            (const cai_elf32_phdr_t *)(elf_data + phoff +
                                        (size_t)i * sizeof(cai_elf32_phdr_t));

        if (ph->p_type != CAI_PT_LOAD)
            continue;
        if (ph->p_memsz == 0)
            continue;
        if (ph->p_filesz > ph->p_memsz) {
            debuglog(DEBUG_WARN, "cai_elf32: seg %u filesz > memsz\n", i);
            return CAI_EINVAL;
        }
        if ((size_t)ph->p_offset + ph->p_filesz > elf_size) {
            debuglog(DEBUG_WARN, "cai_elf32: seg %u data outside image\n", i);
            return CAI_EINVAL;
        }

        uint64_t seg_vaddr = (uint64_t)ph->p_vaddr;
        uint64_t seg_memsz = (uint64_t)ph->p_memsz;
        uint64_t seg_filesz= (uint64_t)ph->p_filesz;
        uint32_t flags     = pf_to_cai(ph->p_flags) | CAI_MEM_READ;

        /* Page-align the mapped region */
        uint64_t map_base  = page_align_down(seg_vaddr);
        uint64_t map_end   = page_align_up(seg_vaddr + seg_memsz);
        size_t   map_size  = (size_t)(map_end - map_base);

        if (cai_as_map(as, map_base, map_size, flags) != 0) {
            debuglog(DEBUG_ERROR,
                     "cai_elf32: cai_as_map failed for seg %u gva=0x%llx\n",
                     i, (unsigned long long)map_base);
            return CAI_ENOMEM;
        }

        /* Copy file bytes into guest memory */
        if (seg_filesz > 0) {
            const uint8_t *src = elf_data + ph->p_offset;
            for (uint64_t j = 0; j < seg_filesz; j++)
                cai_mem_w8(as, seg_vaddr + j, src[j]);
        }

        /* BSS: already zeroed by kzalloc inside cai_as_map */

        if (map_base < load_base) load_base = map_base;
        if (map_end  > load_end)  load_end  = map_end;
        segs_loaded++;
    }

    if (segs_loaded == 0) {
        debuglog(DEBUG_WARN, "cai_elf32: no PT_LOAD segments found\n");
        return CAI_EINVAL;
    }

    result->entry_point = (uint64_t)eh->e_entry;
    result->load_base   = load_base;
    result->load_end    = load_end;
    return CAI_OK;
}

/* =========================================================================
 * ELF64 loading path
 * ========================================================================= */

static int load_elf64(const uint8_t *elf_data, size_t elf_size,
                      uint16_t expected_machine,
                      cai_address_space_t *as,
                      cai_elf_load_result_t *result)
{
    if (elf_size < sizeof(cai_elf64_ehdr_t)) {
        debuglog(DEBUG_WARN, "cai_elf64: image too small (%u)\n",
                 (unsigned)elf_size);
        return CAI_EINVAL;
    }

    const cai_elf64_ehdr_t *eh = (const cai_elf64_ehdr_t *)elf_data;

    if (eh->e_ident[CAI_EI_DATA] != CAI_ELFDATA2LSB) {
        debuglog(DEBUG_WARN, "cai_elf64: not little-endian\n");
        return CAI_ENOTSUP;
    }
    if (eh->e_machine != expected_machine) {
        debuglog(DEBUG_WARN, "cai_elf64: e_machine mismatch (got %u want %u)\n",
                 eh->e_machine, expected_machine);
        return CAI_EINVAL;
    }
    if (eh->e_type != CAI_ET_EXEC && eh->e_type != CAI_ET_DYN) {
        debuglog(DEBUG_WARN, "cai_elf64: unsupported e_type %u\n", eh->e_type);
        return CAI_ENOTSUP;
    }
    if (eh->e_phnum == 0 || eh->e_phentsize < sizeof(cai_elf64_phdr_t)) {
        debuglog(DEBUG_WARN, "cai_elf64: no usable program headers\n");
        return CAI_EINVAL;
    }

    uint64_t phoff = eh->e_phoff;
    if (phoff + (uint64_t)eh->e_phnum * sizeof(cai_elf64_phdr_t) > elf_size) {
        debuglog(DEBUG_WARN, "cai_elf64: phdr table outside image\n");
        return CAI_EINVAL;
    }

    uint64_t load_base = (uint64_t)-1;
    uint64_t load_end  = 0;
    int segs_loaded = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const cai_elf64_phdr_t *ph =
            (const cai_elf64_phdr_t *)(elf_data + phoff +
                                        (uint64_t)i * sizeof(cai_elf64_phdr_t));

        if (ph->p_type != CAI_PT_LOAD)
            continue;
        if (ph->p_memsz == 0)
            continue;
        if (ph->p_filesz > ph->p_memsz) {
            debuglog(DEBUG_WARN, "cai_elf64: seg %u filesz > memsz\n", i);
            return CAI_EINVAL;
        }
        if (ph->p_offset + ph->p_filesz > elf_size) {
            debuglog(DEBUG_WARN, "cai_elf64: seg %u data outside image\n", i);
            return CAI_EINVAL;
        }

        uint64_t seg_vaddr = ph->p_vaddr;
        uint64_t seg_memsz = ph->p_memsz;
        uint64_t seg_filesz= ph->p_filesz;
        uint32_t flags     = pf_to_cai(ph->p_flags) | CAI_MEM_READ;

        uint64_t map_base  = page_align_down(seg_vaddr);
        uint64_t map_end   = page_align_up(seg_vaddr + seg_memsz);
        size_t   map_size  = (size_t)(map_end - map_base);

        if (cai_as_map(as, map_base, map_size, flags) != 0) {
            debuglog(DEBUG_ERROR,
                     "cai_elf64: cai_as_map failed seg %u gva=0x%llx\n",
                     i, (unsigned long long)map_base);
            return CAI_ENOMEM;
        }

        if (seg_filesz > 0) {
            const uint8_t *src = elf_data + ph->p_offset;
            for (uint64_t j = 0; j < seg_filesz; j++)
                cai_mem_w8(as, seg_vaddr + j, src[j]);
        }

        if (map_base < load_base) load_base = map_base;
        if (map_end  > load_end)  load_end  = map_end;
        segs_loaded++;
    }

    if (segs_loaded == 0) {
        debuglog(DEBUG_WARN, "cai_elf64: no PT_LOAD segments found\n");
        return CAI_EINVAL;
    }

    result->entry_point = eh->e_entry;
    result->load_base   = load_base;
    result->load_end    = load_end;
    return CAI_OK;
}

/* =========================================================================
 * Stack setup: Linux initial-process stack layout
 *
 * The stack grows down.  We build it from the top (CAI_STACK_GUEST_TOP).
 *
 * 32-bit layout (word = 4 bytes):
 *   [argc] [argv[0]] ... [argv[argc-1]] [NULL] [NULL(envp)] [AT_NULL,0]
 *   followed by the argument strings above (at higher addresses)
 *
 * 64-bit layout (word = 8 bytes): identical structure, wider pointers.
 *
 * We build the string area at the top of the stack and the pointer table
 * just below it, then write argc below that.  The final sp points at argc.
 * ========================================================================= */

static int setup_stack32(cai_address_space_t *as, int argc, char **argv,
                         uint64_t *sp_out)
{
    /* Map the stack region */
    if (cai_as_map(as, CAI_STACK_GUEST_BASE, CAI_STACK_SIZE,
                   CAI_MEM_READ | CAI_MEM_WRITE) != 0) {
        debuglog(DEBUG_ERROR, "cai_elf: stack map failed (32-bit)\n");
        return CAI_ENOMEM;
    }

    /* Write argument strings starting just below the very top */
    uint64_t str_ptr = CAI_STACK_GUEST_TOP - 4; /* leave a little guard room */

    /* argv string addresses (guest VAs), stored forwards */
    uint32_t argv_gvas[64]; /* support up to 64 args */
    int effective_argc = (argc > 64) ? 64 : argc;

    for (int i = effective_argc - 1; i >= 0; i--) {
        const char *s  = argv[i] ? argv[i] : "";
        size_t      len = cai_slen(s) + 1; /* include NUL */
        str_ptr -= (uint64_t)len;
        for (size_t j = 0; j < len; j++)
            cai_mem_w8(as, str_ptr + j, (uint8_t)s[j]);
        argv_gvas[i] = (uint32_t)str_ptr;
    }

    /* Align sp down to 4-byte boundary */
    str_ptr &= ~(uint64_t)3;

    /*
     * Build the pointer table below the strings.
     * Layout (top to bottom in memory, i.e. we push from higher to lower):
     *   AT_NULL aux entry : 0, 0   (two uint32_t)
     *   envp terminator   : NULL   (one uint32_t)
     *   argv[argc-1]      :        (uint32_t)
     *   ...
     *   argv[0]           :        (uint32_t)
     *   argc              :        (uint32_t)  ← sp
     */
    uint64_t sp = str_ptr;

    /* AT_NULL auxiliary vector (two zero words) */
    sp -= 4; cai_mem_w32(as, sp, 0); /* auxv value */
    sp -= 4; cai_mem_w32(as, sp, 0); /* AT_NULL tag */

    /* envp NULL terminator */
    sp -= 4; cai_mem_w32(as, sp, 0);

    /* argv pointers (reversed order so argv[0] is closest to argc) */
    sp -= 4; cai_mem_w32(as, sp, 0); /* argv NULL terminator */
    for (int i = effective_argc - 1; i >= 0; i--) {
        sp -= 4;
        cai_mem_w32(as, sp, argv_gvas[i]);
    }

    /* argc */
    sp -= 4;
    cai_mem_w32(as, sp, (uint32_t)effective_argc);

    *sp_out = sp;
    return CAI_OK;
}

static int setup_stack64(cai_address_space_t *as, int argc, char **argv,
                         uint64_t *sp_out)
{
    if (cai_as_map(as, CAI_STACK_GUEST_BASE, CAI_STACK_SIZE,
                   CAI_MEM_READ | CAI_MEM_WRITE) != 0) {
        debuglog(DEBUG_ERROR, "cai_elf: stack map failed (64-bit)\n");
        return CAI_ENOMEM;
    }

    uint64_t str_ptr = CAI_STACK_GUEST_TOP - 8;

    uint64_t argv_gvas[64];
    int effective_argc = (argc > 64) ? 64 : argc;

    for (int i = effective_argc - 1; i >= 0; i--) {
        const char *s   = argv[i] ? argv[i] : "";
        size_t      len = cai_slen(s) + 1;
        str_ptr -= (uint64_t)len;
        for (size_t j = 0; j < len; j++)
            cai_mem_w8(as, str_ptr + j, (uint8_t)s[j]);
        argv_gvas[i] = str_ptr;
    }

    /* Align to 16 bytes (AArch64 and x86-64 requirement) */
    str_ptr &= ~(uint64_t)15;

    uint64_t sp = str_ptr;

    /* AT_NULL auxiliary vector */
    sp -= 8; cai_mem_w64(as, sp, 0); /* value */
    sp -= 8; cai_mem_w64(as, sp, 0); /* AT_NULL */

    /* envp NULL */
    sp -= 8; cai_mem_w64(as, sp, 0);

    /* argv[argc] = NULL */
    sp -= 8; cai_mem_w64(as, sp, 0);
    for (int i = effective_argc - 1; i >= 0; i--) {
        sp -= 8;
        cai_mem_w64(as, sp, argv_gvas[i]);
    }

    /* argc */
    sp -= 8;
    cai_mem_w64(as, sp, (uint64_t)effective_argc);

    *sp_out = sp;
    return CAI_OK;
}

/* =========================================================================
 * cai_elf_load – main entry point
 * ========================================================================= */

int cai_elf_load(const uint8_t *elf_data, size_t elf_size,
                 cai_arch_t target_arch, cai_address_space_t *as,
                 int argc, char **argv,
                 cai_elf_load_result_t *result)
{
    if (!elf_data || elf_size < CAI_EI_NIDENT || !as || !result)
        return CAI_EINVAL;

    if (check_elf_magic(elf_data) != 0) {
        debuglog(DEBUG_WARN, "cai_elf_load: bad ELF magic\n");
        return CAI_EINVAL;
    }

    int rc;

    switch (target_arch) {
    case CAI_ARCH_X86_32:
        rc = load_elf32(elf_data, elf_size, CAI_EM_386, as, result);
        break;
    case CAI_ARCH_ARM32:
        rc = load_elf32(elf_data, elf_size, CAI_EM_ARM, as, result);
        break;
    case CAI_ARCH_X86_64:
        rc = load_elf64(elf_data, elf_size, CAI_EM_X86_64, as, result);
        break;
    case CAI_ARCH_AARCH64:
        rc = load_elf64(elf_data, elf_size, CAI_EM_AARCH64, as, result);
        break;
    default:
        debuglog(DEBUG_WARN, "cai_elf_load: unsupported arch %d\n",
                 (int)target_arch);
        return CAI_ENOTSUP;
    }

    if (rc != CAI_OK)
        return rc;

    /* Set up the initial stack */
    uint64_t sp = 0;
    if (target_arch == CAI_ARCH_X86_32 || target_arch == CAI_ARCH_ARM32)
        rc = setup_stack32(as, argc, argv, &sp);
    else
        rc = setup_stack64(as, argc, argv, &sp);

    if (rc != CAI_OK)
        return rc;

    result->stack_top = sp;

    debuglog(DEBUG_INFO,
             "cai_elf_load: arch=%d entry=0x%llx sp=0x%llx "
             "load=[0x%llx,0x%llx)\n",
             (int)target_arch,
             (unsigned long long)result->entry_point,
             (unsigned long long)result->stack_top,
             (unsigned long long)result->load_base,
             (unsigned long long)result->load_end);

    return CAI_OK;
}
