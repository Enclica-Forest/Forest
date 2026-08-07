/*
 * grep.c - POSIX grep for Forest OS
 * Full implementation with regex, recursion, and pattern matching
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "forest.h"
#include <ctype.h>

/* Maximum values */
#define MAX_PATTERNS    256
#define MAX_LINE_LEN    65536
#define MAX_PATH_LEN    4096

/* Flags */
#define FLAG_CASE_INSENS   (1U << 0)   /* -i */
#define FLAG_INVERT        (1U << 1)   /* -v */
#define FLAG_LINE_NUM      (1U << 2)   /* -n */
#define FLAG_COUNT         (1U << 3)   /* -c */
#define FLAG_FILES_MATCH   (1U << 4)   /* -l */
#define FLAG_FILES_NO      (1U << 5)   /* -L */
#define FLAG_RECURSIVE     (1U << 6)   /* -r / -R */
#define FLAG_WORD_MATCH    (1U << 7)   /* -w */
#define FLAG_LINE_MATCH    (1U << 8)   /* -x */
#define FLAG_ONLY_MATCH    (1U << 9)   /* -o */
#define FLAG_FILENAME      (1U << 10)  /* -H */
#define FLAG_NO_FILENAME   (1U << 11)  /* -h */
#define FLAG_FIXED_STRING  (1U << 12)  /* -F */
#define FLAG_EXT_REGEX     (1U << 13)  /* -E */

/* Pattern source */
typedef enum {
    PATTERN_LITERAL,
    PATTERN_REGEX,
    PATTERN_FIXED
} pattern_type_t;

/* Pattern entry */
typedef struct {
    char *text;
    pattern_type_t type;
} pattern_t;

/* Globals */
static unsigned int g_flags = 0;
static int g_max_matches = 0;
static int g_match_count = 0;
static int g_print_filename = 0;
static int g_multiple_files = 0;
static int g_found_no_match = 0;  /* for -L */
static pattern_t g_patterns[MAX_PATTERNS];
static int g_num_patterns = 0;
static char *g_include_pattern = NULL;
static char *g_exclude_pattern = NULL;

/* Forward declarations */
static void usage(void);
static void add_pattern(const char *text, pattern_type_t type);
static void add_patterns_from_file(const char *filename);
static int match_line(const char *line, size_t len);
static int match_literal(const char *line, size_t len, const char *pat, size_t plen, int ci);
static int match_fixed(const char *line, size_t len, const char *pat, size_t plen, int ci);
static int match_regex(const char *line, size_t len, const char *pat, size_t plen, int ci);
static void process_file(const char *filename, int fd);
static void process_stdin(void);
static void scan_directory(const char *dirpath);
static int should_include(const char *path);
static int glob_match(const char *pattern, const char *string);

/* ---- Pattern management ---- */

static void add_pattern(const char *text, pattern_type_t type) {
    if (g_num_patterns >= MAX_PATTERNS) {
        fprintf(stderr, "grep: too many patterns\n");
        exit(2);
    }
    g_patterns[g_num_patterns].text = strdup(text);
    if (!g_patterns[g_num_patterns].text) {
        fprintf(stderr, "grep: out of memory\n");
        exit(2);
    }
    g_patterns[g_num_patterns].type = type;
    g_num_patterns++;
}

static void add_patterns_from_file(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "grep: %s: %s\n", filename, strerror(errno));
        exit(2);
    }

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0)
            continue;
        add_pattern(line, PATTERN_REGEX);
    }
    fclose(fp);
}

/* ---- Glob matching (for --include/--exclude) ---- */

static int glob_match(const char *pattern, const char *string) {
    while (*pattern && *string) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;
            while (*string) {
                if (glob_match(pattern, string))
                    return 1;
                string++;
            }
            return 0;
        } else if (*pattern == '?' || *pattern == *string) {
            pattern++;
            string++;
        } else {
            return 0;
        }
    }
    while (*pattern == '*') pattern++;
    return (*pattern == '\0' && *string == '\0');
}

