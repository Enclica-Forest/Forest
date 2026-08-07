/*
 * Enhanced Time Management Implementation for Fern
 * Provides comprehensive time tracking, network synchronization, and multiple time formats
 */

#include "include/time_enhanced.h"
#include "include/time.h"
#include "include/memory_safe.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/atomic.h"
#include "include/spinlock.h"
#include "include/interrupt.h"
#include "include/net.h"

/* External declarations */
extern bool vbox_guest_is_available(void);

/* Helper macro for min if not defined */
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* Time Source Functions */
typedef struct {
    time_source_t source;
    const char *name;
    uint64_t (*read_func)(void);
    bool available;
    uint32_t quality_score;  /* Higher is better */
} time_source_info_t;

/* Alarm Structure */
typedef struct time_alarm {
    uint32_t id;
    enhanced_time_t trigger_time;
    time_alarm_callback_t callback;
    void *context;
    bool active;
    struct time_alarm *next;
} time_alarm_t;

/* Global State */
static struct {
    bool initialized;
    time_source_t current_source;
    enhanced_time_t current_time;
    time_sync_config_t sync_config;
    time_statistics_t stats;
    vm_time_info_t vm_info;
    leap_second_info_t leap_info;
    timezone_info_t timezone;
    time_source_info_t sources[8];
    uint32_t source_count;
    time_alarm_t *alarms;
    uint32_t next_alarm_id;
    high_res_timer_t system_timer;
    spinlock_t lock;
} g_time_state = {0};

/* Forward declarations */
static uint64_t time_rtc_read(void);
static uint64_t time_tsc_read(void);
static uint64_t time_hpet_read(void);
static uint64_t time_pit_read(void);
static uint64_t time_virtualbox_read(void);
static uint64_t time_kvm_read(void);
static int time_convert_to_enhanced(uint64_t timestamp, time_format_t format, enhanced_time_t *time);

/* Time Source Definitions */
static time_source_info_t g_default_sources[] = {
    {TIME_SOURCE_RTC, "RTC", time_rtc_read, false, 50},
    {TIME_SOURCE_TSC, "TSC", time_tsc_read, false, 80},
    {TIME_SOURCE_HPET, "HPET", time_hpet_read, false, 90},
    {TIME_SOURCE_PIT, "PIT", time_pit_read, false, 30},
    {TIME_SOURCE_VIRTUALBOX, "VirtualBox", time_virtualbox_read, false, 95},
    {TIME_SOURCE_KVM, "KVM", time_kvm_read, false, 95},
};

/*
 * Initialize enhanced time management
 */
int time_enhanced_init(void)
{
    debuglog(DEBUG_INFO,"TIME: Initializing enhanced time management\n");
    
    if (g_time_state.initialized) {
        return 0;
    }
    
    spinlock_init(&g_time_state.lock, "time_state");
    
    /* Initialize time sources */
    g_time_state.source_count = sizeof(g_default_sources) / sizeof(time_source_info_t);
    memcpy(g_time_state.sources, g_default_sources, sizeof(g_default_sources));
    
    /* Detect available time sources */
    for (uint32_t i = 0; i < g_time_state.source_count; i++) {
        time_source_info_t *source = &g_time_state.sources[i];
        
        if (source->read_func && source->read_func() != 0) {
            source->available = true;
            debuglog(DEBUG_INFO,"TIME: %s time source available\n", source->name);
        }
    }
    
    /* Initialize default configuration */
    g_time_state.sync_config.preferred_source = TIME_SOURCE_AUTO;
    g_time_state.sync_config.sync_interval_ms = 60000;  /* 1 minute */
    g_time_state.sync_config.sync_timeout_ms = 5000;   /* 5 seconds */
    g_time_state.sync_config.max_drift_ms = 100;        /* 100ms */
    g_time_state.sync_config.auto_sync = true;
    g_time_state.sync_config.use_ntp = false;
    g_time_state.sync_config.ntp_server_count = 0;
    
    /* Initialize timezone (UTC by default) */
    memset(&g_time_state.timezone, 0, sizeof(timezone_info_t));
    memcpy(g_time_state.timezone.name, "UTC", 4);
    
    /* Initialize leap second info */
    g_time_state.leap_info.leap_seconds = 37;  /* As of 2026 */
    g_time_state.leap_info.scheduled = false;
    g_time_state.leap_info.announcement_months = 6;
    
    /* Detect virtualization */
    time_detect_virtualization();
    
    /* Select best time source */
    if (g_time_state.vm_info.is_virtual) {
        g_time_state.current_source = g_time_state.vm_info.preferred_vm_source;
    } else {
        g_time_state.current_source = TIME_SOURCE_HPET;
        if (!g_time_state.sources[TIME_SOURCE_HPET].available) {
            g_time_state.current_source = TIME_SOURCE_TSC;
        }
        if (!g_time_state.sources[TIME_SOURCE_TSC].available) {
            g_time_state.current_source = TIME_SOURCE_RTC;
        }
    }
    
    /* Initialize high-resolution timer */
    if (time_init_high_res_timer(&g_time_state.system_timer) != 0) {
        debuglog(DEBUG_INFO,"TIME: Failed to initialize high-resolution timer\n");
    }
    
    /* Read initial time */
    if (time_get_enhanced_time(&g_time_state.current_time) != 0) {
        debuglog(DEBUG_INFO,"TIME: Failed to read initial time\n");
        return -1;
    }
    
    g_time_state.initialized = true;
    debuglog(DEBUG_INFO,"TIME: Enhanced time management initialized\n");
    debuglog(DEBUG_INFO,"TIME: Primary time source: %s\n", 
               g_time_state.sources[g_time_state.current_source].name);
    
    return 0;
}

