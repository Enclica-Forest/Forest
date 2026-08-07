/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/boot_linux.c - Boot a Linux 'vmlinuz' via the modern EFI-stub path.
 * =============================================================================
 * Freestanding (no libc). Same clang recipe as the rest of the UEFI loader.
 * See boot_linux.h for the high-level flow. Arch-neutral: no x86-only code, so
 * it links into BOOTX64.EFI, BOOTAA64.EFI and BOOTRISCV64.EFI alike.
 * =============================================================================
 */

#include "boot_linux.h"
#include "../efi_ext.h"    /* device-path nodes, LoadFile2, LoadImage wrappers   */
#include "../core/config.h"     /* esp_open_root / esp_ascii_to_char16 / esp_read_file */

/* =============================================================================
 * File-scope GUID copies (same pattern as config.c/modules.c/shell.c: each TU
 * keeps its own so there is never a duplicate-symbol clash at link time).
 * ========================================================================== */
static EFI_GUID bl_loaded_img_guid   = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID bl_device_path_guid  = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID bl_load_file2_guid   = EFI_LOAD_FILE2_PROTOCOL_GUID;
static EFI_GUID bl_initrd_media_guid = LINUX_EFI_INITRD_MEDIA_GUID;

/* =============================================================================
 * COM1 serial log (mirrors bootx64.c; those helpers are static there, so we
 * keep a tiny private copy). Visible on QEMU -serial stdio.
 * ========================================================================== */
#if defined(__x86_64__)
static inline void bl_outb(UINT16 port, UINT8 v)
{ __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port)); }
static void bl_putc(char c) { if (c == '\n') bl_outb(0x3F8, '\r'); bl_outb(0x3F8, (UINT8)c); }
#else
/* On AArch64/RISC-V there is no port I/O; degrade to a no-op (the firmware
 * console still shows ConOut messages the caller prints). */
static void bl_putc(char c) { (void)c; }
#endif

static void bl_log(const char *s) { while (s && *s) bl_putc(*s++); }
static void bl_loghex(UINT64 v, int digits)
{
    static const char hx[] = "0123456789ABCDEF";
    bl_log("0x");
    for (int i = (digits - 1) * 4; i >= 0; i -= 4) bl_putc(hx[(v >> i) & 0xF]);
}

/* =============================================================================
 * Tiny string helpers.
 * ========================================================================== */
static UINTN bl_slen(const char *s) { UINTN n = 0; while (s && s[n]) n++; return n; }
static UINTN bl_wlen(const CHAR16 *s) { UINTN n = 0; while (s && s[n]) n++; return n; }

/* Copy `n` bytes: UINTN-word-wise when both ends are word-aligned (the common
 * case: firmware pool/page buffers), byte-wise otherwise + for the tail. */
static void copy_bytes(void *dst, const void *src, UINTN n)
{
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    if ((((UINTN)d | (UINTN)s) & (sizeof(UINTN) - 1)) == 0) {
        while (n >= sizeof(UINTN)) {
            *(UINTN *)d = *(const UINTN *)s;
            d += sizeof(UINTN); s += sizeof(UINTN); n -= sizeof(UINTN);
        }
    }
    while (n--) *d++ = *s++;
}

/* =============================================================================
 * Device-path helpers.
 * ---------------------------------------------------------------------------
 * A device path is a packed byte stream of nodes ending in an END node. Nodes
 * are NOT guaranteed to be naturally aligned, so we read/write the multi-byte
 * fields byte-wise where it matters.
 * ========================================================================== */

/* Total bytes of a device path up to (but NOT including) its END node. */
static UINTN dp_len_no_end(const EFI_DEVICE_PATH_PROTOCOL *dp)
{
    const EFI_DEVICE_PATH_PROTOCOL *n = dp;
    UINTN total = 0;
    while (n && !EFI_DP_IS_END(n)) {
        UINT16 l = EFI_DP_NODE_LEN(n);
        if (l < sizeof(EFI_DEVICE_PATH_PROTOCOL)) break;  /* malformed guard */
        total += l;
        n = (const EFI_DEVICE_PATH_PROTOCOL *)((const UINT8 *)n + l);
    }
    return total;
}

/* Fetch the EFI_DEVICE_PATH_PROTOCOL installed on a handle (NULL on failure). */
static EFI_DEVICE_PATH_PROTOCOL *dp_of_handle(EFI_BOOT_SERVICES *bs, EFI_HANDLE h)
{
    EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
    if (!bs || !h) return NULL;
    if (EFI_ERROR(bs->HandleProtocol(h, &bl_device_path_guid, (VOID **)&dp))) return NULL;
    return dp;
}

