#ifndef CMOS_RTC_H
#define CMOS_RTC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* CMOS/RTC Enhanced Driver for Fern
 * Provides comprehensive CMOS access and RTC functionality
 */

/* CMOS I/O Ports */
#define CMOS_INDEX_PORT         0x70    /* CMOS address/index port */
#define CMOS_DATA_PORT          0x71    /* CMOS data port */

/* CMOS Register Addresses */
#define CMOS_REG_SECONDS        0x00    /* Seconds (0-59) */
#define CMOS_REG_SECONDS_ALARM  0x01    /* Seconds Alarm */
#define CMOS_REG_MINUTES        0x02    /* Minutes (0-59) */
#define CMOS_REG_MINUTES_ALARM  0x03    /* Minutes Alarm */
#define CMOS_REG_HOURS          0x04    /* Hours (1-12 or 0-23) */
#define CMOS_REG_HOURS_ALARM    0x05    /* Hours Alarm */
#define CMOS_REG_DAY_WEEK       0x06    /* Day of Week (1-7) */
#define CMOS_REG_DAY_MONTH      0x07    /* Day of Month (1-31) */
#define CMOS_REG_MONTH          0x08    /* Month (1-12) */
#define CMOS_REG_YEAR           0x09    /* Year (0-99) */
#define CMOS_REG_STATUS_A       0x0A    /* Status Register A */
#define CMOS_REG_STATUS_B       0x0B    /* Status Register B */
#define CMOS_REG_STATUS_C       0x0C    /* Status Register C */
#define CMOS_REG_STATUS_D       0x0D    /* Status Register D */
#define CMOS_REG_CENTURY        0x32    /* Century (19-20) - if available */

/* Extended CMOS Registers */
#define CMOS_REG_DIAGNOSTIC    0x0E    /* Diagnostic status */
#define CMOS_REG_SHUTDOWN       0x0F    /* Shutdown status */
#define CMOS_REG_DISKETTE_TYPE 0x10    /* Floppy drive types */
#define CMOS_REG_BASE_MEMORY_LOW 0x15    /* Base memory low byte */
#define CMOS_REG_BASE_MEMORY_HIGH 0x16    /* Base memory high byte */
#define CMOS_REG_EXT_MEMORY_LOW 0x17    /* Extended memory low byte */
#define CMOS_REG_EXT_MEMORY_HIGH 0x18    /* Extended memory high byte */
#define CMOS_REG_DRIVE_C_TYPE   0x19    /* Drive C type */
#define CMOS_REG_DRIVE_D_TYPE   0x1A    /* Drive D type */
#define CMOS_REG_CUSTOM_DATA    0x2F    /* Custom data pointer */
#define CMOS_REG_RESET_CODE     0x2F    /* Reset code */

/* Status Register A Bits */
#define CMOS_STAT_A_UIP         0x80    /* Update In Progress */
#define CMOS_STAT_A_DV_MASK     0x70    /* Divider Select Mask */
#define CMOS_STAT_A_DV_4KHZ     0x00    /* 4.194304 MHz oscillator */
#define CMOS_STAT_A_DV_32KHZ    0x20    /* 32.768 kHz oscillator */
#define CMOS_STAT_A_RS_MASK     0x0F    /* Rate Select Mask */
#define CMOS_STAT_A_RS_NONE     0x00    /* No interrupts */
#define CMOS_STAT_A_RS_2HZ      0x0F    /* 2 Hz */
#define CMOS_STAT_A_RS_4HZ      0x0E    /* 4 Hz */
#define CMOS_STAT_A_RS_8HZ      0x0D    /* 8 Hz */
#define CMOS_STAT_A_RS_16HZ     0x0C    /* 16 Hz */
#define CMOS_STAT_A_RS_32HZ     0x0B    /* 32 Hz */
#define CMOS_STAT_A_RS_64HZ     0x0A    /* 64 Hz */
#define CMOS_STAT_A_RS_128HZ    0x09    /* 128 Hz */
#define CMOS_STAT_A_RS_256HZ    0x08    /* 256 Hz */
#define CMOS_STAT_A_RS_512HZ    0x07    /* 512 Hz */
#define CMOS_STAT_A_RS_1024HZ   0x06    /* 1024 Hz */
#define CMOS_STAT_A_RS_2048HZ   0x05    /* 2048 Hz */
#define CMOS_STAT_A_RS_4096HZ   0x04    /* 4096 Hz */
#define CMOS_STAT_A_RS_8192HZ   0x03    /* 8192 Hz */

/* Status Register B Bits */
#define CMOS_STAT_B_SET         0x80    /* Set bit - halt clock updates */
#define CMOS_STAT_B_PIE         0x40    /* Periodic Interrupt Enable */
#define CMOS_STAT_B_AIE         0x20    /* Alarm Interrupt Enable */
#define CMOS_STAT_B_UIE         0x10    /* Update-ended Interrupt Enable */
#define CMOS_STAT_B_SQWE        0x08    /* Square Wave Enable */
#define CMOS_STAT_B_DM          0x04    /* Data Mode (0=BCD, 1=Binary) */
#define CMOS_STAT_B_24H         0x02    /* 24-hour format */
#define CMOS_STAT_B_DSE         0x01    /* Daylight Saving Enable */

