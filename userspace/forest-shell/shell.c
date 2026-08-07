/*
 * forest-shell.c - Forest OS primary interactive shell
 *
 * A POSIX-compatible shell with builtins, job control, piping,
 * redirection, globbing, variable expansion, and line editing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

/* Forest OS libc lacks sig_atomic_t */
#ifndef _SIG_ATOMIC_T
#define _SIG_ATOMIC_T
typedef int sig_atomic_t;
#endif

#include <dirent.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <pwd.h>
#include <time.h>
#include <sys/utsname.h>

/* ---------------------------------------------------------------------------
 * Limits
 * ---------------------------------------------------------------------------*/
#define MAX_LINE       4096
#define MAX_ARGS       256
#define MAX_HISTORY    100
#define MAX_ALIASES    128
#define MAX_JOBS       64
#define MAX_GLOBS      512
#define MAX_PATH_LEN   1024

/* ---------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------------*/
struct job;
static void  shell_init(void);
static void  shell_loop(void);
static char *read_line(void);
static void  add_history(const char *line);
static void  execute_line(char *line);
static char *expand_cmdsub(const char *input);
static char *get_var_value(const char *name);
static void  handle_sigint(int sig);
static void  handle_sigquit(int sig);
static void  handle_sigtstp(int sig);
static void  handle_sigchld(int sig);

/* ---------------------------------------------------------------------------
 * Global state
 * ---------------------------------------------------------------------------*/
static char  g_linebuf[MAX_LINE];
static int   g_linepos = 0;
static int   g_linelen = 0;
static int   g_interactive = 1;
static int   g_running = 1;
static int   g_last_status = 0;
static pid_t g_shell_pgid;

static char *g_history[MAX_HISTORY];
static int   g_history_count = 0;
static int   g_history_idx = -1;

static char *g_aliases_name[MAX_ALIASES];
static char *g_aliases_val[MAX_ALIASES];
static int   g_alias_count = 0;

struct job {
    pid_t   pid;
    int     job_id;
    char    cmd[MAX_LINE];
    int     running;
    int     stopped;
};

static struct job g_jobs[MAX_JOBS];
static int        g_job_count = 0;
static int        g_next_job_id = 1;

/* ---------------------------------------------------------------------------
 * Signal handling
 * ---------------------------------------------------------------------------*/
static volatile sig_atomic_t g_sigint_received = 0;
static volatile sig_atomic_t g_sigchld_received = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_sigint_received = 1;
}

static void handle_sigquit(int sig) {
    (void)sig;
}

static void handle_sigtstp(int sig) {
    (void)sig;
}

static void handle_sigchld(int sig) {
    (void)sig;
    g_sigchld_received = 1;
}

static void install_signal_handlers(void) {
    struct sigaction sa;

    /* SIGINT - Ctrl+C: ignore in shell, handled in child */
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGINT, &sa, NULL);

    /* SIGQUIT - Ctrl+\: ignore */
    sa.sa_handler = handle_sigquit;
    sigaction(SIGQUIT, &sa, NULL);

    /* SIGTSTP - Ctrl+Z: ignore in shell */
    sa.sa_handler = handle_sigtstp;
    sigaction(SIGTSTP, &sa, NULL);

    /* SIGCHLD - child exit: reap children */
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    /* SIGTTIN/SIGTTOU - ignore to prevent background read/write to terminal */
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
}

/* ---------------------------------------------------------------------------
 * Utility: string helpers
 * ---------------------------------------------------------------------------*/
static int streq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    char *r = strdup(s);
    if (!r) {
        fprintf(stderr, "forest-shell: out of memory\n");
        exit(1);
    }
    return r;
}

static char *xmalloc(size_t n) {
    char *p = malloc(n);
    if (!p && n > 0) {
        fprintf(stderr, "forest-shell: out of memory\n");
        exit(1);
    }
    return p;
}

/* ---------------------------------------------------------------------------
 * Prompt generation
 * ---------------------------------------------------------------------------*/
static void print_prompt(void) {
    if (!g_interactive) return;

    const char *user = getenv("USER");
    if (!user || !*user) {
        if (getuid() == 0)
            user = "root";
        else
            user = "user";
    }

    char host[256] = "";
    const char *env_host = getenv("HOSTNAME");
    if (env_host && *env_host) {
        strncpy(host, env_host, sizeof(host) - 1);
    } else if (gethostname(host, sizeof(host)) != 0) {
        strcpy(host, "forest");
    }

    char *cwd = getcwd(NULL, 0);
    if (!cwd) cwd = ".";

    /* Abbreviate home directory to ~ */
    char *home = getenv("HOME");
    char *display_path = cwd;
    char abbreviated[MAX_PATH_LEN] = "";
    if (home && strncmp(cwd, home, strlen(home)) == 0) {
        snprintf(abbreviated, sizeof(abbreviated), "~%s", cwd + strlen(home));
        display_path = abbreviated;
    }

    /* Green user@host, blue path, reset $ */
    printf("\033[1;32m%s@%s\033[0m:\033[1;34m%s\033[0m$ ", user, host, display_path);
    fflush(stdout);

    free(cwd);
}

/* ---------------------------------------------------------------------------
 * History
 * ---------------------------------------------------------------------------*/
static void add_history(const char *line) {
    if (!line || !*line) return;
    /* Don't add duplicate of last entry */
    if (g_history_count > 0 && streq(g_history[g_history_count - 1], line))
        return;

    if (g_history_count < MAX_HISTORY) {
        g_history[g_history_count++] = xstrdup(line);
    } else {
        /* Shift out oldest */
        free(g_history[0]);
        for (int i = 0; i < MAX_HISTORY - 1; i++)
            g_history[i] = g_history[i + 1];
        g_history[MAX_HISTORY - 1] = xstrdup(line);
    }
    g_history_idx = g_history_count;
}

static void show_history(void) {
    for (int i = 0; i < g_history_count; i++)
        printf("%4d  %s\n", i + 1, g_history[i]);
}

/* ---------------------------------------------------------------------------
 * Aliases
 * ---------------------------------------------------------------------------*/
static void alias_set(const char *name, const char *value) {
    for (int i = 0; i < g_alias_count; i++) {
        if (streq(g_aliases_name[i], name)) {
            free(g_aliases_val[i]);
            g_aliases_val[i] = xstrdup(value);
            return;
        }
    }
    if (g_alias_count >= MAX_ALIASES) {
        fprintf(stderr, "forest-shell: too many aliases\n");
        return;
    }
    g_aliases_name[g_alias_count] = xstrdup(name);
    g_aliases_val[g_alias_count] = xstrdup(value);
    g_alias_count++;
}

static void alias_unset(const char *name) {
    for (int i = 0; i < g_alias_count; i++) {
        if (streq(g_aliases_name[i], name)) {
            free(g_aliases_name[i]);
            free(g_aliases_val[i]);
            g_aliases_name[i] = g_aliases_name[g_alias_count - 1];
            g_aliases_val[i] = g_aliases_val[g_alias_count - 1];
            g_alias_count--;
            return;
        }
    }
}

static const char *alias_lookup(const char *name) {
    for (int i = 0; i < g_alias_count; i++)
        if (streq(g_aliases_name[i], name))
            return g_aliases_val[i];
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Job tracking
 * ---------------------------------------------------------------------------*/
static int job_add(pid_t pid, const char *cmd) {
    if (g_job_count >= MAX_JOBS) {
        fprintf(stderr, "forest-shell: too many jobs\n");
        return -1;
    }
    struct job *j = &g_jobs[g_job_count++];
    j->pid = pid;
    j->job_id = g_next_job_id++;
    j->cmd[0] = '\0';
    strncpy(j->cmd, cmd, MAX_LINE - 1);
    j->cmd[MAX_LINE - 1] = '\0';
    j->running = 1;
    j->stopped = 0;
    return j->job_id;
}

static void job_remove(int idx) {
    if (idx < 0 || idx >= g_job_count) return;
    /* Shift remaining */
    for (int i = idx; i < g_job_count - 1; i++)
        g_jobs[i] = g_jobs[i + 1];
    g_job_count--;
}

static struct job *job_find(int job_id) {
    for (int i = 0; i < g_job_count; i++)
        if (g_jobs[i].job_id == job_id)
            return &g_jobs[i];
    return NULL;
}

static void job_reap(void) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        for (int i = 0; i < g_job_count; i++) {
            if (g_jobs[i].pid == pid) {
                if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    if (WIFSIGNALED(status)) {
                        fprintf(stderr, "\n[%d]  Terminated  %s\n",
                                g_jobs[i].job_id, g_jobs[i].cmd);
                    }
                    job_remove(i);
                    i--;
                } else if (WIFSTOPPED(status)) {
                    g_jobs[i].running = 0;
                    g_jobs[i].stopped = 1;
                    fprintf(stderr, "\n[%d]  Stopped  %s\n",
                            g_jobs[i].job_id, g_jobs[i].cmd);
                } else if (WIFCONTINUED(status)) {
                    g_jobs[i].running = 1;
                    g_jobs[i].stopped = 0;
                }
                break;
            }
        }
    }
}