/*
 * Build a NEW device path = (all of `base` except its END node) + a
 * MEDIA_FILEPATH node holding `wpath` + an END node. AllocatePool'd; caller
 * FreePool's it. Returns NULL on OOM.
 */
static EFI_DEVICE_PATH_PROTOCOL *dp_make_file_path(
        EFI_BOOT_SERVICES *bs, const EFI_DEVICE_PATH_PROTOCOL *base,
        const CHAR16 *wpath)
{
    UINTN base_len   = base ? dp_len_no_end(base) : 0;
    UINTN chars      = bl_wlen(wpath);
    UINTN fp_len     = sizeof(EFI_DEVICE_PATH_PROTOCOL) + (chars + 1) * sizeof(CHAR16);
    UINTN end_len    = sizeof(EFI_DEVICE_PATH_PROTOCOL);
    UINTN total      = base_len + fp_len + end_len;
    UINT8 *buf = NULL;

    if (fp_len > 0xFFFF) return NULL;   /* node Length is a 16-bit field */
    if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, total, (VOID **)&buf)) || !buf)
        return NULL;

    /* Copy base nodes verbatim (word-wise when alignment allows). */
    copy_bytes(buf, base, base_len);

    /* MEDIA_FILEPATH node. */
    EFI_DEVICE_PATH_PROTOCOL *fp = (EFI_DEVICE_PATH_PROTOCOL *)(buf + base_len);
    fp->Type    = MEDIA_DEVICE_PATH;
    fp->SubType = MEDIA_FILEPATH_DP;
    EFI_DP_SET_LEN(fp, (UINT16)fp_len);
    /* Store the CHAR16 path directly when the node lands 2-byte aligned;
     * byte-wise otherwise (device-path nodes are packed). */
    UINT8 *pn = buf + base_len + sizeof(EFI_DEVICE_PATH_PROTOCOL);
    if (((UINTN)pn & (sizeof(CHAR16) - 1)) == 0) {
        CHAR16 *pw = (CHAR16 *)pn;
        for (UINTN i = 0; i <= chars; i++) pw[i] = wpath[i];
    } else {
        for (UINTN i = 0; i < chars; i++) {
            pn[2 * i]     = (UINT8)(wpath[i] & 0xFF);
            pn[2 * i + 1] = (UINT8)((wpath[i] >> 8) & 0xFF);
        }
        pn[2 * chars]     = 0;
        pn[2 * chars + 1] = 0;
    }

    /* END node. */
    EFI_DEVICE_PATH_PROTOCOL *end =
        (EFI_DEVICE_PATH_PROTOCOL *)(buf + base_len + fp_len);
    end->Type    = END_DEVICE_PATH_TYPE;
    end->SubType = END_ENTIRE_DP_SUBTYPE;
    EFI_DP_SET_LEN(end, (UINT16)end_len);

    return (EFI_DEVICE_PATH_PROTOCOL *)buf;
}

/* =============================================================================
 * Initrd delivery via EFI_LOAD_FILE2_PROTOCOL + LINUX_EFI_INITRD_MEDIA_GUID.
 * ---------------------------------------------------------------------------
 * The protocol struct has no context pointer, so the loaded initrd bytes live
 * in file-scope statics. Only one Linux boot runs at a time, so this is safe.
 * ========================================================================== */
static VOID  *g_initrd_data = NULL;   /* AllocatePool'd initrd image bytes */
static UINTN  g_initrd_size = 0;

/*
 * EFI_LOAD_FILE2 callback. The EFI stub calls this twice: first with
 * Buffer==NULL to learn the size (we return EFI_BUFFER_TOO_SMALL and set
 * *BufferSize), then with a buffer of that size to receive the bytes.
 */
static EFI_STATUS EFIAPI initrd_load_file(
        EFI_LOAD_FILE2_PROTOCOL *This,
        EFI_DEVICE_PATH_PROTOCOL *FilePath,
        BOOLEAN BootPolicy,
        UINTN *BufferSize,
        VOID *Buffer)
{
    (void)This; (void)FilePath;
    /* LoadFile2 is a non-boot-policy load; the initrd protocol mandates FALSE. */
    if (BootPolicy) return EFI_UNSUPPORTED;
    if (!BufferSize) return EFI_INVALID_PARAMETER;
    if (!g_initrd_data || g_initrd_size == 0) return EFI_NOT_FOUND;

    if (Buffer == NULL || *BufferSize < g_initrd_size) {
        *BufferSize = g_initrd_size;
        return EFI_BUFFER_TOO_SMALL;
    }

    copy_bytes(Buffer, g_initrd_data, g_initrd_size);
    *BufferSize = g_initrd_size;
    return EFI_SUCCESS;
}

