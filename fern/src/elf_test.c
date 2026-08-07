#include "include/elf.h"

// Function declarations for ELF test
void print(const char *str);
void print_hex(uint32 value);

// Simple ELF test without debuglog dependencies
void elf_loader_test(void) {
    const char *test_msg = "ELF loader test: ";
    
    // Test basic ELF validation using our actual userspace hello.elf
    extern uint8_t userspace_elf_data[];
    extern size_t userspace_elf_size;
    
    if (userspace_elf_size > 0) {
        print(test_msg);
        print("Testing userspace ELF validation...\n");
        
        if (elf_is_valid(userspace_elf_data, userspace_elf_size)) {
            print("PASS: ELF validation passed\n");
        } else {
            print("FAIL: ELF validation failed\n");
        }
        
        uint32 entry = elf_get_entry_point(userspace_elf_data);
        print("Entry point: 0x");
        print_hex(entry);
        print("\n");
    } else {
        print(test_msg);
        print("No userspace ELF to test (expected during kernel boot)\n");
    }
}

// Reference to userspace hello.elf data
uint8_t userspace_elf_data[] = {
    // This would be populated by the kernel at runtime
    // For now, we'll just make it point to a valid minimal ELF
    0x7F, 'E', 'L', 'F', // ELF magic
    0x01, 0x01,0x01,0x00, // Class 32, Data LSB, Version 1
    0x02,0x00,0x03,0x00, // Type = EXEC, Machine = i386
    0x54,0x00,0x00,0x00, // Version, Entry point = 0x40001000
    0x20,0x00,0x00,0x00, // Program header offset
    0x00,0x00,0x00,0x00, // Section header offset
    0x00,0x00,0x00,0x00, // Flags
    0x34,0x00,0x20,0x00, // ELF header size, Program header size
    0x02,0x00,0x00,0x00, // Program headers = 2, Section header size = 0
    
    // Program header table
    0x01,0x00,0x00,0x00, // Type = PT_LOAD, Offset = 0x10000
    0x00,0x00,0x00,0x00, // Virtual address = 0x40001000
    0x00,0x00,0x00,0x00, // Physical address = 0x40001000
    0x74,0x00,0x00,0x00, // File size = 116 bytes, Memory size = 116 bytes
    0x07,0x00,0x00,0x00, // Flags = Read+Write+Execute
    0x00,0x00,0x10,0x00, // Alignment
    
    // Second segment
    0x01,0x00,0x00,0x00, // Type = PT_LOAD, Offset = 0x10074
    0x00,0x00,0x00,0x00, // Virtual address = 0x40010074
    0x00,0x00,0x00,0x00, // Physical address = 0x40010074
    0x00,0x10,0x00,0x00, // File size = 16 bytes, Memory size = 16 bytes
    0x06,0x00,0x00,0x00, // Flags = Read+Write
    0x00,0x00,0x10,0x00, // Alignment
};

size_t userspace_elf_size = sizeof(userspace_elf_data);