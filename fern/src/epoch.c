/*
 * Epoch Time Management for Fern
 * Provides real Unix epoch time based on RTC hardware
 */

#include "include/epoch.h"
#include "include/system.h"
#include "include/timer.h"
#include "include/debuglog.h"

uint32_t g_unix_epoch = 0;
uint64_t g_system_uptime_ms = 0;

static const uint8_t days_per_month[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

bool is_leap_year(uint16_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

uint8_t get_days_in_month(uint16_t year, uint8_t month) {
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return days_per_month[month - 1];
}

uint32_t days_since_epoch(uint16_t year, uint8_t month, uint8_t day) {
    uint32_t days = 0;
    
    for (uint16_t y = EPOCH_START_YEAR; y < year; y++) {
        days += is_leap_year(y) ? 366 : 365;
    }
    
    for (uint8_t m = 1; m < month; m++) {
        days += get_days_in_month(year, m);
    }
    
    days += day - 1;
    
    return days;
}

uint32_t rtc_to_unix_timestamp(const rtc_time_t *rt) {
    if (!rt) {
        return 0;
    }
    
    if (rt->year < EPOCH_START_YEAR) {
        return 0;
    }
    
    uint32_t days = days_since_epoch(rt->year, rt->month, rt->day_of_month);
    
    uint32_t hours = rt->hours;
    uint32_t minutes = rt->minutes;
    uint32_t seconds = rt->seconds;
    
    uint32_t timestamp = days * 86400U;
    timestamp += hours * 3600U;
    timestamp += minutes * 60U;
    timestamp += seconds;
    
    return timestamp;
}

uint64_t rtc_to_unix_timestamp_us(const rtc_time_t *rt) {
    return (uint64_t)rtc_to_unix_timestamp(rt) * 1000000ULL;
}

void epoch_init(void) {
    rtc_time_t rtc;
    
    if (rtc_read_time(&rtc)) {
        g_unix_epoch = rtc_to_unix_timestamp(&rtc);
        debuglog(DEBUG_INFO, "EPOCH: RTC time read: %04d-%02d-%02d %02d:%02d:%02d\n",
                rtc.year, rtc.month, rtc.day_of_month,
                rtc.hours, rtc.minutes, rtc.seconds);
        debuglog(DEBUG_INFO, "EPOCH: Unix timestamp: %u\n", g_unix_epoch);
    } else {
        debuglog(DEBUG_WARN, "EPOCH: Failed to read RTC, using default epoch\n");
        g_unix_epoch = 1730000000U;
    }
    
    g_system_uptime_ms = 0;
}

uint32_t epoch_get_current(void) {
    uint32_t ticks = timer_get_ticks();
    uint64_t uptime_ms = (uint64_t)ticks;
    g_system_uptime_ms = uptime_ms;
    
    uint32_t base = g_unix_epoch;
    uint32_t freq = timer_get_frequency();
    
    if (freq > 0) {
        base += (uint32_t)(uptime_ms / 1000);
    }
    
    return base;
}

void epoch_set(uint32_t unix_time) {
    uint32_t current_ticks = timer_get_ticks();
    uint64_t current_uptime_ms = (uint64_t)current_ticks;
    
    if (timer_get_frequency() > 0) {
        current_uptime_ms = current_uptime_ms * 1000 / timer_get_frequency();
    }
    
    if (unix_time > current_uptime_ms / 1000) {
        g_unix_epoch = unix_time - (uint32_t)(current_uptime_ms / 1000);
    } else {
        g_unix_epoch = 0;
    }
    
    debuglog(DEBUG_INFO, "EPOCH: Epoch set to %u\n", unix_time);
}

uint64_t epoch_get_uptime_ms(void) {
    uint32_t ticks = timer_get_ticks();
    uint32_t freq = timer_get_frequency();
    
    if (freq > 0) {
        return (uint64_t)ticks * 1000 / freq;
    }
    
    return (uint64_t)ticks;
}
