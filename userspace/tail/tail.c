#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define DEFAULT_LINES 10
#define BUFSIZE 4096

static const char *progname = "tail";
static int header_mode = -1;
static int count_lines = 1;
static long num_units = DEFAULT_LINES;
static long from_line = 0;
static int follow_mode = 0;

static void usage(void) {
    fprintf(stderr, "Usage: %s [-n NUM | -c NUM] [-f] [-q | -v] [FILE...]\n", progname);
    fprintf(stderr, "  -n NUM   print last NUM lines (default 10), +NUM starts from line NUM\n");
    fprintf(stderr, "  -c NUM   print last NUM bytes\n");
    fprintf(stderr, "  -f       follow file for new data\n");
    fprintf(stderr, "  -q       suppress headers\n");
    fprintf(stderr, "  -v       always show headers\n");
    exit(1);
}

static void print_header(const char *filename) {
    printf("==> %s <==\n", filename);
}

static int should_show_header(int multi_file) {
    if (header_mode == 0) return 0;
    if (header_mode == 1) return 1;
    return multi_file;
}

static void tail_lines_from_start(FILE *fp, long start_line) {
    long line_num = 0;
    int c;
    int printing = 0;

    if (start_line <= 1)
        printing = 1;

    while ((c = fgetc(fp)) != EOF) {
        if (!printing) {
            if (c == '\n') {
                line_num++;
                if (line_num >= start_line - 1)
                    printing = 1;
            }
        } else {
            putchar(c);
        }
    }
}

static void tail_lines(FILE *fp, long n) {
    char *data = NULL;
    size_t data_size = 0;
    size_t data_cap = 0;
    char buf[BUFSIZE];
    size_t bytes_read;
    long total_lines = 0;
    long i;
    long print_from;

    if (n <= 0)
        return;

    while ((bytes_read = fread(buf, 1, BUFSIZE, fp)) > 0) {
        if (data_size + bytes_read > data_cap) {
            data_cap = data_cap ? data_cap * 2 : BUFSIZE;
            data = realloc(data, data_cap);
            if (!data) {
                fprintf(stderr, "%s: out of memory\n", progname);
                return;
            }
        }
        memcpy(data + data_size, buf, bytes_read);
        data_size += bytes_read;
    }

    for (i = 0; i < (long)data_size; i++) {
        if (data[i] == '\n')
            total_lines++;
    }

    if (total_lines <= n) {
        if (data_size > 0)
            fwrite(data, 1, data_size, stdout);
    } else {
        print_from = 0;
        long found = 0;
        for (i = 0; i < (long)data_size; i++) {
            if (data[i] == '\n') {
                found++;
                if (found == total_lines - n + 1) {
                    print_from = i + 1;
                    break;
                }
            }
        }
        if (print_from < (long)data_size)
            fwrite(data + print_from, 1, data_size - print_from, stdout);
    }

    free(data);
}

static void tail_bytes(FILE *fp, long n) {
    struct stat st;
    long file_size, start;

    if (n <= 0)
        return;

    if (fstat(fileno(fp), &st) == 0 && S_ISREG(st.st_mode)) {
        file_size = st.st_size;
        start = (file_size > n) ? file_size - n : 0;
        fseek(fp, start, SEEK_SET);
    } else {
        char *ring;
        long ring_size = n;
        long pos = 0, total = 0;
        size_t bytes_read;

        ring = malloc(ring_size);
        if (!ring) {
            fprintf(stderr, "%s: out of memory\n", progname);
            return;
        }

        while ((bytes_read = fread(ring + pos, 1, ring_size - pos, fp)) > 0) {
            pos = (pos + bytes_read) % ring_size;
            total += bytes_read;
        }

        if (total < ring_size) {
            fwrite(ring, 1, total, stdout);
        } else {
            fwrite(ring + pos, 1, ring_size - pos, stdout);
            fwrite(ring, 1, pos, stdout);
        }

        free(ring);
        return;
    }

    {
        char buf[BUFSIZE];
        size_t bytes_read;
        while ((bytes_read = fread(buf, 1, BUFSIZE, fp)) > 0)
            fwrite(buf, 1, bytes_read, stdout);
    }
}

