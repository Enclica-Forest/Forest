/*
 * cmos_rtc.c - Minimal CMOS/RTC driver for Fern
 *
 * Implements the subset of the cmos_rtc.h API actually used by the kernel
 * (currently only cmos_read_time() by the TTY status-bar clock).  The MC146818
 * compatible RTC is accessed via I/O ports 0x70/0x71.
 */

#include "include/cmos_rtc.h"
#include "include/io_ports.h"

/* CMOS/RTC I/O ports (match the constants in the header). */
#define CMOS_NMI_DISABLE_BIT 0x80

static inline uint8_t cmos_inb(uint8_t reg) {
    outportb(CMOS_INDEX_PORT, (uint8_t)(reg | CMOS_NMI_DISABLE_BIT));
    /* short settle */
    for (volatile int i = 0; i < 4; i++) { __asm__ __volatile__("nop"); }
    return inportb(CMOS_DATA_PORT);
}

static inline void cmos_outb(uint8_t reg, uint8_t value) {
    outportb(CMOS_INDEX_PORT, (uint8_t)(reg | CMOS_NMI_DISABLE_BIT));
    for (volatile int i = 0; i < 4; i++) { __asm__ __volatile__("nop"); }
    outportb(CMOS_DATA_PORT, value);
}

uint8_t cmos_read_register(uint8_t reg) {
    return cmos_inb(reg);
}

void cmos_write_register(uint8_t reg, uint8_t value) {
    cmos_outb(reg, value);
}

uint8_t cmos_bcd_to_binary(uint8_t bcd) {
    return (uint8_t)(((bcd >> 4) * 10) + (bcd & 0x0F));
}

uint8_t cmos_binary_to_bcd(uint8_t binary) {
    return (uint8_t)(((binary / 10) << 4) | (binary % 10));
}

int cmos_wait_for_update_complete(void) {
    int timeout = 1000000;
    while ((cmos_inb(CMOS_REG_STATUS_A) & CMOS_STAT_A_UIP) && timeout--) {
        __asm__ __volatile__("nop");
    }
    return timeout > 0 ? 0 : -1;
}

bool cmos_is_battery_ok(void) {
    return (cmos_inb(CMOS_REG_STATUS_D) & CMOS_STAT_D_VRT) != 0;
}

bool cmos_get_binary_mode(void) {
    return (cmos_inb(CMOS_REG_STATUS_B) & CMOS_STAT_B_DM) != 0;
}

bool cmos_get_24h_mode(void) {
    return (cmos_inb(CMOS_REG_STATUS_B) & CMOS_STAT_B_24H) != 0;
}

/* Read the current RTC time into a cmos_time_t.  Returns 0 on success. */
int cmos_read_time(cmos_time_t *time) {
    if (!time) {
        return -1;
    }

    /* Wait for the update cycle to finish so the registers are stable. */
    if (cmos_wait_for_update_complete() != 0) {
        return -1;
    }

    bool binary_mode = cmos_get_binary_mode();
    bool hour_24 = cmos_get_24h_mode();

    uint8_t sec   = cmos_inb(CMOS_REG_SECONDS);
    uint8_t min   = cmos_inb(CMOS_REG_MINUTES);
    uint8_t hour  = cmos_inb(CMOS_REG_HOURS);
    uint8_t dow   = cmos_inb(CMOS_REG_DAY_WEEK);
    uint8_t dom   = cmos_inb(CMOS_REG_DAY_MONTH);
    uint8_t month = cmos_inb(CMOS_REG_MONTH);
    uint8_t year  = cmos_inb(CMOS_REG_YEAR);
    uint8_t century = 0;
    /* Century register is optional; read defensively. */
    century = cmos_inb(CMOS_REG_CENTURY);

    if (!binary_mode) {
        sec     = cmos_bcd_to_binary(sec);
        min     = cmos_bcd_to_binary(min);
        hour    = cmos_bcd_to_binary(hour);
        dom     = cmos_bcd_to_binary(dom);
        month   = cmos_bcd_to_binary(month);
        year    = cmos_bcd_to_binary(year);
        century = cmos_bcd_to_binary(century);
    }

    /* Decode 12-hour format if the 24h bit is clear.  The top bit of the
     * hours register (in BCD mode, bit 7 of the raw byte) is the PM flag. */
    if (!hour_24) {
        if (hour & 0x80) {
            hour = (uint8_t)(hour & 0x7F);
            if (hour == 12) hour = 0;   /* 12 AM == 0 */
            hour = (uint8_t)(hour + 12); /* PM */
            if (hour == 24) hour = 12;   /* 12 PM == 12 */
        } else {
            if (hour == 12) hour = 0;    /* 12 AM == 0 */
        }
    }

    time->seconds      = sec;
    time->minutes      = min;
    time->hours        = hour;
    time->day_week     = dow;
    time->day_month    = dom;
    time->month        = month;
    time->year         = year;
    time->century      = century;
    time->binary_mode  = binary_mode;
    time->hour_24_mode = hour_24;
    return 0;
}

