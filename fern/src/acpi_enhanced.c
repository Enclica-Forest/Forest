/*
 * Enhanced ACPI Implementation for Fern
 * Provides comprehensive ACPI table enumeration and AML interpretation
 */

#include "include/acpi_enhanced.h"
#include "include/memory_safe.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/panic.h"
#include "include/spinlock.h"

/* Global state */
static acpi_table_info_t *g_table_list = NULL;
static acpi_parser_state_t g_aml_parser = {0};
static acpi_object_t *g_aml_namespace = NULL;
static spinlock_t g_acpi_lock = SPINLOCK_UNLOCKED;
static bool g_acpi_enhanced_initialized = false;

/* Forward declarations */
static int acpi_add_table_info(const acpi_sdt_header_t *header);
static acpi_object_t *acpi_aml_parse_pkg_length(acpi_parser_state_t *parser, uint32_t *length);
static acpi_object_t *acpi_aml_parse_name_string(acpi_parser_state_t *parser);
static acpi_object_t *acpi_aml_parse_buffer(acpi_parser_state_t *parser);
static acpi_object_t *acpi_aml_parse_package(acpi_parser_state_t *parser);
static acpi_object_t *acpi_aml_parse_method(acpi_parser_state_t *parser);

/*
 * Initialize enhanced ACPI subsystem
 */
int acpi_enhanced_init(void)
{
    int ret;
    
    if (g_acpi_enhanced_initialized) {
        return 0;
    }
    
    debuglog(DEBUG_INFO,"ACPI: Initializing enhanced ACPI support\n");
    
    /* First, initialize basic ACPI if not already done */
    if (!acpi_get_rsdp()) {
        if (!acpi_init()) {
            debuglog(DEBUG_INFO,"ACPI: Basic ACPI initialization failed\n");
            return -1;
        }
    }
    
    /* Enumerate all ACPI tables */
    ret = acpi_enumerate_all_tables();
    if (ret != 0) {
        debuglog(DEBUG_INFO,"ACPI: Table enumeration failed\n");
        return ret;
    }
    
    /* Parse DSDT if available */
    ret = acpi_parse_dsd();
    if (ret != 0) {
        debuglog(DEBUG_INFO,"ACPI: DSDT parsing failed\n");
        return ret;
    }
    
    /* Build AML namespace */
    ret = acpi_aml_build_namespace();
    if (ret != 0) {
        debuglog(DEBUG_INFO,"ACPI: AML namespace build failed\n");
        return ret;
    }
    
    g_acpi_enhanced_initialized = true;
    debuglog(DEBUG_INFO,"ACPI: Enhanced ACPI support initialized\n");
    
    return 0;
}

/*
 * Enumerate all ACPI tables
 */
int acpi_enumerate_all_tables(void)
{
    const acpi_rsdp_t *rsdp;
    const acpi_sdt_header_t *root_table;
    const acpi_sdt_header_t *current_table;
    uint32_t i, entry_count;
    int ret;
    
    rsdp = acpi_get_rsdp();
    if (!rsdp) {
        debuglog(DEBUG_INFO,"ACPI: RSDP not found\n");
        return -1;
    }
    
    /* Get root table (XSDT or RSDT) */
    if (rsdp->v1.revision >= 2 && rsdp->xsdt_address) {
        root_table = acpi_map_table(rsdp->xsdt_address);
        if (!root_table) {
            debuglog(DEBUG_INFO,"ACPI: Failed to map XSDT\n");
            return -1;
        }
        entry_count = (root_table->length - sizeof(acpi_sdt_header_t)) / sizeof(uint64);
    } else {
        root_table = acpi_map_table((uint32)rsdp->v1.rsdt_address);
        if (!root_table) {
            debuglog(DEBUG_INFO,"ACPI: Failed to map RSDT\n");
            return -1;
        }
        entry_count = (root_table->length - sizeof(acpi_sdt_header_t)) / sizeof(uint32);
    }
    
    debuglog(DEBUG_INFO,"ACPI: Found %d table entries\n", entry_count);
    
    /* Process each entry */
    for (i = 0; i < entry_count; i++) {
        uint64_t entry_addr;
        
        if (rsdp->v1.revision >= 2 && rsdp->xsdt_address) {
            const uint64 *entries = (const uint64*)((const uint8*)root_table + sizeof(acpi_sdt_header_t));
            entry_addr = entries[i];
        } else {
            const uint32 *entries = (const uint32*)((const uint8*)root_table + sizeof(acpi_sdt_header_t));
            entry_addr = entries[i];
        }
        
        current_table = acpi_map_table(entry_addr & 0xFFFFFFFF);
        if (current_table) {
            ret = acpi_add_table_info(current_table);
            if (ret == 0) {
                debuglog(DEBUG_INFO,"ACPI: Found table %c%c%c%c\n",
                           current_table->signature[0], current_table->signature[1],
                           current_table->signature[2], current_table->signature[3]);
            }
        }
    }
    
    return 0;
}

