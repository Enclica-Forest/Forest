/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/chain.c - chainload another EFI bootloader. See chain.h.
 * =============================================================================
 * Freestanding (no libc). Builds a full device path (the volume's own device
 * path + a MEDIA_FILEPATH node) and LoadImage/StartImage's it so the target
 * loader gets a proper DeviceHandle. Falls back to loading from a source buffer
 * when a volume exposes no device path.
 * =============================================================================
 */
#include "chain.h"
#include "efi_ext.h"
#include "config.h"
#include "ui.h"

static EFI_GUID dp_guid  = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID fi_guid  = EFI_FILE_INFO_ID;

/* Candidate loader paths tried during an auto-scan (CHAR16, ESP-relative). */
static const CHAR16 *const CHAIN_CANDIDATES[] = {
    L"\\EFI\\BOOT\\BOOTX64.EFI",
    L"\\EFI\\BOOT\\BOOTAA64.EFI",
    L"\\EFI\\grub\\grubx64.efi",
    L"\\EFI\\ubuntu\\grubx64.efi",
    L"\\EFI\\debian\\grubx64.efi",
    L"\\EFI\\fedora\\grubx64.efi",
    L"\\EFI\\arch\\grubx64.efi",
    L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
};
#define CHAIN_NCAND (int)(sizeof(CHAIN_CANDIDATES)/sizeof(CHAIN_CANDIDATES[0]))

static UINTN dp_total_len(EFI_DEVICE_PATH_PROTOCOL *dp)
{
    UINTN total = 0;
    EFI_DEVICE_PATH_PROTOCOL *n = dp;
    for (;;) {
        UINT16 nl = EFI_DP_NODE_LEN(n);
        if (nl < sizeof(EFI_DEVICE_PATH_PROTOCOL)) break;   /* malformed guard */
        total += nl;
        if (EFI_DP_IS_END(n)) break;
        n = EFI_DP_NEXT(n);
    }
    return total;
}

/* Build volume_dp (minus its END node) + FILEPATH(path) + END. Caller frees. */
static EFI_DEVICE_PATH_PROTOCOL *append_filepath(EFI_BOOT_SERVICES *bs,
                                                 EFI_DEVICE_PATH_PROTOCOL *vol,
                                                 const CHAR16 *path)
{
    UINTN pchars = 0; while (path[pchars]) pchars++;
    UINTN fp_node = sizeof(EFI_DEVICE_PATH_PROTOCOL) + (pchars + 1) * sizeof(CHAR16);
    UINTN end_node = sizeof(EFI_DEVICE_PATH_PROTOCOL);

    UINTN base = 0;
    if (vol) {
        base = dp_total_len(vol);
        if (base >= end_node) base -= end_node;   /* strip trailing END */
    }

    UINTN total = base + fp_node + end_node;
    UINT8 *out = NULL;
    if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, total, (VOID **)&out)) || !out)
        return NULL;

    UINTN off = 0;
    if (vol && base) {
        const UINT8 *src = (const UINT8 *)vol;
        for (UINTN i = 0; i < base; i++) out[i] = src[i];
        off = base;
    }
    /* FILEPATH node */
    FILEPATH_DEVICE_PATH *fp = (FILEPATH_DEVICE_PATH *)(out + off);
    fp->Header.Type = MEDIA_DEVICE_PATH;
    fp->Header.SubType = MEDIA_FILEPATH_DP;
    EFI_DP_SET_LEN(&fp->Header, (UINT16)fp_node);
    for (UINTN i = 0; i <= pchars; i++) fp->PathName[i] = path[i];
    off += fp_node;
    /* END node */
    EFI_DEVICE_PATH_PROTOCOL *end = (EFI_DEVICE_PATH_PROTOCOL *)(out + off);
    end->Type = END_DEVICE_PATH_TYPE;
    end->SubType = END_ENTIRE_DP_SUBTYPE;
    EFI_DP_SET_LEN(end, (UINT16)end_node);

    return (EFI_DEVICE_PATH_PROTOCOL *)out;
}

/* Does 'path' exist under this already-opened root directory? */
static int file_exists_on_root(EFI_FILE_PROTOCOL *root, const CHAR16 *path)
{
    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS s = root->Open(root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    int ok = (!EFI_ERROR(s) && f);
    if (f) f->Close(f);
    return ok;
}

/* Open the SimpleFileSystem root for a volume handle (NULL on failure). */
static EFI_FILE_PROTOCOL *open_root(EFI_BOOT_SERVICES *bs, EFI_HANDLE fsh)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    if (EFI_ERROR(bs->HandleProtocol(fsh, &sfs_guid, (VOID **)&fs)) || !fs) return NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) return NULL;
    return root;
}

/* Does 'path' exist on the volume rooted at this SimpleFileSystem handle? */
static int file_exists(EFI_BOOT_SERVICES *bs, EFI_HANDLE fsh, const CHAR16 *path)
{
    EFI_FILE_PROTOCOL *root = open_root(bs, fsh);
    if (!root) return 0;
    int ok = file_exists_on_root(root, path);
    root->Close(root);
    (void)fi_guid;
    return ok;
}

