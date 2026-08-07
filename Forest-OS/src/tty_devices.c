/*
 * Virtual Terminal Device Drivers for Fern
 * Implements virtual terminal devices: /dev/tty0, /dev/tty1, ..., /dev/tty63
 *
 * This implementation follows Unix TTY architecture with line discipline
 * and terminal attributes support.
 */

#include "include/device_fs.h"
#include "include/memory_safe.h"
#include "include/debuglog.h"
#include "include/debug.h"
#include "include/string.h"
#include "include/tty.h"
#include "include/kb.h"
#include "include/input_event.h"
#include "include/ps2_keyboard.h"
#include "include/task.h"
#include "include/timer.h"
#include "include/errno_defs.h"

/* Forward declaration for keyboard event callback */
static void tty_keyboard_event_callback(const keyboard_event_t* event);

/* Global virtual TTYs */
virtual_tty_t g_virtual_ttys[MAX_VIRTUAL_TTYS];

/* Forward declarations */
static device_operations_t ttyN_ops;

/* TTY ioctl commands */
#define TIOCGETA    0x5401  /* Get termios structure */
#define TIOCSETA    0x5402  /* Set termios structure (drain) */
#define TIOCSETAW   0x5403  /* Set termios structure (wait) */
#define TIOCSETAF   0x5404  /* Set termios structure (flush) */
#define TIOCGWINSZ  0x5413  /* Get window size */
#define TIOCSWINSZ  0x5414  /* Set window size */
#define TIOCOUTQ    0x5411  /* Get output queue size */
#define TIOCINQ     0x5412  /* Get input queue size */

/* Termios flags */
#define ICANON      0x00000002  /* Canonical input mode */
#define ECHO        0x00000008  /* Echo input characters */
#define ECHOE       0x00000010  /* Echo erase character as backspace */
#define ECHOK       0x00000020  /* Echo kill character */
#define ECHONL      0x00000040  /* Echo newline */
#define ISIG        0x00000080  /* Enable signals */
#define ICRNL       0x00000100  /* Translate CR to NL on input */
#define IXON        0x00000200  /* Enable XON/XOFF flow control */
#define IXOFF       0x00000400  /* Enable XON/XOFF input flow control */
#define OPOST       0x00000001  /* Enable output processing */

/* Control characters */
#define VEOF        0  /* End of file */
#define VEOL        1  /* End of line */
#define VERASE      2  /* Erase character */
#define VKILL       3  /* Kill line */
#define VINTR       4  /* Interrupt character */
#define VQUIT       5  /* Quit character */
#define VSUSP       10 /* Suspend character */
#define VSTART      12 /* Start character */
#define VSTOP       13 /* Stop character */

/* Per-TTY output column tracking for ONOCR/ONLRET handling */
static uint16_t g_tty_output_col[MAX_VIRTUAL_TTYS];

static bool tty_input_queue_pop(virtual_tty_t* tty, char* out) {
    if (!tty || !out || tty->input_head == tty->input_tail) {
        return false;
    }

    *out = (char)tty->input_buffer[tty->input_tail];
    tty->input_tail = (tty->input_tail + 1) % TTY_BUFFER_SIZE;
    return true;
}

static void tty_apply_input_transform(termios_t* termios, bool* drop, char* c) {
    if (!termios || !drop || !c) {
        return;
    }

    *drop = false;
    if (*c == '\r') {
        if (termios->c_iflag & IGNCR) {
            *drop = true;
            return;
        }
        if (termios->c_iflag & ICRNL) {
            *c = '\n';
        }
    } else if (*c == '\n') {
        if (termios->c_iflag & INLCR) {
            *c = '\r';
        }
    }
}

static inline uint32_t tty_ring_next(uint32_t pos) {
    return (pos + 1) % TTY_BUFFER_SIZE;
}

static inline uint32_t tty_ring_prev(uint32_t pos) {
    return (pos + TTY_BUFFER_SIZE - 1) % TTY_BUFFER_SIZE;
}

/* Reprints input_buffer[tty->edit_cursor .. tty->input_head) to the screen,
 * optionally followed by one blank (to erase a stale trailing character
 * left over from a delete/backspace shift), then walks the on-screen
 * cursor back to tty->edit_cursor. Callers are expected to have already
 * updated edit_cursor/input_head to their post-edit values before calling
 * this — it only fixes up what's drawn on screen to match the buffer. */
