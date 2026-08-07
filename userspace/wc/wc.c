#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <locale.h>
#include <wchar.h>

#define BUF_SIZE 65536

enum {
    F_LINES  = 1 << 0,
    F_WORDS  = 1 << 1,
    F_BYTES  = 1 << 2,
    F_CHARS  = 1 << 3,
    F_MAXLINE = 1 << 4,
};

typedef struct {
    long lines;
    long words;
    long bytes;
    long chars;
    long maxline;
    long curlen;
} Counts;

static int flags = F_LINES | F_WORDS | F_BYTES;
static int show_total = 0;

static void print_counts(const Counts *c, const char *name) {
    if (flags & F_LINES)  printf("%7ld", c->lines);
    if (flags & F_WORDS)  printf("%7ld", c->words);
    if (flags & F_CHARS)  printf("%7ld", c->chars);
    if (flags & F_BYTES)  printf("%7ld", c->bytes);
    if (flags & F_MAXLINE) printf("%7ld", c->maxline);
    if (name) printf(" %s", name);
    putchar('\n');
}

static void add_counts(Counts *dst, const Counts *src) {
    dst->lines   += src->lines;
    dst->words   += src->words;
    dst->bytes   += src->bytes;
    dst->chars   += src->chars;
    if (src->maxline > dst->maxline) dst->maxline = src->maxline;
}

static int wc_fd(int fd, const char *name, Counts *out) {
    char buf[BUF_SIZE];
    ssize_t n;
    int in_word = 0;

    memset(out, 0, sizeof(*out));

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        out->bytes += n;

        for (ssize_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)buf[i];

            if (c == '\n') {
                out->lines++;
                if (out->curlen > out->maxline)
                    out->maxline = out->curlen;
                out->curlen = 0;
            } else {
                out->curlen++;
            }

            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                out->words++;
            }
        }

        if (flags & F_CHARS) {
            const char *p = buf;
            const char *end = buf + n;
            mbstate_t st;
            memset(&st, 0, sizeof(st));
            while (p < end) {
                wchar_t wc;
                size_t len = mbrtowc(&wc, p, end - p, &st);
                if (len == 0 || len == (size_t)-1 || len == (size_t)-2)
                    break;
                out->chars++;
                p += len;
            }
        }
    }

    if (out->curlen > out->maxline)
        out->maxline = out->curlen;

    if (n < 0) {
        fprintf(stderr, "wc: %s: %s\n", name ? name : "stdin", strerror(errno));
        return 1;
    }
    return 0;
}

static void usage(void) {
    fprintf(stderr, "Usage: wc [-lwcmmL] [--total] [file ...]\n");
    exit(1);
}

int main(int argc, char *argv[]) {
    Counts total;
    int multiple = 0;

    memset(&total, 0, sizeof(total));

    setlocale(LC_ALL, "");

    int i;
    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
        const char *p = argv[i] + 1;
        if (*p == '-' && *(p + 1) == '\0') {
            i++;
            break;
        }
        if (*p == '\0') {
            usage();
        }
        while (*p) {
            switch (*p) {
            case 'l': flags |= F_LINES; flags &= ~(F_BYTES); break;
            case 'w': flags |= F_WORDS; flags &= ~(F_BYTES); break;
            case 'c': flags |= F_BYTES; break;
            case 'm': flags |= F_CHARS; flags &= ~(F_BYTES); break;
            case 'L': flags |= F_MAXLINE; break;
            case '-':
                if (strcmp(p + 1, "total") == 0) { show_total = 1; goto next_arg; }
                if (strcmp(p + 1, "lines") == 0)  { flags |= F_LINES; goto next_arg; }
                if (strcmp(p + 1, "words") == 0)  { flags |= F_WORDS; goto next_arg; }
                if (strcmp(p + 1, "bytes") == 0)  { flags |= F_BYTES; goto next_arg; }
                if (strcmp(p + 1, "chars") == 0)  { flags |= F_CHARS; flags &= ~(F_BYTES); goto next_arg; }
                if (strcmp(p + 1, "max-line-length") == 0) { flags |= F_MAXLINE; goto next_arg; }
                fprintf(stderr, "wc: unknown option -- %s\n", p);
                usage(); /* fall through */
            default:
                fprintf(stderr, "wc: invalid option -- '%c'\n", *p);
                usage();
            }
            p++;
        }
        next_arg:;
    }

    int file_count = argc - i;
    if (file_count > 1) multiple = 1;

    if (file_count == 0) {
        Counts c;
        if (wc_fd(STDIN_FILENO, NULL, &c)) return 1;
        print_counts(&c, NULL);
        return 0;
    }

    for (; i < argc; i++) {
        Counts c;
        int fd;

        if (strcmp(argv[i], "-") == 0) {
            fd = STDIN_FILENO;
            if (wc_fd(fd, "stdin", &c)) return 1;
        } else {
            fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno));
                return 1;
            }
            if (wc_fd(fd, argv[i], &c)) { close(fd); return 1; }
            close(fd);
        }

        print_counts(&c, multiple ? argv[i] : (file_count > 1 ? argv[i] : NULL));
        add_counts(&total, &c);
    }

    if (show_total || multiple) {
        if (show_total && multiple)
            print_counts(&total, "total");
        else if (multiple)
            print_counts(&total, "total");
    }

    return 0;
}
