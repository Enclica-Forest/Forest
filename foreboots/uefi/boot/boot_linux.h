/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/boot_linux.h - Boot a Linux 'vmlinuz' via the modern EFI-stub path.
 * =============================================================================
 * Implemented in uefi/boot_linux.c. Runs BEFORE ExitBootServices (BootServices
 * still live). This is the `type=linux` boot method: a modern vmlinuz is itself
 * an EFI application (the "EFI stub"), so we hand it to the firmware just like
 * any other EFI binary and let IT set the machine up, rather than doing the
 * bare bzImage boot-protocol handoff ourselves. Concretely:
 *
 *   1. Build a full device path to the vmlinuz file on ForeB's own ESP (the
 *      volume the loader was launched from): <ESP device path> + a
 *      MEDIA_FILEPATH node naming the kernel.
 *   2. BootServices->LoadImage() that device path -> a fresh image handle.
 *   3. Point the new image's LoadedImage->LoadOptions at the UTF-16 kernel
 *      command line (this is how the EFI stub receives `root=`, `quiet`, ...).
 *   4. Expose the initrd through the Linux initrd media protocol: install an
 *      EFI_LOAD_FILE2_PROTOCOL on a throw-away handle whose device path is a
 *      MEDIA_VENDOR node carrying LINUX_EFI_INITRD_MEDIA_GUID. The EFI stub
 *      locates exactly that handle and pulls the initrd bytes from our
 *      LoadFile callback. (This is the mechanism used by systemd-boot/GRUB and
 *      is the ONLY supported initrd path on AArch64.)
 *   5. BootServices->StartImage() the kernel. On success Linux takes over and
 *      never returns; if it DOES return, we tear the initrd handle down, unload
 *      the image, free buffers and hand an EFI_STATUS back to the menu.
 *
 * All work is device-path + protocol based (no firmware text<->path helper is
 * required), so the exact same code compiles and runs on x86_64, AArch64 and
 * RISC-V UEFI.
 * =============================================================================
 */
#ifndef FOREB_UEFI_BOOT_LINUX_H
#define FOREB_UEFI_BOOT_LINUX_H

#include "../efi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Boot a Linux kernel via its EFI stub.
 *
 *   parent_image - the ForeB image handle passed to efi_main (used to resolve
 *                  the ESP volume the kernel/initrd live on and as the parent of
 *                  the LoadImage call).
 *   st           - live EFI system table (BootServices must still be valid;
 *                  call this BEFORE ExitBootServices).
 *   vmlinuz_path - ASCII ESP path of the EFI-stub kernel, e.g.
 *                  "/forebo/vmlinuz" or "\\EFI\\Linux\\vmlinuz-6.9". '/' and
 *                  '\\' separators are both accepted.
 *   initrd_path  - ASCII ESP path of the initramfs, or NULL/"" for no initrd.
 *   cmdline      - ASCII kernel command line, or NULL/"" for none. Passed to the
 *                  stub verbatim as LoadOptions (UTF-16).
 *
 * Returns:
 *   Does NOT return on a successful boot (Linux takes the machine). If it
 *   returns, the value is the failure status: EFI_NOT_FOUND (kernel/initrd
 *   missing), EFI_LOAD_ERROR (LoadImage rejected the file / not an EFI stub),
 *   EFI_INVALID_PARAMETER (bad args), or whatever StartImage reported when the
 *   kernel exited back to us.
 */
EFI_STATUS linux_boot(EFI_HANDLE parent_image, EFI_SYSTEM_TABLE *st,
                      const char *vmlinuz_path, const char *initrd_path,
                      const char *cmdline);

#ifdef __cplusplus
}
#endif

#endif /* FOREB_UEFI_BOOT_LINUX_H */
