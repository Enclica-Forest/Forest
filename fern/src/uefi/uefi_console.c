#include "uefi_console.h"
#include "../include/debuglog.h"

static EFI_SIMPLE_TEXT_INPUT_PROTOCOL *g_con_in = NULL;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *g_con_out = NULL;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *g_con_err = NULL;
static bool g_console_initialized = false;

static void uefi_print_string(uint16_t *str) {
    if (!g_con_out || !g_con_out->output_string) {
        return;
    }
    g_con_out->output_string(g_con_out, str);
}

static uint16_t *uefi_char_to_wide(char c) {
    static uint16_t buf[2];
    buf[0] = (uint16_t)(unsigned char)c;
    buf[1] = 0;
    return buf;
}

void uefi_console_init(EFI_SYSTEM_TABLE *system_table) {
    if (!system_table) {
        debuglog(DEBUG_WARN, "[UEFI-CONSOLE] No system table\n");
        return;
    }

    if (system_table->con_in) {
        g_con_in = (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *)system_table->con_in;
        debuglog(DEBUG_INFO, "[UEFI-CONSOLE] Console input available\n");
    }

    if (system_table->con_out) {
        g_con_out = (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *)system_table->con_out;
        debuglog(DEBUG_INFO, "[UEFI-CONSOLE] Console output available\n");
        if (g_con_out->reset) {
            g_con_out->reset(g_con_out, false);
        }
        if (g_con_out->set_attribute) {
            g_con_out->set_attribute(g_con_out,
                EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BACKGROUND_BLACK));
        }
        if (g_con_out->clear_screen) {
            g_con_out->clear_screen(g_con_out);
        }
    }

    if (system_table->std_err) {
        g_con_err = (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *)system_table->std_err;
        debuglog(DEBUG_INFO, "[UEFI-CONSOLE] Standard error available\n");
    }

    g_console_initialized = true;
    debuglog(DEBUG_INFO, "[UEFI-CONSOLE] Console initialized\n");
}

void uefi_putchar(char c) {
    if (!g_con_out || !g_con_out->output_string) {
        return;
    }

    if (c == '\n') {
        uint16_t nl[] = { '\r', '\n', 0 };
        g_con_out->output_string(g_con_out, nl);
        return;
    }

    if (c == '\r') {
        uint16_t cr[] = { '\r', 0 };
        g_con_out->output_string(g_con_out, cr);
        return;
    }

    if (c == '\t') {
        for (int i = 0; i < 4; i++) {
            uint16_t tab[] = { ' ', 0 };
            g_con_out->output_string(g_con_out, tab);
        }
        return;
    }

    uint16_t ch[] = { (uint16_t)(unsigned char)c, 0 };
    g_con_out->output_string(g_con_out, ch);
}

void uefi_puts(const char *str) {
    if (!str) return;
    while (*str) {
        uefi_putchar(*str++);
    }
}

void uefi_vprintf(const char *format, va_list args) {
    if (!format) return;

    char buf[1024];
    int len = vsnprintf(buf, sizeof(buf), format, args);
    if (len < 0) return;
    if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;

    for (int i = 0; i < len; i++) {
        uefi_putchar(buf[i]);
    }
}

void uefi_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    uefi_vprintf(format, args);
    va_end(args);
}

EFI_INPUT_KEY uefi_get_key(void) {
    EFI_INPUT_KEY key = { 0, 0 };
    if (!g_con_in || !g_con_in->read_key_stroke) {
        return key;
    }
    g_con_in->read_key_stroke(g_con_in, &key);
    return key;
}

bool uefi_get_key_nonblock(EFI_INPUT_KEY *key) {
    if (!g_con_in || !g_con_in->read_key_stroke || !key) {
        return false;
    }
    EFI_STATUS status = g_con_in->read_key_stroke(g_con_in, key);
    return status == 0;
}

void uefi_clear_screen(void) {
    if (g_con_out && g_con_out->clear_screen) {
        g_con_out->clear_screen(g_con_out);
    }
}

void uefi_set_text_color(uint8_t foreground, uint8_t background) {
    if (g_con_out && g_con_out->set_attribute) {
        g_con_out->set_attribute(g_con_out, EFI_TEXT_ATTR(foreground, background));
    }
}

void uefi_set_cursor(int col, int row) {
    if (g_con_out && g_con_out->set_cursor_position) {
        g_con_out->set_cursor_position(g_con_out, (uint64_t)col, (uint64_t)row);
    }
}

void uefi_enable_cursor(bool visible) {
    if (g_con_out && g_con_out->enable_cursor) {
        g_con_out->enable_cursor(g_con_out, visible);
    }
}
