/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/chainload.c - Enumerate volumes + chainload another EFI bootloader.
 * =============================================================================
 * Freestanding (no libc). Same clang recipe as the rest of the UEFI loader.
 * See chainload.h for the high-level flow. Arch-neutral (SimpleFileSystem +
 * device paths only), so it links into BOOTX64/AA64/RISCV64.EFI unchanged.
 * =============================================================================
 */

#include "chainload.h"
#include "../efi_ext.h"   /* device-path nodes + LoadImage/StartImage wrappers */
#include "../arch.h"      /* FOREB_ARCH_* -> arch-appropriate loader filenames  */

/* =============================================================================
 * File-scope GUID copies (own-per-TU pattern; avoids duplicate symbols).
 * ========================================================================== */
static EFI_GUID cl_sfs_guid         = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID cl_file_info_guid   = EFI_FILE_INFO_ID;
static EFI_GUID cl_loaded_img_guid  = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID cl_device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;

/* MESSAGING_DEVICE_PATH sub-type for USB (used only to label volumes). */
#define MSG_USB_DP  0x05

/* =============================================================================
 * COM1 serial log (private copy; see boot_linux.c for the rationale).
 * ========================================================================== */
#if defined(__x86_64__)
static inline void cl_outb(UINT16 port, UINT8 v)
{ __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port)); }
static void cl_putc(char c) { if (c == '\n') cl_outb(0x3F8, '\r'); cl_outb(0x3F8, (UINT8)c); }
#else
static void cl_putc(char c) { (void)c; }
#endif
static void cl_log(const char *s) { while (s && *s) cl_putc(*s++); }
static void cl_loghex(UINT64 v, int digits)
{
    static const char hx[] = "0123456789ABCDEF";
    cl_log("0x");
    for (int i = (digits - 1) * 4; i >= 0; i -= 4) cl_putc(hx[(v >> i) & 0xF]);
}

/* =============================================================================
 * Arch-appropriate candidate loader filenames.
 * ---------------------------------------------------------------------------
 * The removable-media default lives at \EFI\BOOT\BOOT<arch>.EFI. GRUB/shim from
 * a distro live at \EFI\<vendor>\{grub,shim}<arch>.efi. We always ALSO try the
 * x64 spellings so an x64 USB inserted in an x64 machine is found regardless of
 * our own build arch (the common case).
 * ========================================================================== */
#if FOREB_ARCH_IS_AA64
#  define ARCH_REMOVABLE  L"BOOTAA64.EFI"
#  define ARCH_GRUB       L"grubaa64.efi"
#  define ARCH_SHIM       L"shimaa64.efi"
#elif FOREB_ARCH_IS_RISCV
#  define ARCH_REMOVABLE  L"BOOTRISCV64.EFI"
#  define ARCH_GRUB       L"grubriscv64.efi"
#  define ARCH_SHIM       L"shimriscv64.efi"
#else
#  define ARCH_REMOVABLE  L"BOOTX64.EFI"
#  define ARCH_GRUB       L"grubx64.efi"
#  define ARCH_SHIM       L"shimx64.efi"
#endif

/* =============================================================================
 * Tiny wide/narrow string helpers.
 * ========================================================================== */
static UINTN cl_wlen(const CHAR16 *s) { UINTN n = 0; while (s && s[n]) n++; return n; }

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

/* Append src to dst (a CHAR16 buffer of capacity `cap` incl. NUL). Returns dst. */
static CHAR16 *cl_wcat(CHAR16 *dst, UINTN cap, const CHAR16 *src)
{
    UINTN d = cl_wlen(dst), i = 0;
    if (cap == 0) return dst;
    while (src && src[i] && d + 1 < cap) dst[d++] = src[i++];
    dst[d] = 0;
    return dst;
}
static void cl_wcpy(CHAR16 *dst, UINTN cap, const CHAR16 *src)
{ if (cap) { dst[0] = 0; cl_wcat(dst, cap, src); } }

/* CHAR16 -> ASCII (best-effort, for labels). */
static void cl_w2a(const CHAR16 *w, char *a, UINTN cap)
{
    UINTN i = 0;
    if (!a || cap == 0) return;
    while (w && w[i] && i + 1 < cap) { a[i] = (char)(w[i] & 0x7F); i++; }
    a[i] = 0;
}
static int cl_weq(const CHAR16 *a, const CHAR16 *b)
{ UINTN i = 0; while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; } return a[i] == b[i]; }

