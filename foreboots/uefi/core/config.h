/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/config.h - ESP file access + forebo.cfg loader (public API).
 * =============================================================================
 * Implemented in uefi/config.c. Freestanding (no libc); links against the same
 * EFI type/protocol definitions the rest of the UEFI loader uses (efi.h) and
 * the parsed-config data model (include/forebo_cfg.h).
 *
 * These routines run BEFORE ExitBootServices (BootServices still live): they
 * open the ESP volume via the running image's LoadedImage->DeviceHandle exactly
 * like bootx64.c's load_kernel_file(), read a whole file into a pool buffer, and
 * parse forebo.cfg into a POD struct forebo_config that stays valid afterwards.
 *
 * The caller (bootx64.c) owns the gBS pointer and its own EFI_HANDLE (the image
 * handle passed to efi_main); both are threaded through explicitly so config.c
 * needs no globals and no init handshake.
 * =============================================================================
 */
#ifndef FOREB_UEFI_CONFIG_H
#define FOREB_UEFI_CONFIG_H

#include "../efi.h"
#include "../../include/forebo_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * ESP file helpers (also reused by uefi/modules.c and image/icon loaders).
 * -------------------------------------------------------------------------- */

/*
 * Open the root directory of the volume the loader image was launched from
 * (its ESP). Mirrors load_kernel_file(): HandleProtocol(LoadedImage) ->
 * HandleProtocol(SimpleFileSystem) on DeviceHandle -> OpenVolume. On success
 * *out_root is an open EFI_FILE_PROTOCOL the caller must Close(). Returns an
 * EFI_STATUS; on error *out_root is left NULL.
 */
EFI_STATUS esp_open_root(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                         EFI_FILE_PROTOCOL **out_root);

/*
 * Convert an ASCII ESP path (using '/' or '\' separators, e.g. "/forebo/x")
 * into a NUL-terminated CHAR16 path with '\' separators suitable for
 * EFI_FILE_PROTOCOL.Open(). 'cap' is the CHAR16 capacity of 'out' incl. NUL.
 */
void esp_ascii_to_char16(const char *in, CHAR16 *out, UINTN cap);

/*
 * Read an entire ESP file into a freshly AllocatePool(EfiLoaderData) buffer.
 * On success *out_buf points at the bytes and *out_size is the length; the
 * caller frees with esp_free_file(). Returns EFI_SUCCESS or an error (buffer
 * left NULL / size 0 on failure).
 */
EFI_STATUS esp_read_file(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                         const char *path, void **out_buf, UINTN *out_size);

/* Free a buffer returned by esp_read_file (NULL-safe). */
void esp_free_file(EFI_BOOT_SERVICES *bs, void *buf);

/* -----------------------------------------------------------------------------
 * Config loader.
 * -------------------------------------------------------------------------- */

/*
 * Load and parse the ForeB config file from the ESP into *cfg. If the file is
 * absent or empty/malformed to the point of yielding zero entries, a built-in
 * default config (matching today's 4 menu entries) is installed instead, so the
 * loader ALWAYS has something bootable. 'path' is an ASCII ESP path; pass
 * FOREB_CFG_ESP_PATH for the default location. Returns cfg->count (>=1).
 */
int forebo_config_load(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                       const char *path, struct forebo_config *cfg);

/* Install the built-in default config (exposed for callers that want it
 * directly, e.g. the shell's "reset config" or a forced fallback). */
void forebo_config_default(struct forebo_config *cfg);

/* Parsed PC-speaker audio config (NULL if forebo.cfg set no audio_/pcspeaker
 * keys). Defined in config.c; consumed by bootx64 -> audio_configure(). */
struct audio_cfg;
const struct audio_cfg *forebo_cfg_audio(void);

#ifdef __cplusplus
}
#endif

#endif /* FOREB_UEFI_CONFIG_H */