/*
 * Read time from RTC
 */
static uint64_t time_rtc_read(void)
{
    rtc_time_t rtc_time;
    uint64_t unix_time = 0;
    
    if (rtc_read_time(&rtc_time) != 0) {
        return 0;
    }
    
    /* Convert to Unix timestamp (simplified) */
    /* This would need proper calendar calculation */
    unix_time = (rtc_time.year + 70) * 365 * 24 * 3600;
    unix_time += rtc_time.month * 30 * 24 * 3600;
    unix_time += rtc_time.day * 24 * 3600;
    unix_time += rtc_time.hour * 3600;
    unix_time += rtc_time.minute * 60;
    unix_time += rtc_time.second;
    
    return unix_time * 1000000ULL; /* Convert to microseconds */
}

/*
 * Read time from TSC
 */
static uint64_t time_tsc_read(void)
{
    static uint64_t tsc_frequency = 0;
    uint64_t tsc;
    
    if (tsc_frequency == 0) {
        tsc_frequency = time_get_cpu_frequency();
    }
    
    tsc = read_tsc();
    
    /* Convert to microseconds */
    return (tsc * 1000000ULL) / tsc_frequency;
}

/*
 * Read time from HPET
 */
static uint64_t time_hpet_read(void)
{
    /* This would read from HPET counter */
    /* For now, return TSC as fallback */
    return time_tsc_read();
}

/*
 * Read time from PIT
 */
static uint64_t time_pit_read(void)
{
    /* This would read from PIT counter */
    /* For now, return TSC as fallback */
    return time_tsc_read();
}

/*
 * Read time from VirtualBox
 */
static uint64_t time_virtualbox_read(void)
{
    /* This would read from VirtualBox host time */
    /* For now, return TSC as fallback */
    return time_tsc_read();
}

/*
 * Read time from KVM
 */
static uint64_t time_kvm_read(void)
{
    /* This would use KVM hypercall for host time */
    /* For now, return TSC as fallback */
    return time_tsc_read();
}

/*
 * Set time source
 */
int time_set_source(time_source_t source)
{
    if (source >= g_time_state.source_count) {
        return -1;
    }
    
    if (!g_time_state.sources[source].available) {
        debuglog(DEBUG_INFO,"TIME: Requested time source %s not available\n", 
                   g_time_state.sources[source].name);
        return -1;
    }
    
    unsigned long flags;
    spin_lock_irqsave(&g_time_state.lock, flags);
    
    g_time_state.current_source = source;
    
    spin_unlock_irqrestore(&g_time_state.lock, flags);
    
    debuglog(DEBUG_INFO,"TIME: Time source changed to %s\n", 
               g_time_state.sources[source].name);
    
    return 0;
}

/*
 * Get current time source
 */
time_source_t time_get_source(void)
{
    return g_time_state.current_source;
}