static void show_jobs(void) {
    job_reap();
    for (int i = 0; i < g_job_count; i++) {
        const char *state = g_jobs[i].stopped ? "Stopped" : "Running";
        printf("[%d]  %s  %s\n", g_jobs[i].job_id, state, g_jobs[i].cmd);
    }
}

/* ---------------------------------------------------------------------------
 * Line editing (character-by-character from stdin)
 * ---------------------------------------------------------------------------*/
static int read_char(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) == 1)
        return c;
    return -1;
}

/* Read a line with editing support. Returns line or NULL on EOF. */
static char *read_line_interactive(void) {
    g_linepos = 0;
    g_linelen = 0;
    g_history_idx = g_history_count;
    memset(g_linebuf, 0, MAX_LINE);

    while (1) {
        int c = read_char();

        if (c == -1 || c == 4) { /* EOF / Ctrl+D */
            if (g_linepos == 0) {
                printf("\n");
                return NULL;
            }
            continue;
        }

        if (c == '\n' || c == '\r') {
            g_linebuf[g_linepos] = '\0';
            printf("\n");
            return xstrdup(g_linebuf);
        }

        if (c == 127 || c == 8) { /* Backspace / Delete */
            if (g_linepos > 0) {
                g_linepos--;
                g_linebuf[g_linepos] = '\0';
                /* Move cursor back, clear, move back */
                printf("\b \b");
            }
            continue;
        }

        if (c == 27) { /* Escape sequence */
            int c2 = read_char();
            int c3 = read_char();
            if (c2 == '[') {
                switch (c3) {
                case 'A': /* Up arrow - previous history */
                    if (g_history_idx > 0) {
                        g_history_idx--;
                        /* Clear current line */
                        while (g_linepos > 0) {
                            printf("\b \b");
                            g_linepos--;
                        }
                        strncpy(g_linebuf, g_history[g_history_idx], MAX_LINE - 1);
                        g_linepos = strlen(g_linebuf);
                        g_linebuf[g_linepos] = '\0';
                        printf("%s", g_linebuf);
                    }
                    break;
                case 'B': /* Down arrow - next history */
                    if (g_history_idx < g_history_count - 1) {
                        g_history_idx++;
                        while (g_linepos > 0) {
                            printf("\b \b");
                            g_linepos--;
                        }
                        strncpy(g_linebuf, g_history[g_history_idx], MAX_LINE - 1);
                        g_linepos = strlen(g_linebuf);
                        g_linebuf[g_linepos] = '\0';
                        printf("%s", g_linebuf);
                    } else if (g_history_idx == g_history_count - 1) {
                        g_history_idx = g_history_count;
                        while (g_linepos > 0) {
                            printf("\b \b");
                            g_linepos--;
                        }
                        g_linebuf[0] = '\0';
                    }
                    break;
                case 'C': /* Right arrow */
                    if (g_linepos < g_linelen) {
                        printf("%c", g_linebuf[g_linepos]);
                        g_linepos++;
                    }
                    break;
                case 'D': /* Left arrow */
                    if (g_linepos > 0) {
                        printf("\b");
                        g_linepos--;
                    }
                    break;
                }
            }
            continue;
        }

        /* Regular character */
        if (g_linepos < MAX_LINE - 1) {
            g_linebuf[g_linepos++] = (char)c;
            g_linebuf[g_linepos] = '\0';
            g_linelen = g_linepos;
            putchar(c);
        }
    }
}

/* Read a line from stdin (batch mode - just fgets) */
static char *read_line_batch(void) {
    if (!fgets(g_linebuf, MAX_LINE, stdin))
        return NULL;
    g_linelen = strlen(g_linebuf);
    if (g_linelen > 0 && g_linebuf[g_linelen - 1] == '\n') {
        g_linebuf[--g_linelen] = '\0';
    }
    return xstrdup(g_linebuf);
}

static char *read_line(void) {
    if (g_interactive)
        return read_line_interactive();
    else
        return read_line_batch();
}

/* ---------------------------------------------------------------------------
 * Word splitting and tokenization
 * ---------------------------------------------------------------------------*/
struct token {
    char *words[MAX_ARGS];
    int   count;
    int   background;
    int   single_quoted[MAX_ARGS]; /* 1 if token was in single quotes */
};

static void token_free(struct token *t) {
    for (int i = 0; i < t->count; i++)
        free(t->words[i]);
    t->count = 0;
    t->background = 0;
}

/* Skip whitespace, return position of next non-space */
static const char *skip_space(const char *s) {
    while (*s && (*s == ' ' || *s == '\t'))
        s++;
    return s;
}

/*
 * Tokenize a command line into words, handling:
 *  - single quotes (no expansion)
 *  - double quotes (variable expansion)
 *  - backslash escaping
 *  - & at end for background
 *  - |, >, >>, <, 2>, 2>>, &> are separate tokens
 */
static int tokenize(const char *input, struct token *t) {
    t->count = 0;
    t->background = 0;
    memset(t->single_quoted, 0, sizeof(t->single_quoted));
    const char *p = input;

    while (*p && t->count < MAX_ARGS - 1) {
        p = skip_space(p);
        if (!*p) break;

        /* Check for operator tokens */
        if (*p == '|' && *(p+1) != '|') {
            t->words[t->count++] = xstrdup("|");
            p++;
            continue;
        }
        if (*p == '>' && *(p+1) == '>') {
            t->words[t->count++] = xstrdup(">>");
            p += 2;
            continue;
        }
        if (*p == '>' && *(p+1) != '>') {
            /* Check for &> */
            if (p == input || *(p-1) == '&') {
                /* already handled as part of &> */
            }
            t->words[t->count++] = xstrdup(">");
            p++;
            continue;
        }
        if (*p == '<') {
            t->words[t->count++] = xstrdup("<");
            p++;
            continue;
        }
        if (*p == '2' && *(p+1) == '>') {
            if (*(p+2) == '>') {
                t->words[t->count++] = xstrdup("2>>");
                p += 3;
            } else {
                t->words[t->count++] = xstrdup("2>");
                p += 2;
            }
            continue;
        }
        if (*p == '&' && *(p+1) == '>') {
            t->words[t->count++] = xstrdup("&>");
            p += 2;
            continue;
        }

        /* Quoted or unquoted word */
        char word[MAX_LINE];
        int wlen = 0;
        int has_single = 0; /* 1 if any part was in single quotes (skip expansion) */

        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '`') {
                /* Command substitution: copy backticks and content including spaces */
                if (wlen < MAX_LINE - 1) word[wlen++] = *p;
                p++;
                while (*p && *p != '`') {
                    if (wlen < MAX_LINE - 1)
                        word[wlen++] = *p;
                    p++;
                }
                if (*p == '`') {
                    if (wlen < MAX_LINE - 1) word[wlen++] = *p;
                    p++;
                }
                continue;
            }
            if (*p == '\'' ) {
                has_single = 1;
                p++;
                while (*p && *p != '\'') {
                    if (wlen < MAX_LINE - 1)
                        word[wlen++] = *p;
                    p++;
                }
                if (*p == '\'') p++;
                continue;
            }
            if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && *(p+1)) {
                        p++;
                        if (wlen < MAX_LINE - 1)
                            word[wlen++] = *p;
                        p++;
                    } else if (*p == '$') {
                        /* Expand variables inside double quotes */
                        p++;
                        char vname[256] = "";
                        int vlen = 0;
                        int special = 0;

                        if (*p == '?') {
                            p++;
                            vlen = snprintf(vname, sizeof(vname), "%d", g_last_status);
                            special = 1;
                        } else if (*p == '#') {
                            p++;
                            vlen = snprintf(vname, sizeof(vname), "%d", 0);
                            special = 1;
                        } else if (*p == '0') {
                            p++;
                            vlen = snprintf(vname, sizeof(vname), "forest-shell");
                            special = 1;
                        } else if (*p == '$') {
                            p++;
                            vlen = snprintf(vname, sizeof(vname), "%d", getpid());
                            special = 1;
                        } else if (*p == '{') {
                            p++;
                            while (*p && *p != '}' && vlen < (int)sizeof(vname) - 1)
                                vname[vlen++] = *p++;
                            if (*p == '}') p++;
                        } else if (isalpha(*p) || *p == '_') {
                            while ((isalnum(*p) || *p == '_') && vlen < (int)sizeof(vname) - 1)
                                vname[vlen++] = *p++;
                        } else {
                            if (wlen < MAX_LINE - 1) word[wlen++] = '$';
                            continue;
                        }
                        vname[vlen] = '\0';
                        const char *val = special ? vname : get_var_value(vname);
                        if (val) {
                            size_t vallen = strlen(val);
                            if (wlen + (int)vlen < MAX_LINE - 1) {
                                memcpy(word + wlen, val, vallen);
                                wlen += vallen;
                            }
                        }
                    } else {
                        if (wlen < MAX_LINE - 1)
                            word[wlen++] = *p;
                        p++;
                    }
                }
                if (*p == '"') p++;
                continue;
            }
            if (*p == '\\') {
                p++;
                if (*p && wlen < MAX_LINE - 1)
                    word[wlen++] = *p;
                if (*p) p++;
                continue;
            }
            if (*p == '#') break;
            if (wlen < MAX_LINE - 1)
                word[wlen++] = *p;
            p++;
        }
        word[wlen] = '\0';

        /* Check if this is "&" - background operator */
        if (wlen == 1 && word[0] == '&') {
            t->words[t->count] = xstrdup("&");
            t->single_quoted[t->count] = 0;
            t->count++;
            t->background = 1;
            continue;
        }

        t->words[t->count] = xstrdup(word);
        t->single_quoted[t->count] = has_single;
        t->count++;
    }

    return t->count;
}

