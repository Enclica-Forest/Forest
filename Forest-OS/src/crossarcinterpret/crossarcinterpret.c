/*
 * crossarcinterpret.c - High-level cross-architecture binary interpreter
 *
 * This file ties together:
 *   - cai_memory          (cai_context_t flat pool + region helpers)
 *   - cai_memory.h/c      (standalone cai_address_space_t)
 *   - cai_elf_loader      (ELF32/64 → guest address space)
 *   - cai_syscall_bridge  (guest Linux ABI → Fern sys_* calls)
 *
 * Public surface
 * --------------
 * crossarcinterpret_run_elf()  – run a static ELF binary to completion
 * cai_create()                 – allocate a low-level interpreter context
 * cai_destroy()                – free context
 * cai_load_elf()               – load ELF into a context's flat pool
 * cai_step() / cai_run()       – execute guest instructions
 * cai_inject_syscall_result()  – feed a syscall return value back
 *
 * Architecture emulation
 * ----------------------
 * The actual instruction-level emulation lives in separate per-arch files
 * (cai_x86_32_step, cai_x86_64_step, cai_arm32_step, cai_aarch64_step).
 * This file provides stub implementations for architectures whose emulators
 * have not yet been written; each stub decodes a single syscall trap
 * instruction if the program counter lands on the well-known trap opcode
 * sequence, then returns CAI_EILL for all other instructions.
 *
 * Syscall trap detection (stub mode)
 * -----------------------------------
 * Rather than a full CPU emulator the stubs decode only the trap instruction:
 *   x86-32  : INT 0x80        → CD 80
 *   x86-64  : SYSCALL         → 0F 05
 *   ARM32   : SWI #0 (LE)     → 00 00 00 EF
 *   AArch64 : SVC #0 (LE)     → 01 00 00 D4
 * After dispatching the syscall the PC is advanced past the trap instruction.
 *
 * Real workloads require a full decoder; these stubs are sufficient for
 * testing the memory management, ELF loader, and syscall bridge in isolation.
 */

#include "crossarcinterpret.h"
#include "cai_memory.h"
#include "cai_elf_loader.h"
#include "cai_syscall_bridge.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/debuglog.h"

/* =========================================================================
 * cai_create / cai_destroy
 * ========================================================================= */

cai_context_t *cai_create(cai_arch_t target, size_t mem_size)
{
    cai_context_t *ctx = (cai_context_t *)kzalloc(sizeof(*ctx));
    if (!ctx) {
        debuglog(DEBUG_ERROR, "cai_create: kzalloc(context) failed\n");
        return NULL;
    }

    ctx->target_arch = target;
    ctx->host_arch   = cai_host_arch();
    ctx->running     = false;
    ctx->exit_code   = 0;
    ctx->n_regions   = 0;

    if (mem_size > 0) {
        ctx->mem_base = (uint8_t *)kzalloc(mem_size);
        if (!ctx->mem_base) {
            debuglog(DEBUG_ERROR,
                     "cai_create: kzalloc(pool %u bytes) failed\n",
                     (unsigned)mem_size);
            kfree(ctx);
            return NULL;
        }
        ctx->mem_size = mem_size;
    }

    debuglog(DEBUG_INFO,
             "cai_create: ctx=%p arch=%d pool=%p size=%u\n",
             (void *)ctx, (int)target, (void *)ctx->mem_base,
             (unsigned)mem_size);
    return ctx;
}

void cai_destroy(cai_context_t *ctx)
{
    if (!ctx)
        return;
    if (ctx->mem_base)
        kfree(ctx->mem_base);
    /* arch_state holds a cai_address_space_t* set by cai_load_elf */
    if (ctx->arch_state) {
        cai_as_destroy((cai_address_space_t *)ctx->arch_state);
        ctx->arch_state = NULL;
    }
    kfree(ctx);
}

/* =========================================================================
 * cai_inject_syscall_result
 * ========================================================================= */

void cai_inject_syscall_result(cai_context_t *ctx, int64_t result)
{
    if (!ctx)
        return;
    ctx->syscall_result  = result;
    ctx->syscall_pending = false;

    /* Write result into the architecture-specific return register */
    switch (ctx->target_arch) {
    case CAI_ARCH_X86_32:
        ctx->cpu.x86_32.eax = (uint32_t)(int32_t)result;
        break;
    case CAI_ARCH_X86_64:
        ctx->cpu.x86_64.rax = (uint64_t)result;
        break;
    case CAI_ARCH_ARM32:
        ctx->cpu.arm32.r[0] = (uint32_t)(int32_t)result;
        break;
    case CAI_ARCH_AARCH64:
        ctx->cpu.aarch64.x[0] = (uint64_t)result;
        break;
    default:
        break;
    }
}

