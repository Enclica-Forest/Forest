#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>

static int flag_s = 0;
static int flag_n = 0;
static int flag_r = 0;
static int flag_v = 0;
static int flag_m = 0;
static int flag_p = 0;
static int flag_i = 0;
static int flag_o = 0;
static int flag_a = 0;

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-snrvmpioa]\n", prog);
}

static void print_field(const char *field, int *first) {
    if (*first) {
        *first = 0;
    } else {
        putchar(' ');
    }
    fputs(field, stdout);
}

int main(int argc, char *argv[]) {
    int opt;
    int any_flag = 0;

    while ((opt = getopt(argc, argv, "snrvmpioa")) != -1) {
        switch (opt) {
            case 's': flag_s = 1; any_flag = 1; break;
            case 'n': flag_n = 1; any_flag = 1; break;
            case 'r': flag_r = 1; any_flag = 1; break;
            case 'v': flag_v = 1; any_flag = 1; break;
            case 'm': flag_m = 1; any_flag = 1; break;
            case 'p': flag_p = 1; any_flag = 1; break;
            case 'i': flag_i = 1; any_flag = 1; break;
            case 'o': flag_o = 1; any_flag = 1; break;
            case 'a': flag_a = 1; any_flag = 1; break;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (flag_a) {
        flag_s = flag_n = flag_r = flag_v = flag_m = flag_p = flag_i = flag_o = 1;
    }

    if (!any_flag) {
        flag_s = 1;
    }

    struct utsname uts;
    if (uname(&uts) < 0) {
        perror("uname");
        return 1;
    }

    int first = 1;

    if (flag_s)
        print_field(uts.sysname, &first);
    if (flag_n)
        print_field(uts.nodename, &first);
    if (flag_r)
        print_field(uts.release, &first);
    if (flag_v)
        print_field(uts.version, &first);
    if (flag_m)
        print_field(uts.machine, &first);
    if (flag_p)
        print_field(uts.machine, &first);
    if (flag_i)
        print_field("unknown", &first);
    if (flag_o)
        print_field("GNU/Linux", &first);

    putchar('\n');

    return 0;
}
