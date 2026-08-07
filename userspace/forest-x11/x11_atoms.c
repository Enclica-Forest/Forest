#include "x11_atoms.h"
#include <string.h>

static x11_atom_entry_t g_atoms[X11_MAX_ATOMS];
static int g_atom_count = 0;

static const char* predef_names[] = {
    "NONE", "PRIMARY", "CLIPBOARD", "WM_PROTOCOLS", "WM_DELETE_WINDOW",
    "WM_NAME", "WM_CLASS", "WM_HINTS", "WM_NORMAL_HINTS",
    "STRING", "UTF8_STRING", "_NET_WM_NAME", "_NET_WM_PID",
    "_NET_WM_STATE", "TARGETS", "TEXT", "TIMESTAMP", "MULTIPLE", "DATOMS"
};

void x11_atoms_init(void) {
    memset(g_atoms, 0, sizeof(g_atoms));
    g_atom_count = X11_NUM_PREDEF_ATOMS;
    for (int i = 0; i < X11_NUM_PREDEF_ATOMS; i++) {
        g_atoms[i].id = i;
        g_atoms[i].used = 1;
        strncpy(g_atoms[i].name, predef_names[i], 63);
        g_atoms[i].name[63] = '\0';
    }
}

uint32_t x11_intern_atom(const char* name, int len) {
    if (len <= 0) len = strlen(name);
    for (int i = 0; i < g_atom_count; i++) {
        if (g_atoms[i].used && strncmp(g_atoms[i].name, name, len) == 0
            && (int)strlen(g_atoms[i].name) == len) {
            return g_atoms[i].id;
        }
    }
    if (g_atom_count < X11_MAX_ATOMS) {
        int idx = g_atom_count++;
        g_atoms[idx].id = idx;
        g_atoms[idx].used = 1;
        if (len > 63) len = 63;
        memcpy(g_atoms[idx].name, name, len);
        g_atoms[idx].name[len] = '\0';
        return g_atoms[idx].id;
    }
    return 0;
}

const char* x11_get_atom_name(uint32_t atom) {
    if (atom < (uint32_t)g_atom_count && g_atoms[atom].used)
        return g_atoms[atom].name;
    return "";
}

uint32_t x11_get_atom_id(const char* name) {
    return x11_intern_atom(name, strlen(name));
}