static void tty_echo_tail_and_reposition(virtual_tty_t* tty, bool erase_extra) {
    uint32_t len = (tty->input_head + TTY_BUFFER_SIZE - tty->edit_cursor) % TTY_BUFFER_SIZE;
    uint32_t pos = tty->edit_cursor;
    for (uint32_t i = 0; i < len; i++) {
        tty_putc((char)tty->input_buffer[pos]);
        pos = tty_ring_next(pos);
    }
    uint32_t back = len;
    if (erase_extra) {
        tty_putc(' ');
        back++;
    }
    for (uint32_t i = 0; i < back; i++) {
        tty_write_ansi("\x1b[D");
    }
}

/* Erase the character immediately before edit_cursor (backspace). Handles
 * both the common "cursor at end of line" case and deleting from the
 * middle of a line the user has moved the cursor back into. */
static bool tty_canonical_erase_before_cursor(virtual_tty_t* tty) {
    if (!tty || tty->edit_cursor == tty->line_start) {
        return false;
    }

    if (tty->edit_cursor == tty->input_head) {
        /* Simple case: erase the last character in the line. */
        tty->input_head = tty_ring_prev(tty->input_head);
        tty->edit_cursor = tty->input_head;
        return true;
    }

    /* Mid-line: shift everything after the deleted character left by one. */
    uint32_t pos = tty->edit_cursor;
    while (pos != tty->input_head) {
        uint32_t prev = tty_ring_prev(pos);
        tty->input_buffer[prev] = tty->input_buffer[pos];
        pos = tty_ring_next(pos);
    }
    tty->input_head = tty_ring_prev(tty->input_head);
    tty->edit_cursor = tty_ring_prev(tty->edit_cursor);
    return true;
}

/* Delete the character at edit_cursor (forward delete / Del key). Cursor
 * position itself doesn't move; only the buffer and screen tail shift. */
static bool tty_canonical_delete_at_cursor(virtual_tty_t* tty) {
    if (!tty || tty->edit_cursor == tty->input_head) {
        return false;
    }

    uint32_t src = tty_ring_next(tty->edit_cursor);
    uint32_t dst = tty->edit_cursor;
    while (src != tty->input_head) {
        tty->input_buffer[dst] = tty->input_buffer[src];
        dst = src;
        src = tty_ring_next(src);
    }
    tty->input_head = dst;
    return true;
}

/* Insert c at edit_cursor, shifting any characters after it right by one. */
static void tty_canonical_insert_at_cursor(virtual_tty_t* tty, char c) {
    uint32_t pos = tty->input_head;
    while (pos != tty->edit_cursor) {
        uint32_t prev = tty_ring_prev(pos);
        tty->input_buffer[pos] = tty->input_buffer[prev];
        pos = prev;
    }
    tty->input_buffer[tty->edit_cursor] = (uint8_t)c;
    tty->input_head = tty_ring_next(tty->input_head);
}

static uint32_t tty_vtime_to_ticks(uint8_t vtime_tenths) {
    uint32_t freq = timer_get_frequency();
    uint32_t ticks = (vtime_tenths * freq) / 10;
    return (ticks == 0) ? 1 : ticks;
}

static void tty_output_emit_char(virtual_tty_t* tty, char c) {
    if (!tty) {
        return;
    }

    tty_putc(c);

    if (c == '\r') {
        g_tty_output_col[tty->tty_number] = 0;
    } else if (c == '\n') {
        if (tty->termios.c_oflag & ONLRET) {
            g_tty_output_col[tty->tty_number] = 0;
        }
    } else if (c == '\b') {
        if (g_tty_output_col[tty->tty_number] > 0) {
            g_tty_output_col[tty->tty_number]--;
        }
    } else {
        if (tty->winsize.cols > 0) {
            g_tty_output_col[tty->tty_number] =
                (g_tty_output_col[tty->tty_number] + 1) % tty->winsize.cols;
        } else {
            g_tty_output_col[tty->tty_number]++;
        }
    }
}