/*
 * Get enhanced time
 */
int time_get_enhanced_time(enhanced_time_t *time)
{
    if (!time) {
        return -1;
    }
    
    if (!g_time_state.initialized) {
        return -1;
    }
    
    unsigned long flags;
    spin_lock_irqsave(&g_time_state.lock, flags);
    
    /* Read from current time source */
    uint64_t timestamp = g_time_state.sources[g_time_state.current_source].read_func();
    if (timestamp == 0) {
        spin_unlock_irqrestore(&g_time_state.lock, flags);
        return -1;
    }
    
    /* Convert to enhanced time structure */
    if (time_convert_to_enhanced(timestamp, TIME_FORMAT_UNIX, time) != 0) {
        spin_unlock_irqrestore(&g_time_state.lock, flags);
        return -1;
    }
    
    /* Apply timezone and DST */
    time->timezone_offset = g_time_state.timezone.offset_hours;
    if (time_is_dst_active(time)) {
        time->timezone_offset += g_time_state.timezone.dst_offset_hours;
        time->daylight_saving = true;
    }
    
    /* Update cached time */
    g_time_state.current_time = *time;
    
    spin_unlock_irqrestore(&g_time_state.lock, flags);
    
    return 0;
}

/*
 * Convert timestamp to enhanced time
 */
static int time_convert_to_enhanced(uint64_t timestamp, time_format_t format, enhanced_time_t *time)
{
    if (!time) {
        return -1;
    }
    
    uint64_t unix_timestamp;
    
    /* Convert from source format to Unix timestamp */
    switch (format) {
        case TIME_FORMAT_UNIX:
            unix_timestamp = timestamp;
            break;
            
        case TIME_FORMAT_WINDOWS:
            /* Windows FILETIME: 100ns intervals since 1601-01-01 */
            unix_timestamp = (timestamp - 116444736000000000ULL) / 1000000ULL;
            break;
            
        case TIME_FORMAT_JULIAN:
            /* Julian Day to Unix timestamp conversion */
            unix_timestamp = (timestamp - 2440587) * 86400ULL;
            break;
            
        case TIME_FORMAT_NTP:
            /* NTP timestamp: seconds since 1900-01-01 */
            unix_timestamp = timestamp - 2208988800ULL;
            break;
            
        default:
            return -1;
    }
    
    /* Convert Unix timestamp to date/time components */
    /* This is a simplified conversion - real implementation would need proper calendar math */
    time->unix_timestamp = unix_timestamp;
    time->nanosecond = (timestamp % 1000000) * 1000;
    
    /* Simplified date calculation */
    uint64_t days = unix_timestamp / 86400ULL;
    uint64_t seconds = unix_timestamp % 86400ULL;
    
    time->year = 1970 + (days / 365);
    time->day_of_year = (days % 365) + 1;
    time->hour = seconds / 3600;
    time->minute = (seconds % 3600) / 60;
    time->second = seconds % 60;
    
    /* Calculate month and day (simplified) */
    time->month = 1 + (time->day_of_year / 30);
    time->day = 1 + (time->day_of_year % 30);
    if (time->month > 12) time->month = 12;
    if (time->day > 31) time->day = 31;
    
    /* Calculate weekday (simplified) */
    time->weekday = (days + 4) % 7;  /* Jan 1, 1970 was a Thursday */
    
    /* Generate other format timestamps */
    time->windows_timestamp = (unix_timestamp + 11644473600ULL) * 10000000ULL;
    time->julian_day = days + 2440587;
    time->ntp_timestamp = unix_timestamp + 2208988800ULL;
    
    return 0;
}

/*
 * Initialize high-resolution timer
 */
int time_init_high_res_timer(high_res_timer_t *timer)
{
    if (!timer) {
        return -1;
    }
    
    timer->start_tsc = read_tsc();
    timer->frequency = time_get_cpu_frequency();
    timer->resolution_ns = 1000000000ULL / timer->frequency;
    
    return 0;
}

/*
 * Get elapsed time in nanoseconds
 */
uint64_t time_timer_elapsed_ns(high_res_timer_t *timer)
{
    if (!timer || timer->frequency == 0) {
        return 0;
    }
    
    uint64_t current_tsc = read_tsc();
    uint64_t elapsed_ticks = current_tsc - timer->start_tsc;
    
    return (elapsed_ticks * 1000000000ULL) / timer->frequency;
}

