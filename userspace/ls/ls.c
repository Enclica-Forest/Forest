#define _GNU_SOURCE

#include "forest.h"
#include <sys/sysmacros.h>
#include <pwd.h>
#include <grp.h>
#include <limits.h>

#define MAX_ENTRIES 4096

typedef struct {
    char *name;
    char *path;
    struct stat st;
} Entry;

typedef struct {
    int show_long;
    int show_all;
    int show_almost_all;
    int one_per_line;
    int recursive;
    int human;
    int sort_size;
    int sort_time;
    int reverse;
    int show_inode;
    int show_size;
    int print_dir_name;
    int is_terminal;
} Options;

static Options opts;
static int color_enabled;

static const char *color_reset  = "\033[0m";
static const char *color_dir    = "\033[1;34m";
static const char *color_link   = "\033[1;36m";
static const char *color_exec   = "\033[1;32m";
static const char *color_sock   = "\033[1;35m";
static const char *color_fifo   = "\033[33m";
static const char *color_block   = "\033[1;33;40m";
static const char *color_chardev = "\033[1;33;40m";

static const char *get_color(mode_t mode) {
    if (!color_enabled) return "";
    if (S_ISDIR(mode))    return color_dir;
    if (S_ISLNK(mode))    return color_link;
    if (S_ISREG(mode) && (mode & S_IXUSR)) return color_exec;
    if (S_ISSOCK(mode))   return color_sock;
    if (S_ISFIFO(mode))   return color_fifo;
    if (S_ISBLK(mode))    return color_block;
    if (S_ISCHR(mode))    return color_chardev;
    return "";
}

static const char *file_type_char(mode_t mode) {
    if (S_ISREG(mode))  return "-";
    if (S_ISDIR(mode))  return "d";
    if (S_ISLNK(mode))  return "l";
    if (S_ISSOCK(mode)) return "s";
    if (S_ISFIFO(mode)) return "p";
    if (S_ISBLK(mode))  return "b";
    if (S_ISCHR(mode))  return "c";
    return "?";
}

static void format_time(time_t t, char *buf, size_t buflen) {
    struct tm *tm = localtime(&t);
    time_t now = time(NULL);

    if (t > now || (now - t) > 15724800) {
        strftime(buf, buflen, "%b %e  %Y", tm);
    } else {
        strftime(buf, buflen, "%b %e %H:%M", tm);
    }
}

static void format_size(off_t size, char *buf, size_t buflen) {
    if (opts.human) {
        const char *units = "BKMGTPE";
        double s = size;
        int i = 0;
        while (s >= 1024 && i < 7) {
            s /= 1024;
            i++;
        }
        if (i == 0)
            snprintf(buf, buflen, "%lld", (long long)size);
        else if (s >= 10)
            snprintf(buf, buflen, "%.0f%c", s, units[i]);
        else
            snprintf(buf, buflen, "%.1f%c", s, units[i]);
    } else {
        snprintf(buf, buflen, "%lld", (long long)size);
    }
}

static void format_mode(mode_t mode, char *buf) {
    buf[0] = file_type_char(mode)[0];
    buf[1] = (mode & S_IRUSR) ? 'r' : '-';
    buf[2] = (mode & S_IWUSR) ? 'w' : '-';
    buf[3] = (mode & S_IXUSR) ? 'x' : '-';
    buf[4] = (mode & S_IRGRP) ? 'r' : '-';
    buf[5] = (mode & S_IWGRP) ? 'w' : '-';
    buf[6] = (mode & S_IXGRP) ? 'x' : '-';
    buf[7] = (mode & S_IROTH) ? 'r' : '-';
    buf[8] = (mode & S_IWOTH) ? 'w' : '-';
    buf[9] = (mode & S_IXOTH) ? 'x' : '-';
    if (mode & S_ISUID) buf[3] = (mode & S_IXUSR) ? 's' : 'S';
    if (mode & S_ISGID) buf[6] = (mode & S_IXGRP) ? 's' : 'S';
    if (mode & S_ISVTX) buf[9] = (mode & S_IXOTH) ? 't' : 'T';
    buf[10] = '\0';
}

static int entry_compare(const void *a, const void *b) {
    const Entry *ea = (const Entry *)a;
    const Entry *eb = (const Entry *)b;
    int cmp = 0;

    if (opts.sort_size) {
        cmp = (eb->st.st_size > ea->st.st_size) - (eb->st.st_size < ea->st.st_size);
    } else if (opts.sort_time) {
        cmp = (eb->st.st_mtime > ea->st.st_mtime) - (eb->st.st_mtime < ea->st.st_mtime);
    } else {
        cmp = strcasecmp(ea->name, eb->name);
    }

    return opts.reverse ? -cmp : cmp;
}

