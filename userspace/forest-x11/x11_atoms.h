#ifndef X11_ATOMS_H
#define X11_ATOMS_H

#include <stdint.h>

#define X11_MAX_ATOMS 256

typedef struct {
    uint32_t id;
    char   name[64];
    int    used;
} x11_atom_entry_t;

void     x11_atoms_init(void);
uint32_t   x11_intern_atom(const char* name, int len);
const char* x11_get_atom_name(uint32_t atom);
uint32_t   x11_get_atom_id(const char* name);

#define X11_ATOM_NONE             0
#define X11_ATOM_PRIMARY          1
#define X11_ATOM_CLIPBOARD        2
#define X11_ATOM_WM_PROTOCOLS     3
#define X11_ATOM_WM_DELETE_WINDOW 4
#define X11_ATOM_WM_NAME          5
#define X11_ATOM_WM_CLASS         6
#define X11_ATOM_WM_HINTS         7
#define X11_ATOM_WM_NORMAL_HINTS  8
#define X11_ATOM_STRING           9
#define X11_ATOM_UTF8_STRING      10
#define X11_ATOM_NET_WM_NAME      11
#define X11_ATOM_NET_WM_PID       12
#define X11_ATOM_NET_WM_STATE     13
#define X11_ATOM_TARGETS          14
#define X11_ATOM_TEXT             15
#define X11_ATOM_TIMESTAMP        16
#define X11_ATOM_MULTIPLE         17
#define X11_ATOM_DATOMS           18

#define X11_NUM_PREDEF_ATOMS 19

#endif
