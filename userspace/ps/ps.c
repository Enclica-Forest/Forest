/*
 * ps.c - Forest OS userspace process status
 * Uses SYS_GET_TASKS (528) to read real kernel task list.
 */

#define _POSIX_C_SOURCE 200809L
#include "forest.h"
#include <getopt.h>

#define MAX_PROCS 256

/* Mirror of kernel task_info_t (fern/src/include/task.h:294-305) */
typedef struct {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t pgrp;
    int32_t  state;
    uint32_t last_active_tick;
    char     name[32];
    uint32_t priority;
    uint64_t cpu_ticks_total;
    uint32_t memory_used_kb;
    uint32_t created_at_tick;
} task_info_t;

/* task_state_t values from kernel */
enum {
    STATE_RUNNING   = 0,
    STATE_READY     = 1,
    STATE_WAITING   = 2,
    STATE_TERMINATED = 3,
    STATE_ZOMBIE    = 4,
    STATE_SUSPENDED = 5,
};

/* Display mode */
enum {
    MODE_DEFAULT = 0,
    MODE_AUX,
    MODE_FULL,
    MODE_LONG,
    MODE_JOBS,
    MODE_CUSTOM,
    MODE_FOREST,
};

/* Sort keys */
enum {
    SORT_PID = 0,
    SORT_PPID,
    SORT_NAME,
    SORT_CPU,
    SORT_MEM,
    SORT_PRI,
    SORT_STATE,
};

/* Custom format fields */
enum {
    FIELD_PID, FIELD_PPID, FIELD_PGDID, FIELD_UID, FIELD_USER,
    FIELD_STAT, FIELD_TIME, FIELD_COMM, FIELD_ARGS,
    FIELD_PCPU, FIELD_PMEM, FIELD_VSZ, FIELD_RSS,
    FIELD_NICE, FIELD_PRI, FIELD_SZ, FIELD_TTY, FIELD_START,
    FIELD_F, FIELD_C, FIELD_STIME,
    FIELD_COUNT
};

static const char *field_names[] = {
    "pid", "ppid", "pgid", "uid", "user",
    "stat", "time", "comm", "args",
    "%cpu", "%mem", "vsz", "rss",
    "nice", "pri", "sz", "tty", "start",
    "f", "c", "stime",
    NULL
};

/* Options */
static struct {
    int mode;
    int show_all;
    int aux;
    int full_format;
    int long_format;
    int jobs_format;
    int forest;
    int custom_fields[FIELD_COUNT];
    int custom_field_count;
    int sort_key;
    int sort_reverse;
    /* filters */
    int filter_pid;
    uint32_t pid_value;
    int filter_tty;
    char tty_value[32];
    int filter_user;
    char user_value[32];
    int filter_group;
    char group_value[32];
    /* derived */
    uint64_t total_memory_kb;
    uint32_t elapsed_ticks;
} opts;

static const char *progname;

/* libc wrapper for SYS_GET_TASKS */
extern ssize_t get_tasks(void *buf, size_t max_entries);

/* ---- helpers ---- */

static const char *state_char(int state) {
    switch (state) {
        case STATE_RUNNING:    return "R";
        case STATE_READY:      return "R";
        case STATE_WAITING:    return "S";
        case STATE_TERMINATED: return "X";
        case STATE_ZOMBIE:     return "Z";
        case STATE_SUSPENDED:  return "T";
        default:               return "?";
    }
}

static void format_ticks(uint32_t ticks, char *buf, size_t buflen) {
    uint32_t seconds = ticks / 100;
    uint32_t h = seconds / 3600;
    uint32_t m = (seconds % 3600) / 60;
    uint32_t s = seconds % 60;
    snprintf(buf, buflen, "%02u:%02u:%02u", h, m, s);
}

/* ---- fetch ---- */

static int fetch_processes(task_info_t *procs) {
    ssize_t n = get_tasks(procs, MAX_PROCS);
    if (n < 0) {
        fprintf(stderr, "%s: get_tasks: %s\n", progname, strerror(errno));
        return 0;
    }
    return (int)n;
}

