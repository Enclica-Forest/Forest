/*
 * fault.h - ARM32 fault handler interface for Forest OS
 *
 * DFSR/IFSR status codes (bits [5:0]) from the ARM Architecture Reference
 * Manual (ARMv7-A/R DDI 0406C, B3.10.1 "Data Fault Status Register").
 *
 * Each status code describes why the MMU aborted a data access or
 * instruction fetch.  The fault.c handler decodes these to print
 * diagnostics and (future) invoke the VMM demand-paging path.
 */

#ifndef ARM32_FAULT_H
#define ARM32_FAULT_H

#include <stdint.h>

/* DFSR/IFSR bit layout (ARMv7) */
#define DFSR_STATUS_MASK    0x0000003FU  /* bits [5:0] = DFSC / IFSC status */
#define DFSR_WNR            (1U << 6)    /* Write not Read: 1 = write fault   */
#define DFSR_EA             (1U << 9)    /* External Abort (implementation)   */
#define DFSR_CACHMaint      (1U << 11)   /* Cache maintenance operation       */

/*
 * DFSR/IFSR fault status codes (bits [5:0]).
 *
 * Translation faults: page not mapped.
 * Permission faults: page mapped but access denied.
 * External faults: memory bus error.
 */
#define FS_ADDRESS_SIZE_FAULT       0x00U
#define FS_ALIGNMENT_FAULT          0x01U
#define FS_IC_CACHE_MAINT_FAULT     0x02U
#define FS_TRANSLATION_FAULT_L1     0x04U
#define FS_TRANSLATION_FAULT_L2     0x06U
#define FS_PERMISSION_FAULT_L1      0x05U
#define FS_PERMISSION_FAULT_L2      0x07U
#define FS_PRECISE_EXT_ABORT_L1     0x08U
#define FS_PRECISE_EXT_ABORT_L2     0x0AU
#define FS_DOMAIN_FAULT_L1          0x09U
#define FS_DOMAIN_FAULT_L2          0x0BU
#define FS_ASYNC_EXT_ABORT          0x0CU
#define FS_TLB_CONFLICT_ABORT       0x10U
#define FS_IMPRECISE_EXT_ABORT      0x16U
#define FS_ECC_ERROR                0x18U

/* Virtual address boundary between user and kernel space */
#define ARM_USER_SPACE_LIMIT    0x80000000U

#endif /* ARM32_FAULT_H */