static void tty_init_termios(termios_t* termios) {
    memset(termios, 0, sizeof(termios_t));
    termios->c_lflag = ICANON | ECHO | ECHOE | ECHOK | ECHONL | ISIG | IEXTEN;
    termios->c_iflag = ICRNL | IXON | IXOFF | IXANY;
    termios->c_oflag = OPOST | ONLCR;
    termios->c_cflag = CREAD | CS8;
    
    termios->c_cc[VEOF] = 4;    /* Ctrl+D */
    termios->c_cc[VEOL] = 0;    /* Not used */
    termios->c_cc[VERASE] = 127; /* Backspace */
    termios->c_cc[VKILL] = 21;  /* Ctrl+U */
    termios->c_cc[VINTR] = 3;   /* Ctrl+C */
    termios->c_cc[VQUIT] = 28;  /* Ctrl+\ */
    termios->c_cc[VSUSP] = 26;  /* Ctrl+Z */
    termios->c_cc[VSTART] = 17; /* Ctrl+Q */
    termios->c_cc[VSTOP] = 19;  /* Ctrl+S */
    termios->c_cc[VMIN] = 1;    /* Minimum characters to read */
    termios->c_cc[VTIME] = 0;   /* Timeout (tenths of a second) */
    termios->c_cc[VSWTC] = 0;   /* Switch terminal characters */
    termios->c_cc[VREPRINT] = 18; /* Ctrl+R */
    termios->c_cc[VDISCARD] = 15; /* Ctrl+O */
    termios->c_cc[VWERASE] = 23; /* Ctrl+W */
    termios->c_cc[VLNEXT] = 22; /* Ctrl+V */
    termios->c_cc[VEOF2] = 0;   /* Alternate end-of-file character */
}

/* keyboard_scancode_to_ascii() only maps printable-key scancodes to a
 * single ASCII char and returns 0 for navigation keys, so arrows/Home/End/
 * Delete were silently dropped before reaching here. This TTY driver — not
 * a userspace readline — owns canonical-mode line editing (see ttyN_read,
 * tty_try_canonical_erase's replacement below), so arrow keys have to be
 * handled entirely in-kernel by moving/mutating edit_cursor, not by
 * forwarding raw escape bytes to the reading process. Up/Down/PageUp/
 * PageDown/Insert are intentionally no-ops: there's no command history or
 * overwrite-mode buffer to act on yet. */
