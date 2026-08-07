#ifndef UEFI_CONSOLE_H
#define UEFI_CONSOLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "uefi_runtime.h"

typedef struct {
    uint16_t scan_code;
    uint16_t unicode_char;
} EFI_INPUT_KEY;

typedef struct {
    uint64_t signature;
    EFI_STATUS (*reset)(void *this, bool extended_verification);
    EFI_STATUS (*read_key_stroke)(void *this, EFI_INPUT_KEY *key);
    void *wait_for_key;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct {
    int32_t cr;
    int32_t cl;
    int32_t ul;
    int32_t lr;
} EFI_SIMPLE_TEXT_OUTPUT_MODE;

typedef struct {
    uint64_t signature;
    EFI_STATUS (*reset)(void *this, bool extended_verification);
    EFI_STATUS (*output_string)(void *this, uint16_t *string);
    EFI_STATUS (*test_string)(void *this, uint16_t *string);
    EFI_STATUS (*query_mode)(void *this, uint64_t mode_number, uint64_t *columns, uint64_t *rows);
    EFI_STATUS (*set_mode)(void *this, uint64_t mode_number);
    EFI_STATUS (*set_attribute)(void *this, uint64_t attribute);
    EFI_STATUS (*clear_screen)(void *this);
    EFI_STATUS (*set_cursor_position)(void *this, uint64_t column, uint64_t row);
    EFI_STATUS (*enable_cursor)(void *this, bool visible);
    EFI_SIMPLE_TEXT_OUTPUT_MODE *mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

#define EFI_BLACK         0x00
#define EFI_BLUE          0x01
#define EFI_GREEN         0x02
#define EFI_CYAN          0x03
#define EFI_RED           0x04
#define EFI_MAGENTA       0x05
#define EFI_BROWN         0x06
#define EFI_LIGHTGRAY     0x07
#define EFI_BRIGHT        0x08
#define EFI_LIGHTBLUE     0x09
#define EFI_LIGHTGREEN    0x0A
#define EFI_LIGHTCYAN     0x0B
#define EFI_LIGHTRED      0x0C
#define EFI_LIGHTMAGENTA  0x0D
#define EFI_YELLOW        0x0E
#define EFI_WHITE         0x0F

#define EFI_BACKGROUND_BLACK     0x00
#define EFI_BACKGROUND_BLUE      0x10
#define EFI_BACKGROUND_GREEN     0x20
#define EFI_BACKGROUND_CYAN      0x30
#define EFI_BACKGROUND_RED       0x40
#define EFI_BACKGROUND_MAGENTA   0x50
#define EFI_BACKGROUND_BROWN     0x60
#define EFI_BACKGROUND_LIGHTGRAY 0x70

#define EFI_TEXT_ATTR(Foreground, Background) ((Foreground) | ((Background) << 4))

#define EFI_SCAN_UP       0x01
#define EFI_SCAN_DOWN     0x02
#define EFI_SCAN_RIGHT    0x03
#define EFI_SCAN_LEFT     0x04
#define EFI_SCAN_HOME     0x06
#define EFI_SCAN_END      0x07
#define EFI_SCAN_PAGE_UP  0x09
#define EFI_SCAN_PAGE_DOWN 0x0B
#define EFI_SCAN_ESC      0x17

#define EFI_KEY_NULL      0x00
#define EFI_KEY_CHAR_UP   0x5E
#define EFI_KEY_CHAR_DOWN 0x5F

void uefi_console_init(EFI_SYSTEM_TABLE *system_table);
void uefi_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
void uefi_vprintf(const char *format, va_list args);
void uefi_puts(const char *str);
void uefi_putchar(char c);
EFI_INPUT_KEY uefi_get_key(void);
bool uefi_get_key_nonblock(EFI_INPUT_KEY *key);
void uefi_clear_screen(void);
void uefi_set_text_color(uint8_t foreground, uint8_t background);
void uefi_set_cursor(int col, int row);
void uefi_enable_cursor(bool visible);

#endif