/* ---------------------------------------------------------------------------
 * Variable expansion
 * ---------------------------------------------------------------------------*/
static char *get_var_value(const char *name) {
    return getenv(name);
}

/*
 * Expand variables in a string.
 * Supports: $VAR, ${VAR}, $?, $#, $0, $@, $*
 * Returns newly allocated string.
 */
static char *expand_vars(const char *input, int argc, char **argv) {
    if (!input) return NULL;

    size_t outlen = strlen(input) * 4 + 256;
    char *out = xmalloc(outlen);
    size_t opos = 0;

    const char *p = input;
    while (*p && opos < outlen - 1) {
        if (*p == '$' && *(p+1)) {
            p++;
            char varname[256] = "";
            int vlen = 0;
            int is_special = 0; /* 1 = varname holds value directly, not a name */

            if (*p == '?') {
                p++;
                snprintf(varname, sizeof(varname), "%d", g_last_status);
                vlen = strlen(varname);
                is_special = 1;
            } else if (*p == '#') {
                p++;
                snprintf(varname, sizeof(varname), "%d", argc);
                vlen = strlen(varname);
                is_special = 1;
            } else if (*p == '0') {
                p++;
                snprintf(varname, sizeof(varname), "forest-shell");
                vlen = strlen(varname);
                is_special = 1;
            } else if (*p == '@' || *p == '*') {
                p++;
                varname[0] = '\0';
                for (int i = 1; i < argc; i++) {
                    if (i > 1 && vlen < (int)sizeof(varname) - 1)
                        varname[vlen++] = ' ';
                    int slen = strlen(argv[i]);
                    if (vlen + slen < (int)sizeof(varname) - 1) {
                        memcpy(varname + vlen, argv[i], slen);
                        vlen += slen;
                    }
                }
                is_special = 1;
            } else if (*p == '{') {
                p++;
                while (*p && *p != '}' && vlen < (int)sizeof(varname) - 1)
                    varname[vlen++] = *p++;
                if (*p == '}') p++;
            } else if (isalpha(*p) || *p == '_') {
                while ((isalnum(*p) || *p == '_') && vlen < (int)sizeof(varname) - 1)
                    varname[vlen++] = *p++;
            } else if (*p == '$') {
                p++;
                snprintf(varname, sizeof(varname), "%d", getpid());
                vlen = strlen(varname);
                is_special = 1;
            } else if (*p == '!') {
                p++;
                snprintf(varname, sizeof(varname), "%d", (int)getpgrp());
                vlen = strlen(varname);
                is_special = 1;
            } else {
                if (opos < outlen - 2) out[opos++] = '$';
                if (opos < outlen - 2) out[opos++] = *p;
                p++;
                continue;
            }

            varname[vlen] = '\0';

            const char *val = is_special ? varname : get_var_value(varname);
            if (val) {
                size_t vallen = strlen(val);
                if (opos + vallen < outlen) {
                    memcpy(out + opos, val, vallen);
                    opos += vallen;
                }
            }
        } else {
            out[opos++] = *p;
            p++;
        }
    }
    out[opos] = '\0';
    return out;
}

/* ---------------------------------------------------------------------------
 * Glob expansion
 * ---------------------------------------------------------------------------*/
static char *g_glob_buf[MAX_GLOBS];
static int   g_glob_count = 0;

/*
 * Match a pattern against a filename.
 * Supports: * (any), ? (single char), [abc] (character class).
 */
static int glob_match(const char *pattern, const char *name) {
    while (*pattern && *name) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;
            while (*name) {
                if (glob_match(pattern, name))
                    return 1;
                name++;
            }
            return glob_match(pattern, name);
        }
        if (*pattern == '?') {
            pattern++;
            name++;
            continue;
        }
        if (*pattern == '[') {
            pattern++;
            int negate = 0;
            if (*pattern == '^' || *pattern == '!') {
                negate = 1;
                pattern++;
            }
            int match = 0;
            while (*pattern && *pattern != ']') {
                if (*(pattern + 1) == '-' && *(pattern + 2) != ']') {
                    if (*name >= *pattern && *name <= *(pattern + 2))
                        match = 1;
                    pattern += 3;
                } else {
                    if (*name == *pattern)
                        match = 1;
                    pattern++;
                }
            }
            if (*pattern == ']') pattern++;
            if (negate) match = !match;
            if (!match) return 0;
            name++;
            continue;
        }
        if (*pattern == *name) {
            pattern++;
            name++;
            continue;
        }
        return 0;
    }
    return (*pattern == '\0' && *name == '\0');
}

static void glob_add(const char *path) {
    if (g_glob_count >= MAX_GLOBS) return;
    g_glob_buf[g_glob_count++] = xstrdup(path);
}

static void glob_free(void) {
    for (int i = 0; i < g_glob_count; i++)
        free(g_glob_buf[i]);
    g_glob_count = 0;
}

/*
 * Expand glob patterns in an argument list.
 * Returns new count and fills new_args with expanded results.
 * Original args are freed.
 */
static int expand_glob(char **args, int argc, char **new_args, int max_new) {
    int newc = 0;

    for (int i = 0; i < argc && newc < max_new; i++) {
        const char *arg = args[i];

        /* Check if arg contains glob chars */
        int has_glob = 0;
        for (const char *p = arg; *p; p++) {
            if (*p == '*' || *p == '?' || *p == '[') {
                has_glob = 1;
                break;
            }
        }

        if (!has_glob) {
            new_args[newc++] = xstrdup(arg);
            continue;
        }

        /* Split into directory and pattern */
        char dir[MAX_PATH_LEN] = ".";
        char pattern[MAX_PATH_LEN];
        strncpy(pattern, arg, sizeof(pattern) - 1);
        pattern[sizeof(pattern) - 1] = '\0';

        const char *last_slash = strrchr(arg, '/');
        if (last_slash) {
            size_t dlen = last_slash - arg;
            if (dlen == 0) dlen = 1;
            if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
            memcpy(dir, arg, dlen);
            dir[dlen] = '\0';
            strncpy(pattern, last_slash + 1, sizeof(pattern) - 1);
        }

        DIR *d = opendir(dir);
        if (!d) {
            new_args[newc++] = xstrdup(arg);
            continue;
        }

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                    continue;
                if (pattern[0] != '.')
                    continue;
            }
            if (glob_match(pattern, ent->d_name)) {
                char full[MAX_PATH_LEN];
                if (streq(dir, "."))
                    snprintf(full, sizeof(full), "%s", ent->d_name);
                else
                    snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
                glob_add(full);
            }
        }
        closedir(d);

        if (g_glob_count == 0) {
            new_args[newc++] = xstrdup(arg);
        } else {
            /* Sort glob results */
            for (int a = 0; a < g_glob_count - 1; a++)
                for (int b = a + 1; b < g_glob_count; b++)
                    if (strcmp(g_glob_buf[a], g_glob_buf[b]) > 0) {
                        char *tmp = g_glob_buf[a];
                        g_glob_buf[a] = g_glob_buf[b];
                        g_glob_buf[b] = tmp;
                    }
            for (int g = 0; g < g_glob_count && newc < max_new; g++)
                new_args[newc++] = xstrdup(g_glob_buf[g]);
            glob_free();
        }
    }

    return newc;
}