// `pressed` is true for both an initial key-down and hardware typematic
// auto-repeat (the caller folds KEY_STATE_PRESSED and KEY_STATE_REPEAT into
// the same true value -- see tty_keyboard_event_callback()), false only for
// key-up. Editing/echo below fires on press/repeat, matching every real
// terminal driver (a key does something when you press it, not when you
// release it) -- this used to fire on `!pressed` (release only), so the
// character/backspace/arrow-key effect of a keystroke landed one full
// press-release cycle later than the physical key event, which is exactly
// backwards and, under fast typing or IRQ scheduling jitter, let a key's
// belated release-triggered echo land after a *different* line had already
// started (e.g. after a foreground child like htop exited and the shell had
// already redrawn its next empty prompt).
static void ttyN_handle_keyboard_event(uint8_t tty_num, uint8_t keycode, bool pressed) {
    if (tty_num >= MAX_VIRTUAL_TTYS || !g_virtual_ttys[tty_num].active) {
        return;
    }

    virtual_tty_t* tty = &g_virtual_ttys[tty_num];

    if (pressed) {
        if (tty->termios.c_lflag & ICANON) {
            switch (keycode) {
                case PS2_KEY_LEFT:
                    if (tty->edit_cursor != tty->line_start) {
                        tty->edit_cursor = tty_ring_prev(tty->edit_cursor);
                        if (tty->termios.c_lflag & ECHO) tty_write_ansi("\x1b[D");
                    }
                    return;
                case PS2_KEY_RIGHT:
                    if (tty->edit_cursor != tty->input_head) {
                        tty->edit_cursor = tty_ring_next(tty->edit_cursor);
                        if (tty->termios.c_lflag & ECHO) tty_write_ansi("\x1b[C");
                    }
                    return;
                case PS2_KEY_HOME:
                    while (tty->edit_cursor != tty->line_start) {
                        tty->edit_cursor = tty_ring_prev(tty->edit_cursor);
                        if (tty->termios.c_lflag & ECHO) tty_write_ansi("\x1b[D");
                    }
                    return;
                case PS2_KEY_END:
                    while (tty->edit_cursor != tty->input_head) {
                        tty->edit_cursor = tty_ring_next(tty->edit_cursor);
                        if (tty->termios.c_lflag & ECHO) tty_write_ansi("\x1b[C");
                    }
                    return;
                case PS2_KEY_DELETE:
                    if (tty_canonical_delete_at_cursor(tty) && (tty->termios.c_lflag & ECHO)) {
                        tty_echo_tail_and_reposition(tty, true);
                    }
                    return;
                default:
                    break;
            }
        }

        char c = keyboard_scancode_to_ascii(keycode);
        if (c != 0) {
            bool drop_input = false;
            tty_apply_input_transform(&tty->termios, &drop_input, &c);
            if (drop_input) {
                return;
            }

            /* Handle special characters. VINTR/VQUIT/VSUSP are gated on ISIG
             * (POSIX: clearing ISIG, as any raw/cbreak-mode reader like
             * htop/vi/sh's line editor does, means these bytes must reach
             * the reader literally instead of being turned into a signal --
             * previously this fired unconditionally, so Ctrl-C at a raw-mode
             * prompt silently vanished into an ignored SIGINT instead of
             * ever reaching the program). */
            if ((tty->termios.c_lflag & ISIG) && c == tty->termios.c_cc[VINTR]) {
                /* Interrupt - send SIGINT to the TTY's foreground process
                 * group, not task_send_signal(0, ...)'s "current task's own
                 * group": this fires from keyboard-IRQ context, where
                 * current_task is whatever happened to be scheduled at
                 * interrupt time, not necessarily the foreground job. */
                debuglog(DEBUG_INFO, "TTY%d: Received interrupt character, sending SIGINT\n", tty_num);
                uint32_t target_pgid = tty->fg_pgid ? tty->fg_pgid : (current_task ? current_task->pgrp : 0);
                task_send_signal_to_pgrp(target_pgid, SIGINT);
            } else if ((tty->termios.c_lflag & ISIG) && c == tty->termios.c_cc[VQUIT]) {
                /* Quit - send SIGQUIT to the TTY's foreground process group */
                debuglog(DEBUG_INFO, "TTY%d: Received quit character, sending SIGQUIT\n", tty_num);
                uint32_t target_pgid = tty->fg_pgid ? tty->fg_pgid : (current_task ? current_task->pgrp : 0);
                task_send_signal_to_pgrp(target_pgid, SIGQUIT);
            } else if ((tty->termios.c_lflag & ISIG) && c == tty->termios.c_cc[VSUSP]) {
                /* Suspend - send SIGTSTP to the TTY's foreground process group */
                debuglog(DEBUG_INFO, "TTY%d: Received suspend character, sending SIGTSTP\n", tty_num);
                uint32_t target_pgid = tty->fg_pgid ? tty->fg_pgid : (current_task ? current_task->pgrp : 0);
                task_send_signal_to_pgrp(target_pgid, SIGTSTP);
            } else if ((tty->termios.c_lflag & ICANON) && c == tty->termios.c_cc[VERASE]) {
                bool at_end = (tty->edit_cursor == tty->input_head);
                bool erased = tty_canonical_erase_before_cursor(tty);
                if (erased && (tty->termios.c_lflag & ECHO)) {
                    if (tty->termios.c_lflag & ECHOE) {
                        if (at_end) {
                            tty_write_ansi("\b \b");
                        } else {
                            /* Cursor was mid-line: redraw the shifted tail. */
                            tty_write_ansi("\x1b[D");
                            tty_echo_tail_and_reposition(tty, true);
                        }
                    } else {
                        tty_putc(c);
                    }
                }
            } else {
                /* Normal character - insert at cursor (append if the
                 * cursor is already at the end of the line, the common
                 * case for straight-line typing). */
                uint32_t next_head = tty_ring_next(tty->input_head);
                if (next_head != tty->input_tail) {
                    bool was_at_end = (tty->edit_cursor == tty->input_head);
                    if (was_at_end) {
                        tty->input_buffer[tty->input_head] = c;
                        tty->input_head = next_head;
                        tty->edit_cursor = tty->input_head;

                        if (tty->termios.c_lflag & ECHO) {
                            if (c == '\b' || c == 127) {
                                tty_write_ansi("\b \b");
                            } else {
                                tty_putc(c);
                            }
                        }
                    } else {
                        tty_canonical_insert_at_cursor(tty, c);
                        if (tty->termios.c_lflag & ECHO) {
                            tty_echo_tail_and_reposition(tty, false);
                        }
                        tty->edit_cursor = tty_ring_next(tty->edit_cursor);
                        if (tty->termios.c_lflag & ECHO) {
                            tty_write_ansi("\x1b[C");
                        }
                    }

                    /* In canonical mode, a newline/EOF commits the line:
                     * the next line starts fresh right after it. */
                    if ((tty->termios.c_lflag & ICANON) && (c == '\n' || c == tty->termios.c_cc[VEOF])) {
                        tty->line_start = tty->input_head;
                        tty->edit_cursor = tty->input_head;
                        debuglog(DEBUG_INFO, "TTY%d: Input ready (canonical mode)\n", tty_num);
                    }
                }
            }
        }
    }
}