/*
 * Add table information to the list
 */
static int acpi_add_table_info(const acpi_sdt_header_t *header)
{
    acpi_table_info_t *table_info;
    unsigned long flags;
    
    if (!header) {
        return -1;
    }
    
    table_info = memory_heap_alloc(sizeof(acpi_table_info_t));
    if (!table_info) {
        return -1;
    }
    
    /* Copy table information */
    memcpy(table_info->signature, header->signature, 4);
    table_info->signature[4] = '\0';
    table_info->length = header->length;
    table_info->revision = header->revision;
    table_info->oem_revision = header->oem_revision;
    memcpy(table_info->oem_id, header->oem_id, 6);
    table_info->oem_id[6] = '\0';
    memcpy(table_info->oem_table_id, header->oem_table_id, 8);
    table_info->oem_table_id[8] = '\0';
    table_info->header = header;
    table_info->mapped_data = NULL;
    
    spin_lock_irqsave(&g_acpi_lock, flags);
    
    /* Add to list */
    table_info->next = g_table_list;
    g_table_list = table_info;
    
    spin_unlock_irqrestore(&g_acpi_lock, flags);
    
    return 0;
}

/*
 * Find table by signature
 */
const acpi_table_info_t *acpi_find_table_by_signature(const char *signature)
{
    acpi_table_info_t *current;
    unsigned long flags;
    
    if (!signature) {
        return NULL;
    }
    
    spin_lock_irqsave(&g_acpi_lock, flags);
    
    current = g_table_list;
    while (current) {
        if (memcmp(current->signature, signature, 4) == 0) {
            spin_unlock_irqrestore(&g_acpi_lock, flags);
            return current;
        }
        current = current->next;
    }
    
    spin_unlock_irqrestore(&g_acpi_lock, flags);
    return NULL;
}

/*
 * Get table list
 */
const acpi_table_info_t *acpi_get_table_list(void)
{
    return g_table_list;
}

/*
 * Map table data
 */
int acpi_map_table_data(acpi_table_info_t *table_info)
{
    if (!table_info || !table_info->header) {
        return -1;
    }

    if (table_info->mapped_data) {
        return 0; /* Already mapped */
    }

    /* For our simplified implementation, the header is the mapped data */
    /* In a full implementation, this would do proper physical-to-virtual mapping */
    table_info->mapped_data = (void*)table_info->header;

    return table_info->mapped_data ? 0 : -1;
}

/*
 * Parse DSDT (Differentiated System Description Table)
 */