/* =============================================================================
 * Device-path helpers (self-contained; mirror boot_linux.c's).
 * ========================================================================== */
static UINTN dp_len_no_end(const EFI_DEVICE_PATH_PROTOCOL *dp)
{
    const EFI_DEVICE_PATH_PROTOCOL *n = dp;
    UINTN total = 0;
    while (n && !EFI_DP_IS_END(n)) {
        UINT16 l = EFI_DP_NODE_LEN(n);
        if (l < sizeof(EFI_DEVICE_PATH_PROTOCOL)) break;
        total += l;
        n = (const EFI_DEVICE_PATH_PROTOCOL *)((const UINT8 *)n + l);
    }
    return total;
}

static EFI_DEVICE_PATH_PROTOCOL *dp_of_handle(EFI_BOOT_SERVICES *bs, EFI_HANDLE h)
{
    EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
    if (!bs || !h) return NULL;
    if (EFI_ERROR(bs->HandleProtocol(h, &cl_device_path_guid, (VOID **)&dp))) return NULL;
    return dp;
}

/* True if the volume's device path traverses a USB messaging node. */
static int dp_is_usb(const EFI_DEVICE_PATH_PROTOCOL *dp)
{
    const EFI_DEVICE_PATH_PROTOCOL *n = dp;
    while (n && !EFI_DP_IS_END(n)) {
        UINT16 l = EFI_DP_NODE_LEN(n);
        if (l < sizeof(EFI_DEVICE_PATH_PROTOCOL)) break;
        if (n->Type == MESSAGING_DEVICE_PATH && n->SubType == MSG_USB_DP) return 1;
        n = (const EFI_DEVICE_PATH_PROTOCOL *)((const UINT8 *)n + l);
    }
    return 0;
}

static EFI_DEVICE_PATH_PROTOCOL *dp_make_file_path(
        EFI_BOOT_SERVICES *bs, const EFI_DEVICE_PATH_PROTOCOL *base,
        const CHAR16 *wpath)
{
    UINTN base_len = base ? dp_len_no_end(base) : 0;
    UINTN chars    = cl_wlen(wpath);
    UINTN fp_len   = sizeof(EFI_DEVICE_PATH_PROTOCOL) + (chars + 1) * sizeof(CHAR16);
    UINTN end_len  = sizeof(EFI_DEVICE_PATH_PROTOCOL);
    UINTN total    = base_len + fp_len + end_len;
    UINT8 *buf = NULL;

    if (fp_len > 0xFFFF) return NULL;
    if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, total, (VOID **)&buf)) || !buf)
        return NULL;

    copy_bytes(buf, base, base_len);

    EFI_DEVICE_PATH_PROTOCOL *fp = (EFI_DEVICE_PATH_PROTOCOL *)(buf + base_len);
    fp->Type    = MEDIA_DEVICE_PATH;
    fp->SubType = MEDIA_FILEPATH_DP;
    EFI_DP_SET_LEN(fp, (UINT16)fp_len);
    UINT8 *pn = buf + base_len + sizeof(EFI_DEVICE_PATH_PROTOCOL);
    if (((UINTN)pn & (sizeof(CHAR16) - 1)) == 0) {
        CHAR16 *pw = (CHAR16 *)pn;
        for (UINTN i = 0; i <= chars; i++) pw[i] = wpath[i];
    } else {
        for (UINTN i = 0; i < chars; i++) {
            pn[2 * i]     = (UINT8)(wpath[i] & 0xFF);
            pn[2 * i + 1] = (UINT8)((wpath[i] >> 8) & 0xFF);
        }
        pn[2 * chars] = 0; pn[2 * chars + 1] = 0;
    }

    EFI_DEVICE_PATH_PROTOCOL *end = (EFI_DEVICE_PATH_PROTOCOL *)(buf + base_len + fp_len);
    end->Type    = END_DEVICE_PATH_TYPE;
    end->SubType = END_ENTIRE_DP_SUBTYPE;
    EFI_DP_SET_LEN(end, (UINT16)end_len);

    return (EFI_DEVICE_PATH_PROTOCOL *)buf;
}

/* =============================================================================
 * Filesystem probing.
 * ========================================================================== */

