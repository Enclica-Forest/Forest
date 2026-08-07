#ifndef VM_TIMEKEEPING_H
#define VM_TIMEKEEPING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "time_enhanced.h"
#include "spinlock.h"

/* Virtualization-Aware Timekeeping for Fern
 * Optimizes time management for virtualized environments
 */

/* Hypervisor Types */
typedef enum {
    HYPERVISOR_UNKNOWN = 0,
    HYPERVISOR_VMWARE,
    HYPERVISOR_VIRTUALBOX,
    HYPERVISOR_KVM,
    HYPERVISOR_HYPER_V,
    HYPERVISOR_XEN,
    HYPERVISOR_QEMU,
    HYPERVISOR_BHYVE,
    HYPERVISOR_PARALLELS
} hypervisor_type_t;

/* VM Time Synchronization Methods */
typedef enum {
    VM_SYNC_METHOD_NONE = 0,
    VM_SYNC_METHOD_HYPERCALL,      /* Hypervisor time hypercall */
    VM_SYNC_METHOD_MMIO,          /* Memory-mapped time source */
    VM_SYNC_METHOD_PARAVIRT,       /* Paravirtualized time source */
    VM_SYNC_METHOD_TSC_ADJUST,    /* TSC adjustment registers */
    VM_SYNC_METHOD_HOST_CLOCK,     /* Direct host clock access */
    VM_SYNC_METHOD_NETWORK         /* Network time sync */
} vm_sync_method_t;

/* VirtualBox Timekeeping */
#define VBOX_TSC_SYNC_INTERVAL_MS  1000
#define VBOX_MAX_DRIFT_US         100000  /* 100ms max drift */

/* KVM Timekeeping */
#define KVM_HC_CLOCK_PAIRING       9
#define KVM_CLOCK_PAIRING_WALLCLOCK 0
#define KVM_TSC_SYNC_INTERVAL_MS   500
#define KVM_MAX_DRIFT_US          50000   /* 50ms max drift */

/* Hyper-V Timekeeping */
#define HV_REF_TSC                0x4000003
#define HV_TIME_REF_COUNT          0x40000020
#define HV_GUEST_IDLE            0x400000F
#define HV_MAX_TSC_ADJUST_US     100000  /* 100ms max adjust */

/* VM Time Information */
typedef struct {
    hypervisor_type_t hypervisor;
    char hypervisor_name[32];
    char version_string[64];
    vm_sync_method_t preferred_method;
    bool supports_host_time;
    bool supports_tsc_sync;
    bool supports_adjustment;
    uint64_t tsc_frequency;
    uint64_t host_time_frequency;
    uint32_t sync_interval_ms;
    uint32_t max_drift_us;
} vm_time_config_t;

/* VM Time Statistics */
typedef struct {
    uint64_t total_sync_count;
    uint64_t successful_syncs;
    uint64_t failed_syncs;
    uint64_t host_time_reads;
    uint64_t tsc_adjustments;
    int64_t total_drift_us;
    int64_t max_drift_us;
    int64_t last_drift_us;
    uint32_t average_sync_latency_us;
    uint32_t max_sync_latency_us;
} vm_time_stats_t;

/* Synchronization Context */
typedef struct {
    uint64_t last_host_time;
    uint64_t last_tsc_time;
    uint64_t last_sync_time;
    int64_t accumulated_drift;
    bool sync_in_progress;
    uint32_t sync_failures;
} vm_sync_context_t;

/* Hypercall Structures */
typedef struct {
    int64_t sec;
    int64_t nsec;
} kvm_clock_pairing_t;

typedef struct {
    uint32_t version;
    uint32_t sequence;
    uint64_t tsc_scale;
    int64_t tsc_offset;
    uint64_t time_ref_count;
    uint64_t tsc_ref_count;
} hv_reference_tsc_t;

typedef struct {
    uint64_t tsc;
    uint64_t scale;
    int64_t offset;
    bool valid;
} vm_time_adjustment_t;

/* VM Time Manager */
typedef struct vm_time_manager {
    vm_time_config_t config;
    vm_time_stats_t stats;
    vm_sync_context_t sync_ctx;
    vm_time_adjustment_t adjustment;
    high_res_timer_t sync_timer;
    bool initialized;
    bool time_stable;
    spinlock_t lock;
} vm_time_manager_t;

/* Core Functions */
int vm_timekeeper_init(void);
void vm_timekeeper_cleanup(void);
bool vm_timekeeper_is_available(void);

/* Hypervisor Detection */
hypervisor_type_t vm_detect_hypervisor(void);
int vm_get_hypervisor_info(vm_time_config_t *config);
bool vm_is_running_in_vm(void);

/* Time Synchronization */
int vm_sync_with_host(void);
int vm_sync_tsc_with_host(void);
int vm_adjust_time(int64_t adjustment_us);
int vm_calibrate_tsc_frequency(void);

/* Time Reading Functions */
int vm_read_host_time(uint64_t *host_time_ns);
int vm_read_paravirtual_time(uint64_t *pv_time_ns);
int vm_read_adjusted_tsc_time(uint64_t *tsc_time_ns);
int vm_get_best_time_source(time_source_t *source, uint32_t *quality);

/* Hypercall Functions */
int vm_kvm_clock_pairing(uint64_t *wallclock_ns);
int vm_hyper_v_read_reference_tsc(hv_reference_tsc_t *ref_tsc);
int vm_virtualbox_get_host_time(uint64_t *host_time_ns);

/* TSC Management */
int vm_enable_tsc_adjustment(void);
int vm_disable_tsc_adjustment(void);
int vm_set_tsc_scaling(uint64_t numerator, uint64_t denominator);
int vm_measure_tsc_drift(int64_t *drift_us);

/* Paravirtual Time Support */
int vm_setup_paravirtual_clock(void);
int vm_register_paravirtual_timer(void);
int vm_handle_paravirtual_timer_interrupt(void);

/* Power Management Integration */
int vm_handle_suspend(void);
int vm_handle_resume(void);
int vm_handle_migration(void);
int vm_adjust_for_suspension(void);

/* Migration Support */
int vm_save_time_state(void *buffer, size_t *size);
int vm_restore_time_state(const void *buffer, size_t size);
int vm_handle_live_migration(void);

/* Configuration Functions */
int vm_set_sync_method(vm_sync_method_t method);
int vm_configure_sync_interval(uint32_t interval_ms);
int vm_set_drift_threshold(int64_t threshold_us);
int vm_get_time_config(vm_time_config_t *config);

/* Statistics and Monitoring */
int vm_get_time_statistics(vm_time_stats_t *stats);
void vm_reset_statistics(void);
void vm_dump_time_statistics(void);
int vm_validate_time_accuracy(void);

/* Debug and Diagnostics */
int vm_run_time_sync_test(void);
void vm_dump_hypervisor_info(void);
int vm_calculate_time_quality(uint32_t *quality_percent);

/* Integration with Enhanced Time System */
int vm_integrate_with_time_system(void);
int vm_override_time_source(time_source_t source);
int vm_provide_time_calibration(void);

/* Utility Functions */
uint64_t vm_calculate_tsc_ns(uint64_t tsc, uint64_t frequency);
int64_t vm_calculate_drift_ns(uint64_t local_time, uint64_t host_time);
bool vm_is_time_stable(void);
int vm_estimate_sync_latency(uint32_t *latency_us);

#endif /* VM_TIMEKEEPING_H */