int acpi_parse_dsd(void)
{
    const acpi_table_info_t *dsdt_info;
    const acpi_sdt_header_t *dsdt_header;
    int ret;
    
    /* Find DSDT */
    dsdt_info = acpi_find_table_by_signature("DSDT");
    if (!dsdt_info) {
        debuglog(DEBUG_INFO,"ACPI: DSDT not found\n");
        return -1;
    }
    
    /* Map DSDT data */
    ret = acpi_map_table_data((acpi_table_info_t*)dsdt_info);
    if (ret != 0) {
        debuglog(DEBUG_INFO,"ACPI: Failed to map DSDT\n");
        return ret;
    }
    
    dsdt_header = dsdt_info->header;
    
    /* Initialize AML parser */
    ret = acpi_aml_init_parser(&g_aml_parser, 
                               (const uint8_t*)dsdt_info->mapped_data + sizeof(acpi_sdt_header_t),
                               dsdt_header->length - sizeof(acpi_sdt_header_t));
    if (ret != 0) {
        debuglog(DEBUG_INFO,"ACPI: Failed to initialize AML parser\n");
        return ret;
    }
    
    debuglog(DEBUG_INFO,"ACPI: DSDT parsed successfully, AML length: %d\n", 
               g_aml_parser.aml_length);
    
    return 0;
}

/*
 * Initialize AML parser
 */
int acpi_aml_init_parser(acpi_parser_state_t *parser, const uint8_t *aml, uint32_t length)
{
    if (!parser || !aml || length == 0) {
        return -1;
    }
    
    memset(parser, 0, sizeof(acpi_parser_state_t));
    
    parser->aml_start = aml;
    parser->aml_end = aml + length;
    parser->current_pos = aml;
    parser->aml_length = length;
    parser->namespace_path = "\\";
    
    return 0;
}

/*
 * Parse AML object
 */
acpi_object_t *acpi_aml_parse_object(acpi_parser_state_t *parser)
{
    acpi_object_t *object;
    uint8_t opcode;
    uint32_t pkg_length;
    (void)pkg_length;
    
    if (!parser || parser->current_pos >= parser->aml_end) {
        return NULL;
    }
    
    opcode = *parser->current_pos;
    
    /* Allocate object */
    object = memory_heap_alloc(sizeof(acpi_object_t));
    if (!object) {
        return NULL;
    }
    
    memset(object, 0, sizeof(acpi_object_t));
    
    switch (opcode) {
        case AML_ZERO_OP:
            object->type = ACPI_TYPE_INTEGER;
            object->data.integer = 0;
            parser->current_pos++;
            break;
            
        case AML_ONE_OP:
            object->type = ACPI_TYPE_INTEGER;
            object->data.integer = 1;
            parser->current_pos++;
            break;
            
        case AML_NAME_OP:
            object->type = ACPI_TYPE_INTEGER;
            object->data.named.name = (const char*)acpi_aml_parse_name_string(parser);
            parser->current_pos++;
            object->data.named.value = acpi_aml_parse_object(parser);
            break;
            
        case AML_BYTE_PREFIX:
            parser->current_pos++;
            if (parser->current_pos < parser->aml_end) {
                object->type = ACPI_TYPE_INTEGER;
                object->data.integer = *parser->current_pos++;
            }
            break;
            
        case AML_WORD_PREFIX:
            parser->current_pos++;
            if (parser->current_pos + 1 < parser->aml_end) {
                object->type = ACPI_TYPE_INTEGER;
                object->data.integer = *(const uint16_t*)parser->current_pos;
                parser->current_pos += 2;
            }
            break;
            
        case AML_DWORD_PREFIX:
            parser->current_pos++;
            if (parser->current_pos + 3 < parser->aml_end) {
                object->type = ACPI_TYPE_INTEGER;
                object->data.integer = *(const uint32_t*)parser->current_pos;
                parser->current_pos += 4;
            }
            break;
            
        case AML_STRING_PREFIX:
            parser->current_pos++;
            object->type = ACPI_TYPE_STRING;
            object->data.string.ptr = (const char*)parser->current_pos;
            object->data.string.length = 0;
            while (parser->current_pos < parser->aml_end && *parser->current_pos != '\0') {
                parser->current_pos++;
                object->data.string.length++;
            }
            parser->current_pos++; /* Skip null terminator */
            break;
            
        case AML_BUFFER_OP:
            return acpi_aml_parse_buffer(parser);
            
        case AML_PACKAGE_OP:
            return acpi_aml_parse_package(parser);
            
        case AML_METHOD_OP:
            return acpi_aml_parse_method(parser);
            
        default:
            /* Handle extended opcodes and other cases */
            if (opcode == AML_EXT_OP && parser->current_pos + 1 < parser->aml_end) {
                uint16_t ext_opcode = AML_EXT_PREFIX(parser->current_pos[1]);
                (void)ext_opcode;
                parser->current_pos += 2;
                /* Handle extended opcodes here */
            } else {
                /* Unknown opcode, skip */
                parser->current_pos++;
            }
            memory_heap_free(object);
            return NULL;
    }
    
    return object;
}

