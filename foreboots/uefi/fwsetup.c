/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/fwsetup.c - Reboot into firmware / UEFI setup via OsIndications.
 * =============================================================================
 * See fwsetup.h for the contract. Built with the same freestanding clang
 * invocation as the other UEFI sources (no libc; -fshort-wchar so L"" == CHAR16).
 * ==========================================================================*/

#include "fwsetup.h"

/* Vendor GUID for the standard UEFI global variables (OsIndications lives here). */
static EFI_GUID fw_global_var_guid = EFI_GLOBAL_VARIABLE;

/* Variable names. With -fshort-wchar these UCS-2 literals are CHAR16 arrays. */
static CHAR16 fw_name_supported[] = L"OsIndicationsSupported";
static CHAR16 fw_name_indications[] = L"OsIndications";

/* SetVariable attributes required for OsIndications: the firmware reads it on
 * the next boot, so it must be non-volatile and reachable from both boot- and
 * runtime services. */
#define FW_OSIND_ATTRS  (EFI_VARIABLE_NON_VOLATILE |        \
                         EFI_VARIABLE_BOOTSERVICE_ACCESS |  \
                         EFI_VARIABLE_RUNTIME_ACCESS)

/* Read a UINT64 global variable. Returns 1 on success (value in *out, zero-
 * padded if the firmware stored fewer than 8 bytes), 0 otherwise. */
static int fw_read_u64(EFI_RUNTIME_SERVICES *rt, CHAR16 *name, UINT64 *out)
{
    UINT64 v = 0;
    UINTN  sz = sizeof(v);
    EFI_STATUS st;

    if (!rt || !rt->GetVariable || !name || !out) return 0;
    st = rt->GetVariable(name, &fw_global_var_guid, NULL, &sz, &v);
    if (EFI_ERROR(st)) return 0;   /* not present / too small for our buffer */
    *out = v;
    return 1;
}

int fw_setup_supported(EFI_RUNTIME_SERVICES *rt)
{
    UINT64 sup = 0;
    if (!fw_read_u64(rt, fw_name_supported, &sup)) return 0;
    return (sup & EFI_OS_INDICATIONS_BOOT_TO_FW_UI) ? 1 : 0;
}

int fw_boot_to_setup(EFI_RUNTIME_SERVICES *rt)
{
    UINT64 sup = 0, ind = 0;
    EFI_STATUS st;

    if (!rt || !rt->GetVariable || !rt->SetVariable || !rt->ResetSystem)
        return FW_SETUP_ERROR;

    /* 1. Firmware must advertise the capability. */
    if (!fw_read_u64(rt, fw_name_supported, &sup) ||
        !(sup & EFI_OS_INDICATIONS_BOOT_TO_FW_UI))
        return FW_SETUP_UNSUPPORTED;

    /* 2. Read-modify-write OsIndications (may not exist yet -> default 0). */
    (void)fw_read_u64(rt, fw_name_indications, &ind);
    ind |= EFI_OS_INDICATIONS_BOOT_TO_FW_UI;

    st = rt->SetVariable(fw_name_indications, &fw_global_var_guid,
                         FW_OSIND_ATTRS, sizeof(ind), &ind);
    if (EFI_ERROR(st))
        return FW_SETUP_ERROR;

    /* 3. Reboot; the firmware honours the pending indication and enters setup. */
    rt->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);

    /* Not reached if the reset fires. */
    return FW_SETUP_OK;
}
