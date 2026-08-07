#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#define INITIAL_CAP 256

typedef struct {
    char *data;
    size_t len;
    size_t idx;
} Line;

typedef struct {
    Line *lines;
    size_t count;
    size_t cap;
} LineArray;

typedef struct {
    int numeric;
    int reverse;
    int fold_case;
    int unique;
    int human_numeric;
    int stable;
    int version;
    int check_only;
    int merge_mode;
    char field_sep;
    int field_sep_set;
    int key_start;
    int key_end;
    int key_set;
    const char *outfile;
} Options;

static Options opts;

static void la_init(LineArray *la) {
    la->lines = NULL;
    la->count = 0;
    la->cap = 0;
}

static void la_free(LineArray *la) {
    for (size_t i = 0; i < la->count; i++)
        free(la->lines[i].data);
    free(la->lines);
}

static int la_push(LineArray *la, const char *data, size_t len) {
    if (la->count >= la->cap) {
        size_t newcap = la->cap ? la->cap * 2 : INITIAL_CAP;
        Line *p = realloc(la->lines, newcap * sizeof(Line));
        if (!p) return -1;
        la->lines = p;
        la->cap = newcap;
    }
    char *d = malloc(len + 1);
    if (!d) return -1;
    memcpy(d, data, len);
    d[len] = '\0';
    la->lines[la->count].data = d;
    la->lines[la->count].len = len;
    la->lines[la->count].idx = la->count;
    la->count++;
    return 0;
}

static const char *get_field(const char *line, int field_num, size_t *flen) {
    char sep = opts.field_sep_set ? opts.field_sep : '\t';
    const char *p = line;
    int field = 1;

    while (field < field_num && *p) {
        const char *f = memchr(p, sep, strlen(p));
        if (!f) break;
        p = f + 1;
        field++;
    }

    const char *start = p;
    const char *end;
    if (opts.field_sep_set) {
        end = strchr(p, sep);
        if (!end) end = p + strlen(p);
    } else {
        end = p + strlen(p);
    }

    *flen = (size_t)(end - start);
    return start;
}

static const char *get_sort_key(const char *line, size_t *klen) {
    if (!opts.key_set) {
        *klen = strlen(line);
        return line;
    }
    return get_field(line, opts.key_start, klen);
}

