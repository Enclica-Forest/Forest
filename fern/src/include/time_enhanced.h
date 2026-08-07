#ifndef TIME_ENHANCED_H
#define TIME_ENHANCED_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "time.h"

/* Enhanced time management for Fern
 * Provides comprehensive time tracking, network synchronization, and multiple time formats
 */

/* Time Sources */
typedef enum {
    TIME_SOURCE_RTC = 0,
    TIME_SOURCE_TSC,
    TIME_SOURCE_HPET,
    TIME_SOURCE_PIT,
    TIME_SOURCE_NETWORK,
    TIME_SOURCE_VIRTUALBOX,
    TIME_SOURCE_KVM,
    TIME_SOURCE_AUTO  /* Choose best available */
} time_source_t;

/* Time Formats */
typedef enum {
    TIME_FORMAT_UNIX = 0,      /* Unix timestamp (seconds since 1970-01-01) */
    TIME_FORMAT_WINDOWS,        /* Windows FILETIME (100ns intervals since 1601-01-01) */
    TIME_FORMAT_JULIAN,         /* Julian Day Number */
    TIME_FORMAT_GREGORIAN,      /* Year, month, day format */
    TIME_FORMAT_ISO8601,        /* ISO 8601 string format */
    TIME_FORMAT_NTP             /* NTP 32-bit timestamp */
} time_format_t;

/* Time Zones */
typedef enum {
    TIMEZONE_UTC = 0,
    TIMEZONE_LOCAL,
    TIMEZONE_CUSTOM
} timezone_type_t;

/* Enhanced Time Structure */
typedef struct {
    /* Basic components */
    uint16_t year;           /* Full year (e.g., 2026) */
    uint8_t month;           /* 1-12 */
    uint8_t day;             /* 1-31 */
    uint8_t hour;            /* 0-23 */
    uint8_t minute;          /* 0-59 */
    uint8_t second;          /* 0-59 */
    uint32_t nanosecond;     /* 0-999999999 */
    
    /* Extended information */
    uint8_t weekday;         /* 0-6 (0 = Sunday) */
    uint16_t day_of_year;    /* 1-366 */
    int8_t timezone_offset;  /* Hours from UTC (-12 to +14) */
    bool daylight_saving;    /* Daylight saving time active */
    
    /* Timestamps in different formats */
    uint64_t unix_timestamp;     /* Unix epoch */
    uint64_t windows_timestamp;  /* Windows FILETIME */
    uint32_t julian_day;        /* Julian Day Number */
    uint64_t ntp_timestamp;      /* NTP timestamp */
} enhanced_time_t;

/* Network Time Protocol (NTP) */
typedef struct {
    uint32_t reference_timestamp;     /* Reference time */
    uint32_t originate_timestamp;    /* Originate time */
    uint32_t receive_timestamp;      /* Receive time */
    uint32_t transmit_timestamp;     /* Transmit time */
} ntp_packet_t;

/* NTP Server Information */
typedef struct {
    char hostname[64];
    char ip_address[16];
    uint16_t port;
    int32_t offset_ms;
    uint32_t roundtrip_ms;
    bool reachable;
} ntp_server_t;

/* Time Synchronization Configuration */
typedef struct {
    time_source_t preferred_source;
    uint32_t sync_interval_ms;
    uint32_t sync_timeout_ms;
    uint32_t max_drift_ms;
    bool auto_sync;
    bool use_ntp;
    ntp_server_t ntp_servers[4];
    uint8_t ntp_server_count;
} time_sync_config_t;

/* Time Statistics */
typedef struct {
    uint64_t total_uptime_ms;
    uint64_t time_adjustments;
    int32_t total_drift_ms;
    uint32_t last_sync_time;
    time_source_t current_source;
    uint32_t sync_failures;
    uint64_t ntp_sync_count;
    uint64_t virtualbox_sync_count;
} time_statistics_t;

/* Virtualization-Aware Timekeeping */
typedef struct {
    bool is_virtual;
    char hypervisor_name[32];
    time_source_t preferred_vm_source;
    uint64_t tsc_frequency;
    uint64_t vm_sync_counter;
} vm_time_info_t;