static void tail_file(const char *filename, int multi_file) {
    FILE *fp;
    int fd;
    int show_header = should_show_header(multi_file);

    if (strcmp(filename, "-") == 0) {
        fp = stdin;
        if (show_header)
            print_header("standard input");
    } else {
        fd = open(filename, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "%s: cannot open '%s': ", progname, filename);
            perror("");
            return;
        }
        fp = fdopen(fd, "r");
        if (!fp) {
            close(fd);
            fprintf(stderr, "%s: cannot open '%s': ", progname, filename);
            perror("");
            return;
        }
        if (show_header)
            print_header(filename);
    }

    if (from_line > 0) {
        tail_lines_from_start(fp, from_line);
    } else if (count_lines) {
        tail_lines(fp, num_units);
    } else {
        tail_bytes(fp, num_units);
    }

    if (fp != stdin)
        fclose(fp);
}

static void follow_file(const char *filename) {
    int fd;
    FILE *fp;
    struct stat st;
    long last_size;

    if (strcmp(filename, "-") == 0) {
        char buf[BUFSIZE];
        size_t bytes_read;
        while (1) {
            bytes_read = fread(buf, 1, BUFSIZE, stdin);
            if (bytes_read > 0)
                fwrite(buf, 1, bytes_read, stdout);
            else
                usleep(1000000);
        }
        return;
    }

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "%s: cannot open '%s': ", progname, filename);
        perror("");
        return;
    }

    fp = fdopen(fd, "r");
    if (!fp) {
        close(fd);
        return;
    }

    if (fstat(fd, &st) == 0)
        last_size = st.st_size;
    else
        last_size = 0;

    while (1) {
        char buf[BUFSIZE];
        size_t bytes_read;

        while ((bytes_read = fread(buf, 1, BUFSIZE, fp)) > 0)
            fwrite(buf, 1, bytes_read, stdout);

        fflush(stdout);

        if (fstat(fd, &st) == 0) {
            if (st.st_size < last_size) {
                rewind(fp);
            }
            last_size = st.st_size;
        }

        usleep(1000000);
    }
}

static void parse_num_arg(const char *arg, const char *optname) {
    char *endptr;
    num_units = strtol(arg, &endptr, 10);
    if (*endptr != '\0' || num_units <= 0) {
        fprintf(stderr, "%s: invalid number for %s: '%s'\n",
                progname, optname, arg);
        usage();
    }
    if (arg[0] == '+') {
        from_line = num_units;
        num_units = DEFAULT_LINES;
    }
}

int main(int argc, char *argv[]) {
    int i, opt;
    int new_argc;
    char *new_argv[256];

    progname = argv[0];

    new_argc = 1;
    new_argv[0] = argv[0];

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--lines=", 8) == 0) {
            parse_num_arg(argv[i] + 8, "--lines");
            count_lines = 1;
        } else if (strncmp(argv[i], "--bytes=", 8) == 0) {
            parse_num_arg(argv[i] + 8, "--bytes");
            count_lines = 0;
        } else if (strcmp(argv[i], "--follow") == 0) {
            follow_mode = 1;
        } else {
            new_argv[new_argc++] = argv[i];
        }
    }
    new_argv[new_argc] = NULL;

    while ((opt = getopt(new_argc, new_argv, "n:c:fqvh")) != -1) {
        switch (opt) {
        case 'n':
            parse_num_arg(optarg, "-n");
            count_lines = 1;
            break;
        case 'c':
            parse_num_arg(optarg, "-c");
            count_lines = 0;
            break;
        case 'f':
            follow_mode = 1;
            break;
        case 'q':
            header_mode = 0;
            break;
        case 'v':
            header_mode = 1;
            break;
        case 'h':
            usage();
            break;
        default:
            usage();
            break;
        }
    }

    if (follow_mode) {
        if (optind >= new_argc) {
            follow_file("-");
        } else {
            for (i = optind; i < new_argc; i++)
                follow_file(new_argv[i]);
        }
    } else {
        if (optind >= new_argc) {
            tail_file("-", 0);
        } else {
            int nfiles = new_argc - optind;
            for (i = optind; i < new_argc; i++)
                tail_file(new_argv[i], nfiles > 1);
        }
    }

    return 0;
}
