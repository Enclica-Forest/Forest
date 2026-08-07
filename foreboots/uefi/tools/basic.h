#ifndef FOREB_BASIC_H
#define FOREB_BASIC_H
#include "../efi.h"

// Run a BASIC program from the given text buffer (freestanding, no libc).
// Output goes to the con_puts/con_putc functions (defined in shell.c).
// Returns 0 on success, -1 on error.
int basic_run(const char *program);

// Open an interactive BASIC REPL (read-eval-print loop).
// Uses con_puts/con_putc for I/O.
// Returns 0 on exit.
int basic_repl(void);

#endif