/* Status Register C Bits (read-only) */
#define CMOS_STAT_C_IRQF        0x80    /* Interrupt Request Flag */
#define CMOS_STAT_C_PF          0x40    /* Periodic Interrupt Flag */
#define CMOS_STAT_C_AF          0x20    /* Alarm Interrupt Flag */
#define CMOS_STAT_C_UF          0x10    /* Update-ended Interrupt Flag */

/* Status Register D Bits */
#define CMOS_STAT_D_VRT         0x80    /* Valid RAM and Time */

/* CMOS Information Types */
typedef struct {
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t day_week;
    uint8_t day_month;
    uint8_t month;
    uint8_t year;
    uint8_t century;
    bool binary_mode;
    bool hour_24_mode;
} cmos_time_t;

typedef struct {
    uint16_t base_memory_kb;       /* Base memory in KB */
    uint16_t extended_memory_kb;    /* Extended memory in KB */
    uint8_t floppy_types[2];      /* Floppy drive types */
    uint8_t hard_drive_types[2];   /* Hard drive types */
    uint8_t diagnostic_status;      /* Diagnostic status */
    uint8_t shutdown_status;       /* Shutdown status */
} cmos_info_t;

typedef struct {
    uint8_t frequency;            /* Interrupt frequency */
    bool periodic_enabled;          /* Periodic interrupts enabled */
    bool alarm_enabled;            /* Alarm interrupts enabled */
    bool update_enabled;           /* Update interrupts enabled */
    bool square_wave_enabled;      /* Square wave output enabled */
    uint32_t periodic_count;       /* Periodic interrupt count */
    uint32_t alarm_count;         /* Alarm interrupt count */
    uint32_t update_count;        /* Update interrupt count */
} cmos_rtc_stats_t;

/* CMOS Access Functions */
uint8_t cmos_read_register(uint8_t reg);
void cmos_write_register(uint8_t reg, uint8_t value);
int cmos_wait_for_update_complete(void);
bool cmos_is_battery_ok(void);

/* Time Functions */
int cmos_read_time(cmos_time_t *time);
int cmos_write_time(const cmos_time_t *time);
int cmos_read_alarm(cmos_time_t *alarm);
int cmos_write_alarm(const cmos_time_t *alarm);
int cmos_set_alarm(uint8_t seconds, uint8_t minutes, uint8_t hours);
bool cmos_is_alarm_pending(void);
void cmos_clear_alarm_flag(void);

/* RTC Interrupt Functions */
int cmos_enable_periodic_interrupt(uint8_t frequency);
int cmos_disable_periodic_interrupt(void);
int cmos_enable_alarm_interrupt(void);
int cmos_disable_alarm_interrupt(void);
int cmos_enable_update_interrupt(void);
int cmos_disable_update_interrupt(void);
void cmos_clear_interrupt_flags(void);
uint8_t cmos_get_interrupt_status(void);

/* Configuration Functions */
int cmos_set_binary_mode(bool binary);
int cmos_set_24h_mode(bool hour_24);
int cmos_set_square_wave(bool enable);
int cmos_set_daylight_saving(bool enable);
bool cmos_get_binary_mode(void);
bool cmos_get_24h_mode(void);
bool cmos_get_square_wave_enabled(void);
bool cmos_get_daylight_saving_enabled(void);

/* Information Functions */
int cmos_read_info(cmos_info_t *info);
int cmos_write_info(const cmos_info_t *info);
int cmos_get_memory_size(uint32_t *base_kb, uint32_t *extended_kb);
int cmos_get_drive_types(uint8_t *floppy, uint8_t *hard);

/* Statistics Functions */
int cmos_get_statistics(cmos_rtc_stats_t *stats);
void cmos_reset_statistics(void);

/* Advanced Functions */
int cmos_calibrate_frequency(void);
int cmos_test_register_access(void);
int cmos_dump_all_registers(uint8_t *buffer, size_t size);
int cmos_restore_all_registers(const uint8_t *buffer, size_t size);

/* Utility Functions */
uint8_t cmos_bcd_to_binary(uint8_t bcd);
uint8_t cmos_binary_to_bcd(uint8_t binary);
bool cmos_is_valid_time(const cmos_time_t *time);
bool cmos_is_leap_year(uint16_t year);
uint8_t cmos_days_in_month(uint16_t year, uint8_t month);

/* Power Management Functions */
int cmos_power_off_system(void);
int cmos_reboot_system(void);
int cmos_set_wake_time(const cmos_time_t *wake_time);

/* Debug and Diagnostics */
void cmos_dump_time(const cmos_time_t *time);
void cmos_dump_info(const cmos_info_t *info);
void cmos_dump_statistics(const cmos_rtc_stats_t *stats);
int cmos_run_self_test(void);

/* Initialization and Cleanup */
int cmos_rtc_init(void);
void cmos_rtc_cleanup(void);
bool cmos_rtc_is_available(void);

#endif /* CMOS_RTC_H */