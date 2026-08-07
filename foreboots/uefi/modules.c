/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/modules.c - Multiboot1 module loader (ESP files -> mb_module[] array).
 * =============================================================================
 * Freestanding (no libc). Same clang invocation as bootx64.c/config.c; linked
 * into BOOTX64.EFI. Reuses config.c's ESP helpers (esp_open_root /
 * esp_ascii_to_char16) so all asset loading shares one code path.
 *
 * Memory placement (per the multiboot recon):
 *   - Module PAYLOADS (the actual initrd bytes) go in firmware-allocated pages
 *     constrained below 4 GiB via AllocatePages(AllocateMaxAddress). Those pages
 *     are owned by the loader and survive ExitBootServices, and their addresses
 *     fit the 32-bit mb_module.mod_start/mod_end fields the kernel reads.
 *   - The mb_module[] ARRAY + per-module name strings go at the fixed low slot
 *     0x1900 (FOREB_MB_MODULE_ARRAY_ADDR) inside the loader-reserved
 *     0x1000..0x7FFF region, just past the multiboot_info at 0x1800.
 * =============================================================================
 */

#include "modules.h"
#include "config.h"   /* esp_open_root / esp_ascii_to_char16 */

/* Local GUID copy for GetInfo(EFI_FILE_INFO). */
static EFI_GUID mod_file_info_guid = EFI_FILE_INFO_ID;

#define MOD_PAGE_SIZE   4096ULL
#define MOD_PAGES_FOR(sz)  (((sz) + MOD_PAGE_SIZE - 1) / MOD_PAGE_SIZE)

/* Byte copy (static; compiler-emitted memcpy resolves to bootx64.c's global). */
static void bcopy_str(char *dst, const char *src, UINTN cap)
{
    UINTN i = 0;
    if (cap == 0) return;
    for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static UINTN str_len(const char *s)
{
    UINTN n = 0;
    while (s && s[n]) n++;
    return n;
}

/* -----------------------------------------------------------------------------
 * Load one ESP file into page-allocated memory below 4 GiB.
 *   root       - already-open ESP root directory
 *   bs         - live BootServices
 *   path       - ASCII ESP path ('/' or '\' separators)
 *   out_addr   - receives the physical load address
 *   out_size   - receives the byte length
 * Returns EFI_SUCCESS or an EFI error (out_* left 0 on failure).
 * -------------------------------------------------------------------------- */
static EFI_STATUS load_one(EFI_FILE_PROTOCOL *root, EFI_BOOT_SERVICES *bs,
                           const char *path,
                           EFI_PHYSICAL_ADDRESS *out_addr, UINTN *out_size)
{
    EFI_STATUS st;
    EFI_FILE_PROTOCOL *f = NULL;
    CHAR16 wpath[FOREB_CFG_PATH_LEN + 2];
    UINT8 infobuf[512];
    UINTN infosz = sizeof(infobuf);
    UINTN fsize, pages, done = 0;
    EFI_PHYSICAL_ADDRESS addr = 0xFFFFFFFFULL;   /* AllocateMaxAddress ceiling */

    if (out_addr) *out_addr = 0;
    if (out_size) *out_size = 0;
    if (!root || !bs || !path || !out_addr || !out_size) return EFI_INVALID_PARAMETER;

    esp_ascii_to_char16(path, wpath, FOREB_CFG_PATH_LEN + 2);
    st = root->Open(root, &f, wpath, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !f) return EFI_ERROR(st) ? st : EFI_NOT_FOUND;

    st = f->GetInfo(f, &mod_file_info_guid, &infosz, infobuf);
    if (EFI_ERROR(st)) { f->Close(f); return st; }
    fsize = (UINTN)((EFI_FILE_INFO *)infobuf)->FileSize;
    if (fsize == 0) { f->Close(f); return EFI_LOAD_ERROR; }

    pages = (UINTN)MOD_PAGES_FOR((UINT64)fsize);
    /* AllocateMaxAddress: the allocated region END must be <= *addr. Keeping it
     * under 4 GiB guarantees mod_start/mod_end fit the 32-bit fields. */
    st = bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(st)) { f->Close(f); return st; }

    UINT8 *dst = (UINT8 *)(UINTN)addr;
    const UINTN CHUNK = 256u * 1024u;
    while (done < fsize) {
        UINTN want = fsize - done;
        if (want > CHUNK) want = CHUNK;
        UINTN got = want;
        st = f->Read(f, &got, dst + done);
        if (EFI_ERROR(st)) {
            bs->FreePages(addr, pages);
            f->Close(f);
            return st;
        }
        if (got == 0) break;   /* EOF / short read */
        done += got;
    }
    f->Close(f);

    *out_addr = addr;
    *out_size = done;
    return EFI_SUCCESS;
}

EFI_STATUS modules_load(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                        const struct forebo_menuentry *entry,
                        struct multiboot_info *mbi,
                        struct foreboots_boot_info *boot_info)
{
    if (!bs || !mbi) return EFI_INVALID_PARAMETER;
    if (!entry || entry->module_count <= 0)
        return EFI_SUCCESS;   /* nothing to do */

    EFI_FILE_PROTOCOL *root = NULL;
    EFI_STATUS st = esp_open_root(image, bs, &root);
    if (EFI_ERROR(st) || !root) return EFI_ERROR(st) ? st : EFI_NOT_FOUND;

    struct mb_module *arr = (struct mb_module *)(UINTN)FOREB_MB_MODULE_ARRAY_ADDR;
    char *strpool     = (char *)(UINTN)FOREB_MB_MODULE_STR_ADDR;
    char *strpool_end = (char *)(UINTN)FOREB_MB_MODULE_STR_END;

    int want = entry->module_count;
    if (want > FOREB_CFG_MAX_MODULES) want = FOREB_CFG_MAX_MODULES;

    int loaded = 0;
    int failed = 0;
    for (int i = 0; i < want; i++) {
        const char *path = entry->modules[i];
        if (!path[0]) continue;

        EFI_PHYSICAL_ADDRESS maddr = 0;
        UINTN msize = 0;
        st = load_one(root, bs, path, &maddr, &msize);
        if (EFI_ERROR(st)) { failed++; continue; }

        struct mb_module *m = &arr[loaded];
        m->mod_start = (foreb_u32)maddr;
        m->mod_end   = (foreb_u32)(maddr + msize);
        m->reserved  = 0;

        /* Copy the module's path into the low string pool as its cmdline/name,
         * if there is room; otherwise leave string = 0 (spec-legal). */
        UINTN need = str_len(path) + 1;
        if (strpool + need <= strpool_end) {
            bcopy_str(strpool, path, need);
            m->string = (foreb_u32)(UINTN)strpool;
            strpool += need;
        } else {
            m->string = 0;
        }

        /* Mirror the first (primary) module into foreboots_boot_info as initrd. */
        if (loaded == 0 && boot_info) {
            boot_info->initrd_addr = (foreb_u32)maddr;
            boot_info->initrd_size = (foreb_u32)msize;
            boot_info->flags |= FOREB_BIF_INITRD;
        }

        loaded++;
    }

    root->Close(root);

    if (loaded > 0) {
        mbi->flags |= MB_FLAG_MODS;
        mbi->mods_count = (foreb_u32)loaded;
        mbi->mods_addr  = FOREB_MB_MODULE_ARRAY_ADDR;
    }

    if (failed > 0) return EFI_NOT_FOUND;   /* partial: some modules skipped */
    return EFI_SUCCESS;
}