/*
 * Parse AML buffer
 */
static acpi_object_t *acpi_aml_parse_buffer(acpi_parser_state_t *parser)
{
    acpi_object_t *object;
    uint32_t pkg_length, buffer_length;
    
    if (parser->current_pos >= parser->aml_end || *parser->current_pos != AML_BUFFER_OP) {
        return NULL;
    }
    
    parser->current_pos++; /* Skip AML_BUFFER_OP */
    
    /* Parse package length */
    acpi_aml_parse_pkg_length(parser, &pkg_length);
    
    /* Parse buffer length */
    if (parser->current_pos < parser->aml_end) {
        if (*parser->current_pos == AML_BYTE_PREFIX) {
            parser->current_pos++;
            buffer_length = *parser->current_pos++;
        } else if (*parser->current_pos == AML_WORD_PREFIX) {
            parser->current_pos++;
            buffer_length = *(const uint16_t*)parser->current_pos;
            parser->current_pos += 2;
        } else if (*parser->current_pos == AML_DWORD_PREFIX) {
            parser->current_pos++;
            buffer_length = *(const uint32_t*)parser->current_pos;
            parser->current_pos += 4;
        } else {
            buffer_length = *parser->current_pos++;
        }
    }
    
    object = memory_heap_alloc(sizeof(acpi_object_t));
    if (!object) {
        return NULL;
    }
    
    object->type = ACPI_TYPE_BUFFER;
    object->data.buffer.ptr = parser->current_pos;
    object->data.buffer.length = buffer_length;
    
    parser->current_pos += buffer_length;
    
    return object;
}

/*
 * Parse AML package
 */
static acpi_object_t *acpi_aml_parse_package(acpi_parser_state_t *parser)
{
    acpi_object_t *object;
    uint32_t pkg_length, element_count, i;
    
    if (parser->current_pos >= parser->aml_end || *parser->current_pos != AML_PACKAGE_OP) {
        return NULL;
    }
    
    parser->current_pos++; /* Skip AML_PACKAGE_OP */
    
    /* Parse package length */
    acpi_aml_parse_pkg_length(parser, &pkg_length);
    
    /* Parse element count */
    element_count = *parser->current_pos++;
    
    object = memory_heap_alloc(sizeof(acpi_object_t));
    if (!object) {
        return NULL;
    }
    
    object->type = ACPI_TYPE_PACKAGE;
    object->data.package.count = element_count;
    object->data.package.objects = memory_heap_alloc(element_count * sizeof(acpi_object_t*));
    
    /* Parse each element */
    for (i = 0; i < element_count; i++) {
        object->data.package.objects[i] = acpi_aml_parse_object(parser);
    }
    
    return object;
}

/*
 * Parse AML method
 */
