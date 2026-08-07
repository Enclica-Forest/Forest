/*
 * Fern AArch64 - Boot constant definitions (used in both ASM and C)
 */
#ifndef AARCH64_BOOT_DEFS_H
#define AARCH64_BOOT_DEFS_H

/*
 * SCTLR_EL1 reset value with required RES1 bits set.
 * M=0 (MMU off), C=0 (D-cache off), I=0 (I-cache off).
 * RES1 bits: 11, 20, 22, 23, 28, 29 (per ARMv8-A ref manual).
 */
#define SCTLR_EL1_RES1  0x00C50830

/* SCTLR_EL1 control bits */
#define SCTLR_EL1_M     (1 << 0)    /* MMU enable                */
#define SCTLR_EL1_A     (1 << 1)    /* Alignment fault enable    */
#define SCTLR_EL1_C     (1 << 2)    /* Data cache enable         */
#define SCTLR_EL1_SA    (1 << 3)    /* Stack alignment check     */
#define SCTLR_EL1_SA0   (1 << 4)    /* EL0 stack alignment check */
#define SCTLR_EL1_I     (1 << 12)   /* I-cache enable            */
#define SCTLR_EL1_WXN   (1 << 19)   /* Write implies XN          */

/* HCR_EL2 bits */
#define HCR_EL2_RW      (1 << 31)   /* EL1 is AArch64            */
#define HCR_EL2_AMO    (1 << 5)
#define HCR_EL2_IMO    (1 << 4)
#define HCR_EL2_FMO    (1 << 3)

#endif /* AARCH64_BOOT_DEFS_H */
