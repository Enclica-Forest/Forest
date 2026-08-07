#include "uefi_gop.h"
#include "uefi_boot_services.h"

/* Stored GOP info for kernel use */
static uefi_gop_info_t g_gop_info;
static int g_gop_initialized = 0;

/* EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID */
static EFI_GUID g_gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

/*
 * Initialize GOP and set highest resolution mode
 * Returns 0 on success, non-zero on failure
 */
int uefi_gop_init(EFI_SYSTEM_TABLE* SystemTable)
{
    EFI_STATUS status;
    EFI_HANDLE* handles = NULL;
    UINTN num_handles = 0;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info = NULL;
    uint64_t info_size = 0;
    uint32_t best_mode = 0;
    uint32_t best_width = 0;
    uint32_t best_height = 0;

    if (!SystemTable || !SystemTable->BootServices) {
        return -1;
    }

    EFI_BOOT_SERVICES* bs = SystemTable->BootServices;

    /* Locate all GOP handles */
    status = bs->LocateHandleBuffer(
        &g_gop_guid,
        NULL,
        &num_handles,
        &handles
    );

    if (EFI_ERROR(status) || num_handles == 0) {
        return -2;
    }

    /* Get the first GOP handle */
    status = bs->HandleProtocol(
        handles[0],
        &g_gop_guid,
        (void**)&gop
    );

    /* Free the handle buffer - we don't need it anymore */
    bs->FreePool(handles);

    if (EFI_ERROR(status) || !gop) {
        return -3;
    }

    /* Find the best (highest resolution) mode */
    for (uint32_t i = 0; i < gop->Mode->MaxMode; i++) {
        status = gop->QueryMode(gop, i, &info_size, &info);
        if (EFI_ERROR(status) || !info) {
            continue;
        }

        /* Look for the highest resolution */
        if (info->HorizontalResolution > best_width ||
            (info->HorizontalResolution == best_width &&
             info->VerticalResolution > best_height)) {
            best_width = info->HorizontalResolution;
            best_height = info->VerticalResolution;
            best_mode = i;
        }
    }

    /* Set the best mode */
    status = gop->SetMode(gop, best_mode);
    if (EFI_ERROR(status)) {
        return -4;
    }

    /* Store GOP info for kernel use */
    g_gop_info.BaseAddress = (void*)gop->Mode->FrameBufferBase;
    g_gop_info.BufferSize = gop->Mode->FrameBufferSize;
    g_gop_info.Width = gop->Mode->Info->HorizontalResolution;
    g_gop_info.Height = gop->Mode->Info->VerticalResolution;
    g_gop_info.PixelsPerScanLine = gop->Mode->Info->PixelsPerScanLine;
    g_gop_info.BitsPerPixel = 32; /* UEFI typically uses 32bpp */

    /* Store pixel format info */
    EFI_PIXEL_BITMASK* mask = &gop->Mode->Info->PixelInformation;
    g_gop_info.PixelFormat = gop->Mode->Info->PixelFormat;
    g_gop_info.RedMask = mask->RedMask;
    g_gop_info.GreenMask = mask->GreenMask;
    g_gop_info.BlueMask = mask->BlueMask;
    g_gop_info.ReservedMask = mask->ReservedMask;

    g_gop_initialized = 1;

    return 0;
}

/*
 * Get the GOP framebuffer info
 * Returns pointer to info structure, or NULL if not initialized
 */
const uefi_gop_info_t* uefi_gop_get_info(void)
{
    if (!g_gop_initialized) {
        return NULL;
    }
    return &g_gop_info;
}

/*
 * Get framebuffer base address
 */
void* uefi_gop_get_framebuffer(void)
{
    if (!g_gop_initialized) {
        return NULL;
    }
    return g_gop_info.BaseAddress;
}

/*
 * Get framebuffer size in bytes
 */
uint64_t uefi_gop_get_framebuffer_size(void)
{
    if (!g_gop_initialized) {
        return 0;
    }
    return g_gop_info.BufferSize;
}

/*
 * Get display width
 */
uint32_t uefi_gop_get_width(void)
{
    if (!g_gop_initialized) {
        return 0;
    }
    return g_gop_info.Width;
}

/*
 * Get display height
 */
uint32_t uefi_gop_get_height(void)
{
    if (!g_gop_initialized) {
        return 0;
    }
    return g_gop_info.Height;
}

/*
 * Get pixels per scanline
 */
uint32_t uefi_gop_get_pitch(void)
{
    if (!g_gop_initialized) {
        return 0;
    }
    return g_gop_info.PixelsPerScanLine;
}

/*
 * Fill framebuffer with a solid color (for testing)
 */
void uefi_gop_fill(uint32_t color)
{
    if (!g_gop_initialized || !g_gop_info.BaseAddress) {
        return;
    }

    uint32_t* fb = (uint32_t*)g_gop_info.BaseAddress;
    uint32_t pixels = g_gop_info.Width * g_gop_info.Height;

    for (uint32_t i = 0; i < pixels; i++) {
        fb[i] = color;
    }
}