static acpi_object_t *acpi_aml_parse_method(acpi_parser_state_t *parser)
{
    acpi_object_t *object;
    uint32_t pkg_length, arg_count;
    const uint8_t *method_start;
    (void)arg_count;
    
    if (parser->current_pos >= parser->aml_end || *parser->current_pos != AML_METHOD_OP) {
        return NULL;
    }
    
    parser->current_pos++; /* Skip AML_METHOD_OP */
    
    /* Parse package length */
    acpi_aml_parse_pkg_length(parser, &pkg_length);
    
    /* Parse argument count */
    arg_count = *parser->current_pos++;
    
    /* Parse name */
    const char *method_name = (const char*)acpi_aml_parse_name_string(parser);
    (void)method_name; /* Used for debugging */
    
    method_start = parser->current_pos;
    
    object = memory_heap_alloc(sizeof(acpi_object_t));
    if (!object) {
        return NULL;
    }
    
    object->type = ACPI_TYPE_METHOD;
    /* Store method information */
    
    /* Skip method body for now */
    while (parser->current_pos < parser->aml_end && 
           (uint32_t)(uintptr_t)(parser->current_pos - method_start) < pkg_length) {
        parser->current_pos++;
    }
    
    return object;
}

/*
 * Parse package length (simplified)
 */
static acpi_object_t *acpi_aml_parse_pkg_length(acpi_parser_state_t *parser, uint32_t *length)
{
    if (!parser || !length || parser->current_pos >= parser->aml_end) {
        return NULL;
    }
    
    uint8_t byte = *parser->current_pos++;
    
    if (byte & 0x80) {
        /* Multi-byte length */
        uint8_t byte_count = (byte >> 6) & 0x03;
        *length = byte & 0x0F;
        
        for (uint8_t i = 0; i < byte_count; i++) {
            if (parser->current_pos >= parser->aml_end) {
                return NULL;
            }
            *length |= (*parser->current_pos++ << (8 * (i + 2)));
        }
    } else {
        /* Single byte length */
        *length = byte;
    }
    
    return NULL; /* Not returning an object, just setting length */
}

/*
 * Parse name string (simplified)
 */
static acpi_object_t *acpi_aml_parse_name_string(acpi_parser_state_t *parser)
{
    if (!parser || parser->current_pos >= parser->aml_end) {
        return NULL;
    }
    
    const char *name_start = (const char*)parser->current_pos;
    
    if (*parser->current_pos == AML_ROOT_CHAR) {
        parser->current_pos++; /* Skip root prefix */
    } else if (*parser->current_pos == AML_PARENT_PREFIX) {
        while (parser->current_pos < parser->aml_end && 
               *parser->current_pos == AML_PARENT_PREFIX) {
            parser->current_pos++; /* Skip parent prefixes */
        }
    }
    
    /* Parse name segments */
    while (parser->current_pos < parser->aml_end) {
        if (*parser->current_pos == AML_DUAL_NAME_PREFIX) {
            parser->current_pos++;
            /* Read 4-character name */
            if (parser->current_pos + 3 < parser->aml_end) {
                parser->current_pos += 4;
            }
        } else if (*parser->current_pos == AML_MULTI_NAME_PREFIX) {
            parser->current_pos++;
            uint8_t segment_count = *parser->current_pos++;
            for (uint8_t i = 0; i < segment_count && parser->current_pos + 3 < parser->aml_end; i++) {
                parser->current_pos += 4;
            }
        } else if (*parser->current_pos == 0 || *parser->current_pos == AML_NAME_CHAR_TAIL) {
            break;
        } else {
            /* Single name segment */
            parser->current_pos += 4;
            break;
        }
    }
    
    return (acpi_object_t*)name_start;
}

/*
 * Build AML namespace (simplified implementation)
 */
int acpi_aml_build_namespace(void)
{
    debuglog(DEBUG_INFO,"ACPI: Building AML namespace\n");
    
    /* For now, just mark namespace as built */
    g_aml_namespace = memory_heap_alloc(sizeof(acpi_object_t));
    if (!g_aml_namespace) {
        return -1;
    }
    
    memset(g_aml_namespace, 0, sizeof(acpi_object_t));
    g_aml_namespace->type = ACPI_TYPE_ANY;
    
    return 0;
}

