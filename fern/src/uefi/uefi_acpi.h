#ifndef UEFI_ACPI_H
#define UEFI_ACPI_H

#include <stdint.h>
#include <stdbool.h>
#include "uefi_runtime.h"

#define ACPI_20_TABLE_GUID \
    { 0x8868E871, 0xE4F1, 0x11D3, { 0x22, 0x9C, 0x00, 0x80, 0xC7, 0xB4, 0xD6, 0x9F } }

#define ACPI_TABLE_GUID \
    { 0xEB9D2D2F, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0xFD } }

#define ACPI_RSDP_SIGNATURE "RSD PTR "
#define ACPI_MADT_SIGNATURE "APIC"
#define ACPI_HPET_SIGNATURE "HPET"
#define ACPI_FADT_SIGNATURE "FACP"
#define ACPI_MCFG_SIGNATURE "MCFG"
#define ACPI_XSDT_SIGNATURE "XSDT"
#define ACPI_RSDT_SIGNATURE "RSDT"

typedef struct {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
} __attribute__((packed)) uefi_acpi_rsdp_v1_t;

typedef struct {
    uefi_acpi_rsdp_v1_t v1;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) uefi_acpi_rsdp_t;

typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) uefi_acpi_table_header_t;

typedef struct {
    uefi_acpi_table_header_t header;
    uint32_t entries[];
} __attribute__((packed)) uefi_acpi_xsdt_t;

typedef struct {
    uefi_acpi_table_header_t header;
    uint32_t entries[];
} __attribute__((packed)) uefi_acpi_rsdt_t;

#define uefi_acpi_table_signature_match(table, sig) \
    (!__builtin_memcmp((table)->signature, (sig), 4))

bool uefi_find_acpi_rsdp(EFI_SYSTEM_TABLE *system_table, uefi_acpi_rsdp_t **out_rsdp);
const uefi_acpi_xsdt_t *uefi_find_acpi_xsdt(const uefi_acpi_rsdp_t *rsdp);
const uefi_acpi_table_header_t *uefi_find_acpi_table(const uefi_acpi_xsdt_t *xsdt, const char *signature);
bool uefi_acpi_init(EFI_SYSTEM_TABLE *system_table);

const uefi_acpi_rsdp_t *uefi_acpi_get_rsdp(void);
const uefi_acpi_xsdt_t *uefi_acpi_get_xsdt(void);
const uefi_acpi_table_header_t *uefi_acpi_get_madt(void);
const uefi_acpi_table_header_t *uefi_acpi_get_hpet(void);
const uefi_acpi_table_header_t *uefi_acpi_get_fadt(void);
const uefi_acpi_table_header_t *uefi_acpi_get_mcfg(void);

#endif
