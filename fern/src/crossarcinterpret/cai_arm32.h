/*
 * cai_arm32.h - ARM32 (ARMv7) instruction interpreter for crossarcinterpret
 *
 * Provides per-instruction decode and emulation of ARM32 (little-endian,
 * ARMv7-A) ELF binaries running under the Fern cross-architecture
 * interpreter framework.
 *
 * Register file, CPSR layout, and instruction encodings follow:
 *   ARM Architecture Reference Manual ARMv7-A and ARMv7-R edition (ARM DDI 0406C)
 */

#ifndef CAI_ARM32_H
#define CAI_ARM32_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* =========================================================================
 * ARM32 register file
 * ========================================================================= */

typedef struct arm32_regs {
    uint32_t r[16];   /* r0-r15  (r13=SP, r14=LR, r15=PC)                   */
    uint32_t cpsr;    /* Current Program Status Register                      */
    uint32_t spsr;    /* Saved Program Status Register (banked per mode)      */
} arm32_regs_t;

/* =========================================================================
 * CPSR / SPSR bit definitions  (ARM DDI 0406C §B1.3.3)
 * ========================================================================= */

#define ARM_CPSR_N  (1u << 31)  /* Negative / Less Than flag                */
#define ARM_CPSR_Z  (1u << 30)  /* Zero flag                                */
#define ARM_CPSR_C  (1u << 29)  /* Carry / Borrow / Extend flag             */
#define ARM_CPSR_V  (1u << 28)  /* Overflow flag                            */
#define ARM_CPSR_Q  (1u << 27)  /* Saturation / Sticky overflow flag        */
#define ARM_CPSR_IT (0xFFu << 10)/* If-Then execution state (Thumb-2)       */
#define ARM_CPSR_J  (1u <<  24) /* Jazelle state (unused here)              */
#define ARM_CPSR_E  (1u <<  9)  /* Endianness execution state (0=LE)        */
#define ARM_CPSR_A  (1u <<  8)  /* Asynchronous abort disable              */
#define ARM_CPSR_I  (1u <<  7)  /* IRQ disable                              */
#define ARM_CPSR_F  (1u <<  6)  /* FIQ disable                              */
#define ARM_CPSR_T  (1u <<  5)  /* Thumb execution state                    */
#define ARM_CPSR_M  (0x1Fu)     /* Mode field mask                          */

/* Processor mode field values */
#define ARM_MODE_USR 0x10u  /* User mode                                     */
#define ARM_MODE_FIQ 0x11u  /* FIQ mode                                      */
#define ARM_MODE_IRQ 0x12u  /* IRQ mode                                      */
#define ARM_MODE_SVC 0x13u  /* Supervisor mode                               */
#define ARM_MODE_ABT 0x17u  /* Abort mode                                    */
#define ARM_MODE_UND 0x1Bu  /* Undefined instruction mode                    */
#define ARM_MODE_SYS 0x1Fu  /* System mode (privileged user)                 */

/* =========================================================================
 * Register number aliases
 * ========================================================================= */

#define ARM_SP  13   /* Stack Pointer                                         */
#define ARM_LR  14   /* Link Register                                         */
#define ARM_PC  15   /* Program Counter                                       */

/* =========================================================================
 * Standalone ARM32 interpreter context
 *
 * Used when the file is consumed outside the full cai_context_t framework
 * (e.g. unit tests).  The main interpreter uses cai_arm32_step() which
 * operates directly on cai_context_t.
 * ========================================================================= */

typedef struct cai_arm32_ctx {
    arm32_regs_t  regs;        /* CPU register file                          */
    uint8_t      *mem;         /* Flat guest memory buffer                   */
    size_t        mem_size;    /* Size of the buffer in bytes                */
    uint32_t      mem_base;    /* Guest virtual address of buffer start      */
    bool          running;     /* Execution loop control flag                */
    bool          thumb_mode;  /* true when in Thumb state                   */
    int           exit_code;   /* Exit code (set on SWI exit syscall)        */
} cai_arm32_ctx_t;

/* =========================================================================
 * Public API for the standalone context
 * ========================================================================= */

/*
 * cai_arm32_create - Allocate and initialise a standalone ARM32 context.
 * @mem_size : size of the flat guest memory pool to allocate.
 * Returns NULL on allocation failure.
 */
cai_arm32_ctx_t *cai_arm32_create(size_t mem_size);

/*
 * cai_arm32_destroy - Release all resources.
 */
void cai_arm32_destroy(cai_arm32_ctx_t *ctx);

/*
 * cai_arm32_step_sa - Execute one instruction in the standalone context.
 * (The canonical cai_arm32_step(cai_context_t*) is in crossarcinterpret.h.)
 * Returns 0 on success, negative on error, 2 on exit.
 */
int cai_arm32_step_sa(cai_arm32_ctx_t *ctx);

/*
 * cai_arm32_run - Execute up to @max_steps instructions.
 * Returns 0 (CAI_OK), 2 (CAI_EXITED), or negative error code.
 */
int cai_arm32_run(cai_arm32_ctx_t *ctx, int max_steps);

/*
 * cai_arm32_load_elf - Load an ARM32 ELF binary into the standalone context.
 * @elf  : pointer to ELF image in host memory.
 * @size : byte length of the ELF image.
 * Returns 0 on success, negative on error.
 */
int cai_arm32_load_elf(cai_arm32_ctx_t *ctx, const uint8_t *elf, size_t size);

#endif /* CAI_ARM32_H */
