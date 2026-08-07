/*
 * vm_detect.c - Hypervisor / virtual machine detection for Forest-OS
 *
 * Strategy (in order, cheapest first):
 *   1. CPUID leaf 0x40000000 hypervisor vendor signature (always present on a
 *      VM that follows the Intel/AMD virtualisation spec).
 *   2. CPUID leaf 1 ECX bit 31 (HYPERVISOR present flag).
 *   3. ACPI table OEM id / table id (matched against known VM vendors).
 *   4. DMI/SMBIOS OEM strings (0xF0000..0xFFFFF scan) as a last resort.
 *
 * Result is cached after the first vm_detect() call.
 */

#include "include/vm_detect.h"
#include "include/hardware.h"
#include "include/acpi.h"
#include "include/string.h"
#include "include/debuglog.h"

/* CPUID hypervisor vendor signatures (12 bytes, cmp as 3 uint32s). */
#define VM_SIG_KVM                " KVMKVMKVM  "
#define VM_SIG_QEMU_TCG           "TCGTCGTCGTCG"
#define VM_SIG_VMWARE             "VMwareVMware"
#define VM_SIG_VIRTUALBOX         "VBoxVBoxVBox"
#define VM_SIG_HYPERV             "Microsoft Hv"
#define VM_SIG_XEN                "XenVMMXenVMM"
#define VM_SIG_PARALLELS          " prl hyperv "
#define VM_SIG_BHYVE              "bhyve bhyve "

static vm_info_t g_vm_info;
static bool       g_vm_detected = false;

static void cpuid_hypervisor_leaf(cpuid_regs_t *regs) {
    /* leaf 0x40000000, subleaf 0 */
    __asm__ __volatile__(
        "cpuid"
        : "=a"(regs->eax), "=b"(regs->ebx), "=c"(regs->ecx), "=d"(regs->edx)
        : "a"(0x40000000u), "c"(0u));
}

static int sign_eq12(const char *a, const char *b) {
    for (int i = 0; i < 12; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0 && b[i] == 0) return 1;
    }
    return 1;
}

static vm_type_t classify_signature(const char *sig) {
    if (sign_eq12(sig, VM_SIG_KVM))         return VM_KVM;
    if (sign_eq12(sig, VM_SIG_QEMU_TCG))   return VM_QEMU;
    if (sign_eq12(sig, VM_SIG_VMWARE))     return VM_VMWARE;
    if (sign_eq12(sig, VM_SIG_VIRTUALBOX)) return VM_VIRTUALBOX;
    if (sign_eq12(sig, VM_SIG_HYPERV))     return VM_HYPERV;
    if (sign_eq12(sig, VM_SIG_XEN))        return VM_XEN;
    if (sign_eq12(sig, VM_SIG_PARALLELS))  return VM_NONE; /* not modelled yet */
    if (sign_eq12(sig, VM_SIG_BHYVE))      return VM_NONE; /* not modelled yet */
    return VM_NONE;
}

/* ACPI fallback: match against OEM strings of common VMs. */
static void probe_acpi_fallback(void) {
    const acpi_rsdp_t *rsdp = acpi_get_rsdp();
    if (!rsdp) return;
    /* RSDP OEM id is 6 bytes; table id is on each SDT. */
    if (memcmp(rsdp->v1.oem_id, "BOCHS ", 6) == 0 ||
        memcmp(rsdp->v1.oem_id, "BXPC  ", 6) == 0) {
        g_vm_info.type = VM_QEMU;
        g_vm_info.detected_via_acpi = 1;
        memcpy(g_vm_info.oem_id, rsdp->v1.oem_id, 6);
        g_vm_info.oem_id[6] = 0;
        return;
    }
    if (memcmp(rsdp->v1.oem_id, "VBOX  ", 6) == 0 ||
        memcmp(rsdp->v1.oem_id, "INNOTEK", 7) == 0) {
        g_vm_info.type = VM_VIRTUALBOX;
        g_vm_info.detected_via_acpi = 1;
        memcpy(g_vm_info.oem_id, rsdp->v1.oem_id, 6);
        g_vm_info.oem_id[6] = 0;
        return;
    }
    if (memcmp(rsdp->v1.oem_id, "VMWARE", 6) == 0) {
        g_vm_info.type = VM_VMWARE;
        g_vm_info.detected_via_acpi = 1;
        memcpy(g_vm_info.oem_id, rsdp->v1.oem_id, 6);
        g_vm_info.oem_id[6] = 0;
        return;
    }
    if (memcmp(rsdp->v1.oem_id, "MSFT  ", 6) == 0) {
        /* Microsoft/Hyper-V RSDP OEM id */
        g_vm_info.type = VM_HYPERV;
        g_vm_info.detected_via_acpi = 1;
        memcpy(g_vm_info.oem_id, rsdp->v1.oem_id, 6);
        g_vm_info.oem_id[6] = 0;
        return;
    }
}