/* LoadImage+StartImage the given file on the given volume handle. Returns only
 * on failure. */
static EFI_STATUS launch_on(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                            EFI_HANDLE fsh, const CHAR16 *path)
{
    EFI_DEVICE_PATH_PROTOCOL *vol = NULL;
    bs->HandleProtocol(fsh, &dp_guid, (VOID **)&vol);   /* may be NULL */

    EFI_HANDLE img = NULL;
    EFI_STATUS s = EFI_LOAD_ERROR;

    EFI_DEVICE_PATH_PROTOCOL *full = append_filepath(bs, vol, path);
    if (full) {
        s = foreb_LoadImage(bs, FALSE, image, full, NULL, 0, &img);
        bs->FreePool(full);
    }
    if ((EFI_ERROR(s) || !img)) {
        /* Fallback: read the file into a buffer and load from source. */
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
        if (!EFI_ERROR(bs->HandleProtocol(fsh, &sfs_guid, (VOID **)&fs)) && fs) {
            EFI_FILE_PROTOCOL *root = NULL, *f = NULL;
            if (!EFI_ERROR(fs->OpenVolume(fs, &root)) && root) {
                if (!EFI_ERROR(root->Open(root, &f, (CHAR16 *)path,
                                          EFI_FILE_MODE_READ, 0)) && f) {
                    UINT8 info[512]; UINTN isz = sizeof(info);
                    if (!EFI_ERROR(f->GetInfo(f, &fi_guid, &isz, info))) {
                        UINTN fsize = (UINTN)((EFI_FILE_INFO *)info)->FileSize;
                        VOID *buf = NULL;
                        if (!EFI_ERROR(bs->AllocatePool(EfiLoaderData, fsize, &buf)) && buf) {
                            UINTN got = fsize;
                            EFI_STATUS ls = EFI_LOAD_ERROR;
                            if (!EFI_ERROR(f->Read(f, &got, buf)))
                                ls = foreb_LoadImage(bs, FALSE, image, NULL, buf, got, &img);
                            /* buf is owned by firmware ONLY if LoadImage succeeded;
                             * otherwise free it - this fallback is retried from the
                             * resident menu, so a leak here accumulates. */
                            if (!EFI_ERROR(ls) && img) s = ls;
                            else bs->FreePool(buf);
                        }
                    }
                    f->Close(f);
                }
                root->Close(root);
            }
        }
    }
    if (EFI_ERROR(s) || !img) return EFI_ERROR(s) ? s : EFI_LOAD_ERROR;

    ui_status("Chainloading..."); ui_present();
    UINTN exitsz = 0; CHAR16 *exitdata = NULL;
    s = foreb_StartImage(bs, img, &exitsz, &exitdata);
    /* Only returns if the chained loader exited. */
    foreb_UnloadImage(bs, img);
    return EFI_ERROR(s) ? s : EFI_LOAD_ERROR;
}

EFI_STATUS chainload(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                     EFI_SYSTEM_TABLE *st, const struct forebo_menuentry *ent)
{
    (void)st;
    if (!bs || !ent) return EFI_INVALID_PARAMETER;

    /* Enumerate every SimpleFileSystem volume (internal + USB). */
    UINTN nH = 0; EFI_HANDLE *handles = NULL;
    EFI_STATUS s = bs->LocateHandleBuffer(ByProtocol, &sfs_guid, NULL, &nH, &handles);
    if (EFI_ERROR(s) || !handles || nH == 0) {
        ui_status("Chainload: no filesystem volumes"); ui_present();
        return EFI_NOT_FOUND;
    }

    /* Case 1: an explicit chain= path. Try ForeB's own volume first, then any. */
    if (ent->chain[0]) {
        CHAR16 wpath[FOREB_CFG_PATH_LEN + 2];
        esp_ascii_to_char16(ent->chain, wpath, FOREB_CFG_PATH_LEN + 2);
        for (UINTN i = 0; i < nH; i++) {
            if (file_exists(bs, handles[i], wpath)) {
                s = launch_on(image, bs, handles[i], wpath);   /* returns on fail */
            }
        }
        bs->FreePool(handles);
        ui_status("Chainload: target not found on any volume"); ui_present();
        return EFI_NOT_FOUND;
    }

    /* Case 2: auto-scan for a standard loader on every volume. Open each
     * volume's root once and probe all candidates against it (instead of
     * re-doing HandleProtocol+OpenVolume+Close per candidate). */
    for (UINTN i = 0; i < nH; i++) {
        EFI_FILE_PROTOCOL *root = open_root(bs, handles[i]);
        if (!root) continue;
        for (int c = 0; c < CHAIN_NCAND; c++) {
            if (file_exists_on_root(root, CHAIN_CANDIDATES[c])) {
                s = launch_on(image, bs, handles[i], CHAIN_CANDIDATES[c]);
                /* if it returns, that candidate failed; keep scanning */
            }
        }
        root->Close(root);
    }
    bs->FreePool(handles);
    ui_status("Chainload: no bootable EFI loader found"); ui_present();
    return EFI_NOT_FOUND;
}