static ssize_t ttyN_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset) {
    (void)offset;

    if (!dev || !buffer || count == 0) return 0;

    /* Get TTY number from minor */
    uint32_t tty_num = dev->minor;
    if (tty_num >= MAX_VIRTUAL_TTYS) return DEVICE_ERROR_INVALID_PARAM;

    virtual_tty_t *tty = &g_virtual_ttys[tty_num];
    if (!tty->active) return 0;

    /* Background read from controlling terminal: POSIX SIGTTIN handling.
     *
     * Per POSIX, if the process group is orphaned, or the calling task has
     * SIGTTIN blocked/ignored, the signal must NOT be generated at all and
     * the call must fail with EIO (otherwise the group could be stopped
     * forever with nobody left able to foreground it). Otherwise, SIGTTIN
     * is sent to the whole background process group and - since SIGTTIN is
     * a stop-class signal - task_send_signal_to_pgrp() actually transitions
     * every member (including this task) to TASK_STATE_SUSPENDED via
     * task_suspend() and records the job as stopped via job_update_state(),
     * mirroring how SIGSTOP is handled. We then block/retry (instead of
     * returning a fake result to a caller that isn't expecting one) until
     * job_foreground() makes this pgrp the controlling terminal's
     * foreground group again.
     *
     * This restriction only applies to a task's controlling terminal
     * (POSIX): current_task->tty_fd records which /dev/ttyN that is (see
     * sys_tcgetpgrp()/sys_tcsetpgrp() in syscall.c), so skip the check
     * entirely for any other tty this task happens to open. */
    if (tty->fg_pgid != 0 && current_task && current_task->tty_fd == (int32)tty_num &&
        current_task->pgrp != tty->fg_pgid) {
        if (task_pgrp_is_orphaned(current_task->pgrp, current_task->session)) {
            debuglog(DEBUG_INFO, "TTY%d: Background read from orphaned pgrp %u, failing with EIO\n",
                     tty_num, current_task->pgrp);
            return -EIO;
        }

        debuglog(DEBUG_INFO, "TTY%d: Background read from pgrp %u (fg=%u), sending SIGTTIN\n",
                 tty_num, current_task->pgrp, tty->fg_pgid);
        if (task_send_signal_to_pgrp_checked(current_task->pgrp, SIGTTIN, current_task->id) ==
            SIGNAL_DELIVERY_BLOCKED_OR_IGNORED) {
            return -EIO;
        }

        while (tty->fg_pgid != 0 && current_task->pgrp != tty->fg_pgid) {
            task_yield();
        }
    }

    char *buf = (char*)buffer;
    size_t read = 0;

    /* In canonical mode, wait for newline or EOF */
    if (tty->termios.c_lflag & ICANON) {
        while (read == 0 && tty->input_head == tty->input_tail) {
            /* Wait for input - for now, just yield */
            task_yield();
        }
        
        /* Read until newline or EOF */
        while (read < count && tty->input_head != tty->input_tail) {
            char c = tty->input_buffer[tty->input_tail];
            tty->input_tail = (tty->input_tail + 1) % TTY_BUFFER_SIZE;
            buf[read++] = c;
            
            if (c == '\n' || c == tty->termios.c_cc[VEOF]) {
                break;
            }
        }
    } else {
        /* In non-canonical mode (raw or cbreak) */
        uint8_t vmin = tty->termios.c_cc[VMIN];
        uint8_t vtime = tty->termios.c_cc[VTIME];
        size_t target = (vmin > count) ? count : vmin;

        if (vmin > 0 && vtime == 0) {
            /* Block until at least min(VMIN, count) bytes are available. */
            while (read < target) {
                char c;
                if (tty_input_queue_pop(tty, &c)) {
                    buf[read++] = c;
                } else {
                    task_yield();
                }
            }
        } else if (vmin == 0 && vtime == 0) {
            /* Polling read: return immediately with whatever is queued. */
            char c;
            while (read < count && tty_input_queue_pop(tty, &c)) {
                buf[read++] = c;
            }
        } else if (vmin == 0) {
            /* Timed read: wait up to VTIME for first byte, then return. */
            uint32_t start_tick = timer_get_ticks();
            uint32_t timeout_ticks = tty_vtime_to_ticks(vtime);
            while (timer_get_ticks() - start_tick < timeout_ticks) {
                if (tty_input_queue_pop(tty, &buf[read])) {
                    read++;
                    break;
                }
                task_yield();
            }

            if (read > 0) {
                char c;
                while (read < count && tty_input_queue_pop(tty, &c)) {
                    buf[read++] = c;
                }
            }
        } else {
            /* Inter-byte timer mode: block for first byte, then timeout between bytes. */
            while (read == 0) {
                if (tty_input_queue_pop(tty, &buf[read])) {
                    read++;
                    break;
                }
                task_yield();
            }

            uint32_t timeout_ticks = tty_vtime_to_ticks(vtime);
            uint32_t last_input_tick = timer_get_ticks();
            while (read < count && read < target) {
                char c;
                if (tty_input_queue_pop(tty, &c)) {
                    buf[read++] = c;
                    last_input_tick = timer_get_ticks();
                    continue;
                }

                if (timer_get_ticks() - last_input_tick >= timeout_ticks) {
                    break;
                }
                task_yield();
            }
        }
    }

    return read;
}

