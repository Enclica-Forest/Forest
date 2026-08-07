/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/linux.c - EFI-stub vmlinuz + initrd boot. See linux.h.
 * =============================================================================
 * Freestanding (no libc). Uses config.c's ESP helpers to read the kernel and
 * initrd, efi_ext.h's LoadImage/StartImage wrappers, and the Linux initrd media
 * LoadFile2 protocol to hand the initramfs to the stub.
 * =============================================================================
 */
#include "linux.h"
#include "efi_ext.h"
#include "config.h"
#include "ui.h"

static EFI_GUID li_guid     = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID dp_guid     = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID lf2_guid    = EFI_LOAD_FILE2_PROTOCOL_GUID;
static EFI_GUID initrd_guid = LINUX_EFI_INITRD_MEDIA_GUID;

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

/*
 * Build a NEW device path = (all of `base` except its END node) + a
 * MEDIA_FILEPATH node holding `wpath` + an END node. AllocatePool'd; caller
 * FreePool's it. Returns NULL on OOM.
 */
static EFI_DEVICE_PATH_PROTOCOL *dp_make_file_path(
        EFI_BOOT_SERVICES *bs, const EFI_DEVICE_PATH_PROTOCOL *base,
        const CHAR16 *wpath)
{
    UINTN base_len = base ? dp_len_no_end(base) : 0;
    UINTN chars    = 0; while (wpath && wpath[chars]) chars++;
    UINTN fp_len   = sizeof(EFI_DEVICE_PATH_PROTOCOL) + (chars + 1) * sizeof(CHAR16);
    UINTN end_len  = sizeof(EFI_DEVICE_PATH_PROTOCOL);
    UINTN total    = base_len + fp_len + end_len;
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

/* ---- initrd delivery via LoadFile2 -------------------------------------- */
static VOID  *g_initrd_data;
static UINTN  g_initrd_size;

static EFI_STATUS EFIAPI initrd_load_file(EFI_LOAD_FILE2_PROTOCOL *This,
                                          EFI_DEVICE_PATH_PROTOCOL *FilePath,
                                          BOOLEAN BootPolicy,
                                          UINTN *BufferSize, VOID *Buffer)
{
    (void)This; (void)FilePath;
    if (BootPolicy) return EFI_UNSUPPORTED;         /* LoadFile2 => must be FALSE */
    if (!BufferSize) return EFI_INVALID_PARAMETER;
    if (!g_initrd_data || g_initrd_size == 0) return EFI_NOT_FOUND;
    if (!Buffer || *BufferSize < g_initrd_size) {
        *BufferSize = g_initrd_size;
        return EFI_BUFFER_TOO_SMALL;
    }
    copy_bytes(Buffer, g_initrd_data, g_initrd_size);
    *BufferSize = g_initrd_size;
    return EFI_SUCCESS;
}

static EFI_LOAD_FILE2_PROTOCOL g_initrd_lf2 = { initrd_load_file };

/* The vendor device path the Linux stub searches for. */
static FOREB_INITRD_DEVICE_PATH g_initrd_dp;

static void ascii_to_char16(const char *in, CHAR16 *out, UINTN cap)
{
    UINTN i = 0;
    if (!out || cap == 0) return;
    if (in) for (; in[i] && i + 1 < cap; i++) out[i] = (CHAR16)(unsigned char)in[i];
    out[i] = 0;
}

EFI_STATUS boot_linux(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                      EFI_SYSTEM_TABLE *st, const struct forebo_menuentry *ent)
{
    (void)st;
    if (!bs || !ent) return EFI_INVALID_PARAMETER;

    const char *kpath = ent->vmlinuz[0] ? ent->vmlinuz : ent->kernel;
    if (!kpath || !kpath[0]) return EFI_NOT_FOUND;

    /* ---- 1. Read the kernel PE into a pool buffer ---- */
    void *kbuf = NULL; UINTN ksize = 0;
    EFI_STATUS s = esp_read_file(image, bs, kpath, &kbuf, &ksize);
    if (EFI_ERROR(s) || !kbuf || !ksize) {
        esp_free_file(bs, kbuf);   /* 0-byte file: success + allocated buf we must free */
        ui_status("Linux: vmlinuz not found"); ui_present();
        return EFI_ERROR(s) ? s : EFI_NOT_FOUND;
    }

    /* ---- 2. LoadImage from the source buffer ---- */
    EFI_HANDLE kimg = NULL;
    s = foreb_LoadImage(bs, FALSE, image, NULL, kbuf, ksize, &kimg);
    if (EFI_ERROR(s) || !kimg) {
        ui_status("Linux: LoadImage failed (not an EFI-stub kernel?)"); ui_present();
        esp_free_file(bs, kbuf);
        return EFI_ERROR(s) ? s : EFI_LOAD_ERROR;
    }

    /* ---- 3. Command line -> LoadOptions (CHAR16) ---- */
    CHAR16 *wcmd = NULL;
    if (ent->cmdline[0]) {
        UINTN n = 0; while (ent->cmdline[n]) n++;
        UINTN bytes = (n + 1) * sizeof(CHAR16);
        if (!EFI_ERROR(bs->AllocatePool(EfiLoaderData, bytes, (VOID **)&wcmd)) && wcmd) {
            ascii_to_char16(ent->cmdline, wcmd, n + 1);
            EFI_LOADED_IMAGE_PROTOCOL *kli = NULL;
            if (!EFI_ERROR(bs->HandleProtocol(kimg, &li_guid, (VOID **)&kli)) && kli) {
                kli->LoadOptions = wcmd;
                kli->LoadOptionsSize = (UINT32)bytes;   /* incl. terminating NUL */
            }
        }
    }

    /* ---- 4. Publish the initrd (if any) via LoadFile2 ---- */
    EFI_HANDLE initrd_handle = NULL;
    int initrd_installed = 0;
    if (ent->initrd[0]) {
        void *ibuf = NULL; UINTN isize = 0;
        if (!EFI_ERROR(esp_read_file(image, bs, ent->initrd, &ibuf, &isize)) &&
            ibuf && isize) {
            g_initrd_data = ibuf;
            g_initrd_size = isize;
            /* (0-byte initrd path frees ibuf in the else below) */
            /* Build the MEDIA_VENDOR(initrd) + END device path. */
            g_initrd_dp.Vendor.Header.Type    = MEDIA_DEVICE_PATH;
            g_initrd_dp.Vendor.Header.SubType = MEDIA_VENDOR_DP;
            EFI_DP_SET_LEN(&g_initrd_dp.Vendor.Header, (UINT16)sizeof(VENDOR_DEVICE_PATH));
            g_initrd_dp.Vendor.Guid = initrd_guid;
            g_initrd_dp.End.Type    = END_DEVICE_PATH_TYPE;
            g_initrd_dp.End.SubType = END_ENTIRE_DP_SUBTYPE;
            EFI_DP_SET_LEN(&g_initrd_dp.End, (UINT16)sizeof(EFI_DEVICE_PATH_PROTOCOL));

            EFI_STATUS is =
                ((EFI_INSTALL_MULTIPLE_PROTOCOL_INTERFACES)
                     bs->InstallMultipleProtocolInterfaces)(
                    &initrd_handle,
                    &dp_guid,  &g_initrd_dp,
                    &lf2_guid, &g_initrd_lf2,
                    NULL);
            if (!EFI_ERROR(is)) initrd_installed = 1;
            else esp_free_file(bs, ibuf);   /* couldn't publish; drop it */
        } else if (ibuf) {
            esp_free_file(bs, ibuf);   /* 0-byte initrd: buf allocated but block skipped */
        }
    }

    /* ---- 5. Hand off ---- */
    ui_status("Starting Linux..."); ui_present();
    UINTN exitsz = 0; CHAR16 *exitdata = NULL;
    s = foreb_StartImage(bs, kimg, &exitsz, &exitdata);

    /* StartImage only returns if the kernel/stub failed or exited. Tear down. */
    if (initrd_installed) {
        ((EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES)
             bs->UninstallMultipleProtocolInterfaces)(
            initrd_handle, &dp_guid, &g_initrd_dp, &lf2_guid, &g_initrd_lf2, NULL);
        if (g_initrd_data) esp_free_file(bs, g_initrd_data);
        g_initrd_data = NULL; g_initrd_size = 0;
    }
    foreb_UnloadImage(bs, kimg);
    if (wcmd) bs->FreePool(wcmd);
    esp_free_file(bs, kbuf);
    ui_status("Linux exited / boot failed"); ui_present();
    return EFI_ERROR(s) ? s : EFI_LOAD_ERROR;
}
