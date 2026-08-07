/*
 * vm_guest.c - Hypervisor-specific guest integration glue.
 *
 * Each backend is gated by ENABLE_VM_GUEST_* and is intentionally safe: if
 * the host does not implement a backdoor we simply return an error rather
 * than touching I/O that may be assigned to a real device on bare metal.
 */

#include "include/vm_guest.h"
#include "include/vm_detect.h"
#include "include/hardware.h"
#include "include/apic.h"
#include "include/system.h"
#include "include/string.h"
#include "include/debuglog.h"

/* ---------- QEMU fw_cfg (port 0x510 / 0x511) -------------------- */

#define QEMU_FW_CFG_SELECTOR   0x510
#define QEMU_FW_CFG_DATA        0x511
#define QEMU_FW_CFG_DMA         0x514   /* modern DMA interface (optional) */

#define FW_CFG_SIGNATURE        0x00
#define FW_CFG_ID               0x01
#define FW_CFG_FILE_DIR         0x19

static inline void fw_cfg_select(uint16 sel) {
    outportw(QEMU_FW_CFG_SELECTOR, sel);
}

int vm_qemu_fw_cfg_read(uint16 selector, void *buf, uint32 len) {
#if ENABLE_VM_GUEST_QEMU
    if (!buf || len == 0) return -1;
    if (vm_get() == VM_BARE_METAL) return -1;
    fw_cfg_select(selector);
    uint8 *p = (uint8 *)buf;
    for (uint32 i = 0; i < len; i++)
        p[i] = inportb(QEMU_FW_CFG_DATA);
    return 0;
#else
    (void)selector; (void)buf; (void)len;
    return -1;
#endif
}

uint32 vm_qemu_fw_cfg_read_uint32(uint16 selector) {
#if ENABLE_VM_GUEST_QEMU
    uint8 b[4] = {0};
    if (vm_qemu_fw_cfg_read(selector, b, 4) != 0) return 0;
    return ((uint32)b[0]       | ((uint32)b[1] << 8)
          | ((uint32)b[2] << 16) | ((uint32)b[3] << 24));
#else
    (void)selector;
    return 0;
#endif
}

/* ---------- QEMU isa-debugcon (port 0xE9) ------------------------ */

void vm_qemu_debug_putc(char c) {
#if ENABLE_VM_GUEST_QEMU
    if (vm_get() == VM_BARE_METAL) return;
    outportb(0xE9, (uint8)c);
#else
    (void)c;
#endif
}

void vm_qemu_debug_puts(const char *s) {
#if ENABLE_VM_GUEST_QEMU
    if (!s) return;
    while (*s) vm_qemu_debug_putc(*s++);
#else
    (void)s;
#endif
}

/* ---------- VMware backdoor (port 0x5658 / 0x5659) -------------- */

#define VMWARE_PORT             0x5658
#define VMWARE_PORT_HIGH        0x5659
#define VMWARE_MAGIC            0x564D5868u  /* "VMXh" */
#define VMWARE_CMD_GETVERSION   10

static uint32 vmware_backdoor(uint32 cmd, uint32 *ebx, uint32 *ecx) {
#if ENABLE_VM_GUEST_VMWARE
    uint32 eax = VMWARE_MAGIC;
    uint32 d = VMWARE_PORT;
    __asm__ __volatile__(
        "in %%dx, %%eax"
        : "+a"(eax), "+b"(*ebx), "+c"(*ecx), "+d"(d)
        :
        : "memory");
    (void)cmd;
    return eax;
#else
    (void)cmd; (void)ebx; (void)ecx;
    return 0;
#endif
}

int vmware_guest_init(void) {
#if ENABLE_VM_GUEST_VMWARE
    if (vm_get() != VM_VMWARE) return -1;
    uint32 ebx = 0, ecx = VMWARE_CMD_GETVERSION;
    uint32 eax = vmware_backdoor(VMWARE_CMD_GETVERSION, &ebx, &ecx);
    debuglog(DEBUG_INFO, "VMWARE: guest init (version %#x, magic %#x)\n", eax, ebx);
    return (ebx == VMWARE_MAGIC) ? 0 : -1;
#else
    return -1;
#endif
}