/* The LoadFile2 interface instance published on the initrd handle. */
static EFI_LOAD_FILE2_PROTOCOL g_initrd_lf2 = { initrd_load_file };

/* The device path installed on the initrd handle: a MEDIA_VENDOR node carrying
 * LINUX_EFI_INITRD_MEDIA_GUID, then an END node. */
static FOREB_INITRD_DEVICE_PATH g_initrd_dp;

static void initrd_dp_build(void)
{
    g_initrd_dp.Vendor.Header.Type    = MEDIA_DEVICE_PATH;
    g_initrd_dp.Vendor.Header.SubType = MEDIA_VENDOR_DP;
    EFI_DP_SET_LEN(&g_initrd_dp.Vendor.Header, (UINT16)sizeof(VENDOR_DEVICE_PATH));
    g_initrd_dp.Vendor.Guid = bl_initrd_media_guid;

    g_initrd_dp.End.Type    = END_DEVICE_PATH_TYPE;
    g_initrd_dp.End.SubType = END_ENTIRE_DP_SUBTYPE;
    EFI_DP_SET_LEN(&g_initrd_dp.End, (UINT16)sizeof(EFI_DEVICE_PATH_PROTOCOL));
}

/*
 * Read the initrd file off ForeB's ESP and publish it on a fresh handle so the
 * EFI stub can find it. On success *out_handle is the new handle (uninstall it
 * after StartImage). Returns EFI_SUCCESS, or an error (nothing installed).
 */
static EFI_STATUS initrd_install(EFI_HANDLE parent_image, EFI_BOOT_SERVICES *bs,
                                 const char *initrd_path, EFI_HANDLE *out_handle)
{
    EFI_STATUS st;
    void *buf = NULL;
    UINTN size = 0;

    if (out_handle) *out_handle = NULL;
    if (!initrd_path || !initrd_path[0]) return EFI_SUCCESS;  /* no initrd = ok */

    st = esp_read_file(parent_image, bs, initrd_path, &buf, &size);
    if (EFI_ERROR(st) || !buf || size == 0) {
        bl_log("  [x] initrd read failed: "); bl_log(initrd_path); bl_putc('\n');
        return EFI_ERROR(st) ? st : EFI_NOT_FOUND;
    }
    g_initrd_data = buf;
    g_initrd_size = size;
    bl_log("  [*] initrd loaded, bytes="); bl_loghex(size, 8); bl_putc('\n');

    initrd_dp_build();

    EFI_HANDLE h = NULL;
    st = ((EFI_INSTALL_MULTIPLE_PROTOCOL_INTERFACES)
              bs->InstallMultipleProtocolInterfaces)(
              &h,
              &bl_device_path_guid, &g_initrd_dp,
              &bl_load_file2_guid,  &g_initrd_lf2,
              NULL);
    if (EFI_ERROR(st)) {
        bl_log("  [x] initrd handle install failed: "); bl_loghex(st, 16); bl_putc('\n');
        esp_free_file(bs, buf);
        g_initrd_data = NULL; g_initrd_size = 0;
        return st;
    }
    if (out_handle) *out_handle = h;
    return EFI_SUCCESS;
}

static void initrd_uninstall(EFI_BOOT_SERVICES *bs, EFI_HANDLE h)
{
    if (h) {
        ((EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES)
             bs->UninstallMultipleProtocolInterfaces)(
             h,
             &bl_device_path_guid, &g_initrd_dp,
             &bl_load_file2_guid,  &g_initrd_lf2,
             NULL);
    }
    if (g_initrd_data) esp_free_file(bs, g_initrd_data);
    g_initrd_data = NULL;
    g_initrd_size = 0;
}

/* =============================================================================
 * Public: linux_boot()
 * ========================================================================== */