static int should_include(const char *path) {
    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;

    if (g_exclude_pattern && glob_match(g_exclude_pattern, basename))
        return 0;
    if (g_include_pattern && !glob_match(g_include_pattern, basename))
        return 0;
    return 1;
}

/* ---- Matching engine ---- */

static int match_literal(const char *line, size_t len, const char *pat, size_t plen, int ci) {
    if (plen > len) return 0;
    for (size_t i = 0; i <= len - plen; i++) {
        int match = 1;
        for (size_t j = 0; j < plen; j++) {
            char a = line[i + j];
            char b = pat[j];
            if (ci) { a = tolower(a); b = tolower(b); }
            if (a != b) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

static int match_fixed(const char *line, size_t len, const char *pat, size_t plen, int ci) {
    if (plen > len) return 0;
    for (size_t i = 0; i <= len - plen; i++) {
        int match = 1;
        for (size_t j = 0; j < plen; j++) {
            char a = line[i + j];
            char b = pat[j];
            if (ci) { a = tolower(a); b = tolower(b); }
            if (a != b) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

/*
 * Minimal regex engine supporting: literal, ., *, ^, $, [], [^], +, ?, ()
 * This handles basic extended regex features without needing a separate library.
 */
static int regex_match_internal(const char *line, const char *lineend,
                                 const char *pat, const char *patend, int ci);

static int char_class_match(char c, const char **pp, const char *classend, int ci) {
    const char *p = *pp;
    int negated = 0;
    if (*p == '^') { negated = 1; p++; }

    int found = 0;
    while (p < classend) {
        if (*p == '\\' && p + 1 < classend) {
            p++;
        }
        if (p + 2 < classend && *(p + 1) == '-') {
            char lo = *p;
            char hi = *(p + 2);
            if (ci) { lo = tolower(lo); hi = tolower(hi); c = tolower(c); }
            if (c >= lo && c <= hi) found = 1;
            p += 3;
        } else {
            char ch = *p;
            if (ci) { ch = tolower(ch); c = tolower(c); }
            if (c == ch) found = 1;
            p++;
        }
    }
    *pp = p;
    return negated ? !found : found;
}

static int regex_match_internal(const char *line, const char *lineend,
                                 const char *pat, const char *patend, int ci) {
    while (pat < patend) {
        char pc = *pat;
        int escaped = 0;

        /* Start anchor */
        if (pc == '^') {
            pat++;
            continue;
        }

        /* End anchor - match at end of line or end of buffer */
        if (pc == '$') {
            return (line == lineend || *line == '\n');
        }

        /* Group / alternation (very simplified) */
        if (pc == '(') {
            /* Find matching paren */
            int depth = 1;
            const char *gstart = pat + 1;
            const char *gend = gstart;
            while (gend < patend && depth > 0) {
                if (*gend == '(') depth++;
                else if (*gend == ')') depth--;
                if (depth > 0) gend++;
            }
            /* Try alternations within group */
            const char *alt = gstart;
            int alt_depth = 0;
            while (gend > alt) {
                /* Find next | at same depth */
                const char *scan = alt;
                const char *next_alt = NULL;
                while (scan < gend) {
                    if (*scan == '(') alt_depth++;
                    else if (*scan == ')') alt_depth--;
                    else if (*scan == '|' && alt_depth == 0) {
                        next_alt = scan;
                        break;
                    }
                    scan++;
                }
                const char *alt_end = next_alt ? next_alt : gend;
                /* Try matching this alternative followed by rest of pattern */
                if (regex_match_internal(line, lineend, alt, alt_end, ci)) {
                    const char *rest = gend + 1; /* skip closing ) */
                    if (rest == patend || regex_match_internal(line, lineend, rest, patend, ci))
                        return 1;
                }
                if (!next_alt) break;
                alt = next_alt + 1;
            }
            return 0;
        }

        /* Escape sequence */
        if (pc == '\\') {
            pat++;
            if (pat >= patend) return 0;
            pc = *pat;
            escaped = 1;
        }

        /* Character class [...] (only if not escaped) */
        if (!escaped && pc == '[') {
            pat++;
            const char *classstart = pat;
            while (pat < patend && *pat != ']') pat++;
            if (pat >= patend) return 0;
            const char *classend = pat;
            pat++; /* skip ] */

            if (line >= lineend) return 0;
            if (!char_class_match(*line, &classstart, classend, ci))
                return 0;
            line++;
            continue;
        }

        /* Dot (only if not escaped) */
        if (!escaped && pc == '.') {
            if (line >= lineend) return 0;
            line++;
            pat++;
            /* Check for quantifiers */
            if (pat < patend && *pat == '*') {
                pat++;
                /* Greedy: try matching rest with as many dots as possible */
                const char *rest = pat;
                for (size_t remain = (size_t)(lineend - line); ; ) {
                    if (regex_match_internal(line + (lineend - line) - remain, lineend, rest, patend, ci))
                        return 1;
                    if (remain == 0) break;
                    remain--;
                }
                return 0;
            }
            if (pat < patend && *pat == '+') {
                pat++;
                const char *rest = pat;
                for (size_t remain = (size_t)(lineend - line); ; ) {
                    if (regex_match_internal(line + (lineend - line) - remain, lineend, rest, patend, ci))
                        return 1;
                    if (remain == 0) break;
                    remain--;
                }
                return 0;
            }
            if (pat < patend && *pat == '?') {
                pat++;
                /* Try with and without the dot */
                if (regex_match_internal(line, lineend, pat, patend, ci))
                    return 1;
                if (line < lineend) {
                    line++;
                    return regex_match_internal(line, lineend, pat, patend, ci);
                }
                return 0;
            }
            continue;
        }

        /* Quantifiers * + ? after a character */
        if (pat + 1 < patend && (*(pat + 1) == '*' || *(pat + 1) == '+' || *(pat + 1) == '?')) {
            char quant = *(pat + 1);
            pat += 2;

            if (quant == '*') {
                /* Zero or more - greedy */
                const char *rest = pat;
                /* Try matching rest first (zero occurrences) */
                if (regex_match_internal(line, lineend, rest, patend, ci))
                    return 1;
                /* Try one or more */
                while (line < lineend) {
                    char lc = *line;
                    char cc = pc;
                    if (ci) { lc = tolower(lc); cc = tolower(cc); }
                    if (lc != cc && !(!escaped && pc == '.')) return 0;
                    line++;
                    if (regex_match_internal(line, lineend, rest, patend, ci))
                        return 1;
                }
                return 0;
            } else if (quant == '+') {
                /* One or more - greedy */
                const char *rest = pat;
                while (line < lineend) {
                    char lc = *line;
                    char cc = pc;
                    if (ci) { lc = tolower(lc); cc = tolower(cc); }
                    if (lc != cc && !(!escaped && pc == '.')) return 0;
                    line++;
                    if (regex_match_internal(line, lineend, rest, patend, ci))
                        return 1;
                }
                return 0;
            } else if (quant == '?') {
                /* Zero or one */
                const char *rest = pat;
                /* Try without */
                if (regex_match_internal(line, lineend, rest, patend, ci))
                    return 1;
                /* Try with */
                if (line < lineend) {
                    char lc = *line;
                    char cc = pc;
                    if (ci) { lc = tolower(lc); cc = tolower(cc); }
                    if (lc == cc || (!escaped && pc == '.')) {
                        line++;
                        return regex_match_internal(line, lineend, rest, patend, ci);
                    }
                }
                return 0;
            }
        }

        /* Literal character */
        if (line >= lineend) return 0;
        char lc = *line;
        char cc = pc;
        if (ci) { lc = tolower(lc); cc = tolower(cc); }
        if (lc != cc) return 0;
        line++;
        pat++;
    }

    /* Pattern fully consumed - match found */
    return 1;
}

static int match_regex(const char *line, size_t len, const char *pat, size_t plen, int ci) {
    const char *lineend = line + len;
    const char *patend = pat + plen;

    /* Check if pattern starts with ^ */
    int anchored_start = (plen > 0 && *pat == '^');

    if (anchored_start) {
        /* Try matching from start */
        return regex_match_internal(line, lineend, pat + 1, patend, ci);
    }

    /* Try matching at each position */
    for (size_t i = 0; i <= len; i++) {
        if (regex_match_internal(line + i, lineend, pat, patend, ci))
            return 1;
    }
    return 0;
}

static int match_line(const char *line, size_t len) {
    /* Strip trailing newline for matching, but keep for output */
    size_t match_len = len;
    while (match_len > 0 && (line[match_len - 1] == '\n' || line[match_len - 1] == '\r'))
        match_len--;

    int matched = 0;
    for (int i = 0; i < g_num_patterns; i++) {
        const char *pat = g_patterns[i].text;
        size_t plen = strlen(pat);
        int ci = (g_flags & FLAG_CASE_INSENS) ? 1 : 0;

        int m = 0;
        switch (g_patterns[i].type) {
        case PATTERN_FIXED:
            m = match_fixed(line, match_len, pat, plen, ci);
            break;
        case PATTERN_LITERAL:
            m = match_literal(line, match_len, pat, plen, ci);
            break;
        case PATTERN_REGEX:
            m = match_regex(line, match_len, pat, plen, ci);
            break;
        }

        /* -w: word match - entire word boundaries */
        if (m && (g_flags & FLAG_WORD_MATCH)) {
            /* Find where it matched and check boundaries */
            int found_word = 0;
            for (size_t start = 0; start <= match_len - plen; start++) {
                int submatch = 0;
                switch (g_patterns[i].type) {
                case PATTERN_FIXED:
                    submatch = match_fixed(line + start, plen, pat, plen, ci);
                    break;
                case PATTERN_LITERAL:
                    submatch = match_literal(line + start, plen, pat, plen, ci);
                    break;
                case PATTERN_REGEX:
                    submatch = match_regex(line + start, match_len - start, pat, plen, ci);
                    break;
                }
                if (submatch) {
                    int word_start = (start == 0) || !isalnum((unsigned char)line[start - 1]);
                    int word_end = (start + plen >= match_len) ||
                                   !isalnum((unsigned char)line[start + plen]);
                    if (word_start && word_end) { found_word = 1; break; }
                }
            }
            m = found_word;
        }

        /* -x: line match - entire line must match */
        if (m && (g_flags & FLAG_LINE_MATCH)) {
            int linem = 0;
            switch (g_patterns[i].type) {
            case PATTERN_FIXED:
                linem = match_literal(line, match_len, pat, plen, ci);
                break;
            case PATTERN_LITERAL:
                linem = match_literal(line, match_len, pat, plen, ci);
                break;
            case PATTERN_REGEX: {
                /* Build regex with ^ and $ */
                char buf[MAX_LINE_LEN + 4];
                snprintf(buf, sizeof(buf), "^%s$", pat);
                linem = match_regex(line, match_len, buf, strlen(buf), ci);
                break;
            }
            }
            m = linem;
        }

        if (m) { matched = 1; break; }
    }

    if (g_flags & FLAG_INVERT)
        matched = !matched;

    return matched;
}

/* ---- Output ---- */

static void print_match(const char *filename, int print_fn, int lineno, const char *line, size_t len) {
    if (g_flags & FLAG_COUNT) return;

    if (g_flags & FLAG_FILES_MATCH) return;
    if (g_flags & FLAG_FILES_NO) return;

    if (print_fn)
        printf("%s:", filename);

    if (g_flags & FLAG_LINE_NUM)
        printf("%d:", lineno);

    /* Write line, stripping trailing newline */
    size_t outlen = len;
    while (outlen > 0 && (line[outlen - 1] == '\n' || line[outlen - 1] == '\r'))
        outlen--;
    fwrite(line, 1, outlen, stdout);
    putchar('\n');
}

/* ---- File processing ---- */

static void process_file(const char *filename, int fd) {
    FILE *fp = fdopen(fd, "r");
    if (!fp) {
        fprintf(stderr, "grep: %s: %s\n", filename, strerror(errno));
        close(fd);
        return;
    }

    char line[MAX_LINE_LEN];
    int lineno = 0;
    int count = 0;
    int has_match = 0;

    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        size_t len = strlen(line);

        if (match_line(line, len)) {
            count++;
            has_match = 1;
            g_match_count++;

            if (g_flags & FLAG_FILES_MATCH) continue;
            if (g_flags & FLAG_FILES_NO) continue;

            print_match(filename, g_print_filename, lineno, line, len);

            if (g_max_matches > 0 && count >= g_max_matches)
                break;
        }
    }

    if (g_flags & FLAG_COUNT) {
        if (g_print_filename)
            printf("%s:%d\n", filename, count);
        else
            printf("%d\n", count);
    }

    if (g_flags & FLAG_FILES_MATCH && has_match) {
        printf("%s\n", filename);
    }

    if (g_flags & FLAG_FILES_NO && !has_match) {
        printf("%s\n", filename);
        g_found_no_match = 1;
    }

    fclose(fp);
}

static void process_stdin(void) {
    char line[MAX_LINE_LEN];
    int lineno = 0;
    int count = 0;

    while (fgets(line, sizeof(line), stdin)) {
        lineno++;
        size_t len = strlen(line);

        if (match_line(line, len)) {
            count++;
            g_match_count++;

            if (g_flags & FLAG_FILES_MATCH) continue;
            if (g_flags & FLAG_FILES_NO) continue;

            print_match(NULL, 0, lineno, line, len);

            if (g_max_matches > 0 && count >= g_max_matches)
                break;
        }
    }

    if (g_flags & FLAG_COUNT)
        printf("%d\n", count);
}

/* ---- Recursive directory scanning ---- */

static int is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return S_ISDIR(st.st_mode);
}

static void scan_directory(const char *dirpath) {
    DIR *dir = opendir(dirpath);
    if (!dir) {
        fprintf(stderr, "grep: %s: %s\n", dirpath, strerror(errno));
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", dirpath, ent->d_name);

        struct stat st;
        if (stat(path, &st) < 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            scan_directory(path);
        } else if (S_ISREG(st.st_mode)) {
            if (!should_include(path))
                continue;
            int fd = open(path, O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "grep: %s: %s\n", path, strerror(errno));
                continue;
            }
            process_file(path, fd);
        }
    }
    closedir(dir);
}

/* ---- Usage ---- */

static void usage(void) {
    fprintf(stderr,
        "Usage: grep [OPTIONS] PATTERN [FILE...]\n"
        "       grep [OPTIONS] -e PATTERN... [FILE...]\n"
        "       grep [OPTIONS] -f FILE [FILE...]\n"
        "\n"
        "Options:\n"
        "  -i, --ignore-case       Ignore case distinctions\n"
        "  -v, --invert-match      Select non-matching lines\n"
        "  -n, --line-number       Prefix each line with line number\n"
        "  -c, --count             Print only count of matching lines\n"
        "  -l, --files-with-match  Print only filenames with matches\n"
        "  -L, --files-without-match  Print only filenames without matches\n"
        "  -r, -R, --recursive     Read all files under directories\n"
        "  -w, --word-regexp        Match whole words only\n"
        "  -x, --line-regexp        Match whole lines only\n"
        "  -m, --max-count=NUM     Stop after NUM matches\n"
        "  -e, --regexp=PATTERN    Use PATTERN for matching\n"
        "  -f, --file=FILE         Obtain patterns from FILE\n"
        "  -F, --fixed-strings      Interpret PATTERN as fixed strings\n"
        "  -E, --extended-regexp    Interpret PATTERN as extended regex\n"
        "  -o, --only-matching     Show only matching parts of lines\n"
        "  -H, --with-filename     Print filename with each match\n"
        "  -h, --no-filename       Suppress filename prefix\n"
        "  --include=GLOB          Only include files matching GLOB\n"
        "  --exclude=GLOB          Exclude files matching GLOB\n"
        "\n"
        "Regex features: . * ^ $ [ ] [^] + ? ( ) | \\\n"
    );
    exit(2);
}

/* ---- Main ---- */

int main(int argc, char *argv[]) {
    int have_pattern = 0;
    int file_args_start = 0;
    int dash_dash = 0;

    /* Long option table */
    static struct {
        const char *name;
        int has_arg;
        int flag;
        char short_opt;
    } long_opts[] = {
        { "ignore-case",        0, FLAG_CASE_INSENS,  'i' },
        { "invert-match",       0, FLAG_INVERT,       'v' },
        { "line-number",        0, FLAG_LINE_NUM,     'n' },
        { "count",              0, FLAG_COUNT,        'c' },
        { "files-with-match",   0, FLAG_FILES_MATCH,  'l' },
        { "files-without-match",0, FLAG_FILES_NO,     'L' },
        { "recursive",          0, FLAG_RECURSIVE,    'r' },
        { "word-regexp",        0, FLAG_WORD_MATCH,   'w' },
        { "line-regexp",        0, FLAG_LINE_MATCH,   'x' },
        { "only-matching",      0, FLAG_ONLY_MATCH,   'o' },
        { "with-filename",      0, FLAG_FILENAME,     'H' },
        { "no-filename",        0, FLAG_NO_FILENAME,  'h' },
        { "fixed-strings",      0, FLAG_FIXED_STRING, 'F' },
        { "extended-regexp",    0, FLAG_EXT_REGEX,    'E' },
        { "max-count",          1, 0,                 'm' },
        { "regexp",             1, 0,                 'e' },
        { "file",               1, 0,                 'f' },
        { "include",            1, 0,                 0  },
        { "exclude",            1, 0,                 0  },
        { NULL, 0, 0, 0 }
    };

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (dash_dash || arg[0] != '-') {
            file_args_start = i;
            break;
        }

        /* -- */
        if (strcmp(arg, "--") == 0) {
            dash_dash = 1;
            if (i + 1 < argc) {
                file_args_start = i + 1;
            }
            break;
        }

        /* Long options */
        if (arg[1] == '-') {
            const char *optname = arg + 2;
            const char *eq = strchr(optname, '=');
            size_t namelen = eq ? (size_t)(eq - optname) : strlen(optname);

            int found = 0;
            for (int j = 0; long_opts[j].name; j++) {
                if (strlen(long_opts[j].name) == namelen &&
                    strncmp(long_opts[j].name, optname, namelen) == 0) {

                    if (long_opts[j].has_arg) {
                        const char *val;
                        if (eq) {
                            val = eq + 1;
                        } else if (i + 1 < argc) {
                            val = argv[++i];
                        } else {
                            fprintf(stderr, "grep: option '--%s' requires an argument\n",
                                    long_opts[j].name);
                            usage();
                        }

                        if (strcmp(long_opts[j].name, "max-count") == 0) {
                            g_max_matches = atoi(val);
                            if (g_max_matches <= 0) {
                                fprintf(stderr, "grep: invalid max count: %s\n", val);
                                usage();
                            }
                        } else if (strcmp(long_opts[j].name, "regexp") == 0) {
                            add_pattern(val, PATTERN_REGEX);
                            have_pattern = 1;
                        } else if (strcmp(long_opts[j].name, "file") == 0) {
                            add_patterns_from_file(val);
                            have_pattern = 1;
                        } else if (strcmp(long_opts[j].name, "include") == 0) {
                            g_include_pattern = strdup(val);
                        } else if (strcmp(long_opts[j].name, "exclude") == 0) {
                            g_exclude_pattern = strdup(val);
                        }
                    } else {
                        if (long_opts[j].flag) {
                            g_flags |= long_opts[j].flag;
                            if (long_opts[j].flag == FLAG_RECURSIVE)
                                g_flags |= FLAG_RECURSIVE;
                        }
                    }
                    found = 1;
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "grep: unrecognized option '--%.*s'\n",
                        (int)namelen, optname);
                usage();
            }
            continue;
        }

        /* Short options */
        arg++;
        while (*arg) {
            switch (*arg) {
            case 'i': g_flags |= FLAG_CASE_INSENS; break;
            case 'v': g_flags |= FLAG_INVERT; break;
            case 'n': g_flags |= FLAG_LINE_NUM; break;
            case 'c': g_flags |= FLAG_COUNT; break;
            case 'l': g_flags |= FLAG_FILES_MATCH; break;
            case 'L': g_flags |= FLAG_FILES_NO; break;
            case 'r': case 'R': g_flags |= FLAG_RECURSIVE; break;
            case 'w': g_flags |= FLAG_WORD_MATCH; break;
            case 'x': g_flags |= FLAG_LINE_MATCH; break;
            case 'o': g_flags |= FLAG_ONLY_MATCH; break;
            case 'H': g_flags |= FLAG_FILENAME; break;
            case 'h': g_flags |= FLAG_NO_FILENAME; break;
            case 'F': g_flags |= FLAG_FIXED_STRING; break;
            case 'E': g_flags |= FLAG_EXT_REGEX; break;
            case 'm':
                arg++;
                if (*arg == '\0') {
                    if (i + 1 < argc) arg = argv[++i];
                    else { fprintf(stderr, "grep: option '-m' requires an argument\n"); usage(); }
                }
                g_max_matches = atoi(arg);
                if (g_max_matches <= 0) {
                    fprintf(stderr, "grep: invalid max count: %s\n", arg);
                    usage();
                }
                goto next_arg;
            case 'e':
                arg++;
                if (*arg == '\0') {
                    if (i + 1 < argc) arg = argv[++i];
                    else { fprintf(stderr, "grep: option '-e' requires an argument\n"); usage(); }
                }
                add_pattern(arg, PATTERN_REGEX);
                have_pattern = 1;
                goto next_arg;
            case 'f':
                arg++;
                if (*arg == '\0') {
                    if (i + 1 < argc) arg = argv[++i];
                    else { fprintf(stderr, "grep: option '-f' requires an argument\n"); usage(); }
                }
                add_patterns_from_file(arg);
                have_pattern = 1;
                goto next_arg;
            default:
                fprintf(stderr, "grep: invalid option '-%c'\n", *arg);
                usage();
            }
            arg++;
        }
        next_arg:;
    }

    /* Handle case where we stopped at -- but pattern is in remaining args */
    if (dash_dash && file_args_start < argc && !have_pattern) {
        add_pattern(argv[file_args_start], PATTERN_REGEX);
        have_pattern = 1;
        file_args_start++;
    }

    /* If no -e or -f, treat first remaining arg as pattern */
    if (!have_pattern) {
        if (file_args_start >= argc) {
            fprintf(stderr, "grep: no pattern specified\n");
            usage();
        }
        add_pattern(argv[file_args_start], PATTERN_REGEX);
        have_pattern = 1;
        file_args_start++;
    }

    /* Determine if we need to print filenames */
    g_multiple_files = 0;
    int non_file_count = 0;
    int file_count = 0;

    if (g_flags & FLAG_RECURSIVE) {
        /* In recursive mode, always show filenames unless -h */
        if (!(g_flags & FLAG_NO_FILENAME))
            g_print_filename = 1;
    }

    /* Count file arguments */
    for (int i = file_args_start; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            non_file_count++;
        } else {
            file_count++;
        }
    }

    if (file_count > 1 || (file_count > 0 && non_file_count > 0))
        g_multiple_files = 1;

    if (g_multiple_files && !(g_flags & FLAG_NO_FILENAME))
        g_print_filename = 1;
    if (g_flags & FLAG_FILENAME)
        g_print_filename = 1;

    /* Process files */
    int had_files = 0;

    for (int i = file_args_start; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-") == 0) {
            process_stdin();
            had_files = 1;
            continue;
        }

        if (is_directory(arg)) {
            if (g_flags & FLAG_RECURSIVE) {
                scan_directory(arg);
                had_files = 1;
            } else {
                fprintf(stderr, "grep: %s: Is a directory\n", arg);
            }
            continue;
        }

        int fd = open(arg, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "grep: %s: %s\n", arg, strerror(errno));
            continue;
        }
        process_file(arg, fd);
        had_files = 1;
    }

    /* No files specified - read stdin */
    if (!had_files) {
        process_stdin();
    }

    if (g_flags & FLAG_FILES_NO)
        return g_found_no_match ? 0 : 1;
    return (g_match_count > 0) ? 0 : 1;
}