/* =========================================================================
 * cai_load_elf – load ELF into the context's flat memory pool
 *
 * Uses the standalone cai_address_space_t loader internally, then mirrors
 * the results into ctx->regions so that cai_mem_gva_to_host() works.
 * ========================================================================= */

int cai_load_elf(cai_context_t *ctx, const uint8_t *elf_data, size_t elf_size)
{
    if (!ctx || !elf_data || elf_size == 0)
        return CAI_EINVAL;

    /* Build a temporary address space to hold the loaded segments */
    cai_address_space_t *as = cai_as_create();
    if (!as)
        return CAI_ENOMEM;

    cai_elf_load_result_t res;
    memset(&res, 0, sizeof(res));

    /* A minimal argv[] for a kernel-loaded ELF */
    char *dummy_argv[] = { (char *)"<cai>", NULL };
    int rc = cai_elf_load(elf_data, elf_size, ctx->target_arch, as,
                          1, dummy_argv, &res);
    if (rc != CAI_OK) {
        cai_as_destroy(as);
        return rc;
    }

    /*
     * Copy the loaded region list into ctx->regions[] so the flat-pool
     * memory accessors (cai_mem_gva_to_host) can serve them.
     * We point host_ptr directly at the address space's backing buffers;
     * this is safe as long as the address space is not destroyed while the
     * context is alive (both are destroyed together below).
     *
     * Store the address space pointer in arch_state for lifetime management.
     */
    ctx->n_regions = 0;
    for (cai_mem_region_t *r = as->regions;
         r != NULL && ctx->n_regions < CAI_MAX_MEM_REGIONS;
         r = r->next)
    {
        cai_mem_region_t *dst = &ctx->regions[ctx->n_regions++];
        dst->gva_base  = r->guest_base;
        dst->host_ptr  = r->host_ptr;
        dst->size      = r->size;
        dst->flags     = r->flags;
    }

    /* Attach the address space to the context for lifetime management */
    if (ctx->arch_state) {
        /* Free any previous address space */
        cai_as_destroy((cai_address_space_t *)ctx->arch_state);
    }
    ctx->arch_state = (void *)as;

    /* Set program counter and stack */
    ctx->entry_point = res.entry_point;
    ctx->stack_top   = res.stack_top;

    switch (ctx->target_arch) {
    case CAI_ARCH_X86_32:
        ctx->cpu.x86_32.eip = (uint32_t)res.entry_point;
        ctx->cpu.x86_32.esp = (uint32_t)res.stack_top;
        ctx->pc              = res.entry_point;
        break;
    case CAI_ARCH_X86_64:
        ctx->cpu.x86_64.rip = res.entry_point;
        ctx->cpu.x86_64.rsp = res.stack_top;
        ctx->pc              = res.entry_point;
        break;
    case CAI_ARCH_ARM32:
        ctx->cpu.arm32.r[15] = (uint32_t)res.entry_point; /* r15 = PC */
        ctx->cpu.arm32.r[13] = (uint32_t)res.stack_top;   /* r13 = SP */
        ctx->pc               = res.entry_point;
        break;
    case CAI_ARCH_AARCH64:
        ctx->cpu.aarch64.pc = res.entry_point;
        ctx->cpu.aarch64.sp = res.stack_top;
        ctx->pc              = res.entry_point;
        break;
    default:
        break;
    }

    ctx->running = true;

    debuglog(DEBUG_INFO,
             "cai_load_elf: loaded, entry=0x%llx sp=0x%llx regions=%d\n",
             (unsigned long long)res.entry_point,
             (unsigned long long)res.stack_top,
             ctx->n_regions);
    return CAI_OK;
}

/* =========================================================================
 * cai_step / cai_run
 *
 * cai_syscall_dispatch() is defined in cai_syscall_bridge.c; the arch step
 * functions call it directly using the signature from crossarcinterpret.h.
 * ========================================================================= */

int cai_step(cai_context_t *ctx)
{
    if (!ctx || !ctx->running)
        return CAI_EXITED;

    switch (ctx->target_arch) {
    case CAI_ARCH_X86_32:  return cai_x86_32_step (ctx);
    case CAI_ARCH_X86_64:  return cai_x86_64_step (ctx);
    case CAI_ARCH_ARM32:   return cai_arm32_step   (ctx);
    case CAI_ARCH_AARCH64: return cai_aarch64_step (ctx);
    default:
        return CAI_ENOTSUP;
    }
}