static int entry_compare_name(const void *a, const void *b) {
    const Entry *ea = (const Entry *)a;
    const Entry *eb = (const Entry *)b;
    int cmp = strcasecmp(ea->name, eb->name);
    return opts.reverse ? -cmp : cmp;
}

static void print_long_entry(const Entry *e) {
    char modebuf[11];
    char timebuf[64];
    char sizebuf[32];
    struct passwd *pw;
    struct group *gr;

    format_mode(e->st.st_mode, modebuf);
    format_time(e->st.st_mtime, timebuf, sizeof(timebuf));

    pw = getpwuid(e->st.st_uid);
    gr = getgrgid(e->st.st_gid);

    if (opts.show_inode)
        printf("%8lu ", (unsigned long)e->st.st_ino);

    if (opts.show_size) {
        format_size(e->st.st_size, sizebuf, sizeof(sizebuf));
        printf("%6s ", sizebuf);
    }

    printf("%s ", modebuf);
    printf("%3lu ", (unsigned long)e->st.st_nlink);
    printf("%-8s ", pw ? pw->pw_name : "???");
    printf("%-8s ", gr ? gr->gr_name : "???");

    if (S_ISBLK(e->st.st_mode) || S_ISCHR(e->st.st_mode)) {
        printf("%3lu, %2lu ", (unsigned long)major(e->st.st_rdev),
               (unsigned long)minor(e->st.st_rdev));
    } else {
        format_size(e->st.st_size, sizebuf, sizeof(sizebuf));
        printf("%6s ", sizebuf);
    }

    printf("%s %s", timebuf, e->name);

    if (S_ISLNK(e->st.st_mode)) {
        char linkbuf[PATH_MAX];
        ssize_t len = readlink(e->path, linkbuf, sizeof(linkbuf) - 1);
        if (len > 0) {
            linkbuf[len] = '\0';
            printf(" -> %s", linkbuf);
        }
    }

    printf("\n");
}

static void print_name(const char *name, const struct stat *st) {
    const char *c = get_color(st->st_mode);
    const char *end = color_enabled ? color_reset : "";

    if (S_ISLNK(st->st_mode)) {
        printf("%s%s%s -> ", c, name, end);
        char linkbuf[PATH_MAX];
        ssize_t len = readlink(name, linkbuf, sizeof(linkbuf) - 1);
        if (len > 0) {
            linkbuf[len] = '\0';
            struct stat lst;
            if (lstat(linkbuf, &lst) == 0)
                printf("%s%s%s", get_color(lst.st_mode), linkbuf, color_enabled ? color_reset : "");
            else
                printf("%s", linkbuf);
        }
    } else {
        printf("%s%s%s", c, name, end);
    }
}

static int list_entries(Entry *entries, int count) {
    if (count == 0) return 0;

    if (!opts.show_long && !opts.one_per_line && opts.is_terminal) {
        int max_name = 0;
        for (int i = 0; i < count; i++) {
            int len = strlen(entries[i].name);
            if (opts.show_inode) len += 9;
            if (len > max_name) max_name = len;
        }
        max_name += 2;

        int cols = 80 / max_name;
        if (cols < 1) cols = 1;
        int rows = (count + cols - 1) / cols;

        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int idx = c * rows + r;
                if (idx >= count) break;
                if (opts.show_inode)
                    printf("%8lu ", (unsigned long)entries[idx].st.st_ino);
                print_name(entries[idx].name, &entries[idx].st);
                int name_len = strlen(entries[idx].name);
                int padding = max_name - name_len;
                if (opts.show_inode) padding -= 9;
                for (int p = 0; p < padding && p < max_name; p++)
                    putchar(' ');
            }
            putchar('\n');
        }
        return 0;
    }

    for (int i = 0; i < count; i++) {
        if (opts.show_long) {
            print_long_entry(&entries[i]);
        } else {
            if (opts.show_inode)
                printf("%8lu ", (unsigned long)entries[i].st.st_ino);
            if (opts.show_size) {
                char sizebuf[32];
                format_size(entries[i].st.st_size, sizebuf, sizeof(sizebuf));
                printf("%6s ", sizebuf);
            }
            print_name(entries[i].name, &entries[i].st);
            if (opts.one_per_line || i == count - 1)
                putchar('\n');
            else
                printf("  ");
        }
    }
    return 0;
}

