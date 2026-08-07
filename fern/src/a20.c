/**
 * @file a20.c
 * @brief A20 Line Enable Implementation
 * 
 * The A20 address line must be enabled to access memory above 1MB.
 * This module implements multiple methods to enable the A20 line with
 * proper fallback mechanisms as per OSDev wiki recommendations.
 * 
 * Methods (in order of preference):
 * 1. BIOS INT 15h (safest, not available after real mode)
 * 2. Fast A20 Gate (Port 0x92)
 * 3. Keyboard Controller (8042 chip)
 * 4. Port 0xEE (some systems)
 */

#include "include/a20.h"
#include "include/types.h"
#include "include/screen.h"
#include "include/io.h"

// A20 status
static bool a20_enabled = false;

// Keyboard controller ports
#define KBC_DATA_PORT       0x60
#define KBC_STATUS_PORT     0x64
#define KBC_COMMAND_PORT    0x64

// Keyboard controller commands
#define KBC_CMD_READ_OUTPUT     0xD0
#define KBC_CMD_WRITE_OUTPUT    0xD1
#define KBC_CMD_DISABLE_KBD     0xAD
#define KBC_CMD_ENABLE_KBD      0xAE

// Status register bits
#define KBC_STATUS_OUTPUT_FULL  0x01
#define KBC_STATUS_INPUT_FULL   0x02

// Fast A20 gate port
#define FAST_A20_PORT       0x92
#define FAST_A20_ENABLE     0x02
#define FAST_A20_RESET      0x01

// Port 0xEE A20 gate
#define PORT_EE_A20         0xEE

// Test addresses for A20 check
#define A20_TEST_LOW        0x000500
#define A20_TEST_HIGH       0x100500

/**
 * @brief Wait for keyboard controller input buffer to be empty
 * @return true if successful, false if timeout
 */