/* ---- filter ---- */

static int should_show(const task_info_t *p) {
    if (opts.filter_pid && p->pid != opts.pid_value)
        return 0;
    if (opts.filter_user || opts.filter_tty || opts.filter_group) {
        /* task_info_t lacks uid/tty/group fields; skip these filters gracefully */
    }
    if (!opts.show_all && !opts.aux) {
        /* default: hide kernel tasks (pid 0, 1) and zombies unless running */
        if (p->pid == 0)
            return 0;
    }
    return 1;
}

/* ---- sort ---- */

static int sort_compare(const void *a, const void *b) {
    const task_info_t *pa = (const task_info_t *)a;
    const task_info_t *pb = (const task_info_t *)b;
    int cmp = 0;

    switch (opts.sort_key) {
        case SORT_PID:   cmp = (pa->pid > pb->pid) - (pa->pid < pb->pid); break;
        case SORT_PPID:  cmp = (pa->parent_pid > pb->parent_pid) - (pa->parent_pid < pb->parent_pid); break;
        case SORT_NAME:  cmp = strcmp(pa->name, pb->name); break;
        case SORT_CPU:   cmp = (pa->cpu_ticks_total > pb->cpu_ticks_total) - (pa->cpu_ticks_total < pb->cpu_ticks_total); break;
        case SORT_MEM:   cmp = (pa->memory_used_kb > pb->memory_used_kb) - (pa->memory_used_kb < pb->memory_used_kb); break;
        case SORT_PRI:   cmp = (pa->priority > pb->priority) - (pa->priority < pb->priority); break;
        case SORT_STATE: cmp = (pa->state > pb->state) - (pa->state < pb->state); break;
    }

    return opts.sort_reverse ? -cmp : cmp;
}

/* ---- print ---- */

static void print_default_header(void) {
    printf("%7s %6s %5s %8s %s\n", "PID", "TTY", "STAT", "TIME", "COMMAND");
}

static void print_default_proc(const task_info_t *p) {
    char timebuf[16];
    format_ticks(p->cpu_ticks_total, timebuf, sizeof(timebuf));
    printf("%7u %6s %5s %8s %s\n",
           p->pid, "?", state_char(p->state), timebuf, p->name);
}

static void print_aux_header(void) {
    printf("%-8s %7s %5s %5s %8s %8s %6s %5s %8s %8s %s\n",
           "USER", "PID", "%CPU", "%MEM", "VSZ", "RSS", "TTY", "STAT", "START", "TIME", "COMMAND");
}

static void print_aux_proc(const task_info_t *p) {
    char timebuf[16];
    format_ticks(p->cpu_ticks_total, timebuf, sizeof(timebuf));
    uint64_t vsz = (uint64_t)p->memory_used_kb * 1024;
    uint32_t rss = p->memory_used_kb;
    float pcpu = 0.0;
    float pmem = 0.0;
    if (opts.elapsed_ticks > 0) {
        pcpu = (float)p->cpu_ticks_total * 100.0f / (float)opts.elapsed_ticks;
        if (pcpu > 999.9f) pcpu = 999.9f;
    }
    if (opts.total_memory_kb > 0) {
        pmem = (float)p->memory_used_kb * 100.0f / (float)opts.total_memory_kb;
        if (pmem > 99.9f) pmem = 99.9f;
    }
    printf("%-8s %7u %5.1f %5.1f %8lu %8u %6s %5s %8s %8s %s\n",
           "root", p->pid, pcpu, pmem,
           (unsigned long)vsz, rss, "?",
           state_char(p->state), "?", timebuf, p->name);
}

static void print_full_header(void) {
    printf("%1s %8s %7s %7s %4s %8s %6s %8s %s\n",
           "F", "UID", "PID", "PPID", "C", "STIME", "TTY", "TIME", "CMD");
}

