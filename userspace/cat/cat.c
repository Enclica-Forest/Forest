/*
 * cat.c - Forest OS userspace cat implementation
 * Full POSIX cat with line numbering, blank squeezing, and display modes.
 */

#include "forest.h"

/* Options */
static int opt_number      = 0;  /* -n: number all lines */
static int opt_nonblank    = 0;  /* -b: number non-blank lines */
static int opt_squeeze     = 0;  /* -s: squeeze multiple blank lines */
static int opt_show_nonprint = 0; /* -A: show non-printing */
static int opt_verbose     = 0;  /* -v: show non-printing with ^ and M- */
static int opt_tabs        = 0;  /* -T: show tabs as ^I */
static int opt_ends        = 0;  /* -E: show line ends as $ */

static int exit_code = 0;
static const char *progname;

static void usage(void) {
    fprintf(stderr, "Usage: %s [-nbsAvTEu] [FILE...]\n", progname);
    exit(1);
}

/*
 * Process and output a single character with display flags.
 * Handles -v, -A, -T, -E transformations.
 * Returns 1 if a newline was written, 0 otherwise.
 */
static int output_char(int c) {

    /* -T: show tabs as ^I */
    if (opt_tabs && c == '\t') {
        putchar('^');
        putchar('I');
        return 0;
    }

    /* -E: show line ends as $ */
    if (opt_ends && c == '\n') {
        putchar('$');
        putchar('\n');
        return 1;
    }

    /* -v or -A: show non-printing characters */
    if (opt_verbose || opt_show_nonprint) {
        if (c == '\n') {
            putchar('\n');
            return 1;
        }
        if (c >= 128) {
            /* High-bit character: M- notation */
            c -= 128;
            putchar('M');
            putchar('-');
            if (c < 32) {
                putchar('^');
                putchar(c + 64);
            } else if (c == 127) {
                putchar('^');
                putchar('?');
            } else {
                putchar(c);
            }
            return 0;
        }
        if (c < 32) {
            /* Control character: ^ notation */
            if (c == 0) {
                /* NUL: show as ^@ */
                putchar('^');
                putchar('@');
            } else {
                putchar('^');
                putchar(c + 64);
            }
            return 0;
        }
        if (c == 127) {
            putchar('^');
            putchar('?');
            return 0;
        }
        /* Printable ASCII */
        putchar(c);
        return 0;
    }

    /* Default: raw output */
    putchar(c);
    return (c == '\n');
}

/*
 * Process a file descriptor, applying all options.
 */
static void cat_file(const char *filename, int fd) {
    char buf[4096];
    ssize_t nread;
    long line_number = 0;
    int bol = 1; /* at beginning of line */
    int last_was_nl = 1; /* treat start-of-file as after newline */

    while ((nread = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < nread; i++) {
            int c = (unsigned char)buf[i];

            /* -s: squeeze blank lines (skip consecutive newlines) */
            if (opt_squeeze && c == '\n' && last_was_nl) {
                continue;
            }

            if (bol) {
                /* At beginning of line: handle numbering */
                int is_blank_line = (c == '\n');

                if (opt_number && !opt_nonblank) {
                    line_number++;
                    printf("%6ld\t", line_number);
                } else if (opt_nonblank && !is_blank_line) {
                    line_number++;
                    printf("%6ld\t", line_number);
                }

                bol = 0;
            }

            if (output_char(c)) {
                bol = 1;
                last_was_nl = 1;
            } else {
                last_was_nl = 0;
            }
        }
    }

    if (nread < 0) {
        fprintf(stderr, "%s: read error on '%s'\n", progname, filename);
        exit_code = EXIT_FAIL;
    }
}

int main(int argc, char *argv[]) {
    int opt;

    progname = argv[0];
    if (strncmp(progname, "./", 2) == 0)
        progname += 2;

    while ((opt = getopt(argc, argv, "nbsAvTEu")) != -1) {
        switch (opt) {
        case 'n':
            opt_number = 1;
            break;
        case 'b':
            opt_nonblank = 1;
            break;
        case 's':
            opt_squeeze = 1;
            break;
        case 'A':
            opt_show_nonprint = 1;
            break;
        case 'v':
            opt_verbose = 1;
            break;
        case 'T':
            opt_tabs = 1;
            break;
        case 'E':
            opt_ends = 1;
            break;
        case 'u':
            /* ignored for compatibility */
            break;
        default:
            usage();
        }
    }

    /* -b overrides -n */
    if (opt_nonblank)
        opt_number = 0;

    /* -A enables -v */
    if (opt_show_nonprint)
        opt_verbose = 1;

    if (optind >= argc) {
        /* No arguments: read from stdin */
        cat_file("-", STDIN_FILENO);
    } else {
        for (int i = optind; i < argc; i++) {
            if (strcmp(argv[i], "-") == 0) {
                cat_file("-", STDIN_FILENO);
            } else {
                int fd = open(argv[i], O_RDONLY);
                if (fd < 0) {
                    fprintf(stderr, "%s: cannot open '%s': %s\n",
                            progname, argv[i], strerror(errno));
                    exit_code = EXIT_FAIL;
                    continue;
                }
                cat_file(argv[i], fd);
                close(fd);
            }
        }
    }

    return exit_code;
}