static ssize_t ttyN_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset) {
    (void)offset;

    if (!dev || !buffer || count == 0) return 0;

    /* Get TTY number from minor */
    uint32_t tty_num = dev->minor;
    if (tty_num >= MAX_VIRTUAL_TTYS) return DEVICE_ERROR_INVALID_PARAM;

    virtual_tty_t *tty = &g_virtual_ttys[tty_num];
    if (!tty->active) {
        /* Initialize TTY on first write. Note: fg_pgid/termios are NOT
         * reset here - tty_devices_init() already establishes correct
         * defaults for every tty at boot, and by the time of the first
         * write a shell may have already legitimately configured this
         * tty's job-control state (fg_pgid via tcsetpgrp(), termios via
         * tcsetattr()) before ever writing to it. Clobbering that here
         * would silently discard it. */
        tty->active = true;
        tty->input_head = 0;
        tty->input_tail = 0;
        tty->edit_cursor = 0;
        tty->line_start = 0;
        tty->output_head = 0;
        tty->output_tail = 0;
        tty->current_attr = 0x07; /* Default white on black */

        /* Set default terminal size */
        tty->winsize.rows = 25;
        tty->winsize.cols = 80;
        tty->winsize.xpixel = 640;
        tty->winsize.ypixel = 400;
        g_tty_output_col[tty_num] = 0;
    }

    /* Background write to controlling terminal: SIGTTOU if TOSTOP is set.
     * See the matching SIGTTIN block in ttyN_read() for the full POSIX
     * blocked/ignored/orphaned-pgrp EIO-fallback and real-stop rationale;
     * the same logic applies here for SIGTTOU. This restriction only
     * applies to a task's controlling terminal (POSIX) - see the matching
     * tty_fd check in ttyN_read(). */
    if (tty->fg_pgid != 0 && current_task && current_task->tty_fd == (int32)tty_num &&
        current_task->pgrp != tty->fg_pgid && (tty->termios.c_lflag & TOSTOP)) {
        if (task_pgrp_is_orphaned(current_task->pgrp, current_task->session)) {
            debuglog(DEBUG_INFO, "TTY%d: Background write from orphaned pgrp %u, failing with EIO\n",
                     tty_num, current_task->pgrp);
            return -EIO;
        }

        debuglog(DEBUG_INFO, "TTY%d: Background write from pgrp %u (fg=%u), sending SIGTTOU\n",
                 tty_num, current_task->pgrp, tty->fg_pgid);
        if (task_send_signal_to_pgrp_checked(current_task->pgrp, SIGTTOU, current_task->id) ==
            SIGNAL_DELIVERY_BLOCKED_OR_IGNORED) {
            return -EIO;
        }

        while (tty->fg_pgid != 0 && current_task->pgrp != tty->fg_pgid) {
            task_yield();
        }
    }

    const char *buf = (const char*)buffer;

    /* Output processing based on termios flags */
    for (size_t i = 0; i < count; i++) {
        char c = buf[i];
        
        /* Output processing (if OPOST is set) */
        if (tty->termios.c_oflag & OPOST) {
            if (c == '\n') {
                if (tty->termios.c_oflag & ONLCR) {
                    /* Translate NL to CR-NL */
                    tty_output_emit_char(tty, '\r');
                    tty_output_emit_char(tty, '\n');
                } else {
                    tty_output_emit_char(tty, '\n');
                }
            } else if (c == '\r') {
                if (tty->termios.c_oflag & OCRNL) {
                    /* Translate CR to NL */
                    tty_output_emit_char(tty, '\n');
                } else if (tty->termios.c_oflag & ONOCR) {
                    /* No CR output at column 0 */
                    if (g_tty_output_col[tty_num] != 0) {
                        tty_output_emit_char(tty, '\r');
                    }
                } else {
                    tty_output_emit_char(tty, '\r');
                }
            } else {
                tty_output_emit_char(tty, c);
            }
        } else {
            /* No output processing */
            tty_output_emit_char(tty, c);
        }
    }

    return count;
}

