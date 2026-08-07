#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_LINES 10

static const char *progname = "head";
static int show_headers = 1;
static int count_lines = 1;
static long num_units = DEFAULT_LINES;

static void usage(void) {
    fprintf(stderr, "Usage: %s [-n NUM | -c NUM] [-qv] [FILE...]\n", progname);
    exit(1);
}

static void print_header(const char *filename) {
    printf("==> %s <==\n", filename);
}

static void head_lines(FILE *fp, long n) {
    int c;
    long count = 0;
    while ((c = fgetc(fp)) != EOF) {
        putchar(c);
        if (c == '\n') {
            if (++count >= n)
                return;
        }
    }
}

static void head_bytes(FILE *fp, long n) {
    long count = 0;
    int c;
    while (count < n && (c = fgetc(fp)) != EOF) {
        putchar(c);
        count++;
    }
}

static void head_file(const char *filename, int show_name) {
    FILE *fp;
    if (strcmp(filename, "-") == 0) {
        fp = stdin;
        if (show_headers && show_name)
            print_header("standard input");
    } else {
        fp = fopen(filename, "r");
        if (!fp) {
            fprintf(stderr, "%s: cannot open '%s': ", progname, filename);
            perror("");
            return;
        }
        if (show_headers && show_name)
            print_header(filename);
    }

    if (count_lines)
        head_lines(fp, num_units);
    else
        head_bytes(fp, num_units);

    if (fp != stdin)
        fclose(fp);
}

int main(int argc, char *argv[]) {
    int i, j, nfiles;
    char *endptr;
    int opt;

    progname = argv[0];

    /* Process long options before getopt */
    j = 1;
    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--lines=", 8) == 0) {
            num_units = strtol(argv[i] + 8, &endptr, 10);
            if (*endptr != '\0' || num_units <= 0) {
                fprintf(stderr, "%s: invalid number of lines: '%s'\n",
                        progname, argv[i] + 8);
                usage();
            }
            count_lines = 1;
        } else if (strncmp(argv[i], "--bytes=", 8) == 0) {
            num_units = strtol(argv[i] + 8, &endptr, 10);
            if (*endptr != '\0' || num_units <= 0) {
                fprintf(stderr, "%s: invalid number of bytes: '%s'\n",
                        progname, argv[i] + 8);
                usage();
            }
            count_lines = 0;
        } else {
            argv[j++] = argv[i];
        }
    }
    argc = j;

    while ((opt = getopt(argc, argv, "n:c:qv")) != -1) {
        switch (opt) {
        case 'n':
            num_units = strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || num_units <= 0) {
                fprintf(stderr, "%s: invalid number of lines: '%s'\n",
                        progname, optarg);
                usage();
            }
            count_lines = 1;
            break;
        case 'c':
            num_units = strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || num_units <= 0) {
                fprintf(stderr, "%s: invalid number of bytes: '%s'\n",
                        progname, optarg);
                usage();
            }
            count_lines = 0;
            break;
        case 'q':
            show_headers = 0;
            break;
        case 'v':
            show_headers = 1;
            break;
        default:
            usage();
        }
    }

    nfiles = argc - optind;
    if (nfiles == 0) {
        head_file("-", 0);
    } else {
        for (i = optind; i < argc; i++) {
            head_file(argv[i], nfiles > 1);
        }
    }

    return 0;
}
