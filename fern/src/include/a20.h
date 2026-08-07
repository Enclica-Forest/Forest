/**
 * @file a20.h
 * @brief A20 Line Enable Interface
 * 
 * The A20 address line enables access to memory above 1MB.
 * This module provides functions to enable and check the A20 line
 * using multiple fallback methods.
 */

#ifndef A20_H
#define A20_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief A20 operation result codes
 */
typedef enum {
    A20_SUCCESS = 0,        /**< A20 operation successful */
    A20_FAILED,             /**< A20 enable failed */
    A20_ALREADY_ENABLED,    /**< A20 was already enabled */
    A20_TIMEOUT,            /**< Operation timed out */
    A20_NOT_SUPPORTED       /**< Method not supported */
} a20_result_t;

/**
 * @brief Enable the A20 line
 * 
 * Tries multiple methods in order:
 * 1. Fast A20 Gate (Port 0x92)
 * 2. Keyboard Controller (8042 chip)
 * 3. Port 0xEE
 * 
 * @return A20_SUCCESS on success, error code on failure
 */
a20_result_t a20_enable(void);

/**
 * @brief Disable the A20 line
 * 
 * Rarely needed, but provided for completeness.
 * 
 * @return A20_SUCCESS on success
 */
a20_result_t a20_disable(void);

/**
 * @brief Check if A20 line is enabled
 * 
 * Tests memory wraparound to determine A20 status.
 * 
 * @return true if A20 is enabled, false otherwise
 */
bool a20_check(void);

/**
 * @brief Check if A20 is currently enabled (cached)
 * @return true if enabled, false otherwise
 */
bool a20_is_enabled(void);

/**
 * @brief Get A20 status as a string
 * @return "A20 Enabled" or "A20 Disabled"
 */
const char* a20_status_string(void);

#endif /* A20_H */
