/**
 * Forest-OS sysfs Filesystem Header
 * Provides kernel object information to userspace applications
 */

#ifndef SYSFS_H
#define SYSFS_H

#include <stdint.h>
#include <stdbool.h>
#include "vfs.h"

// Initialize the sysfs filesystem
bool sysfs_init(void);

// Get the sysfs root node
vfs_node_t* sysfs_get_root(void);

#endif // SYSFS_H