static int ttyN_ioctl(struct device_node *dev, uint32_t request, void *arg) {
    if (!dev) return DEVICE_ERROR_INVALID_PARAM;

    uint32_t tty_num = dev->minor;
    if (tty_num >= MAX_VIRTUAL_TTYS) return DEVICE_ERROR_INVALID_PARAM;

    virtual_tty_t *tty = &g_virtual_ttys[tty_num];

    switch (request) {
        case DEVICE_IOCTL_GET_INFO: {
            device_info_t *info = (device_info_t*)arg;
            if (info) {
                char name[16];
                string_format(name, sizeof(name), "tty%d", tty_num);
                strncpy(info->name, name, sizeof(info->name) - 1);
                info->size = 0; /* TTYs don't have size */
                info->block_size = 1;
                info->readable = true;
                info->writable = true;
                info->seekable = false;
            }
            return DEVICE_SUCCESS;
        }
        case TIOCGETA: {
            if (!arg) return DEVICE_ERROR_INVALID_PARAM;
            memcpy(arg, &tty->termios, sizeof(termios_t));
            return DEVICE_SUCCESS;
        }
        case TIOCSETA:
        case TIOCSETAW:
        case TIOCSETAF: {
            if (!arg) return DEVICE_ERROR_INVALID_PARAM;
            memcpy(&tty->termios, arg, sizeof(termios_t));
            return DEVICE_SUCCESS;
        }
        case TIOCGWINSZ: {
            if (!arg) return DEVICE_ERROR_INVALID_PARAM;
            memcpy(arg, &tty->winsize, sizeof(winsize_t));
            return DEVICE_SUCCESS;
        }
        case TIOCSWINSZ: {
            if (!arg) return DEVICE_ERROR_INVALID_PARAM;
            winsize_t old_winsize = tty->winsize;
            memcpy(&tty->winsize, arg, sizeof(winsize_t));
            
            /* If size changed, send SIGWINCH to foreground process group */
            if (tty->winsize.rows != old_winsize.rows || tty->winsize.cols != old_winsize.cols) {
                debuglog(DEBUG_INFO, "TTY%d: Window resized from %dx%d to %dx%d, sending SIGWINCH\n", 
                         tty_num, old_winsize.cols, old_winsize.rows, 
                         tty->winsize.cols, tty->winsize.rows);
                task_send_signal(0, SIGWINCH); // Send to current process group
            }
            
            return DEVICE_SUCCESS;
        }
        case TIOCOUTQ: {
            if (!arg) return DEVICE_ERROR_INVALID_PARAM;
            uint32_t* outq = (uint32_t*)arg;
            *outq = tty->output_head - tty->output_tail;
            if (tty->output_head < tty->output_tail) {
                *outq += TTY_BUFFER_SIZE;
            }
            return DEVICE_SUCCESS;
        }
        case TIOCINQ: {
            if (!arg) return DEVICE_ERROR_INVALID_PARAM;
            uint32_t* inq = (uint32_t*)arg;
            *inq = tty->input_head - tty->input_tail;
            if (tty->input_head < tty->input_tail) {
                *inq += TTY_BUFFER_SIZE;
            }
            return DEVICE_SUCCESS;
        }
        default:
            return DEVICE_ERROR_NOT_SUPPORTED;
    }
}