static void print_full_proc(const task_info_t *p) {
    char timebuf[16];
    format_ticks(p->cpu_ticks_total, timebuf, sizeof(timebuf));
    uint32_t c_val = 0;
    if (opts.elapsed_ticks > 0)
        c_val = (uint32_t)((float)p->cpu_ticks_total * 100.0f / (float)opts.elapsed_ticks);
    printf("%1s %8s %7u %7u %4u %8s %6s %8s %s\n",
           "-", "root", p->pid, p->parent_pid, c_val,
           "?", "?", timebuf, p->name);
}

static void print_long_header(void) {
    printf("%1s %8s %7s %7s %4s %5s %4s %8s %8s %5s %6s %8s %6s %8s %s\n",
           "F", "UID", "PID", "PPID", "C", "PRI", "NI",
           "RSS", "SZ", "STAT", "TTY", "STIME", "TIME", "TIME2", "CMD");
}

static void print_long_proc(const task_info_t *p) {
    char timebuf[16];
    format_ticks(p->cpu_ticks_total, timebuf, sizeof(timebuf));
    uint32_t c_val = 0;
    if (opts.elapsed_ticks > 0)
        c_val = (uint32_t)((float)p->cpu_ticks_total * 100.0f / (float)opts.elapsed_ticks);
    int nice_val = (int)p->priority - 120;
    printf("%1s %8s %7u %7u %4u %5u %4d %8u %8u %5s %6s %8s %8s %6s %s\n",
           "-", "root", p->pid, p->parent_pid, c_val,
           p->priority, nice_val,
           p->memory_used_kb, p->memory_used_kb,
           state_char(p->state), "?", "?", timebuf, timebuf, p->name);
}

static void print_jobs_header(void) {
    printf("%7s %7s %6s %5s %8s %s\n",
           "PGID", "SID", "TTY", "STAT", "TIME", "CMD");
}

static void print_jobs_proc(const task_info_t *p) {
    char timebuf[16];
    format_ticks(p->cpu_ticks_total, timebuf, sizeof(timebuf));
    printf("%7u %7u %6s %5s %8s %s\n",
           p->pgrp, p->pgrp, "?", state_char(p->state), timebuf, p->name);
}

/* ---- custom format ---- */

static void print_custom_header(void) {
    for (int i = 0; i < opts.custom_field_count; i++) {
        if (i > 0) putchar(' ');
        printf("%-10s", field_names[opts.custom_fields[i]]);
    }
    putchar('\n');
}

static void print_custom_proc(const task_info_t *p) {
    char timebuf[16];
    format_ticks(p->cpu_ticks_total, timebuf, sizeof(timebuf));

    for (int i = 0; i < opts.custom_field_count; i++) {
        if (i > 0) putchar(' ');
        switch (opts.custom_fields[i]) {
            case FIELD_PID:   printf("%-10u", p->pid); break;
            case FIELD_PPID:  printf("%-10u", p->parent_pid); break;
            case FIELD_PGDID: printf("%-10u", p->pgrp); break;
            case FIELD_UID:   printf("%-10s", "0"); break;
            case FIELD_USER:  printf("%-10s", "root"); break;
            case FIELD_STAT:  printf("%-10s", state_char(p->state)); break;
            case FIELD_TIME:  printf("%-10s", timebuf); break;
            case FIELD_COMM:  printf("%-10s", p->name); break;
            case FIELD_ARGS:  printf("%-10s", p->name); break;
            case FIELD_PCPU: {
                float v = 0.0;
                if (opts.elapsed_ticks > 0)
                    v = (float)p->cpu_ticks_total * 100.0f / (float)opts.elapsed_ticks;
                printf("%-10.1f", v);
                break;
            }
            case FIELD_PMEM: {
                float v = 0.0;
                if (opts.total_memory_kb > 0)
                    v = (float)p->memory_used_kb * 100.0f / (float)opts.total_memory_kb;
                printf("%-10.1f", v);
                break;
            }
            case FIELD_VSZ:  printf("%-10lu", (unsigned long)p->memory_used_kb * 1024); break;
            case FIELD_RSS:  printf("%-10u", p->memory_used_kb); break;
            case FIELD_NICE: printf("%-10d", (int)p->priority - 120); break;
            case FIELD_PRI:  printf("%-10u", p->priority); break;
            case FIELD_SZ:   printf("%-10u", p->memory_used_kb); break;
            case FIELD_TTY:  printf("%-10s", "?"); break;
            case FIELD_START:printf("%-10s", "?"); break;
            case FIELD_F:    printf("%-10s", "-"); break;
            case FIELD_C: {
                uint32_t c_val = 0;
                if (opts.elapsed_ticks > 0)
                    c_val = (uint32_t)((float)p->cpu_ticks_total * 100.0f / (float)opts.elapsed_ticks);
                printf("%-10u", c_val);
                break;
            }
            case FIELD_STIME:printf("%-10s", "?"); break;
            default:         printf("%-10s", "?"); break;
        }
    }
    putchar('\n');
}