/* Open a volume's root directory (NULL on failure). */
static EFI_FILE_PROTOCOL *open_volume_root(EFI_BOOT_SERVICES *bs, EFI_HANDLE dev)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    if (EFI_ERROR(bs->HandleProtocol(dev, &cl_sfs_guid, (VOID **)&fs)) || !fs) return NULL;
    if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) return NULL;
    return root;
}

/* True if `path` opens for reading under `root` (then closed). */
static int file_exists(EFI_FILE_PROTOCOL *root, const CHAR16 *path)
{
    EFI_FILE_PROTOCOL *f = NULL;
    if (EFI_ERROR(root->Open(root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ, 0)) || !f)
        return 0;
    f->Close(f);
    return 1;
}

/*
 * Append one discovered candidate to the result set.
 *   name - optional friendly ASCII name (e.g. "Windows Boot Manager"). When
 *          non-NULL it is used for the label body instead of the raw path; the
 *          "<USB|disk> vol N: " prefix is kept so the source volume is clear.
 */
static void add_result(struct foreb_chain_list *out, EFI_HANDLE dev, int vol_idx,
                       int kind, int is_usb, const CHAR16 *path, const char *name)
{
    if (out->count >= FOREB_CHAIN_MAX_RESULTS) return;
    struct foreb_chain_entry *e = &out->items[out->count];
    e->device       = dev;
    e->volume_index = vol_idx;
    e->kind         = kind;
    cl_wcpy(e->path, FOREB_CHAIN_PATH_LEN, path);

    /* Label: "<USB|disk> vol N: <name-or-path>" */
    e->label[0] = 0;
    {
        char *L = e->label;
        UINTN cap = FOREB_CHAIN_LABEL_LEN;
        /* build ASCII label manually */
        const char *pfx = is_usb ? "USB" : "disk";
        UINTN d = 0;
        for (UINTN i = 0; pfx[i] && d + 1 < cap; i++) L[d++] = pfx[i];
        if (d + 6 < cap) { L[d++] = ' '; L[d++] = 'v'; L[d++] = 'o'; L[d++] = 'l'; L[d++] = ' '; }
        if (d + 2 < cap) { L[d++] = (char)('0' + (vol_idx % 10)); }
        if (d + 2 < cap) { L[d++] = ':'; L[d++] = ' '; }
        L[d] = 0;
        if (name) {
            /* friendly name (e.g. "Windows Boot Manager") */
            for (UINTN i = 0; name[i] && d + 1 < cap; i++) L[d++] = name[i];
        } else {
            /* append the path in ASCII */
            char pa[FOREB_CHAIN_PATH_LEN];
            cl_w2a(path, pa, FOREB_CHAIN_PATH_LEN);
            for (UINTN i = 0; pa[i] && d + 1 < cap; i++) L[d++] = pa[i];
        }
        L[d] = 0;
    }
    out->count++;
}

/* Canonical Windows Boot Manager location (fixed on every UEFI arch). */
#define CL_WINDOWS_WPATH  L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi"

/*
 * Probe one already-opened volume root for the Windows Boot Manager and, if
 * present, append a FOREB_CHAIN_WINDOWS result. Returns 1 if a hit was added.
 */
static int scan_volume_windows(struct foreb_chain_list *out, EFI_FILE_PROTOCOL *root,
                               EFI_HANDLE dev, int vol_idx, int is_usb)
{
    if (!out || !root) return 0;
    if (file_exists(root, CL_WINDOWS_WPATH)) {
        add_result(out, dev, vol_idx, FOREB_CHAIN_WINDOWS, is_usb,
                   CL_WINDOWS_WPATH, "Windows Boot Manager");
        return 1;
    }
    return 0;
}

/*
 * Scan one volume for candidate loaders:
 *   - \EFI\BOOT\<removable>            (REMOVABLE)
 *   - \EFI\<vendor>\{grub,shim}<arch>  (GRUB / SHIM), by listing \EFI subdirs.
 */
