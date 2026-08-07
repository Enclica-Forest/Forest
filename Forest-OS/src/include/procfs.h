/**
 * Forest-OS /proc Filesystem Header
 * Provides process and system information to userspace applications
 */

#ifndef PROCFS_H
#define PROCFS_H

#include "types.h"
#include "stdbool.h"
#include "vfs.h"

// Initialize the procfs filesystem
bool procfs_init(void);

// Get the procfs root node
vfs_node_t* procfs_get_root(void);

// Add/remove process entries
void procfs_add_process(uint32 pid);
void procfs_remove_process(uint32 pid);

#endif // PROCFS_H