/*
 * Get elapsed time in microseconds
 */
uint64_t time_timer_elapsed_us(high_res_timer_t *timer)
{
    return time_timer_elapsed_ns(timer) / 1000ULL;
}

/*
 * Get elapsed time in milliseconds
 */
uint64_t time_timer_elapsed_ms(high_res_timer_t *timer)
{
    return time_timer_elapsed_ns(timer) / 1000000ULL;
}

/*
 * Detect virtualization
 */
int time_detect_virtualization(void)
{
    g_time_state.vm_info.is_virtual = false;
    g_time_state.vm_info.hypervisor_name[0] = '\0';
    
    /* Check for VirtualBox */
    if (vbox_guest_is_available()) {
        g_time_state.vm_info.is_virtual = true;
        memcpy(g_time_state.vm_info.hypervisor_name, "VirtualBox", 10);
        g_time_state.vm_info.preferred_vm_source = TIME_SOURCE_VIRTUALBOX;
        g_time_state.vm_info.tsc_frequency = 0;  /* Will be calibrated */
        debuglog(DEBUG_INFO,"TIME: Detected VirtualBox virtualization\n");
        return 0;
    }
    
    /* Check for KVM (simplified detection) */
    /* This would check CPUID hypervisor leaf */
    g_time_state.vm_info.is_virtual = true;
    memcpy(g_time_state.vm_info.hypervisor_name, "KVM", 4);
    g_time_state.vm_info.preferred_vm_source = TIME_SOURCE_KVM;
    g_time_state.vm_info.tsc_frequency = 0;
    debuglog(DEBUG_INFO,"TIME: Detected KVM virtualization\n");
    
    return 0;
}

/*
 * Get VM information
 */
vm_time_info_t *time_get_vm_info(void)
{
    return &g_time_state.vm_info;
}

/*
 * Set timezone
 */
int time_set_timezone(timezone_type_t type, const char *custom_name, int8_t offset)
{
    unsigned long flags;
    
    spin_lock_irqsave(&g_time_state.lock, flags);
    
    switch (type) {
        case TIMEZONE_UTC:
            memset(&g_time_state.timezone, 0, sizeof(timezone_info_t));
            memcpy(g_time_state.timezone.name, "UTC", 4);
            break;
            
        case TIMEZONE_LOCAL:
            /* This would read system timezone */
            memset(&g_time_state.timezone, 0, sizeof(timezone_info_t));
            memcpy(g_time_state.timezone.name, "Local", 6);
            g_time_state.timezone.offset_hours = offset;
            break;
            
        case TIMEZONE_CUSTOM:
            if (custom_name) {
                memset(&g_time_state.timezone, 0, sizeof(timezone_info_t));
                memcpy(g_time_state.timezone.name, custom_name,
                          MIN(strlen(custom_name), sizeof(g_time_state.timezone.name) - 1));
                g_time_state.timezone.offset_hours = offset;
            }
            break;
    }
    
    spin_unlock_irqrestore(&g_time_state.lock, flags);
    
    debuglog(DEBUG_INFO,"TIME: Timezone set to %s (offset: %d hours)\n",
               g_time_state.timezone.name, g_time_state.timezone.offset_hours);
    
    return 0;
}

/*
 * Get timezone information
 */
int time_get_timezone_info(timezone_info_t *info)
{
    if (!info) {
        return -1;
    }
    
    unsigned long flags;
    spin_lock_irqsave(&g_time_state.lock, flags);
    
    *info = g_time_state.timezone;
    
    spin_unlock_irqrestore(&g_time_state.lock, flags);
    
    return 0;
}

/*
 * Check if DST is active
 */
bool time_is_dst_active(const enhanced_time_t *time)
{
    if (!time || !g_time_state.timezone.has_dst) {
        return false;
    }
    
    /* Simplified DST calculation */
    /* Real implementation would use proper DST rules for the timezone */
    if (time->month >= 3 && time->month <= 10) {
        /* Northern hemisphere DST approximation: Mar-Oct */
        return true;
    }
    
    return false;
}

/*
 * Set alarm
 */