static int list_path(const char *path) {
    DIR *dir;
    struct dirent *dp;
    Entry *entries = NULL;
    int count = 0;
    int capacity = 256;

    entries = malloc(capacity * sizeof(Entry));
    if (!entries) {
        perror("malloc");
        return 1;
    }

    dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "ls: cannot access '%s': %s\n", path, strerror(errno));
        free(entries);
        return 1;
    }

    while ((dp = readdir(dir)) != NULL) {
        if (!opts.show_all) {
            if (dp->d_name[0] == '.') {
                if (!opts.show_almost_all)
                    continue;
                if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
                    continue;
            }
        }

        if (count >= capacity) {
            capacity *= 2;
            Entry *new_entries = realloc(entries, capacity * sizeof(Entry));
            if (!new_entries) {
                perror("realloc");
                closedir(dir);
                free(entries);
                return 1;
            }
            entries = new_entries;
        }

        char fullpath[PATH_MAX];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, dp->d_name);

        if (lstat(fullpath, &entries[count].st) < 0) {
            continue;
        }
        entries[count].name = strdup(dp->d_name);
        entries[count].path = strdup(fullpath);
        count++;
    }
    closedir(dir);

    if (opts.sort_size || opts.sort_time)
        qsort(entries, count, sizeof(Entry), entry_compare);
    else
        qsort(entries, count, sizeof(Entry), entry_compare_name);

    if (opts.print_dir_name)
        printf("%s:\n", path);

    list_entries(entries, count);

    if (opts.recursive) {
        for (int i = 0; i < count; i++) {
            if (S_ISDIR(entries[i].st.st_mode)) {
                if (strcmp(entries[i].name, ".") == 0 ||
                    strcmp(entries[i].name, "..") == 0)
                    continue;
                putchar('\n');
                opts.print_dir_name = 1;
                list_path(entries[i].path);
            }
        }
    }

    for (int i = 0; i < count; i++) {
        free(entries[i].name);
        free(entries[i].path);
    }
    free(entries);

    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-laA1RhStirs] [file ...]\n", prog);
}

int main(int argc, char *argv[]) {
    int opt;
    int exit_code = 0;
    opts.is_terminal = isatty(STDOUT_FILENO);
    color_enabled = opts.is_terminal;

    while ((opt = getopt(argc, argv, "laA1RhStirs")) != -1) {
        switch (opt) {
            case 'l': opts.show_long = 1; break;
            case 'a': opts.show_all = 1; break;
            case 'A': opts.show_almost_all = 1; break;
            case '1': opts.one_per_line = 1; break;
            case 'R': opts.recursive = 1; break;
            case 'h': opts.human = 1; break;
            case 'S': opts.sort_size = 1; break;
            case 't': opts.sort_time = 1; break;
            case 'r': opts.reverse = 1; break;
            case 'i': opts.show_inode = 1; break;
            case 's': opts.show_size = 1; break;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (optind >= argc) {
        opts.print_dir_name = 0;
        exit_code = list_path(".");
    } else {
        Entry *files = NULL;
        int file_count = 0;
        int file_cap = 0;
        int dir_count = 0;

        for (int i = optind; i < argc; i++) {
            struct stat st;
            if (lstat(argv[i], &st) < 0) {
                fprintf(stderr, "ls: cannot access '%s': %s\n", argv[i], strerror(errno));
                exit_code = 1;
                continue;
            }

            if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
                dir_count++;
            } else {
                if (file_count >= file_cap) {
                    file_cap = file_cap ? file_cap * 2 : 16;
                    Entry *new_files = realloc(files, file_cap * sizeof(Entry));
                    if (!new_files) { perror("realloc"); return 1; }
                    files = new_files;
                }
                files[file_count].name = strdup(argv[i]);
                files[file_count].path = strdup(argv[i]);
                files[file_count].st = st;
                file_count++;
            }
        }

        if (file_count > 0) {
            if (opts.sort_size || opts.sort_time)
                qsort(files, file_count, sizeof(Entry), entry_compare);
            else
                qsort(files, file_count, sizeof(Entry), entry_compare_name);

            opts.print_dir_name = 0;
            list_entries(files, file_count);

            for (int i = 0; i < file_count; i++) {
                free(files[i].name);
                free(files[i].path);
            }
            free(files);
        }

        if (dir_count > 0) {
            for (int i = optind; i < argc; i++) {
                struct stat st;
                if (lstat(argv[i], &st) < 0) continue;
                if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
                    if (file_count + dir_count > 1)
                        opts.print_dir_name = 1;
                    else
                        opts.print_dir_name = 0;

                    if (file_count + dir_count > 1 && i > optind)
                        putchar('\n');
                    list_path(argv[i]);
                }
            }
        }
    }

    return exit_code;
}
