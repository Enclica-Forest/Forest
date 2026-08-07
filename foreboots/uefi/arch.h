/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/arch.h - tiny architecture-abstraction layer for the UEFI loader
 * =============================================================================
 * ForeB's UEFI payload targets three UEFI arches from one C source tree:
 *
 *     x86_64  (BOOTX64.EFI)     - full loader: Forest multiboot handoff + UI +
 *                                 shell + Linux boot + chainload.
 *     aarch64 (BOOTAA64.EFI)    - UI + shell + Linux boot + chainload. NO
 *                                 multiboot Forest handoff (x86-only protocol).
 *     riscv64 (BOOTRISCV64.EFI) - same feature set as aarch64. See the PACKAGING
 *                                 BOUNDARY note below: the C compiles for RISC-V
 *                                 but producing the final PE needs edk2 GenFw.
 *
 * This header centralises every "which arch am I" decision so the rest of the
 * code says WHAT it wants (e.g. FOREB_MULTIBOOT_SUPPORTED) instead of repeating
 * compiler #ifdefs. It has no dependencies beyond efi.h.
 * ========================================================================== */

#ifndef FOREB_ARCH_H
#define FOREB_ARCH_H

#include "efi.h"

/* -----------------------------------------------------------------------------
 * 1. Arch detection from compiler predefines.
 * Exactly one FOREB_ARCH_* is 1. FOREB_ARCH is a small enum-like integer.
 * -------------------------------------------------------------------------- */
#define FOREB_ARCH_UNKNOWN  0
#define FOREB_ARCH_X64      1
#define FOREB_ARCH_AA64     2
#define FOREB_ARCH_RISCV    3

#if defined(__x86_64__) || defined(_M_X64)
#  define FOREB_ARCH        FOREB_ARCH_X64
#  define FOREB_ARCH_IS_X64    1
#  define FOREB_ARCH_IS_AA64   0
#  define FOREB_ARCH_IS_RISCV  0
#  define FOREB_ARCH_NAME   "x86_64"
#  define FOREB_EFI_IMAGE_NAME  "BOOTX64.EFI"
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define FOREB_ARCH        FOREB_ARCH_AA64
#  define FOREB_ARCH_IS_X64    0
#  define FOREB_ARCH_IS_AA64   1
#  define FOREB_ARCH_IS_RISCV  0
#  define FOREB_ARCH_NAME   "aarch64"
#  define FOREB_EFI_IMAGE_NAME  "BOOTAA64.EFI"
#elif defined(__riscv) && (__riscv_xlen == 64)
#  define FOREB_ARCH        FOREB_ARCH_RISCV
#  define FOREB_ARCH_IS_X64    0
#  define FOREB_ARCH_IS_AA64   0
#  define FOREB_ARCH_IS_RISCV  1
#  define FOREB_ARCH_NAME   "riscv64"
#  define FOREB_EFI_IMAGE_NAME  "BOOTRISCV64.EFI"
#else
#  define FOREB_ARCH        FOREB_ARCH_UNKNOWN
#  define FOREB_ARCH_IS_X64    0
#  define FOREB_ARCH_IS_AA64   0
#  define FOREB_ARCH_IS_RISCV  0
#  define FOREB_ARCH_NAME   "unknown"
#  define FOREB_EFI_IMAGE_NAME  "BOOT.EFI"
#endif

/* -----------------------------------------------------------------------------
 * 2. Calling convention note (EFIAPI).
 * efi.h defines EFIAPI = __attribute__((ms_abi)) for every GNU/clang target.
 * That is CORRECT for x86_64 and aarch64 UEFI. For RISC-V UEFI the STANDARD
 * RISC-V C ABI is used and ms_abi is meaningless -- clang emits a warning and
 * ignores it, which is harmless (the ABI is right anyway). FOREB_EFIAPI below
 * is the "clean" per-arch convention for any NEW firmware entry points ForeB
 * declares; existing efi.h typedefs keep using efi.h's EFIAPI.
 *
 * BOUNDARY: to silence the RISC-V warning entirely, guard efi.h's EFIAPI define
 * with `#if defined(__x86_64__) || defined(__aarch64__)`. Left as a one-line
 * follow-up so this abstraction stays additive and never touches efi.h's ABI on
 * the shipping x86/ARM builds.
 * -------------------------------------------------------------------------- */
