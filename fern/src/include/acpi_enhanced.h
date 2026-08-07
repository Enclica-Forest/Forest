#ifndef ACPI_ENHANCED_H
#define ACPI_ENHANCED_H

#include "acpi.h"
#include "types.h"
#include <stdbool.h>

/* Enhanced ACPI Table Signatures */
#define ACPI_SIG_FADT           "FACP"
#define ACPI_SIG_MADT           "APIC"
#define ACPI_SIG_DSDT           "DSDT"
#define ACPI_SIG_SSDT           "SSDT"
#define ACPI_SIG_PSDT           "PSDT"
#define ACPI_SIG_XSDT           "XSDT"
#define ACPI_SIG_RSDT           "RSDT"
#define ACPI_SIG_FACS           "FACS"
#define ACPI_SIG_SBST           "SBST"
#define ACPI_SIG_ECDT           "ECDT"
#define ACPI_SIG_HPET           "HPET"
#define ACPI_SIG_MCFG           "MCFG"
#define ACPI_SIG_ECDT           "ECDT"
#define ACPI_SIG_SRAT           "SRAT"
#define ACPI_SIG_SLIT           "SLIT"
#define ACPI_SIG_BERT           "BERT"
#define ACPI_SIG_HEST           "HEST"
#define ACPI_SIG_MSCT           "MSCT"
#define ACPI_SIG_DMAR           "DMAR"
#define ACPI_SIG_TCPA           "TCPA"
#define ACPI_SIG_ASF            "ASF!"
#define ACPI_SIG_BOOT           "BOOT"
#define ACPI_SIG_CPEP           "CPEP"
#define ACPI_SIG_DBGP           "DBGP"
#define ACPI_SIG_DBG2           "DBG2"
#define ACPI_SIG_EINJ           "EINJ"
#define ACPI_SIG_ERST           "ERST"
#define ACPI_SIG_FPDT           "FPDT"
#define ACPI_SIG_GTDT           "GTDT"
#define ACPI_SIG_HMAT           "HMAT"
#define ACPI_SIG_MSCT           "MSCT"
#define ACPI_SIG_PCCT           "PCCT"
#define ACPI_SIG_PMTT           "PMTT"
#define ACPI_SIG_RSDP           "RSD PTR "
#define ACPI_SIG_S3PT           "S3PT"
#define ACPI_SIG_SDEV           "SDEV"
#define ACPI_SIG_SPMI           "SPMI"
#define ACPI_SIG_TPM2           "TPM2"
#define ACPI_SIG_UEFI           "UEFI"
#define ACPI_SIG_WAET           "WAET"
#define ACPI_SIG_WDAT           "WDAT"
#define ACPI_SIG_WDDT           "WDDT"
#define ACPI_SIG_WDRT           "WDRT"

/* AML (ACPI Machine Language) Opcodes */
#define AML_ZERO_OP             0x00
#define AML_ONE_OP              0x01
#define AML_ALIAS_OP            0x06
#define AML_NAME_OP             0x08
#define AML_BYTE_PREFIX         0x0A
#define AML_WORD_PREFIX         0x0B
#define AML_DWORD_PREFIX        0x0C
#define AML_STRING_PREFIX       0x0D
#define AML_QWORD_PREFIX        0x0E
#define AML_PACKAGE_OP          0x12
#define AML_VAR_PACKAGE_OP      0x13
#define AML_BUFFER_OP           0x11
#define AML_METHOD_OP           0x14
#define AML_EXTERNAL_OP         0x15
#define AML_DUAL_NAME_PREFIX    0x2E
#define AML_MULTI_NAME_PREFIX   0x2F
#define AML_ROOT_CHAR           '\\'
#define AML_PARENT_PREFIX       '^'
#define AML_NAME_CHAR_TAIL      '.'

/* Extended AML Opcodes */
#define AML_EXT_OP             0x5B
#define AML_EXT_PREFIX(x)      (AML_EXT_OP | ((x) << 8))

#define AML_LNOTEQUAL_OP        AML_EXT_PREFIX(0x92)
#define AML_LLESSEQUAL_OP       AML_EXT_PREFIX(0x93)
#define AML_LGREATEREQUAL_OP    AML_EXT_PREFIX(0x94)
#define AML_LEQUAL_OP          AML_EXT_PREFIX(0x93)
#define AML_LGREATER_OP        AML_EXT_PREFIX(0x94)
#define AML_LLESS_OP           AML_EXT_PREFIX(0x95)