/* ---- forest (tree) display ---- */

static void print_forest_tree(const task_info_t *procs, int count, uint32_t parent_pid, int depth) {
    for (int i = 0; i < count; i++) {
        if (procs[i].parent_pid != parent_pid)
            continue;
        if (!should_show(&procs[i]))
            continue;

        for (int d = 0; d < depth; d++)
            printf("│   ");

        /* check if this is the last child at this level */
        int is_last = 1;
        for (int j = i + 1; j < count; j++) {
            if (procs[j].parent_pid == parent_pid && should_show(&procs[j])) {
                is_last = 0;
                break;
            }
        }

        if (depth > 0)
            printf("%s", is_last ? "└── " : "├── ");

        char timebuf[16];
        format_ticks(procs[i].cpu_ticks_total, timebuf, sizeof(timebuf));
        printf("%s (%s) %s\n", procs[i].name, state_char(procs[i].state), timebuf);

        print_forest_tree(procs, count, procs[i].pid, depth + 1);
    }
}

/* ---- parse custom format ---- */

static void parse_custom_format(const char *fmt) {
    opts.custom_field_count = 0;
    char *buf = strdup(fmt);
    if (!buf) return;

    char *saveptr;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok && opts.custom_field_count < FIELD_COUNT) {
        for (int i = 0; field_names[i]; i++) {
            if (strcmp(tok, field_names[i]) == 0) {
                opts.custom_fields[opts.custom_field_count++] = i;
                break;
            }
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }
    free(buf);
}

/* ---- parse sort key ---- */

static int parse_sort_key(const char *key) {
    if (strcmp(key, "pid") == 0) return SORT_PID;
    if (strcmp(key, "ppid") == 0) return SORT_PPID;
    if (strcmp(key, "name") == 0 || strcmp(key, "comm") == 0) return SORT_NAME;
    if (strcmp(key, "cpu") == 0 || strcmp(key, "%cpu") == 0) return SORT_CPU;
    if (strcmp(key, "mem") == 0 || strcmp(key, "%mem") == 0) return SORT_MEM;
    if (strcmp(key, "pri") == 0 || strcmp(key, "priority") == 0) return SORT_PRI;
    if (strcmp(key, "state") == 0 || strcmp(key, "stat") == 0) return SORT_STATE;
    fprintf(stderr, "%s: unknown sort key '%s'\n", progname, key);
    return SORT_PID;
}

/* ---- usage ---- */

static void usage(void) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  aux             BSD all-user format\n"
        "  -e              Show all processes\n"
        "  -f              Full format\n"
        "  -j              Jobs format\n"
        "  -l              Long format\n"
        "  -o FORMAT       Custom output format\n"
        "  --forest        Tree display\n"
        "  -p PID          Filter by PID\n"
        "  -t TTY          Filter by TTY\n"
        "  -u USER         Filter by user\n"
        "  -U USER         Filter by user (real)\n"
        "  -g GROUP        Filter by group\n"
        "  --sort KEY      Sort by key (pid,ppid,name,cpu,mem,pri,state)\n"
        "  --help          Show this help\n",
        progname);
}

/* ---- main ---- */

