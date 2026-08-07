#ifndef CLIPBOARD_H
#define CLIPBOARD_H
#include "types.h"
#include <stdbool.h>
#define CLIPBOARD_MAX_SIZE 1048576
typedef enum { CLIPBOARD_TYPE_TEXT=0, CLIPBOARD_TYPE_IMAGE, CLIPBOARD_TYPE_FILE, CLIPBOARD_TYPE_CUSTOM } clipboard_type_t;
typedef struct { clipboard_type_t type; uint32 size; uint32 owner_pid; uint8 data[CLIPBOARD_MAX_SIZE]; bool valid; } clipboard_entry_t;
bool clipboard_init(void);
bool clipboard_set(clipboard_type_t type, const void* data, uint32 size);
const void* clipboard_get(clipboard_type_t type, uint32* out_size);
void clipboard_clear(void);
bool clipboard_has_content(clipboard_type_t type);
long sys_clipboard_set(clipboard_type_t type, const void* user_data, uint32 size);
long sys_clipboard_get(clipboard_type_t type, void* user_data, uint32* user_size);
long sys_clipboard_clear(void);
long sys_clipboard_has(clipboard_type_t type);
#endif
