/*
 * Forest OS - AArch64 UEFI Boot Entry Point
 *
 * Called from uefi_boot.S after BSS is zeroed and the stack is set up.
 * Initialises UEFI console, GOP framebuffer, memory map, and ACPI tables,
 * then hands off to kernel_main() via a boot_info struct.
 *
 * Mirrors src/uefi/uefi_main.c but for AArch64 UEFI firmware.
 */

#ifdef UEFI_BOOT

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

/* EFIAPI: UEFI uses the Microsoft AArch64 calling convention on ARM64.
 * On AArch64 the standard ABI is compatible, so no attribute is needed. */
#ifndef EFIAPI
#  if defined(__aarch64__) || defined(_M_ARM64)
#    define EFIAPI
#  elif defined(__x86_64__) || defined(_M_X64)
#    define EFIAPI __attribute__((ms_abi))
#  else
#    define EFIAPI
#  endif
#endif

#include "../uefi/uefi.h"

/* ------------------------------------------------------------------ */
/* Forward declarations for UEFI helper modules                        */
/* ------------------------------------------------------------------ */

/* Console (uefi_console.c) */
extern void uefi_console_init(void *system_table);
extern void uefi_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
extern void uefi_puts(const char *str);

/* Runtime services (uefi_runtime.c) */
extern void uefi_runtime_init(void *system_table);

/* GOP framebuffer (uefi_gop.c) */
extern int  uefi_gop_init(EFI_SYSTEM_TABLE *SystemTable);
extern const uefi_gop_info_t *uefi_gop_get_info(void);

/* Memory map (uefi_memory.c) */
extern int  uefi_get_memory_map(EFI_SYSTEM_TABLE *SystemTable);
extern memory_region_t *uefi_get_regions(uint32_t *count);
extern int  uefi_pmm_init(EFI_SYSTEM_TABLE *SystemTable);

/* ACPI (uefi_acpi.c) */
extern bool uefi_acpi_init(EFI_SYSTEM_TABLE *system_table);

/* ACPI RSDP pointer getter (defined in uefi_acpi.c) */
typedef struct {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) uefi_acpi_rsdp_t;

extern const uefi_acpi_rsdp_t *uefi_acpi_get_rsdp(void);

/* Kernel entry point */
extern int kernel_main(void);

/* ------------------------------------------------------------------ */
/* Global UEFI handoff state                                           */
/* ------------------------------------------------------------------ */

static EFI_HANDLE            g_image_handle;
static EFI_SYSTEM_TABLE     *g_system_table;
static EFI_BOOT_SERVICES    *g_boot_services;
static EFI_RUNTIME_SERVICES *g_runtime_services;

/*
 * Boot information passed from UEFI firmware to the kernel.
 */
typedef struct {
    uint64_t framebuffer_addr;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_bpp;
    uint64_t memory_map;
    uint32_t memory_map_size;
    uint64_t acpi_rsdp;
    uint64_t rtsptr;
} uefi_boot_info_t;

static uefi_boot_info_t g_boot_info;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void uefi_print_banner(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con)
{
    if (!con)
        return;

    con->SetAttribute(con, EFI_TEXT_ATTR(EFI_LIGHTCYAN, EFI_BACKGROUND_BLACK));
    con->OutputString(con, (uint16_t *)L"\r\n");
    con->OutputString(con, (uint16_t *)L" ==================================\r\n");
    con->OutputString(con, (uint16_t *)L"   Forest OS  -  AArch64 UEFI Boot\r\n");
    con->OutputString(con, (uint16_t *)L" ==================================\r\n");
    con->OutputString(con, (uint16_t *)L"\r\n");
    con->SetAttribute(con, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BACKGROUND_BLACK));
}

static void uefi_status_line(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con,
                              const char *label, bool ok)
{
    if (!con)
        return;

    con->OutputString(con, (uint16_t *)L"  ");
    if (ok) {
        con->SetAttribute(con, EFI_TEXT_ATTR(EFI_LIGHTGREEN, EFI_BACKGROUND_BLACK));
        con->OutputString(con, (uint16_t *)L"[OK]  ");
    } else {
        con->SetAttribute(con, EFI_TEXT_ATTR(EFI_LIGHTRED, EFI_BACKGROUND_BLACK));
        con->OutputString(con, (uint16_t *)L"[FAIL]");
    }
    con->SetAttribute(con, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BACKGROUND_BLACK));

    for (; *label; label++) {
        uint16_t ch[2] = { (uint16_t)(unsigned char)*label, 0 };
        con->OutputString(con, ch);
    }
    con->OutputString(con, (uint16_t *)L"\r\n");
}