#if FOREB_ARCH_IS_X64 || FOREB_ARCH_IS_AA64
#  define FOREB_EFIAPI __attribute__((ms_abi))
#else
#  define FOREB_EFIAPI /* RISC-V UEFI: standard C ABI */
#endif

/* -----------------------------------------------------------------------------
 * 3. Feature gating.
 * The Forest kernel is delivered via the x86 Multiboot1 protocol (EAX=0x2BADB002,
 * EBX=pointer to multiboot_info, a far transition to 32-bit protected mode via
 * handoff64to32.asm). That entire path is x86-only. Linux boot (LoadImage of an
 * EFI-stub vmlinuz) and chainload (LoadImage of another BOOTXXX.EFI) are pure
 * UEFI and work on ALL three arches.
 * -------------------------------------------------------------------------- */
#if FOREB_ARCH_IS_X64
#  define FOREB_MULTIBOOT_SUPPORTED  1   /* Forest-kernel handoff available     */
#else
#  define FOREB_MULTIBOOT_SUPPORTED  0   /* menu shows Forest entries greyed/n/a */
#endif

/* Linux-stub + chainload are available everywhere UEFI runs. */
#define FOREB_LINUX_BOOT_SUPPORTED   1
#define FOREB_CHAINLOAD_SUPPORTED    1

/* -----------------------------------------------------------------------------
 * 4. Multiboot handoff facade.
 * On x86_64 the real handoff lives in handoff64to32.asm + bootx64.c and is
 * declared/called there behind FOREB_MULTIBOOT_SUPPORTED. On other arches we
 * provide a no-op stub that reports the limitation to ConOut and returns, so a
 * `type=forest` entry degrades gracefully instead of failing to link. Callers:
 *
 *     if (FOREB_MULTIBOOT_SUPPORTED) { ... real handoff ... }
 *     else foreb_multiboot_unsupported(gST);
 * -------------------------------------------------------------------------- */
#if !FOREB_MULTIBOOT_SUPPORTED
/* Wide-string form of the arch name for the CHAR16 message below. */
#  if FOREB_ARCH_IS_AA64
#    define FOREB_ARCH_NAME_W  L"aarch64"
#  elif FOREB_ARCH_IS_RISCV
#    define FOREB_ARCH_NAME_W  L"riscv64"
#  else
#    define FOREB_ARCH_NAME_W  L"this arch"
#  endif
static inline void foreb_multiboot_unsupported(EFI_SYSTEM_TABLE *st) {
    if (st && st->ConOut && st->ConOut->OutputString) {
        st->ConOut->OutputString(st->ConOut,
            L"[ForeB] Forest multiboot handoff is unsupported on "
            FOREB_ARCH_NAME_W L". Use a 'linux' or 'chainload' entry.\r\n");
    }
}
#endif

/* -----------------------------------------------------------------------------
 * 5. Build-flag reminders (informational; the Makefile is the source of truth).
 *   x86_64 : clang -target x86_64-unknown-windows  -mno-red-zone -mno-mmx -mno-sse
 *   aarch64: clang -target aarch64-unknown-windows -mno-red-zone -mgeneral-regs-only
 *            (drop -mno-mmx/-mno-sse; those are x86-only)
 *   riscv64: clang -target riscv64-unknown-elf     -mno-relax  (ELF object only;
 *            ELF->PE needs edk2 GenFw -- clang/lld have no RISC-V COFF backend).
 * -------------------------------------------------------------------------- */

#endif /* FOREB_ARCH_H */