/* Initialize virtual TTY devices */
int tty_devices_init(void) {
    debug_print("TTY: Initializing virtual terminal devices\n");

    /* Initialize device operations */
    ttyN_ops.open = NULL;
    ttyN_ops.close = NULL;
    ttyN_ops.read = ttyN_read;
    ttyN_ops.write = ttyN_write;
    ttyN_ops.ioctl = ttyN_ioctl;
    ttyN_ops.mmap = NULL;
    ttyN_ops.poll = NULL;
    ttyN_ops.flush = NULL;
    ttyN_ops.suspend = NULL;
    ttyN_ops.resume = NULL;
    ttyN_ops.get_info = NULL;
    ttyN_ops.set_config = NULL;

    /* Initialize TTY structures */
    for (uint32_t i = 0; i < MAX_VIRTUAL_TTYS; i++) {
        g_virtual_ttys[i].tty_number = i;
        g_virtual_ttys[i].active = false;
        g_virtual_ttys[i].input_head = 0;
        g_virtual_ttys[i].input_tail = 0;
        g_virtual_ttys[i].edit_cursor = 0;
        g_virtual_ttys[i].line_start = 0;
        g_virtual_ttys[i].output_head = 0;
        g_virtual_ttys[i].output_tail = 0;
        g_virtual_ttys[i].current_attr = 0x07;
        g_virtual_ttys[i].fg_pgid = 0;
        tty_init_termios(&g_virtual_ttys[i].termios);

        /* Set default terminal size */
        g_virtual_ttys[i].winsize.rows = 25;
        g_virtual_ttys[i].winsize.cols = 80;
        g_virtual_ttys[i].winsize.xpixel = 640;
        g_virtual_ttys[i].winsize.ypixel = 400;
        g_tty_output_col[i] = 0;
    }

    /* Register virtual TTY devices */
    /* tty0 is special - it's the current console */
    for (uint32_t i = 0; i < MAX_VIRTUAL_TTYS; i++) {
        char name[16];
        string_format(name, sizeof(name), "tty%d", i);

        device_params_t params = {
            .name = name,
            .major = 4, /* TTY_MAJOR */
            .minor = i,
            .type = DT_CHR,
            .mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH, /* rw-rw-rw- */
            .uid = 0,
            .gid = 0,
            .ops = &ttyN_ops,
            .private_data = &g_virtual_ttys[i]
        };

        if (device_register(&params) != 0) {
            debug_print("TTY: Failed to register %s\n", name);
        }
    }

    /* Register /dev/console as alias to /dev/tty0 */
    device_params_t console_params = {
        .name = "console",
        .major = 5, /* CONSOLE_MAJOR */
        .minor = 1,
        .type = DT_CHR,
        .mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH, /* rw-rw-rw- */
        .uid = 0,
        .gid = 0,
        .ops = &ttyN_ops,
        .private_data = &g_virtual_ttys[0]
    };

    if (device_register(&console_params) != 0) {
        debug_print("TTY: Failed to register console\n");
    }

    /* Register keyboard event callback */
    ps2_keyboard_register_event_callback(tty_keyboard_event_callback);
    
    debug_print("TTY: Virtual terminal devices initialized\n");
    return 0;
}

/* Keyboard event callback for TTY devices */
static void tty_keyboard_event_callback(const keyboard_event_t* event) {
    if (!event) {
        return;
    }

    /* Get current active TTY number */
    uint8_t tty_num = tty_get_current_vt() - 1; /* VT numbers are 1-12, TTY numbers are 0-11 */
    
    /* Handle keyboard event for active TTY. REPEAT (typematic auto-repeat
     * while a key is held) counts as "pressed" here too, same as an initial
     * press -- only an actual key-up should be a no-op. */
    ttyN_handle_keyboard_event(tty_num, event->key_code,
                                event->state == KEY_STATE_PRESSED || event->state == KEY_STATE_REPEAT);
}

/* Handle keyboard events for TTY */
void tty_handle_keyboard_event(uint8_t keycode, bool pressed) {
    /* Get current active TTY number */
    uint8_t tty_num = tty_get_current_vt() - 1; /* VT numbers are 1-12, TTY numbers are 0-11 */
    
    ttyN_handle_keyboard_event(tty_num, keycode, pressed);
}

/* Cleanup virtual TTY devices */
void tty_devices_cleanup(void) {
    debug_print("TTY: Cleaning up virtual terminal devices\n");
    /* Devices are automatically cleaned up by device_fs_cleanup() */
}
