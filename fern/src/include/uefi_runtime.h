#ifndef UEFI_RUNTIME_H
#define UEFI_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>

#define EFI_RUNTIME_SERVICES_SIGNATURE 0x56524553544E5552ULL
#define EFI_RUNTIME_SERVICES_REVISION 0x00010000

typedef uint64_t EFI_STATUS;
typedef uint64_t UINTN;
typedef void *EFI_HANDLE;

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t pad1;
    uint32_t nanosecond;
    int16_t timezone;
    uint8_t daylight;
    uint8_t pad2;
} EFI_TIME;

typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t phys_addr;
    uint64_t virt_addr;
    uint64_t num_pages;
    uint64_t attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
} EFI_GUID;

typedef EFI_STATUS (*EFI_GET_TIME)(EFI_TIME *time, void *capabilities);
typedef EFI_STATUS (*EFI_SET_TIME)(EFI_TIME *time);
typedef EFI_STATUS (*EFI_GET_WAKEUP_TIME)(bool *enabled, bool *pending, EFI_TIME *time);
typedef EFI_STATUS (*EFI_SET_WAKEUP_TIME)(bool enable, EFI_TIME *time);
typedef EFI_STATUS (*EFI_SET_VIRTUAL_ADDRESS_MAP)(UINTN memory_map_size, UINTN descriptor_size,
                                                  uint32_t descriptor_version,
                                                  EFI_MEMORY_DESCRIPTOR *virtual_map);
typedef EFI_STATUS (*EFI_CONVERT_POINTER)(UINTN debug_disposition, void **address);
typedef EFI_STATUS (*EFI_GET_VARIABLE)(uint16_t *name, EFI_GUID *vendor_guid,
                                        uint32_t *attributes, UINTN *data_size, void *data);
typedef EFI_STATUS (*EFI_GET_NEXT_VARIABLE_NAME)(UINTN *variable_name_size, uint16_t *variable_name,
                                                  EFI_GUID *vendor_guid);
typedef EFI_STATUS (*EFI_SET_VARIABLE)(uint16_t *name, EFI_GUID *vendor_guid,
                                        uint32_t attributes, UINTN data_size, void *data);
typedef EFI_STATUS (*EFI_GET_NEXT_HIGH_MONOTONIC_COUNT)(uint32_t *count);
typedef void (*EFI_RESET_SYSTEM)(int reset_type, EFI_STATUS reset_status,
                                  UINTN data_size, void *reset_data);

typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;

    EFI_GET_TIME GetTime;
    EFI_SET_TIME SetTime;
    EFI_GET_WAKEUP_TIME GetWakeupTime;
    EFI_SET_WAKEUP_TIME SetWakeupTime;
    EFI_SET_VIRTUAL_ADDRESS_MAP SetVirtualAddressMap;
    EFI_CONVERT_POINTER ConvertPointer;

    EFI_GET_VARIABLE GetVariable;
    EFI_GET_NEXT_VARIABLE_NAME GetNextVariableName;
    EFI_SET_VARIABLE SetVariable;

    EFI_GET_NEXT_HIGH_MONOTONIC_COUNT GetNextHighMonotonicCount;
    EFI_RESET_SYSTEM ResetSystem;
} EFI_RUNTIME_SERVICES;

#define EFI_RUNTIME_SERVICES_STORE 1

typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
} EFI_TABLE_HEADER;

typedef struct {
    EFI_TABLE_HEADER hdr;
    void *firmware_vendor;
    uint32_t firmware_revision;
    EFI_HANDLE console_in_handle;
    void *con_in;
    EFI_HANDLE console_out_handle;
    void *con_out;
    EFI_HANDLE standard_error_handle;
    void *std_err;
    void *runtime_services;
    void *boottime_services;
    uint64_t number_of_table_entries;
    void *configuration_table;
} EFI_SYSTEM_TABLE;

#define EFI_RESET_COLD 0
#define EFI_RESET_WARM 1
#define EFI_RESET_SHUTDOWN 2
#define EFI_RESET_PLATFORM_SPECIFIC 3

void uefi_runtime_init(EFI_SYSTEM_TABLE *system_table);
void uefi_runtime_set_virtual_address(EFI_MEMORY_DESCRIPTOR *map, UINTN count);
const EFI_RUNTIME_SERVICES *uefi_runtime_get_services(void);
bool uefi_runtime_is_initialized(void);

#endif