/* ---------------------------------------------------------------------------
 * PATH lookup
 * ---------------------------------------------------------------------------*/
static char *find_command(const char *cmd) {
    /* If cmd contains /, it's a path */
    if (strchr(cmd, '/')) {
        if (access(cmd, X_OK) == 0)
            return xstrdup(cmd);
        return NULL;
    }

    /* Check builtins - these don't need a path */
    static const char *builtins[] = {
        "cd", "exit", "export", "unset", "env", "set", "pwd", "echo",
        "type", "which", "history", "source", ".", "alias", "unalias",
        "jobs", "fg", "bg", "wait",
        "help", "clear", "mount", "umount", "reboot", "halt", "poweroff",
        "hostname", "dmesg", "login",
        NULL
    };
    for (int i = 0; builtins[i]; i++)
        if (streq(cmd, builtins[i]))
            return NULL; /* builtin, no path needed */

    char *path = getenv("PATH");
    if (!path) path = "/bin:/usr/bin:/usr/local/bin";

    char *pathcopy = xstrdup(path);
    char *saveptr = NULL;
    char *dir = strtok_r(pathcopy, ":", &saveptr);

    while (dir) {
        char full[MAX_PATH_LEN];
        snprintf(full, sizeof(full), "%s/%s", dir, cmd);
        if (access(full, X_OK) == 0) {
            free(pathcopy);
            return xstrdup(full);
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }

    free(pathcopy);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Builtin: cd
 * ---------------------------------------------------------------------------*/
static int builtin_cd(int argc, char **argv) {
    const char *target;

    if (argc < 2 || streq(argv[1], "~")) {
        target = getenv("HOME");
        if (!target) target = "/";
    } else if (streq(argv[1], "-")) {
        target = getenv("OLDPWD");
        if (!target) {
            fprintf(stderr, "forest-shell: OLDPWD not set\n");
            return 1;
        }
        printf("%s\n", target);
    } else {
        target = argv[1];
    }

    char oldcwd[MAX_PATH_LEN];
    if (getcwd(oldcwd, sizeof(oldcwd)) == NULL)
        oldcwd[0] = '\0';

    if (chdir(target) != 0) {
        fprintf(stderr, "forest-shell: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }

    if (oldcwd[0]) {
        setenv("OLDPWD", oldcwd, 1);
    }

    char *newcwd = getcwd(NULL, 0);
    if (newcwd) {
        setenv("PWD", newcwd, 1);
        free(newcwd);
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Builtin: export
 * ---------------------------------------------------------------------------*/
static int builtin_export(int argc, char **argv) {
    if (argc < 2) {
        /* Print all exported vars */
        extern char **environ;
        for (char **e = environ; *e; e++)
            printf("declare -x %s\n", *e);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = '\0';
            setenv(argv[i], eq + 1, 1);
            *eq = '='; /* restore for potential error messages */
        } else {
            /* Just mark as exported (no-op in our implementation since all
               setenv vars are in environ) */
            (void)0;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Builtin: unset
 * ---------------------------------------------------------------------------*/
static int builtin_unset(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        unsetenv(argv[i]);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Builtin: env
 * ---------------------------------------------------------------------------*/
static int builtin_env(int argc, char **argv) {
    (void)argc; (void)argv;
    extern char **environ;
    for (char **e = environ; *e; e++)
        printf("%s\n", *e);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Builtin: set
 * ---------------------------------------------------------------------------*/
static int builtin_set(int argc, char **argv) {
    (void)argc; (void)argv;
    extern char **environ;
    for (char **e = environ; *e; e++)
        printf("%s\n", *e);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Builtin: pwd
 * ---------------------------------------------------------------------------*/
static int builtin_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char *cwd = getcwd(NULL, 0);
    if (cwd) {
        printf("%s\n", cwd);
        free(cwd);
        return 0;
    }
    fprintf(stderr, "forest-shell: pwd: %s\n", strerror(errno));
    return 1;
}

/* ---------------------------------------------------------------------------
 * Builtin: echo
 * ---------------------------------------------------------------------------*/
static int builtin_echo(int argc, char **argv) {
    int no_newline = 0;
    int escape = 0;
    int start = 1;

    /* Parse flags */
    while (start < argc && argv[start][0] == '-') {
        if (streq(argv[start], "--")) {
            start++;
            break;
        }
        int valid = 1;
        for (const char *p = argv[start] + 1; *p; p++) {
            if (*p == 'n') no_newline = 1;
            else if (*p == 'e') escape = 1;
            else if (*p == 'E') escape = 0;
            else { valid = 0; break; }
        }
        if (!valid) break;
        start++;
    }

    for (int i = start; i < argc; i++) {
        if (i > start) putchar(' ');
        const char *s = argv[i];
        while (*s) {
            if (*s == '\\' && escape) {
                s++;
                switch (*s) {
                case 'a':  putchar('\a'); break;
                case 'b':  putchar('\b'); break;
                case 'e':  putchar('\033'); break;
                case 'f':  putchar('\f'); break;
                case 'n':  putchar('\n'); break;
                case 'r':  putchar('\r'); break;
                case 't':  putchar('\t'); break;
                case 'v':  putchar('\v'); break;
                case '\\': putchar('\\'); break;
                case '0': {
                    int val = 0;
                    s++;
                    int digits = 0;
                    while (digits < 3 && *s >= '0' && *s <= '7') {
                        val = val * 8 + (*s - '0');
                        s++;
                        digits++;
                    }
                    if (digits > 0) putchar(val);
                    s--;
                    break;
                }
                default:
                    putchar('\\');
                    putchar(*s);
                    break;
                }
            } else {
                putchar(*s);
            }
            s++;
        }
    }

    if (!no_newline)
        putchar('\n');
    return 0;
}

/* ---------------------------------------------------------------------------
 * Builtin: type
 * ---------------------------------------------------------------------------*/
static int builtin_type(int argc, char **argv) {
    static const char *builtin_names[] = {
        "cd", "exit", "export", "unset", "env", "set", "pwd", "echo",
        "type", "which", "history", "source", ".", "alias", "unalias",
        "jobs", "fg", "bg", "wait",
        "help", "clear", "mount", "umount", "reboot", "halt", "poweroff",
        "hostname", "dmesg", "login",
        NULL
    };

    for (int i = 1; i < argc; i++) {
        int is_builtin = 0;
        for (int j = 0; builtin_names[j]; j++) {
            if (streq(argv[i], builtin_names[j])) {
                is_builtin = 1;
                break;
            }
        }
        if (is_builtin) {
            printf("%s is a shell builtin\n", argv[i]);
            continue;
        }
        const char *al = alias_lookup(argv[i]);
        if (al) {
            printf("%s is aliased to `%s'\n", argv[i], al);
            continue;
        }
        char *path = find_command(argv[i]);
        if (path) {
            printf("%s is %s\n", argv[i], path);
            free(path);
        } else {
            fprintf(stderr, "forest-shell: type: %s: not found\n", argv[i]);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Builtin: which
 * ---------------------------------------------------------------------------*/
static int builtin_which(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char *path = find_command(argv[i]);
        if (path) {
            printf("%s\n", path);
            free(path);
        } else {
            fprintf(stderr, "%s not found\n", argv[i]);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Builtin: source / .
 * ---------------------------------------------------------------------------*/
static int source_file(const char *filename);

static int builtin_source(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "forest-shell: source: filename required\n");
        return 1;
    }
    return source_file(argv[1]);
}

/* ---------------------------------------------------------------------------
 * Builtin: alias / unalias
 * ---------------------------------------------------------------------------*/
static int builtin_alias(int argc, char **argv) {
    if (argc < 2) {
        for (int i = 0; i < g_alias_count; i++)
            printf("alias %s='%s'\n", g_aliases_name[i], g_aliases_val[i]);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = '\0';
            alias_set(argv[i], eq + 1);
            *eq = '=';
        } else {
            const char *val = alias_lookup(argv[i]);
            if (val)
                printf("alias %s='%s'\n", argv[i], val);
            else
                fprintf(stderr, "forest-shell: alias: %s: not found\n", argv[i]);
        }
    }
    return 0;
}

static int builtin_unalias(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "forest-shell: unalias: name required\n");
        return 1;
    }
    for (int i = 1; i < argc; i++)
        alias_unset(argv[i]);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Builtin: jobs, fg, bg, wait
 * ---------------------------------------------------------------------------*/
static int builtin_jobs(int argc, char **argv) {
    (void)argc; (void)argv;
    show_jobs();
    return 0;
}

static int builtin_fg(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "forest-shell: fg: job id required (%%N)\n");
        return 1;
    }

    int job_id = -1;
    const char *spec = argv[1];
    if (*spec == '%') spec++;
    job_id = atoi(spec);

    struct job *j = job_find(job_id);
    if (!j) {
        fprintf(stderr, "forest-shell: fg: %s: no such job\n", argv[1]);
        return 1;
    }

    /* Bring to foreground */
    tcsetpgrp(STDIN_FILENO, j->pid);
    kill(j->pid, SIGCONT);

    int status;
    waitpid(j->pid, &status, WUNTRACED);
    tcsetpgrp(STDIN_FILENO, getpgrp());

    if (WIFEXITED(status))
        g_last_status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        g_last_status = 128 + WTERMSIG(status);
    else if (WIFSTOPPED(status))
        g_last_status = 128 + SIGTSTP;

    return g_last_status;
}

static int builtin_bg(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "forest-shell: bg: job id required (%%N)\n");
        return 1;
    }

    int job_id = -1;
    const char *spec = argv[1];
    if (*spec == '%') spec++;
    job_id = atoi(spec);

    struct job *j = job_find(job_id);
    if (!j) {
        fprintf(stderr, "forest-shell: bg: %s: no such job\n", argv[1]);
        return 1;
    }

    kill(j->pid, SIGCONT);
    j->running = 1;
    j->stopped = 0;
    printf("[%d] %s &\n", j->job_id, j->cmd);
    return 0;
}

static int builtin_wait(int argc, char **argv) {
    (void)argc; (void)argv;
    int status;
    while (waitpid(-1, &status, 0) > 0)
        ;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Builtin: help
 * ---------------------------------------------------------------------------*/
static int builtin_help(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("Forest OS Shell built-in commands:\n\n");
    printf("  cd [dir]        Change directory\n");
    printf("  pwd             Print working directory\n");
    printf("  echo [args]     Print arguments\n");
    printf("  export [var=val] Set/export environment variable\n");
    printf("  unset [var]     Remove environment variable\n");
    printf("  env             Print environment\n");
    printf("  set             Print all shell variables\n");
    printf("  type [cmd]      Show command type\n");
    printf("  which [cmd]     Show command path\n");
    printf("  history         Show command history\n");
    printf("  source [file]   Execute commands from file\n");
    printf("  alias [n=v]     Manage aliases\n");
    printf("  unalias [name]  Remove alias\n");
    printf("  jobs            List active jobs\n");
    printf("  fg [%%N]         Bring job to foreground\n");
    printf("  bg [%%N]         Resume job in background\n");
    printf("  wait            Wait for all background jobs\n");
    printf("  help            Show this help message\n");
    printf("  clear           Clear terminal screen\n");
    printf("  mount [args]    Mount a filesystem\n");
    printf("  umount [args]   Unmount a filesystem\n");
    printf("  reboot          Reboot the system\n");
    printf("  halt            Halt the system\n");
    printf("  poweroff        Power off the system\n");
    printf("  hostname [name] Show or set hostname\n");
    printf("  dmesg           Show kernel messages\n");
    printf("  login [user]    Re-authenticate (for init)\n");
    printf("  exit [code]     Exit the shell\n");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Builtin: clear
 * ---------------------------------------------------------------------------*/
static int builtin_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("\033[2J\033[H");
    fflush(stdout);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Builtin: mount
 * ---------------------------------------------------------------------------*/
static int builtin_mount(int argc, char **argv) {
    /* Try external mount first */
    if (argc > 1) {
        pid_t pid = fork();
        if (pid == 0) {
            setpgid(0, 0);
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            char *cmd = find_command("mount");
            if (cmd) {
                execv(cmd, argv);
                free(cmd);
            }
            fprintf(stderr, "mount: not found\n");
            exit(127);
        }
        if (pid > 0) {
            int st;
            waitpid(pid, &st, 0);
            if (WIFEXITED(st)) return WEXITSTATUS(st);
            return 1;
        }
        return 1;
    }
    /* No args: list mounts - try /proc/mounts or /etc/mtab */
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) f = fopen("/etc/mtab", "r");
    if (!f) {
        fprintf(stderr, "mount: cannot read mount table\n");
        return 1;
    }
    char line[512];
    while (fgets(line, sizeof(line), f))
        printf("%s", line);
    fclose(f);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Builtin: umount
 * ---------------------------------------------------------------------------*/
static int builtin_umount(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "umount: device or mount point required\n");
        return 1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        char *cmd = find_command("umount");
        if (cmd) {
            execv(cmd, argv);
            free(cmd);
        }
        fprintf(stderr, "umount: not found\n");
        exit(127);
    }
    if (pid > 0) {
        int st;
        waitpid(pid, &st, 0);
        if (WIFEXITED(st)) return WEXITSTATUS(st);
        return 1;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Builtin: reboot / halt / poweroff
 * ---------------------------------------------------------------------------*/
static int builtin_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    char *cmd = find_command("reboot");
    if (cmd) {
        execl(cmd, "reboot", NULL);
        free(cmd);
    }
    /* Fallback: try sync + reboot syscall */
    sync();
    fprintf(stderr, "reboot: reboot command not found, attempting sync\n");
    return 0;
}

static int builtin_halt(int argc, char **argv) {
    (void)argc; (void)argv;
    char *cmd = find_command("halt");
    if (cmd) {
        execl(cmd, "halt", NULL);
        free(cmd);
    }
    cmd = find_command("shutdown");
    if (cmd) {
        execl(cmd, "shutdown", "-h", "now", NULL);
        free(cmd);
    }
    fprintf(stderr, "halt: halt/shutdown command not found\n");
    return 1;
}

static int builtin_poweroff(int argc, char **argv) {
    (void)argc; (void)argv;
    char *cmd = find_command("poweroff");
    if (cmd) {
        execl(cmd, "poweroff", NULL);
        free(cmd);
    }
    cmd = find_command("shutdown");
    if (cmd) {
        execl(cmd, "shutdown", "-P", "now", NULL);
        free(cmd);
    }
    fprintf(stderr, "poweroff: poweroff/shutdown command not found\n");
    return 1;
}

/* ---------------------------------------------------------------------------
 * Builtin: hostname
 * ---------------------------------------------------------------------------*/
static int builtin_hostname(int argc, char **argv) {
    if (argc < 2) {
        char host[256] = "";
        if (gethostname(host, sizeof(host)) == 0)
            printf("%s\n", host);
        else
            fprintf(stderr, "hostname: cannot get hostname\n");
        return 0;
    }
    /* Try external hostname command */
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        char *cmd = find_command("hostname");
        if (cmd) {
            execv(cmd, argv);
            free(cmd);
        }
        fprintf(stderr, "hostname: not found\n");
        exit(127);
    }
    if (pid > 0) {
        int st;
        waitpid(pid, &st, 0);
        if (WIFEXITED(st)) return WEXITSTATUS(st);
        return 1;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Builtin: dmesg
 * ---------------------------------------------------------------------------*/
static int builtin_dmesg(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Try /dev/kmsg first, then /proc/kmsg, then external dmesg */
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        fd = open("/proc/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            printf("%s", buf);
        }
        close(fd);
        return 0;
    }
    /* Fallback: try external dmesg */
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        char *cmd = find_command("dmesg");
        if (cmd) {
            execl(cmd, "dmesg", NULL);
            free(cmd);
        }
        fprintf(stderr, "dmesg: not found and /dev/kmsg not available\n");
        exit(127);
    }
    if (pid > 0) {
        int st;
        waitpid(pid, &st, 0);
        if (WIFEXITED(st)) return WEXITSTATUS(st);
        return 1;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Builtin: login
 * ---------------------------------------------------------------------------*/
static int builtin_login(int argc, char **argv) {
    (void)argc; (void)argv;
    char *cmd = find_command("login");
    if (cmd) {
        if (argc > 1)
            execl(cmd, "login", argv[1], NULL);
        else
            execl(cmd, "login", NULL);
        free(cmd);
    }
    fprintf(stderr, "login: login command not found\n");
    return 1;
}

/* ---------------------------------------------------------------------------
 * Dispatch builtin commands
 * ---------------------------------------------------------------------------*/
static int is_builtin(const char *cmd) {
    static const char *builtins[] = {
        "cd", "exit", "export", "unset", "env", "set", "pwd", "echo",
        "type", "which", "history", "source", ".", "alias", "unalias",
        "jobs", "fg", "bg", "wait",
        "help", "clear", "mount", "umount", "reboot", "halt", "poweroff",
        "hostname", "dmesg", "login",
        NULL
    };
    for (int i = 0; builtins[i]; i++)
        if (streq(cmd, builtins[i]))
            return 1;
    return 0;
}

static int run_builtin(int argc, char **argv) {
    const char *cmd = argv[0];

    if (streq(cmd, "cd"))
        return builtin_cd(argc, argv);
    if (streq(cmd, "exit")) {
        int code = 0;
        if (argc > 1) code = atoi(argv[1]);
        exit(code);
    }
    if (streq(cmd, "export"))
        return builtin_export(argc, argv);
    if (streq(cmd, "unset"))
        return builtin_unset(argc, argv);
    if (streq(cmd, "env"))
        return builtin_env(argc, argv);
    if (streq(cmd, "set"))
        return builtin_set(argc, argv);
    if (streq(cmd, "pwd"))
        return builtin_pwd(argc, argv);
    if (streq(cmd, "echo"))
        return builtin_echo(argc, argv);
    if (streq(cmd, "type"))
        return builtin_type(argc, argv);
    if (streq(cmd, "which"))
        return builtin_which(argc, argv);
    if (streq(cmd, "history")) {
        show_history();
        return 0;
    }
    if (streq(cmd, "source") || streq(cmd, "."))
        return builtin_source(argc > 0 ? argc : 1, argv);
    if (streq(cmd, "alias"))
        return builtin_alias(argc, argv);
    if (streq(cmd, "unalias"))
        return builtin_unalias(argc, argv);
    if (streq(cmd, "jobs"))
        return builtin_jobs(argc, argv);
    if (streq(cmd, "fg"))
        return builtin_fg(argc, argv);
    if (streq(cmd, "bg"))
        return builtin_bg(argc, argv);
    if (streq(cmd, "wait"))
        return builtin_wait(argc, argv);
    if (streq(cmd, "help"))
        return builtin_help(argc, argv);
    if (streq(cmd, "clear"))
        return builtin_clear(argc, argv);
    if (streq(cmd, "mount"))
        return builtin_mount(argc, argv);
    if (streq(cmd, "umount"))
        return builtin_umount(argc, argv);
    if (streq(cmd, "reboot"))
        return builtin_reboot(argc, argv);
    if (streq(cmd, "halt"))
        return builtin_halt(argc, argv);
    if (streq(cmd, "poweroff"))
        return builtin_poweroff(argc, argv);
    if (streq(cmd, "hostname"))
        return builtin_hostname(argc, argv);
    if (streq(cmd, "dmesg"))
        return builtin_dmesg(argc, argv);
    if (streq(cmd, "login"))
        return builtin_login(argc, argv);

    fprintf(stderr, "forest-shell: %s: unknown builtin\n", cmd);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Source file execution
 * ---------------------------------------------------------------------------*/
static int source_file(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "forest-shell: source: %s: %s\n",
                filename, strerror(errno));
        return 1;
    }

    char line[MAX_LINE];
    int saved_interactive = g_interactive;
    g_interactive = 0;
    int status = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';
        if (len == 0 || line[0] == '#')
            continue;

        char *dup = xstrdup(line);
        execute_line(dup);
        free(dup);
    }

    fclose(f);
    g_interactive = saved_interactive;
    return status;
}

/* ---------------------------------------------------------------------------
 * Command execution (fork/exec)
 * ---------------------------------------------------------------------------*/

/*
 * Structure representing one command in a pipeline.
 */
struct command {
    char *argv[MAX_ARGS];
    int   argc;
    int   single_quoted[MAX_ARGS];
    char *input_file;
    char *output_file;
    int   output_append;   /* >> vs > */
    char *error_file;
    int   error_append;
    char *output_error_file; /* &> */
};

static void cmd_free(struct command *cmd) {
    for (int i = 0; i < cmd->argc; i++)
        free(cmd->argv[i]);
    cmd->argc = 0;
    free(cmd->input_file);
    free(cmd->output_file);
    free(cmd->error_file);
    free(cmd->output_error_file);
    cmd->input_file = NULL;
    cmd->output_file = NULL;
    cmd->error_file = NULL;
    cmd->output_error_file = NULL;
}

/*
 * Parse tokens into a pipeline of commands.
 * Returns number of commands, fills cmds array.
 */
static int parse_pipeline(struct token *tokens, struct command *cmds, int max_cmds) {
    int cmdc = 0;
    int argi = 0;

    memset(cmds, 0, sizeof(struct command) * max_cmds);
    cmds[0].argc = 0;

    for (int i = 0; i < tokens->count; i++) {
        const char *tok = tokens->words[i];

        if (streq(tok, "|")) {
            /* End current command, start new one */
            cmds[cmdc].argv[cmds[cmdc].argc] = NULL;
            cmdc++;
            if (cmdc >= max_cmds) break;
            cmds[cmdc].argc = 0;
            argi = 0;
            continue;
        }

        if (streq(tok, "&")) {
            /* Background operator - end current command, start new one for what follows */
            cmds[cmdc].argv[cmds[cmdc].argc] = NULL;
            tokens->background = 1;
            cmdc++;
            if (cmdc >= max_cmds) break;
            cmds[cmdc].argc = 0;
            argi = 0;
            continue;
        }

        if (streq(tok, "<")) {
            i++;
            if (i < tokens->count)
                cmds[cmdc].input_file = xstrdup(tokens->words[i]);
            continue;
        }

        if (streq(tok, ">")) {
            i++;
            if (i < tokens->count) {
                free(cmds[cmdc].output_file);
                cmds[cmdc].output_file = xstrdup(tokens->words[i]);
                cmds[cmdc].output_append = 0;
            }
            continue;
        }

        if (streq(tok, ">>")) {
            i++;
            if (i < tokens->count) {
                free(cmds[cmdc].output_file);
                cmds[cmdc].output_file = xstrdup(tokens->words[i]);
                cmds[cmdc].output_append = 1;
            }
            continue;
        }

        if (streq(tok, "2>")) {
            i++;
            if (i < tokens->count)
                cmds[cmdc].error_file = xstrdup(tokens->words[i]);
            continue;
        }

        if (streq(tok, "2>>")) {
            i++;
            if (i < tokens->count) {
                free(cmds[cmdc].error_file);
                cmds[cmdc].error_file = xstrdup(tokens->words[i]);
                cmds[cmdc].error_append = 1;
            }
            continue;
        }

        if (streq(tok, "&>")) {
            i++;
            if (i < tokens->count) {
                free(cmds[cmdc].output_error_file);
                cmds[cmdc].output_error_file = xstrdup(tokens->words[i]);
            }
            continue;
        }

        /* Regular argument */
        if (argi < MAX_ARGS - 1) {
            cmds[cmdc].argv[argi] = xstrdup(tok);
            cmds[cmdc].single_quoted[argi] = tokens->single_quoted[i];
            argi++;
            cmds[cmdc].argc = argi;
        }
    }
    cmds[cmdc].argv[cmds[cmdc].argc] = NULL;

    return cmdc + 1;
}

/* Execute a pipeline of commands */
static int execute_pipeline(struct command *cmds, int cmdc, int background) {
    int prev_fd = -1;
    int status = 0;
    pid_t last_pid = -1;
    int pipeline_pids[MAX_ARGS];

    for (int i = 0; i < cmdc; i++) {
        int pipefd[2] = {-1, -1};

        /* Create pipe if not last command */
        if (i < cmdc - 1) {
            if (pipe(pipefd) < 0) {
                perror("forest-shell: pipe");
                return 1;
            }
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("forest-shell: fork");
            return 1;
        }

        if (pid == 0) {
            /* Child process */
            setpgid(0, 0);

            /* Restore default signal handlers */
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);

            /* Input from previous pipe or file */
            if (prev_fd != -1) {
                dup2(prev_fd, STDIN_FILENO);
                close(prev_fd);
            }
            if (cmds[i].input_file) {
                int fd = open(cmds[i].input_file, O_RDONLY);
                if (fd < 0) {
                    fprintf(stderr, "forest-shell: %s: %s\n",
                            cmds[i].input_file, strerror(errno));
                    exit(1);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            /* Output to next pipe or file */
            if (pipefd[1] != -1) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }
            if (cmds[i].output_file) {
                int flags = O_WRONLY | O_CREAT | (cmds[i].output_append ? O_APPEND : O_TRUNC);
                int fd = open(cmds[i].output_file, flags, 0644);
                if (fd < 0) {
                    fprintf(stderr, "forest-shell: %s: %s\n",
                            cmds[i].output_file, strerror(errno));
                    exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            /* Error redirection */
            if (cmds[i].error_file) {
                int flags = O_WRONLY | O_CREAT | (cmds[i].error_append ? O_APPEND : O_TRUNC);
                int fd = open(cmds[i].error_file, flags, 0644);
                if (fd < 0) {
                    fprintf(stderr, "forest-shell: %s: %s\n",
                            cmds[i].error_file, strerror(errno));
                    exit(1);
                }
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
            if (cmds[i].output_error_file) {
                int flags = O_WRONLY | O_CREAT | O_TRUNC;
                int fd = open(cmds[i].output_error_file, flags, 0644);
                if (fd < 0) {
                    fprintf(stderr, "forest-shell: %s: %s\n",
                            cmds[i].output_error_file, strerror(errno));
                    exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }

            /* Expand environment variables and command substitutions in args.
             * Skip expansion for tokens that were in single quotes. */
            for (int j = 0; j < cmds[i].argc; j++) {
                if (!cmds[i].argv[j] || cmds[i].single_quoted[j])
                    continue;
                /* Command substitution first */
                if (strchr(cmds[i].argv[j], '`')) {
                    char *expanded = expand_cmdsub(cmds[i].argv[j]);
                    free(cmds[i].argv[j]);
                    cmds[i].argv[j] = expanded;
                }
                /* Variable expansion */
                if (strchr(cmds[i].argv[j], '$')) {
                    char *expanded = expand_vars(cmds[i].argv[j], 0, NULL);
                    free(cmds[i].argv[j]);
                    cmds[i].argv[j] = expanded;
                }
            }

            /* Expand globs */
            char *new_argv[MAX_ARGS];
            int new_argc = expand_glob(cmds[i].argv, cmds[i].argc,
                                       new_argv, MAX_ARGS);
            for (int j = 0; j < new_argc; j++)
                cmds[i].argv[j] = new_argv[j];
            cmds[i].argc = new_argc;

            /* Execute */
            if (cmds[i].argc == 0)
                exit(0);

            if (is_builtin(cmds[i].argv[0])) {
                exit(run_builtin(cmds[i].argc, cmds[i].argv));
            }

            char *cmd_path = find_command(cmds[i].argv[0]);
            if (cmd_path) {
                execv(cmd_path, cmds[i].argv);
                fprintf(stderr, "forest-shell: %s: %s\n", cmd_path, strerror(errno));
                free(cmd_path);
            } else {
                fprintf(stderr, "forest-shell: command not found: %s\n",
                        cmds[i].argv[0]);
            }
            exit(127);
        }

        /* Parent */
        pipeline_pids[i] = pid;
        last_pid = pid;

        if (i == 0 && !background) {
            setpgid(pid, pid);
            /* Set child as foreground process group */
            if (g_interactive)
                tcsetpgrp(STDIN_FILENO, pid);
        }

        /* Close pipes in parent */
        if (prev_fd != -1)
            close(prev_fd);
        if (pipefd[1] != -1)
            close(pipefd[1]);

        prev_fd = (pipefd[0] != -1) ? pipefd[0] : -1;
    }

    /* Wait for all processes in pipeline */
    if (!background) {
        for (int i = 0; i < cmdc; i++) {
            int st;
            waitpid(pipeline_pids[i], &st, WUNTRACED);

            if (WIFEXITED(st))
                status = WEXITSTATUS(st);
            else if (WIFSIGNALED(st))
                status = 128 + WTERMSIG(st);
            else if (WIFSTOPPED(st)) {
                /* Job stopped - add to job list */
                char cmdline[MAX_LINE] = "";
                for (int j = 0; j < cmds[0].argc && cmdline[0] == '\0'; j++) {
                    if (j > 0) strncat(cmdline, " ", sizeof(cmdline) - strlen(cmdline) - 1);
                    strncat(cmdline, cmds[0].argv[j], sizeof(cmdline) - strlen(cmdline) - 1);
                }
                int jid = job_add(pipeline_pids[i], cmdline);
                fprintf(stderr, "\n[%d]  Stopped  %s\n", jid, cmdline);
                status = 128 + SIGTSTP;
            }
        }
        /* Restore foreground process group to shell */
        if (g_interactive)
            tcsetpgrp(STDIN_FILENO, g_shell_pgid);
    } else {
        /* Background: add to job list */
        char cmdline[MAX_LINE] = "";
        for (int j = 0; j < cmds[0].argc && cmdline[0] == '\0'; j++) {
            if (j > 0) strncat(cmdline, " ", sizeof(cmdline) - strlen(cmdline) - 1);
            strncat(cmdline, cmds[0].argv[j], sizeof(cmdline) - strlen(cmdline) - 1);
        }
        strncat(cmdline, " &", sizeof(cmdline) - strlen(cmdline) - 1);
        int jid = job_add(last_pid, cmdline);
        fprintf(stderr, "[%d] %d\n", jid, last_pid);
    }

    if (prev_fd != -1)
        close(prev_fd);

    return status;
}

/* ---------------------------------------------------------------------------
 * Command substitution (basic `cmd` support)
 * ---------------------------------------------------------------------------*/
static char *cmdsub_exec(const char *cmd_str) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return NULL;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd_str, NULL);
        _exit(1);
    }

    close(pipefd[1]);

    char buf[MAX_LINE];
    size_t total = 0;
    ssize_t n;
    while ((n = read(pipefd[0], buf + total, sizeof(buf) - total - 1)) > 0)
        total += n;
    close(pipefd[0]);

    /* Wait for child */
    int st;
    waitpid(pid, &st, 0);

    /* Remove trailing newline */
    while (total > 0 && (buf[total-1] == '\n' || buf[total-1] == '\r'))
        total--;
    buf[total] = '\0';

    return xstrdup(buf);
}

/*
 * Expand command substitutions in a token.
 * Replaces `...` with the output.
 */
static char *expand_cmdsub(const char *input) {
    if (!input || !strchr(input, '`'))
        return xstrdup(input);

    size_t outlen = MAX_LINE;
    char *out = xmalloc(outlen);
    size_t opos = 0;

    const char *p = input;
    while (*p && opos < outlen - 1) {
        if (*p == '`') {
            p++;
            char cmd[MAX_LINE];
            int clen = 0;
            while (*p && *p != '`' && clen < MAX_LINE - 1)
                cmd[clen++] = *p++;
            if (*p == '`') p++;
            cmd[clen] = '\0';

            char *result = cmdsub_exec(cmd);
            if (result) {
                size_t rlen = strlen(result);
                if (opos + rlen < outlen) {
                    memcpy(out + opos, result, rlen);
                    opos += rlen;
                }
                free(result);
            }
        } else {
            out[opos++] = *p;
            p++;
        }
    }
    out[opos] = '\0';
    return out;
}

/* ---------------------------------------------------------------------------
 * Execute a single command or pipeline (after splitting on && ;)
 * ---------------------------------------------------------------------------*/
static void execute_single(char *line) {
    if (!line || !*line) return;

    /* Skip leading/trailing whitespace */
    const char *start = skip_space(line);
    if (!*start) return;

    /* Check for comments */
    if (*start == '#') return;

    /* Tokenize */
    struct token tokens;
    tokenize(start, &tokens);

    if (tokens.count == 0) {
        token_free(&tokens);
        return;
    }

    /* Check for builtin command (first token is a builtin name).
     * Run builtins directly in parent without forking so that
     * environment changes persist across commands.
     * Skip if there are pipes or redirections - those need fork/exec. */
    {
        const char *cmd = tokens.words[0];

        if (streq(cmd, "exit")) {
            int code = 0;
            if (tokens.count > 1) code = atoi(tokens.words[1]);
            token_free(&tokens);
            g_running = 0;
            exit(code);
        }

        /* Check if any pipe/redirect/background operators exist */
        int has_operators = 0;
        for (int i = 1; i < tokens.count; i++) {
            if (streq(tokens.words[i], "|") ||
                streq(tokens.words[i], "<") ||
                streq(tokens.words[i], ">") ||
                streq(tokens.words[i], ">>") ||
                streq(tokens.words[i], "2>") ||
                streq(tokens.words[i], "2>>") ||
                streq(tokens.words[i], "&>")) {
                has_operators = 1;
                break;
            }
        }
        if (tokens.background)
            has_operators = 1;

        if (is_builtin(cmd) && !has_operators) {
            /* Expand variables and command substitutions right before running.
             * Skip expansion for tokens that were in single quotes. */
            for (int i = 1; i < tokens.count; i++) {
                if (tokens.single_quoted[i])
                    continue;
                if (strchr(tokens.words[i], '`')) {
                    char *expanded = expand_cmdsub(tokens.words[i]);
                    free(tokens.words[i]);
                    tokens.words[i] = expanded;
                }
                if (strchr(tokens.words[i], '$')) {
                    char *varexp = expand_vars(tokens.words[i], 0, NULL);
                    free(tokens.words[i]);
                    tokens.words[i] = varexp;
                }
            }
            g_last_status = run_builtin(tokens.count, tokens.words);
            token_free(&tokens);
            return;
        }
    }

    /* For piped/redirected commands, expand aliases but NOT variables yet.
     * Variable expansion happens in the child process so that commands like
     * "export X=1 ; echo $X" work correctly. */
    for (int i = 0; i < tokens.count; i++) {
        if (streq(tokens.words[i], "|") ||
            streq(tokens.words[i], "<") ||
            streq(tokens.words[i], ">") ||
            streq(tokens.words[i], ">>") ||
            streq(tokens.words[i], "2>") ||
            streq(tokens.words[i], "2>>") ||
            streq(tokens.words[i], "&>"))
            continue;
        /* Only expand aliases here, variables expand in child */
        const char *al = alias_lookup(tokens.words[i]);
        if (al) {
            free(tokens.words[i]);
            tokens.words[i] = xstrdup(al);
        }
    }

    /* Parse into pipeline */
    struct command cmds[MAX_ARGS];
    int cmdc = parse_pipeline(&tokens, cmds, MAX_ARGS);

    /* Execute pipeline */
    g_last_status = execute_pipeline(cmds, cmdc, tokens.background);

    /* Cleanup */
    for (int i = 0; i < cmdc; i++)
        cmd_free(&cmds[i]);
    token_free(&tokens);
}

/* ---------------------------------------------------------------------------
 * Main execution function for a single line
 *
 * Handles ; and && as command list separators:
 *   cmd1 ; cmd2      - run both unconditionally
 *   cmd1 && cmd2     - run cmd2 only if cmd1 succeeds
 *   cmd1 && cmd2 ; cmd3 - run cmd2 if cmd1 succeeds, run cmd3 always
 * ---------------------------------------------------------------------------*/
static void execute_line(char *line) {
    if (!line || !*line) return;

    /* Skip leading whitespace */
    const char *start = skip_space(line);
    if (!*start) return;

    /* Check for comments */
    if (*start == '#') return;

    /* Add to history */
    add_history(start);

    /* Parse command list: split on ; and && */
    const char *p = start;
    int continue_on_success = 1; /* 1 = unconditional (; or first), 0 = && */

    while (*p && g_running) {
        const char *seg_start = p;
        const char *seg_end = NULL;
        int next_op = 0; /* 0 = end, 1 = ';', 2 = '&&' */

        /* Find the next ; or && operator (not inside quotes) */
        int in_single = 0, in_double = 0;
        while (*p) {
            if (*p == '\'' && !in_double) { in_single = !in_single; p++; continue; }
            if (*p == '"' && !in_single) { in_double = !in_double; p++; continue; }
            if (in_single || in_double) { p++; continue; }

            if (*p == ';' && *(p+1) != ';') {
                seg_end = p;
                p++;
                next_op = 1;
                break;
            }
            if (*p == '&' && *(p+1) == '&') {
                seg_end = p;
                p += 2;
                next_op = 2;
                break;
            }
            p++;
        }

        if (!seg_end) seg_end = p;

        /* Skip whitespace around segment */
        while (seg_start < seg_end && (*seg_start == ' ' || *seg_start == '\t'))
            seg_start++;
        const char *e = seg_end - 1;
        while (e >= seg_start && (*e == ' ' || *e == '\t'))
            e--;
        size_t seg_len = (e + 1) - seg_start;

        /* Execute segment if condition met */
        if (continue_on_success && seg_len > 0) {
            char *seg = xmalloc(seg_len + 1);
            memcpy(seg, seg_start, seg_len);
            seg[seg_len] = '\0';
            execute_single(seg);
            free(seg);
        }

        /* Determine if next segment should run */
        if (next_op == 2) {
            /* && : only continue if previous command succeeded */
            continue_on_success = (g_last_status == 0);
        } else {
            /* ; or end : always continue */
            continue_on_success = 1;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Source file (for "source" and "." builtin)
 * ---------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * Shell initialization
 * ---------------------------------------------------------------------------*/
static void shell_init(void) {
    /* Determine if interactive */
    g_interactive = isatty(STDIN_FILENO);

    /* Install signal handlers */
    install_signal_handlers();

    /* Set up shell process group - shell in its own group */
    g_shell_pgid = getpid();
    setpgid(g_shell_pgid, g_shell_pgid);

    if (g_interactive)
        tcsetpgrp(STDIN_FILENO, g_shell_pgid);

    /* Detect Forest OS vs host */
    int forest_os = 0;
    struct utsname uts;
    if (uname(&uts) == 0) {
        if (strstr(uts.sysname, "Forest") || strstr(uts.sysname, "forest") ||
            strstr(uts.release, "Forest") || strstr(uts.release, "forest"))
            forest_os = 1;
    }

    /* Add shell's own directory to PATH so cross-compiled shell can find
     * Forest OS binaries even when running on host for testing */
    char self_dir[MAX_PATH_LEN] = "";
    ssize_t self_len = readlink("/proc/self/exe", self_dir, sizeof(self_dir) - 1);
    if (self_len > 0) {
        self_dir[self_len] = '\0';
        char *last_slash = strrchr(self_dir, '/');
        if (last_slash) *last_slash = '\0';
    }

    /* Build PATH with self_dir prepended if not already present */
    char full_path[MAX_PATH_LEN * 2];
    const char *base_path = forest_os
        ? "/bin:/usr/bin:/usr/local/bin:/sbin:/usr/sbin"
        : "/bin:/usr/bin:/usr/local/bin";
    if (self_dir[0] && !strstr(base_path, self_dir)) {
        snprintf(full_path, sizeof(full_path), "%s:%s", self_dir, base_path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s", base_path);
    }

    if (!getenv("PATH"))
        setenv("PATH", full_path, 1);
    else {
        /* Prepend self_dir to existing PATH */
        const char *existing = getenv("PATH");
        if (self_dir[0] && !strstr(existing, self_dir)) {
            snprintf(full_path, sizeof(full_path), "%s:%s", self_dir, existing);
            setenv("PATH", full_path, 1);
        }
    }

    /* Set sensible defaults for all env vars */
    if (!getenv("HOME"))
        setenv("HOME", "/home/root", 1);
    if (!getenv("SHELL"))
        setenv("SHELL", "/bin/forest-shell", 1);
    if (!getenv("TERM"))
        setenv("TERM", "forest", 1);
    if (!getenv("USER"))
        setenv("USER", "root", 1);
    if (!getenv("LOGNAME"))
        setenv("LOGNAME", "root", 1);
    if (!getenv("HOSTNAME"))
        setenv("HOSTNAME", forest_os ? "forest" : "localhost", 1);

    /* Initialize cwd to HOME */
    const char *home = getenv("HOME");
    if (home)
        chdir(home);
    else
        chdir("/");

    /* Set PWD to match cwd */
    char *cwd = getcwd(NULL, 0);
    if (cwd) {
        setenv("PWD", cwd, 1);
        free(cwd);
    }
}

/* ---------------------------------------------------------------------------
 * Main shell loop
 * ---------------------------------------------------------------------------*/
static void shell_loop(void) {
    while (g_running) {
        /* Reap any background jobs */
        if (g_sigchld_received) {
            g_sigchld_received = 0;
            job_reap();
        }

        /* Reset SIGINT flag */
        g_sigint_received = 0;

        /* Print prompt */
        print_prompt();

        /* Read line */
        char *line = read_line();
        if (!line) {
            /* EOF */
            g_running = 0;
            break;
        }

        /* Skip empty lines */
        const char *trimmed = skip_space(line);
        if (*trimmed == '\0') {
            free(line);
            continue;
        }

        /* Execute */
        execute_line(line);
        free(line);
    }
}

/* ---------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------------*/
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    shell_init();

    /* If arguments are given, execute as script */
    if (argc > 1) {
        g_interactive = 0;
        if (streq(argv[1], "-c") && argc > 2) {
            /* -c: execute command */
            char *line = xstrdup(argv[2]);
            execute_line(line);
            free(line);
            return g_last_status;
        }
        if (streq(argv[1], "-s")) {
            /* -s: read from stdin */
            shell_loop();
            return g_last_status;
        }
        /* Otherwise treat as script file */
        return source_file(argv[1]);
    }

    shell_loop();
    return g_last_status;
}