EFI_STATUS linux_boot(EFI_HANDLE parent_image, EFI_SYSTEM_TABLE *st,
                      const char *vmlinuz_path, const char *initrd_path,
                      const char *cmdline)
{
    if (!st || !st->BootServices || !parent_image || !vmlinuz_path || !vmlinuz_path[0])
        return EFI_INVALID_PARAMETER;

    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_STATUS st_ret;

    bl_log("[*] linux_boot: vmlinuz="); bl_log(vmlinuz_path);
    if (initrd_path && initrd_path[0]) { bl_log(" initrd="); bl_log(initrd_path); }
    bl_putc('\n');

    /* --- 1. Resolve the ESP volume device path (of the ForeB image). ------- */
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    st_ret = bs->HandleProtocol(parent_image, &bl_loaded_img_guid, (VOID **)&li);
    if (EFI_ERROR(st_ret) || !li) {
        bl_log("  [x] LoadedImage on self failed\n");
        return EFI_ERROR(st_ret) ? st_ret : EFI_NOT_FOUND;
    }
    EFI_DEVICE_PATH_PROTOCOL *vol_dp = dp_of_handle(bs, li->DeviceHandle);
    if (!vol_dp) {
        bl_log("  [x] no device path on ESP handle\n");
        return EFI_NOT_FOUND;
    }

    /* --- 2. Build full device path to the kernel file. -------------------- */
    CHAR16 wkernel[FOREB_CFG_PATH_LEN + 2];
    esp_ascii_to_char16(vmlinuz_path, wkernel, FOREB_CFG_PATH_LEN + 2);
    EFI_DEVICE_PATH_PROTOCOL *kern_dp = dp_make_file_path(bs, vol_dp, wkernel);
    if (!kern_dp) {
        bl_log("  [x] out of memory building kernel device path\n");
        return EFI_OUT_OF_RESOURCES;
    }

    /* --- 3. LoadImage the vmlinuz (it is an EFI-stub PE). ----------------- */
    EFI_HANDLE kernel_image = NULL;
    st_ret = foreb_LoadImage(bs, FALSE, parent_image, kern_dp, NULL, 0, &kernel_image);
    bs->FreePool(kern_dp);
    if (EFI_ERROR(st_ret) || !kernel_image) {
        bl_log("  [x] LoadImage(vmlinuz) failed: "); bl_loghex(st_ret, 16); bl_putc('\n');
        bl_log("      (is this a modern EFI-stub kernel? bare bzImage is unsupported)\n");
        return EFI_ERROR(st_ret) ? st_ret : EFI_LOAD_ERROR;
    }
    bl_log("  [*] vmlinuz LoadImage OK\n");

    /* --- 4. Set the command line via LoadedImage->LoadOptions (UTF-16). ---- */
    CHAR16 *wcmd = NULL;
    EFI_LOADED_IMAGE_PROTOCOL *kli = NULL;
    st_ret = bs->HandleProtocol(kernel_image, &bl_loaded_img_guid, (VOID **)&kli);
    if (!EFI_ERROR(st_ret) && kli) {
        UINTN clen = (cmdline && cmdline[0]) ? bl_slen(cmdline) : 0;
        UINTN bytes = (clen + 1) * sizeof(CHAR16);
        if (!EFI_ERROR(bs->AllocatePool(EfiLoaderData, bytes, (VOID **)&wcmd)) && wcmd) {
            for (UINTN i = 0; i < clen; i++) wcmd[i] = (CHAR16)(unsigned char)cmdline[i];
            wcmd[clen] = 0;
            kli->LoadOptions     = wcmd;
            /* Include the terminating NUL in the size (matches systemd-boot). */
            kli->LoadOptionsSize = (UINT32)bytes;
            if (clen) { bl_log("  [*] cmdline: "); bl_log(cmdline); bl_putc('\n'); }
        }
    } else {
        bl_log("  [!] LoadedImage on kernel failed; booting without cmdline\n");
    }

    /* --- 5. Publish the initrd (LoadFile2 media protocol). ---------------- */
    EFI_HANDLE initrd_handle = NULL;
    st_ret = initrd_install(parent_image, bs, initrd_path, &initrd_handle);
    if (EFI_ERROR(st_ret)) {
        /* Initrd was requested but could not be provided: abort rather than
         * boot a kernel that will panic waiting for its root. */
        if (wcmd) bs->FreePool(wcmd);
        foreb_UnloadImage(bs, kernel_image);
        return st_ret;
    }

    /* --- 6. StartImage: hand the machine to Linux. ----------------------- */
    bl_log("  [*] StartImage(vmlinuz) - handing off to Linux\n");
    UINTN exit_sz = 0;
    CHAR16 *exit_data = NULL;
    st_ret = foreb_StartImage(bs, kernel_image, &exit_sz, &exit_data);

    /* If we get here, Linux returned (boot failed or the stub bailed out). */
    bl_log("  [!] Linux returned to ForeB: "); bl_loghex(st_ret, 16); bl_putc('\n');

    /* --- 7. Tear down. --------------------------------------------------- */
    initrd_uninstall(bs, initrd_handle);
    if (exit_data) bs->FreePool(exit_data);
    if (wcmd)      bs->FreePool(wcmd);
    foreb_UnloadImage(bs, kernel_image);

    return st_ret;
}