/* DMI/SMBIOS string scan over the legacy 0xF0000..0xFFFFF BIOS area.
 * Identity-mapped in real mode / low regions so this is safe on bare metal
 * and decreases gracefully if the region is unmapped (we wrap each deref in
 * a characteristic-string compare only after copying a 64-byte block). */
static void probe_dmi_fallback(void) {
    /* Many VMs leave readable OEM strings in the F-segment. We do a very
     * small, defensive scan for a couple of canonical tokens. This is best
     * effort; if the region is unmapped the kernel will fault - which is why
     * this runs last and only when CPUID/ACPI failed to identify. */
#if 0  /* Disabled by default - kept for documentation; enable with care. */
    const char *p = (const char *)0xF0000;
    for (uint32 off = 0; off < 0x10000 - 32; off += 4, p++) {
        if (memcmp(p, "QEMU", 4) == 0)      { g_vm_info.type = VM_QEMU;       return; }
        if (memcmp(p, "VirtualBox", 10)==0) { g_vm_info.type = VM_VIRTUALBOX; return; }
        if (memcmp(p, "VMware", 6) == 0)    { g_vm_info.type = VM_VMWARE;     return; }
        if (memcmp(p, "Hyper-V", 7) == 0)    { g_vm_info.type = VM_HYPERV;     return; }
    }
#else
    (void)0;
#endif
}

vm_type_t vm_detect(void) {
    if (g_vm_detected) return g_vm_info.type;

    memset(&g_vm_info, 0, sizeof(g_vm_info));
    g_vm_info.type = VM_BARE_METAL;

    const cpuid_info_t *cpu = hardware_get_cpuid_info();
    if (cpu && cpu->cpuid_supported) {
        g_vm_info.hypervisor_present = cpu->hypervisor_present ? 1 : 0;
        if (cpu->max_basic_leaf >= 0x40000000u || cpu->hypervisor_present) {
            cpuid_regs_t r;
            cpuid_hypervisor_leaf(&r);
            if (r.eax != 0 || r.ebx != 0 || r.ecx != 0 || r.edx != 0) {
                ((uint32 *)g_vm_info.signature)[0] = r.ebx;
                ((uint32 *)g_vm_info.signature)[1] = r.edx;
                ((uint32 *)g_vm_info.signature)[2] = r.ecx;
                g_vm_info.signature[12] = 0;
                g_vm_info.detected_via_cpuid = 1;
                vm_type_t t = classify_signature(g_vm_info.signature);
                if (t != VM_NONE) g_vm_info.type = t;
            }
        }
    }

    if (g_vm_info.type == VM_BARE_METAL) probe_acpi_fallback();
    if (g_vm_info.type == VM_BARE_METAL) probe_dmi_fallback();

    g_vm_detected = true;
    debuglog(DEBUG_INFO,
             "VM: detected %s (sig='%s' cpuid=%u dmi=%u acpi=%u hvp=%u)\n",
             vm_name(g_vm_info.type),
             g_vm_info.signature,
             g_vm_info.detected_via_cpuid,
             g_vm_info.detected_via_dmi,
             g_vm_info.detected_via_acpi,
             g_vm_info.hypervisor_present);
    return g_vm_info.type;
}

const char* vm_name(vm_type_t t) {
    switch (t) {
        case VM_QEMU:       return "QEMU/TCG";
        case VM_KVM:        return "KVM";
        case VM_VIRTUALBOX: return "VirtualBox";
        case VM_VMWARE:     return "VMware";
        case VM_HYPERV:     return "Hyper-V";
        case VM_XEN:        return "Xen";
        case VM_NONE:       return "Unknown hypervisor";
        case VM_BARE_METAL:
        default:           return "Bare metal";
    }
}

int vm_is_present(void) {
    if (!g_vm_detected) vm_detect();
    return g_vm_info.type != VM_BARE_METAL && g_vm_info.type != VM_NONE ? 1 : 0;
}

vm_type_t vm_get(void) {
    if (!g_vm_detected) vm_detect();
    return g_vm_info.type;
}

uint32_t vm_hypervisor_features(void) {
    if (!g_vm_detected) vm_detect();
    if (!g_vm_info.hypervisor_present) return 0;
    /* Leaf 0x40000001 returns hypervisor-specific feature bits in EAX. */
    cpuid_regs_t r;
    __asm__ __volatile__(
        "cpuid"
        : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
        : "a"(0x40000001u), "c"(0u));
    return r.eax;
}

const char* vm_hypervisor_signature(void) {
    if (!g_vm_detected) vm_detect();
    return g_vm_info.signature;
}

const vm_info_t* vm_get_info(void) {
    if (!g_vm_detected) vm_detect();
    return &g_vm_info;
}