/* Leap Second Information */
typedef struct {
    bool scheduled;
    uint32_t scheduled_time;     /* Unix timestamp when leap second occurs */
    int8_t leap_seconds;         /* Total leap seconds (positive or negative) */
    uint8_t announcement_months;  /* Months in advance to announce */
} leap_second_info_t;

/* High-Resolution Timer */
typedef struct {
    uint64_t start_tsc;
    uint64_t frequency;
    uint64_t resolution_ns;
} high_res_timer_t;

/* Time Zone Information */
typedef struct {
    char name[32];
    int8_t offset_hours;
    int8_t offset_minutes;
    bool has_dst;
    char dst_name[32];
    int8_t dst_offset_hours;
    int8_t dst_offset_minutes;
    uint8_t dst_start_month;
    uint8_t dst_start_week;
    uint8_t dst_start_day;
    uint8_t dst_start_hour;
    uint8_t dst_end_month;
    uint8_t dst_end_week;
    uint8_t dst_end_day;
    uint8_t dst_end_hour;
} timezone_info_t;

/* Core Functions */
int time_enhanced_init(void);
void time_enhanced_cleanup(void);

/* Time Source Management */
int time_set_source(time_source_t source);
time_source_t time_get_source(void);
int time_register_source(time_source_t source, uint64_t (*read_func)(void));
bool time_source_available(time_source_t source);

/* Enhanced Time Reading */
int time_get_enhanced_time(enhanced_time_t *time);
int time_set_enhanced_time(const enhanced_time_t *time);
uint64_t time_get_high_resolution_us(void);

/* Time Format Conversion */
int time_convert_format(const enhanced_time_t *input, time_format_t from_format, 
                      time_format_t to_format, void *output);
int time_format_to_string(const enhanced_time_t *time, time_format_t format, 
                        char *buffer, size_t buffer_size);
int time_parse_from_string(const char *string, time_format_t format, 
                         enhanced_time_t *time);

/* Network Time Synchronization */
int time_ntp_sync(const char *server, uint32_t timeout_ms);
int time_ntp_sync_auto(void);
int time_get_ntp_servers(ntp_server_t *servers, uint8_t *count);
int time_add_ntp_server(const ntp_server_t *server);

/* Virtualization Timekeeping */
int time_detect_virtualization(void);
int time_setup_vm_timekeeping(void);
int time_sync_with_host(void);
vm_time_info_t *time_get_vm_info(void);

/* Time Zone Management */
int time_set_timezone(timezone_type_t type, const char *custom_name, int8_t offset);
int time_get_timezone_info(timezone_info_t *info);
int time_apply_dst_rules(void);
bool time_is_dst_active(const enhanced_time_t *time);

/* Leap Second Handling */
int time_update_leap_seconds(void);
leap_second_info_t *time_get_leap_second_info(void);
int time_apply_leap_second(void);

/* High-Resolution Timing */
int time_init_high_res_timer(high_res_timer_t *timer);
uint64_t time_timer_elapsed_ns(high_res_timer_t *timer);
uint64_t time_timer_elapsed_us(high_res_timer_t *timer);
uint64_t time_timer_elapsed_ms(high_res_timer_t *timer);

/* Time Synchronization Configuration */
int time_set_sync_config(const time_sync_config_t *config);
int time_get_sync_config(time_sync_config_t *config);
int time_start_auto_sync(void);
int time_stop_auto_sync(void);

/* Statistics and Monitoring */
int time_get_statistics(time_statistics_t *stats);
int time_reset_statistics(void);
int time_get_time_source_quality(time_source_t source, uint32_t *quality_score);

/* Alarm and Timer Functions */
typedef void (*time_alarm_callback_t)(void *context);
int time_set_alarm(const enhanced_time_t *alarm_time, time_alarm_callback_t callback, void *context);
int time_set_relative_alarm(uint32_t delay_ms, time_alarm_callback_t callback, void *context);
int time_cancel_alarm(uint32_t alarm_id);

/* Calibration Functions */
int time_calibrate_tsc(void);
int time_calibrate_rtc(void);
int time_estimate_drift(void);

/* Power Management */
int time_suspend_timekeeping(void);
int time_resume_timekeeping(void);
int time_save_time_state(void);
int time_restore_time_state(void);

/* Debug and Diagnostics */
void time_dump_statistics(void);
void time_dump_configuration(void);
int time_run_self_test(void);

#endif /* TIME_ENHANCED_H */