uint32 vmware_get_version(void) {
#if ENABLE_VM_GUEST_VMWARE
    uint32 ebx = 0, ecx = VMWARE_CMD_GETVERSION;
    vmware_backdoor(VMWARE_CMD_GETVERSION, &ebx, &ecx);
    return ecx;
#else
    return 0;
#endif
}

int vmware_mouse_set(int32 x, int32 y) {
#if ENABLE_VM_GUEST_VMWARE
    /* CMD_ABSPOINTER 28..; a single absolute pointer move is subcommand 6. */
    uint32 ebx = 0, ecx = 28;      /* ABSPOINTER_CMD */
    vmware_backdoor(0, &ebx, &ecx);
    uint32 ebx2 = (uint32)x, ecx2 = 6; /* MOVE */
    vmware_backdoor(0, &ebx2, &ecx2);
    ebx2 = (uint32)y; ecx2 = (uint32)y;
    vmware_backdoor(0, &ebx2, &ecx2);
    return 0;
#else
    (void)x; (void)y;
    return -1;
#endif
}

int vmware_clipboard_send(const char *text) {
#if ENABLE_VM_GUEST_VMWARE
    /* VMBLACKET override stub: Full VMware tools RPCI requires a reliable
     * backdoor protocol; expose only the entry point here. Actual transfer
     * deferred to a userspace driver. */
    (void)text;
    return 0;
#else
    (void)text;
    return -1;
#endif
}

/* ---------- Hyper-V synthetic MSRs / hypercall ----------------- */

/* Hyper-V MSR indices */
#define HV_X64_MSR_GUEST_OS_ID       0x40000000
#define HV_X64_MSR_HYPERCALL         0x40000001
#define HV_X64_MSR_VP_INDEX          0x40000002
#define HV_X64_MSR_TIME_REF_COUNT    0x40000020

int hyperv_guest_init(void) {
#if ENABLE_VM_GUEST_HYPERV
    if (vm_get() != VM_HYPERV) return -1;
    /* Write a non-zero guest OS id to enable the hypercall MSR. */
    uint64_t guest_id = 0x4F525354ULL; /* 'ORST' - arbitrary non-zero OS id */
    write_msr(HV_X64_MSR_GUEST_OS_ID, guest_id);
    uint64_t hc = read_msr(HV_X64_MSR_HYPERCALL);
    if (hc == 0) {
        debuglog(DEBUG_WARN, "HYPERV: hypercall MSR not enabled by host\n");
        return -1;
    }
    /* Enable hypercall code by setting bit 0 with the GPFN of the hypercall
     * page. Without guest memory layout knowledge we leave the page unmapped
     * here; the host's MSR value will already be set on Gen-2 VMs. */
    debuglog(DEBUG_INFO, "HYPERV: guest init ok (hypercall=0x%llx)\n",
             (unsigned long long)hc);
    return 0;
#else
    return -1;
#endif
}

uint64 hyperv_get_features(void) {
#if ENABLE_VM_GUEST_HYPERV
    /* CPUID 0x40000003 returns partition privilege flags in EAX/EBX/ECX/EDX. */
    cpuid_regs_t r = {0,0,0,0};
    __asm__ __volatile__(
        "cpuid"
        : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
        : "a"(0x40000003u), "c"(0u));
    return ((uint64)r.eax | ((uint64)r.ebx << 32));  /* low 64 privilege bits */
#else
    return 0;
#endif
}

/* ---------- Top-level dispatcher ------------------------------- */

int vm_guest_init(void) {
    vm_type_t t = vm_get();
    switch (t) {
        case VM_QEMU:
#if ENABLE_VM_GUEST_QEMU
            /* Probe fw_cfg signature "QEMU". */
            uint32 sig = vm_qemu_fw_cfg_read_uint32(FW_CFG_SIGNATURE);
            debuglog(DEBUG_INFO, "QEMU: fw_cfg signature 0x%08x\n", sig);
#endif
            return 0;
        case VM_KVM:
            return 0; /* KVM uses virtio; nothing special here */
        case VM_VIRTUALBOX:
#if ENABLE_VM_GUEST_VBOX
            return vbox_guest_init();
#else
            return -1;
#endif
        case VM_VMWARE:
            return vmware_guest_init();
        case VM_HYPERV:
            return hyperv_guest_init();
        default:
            return 0;
    }
}