/* ACPI Object Types */
typedef enum {
    ACPI_TYPE_ANY = 0,
    ACPI_TYPE_INTEGER,
    ACPI_TYPE_STRING,
    ACPI_TYPE_BUFFER,
    ACPI_TYPE_PACKAGE,
    ACPI_TYPE_FIELD_UNIT,
    ACPI_TYPE_DEVICE,
    ACPI_TYPE_EVENT,
    ACPI_TYPE_METHOD,
    ACPI_TYPE_MUTEX,
    ACPI_TYPE_REGION,
    ACPI_TYPE_POWER,
    ACPI_TYPE_PROCESSOR,
    ACPI_TYPE_THERMAL,
    ACPI_TYPE_BUFFER_FIELD,
    ACPI_TYPE_DDB_HANDLE,
    ACPI_TYPE_DEBUG_OBJECT,
    ACPI_TYPE_LOCAL_XREF,
    ACPI_TYPE_LOCAL_REGION_FIELD,
    ACPI_TYPE_LOCAL_BANK_FIELD,
    ACPI_TYPE_LOCAL_INDEX_FIELD,
    ACPI_TYPE_LOCAL_REFERENCE,
    ACPI_TYPE_LOCAL_ALIAS,
    ACPI_TYPE_LOCAL_METHOD_ALIAS,
    ACPI_TYPE_LOCAL_SCOPE,
    ACPI_TYPE_NOTIFY,
    ACPI_TYPE_ADDRESS_HANDLER,
    ACPI_TYPE_RESOURCE,
    ACPI_TYPE_RESOURCE_FIELD,
    ACPI_TYPE_RESOURCE_TEMPLATE
} acpi_object_type_t;

/* AML Parser State */
typedef struct acpi_parser_state {
    const uint8_t *aml_start;
    const uint8_t *aml_end;
    const uint8_t *current_pos;
    uint32_t aml_length;
    const char *namespace_path;
} acpi_parser_state_t;

/* AML Object */
typedef struct acpi_object {
    acpi_object_type_t type;
    union {
        uint64_t integer;
        struct {
            const char *ptr;
            uint32_t length;
        } string;
        struct {
            const uint8_t *ptr;
            uint32_t length;
        } buffer;
        struct {
            uint32_t count;
            struct acpi_object **objects;
        } package;
        struct {
            const char *name;
            struct acpi_object *value;
        } named;
    } data;
    struct acpi_object *next;
} acpi_object_t;

/* Enhanced Table Information */
typedef struct acpi_table_info {
    char signature[5];
    uint32_t length;
    uint8_t revision;
    uint32_t oem_revision;
    char oem_id[7];
    char oem_table_id[9];
    const acpi_sdt_header_t *header;
    void *mapped_data;
    struct acpi_table_info *next;
} acpi_table_info_t;

/* FADT (Fixed ACPI Description Table) Enhanced Fields */
typedef struct {
    acpi_sdt_header_t header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t pm1_evt_len;
    uint8_t pm1_cnt_len;
    uint8_t pm2_cnt_len;
    uint8_t pm_tmr_len;
    uint8_t gpe0_blk_len;
    uint8_t gpe1_blk_len;
    uint8_t gpe1_base;
    uint8_t cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alrm;
    uint8_t mon_alrm;
    uint8_t century;
    uint16_t iapc_boot_arch;
    uint8_t reserved2;
    uint32_t flags;
    uint32_t reset_reg;
    uint8_t reset_value;
    uint8_t reserved3[3];
} __attribute__((packed)) acpi_fadt_enhanced_t;

/* MADT (Multiple APIC Description Table) Enhanced */
typedef struct {
    acpi_madt_header_t header;
    /* Entries follow */
} __attribute__((packed)) acpi_madt_enhanced_t;

/* MADT Entry Types */
enum {
    ACPI_MADT_TYPE_LAPIC = 0,
    ACPI_MADT_TYPE_IOAPIC = 1,
    ACPI_MADT_TYPE_IRQ_SRC_OVERRIDE = 2,
    ACPI_MADT_TYPE_NMI_SRC = 3,
    ACPI_MADT_TYPE_LAPIC_NMI = 4,
    ACPI_MADT_TYPE_LAPIC_ADDR_OVERRIDE = 5,
    ACPI_MADT_TYPE_IOSAPIC = 6,
    ACPI_MADT_TYPE_LSAPIC = 7,
    ACPI_MADT_TYPE_PLATFORM_IRQ_SRC = 8,
    ACPI_MADT_TYPE_LX2APIC = 9,
    ACPI_MADT_TYPE_LX2APIC_NMI = 10
};