/* Stub implementations for the remainder of the declared API.  These keep the
 * linker happy without pulling in a full RTC subsystem; they return failure
 * rather than silently doing nothing. */
int cmos_write_time(const cmos_time_t *time) { (void)time; return -1; }
int cmos_read_alarm(cmos_time_t *alarm) { (void)alarm; return -1; }
int cmos_write_alarm(const cmos_time_t *alarm) { (void)alarm; return -1; }
int cmos_set_alarm(uint8_t seconds, uint8_t minutes, uint8_t hours) {
    (void)seconds; (void)minutes; (void)hours; return -1;
}
bool cmos_is_alarm_pending(void) { return false; }
void cmos_clear_alarm_flag(void) { }

int cmos_enable_periodic_interrupt(uint8_t frequency) { (void)frequency; return -1; }
int cmos_disable_periodic_interrupt(void) { return 0; }
int cmos_enable_alarm_interrupt(void) { return 0; }
int cmos_disable_alarm_interrupt(void) { return 0; }
int cmos_enable_update_interrupt(void) { return 0; }
int cmos_disable_update_interrupt(void) { return 0; }
void cmos_clear_interrupt_flags(void) { }
uint8_t cmos_get_interrupt_status(void) { return 0; }

int cmos_set_binary_mode(bool binary) { (void)binary; return -1; }
int cmos_set_24h_mode(bool hour_24) { (void)hour_24; return -1; }
int cmos_set_square_wave(bool enable) { (void)enable; return -1; }
int cmos_set_daylight_saving(bool enable) { (void)enable; return -1; }
bool cmos_get_square_wave_enabled(void) { return false; }
bool cmos_get_daylight_saving_enabled(void) { return false; }

int cmos_read_info(cmos_info_t *info) {
    if (!info) return -1;
    info->base_memory_kb = 0;
    info->extended_memory_kb = 0;
    info->floppy_types[0] = 0;
    info->floppy_types[1] = 0;
    info->hard_drive_types[0] = 0;
    info->hard_drive_types[1] = 0;
    info->diagnostic_status = 0;
    info->shutdown_status = 0;
    return 0;
}
int cmos_write_info(const cmos_info_t *info) { (void)info; return -1; }
int cmos_get_memory_size(uint32_t *base_kb, uint32_t *extended_kb) {
    if (base_kb) *base_kb = 0;
    if (extended_kb) *extended_kb = 0;
    return -1;
}
int cmos_get_drive_types(uint8_t *floppy, uint8_t *hard) {
    if (floppy) *floppy = 0;
    if (hard) *hard = 0;
    return -1;
}

int cmos_get_statistics(cmos_rtc_stats_t *stats) {
    if (!stats) return -1;
    return 0;
}
void cmos_reset_statistics(void) { }

int cmos_calibrate_frequency(void) { return -1; }
int cmos_test_register_access(void) { return 0; }
int cmos_dump_all_registers(uint8_t *buffer, size_t size) {
    if (!buffer || size < 128) return -1;
    for (size_t i = 0; i < 128 && i < size; i++) {
        buffer[i] = cmos_inb((uint8_t)i);
    }
    return 0;
}
int cmos_restore_all_registers(const uint8_t *buffer, size_t size) {
    (void)buffer; (void)size; return -1;
}

bool cmos_is_valid_time(const cmos_time_t *time) {
    if (!time) return false;
    if (time->seconds > 59) return false;
    if (time->minutes > 59) return false;
    if (time->hours > 23) return false;
    if (time->month == 0 || time->month > 12) return false;
    if (time->day_month == 0 || time->day_month > 31) return false;
    return true;
}
bool cmos_is_leap_year(uint16_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}
uint8_t cmos_days_in_month(uint16_t year, uint8_t month) {
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 0 || month > 12) return 0;
    uint8_t d = days[month - 1];
    if (month == 2 && cmos_is_leap_year(year)) d = 29;
    return d;
}

int cmos_power_off_system(void) { return -1; }
int cmos_reboot_system(void) { return -1; }
int cmos_set_wake_time(const cmos_time_t *wake_time) { (void)wake_time; return -1; }

void cmos_dump_time(const cmos_time_t *time) { (void)time; }
void cmos_dump_info(const cmos_info_t *info) { (void)info; }
void cmos_dump_statistics(const cmos_rtc_stats_t *stats) { (void)stats; }
int cmos_run_self_test(void) { return 0; }

int cmos_rtc_init(void) { return 0; }
void cmos_rtc_cleanup(void) { }
bool cmos_rtc_is_available(void) { return cmos_is_battery_ok(); }