static long parse_human(const char *s, size_t len) {
    const char *end = s + len;
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    const char *p = s;
    while (p < end && isspace((unsigned char)*p)) p++;

    double val = 0;
    int neg = 0;
    if (p < end && *p == '-') { neg = 1; p++; }
    else if (p < end && *p == '+') p++;

    while (p < end && *p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    if (p < end && *p == '.') {
        p++;
        double frac = 0.1;
        while (p < end && *p >= '0' && *p <= '9') {
            val += (*p - '0') * frac;
            frac *= 0.1;
            p++;
        }
    }

    while (p < end && *p == ' ') p++;

    long mult = 1;
    if (p < end) {
        switch (*p) {
            case 'k': case 'K': mult = 1024; break;
            case 'm': case 'M': mult = 1024L * 1024; break;
            case 'g': case 'G': mult = 1024L * 1024 * 1024; break;
            case 't': case 'T': mult = 1024L * 1024 * 1024 * 1024; break;
            case 'P': mult = 1024L * 1024 * 1024 * 1024 * 1024; break;
            case 'E': mult = 1024L * 1024 * 1024 * 1024 * 1024 * 1024; break;
        }
    }

    long result = (long)(val * mult);
    return neg ? -result : result;
}

static int version_compare(const char *a, const char *b) {
    while (*a && *b) {
        while (*a && !isdigit((unsigned char)*a) && *a != '.') a++;
        while (*b && !isdigit((unsigned char)*b) && *b != '.') b++;

        if (!*a && !*b) return 0;
        if (!*a) return -1;
        if (!*b) return 1;

        if (*a == '.' && *b == '.') { a++; b++; continue; }
        if (*a == '.') return -1;
        if (*b == '.') return 1;

        unsigned long na = 0, nb = 0;
        while (*a >= '0' && *a <= '9') { na = na * 10 + (*a - '0'); a++; }
        while (*b >= '0' && *b <= '9') { nb = nb * 10 + (*b - '0'); b++; }

        if (na < nb) return -1;
        if (na > nb) return 1;
    }

    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

static int compare_lines(const void *va, const void *vb) {
    const Line *la = (const Line *)va;
    const Line *lb = (const Line *)vb;
    size_t ka_len, kb_len;
    const char *ka = get_sort_key(la->data, &ka_len);
    const char *kb = get_sort_key(lb->data, &kb_len);

    int cmp = 0;

    if (opts.numeric) {
        char *ea, *eb;
        double na = strtod(ka, &ea);
        double nb = strtod(kb, &eb);
        if (na < nb) cmp = -1;
        else if (na > nb) cmp = 1;
        else cmp = 0;
    } else if (opts.human_numeric) {
        long na = parse_human(ka, ka_len);
        long nb = parse_human(kb, kb_len);
        cmp = (na > nb) - (na < nb);
    } else if (opts.version) {
        cmp = version_compare(ka, kb);
    } else if (opts.fold_case) {
        size_t minlen = ka_len < kb_len ? ka_len : kb_len;
        cmp = strncasecmp(ka, kb, minlen);
        if (cmp == 0)
            cmp = (ka_len > kb_len) - (ka_len < kb_len);
    } else {
        size_t minlen = ka_len < kb_len ? ka_len : kb_len;
        cmp = memcmp(ka, kb, minlen);
        if (cmp == 0)
            cmp = (ka_len > kb_len) - (ka_len < kb_len);
    }

    if (opts.reverse) cmp = -cmp;
    if (cmp == 0 && opts.stable)
        cmp = (la->idx > lb->idx) - (la->idx < lb->idx);

    return cmp;
}

static int compare_lines_qsort(const void *va, const void *vb) {
    return compare_lines(va, vb);
}

static int read_file(LineArray *la, const char *path) {
    FILE *f;
    if (!path || strcmp(path, "-") == 0) {
        f = stdin;
    } else {
        f = fopen(path, "r");
        if (!f) {
            fprintf(stderr, "sort: %s: %s\n", path, strerror(errno));
            return 1;
        }
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    while ((len = getline(&line, &cap, f)) != -1) {
        while (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
        }
        if (la_push(la, line, len) < 0) {
            fprintf(stderr, "sort: out of memory\n");
            free(line);
            if (f != stdin) fclose(f);
            return 1;
        }
    }

    free(line);
    if (f != stdin) fclose(f);
    return 0;
}

static int do_merge(int argc, char *argv[]) {
    FILE **fps = calloc(argc, sizeof(FILE *));
    LineArray bufs;
    la_init(&bufs);

    if (!fps) {
        fprintf(stderr, "sort: out of memory\n");
        return 1;
    }

    for (int i = 0; i < argc; i++) {
        fps[i] = fopen(argv[i], "r");
        if (!fps[i]) {
            fprintf(stderr, "sort: %s: %s\n", argv[i], strerror(errno));
            for (int j = 0; j < i; j++) fclose(fps[j]);
            free(fps);
            return 1;
        }
        la_push(&bufs, "", 0);
        char *line = NULL;
        size_t cap = 0;
        ssize_t len;
        if ((len = getline(&line, &cap, fps[i])) != -1) {
            while (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
            free(bufs.lines[i].data);
            char *d = malloc(len + 1);
            memcpy(d, line, len);
            d[len] = '\0';
            bufs.lines[i].data = d;
            bufs.lines[i].len = len;
            bufs.lines[i].idx = i;
        } else {
            free(bufs.lines[i].data);
            bufs.lines[i].data = NULL;
            bufs.lines[i].len = 0;
        }
        free(line);
    }

    while (1) {
        int best = -1;
        for (int i = 0; i < argc; i++) {
            if (!bufs.lines[i].data) continue;
            if (best == -1 || compare_lines(&bufs.lines[i], &bufs.lines[best]) < 0)
                best = i;
        }
        if (best == -1) break;

        printf("%s\n", bufs.lines[best].data);

        char *line = NULL;
        size_t cap = 0;
        ssize_t len;
        if ((len = getline(&line, &cap, fps[best])) != -1) {
            while (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
            free(bufs.lines[best].data);
            char *d = malloc(len + 1);
            memcpy(d, line, len);
            d[len] = '\0';
            bufs.lines[best].data = d;
            bufs.lines[best].len = len;
        } else {
            free(bufs.lines[best].data);
            bufs.lines[best].data = NULL;
            bufs.lines[best].len = 0;
        }
        free(line);
    }

    for (int i = 0; i < argc; i++) fclose(fps[i]);
    free(fps);
    la_free(&bufs);
    return 0;
}

static int do_check(LineArray *la) {
    for (size_t i = 1; i < la->count; i++) {
        if (compare_lines(&la->lines[i - 1], &la->lines[i]) > 0) {
            fprintf(stderr, "sort: %s: disorder on line %zu\n",
                    "(input)", i + 1);
            return 1;
        }
    }
    return 0;
}

static void output_lines(LineArray *la, FILE *out) {
    for (size_t i = 0; i < la->count; i++) {
        fputs(la->lines[i].data, out);
        fputc('\n', out);
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [-rnhfucmsV] [-t CHAR] [-k KEY] [-o FILE] [-m FILE...] [FILE...]\n"
        "  -r          reverse sort\n"
        "  -n          numeric sort\n"
        "  -h          human numeric sort (e.g. 1K 2M 3G)\n"
        "  -f          fold case (ignore case)\n"
        "  -u          unique lines only\n"
        "  -t CHAR     field separator character\n"
        "  -k KEY      sort key (field number, 1-based)\n"
        "  -o FILE     output to FILE instead of stdout\n"
        "  -m          merge sorted files\n"
        "  -c          check for sorted input\n"
        "  -s          stable sort (preserve original order)\n"
        "  -V          version sort\n", prog);
}

static size_t unique_lines(LineArray *la) {
    if (la->count <= 1) return la->count;
    size_t w = 1;
    for (size_t i = 1; i < la->count; i++) {
        if (compare_lines(&la->lines[i], &la->lines[w - 1]) != 0) {
            if (w != i) {
                la->lines[w] = la->lines[i];
            }
            w++;
        } else {
            la->lines[i].data = NULL;
        }
    }
    la->count = w;
    return w;
}

int main(int argc, char *argv[]) {
    opts.field_sep = '\t';

    int opt;
    while ((opt = getopt(argc, argv, "rnhfucmsVt:k:o:")) != -1) {
        switch (opt) {
            case 'r': opts.reverse = 1; break;
            case 'n': opts.numeric = 1; break;
            case 'h': opts.human_numeric = 1; break;
            case 'f': opts.fold_case = 1; break;
            case 'u': opts.unique = 1; break;
            case 's': opts.stable = 1; break;
            case 'V': opts.version = 1; break;
            case 'c': opts.check_only = 1; break;
            case 'm': opts.merge_mode = 1; break;
            case 't':
                opts.field_sep = optarg[0];
                opts.field_sep_set = 1;
                break;
            case 'k': {
                opts.key_start = atoi(optarg);
                opts.key_end = opts.key_start;
                opts.key_set = 1;
                break;
            }
            case 'o':
                opts.outfile = optarg;
                break;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (opts.merge_mode) {
        int nfiles = argc - optind;
        if (nfiles < 1) {
            fprintf(stderr, "sort: -m requires at least one file argument\n");
            return 1;
        }
        return do_merge(nfiles, argv + optind);
    }

    LineArray lines;
    la_init(&lines);

    int has_files = 0;
    for (int i = optind; i < argc; i++) {
        if (read_file(&lines, argv[i]) != 0) {
            la_free(&lines);
            return 1;
        }
        has_files = 1;
    }

    if (!has_files) {
        if (read_file(&lines, NULL) != 0) {
            la_free(&lines);
            return 1;
        }
    }

    if (opts.check_only) {
        int r = do_check(&lines);
        la_free(&lines);
        return r;
    }

    qsort(lines.lines, lines.count, sizeof(Line), compare_lines_qsort);

    if (opts.unique)
        lines.count = unique_lines(&lines);

    FILE *out = stdout;
    if (opts.outfile) {
        out = fopen(opts.outfile, "w");
        if (!out) {
            fprintf(stderr, "sort: %s: %s\n", opts.outfile, strerror(errno));
            la_free(&lines);
            return 1;
        }
    }

    output_lines(&lines, out);

    if (out != stdout) fclose(out);
    la_free(&lines);
    return 0;
}
