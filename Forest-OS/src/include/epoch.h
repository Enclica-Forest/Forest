#ifndef EPOCH_H
#define EPOCH_H

#include <stdint.h>
#include <stdbool.h>
#include "system.h"

#define EPOCH_START_YEAR 1970

bool is_leap_year(uint16_t year);
uint32_t days_since_epoch(uint16_t year, uint8_t month, uint8_t day);
uint32_t rtc_to_unix_timestamp(const rtc_time_t *rt);
uint64_t rtc_to_unix_timestamp_us(const rtc_time_t *rt);
void epoch_init(void);
uint32_t epoch_get_current(void);
void epoch_set(uint32_t unix_time);
uint64_t epoch_get_uptime_ms(void);

extern uint32_t g_unix_epoch;
extern uint64_t g_system_uptime_ms;

#endif
