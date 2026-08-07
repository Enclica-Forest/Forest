#ifndef VM_GUEST_H
#define VM_GUEST_H

#include "types.h"
#include "vm_detect.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Top-level guest integration dispatcher: dispatches to the per-VM init
 * routine based on vm_detect(). Returns 0 if no integration is available,
 * positive on success, negative on error. */
int  vm_guest_init(void);

/* QEMU fw_cfg interface (port 0x510 selector, 0x511 data). */
int  vm_qemu_fw_cfg_read(uint16 selector, void *buf, uint32 len);
uint32 vm_qemu_fw_cfg_read_uint32(uint16 selector);

/* QEMU debug-exit / isa-debug-console (port 0xE9) as an early printk sink. */
void vm_qemu_debug_putc(char c);
void vm_qemu_debug_puts(const char *s);

/* VirtualBox: thin wrappers around the existing VMMDev guest channel.
 * Implemented in src/virtualbox_guest.c. */
int  vbox_guest_init(void);                            /* from virtualbox_guest.h */
int  vbox_clipboard_send(const void *buf, uint32 len);
int  vbox_clipboard_recv(void *buf, uint32 maxlen, uint32 *out_len);
int  vbox_mouse_set(int32 x, int32 y, int absolute);

/* VMware backdoor (port 0x5658 + 0x5659). */
int  vmware_guest_init(void);
uint32 vmware_get_version(void);
int  vmware_mouse_set(int32 x, int32 y);
int  vmware_clipboard_send(const char *text);

/* Hyper-V synthetic MSRs and hypercall support. */
int  hyperv_guest_init(void);
uint64 hyperv_get_features(void);

#ifdef __cplusplus
}
#endif

#endif /* VM_GUEST_H */