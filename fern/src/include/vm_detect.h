#ifndef VM_DETECT_H
#define VM_DETECT_H

#include "types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default build gates. Safe defaults: detection is always on (cheap probe),
 * heavier guest integration / IOMMU / SMP-AP-bringup default off so baseline
 * builds do not perform risky I/O. Override with -DENABLE_*=1 on the command
 * line. */
#ifndef ENABLE_VM_DETECT
#  define ENABLE_VM_DETECT 1
#endif
#ifndef ENABLE_VM_GUEST_QEMU
#  define ENABLE_VM_GUEST_QEMU 1
#endif
#ifndef ENABLE_VM_GUEST_VBOX
#  define ENABLE_VM_GUEST_VBOX 1
#endif
#ifndef ENABLE_VM_GUEST_VMWARE
#  define ENABLE_VM_GUEST_VMWARE 1
#endif
#ifndef ENABLE_VM_GUEST_HYPERV
#  define ENABLE_VM_GUEST_HYPERV 1
#endif
#ifndef ENABLE_VIRTIO
#  define ENABLE_VIRTIO 1
#endif
#ifndef ENABLE_PCIE_ECAM
#  define ENABLE_PCIE_ECAM 1
#endif
#ifndef ENABLE_MSI_X
#  define ENABLE_MSI_X 1
#endif
#ifndef ENABLE_HPET
#  define ENABLE_HPET 1
#endif
#ifndef ENABLE_SMP_BRINGUP
#  define ENABLE_SMP_BRINGUP 0
#endif
#ifndef ENABLE_IOMMU
#  define ENABLE_IOMMU 0
#endif

typedef enum {
    VM_NONE        = 0,
    VM_QEMU        = 1,
    VM_KVM         = 2,
    VM_VIRTUALBOX  = 3,
    VM_VMWARE      = 4,
    VM_HYPERV      = 5,
    VM_XEN         = 6,
    VM_BARE_METAL  = 7
} vm_type_t;

/* Primary probe. Performs CPUID hypervisor leaf + DMI/SMBIOS/ACPI fallbacks.
 * Idempotent: subsequent calls are cached. */
vm_type_t vm_detect(void);

/* Human readable name ("Bare metal", "QEMU/TCG", "KVM", ...). */
const char* vm_name(vm_type_t t);

/* Convenience accessors backed by the cached result. */
int        vm_is_present(void);
vm_type_t  vm_get(void);

/* CPUID hypervisor leaf 0x40000000 features (EBX/ECX/EDX packed low/high) */
uint32_t   vm_hypervisor_features(void);

/* Vendor signature string surfaced via CPUID 0x40000000 (12 bytes + NUL). */
const char* vm_hypervisor_signature(void);

/* Access the cached detection again as a typed struct for debug sinks. */
typedef struct {
    vm_type_t type;
    char      signature[13];
    char      oem_id[7];
    char      oem_table_id[9];
    uint32_t  hypervisor_present : 1;
    uint32_t  detected_via_cpuid  : 1;
    uint32_t  detected_via_dmi    : 1;
    uint32_t  detected_via_acpi    : 1;
} vm_info_t;
const vm_info_t* vm_get_info(void);

#ifdef __cplusplus
}
#endif

#endif /* VM_DETECT_H */