static void scan_volume(EFI_BOOT_SERVICES *bs, struct foreb_chain_list *out,
                        EFI_HANDLE dev, int vol_idx)
{
    EFI_FILE_PROTOCOL *root = open_volume_root(bs, dev);
    if (!root) return;

    int is_usb = dp_is_usb(dp_of_handle(bs, dev));

    /* 1. Removable default. */
    {
        CHAR16 p[FOREB_CHAIN_PATH_LEN];
        cl_wcpy(p, FOREB_CHAIN_PATH_LEN, L"\\EFI\\BOOT\\");
        cl_wcat(p, FOREB_CHAIN_PATH_LEN, ARCH_REMOVABLE);
        if (file_exists(root, p))
            add_result(out, dev, vol_idx, FOREB_CHAIN_REMOVABLE, is_usb, p, NULL);
    }

    /* 2. Windows Boot Manager (\EFI\Microsoft\Boot\bootmgfw.efi). A Windows
     *    install exposes exactly this fixed path on its ESP; bootmgfw is a plain
     *    EFI app, so chain_boot() LoadImage/StartImage's it like any other. */
    scan_volume_windows(out, root, dev, vol_idx, is_usb);

    /* 3. Distro GRUB/shim: enumerate subdirectories of \EFI. */
    EFI_FILE_PROTOCOL *efidir = NULL;
    if (!EFI_ERROR(root->Open(root, &efidir, L"\\EFI", EFI_FILE_MODE_READ, 0)) && efidir) {
        UINT8 entbuf[512];
        for (;;) {
            UINTN sz = sizeof(entbuf);
            if (EFI_ERROR(efidir->Read(efidir, &sz, entbuf)) || sz == 0) break;
            EFI_FILE_INFO *fi = (EFI_FILE_INFO *)entbuf;
            if (!(fi->Attribute & EFI_FILE_DIRECTORY)) continue;
            if (cl_weq(fi->FileName, L".") || cl_weq(fi->FileName, L"..")) continue;
            if (cl_weq(fi->FileName, L"BOOT") || cl_weq(fi->FileName, L"boot")) continue;

            /* For this vendor dir, probe grub then shim. */
            const CHAR16 *leaves[2] = { ARCH_GRUB, ARCH_SHIM };
            const int     kinds[2]  = { FOREB_CHAIN_GRUB, FOREB_CHAIN_SHIM };
            for (int k = 0; k < 2; k++) {
                CHAR16 p[FOREB_CHAIN_PATH_LEN];
                cl_wcpy(p, FOREB_CHAIN_PATH_LEN, L"\\EFI\\");
                cl_wcat(p, FOREB_CHAIN_PATH_LEN, fi->FileName);
                cl_wcat(p, FOREB_CHAIN_PATH_LEN, L"\\");
                cl_wcat(p, FOREB_CHAIN_PATH_LEN, leaves[k]);
                if (file_exists(root, p))
                    add_result(out, dev, vol_idx, kinds[k], is_usb, p, NULL);
            }
        }
        efidir->Close(efidir);
    }

    root->Close(root);
    (void)cl_file_info_guid;   /* reserved for GetInfo-based probing */
}

/* =============================================================================
 * Public: chain_list()
 * ========================================================================== */
int chain_list(EFI_HANDLE parent_image, EFI_SYSTEM_TABLE *st,
               struct foreb_chain_list *out)
{
    (void)parent_image;
    if (!out) return -1;
    out->count = 0;
    if (!st || !st->BootServices) return -1;

    EFI_BOOT_SERVICES *bs = st->BootServices;
    UINTN n = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS st_ret =
        bs->LocateHandleBuffer(ByProtocol, &cl_sfs_guid, NULL, &n, &handles);
    if (EFI_ERROR(st_ret) || !handles) {
        cl_log("[*] chain_list: no SimpleFileSystem volumes\n");
        return 0;
    }

    cl_log("[*] chain_list: scanning "); cl_loghex(n, 4); cl_log(" volume(s)\n");
    for (UINTN i = 0; i < n; i++)
        scan_volume(bs, out, handles[i], (int)i);

    bs->FreePool(handles);
    cl_log("[*] chain_list: found "); cl_loghex((UINT64)(UINT32)out->count, 2);
    cl_log(" candidate(s)\n");
    return out->count;
}

/* =============================================================================
 * Public: chain_find_windows()
 * ---------------------------------------------------------------------------
 * Standalone Windows-only discovery. Appends (does NOT reset *out) so callers
 * can layer it after chain_list(), or use it alone to answer "is Windows
 * installed?". Mirrors chain_list()'s volume-enumeration boilerplate.
 * ========================================================================== */