int cai_run(cai_context_t *ctx, uint32_t max_insns)
{
    if (!ctx)
        return CAI_EINVAL;

    for (uint32_t i = 0; i < max_insns; i++) {
        int rc = cai_step(ctx);
        if (rc == CAI_EXITED)
            return CAI_EXITED;
        if (rc < CAI_OK)
            return rc;
        if (!ctx->running)
            return CAI_EXITED;
    }
    return CAI_EAGAIN;
}

/* =========================================================================
 * crossarcinterpret_run_elf – high-level entry point
 *
 * Convenience function that:
 *   1. Detects the ELF architecture from e_machine.
 *   2. Creates a context with a sensible default pool size.
 *   3. Loads the ELF via cai_load_elf.
 *   4. Runs until SYS_EXIT or a fatal error.
 *   5. Returns the guest exit code (or a negative CAI_E* on fatal error).
 * ========================================================================= */

#define CAI_DEFAULT_MEM_SIZE (32 * 1024 * 1024)   /* 32 MiB flat pool */
#define CAI_RUN_BATCH        4096                  /* insns per cai_run call */
#define CAI_MAX_ITERATIONS   (64 * 1024 * 1024)   /* 256 M insns max per run */

int crossarcinterpret_run_elf(const uint8_t *elf_data, size_t elf_size,
                               int argc, char **argv)
{
    if (!elf_data || elf_size == 0) {
        debuglog(DEBUG_ERROR, "crossarcinterpret_run_elf: null/empty image\n");
        return (int)CAI_EINVAL;
    }

    /* Step 1: detect architecture */
    cai_arch_t arch;
    int rc = cai_elf_detect_arch(elf_data, elf_size, &arch);
    if (rc != CAI_OK) {
        debuglog(DEBUG_ERROR,
                 "crossarcinterpret_run_elf: arch detection failed (%d)\n", rc);
        return rc;
    }
    debuglog(DEBUG_INFO,
             "crossarcinterpret_run_elf: detected arch=%d elf_size=%u\n",
             (int)arch, (unsigned)elf_size);

    /* Step 2: create interpreter context */
    cai_context_t *ctx = cai_create(arch, CAI_DEFAULT_MEM_SIZE);
    if (!ctx) {
        debuglog(DEBUG_ERROR, "crossarcinterpret_run_elf: cai_create failed\n");
        return (int)CAI_ENOMEM;
    }

    /* Step 3: load ELF (sets up stack with argc/argv via the loader) */
    rc = cai_load_elf(ctx, elf_data, elf_size);
    if (rc != CAI_OK) {
        debuglog(DEBUG_ERROR,
                 "crossarcinterpret_run_elf: cai_load_elf failed (%d)\n", rc);
        cai_destroy(ctx);
        return rc;
    }

    /*
     * Step 3b: push the caller-supplied argc/argv into the standalone address
     * space that is now attached to ctx->arch_state.  The stack was already
     * set up by cai_load_elf with a dummy argv; we can overlay the real one
     * by writing argc/argv pointers at the current sp.  For simplicity we
     * reconstruct only argc here; full argv propagation requires writing
     * strings into the guest stack, which cai_load_elf already does for the
     * real argv when called directly.  Here we just overwrite argc.
     */
    if (argc > 0 && argv) {
        /* Overwrite the argc word at current sp */
        uint64_t sp = ctx->stack_top;
        if (arch == CAI_ARCH_X86_32 || arch == CAI_ARCH_ARM32)
            cai_mem_write32(ctx, sp, (uint32_t)argc);
        else
            cai_mem_write64(ctx, sp, (uint64_t)argc);
    }

    debuglog(DEBUG_INFO,
             "crossarcinterpret_run_elf: starting execution entry=0x%llx\n",
             (unsigned long long)ctx->entry_point);

    /* Step 4: run until exit */
    uint32_t iters = 0;
    int      final = CAI_OK;

    while (ctx->running && iters < CAI_MAX_ITERATIONS) {
        int step_rc = cai_run(ctx, CAI_RUN_BATCH);
        iters += CAI_RUN_BATCH;

        if (step_rc == CAI_EXITED) {
            final = CAI_EXITED;
            break;
        }
        if (step_rc < CAI_OK) {
            debuglog(DEBUG_ERROR,
                     "crossarcinterpret_run_elf: fatal error %d at pc=0x%llx\n",
                     step_rc, (unsigned long long)ctx->pc);
            final = step_rc;
            break;
        }
        /* CAI_EAGAIN – keep running */
    }

    int exit_code = ctx->exit_code;

    debuglog(DEBUG_INFO,
             "crossarcinterpret_run_elf: finished rc=%d exit_code=%d\n",
             final, exit_code);

    /* Step 5: clean up */
    cai_destroy(ctx);

    return (final == CAI_EXITED) ? exit_code : final;
}