/* ------------------------------------------------------------------ */
/* UEFI Entry Point (called from uefi_boot.S)                         */
/* ------------------------------------------------------------------ */

void uefi_main_c(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS status;

    /* ---- 1. Store firmware handles --------------------------------- */
    g_image_handle    = ImageHandle;
    g_system_table    = SystemTable;

    if (!SystemTable || !SystemTable->BootServices)
        return;

    g_boot_services    = SystemTable->BootServices;
    g_runtime_services = SystemTable->RuntimeServices;

    /* ---- 2. Initialise UEFI console -------------------------------- */
    uefi_console_init((void *)SystemTable);

    if (SystemTable->ConOut) {
        SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
        uefi_print_banner(SystemTable->ConOut);
    }

    /* ---- 3. Initialise runtime services ---------------------------- */
    uefi_runtime_init((void *)SystemTable);

    /* ---- 4. Initialise GOP framebuffer ----------------------------- */
    bool gop_ok = (uefi_gop_init(SystemTable) == 0);
    uefi_status_line(SystemTable->ConOut, "GOP framebuffer", gop_ok);

    if (gop_ok) {
        const uefi_gop_info_t *gop = uefi_gop_get_info();
        if (gop) {
            g_boot_info.framebuffer_addr   = (uint64_t)(uintptr_t)gop->BaseAddress;
            g_boot_info.framebuffer_width  = gop->Width;
            g_boot_info.framebuffer_height = gop->Height;
            g_boot_info.framebuffer_pitch  = gop->PixelsPerScanLine * 4;
            g_boot_info.framebuffer_bpp    = gop->BitsPerPixel;
        }
    }

    /* ---- 5. Retrieve UEFI memory map ------------------------------- */
    bool mem_ok = (uefi_get_memory_map(SystemTable) == 0);
    uefi_status_line(SystemTable->ConOut, "Memory map", mem_ok);

    if (mem_ok) {
        uint32_t region_count = 0;
        memory_region_t *regions = uefi_get_regions(&region_count);
        if (regions && region_count > 0) {
            g_boot_info.memory_map      = (uint64_t)(uintptr_t)regions;
            g_boot_info.memory_map_size = region_count;
        }
    }

    /* ---- 6. Locate ACPI RSDP via configuration tables -------------- */
    bool acpi_ok = uefi_acpi_init(SystemTable);
    uefi_status_line(SystemTable->ConOut, "ACPI tables", acpi_ok);

    if (acpi_ok) {
        const uefi_acpi_rsdp_t *rsdp = uefi_acpi_get_rsdp();
        if (rsdp)
            g_boot_info.acpi_rsdp = (uint64_t)(uintptr_t)rsdp;
    }

    /* ---- 7. Store runtime services pointer ------------------------- */
    g_boot_info.rtsptr = (uint64_t)(uintptr_t)g_runtime_services;

    /* ---- 7.5. Initialise PMM from UEFI memory map ------------------ */
    bool pmm_ok = (uefi_pmm_init(SystemTable) == 0);
    uefi_status_line(SystemTable->ConOut, "PMM init", pmm_ok);

    /* ---- 8. Print summary ------------------------------------------ */
    if (SystemTable->ConOut) {
        uefi_printf("  Framebuffer: %ux%u @ 0x%lx\n",
            g_boot_info.framebuffer_width,
            g_boot_info.framebuffer_height,
            g_boot_info.framebuffer_addr);
        uefi_printf("  Memory regions: %u\n",
            g_boot_info.memory_map_size);
        if (g_boot_info.acpi_rsdp)
            uefi_printf("  ACPI RSDP: 0x%lx\n", g_boot_info.acpi_rsdp);
    }

    /* ---- 9. Hand off to the kernel --------------------------------- */
    kernel_main();

    /* kernel returned (should not happen) -- exit boot services */
    {
        UINTN memory_map_size   = 0;
        UINTN map_key           = 0;
        UINTN descriptor_size   = 0;
        uint32_t descriptor_ver = 0;

        status = g_boot_services->GetMemoryMap(
            &memory_map_size, NULL, &map_key,
            &descriptor_size, &descriptor_ver);

        if (status == EFI_BUFFER_TOO_SMALL) {
            memory_map_size += descriptor_size * 4;
            g_boot_services->ExitBootServices(g_image_handle, map_key);
        }
    }

    for (;;)
        asm volatile("wfi");
}

#endif /* UEFI_BOOT */