int chain_find_windows(EFI_HANDLE parent_image, EFI_SYSTEM_TABLE *st,
                       struct foreb_chain_list *out)
{
    (void)parent_image;
    if (!out) return -1;
    if (!st || !st->BootServices) return -1;

    EFI_BOOT_SERVICES *bs = st->BootServices;
    UINTN n = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS st_ret =
        bs->LocateHandleBuffer(ByProtocol, &cl_sfs_guid, NULL, &n, &handles);
    if (EFI_ERROR(st_ret) || !handles) {
        cl_log("[*] chain_find_windows: no SimpleFileSystem volumes\n");
        return 0;
    }

    int found = 0;
    for (UINTN i = 0; i < n; i++) {
        EFI_FILE_PROTOCOL *root = open_volume_root(bs, handles[i]);
        if (!root) continue;
        int is_usb = dp_is_usb(dp_of_handle(bs, handles[i]));
        found += scan_volume_windows(out, root, handles[i], (int)i, is_usb);
        root->Close(root);
    }

    bs->FreePool(handles);
    cl_log("[*] chain_find_windows: found "); cl_loghex((UINT64)(UINT32)found, 2);
    cl_log(" Windows Boot Manager(s)\n");
    return found;
}

/* =============================================================================
 * Public: chain_boot()
 * ========================================================================== */
EFI_STATUS chain_boot(EFI_HANDLE parent_image, EFI_SYSTEM_TABLE *st,
                      EFI_HANDLE device, const CHAR16 *path)
{
    if (!st || !st->BootServices || !parent_image || !device || !path || !path[0])
        return EFI_INVALID_PARAMETER;

    EFI_BOOT_SERVICES *bs = st->BootServices;

    cl_log("[*] chain_boot on volume handle\n");

    EFI_DEVICE_PATH_PROTOCOL *vol_dp = dp_of_handle(bs, device);
    if (!vol_dp) {
        cl_log("  [x] no device path on target volume\n");
        return EFI_NOT_FOUND;
    }

    EFI_DEVICE_PATH_PROTOCOL *img_dp = dp_make_file_path(bs, vol_dp, path);
    if (!img_dp) {
        cl_log("  [x] out of memory building loader device path\n");
        return EFI_OUT_OF_RESOURCES;
    }

    EFI_HANDLE image = NULL;
    /* Load FROM the target volume's device path (SourceBuffer=NULL) so the
     * chained loader inherits its own DeviceHandle/FilePath and can find its
     * config + modules (essential for GRUB's $prefix). */
    EFI_STATUS st_ret = foreb_LoadImage(bs, FALSE, parent_image, img_dp, NULL, 0, &image);
    bs->FreePool(img_dp);
    if (EFI_ERROR(st_ret) || !image) {
        cl_log("  [x] LoadImage(chain) failed: "); cl_loghex(st_ret, 16); cl_putc('\n');
        return EFI_ERROR(st_ret) ? st_ret : EFI_LOAD_ERROR;
    }

    /* Sanity: the chained image should itself be an application, not a driver.
     * (We don't enforce a subsystem check here; firmware LoadImage already
     * validated the PE.) */
    (void)cl_loaded_img_guid;

    cl_log("  [*] StartImage(chain) - handing off\n");
    UINTN exit_sz = 0;
    CHAR16 *exit_data = NULL;
    st_ret = foreb_StartImage(bs, image, &exit_sz, &exit_data);

    /* Chained loader returned (usually means the user backed out of GRUB). */
    cl_log("  [!] chained loader returned: "); cl_loghex(st_ret, 16); cl_putc('\n');
    if (exit_data) bs->FreePool(exit_data);
    foreb_UnloadImage(bs, image);
    return st_ret;
}

/* =============================================================================
 * Public: chain_boot_first()
 * ========================================================================== */
EFI_STATUS chain_boot_first(EFI_HANDLE parent_image, EFI_SYSTEM_TABLE *st)
{
    struct foreb_chain_list list;
    int n = chain_list(parent_image, st, &list);
    if (n <= 0) {
        cl_log("[x] chain_boot_first: nothing to chainload\n");
        return EFI_NOT_FOUND;
    }
    /* Prefer a removable/GRUB entry over shim if present; list order already
     * puts the removable default first per volume, so index 0 is a fine pick. */
    return chain_boot(parent_image, st, list.items[0].device, list.items[0].path);
}