static bool kbc_wait_input(void) {
    uint32_t timeout = 100000;
    while (timeout--) {
        if ((inb(KBC_STATUS_PORT) & KBC_STATUS_INPUT_FULL) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Wait for keyboard controller output buffer to be full
 * @return true if successful, false if timeout
 */
static bool kbc_wait_output(void) {
    uint32_t timeout = 100000;
    while (timeout--) {
        if (inb(KBC_STATUS_PORT) & KBC_STATUS_OUTPUT_FULL) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Check if A20 line is enabled
 * 
 * Tests A20 by comparing values at addresses that would be identical
 * if A20 is disabled (memory wraparound).
 * 
 * @return true if A20 is enabled, false otherwise
 */
bool a20_check(void) {
    // Save original values
    volatile uint8_t* low = (volatile uint8_t*)A20_TEST_LOW;
    volatile uint8_t* high = (volatile uint8_t*)A20_TEST_HIGH;
    
    uint8_t original_low = *low;
    uint8_t original_high = *high;
    
    // Write different values to test addresses
    *low = 0x00;
    *high = 0xFF;
    
    // If A20 is disabled, writing to high will overwrite low due to wraparound
    bool a20_status = (*low != 0xFF);
    
    // Restore original values
    *low = original_low;
    *high = original_high;
    
    return a20_status;
}

/**
 * @brief Enable A20 via keyboard controller (8042 method)
 * 
 * This is the traditional method using the keyboard controller.
 * It's slow but works on most systems.
 * 
 * @return true if successful, false otherwise
 */
static bool a20_enable_keyboard_controller(void) {
    // Disable keyboard
    if (!kbc_wait_input()) return false;
    outb(KBC_COMMAND_PORT, KBC_CMD_DISABLE_KBD);
    
    // Read output port
    if (!kbc_wait_input()) return false;
    outb(KBC_COMMAND_PORT, KBC_CMD_READ_OUTPUT);
    
    if (!kbc_wait_output()) return false;
    uint8_t output = inb(KBC_DATA_PORT);
    
    // Write output port with A20 bit set
    if (!kbc_wait_input()) return false;
    outb(KBC_COMMAND_PORT, KBC_CMD_WRITE_OUTPUT);
    
    if (!kbc_wait_input()) return false;
    outb(KBC_DATA_PORT, output | 0x02);
    
    // Re-enable keyboard
    if (!kbc_wait_input()) return false;
    outb(KBC_COMMAND_PORT, KBC_CMD_ENABLE_KBD);
    
    // Wait for A20 to take effect
    if (!kbc_wait_input()) return false;
    
    return true;
}

/**
 * @brief Enable A20 via Fast A20 Gate (Port 0x92)
 * 
 * This is the fastest method, available on PS/2 and later systems.
 * Some systems may not support this or it may have side effects.
 * 
 * @return true if A20 was toggled, false otherwise
 */
static bool a20_enable_fast_gate(void) {
    uint8_t value = inb(FAST_A20_PORT);
    
    // Check if already enabled
    if (value & FAST_A20_ENABLE) {
        return true;  // Already enabled via this method
    }
    
    // Enable A20, but make sure we don't accidentally reset the system
    // (bit 0 is fast reset on some systems)
    value |= FAST_A20_ENABLE;
    value &= ~FAST_A20_RESET;
    
    outb(FAST_A20_PORT, value);
    
    return true;
}

/**
 * @brief Enable A20 via Port 0xEE
 * 
 * Some systems support A20 enable/disable via port 0xEE.
 * Reading enables, writing disables.
 * 
 * @return true (always, as this method doesn't have feedback)
 */
static bool a20_enable_port_ee(void) {
    (void)inb(PORT_EE_A20);  // Reading enables A20
    return true;
}

/**
 * @brief Main A20 enable function with multiple fallback methods
 * 
 * Tries multiple methods in order of safety/preference:
 * 1. Check if already enabled
 * 2. Try Fast A20 Gate
 * 3. Try Keyboard Controller
 * 4. Try Port 0xEE
 * 
 * @return A20_SUCCESS on success, error code on failure
 */
a20_result_t a20_enable(void) {
    // Check if already enabled
    if (a20_check()) {
        a20_enabled = true;
        print("[A20] Already enabled\n");
        return A20_SUCCESS;
    }
    
    print("[A20] Enabling A20 line...\n");
    
    // Try Fast A20 Gate first (fastest and usually safest)
    print("[A20] Trying Fast A20 Gate (Port 0x92)...\n");
    a20_enable_fast_gate();
    
    // Small delay to let it take effect
    for (volatile int i = 0; i < 10000; i++);
    
    if (a20_check()) {
        a20_enabled = true;
        print("[A20] Enabled via Fast A20 Gate\n");
        return A20_SUCCESS;
    }
    
    // Try Keyboard Controller method
    print("[A20] Trying Keyboard Controller method...\n");
    if (a20_enable_keyboard_controller()) {
        // Wait with timeout
        for (int i = 0; i < 100; i++) {
            for (volatile int j = 0; j < 10000; j++);
            if (a20_check()) {
                a20_enabled = true;
                print("[A20] Enabled via Keyboard Controller\n");
                return A20_SUCCESS;
            }
        }
    }
    
    // Try Port 0xEE as last resort
    print("[A20] Trying Port 0xEE method...\n");
    a20_enable_port_ee();
    
    for (volatile int i = 0; i < 10000; i++);
    
    if (a20_check()) {
        a20_enabled = true;
        print("[A20] Enabled via Port 0xEE\n");
        return A20_SUCCESS;
    }
    
    // All methods failed
    print("[A20] ERROR: Failed to enable A20 line!\n");
    return A20_FAILED;
}

/**
 * @brief Disable A20 line (rarely needed)
 * 
 * @return A20_SUCCESS on success
 */
a20_result_t a20_disable(void) {
    // Use Fast A20 Gate to disable
    uint8_t value = inb(FAST_A20_PORT);
    value &= ~FAST_A20_ENABLE;
    value &= ~FAST_A20_RESET;
    outb(FAST_A20_PORT, value);
    
    a20_enabled = false;
    return A20_SUCCESS;
}

/**
 * @brief Check if A20 is currently enabled
 * @return true if enabled, false otherwise
 */
bool a20_is_enabled(void) {
    return a20_enabled || a20_check();
}

/**
 * @brief Get A20 status string
 * @return Status string
 */
const char* a20_status_string(void) {
    if (a20_is_enabled()) {
        return "A20 Enabled";
    } else {
        return "A20 Disabled";
    }
}
