#ifndef ARCH_AHCI_H
#define ARCH_AHCI_H

#include <stdint.h>
#include <stdbool.h>

#include "arch/arch.h"
#include "../include/ahci.h"

/**
 * ahci_read - Read sectors from an AHCI drive.
 *
 * @drive: Port number (0-based index into detected drives).
 * @lba:   Starting Logical Block Address.
 * @count: Number of 512-byte sectors to read.
 * @buf:   Destination buffer (must be at least count * 512 bytes).
 *
 * Returns number of bytes read on success, -1 on error.
 */
static inline int ahci_read(uint8_t drive, uint32_t lba, uint32_t count, uint8_t *buf) {
    ahci_port_t *port = ahci_get_port(drive);
    if (!port) {
        return -1;
    }
    if (!port->initialized) {
        ahci_port_start(port);
    }
    return ahci_read_sectors(port, lba, count, buf);
}

/**
 * ahci_write - Write sectors to an AHCI drive.
 *
 * @drive: Port number (0-based index into detected drives).
 * @lba:   Starting Logical Block Address.
 * @count: Number of 512-byte sectors to write.
 * @buf:   Source buffer (must be at least count * 512 bytes).
 *
 * Returns number of bytes written on success, -1 on error.
 */
static inline int ahci_write(uint8_t drive, uint32_t lba, uint32_t count, const uint8_t *buf) {
    ahci_port_t *port = ahci_get_port(drive);
    if (!port) {
        return -1;
    }
    if (!port->initialized) {
        ahci_port_start(port);
    }
    return ahci_write_sectors(port, lba, count, buf);
}

/**
 * ahci_detect_drives - Detect attached SATA drives.
 *
 * Probes all implemented ports and identifies connected devices.
 * Returns the number of drives detected.
 */
static inline uint32_t ahci_detect_drives(void) {
    return g_ahci_controller.port_count;
}

#endif /* ARCH_AHCI_H */