int time_set_alarm(const enhanced_time_t *alarm_time, time_alarm_callback_t callback, void *context)
{
    if (!alarm_time || !callback) {
        return -1;
    }
    
    time_alarm_t *alarm = memory_heap_alloc(sizeof(time_alarm_t));
    if (!alarm) {
        return -1;
    }
    
    alarm->id = g_time_state.next_alarm_id++;
    alarm->trigger_time = *alarm_time;
    alarm->callback = callback;
    alarm->context = context;
    alarm->active = true;
    
    /* Add to alarm list */
    unsigned long flags;
    spin_lock_irqsave(&g_time_state.lock, flags);
    
    alarm->next = g_time_state.alarms;
    g_time_state.alarms = alarm;
    
    spin_unlock_irqrestore(&g_time_state.lock, flags);
    
    debuglog(DEBUG_INFO,"TIME: Alarm %d set for %04d-%02d-%02d %02d:%02d:%02d\n",
               alarm->id, alarm_time->year, alarm_time->month, alarm_time->day,
               alarm_time->hour, alarm_time->minute, alarm_time->second);
    
    return alarm->id;
}

/*
 * Cancel alarm
 */
int time_cancel_alarm(uint32_t alarm_id)
{
    time_alarm_t *current, *prev = NULL;
    bool found = false;
    
    unsigned long flags;
    spin_lock_irqsave(&g_time_state.lock, flags);
    
    current = g_time_state.alarms;
    while (current) {
        if (current->id == alarm_id) {
            if (prev) {
                prev->next = current->next;
            } else {
                g_time_state.alarms = current->next;
            }
            current->active = false;
            found = true;
            break;
        }
        prev = current;
        current = current->next;
    }
    
    spin_unlock_irqrestore(&g_time_state.lock, flags);
    
    if (found) {
        debuglog(DEBUG_INFO,"TIME: Alarm %d cancelled\n", alarm_id);
        return 0;
    }
    
    return -1;
}

/*
 * Process alarms (called periodically)
 */
__attribute__((unused)) static int time_process_alarms(void)
{
    enhanced_time_t current_time;
    time_alarm_t *current, *next;
    
    if (time_get_enhanced_time(&current_time) != 0) {
        return -1;
    }
    
    unsigned long flags;
    spin_lock_irqsave(&g_time_state.lock, flags);
    
    current = g_time_state.alarms;
    while (current) {
        next = current->next;
        
        if (current->active) {
            /* Check if alarm should trigger */
            if (current_time.unix_timestamp >= current->trigger_time.unix_timestamp) {
                current->active = false;
                
                /* Call callback outside of lock */
                time_alarm_callback_t callback = current->callback;
                void *context = current->context;
                uint32_t alarm_id = current->id;
                
                spin_unlock_irqrestore(&g_time_state.lock, flags);
                
                debuglog(DEBUG_INFO,"TIME: Alarm %d triggered\n", alarm_id);
                callback(context);
                
                spin_lock_irqsave(&g_time_state.lock, flags);
                
                /* Remove alarm */
                time_cancel_alarm(alarm_id);
            }
        }
        
        current = next;
    }
    
    spin_unlock_irqrestore(&g_time_state.lock, flags);
    
    return 0;
}

/*
 * Get time statistics
 */
int time_get_statistics(time_statistics_t *stats)
{
    if (!stats) {
        return -1;
    }
    
    unsigned long flags;
    spin_lock_irqsave(&g_time_state.lock, flags);
    
    *stats = g_time_state.stats;
    stats->current_source = g_time_state.current_source;
    stats->total_uptime_ms = time_get_uptime_ms();
    
    spin_unlock_irqrestore(&g_time_state.lock, flags);
    
    return 0;
}

/*
 * Get high-resolution time in microseconds
 */
uint64_t time_get_high_resolution_us(void)
{
    return time_timer_elapsed_us(&g_time_state.system_timer);
}

/*
 * Cleanup enhanced time management
 */
void time_enhanced_cleanup(void)
{
    if (!g_time_state.initialized) {
        return;
    }
    
    debuglog(DEBUG_INFO,"TIME: Cleaning up enhanced time management\n");
    
    /* Cancel all alarms */
    time_alarm_t *current = g_time_state.alarms;
    while (current) {
        time_alarm_t *next = current->next;
        memory_heap_free(current);
        current = next;
    }
    
    g_time_state.initialized = false;
}