/*
 * Get HPET
 */
const acpi_hpet_t *acpi_get_hpet(void)
{
    const acpi_table_info_t *hpet_info = acpi_find_table_by_signature("HPET");
    if (!hpet_info) {
        return NULL;
    }
    
    return (const acpi_hpet_t*)hpet_info->header;
}

/*
 * Parse MADT
 */
int acpi_parse_madt(void)
{
    const acpi_table_info_t *madt_info = acpi_find_table_by_signature("APIC");
    if (!madt_info) {
        debuglog(DEBUG_INFO,"ACPI: MADT not found\n");
        return -1;
    }
    
    const acpi_madt_enhanced_t *madt = (const acpi_madt_enhanced_t*)madt_info->header;
    const uint8_t *entry_ptr = (const uint8_t*)madt + sizeof(acpi_madt_enhanced_t);
    const uint8_t *madt_end = (const uint8_t*)madt + madt->header.header.length;
    
    uint32_t lapic_count = 0, ioapic_count = 0, override_count = 0;
    
    while (entry_ptr < madt_end) {
        const acpi_madt_entry_header_t *entry = (const acpi_madt_entry_header_t*)entry_ptr;
        
        switch (entry->type) {
            case ACPI_MADT_TYPE_LAPIC: {
                const acpi_madt_lapic_struct_t *lapic = (const acpi_madt_lapic_struct_t*)entry;
                lapic_count++;
                debuglog(DEBUG_INFO,"ACPI: Found Local APIC: Processor ID %d, APIC ID %d, Flags 0x%x\n",
                           lapic->processor_id, lapic->apic_id, lapic->flags);
                break;
            }
            
            case ACPI_MADT_TYPE_IOAPIC: {
                const acpi_madt_ioapic_struct_t *ioapic = (const acpi_madt_ioapic_struct_t*)entry;
                ioapic_count++;
                debuglog(DEBUG_INFO,"ACPI: Found I/O APIC: ID %d, Address 0x%x, GSI Base %d\n",
                           ioapic->ioapic_id, ioapic->ioapic_addr, ioapic->global_irq_base);
                break;
            }
            
            case ACPI_MADT_TYPE_IRQ_SRC_OVERRIDE: {
                const acpi_madt_irq_override_t *override = (const acpi_madt_irq_override_t*)entry;
                override_count++;
                debuglog(DEBUG_INFO,"ACPI: Found IRQ Override: Bus %d, Source %d -> GSI %d, Flags 0x%x\n",
                           override->bus, override->source, override->gsi, override->flags);
                break;
            }
        }
        
        entry_ptr += entry->length;
    }
    
    debuglog(DEBUG_INFO,"ACPI: MADT parsed: %d LAPICs, %d I/O APICs, %d overrides\n",
               lapic_count, ioapic_count, override_count);
    
    return 0;
}

/*
 * Validate ACPI tables
 */
int acpi_validate_tables(void)
{
    const acpi_table_info_t *current = g_table_list;
    uint8_t checksum;
    uint32_t valid_count = 0, total_count = 0;
    
    while (current) {
        total_count++;
        
        /* Calculate checksum */
        checksum = 0;
        const uint8_t *ptr = (const uint8_t*)current->header;
        for (uint32_t i = 0; i < current->length; i++) {
            checksum += ptr[i];
        }
        
        if (checksum == 0) {
            valid_count++;
        } else {
            debuglog(DEBUG_INFO,"ACPI: Checksum error in table %c%c%c%c\n",
                       current->signature[0], current->signature[1],
                       current->signature[2], current->signature[3]);
        }
        
        current = current->next;
    }
    
    debuglog(DEBUG_INFO,"ACPI: Table validation: %d/%d tables valid\n", valid_count, total_count);
    
    return (valid_count == total_count) ? 0 : -1;
}