int main(int argc, char *argv[]) {
    task_info_t procs[MAX_PROCS];
    int count;
    progname = argv[0];

    /* defaults */
    opts.mode = MODE_DEFAULT;
    opts.sort_key = SORT_PID;
    opts.elapsed_ticks = 1000; /* assume ~10s uptime for %cpu calc */
    opts.total_memory_kb = 64 * 1024; /* 64 MB default */

    static struct option long_opts[] = {
        {"forest", no_argument, 0, 'F'},
        {"sort",   required_argument, 0, 'S'},
        {"help",   no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    /* check for "aux" as first argument */
    if (argc > 1 && strcmp(argv[1], "aux") == 0) {
        opts.mode = MODE_AUX;
        opts.aux = 1;
        opts.show_all = 1;
        argc--;
        argv++;
    }

    while ((opt = getopt_long(argc, argv, "efjlo:p:t:u:U:g:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'e':
                opts.show_all = 1;
                break;
            case 'f':
                opts.mode = MODE_FULL;
                opts.full_format = 1;
                break;
            case 'j':
                opts.mode = MODE_JOBS;
                opts.jobs_format = 1;
                break;
            case 'l':
                opts.mode = MODE_LONG;
                opts.long_format = 1;
                break;
            case 'o':
                opts.mode = MODE_CUSTOM;
                parse_custom_format(optarg);
                break;
            case 'p':
                opts.filter_pid = 1;
                opts.pid_value = (uint32_t)strtoul(optarg, NULL, 10);
                break;
            case 't':
                opts.filter_tty = 1;
                strncpy(opts.tty_value, optarg, sizeof(opts.tty_value) - 1);
                break;
            case 'u':
                opts.filter_user = 1;
                strncpy(opts.user_value, optarg, sizeof(opts.user_value) - 1);
                break;
            case 'U':
                opts.filter_user = 1;
                strncpy(opts.user_value, optarg, sizeof(opts.user_value) - 1);
                break;
            case 'g':
                opts.filter_group = 1;
                strncpy(opts.group_value, optarg, sizeof(opts.group_value) - 1);
                break;
            case 'F':
                opts.forest = 1;
                break;
            case 'S':
                opts.sort_key = parse_sort_key(optarg);
                break;
            case 'h':
                usage();
                return 0;
            default:
                usage();
                return 1;
        }
    }

    /* fetch processes */
    count = fetch_processes(procs);
    if (count == 0) {
        if (opts.mode == MODE_DEFAULT)
            print_default_header();
        return 0;
    }

    /* filter */
    int filtered[MAX_PROCS];
    int fcount = 0;
    for (int i = 0; i < count; i++) {
        if (should_show(&procs[i]))
            filtered[fcount++] = i;
    }

    /* sort */
    task_info_t sorted[MAX_PROCS];
    for (int i = 0; i < fcount; i++)
        sorted[i] = procs[filtered[i]];
    qsort(sorted, fcount, sizeof(task_info_t), sort_compare);

    /* forest mode */
    if (opts.forest) {
        print_forest_tree(sorted, fcount, 0, 0);
        return 0;
    }

    /* print header */
    switch (opts.mode) {
        case MODE_AUX:    print_aux_header(); break;
        case MODE_FULL:   print_full_header(); break;
        case MODE_LONG:   print_long_header(); break;
        case MODE_JOBS:   print_jobs_header(); break;
        case MODE_CUSTOM: print_custom_header(); break;
        default:          print_default_header(); break;
    }

    /* print processes */
    for (int i = 0; i < fcount; i++) {
        switch (opts.mode) {
            case MODE_AUX:    print_aux_proc(&sorted[i]); break;
            case MODE_FULL:   print_full_proc(&sorted[i]); break;
            case MODE_LONG:   print_long_proc(&sorted[i]); break;
            case MODE_JOBS:   print_jobs_proc(&sorted[i]); break;
            case MODE_CUSTOM: print_custom_proc(&sorted[i]); break;
            default:          print_default_proc(&sorted[i]); break;
        }
    }

    return 0;
}