/* MADT Local APIC Structure */
typedef struct {
    uint8_t type;
    uint8_t length;
    uint8_t processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed)) acpi_madt_lapic_struct_t;

/* MADT I/O APIC Structure */
typedef struct {
    uint8_t type;
    uint8_t length;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_addr;
    uint32_t global_irq_base;
} __attribute__((packed)) acpi_madt_ioapic_struct_t;

/* MADT IRQ Source Override Structure */
typedef struct {
    uint8_t type;
    uint8_t length;
    uint8_t bus;
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed)) acpi_madt_irq_override_t;

/* HPET (High Precision Event Timer) Table */
typedef struct {
    acpi_sdt_header_t header;
    uint32_t event_timer_block_id;
    struct {
        uint32_t base_address;
        uint16_t hpet_number;
        uint16_t minimum_clock_tick;
        uint8_t page_protection_and_oem_attribute;
    } __attribute__((packed)) event_timer_block;
} __attribute__((packed)) acpi_hpet_t;

/* AML Parser Functions */
int acpi_aml_init_parser(acpi_parser_state_t *parser, const uint8_t *aml, uint32_t length);
acpi_object_t *acpi_aml_parse_object(acpi_parser_state_t *parser);
acpi_object_t *acpi_aml_parse_named_object(acpi_parser_state_t *parser, const char *name);
void acpi_aml_free_object(acpi_object_t *object);
int acpi_aml_execute_method(const char *method_path, acpi_object_t *args, uint32_t arg_count, acpi_object_t **result);

/* Enhanced Table Functions */
int acpi_enumerate_all_tables(void);
const acpi_table_info_t *acpi_find_table_by_signature(const char *signature);
const acpi_table_info_t *acpi_get_table_list(void);
int acpi_map_table_data(acpi_table_info_t *table_info);
void acpi_unmap_table_data(acpi_table_info_t *table_info);

/* Specific Table Access Functions */
const acpi_hpet_t *acpi_get_hpet(void);
int acpi_parse_madt(void);
int acpi_parse_dsd(void);

/* AML Namespace Functions */
int acpi_aml_build_namespace(void);
acpi_object_t *acpi_aml_resolve_name(const char *name);
int acpi_aml_evaluate_method(const char *method_path);

/* Power Management Functions */
int acpi_get_sleep_states(void);
int acpi_enter_sleep_state(uint8_t sleep_state);
int acpi_get_power_button_status(void);

/* Enhanced Event Handling */
typedef void (*acpi_event_handler_t)(uint32_t event_type, void *context);
int acpi_register_event_handler(uint32_t event_type, acpi_event_handler_t handler, void *context);
void acpi_unregister_event_handler(uint32_t event_type);
int acpi_enable_fixed_event(uint32_t event);
int acpi_disable_fixed_event(uint32_t event);
int acpi_clear_fixed_event(uint32_t event);
int acpi_get_fixed_event_status(uint32_t event, uint32_t *status);

/* Resource Management */
typedef struct acpi_resource {
    uint8_t type;
    uint16_t length;
    union {
        struct {
            uint8_t consumer_producer;
            uint8_t decode;
            uint8_t min_address_fixed;
            uint8_t max_address_fixed;
            uint8_t address_space;
            uint8_t resource_usage;
            uint8_t shareable;
            uint8_t type_specific_flags;
            uint64_t address_min;
            uint64_t address_max;
            uint64_t address_translation;
            uint64_t address_length;
            uint64_t resource_source;
        } address_space;
        struct {
            uint8_t resource_usage;
            uint8_t interrupt_type_flags;
            uint8_t interrupt_table_length;
            uint32_t interrupt_number[16];
        } interrupt;
    } data;
    struct acpi_resource *next;
} acpi_resource_t;

int acpi_parse_resources(acpi_object_t *resource_template, acpi_resource_t **resources);
void acpi_free_resources(acpi_resource_t *resources);

/* Debug and Diagnostics */
void acpi_dump_table(const acpi_sdt_header_t *header);
void acpi_dump_aml_tree(const acpi_object_t *object, int indent);
int acpi_validate_tables(void);
int acpi_run_diagnostics(void);

#endif /* ACPI_ENHANCED_H */