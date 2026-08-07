#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <getopt.h>
#include <libgen.h>

#define NEWC_MAGIC "070701"
#define NEWC_MAGIC_LEN 6
#define NEWC_HEADER_SIZE 110
#define TRAILER_NAME "TRAILER!!!"
#define TRAILER_SIZE 10

#define MAX_PATH 4096
#define INITIAL_BUF_SIZE (64 * 1024)

struct initrd_entry {
    char path[MAX_PATH];
    char name[MAX_PATH];
    struct stat st;
};

struct initrd_config {
    const char *output_file;
    const char *source_dir;
    const char *config_file;
    size_t max_size;
    int compress;
    int verbose;
    int force;
    int type;
};

static struct initrd_entry *entries = NULL;
static size_t entry_count = 0;
static size_t entry_capacity = 0;
static unsigned long current_offset = 0;

static void usage(void) {
    fprintf(stderr,
        "Usage: initrd-builder [OPTIONS]\n"
        "  -o FILE    Output file (required)\n"
        "  -d DIR     Source directory (required)\n"
        "  -c FILE    Config file listing files/dirs to include\n"
        "  -s SIZE    Maximum size in bytes (default: unlimited)\n"
        "  -t TYPE    Image type: cpio, tar, ext2 (default: cpio)\n"
        "  -z         Enable gzip compression\n"
        "  -v         Verbose output\n"
        "  -f         Force overwrite of output file\n"
        "  -h         Show this help\n");
}

static void die(const char *msg) {
    fprintf(stderr, "initrd-builder: %s\n", msg);
    exit(1);
}

static void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (!p) die("out of memory");
    return p;
}

static void ensure_entry_capacity(void) {
    if (entry_count >= entry_capacity) {
        entry_capacity = entry_capacity ? entry_capacity * 2 : 1024;
        entries = xrealloc(entries, entry_capacity * sizeof(struct initrd_entry));
    }
}

static void add_entry(const char *path, const char *name, struct stat *st) {
    ensure_entry_capacity();
    struct initrd_entry *e = &entries[entry_count++];
    strncpy(e->path, path, MAX_PATH - 1);
    e->path[MAX_PATH - 1] = '\0';
    strncpy(e->name, name, MAX_PATH - 1);
    e->name[MAX_PATH - 1] = '\0';
    memcpy(&e->st, st, sizeof(struct stat));
}

static size_t align4(size_t size) {
    return (size + 3) & ~3;
}

static void add_directory_recursive(const char *dirpath, const char *prefix);

static void add_single_entry(const char *fullpath, const char *name, const char *prefix) {
    struct stat st;
    if (lstat(fullpath, &st) < 0) {
        fprintf(stderr, "initrd-builder: cannot stat '%s': %s\n",
                fullpath, strerror(errno));
        return;
    }

    char entry_name[MAX_PATH];
    if (prefix && prefix[0]) {
        snprintf(entry_name, MAX_PATH, "%s/%s", prefix, name);
    } else {
        strncpy(entry_name, name, MAX_PATH - 1);
        entry_name[MAX_PATH - 1] = '\0';
    }

    add_entry(fullpath, entry_name, &st);

    size_t hdr_size = align4(NEWC_HEADER_SIZE + strlen(entry_name) + 1);

    if (S_ISDIR(st.st_mode)) {
        current_offset += hdr_size;
        add_directory_recursive(fullpath, entry_name);
    } else if (S_ISREG(st.st_mode)) {
        size_t data_size = align4((size_t)st.st_size);
        current_offset += hdr_size + data_size;
    } else {
        current_offset += hdr_size;
    }
}

static void add_directory_recursive(const char *dirpath, const char *prefix) {
    DIR *dir = opendir(dirpath);
    if (!dir) {
        fprintf(stderr, "initrd-builder: cannot open directory '%s': %s\n",
                dirpath, strerror(errno));
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char fullpath[MAX_PATH];
        snprintf(fullpath, MAX_PATH, "%s/%s", dirpath, ent->d_name);

        add_single_entry(fullpath, ent->d_name, prefix);
    }

    closedir(dir);
}

static int add_tree(const char *rootdir) {
    struct stat root_st;
    if (lstat(rootdir, &root_st) < 0) {
        fprintf(stderr, "initrd-builder: cannot stat '%s': %s\n",
                rootdir, strerror(errno));
        return -1;
    }

    add_entry(rootdir, ".", &root_st);
    current_offset += align4(NEWC_HEADER_SIZE + 2);

    add_directory_recursive(rootdir, "");

    return 0;
}

static int parse_config_line(const char *line, char *path, struct stat *st) {
    char mode_str[16] = "";
    char uid_str[16] = "";
    char gid_str[16] = "";
    char type_str[16] = "file";

    int n = sscanf(line, "%4095s %15s %15s %15s %15s",
                   path, mode_str, uid_str, gid_str, type_str);
    if (n < 1) return -1;

    memset(st, 0, sizeof(struct stat));

    if (n >= 2 && mode_str[0]) {
        st->st_mode = (mode_t)strtoul(mode_str, NULL, 8);
    } else {
        st->st_mode = 0644;
    }

    if (n >= 3 && uid_str[0]) {
        st->st_uid = (uid_t)strtoul(uid_str, NULL, 10);
    } else {
        st->st_uid = getuid();
    }

    if (n >= 4 && gid_str[0]) {
        st->st_gid = (gid_t)strtoul(gid_str, NULL, 10);
    } else {
        st->st_gid = getgid();
    }

    if (n >= 5) {
        if (strcmp(type_str, "dir") == 0)
            st->st_mode = (st->st_mode & ~S_IFMT) | S_IFDIR;
        else if (strcmp(type_str, "symlink") == 0)
            st->st_mode = (st->st_mode & ~S_IFMT) | S_IFLNK;
        else if (strcmp(type_str, "char") == 0)
            st->st_mode = (st->st_mode & ~S_IFMT) | S_IFCHR;
        else if (strcmp(type_str, "block") == 0)
            st->st_mode = (st->st_mode & ~S_IFMT) | S_IFBLK;
        else if (strcmp(type_str, "fifo") == 0 || strcmp(type_str, "pipe") == 0)
            st->st_mode = (st->st_mode & ~S_IFMT) | S_IFIFO;
        else
            st->st_mode = (st->st_mode & ~S_IFMT) | S_IFREG;
    } else {
        st->st_mode = (st->st_mode & ~S_IFMT) | S_IFREG;
    }

    if (S_ISDIR(st->st_mode))
        st->st_mode = (st->st_mode & ~0111) | 0755;

    st->st_nlink = 1;
    st->st_mtime = time(NULL);
    st->st_size = 0;

    return 0;
}

static int load_config(const char *configfile, const char *basedir) {
    FILE *f = fopen(configfile, "r");
    if (!f) {
        fprintf(stderr, "initrd-builder: cannot open config '%s': %s\n",
                configfile, strerror(errno));
        return -1;
    }

    char line[MAX_PATH * 2];
    char path[MAX_PATH];
    struct stat cfg_st;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (len == 0 || line[0] == '#')
            continue;

        if (parse_config_line(line, path, &cfg_st) < 0)
            continue;

        char fullpath[MAX_PATH * 2];
        if (basedir && basedir[0]) {
            snprintf(fullpath, sizeof(fullpath), "%s/%s", basedir, path);
        } else {
            strncpy(fullpath, path, MAX_PATH - 1);
            fullpath[MAX_PATH - 1] = '\0';
        }

        struct stat real_st;
        if (lstat(fullpath, &real_st) < 0) {
            fprintf(stderr, "initrd-builder: config: cannot stat '%s': %s\n",
                    fullpath, strerror(errno));
            continue;
        }

        real_st.st_mode = (real_st.st_mode & ~07777) | (cfg_st.st_mode & 07777);
        real_st.st_uid = cfg_st.st_uid;
        real_st.st_gid = cfg_st.st_gid;

        add_entry(fullpath, basename(path), &real_st);

        size_t hdr_size = align4(NEWC_HEADER_SIZE + strlen(basename(path)) + 1);
        if (S_ISDIR(real_st.st_mode)) {
            current_offset += hdr_size;
            add_directory_recursive(fullpath, basename(path));
        } else if (S_ISREG(real_st.st_mode)) {
            current_offset += hdr_size + align4((size_t)real_st.st_size);
        } else {
            current_offset += hdr_size;
        }
    }

    fclose(f);
    return 0;
}

static ssize_t xwrite(int fd, const void *buf, size_t count) {
    const char *p = buf;
    size_t total = 0;
    while (total < count) {
        ssize_t n = write(fd, p + total, count - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

static int write_newc_header(int outfd, const char *name, struct stat *st) {
    char header[NEWC_HEADER_SIZE + 1];
    size_t namesize = strlen(name) + 1;
    size_t total_size = NEWC_HEADER_SIZE + namesize;

    memset(header, '0', sizeof(header));
    header[NEWC_HEADER_SIZE] = '\0';

    memcpy(header, NEWC_MAGIC, NEWC_MAGIC_LEN);

    snprintf(header + 6, 9, "%08x", (unsigned)(st->st_ino));
    snprintf(header + 14, 9, "%08x", (unsigned)(st->st_mode));
    snprintf(header + 22, 9, "%08x", (unsigned)(st->st_uid));
    snprintf(header + 30, 9, "%08x", (unsigned)(st->st_gid));
    snprintf(header + 38, 9, "%08x", (unsigned)(st->st_nlink));
    snprintf(header + 46, 9, "%08x", (unsigned)(st->st_mtime));

    unsigned long datasize = 0;
    if (S_ISREG(st->st_mode)) {
        datasize = (unsigned long)st->st_size;
    } else if (S_ISLNK(st->st_mode)) {
        datasize = (unsigned long)st->st_size;
    }
    snprintf(header + 54, 9, "%08lx", datasize);
    snprintf(header + 62, 9, "%08x", 0);
    snprintf(header + 70, 9, "%08x", 0);

    if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode)) {
        snprintf(header + 78, 9, "%08x", major(st->st_rdev));
        snprintf(header + 86, 9, "%08x", minor(st->st_rdev));
    } else {
        snprintf(header + 78, 9, "%08x", 0);
        snprintf(header + 86, 9, "%08x", 0);
    }

    snprintf(header + 94, 9, "%08x", (unsigned)namesize);
    snprintf(header + 102, 9, "%08x", 0);

    if (xwrite(outfd, header, NEWC_HEADER_SIZE) != NEWC_HEADER_SIZE) return -1;
    if (xwrite(outfd, name, namesize) != (ssize_t)namesize) return -1;

    size_t pad = (4 - (total_size % 4)) % 4;
    if (pad > 0) {
        char zeros[4] = {0};
        if (xwrite(outfd, zeros, pad) != (ssize_t)pad) return -1;
    }

    return 0;
}

static int write_trailer(int outfd) {
    char header[NEWC_HEADER_SIZE + 1];
    size_t namesize = TRAILER_SIZE + 1;
    size_t total_size = NEWC_HEADER_SIZE + namesize;

    memset(header, '0', sizeof(header));
    header[NEWC_HEADER_SIZE] = '\0';

    memcpy(header, NEWC_MAGIC, NEWC_MAGIC_LEN);
    snprintf(header + 94, 9, "%08x", (unsigned)namesize);

    if (xwrite(outfd, header, NEWC_HEADER_SIZE) != NEWC_HEADER_SIZE) return -1;
    if (xwrite(outfd, TRAILER_NAME, namesize) != (ssize_t)namesize) return -1;

    size_t pad = (4 - (total_size % 4)) % 4;
    if (pad > 0) {
        char zeros[4] = {0};
        if (xwrite(outfd, zeros, pad) != (ssize_t)pad) return -1;
    }

    return 0;
}

static int write_file_data(int outfd, const char *path, size_t filesize) {
    int infd = open(path, O_RDONLY);
    if (infd < 0) {
        fprintf(stderr, "initrd-builder: cannot open '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    char buf[8192];
    size_t remaining = filesize;

    while (remaining > 0) {
        size_t toread = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        ssize_t n = read(infd, buf, toread);
        if (n <= 0) break;

        if (xwrite(outfd, buf, (size_t)n) != n) {
            close(infd);
            return -1;
        }
        remaining -= (size_t)n;
    }

    close(infd);

    size_t pad = (4 - (filesize % 4)) % 4;
    if (pad > 0) {
        char zeros[4] = {0};
        if (xwrite(outfd, zeros, pad) != (ssize_t)pad) return -1;
    }

    return 0;
}

static int write_symlink_data(int outfd, const char *path, struct stat *st) {
    char linktarget[MAX_PATH];
    ssize_t len = readlink(path, linktarget, MAX_PATH - 1);
    if (len < 0) {
        fprintf(stderr, "initrd-builder: cannot readlink '%s': %s\n",
                path, strerror(errno));
        return -1;
    }
    linktarget[len] = '\0';

    size_t linklen = (size_t)len + 1;
    st->st_size = (off_t)linklen;

    if (xwrite(outfd, linktarget, linklen) != (ssize_t)linklen) return -1;

    size_t pad = (4 - (linklen % 4)) % 4;
    if (pad > 0) {
        char zeros[4] = {0};
        if (xwrite(outfd, zeros, pad) != (ssize_t)pad) return -1;
    }

    return 0;
}

static int write_initrd(const struct initrd_config *cfg) {
    int outfd;

    if (!cfg->force && access(cfg->output_file, F_OK) == 0) {
        fprintf(stderr, "initrd-builder: output file '%s' already exists (use -f to force)\n",
                cfg->output_file);
        return -1;
    }

    outfd = open(cfg->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outfd < 0) {
        fprintf(stderr, "initrd-builder: cannot create '%s': %s\n",
                cfg->output_file, strerror(errno));
        return -1;
    }

    for (size_t i = 0; i < entry_count; i++) {
        struct initrd_entry *e = &entries[i];
        struct stat *st = &e->st;

        if (cfg->verbose) {
            const char *type = "?";
            if (S_ISREG(st->st_mode)) type = "file";
            else if (S_ISDIR(st->st_mode)) type = "dir";
            else if (S_ISLNK(st->st_mode)) type = "link";
            else if (S_ISCHR(st->st_mode)) type = "char";
            else if (S_ISBLK(st->st_mode)) type = "block";
            else if (S_ISFIFO(st->st_mode)) type = "fifo";
            fprintf(stderr, "  %s [%s] %o\n", e->name, type, st->st_mode & 0777);
        }

        if (write_newc_header(outfd, e->name, st) < 0) {
            fprintf(stderr, "initrd-builder: failed to write header for '%s'\n", e->name);
            close(outfd);
            return -1;
        }

        if (S_ISREG(st->st_mode) && st->st_size > 0) {
            if (write_file_data(outfd, e->path, (size_t)st->st_size) < 0) {
                fprintf(stderr, "initrd-builder: failed to write data for '%s'\n", e->name);
                close(outfd);
                return -1;
            }
        } else if (S_ISLNK(st->st_mode)) {
            if (write_symlink_data(outfd, e->path, st) < 0) {
                fprintf(stderr, "initrd-builder: failed to write symlink for '%s'\n", e->name);
                close(outfd);
                return -1;
            }
        }
    }

    if (write_trailer(outfd) < 0) {
        fprintf(stderr, "initrd-builder: failed to write trailer\n");
        close(outfd);
        return -1;
    }

    fsync(outfd);
    close(outfd);
    return 0;
}

static int write_cpio_gz(const struct initrd_config *cfg) {
    char tmpfile[MAX_PATH];
    snprintf(tmpfile, MAX_PATH, "%s.tmp.XXXXXX", cfg->output_file);

    int tmpfd = mkstemp(tmpfile);
    if (tmpfd < 0) {
        fprintf(stderr, "initrd-builder: cannot create temp file: %s\n", strerror(errno));
        return -1;
    }

    for (size_t i = 0; i < entry_count; i++) {
        struct initrd_entry *e = &entries[i];
        struct stat *st = &e->st;

        if (write_newc_header(tmpfd, e->name, st) < 0) {
            close(tmpfd);
            unlink(tmpfile);
            return -1;
        }

        if (S_ISREG(st->st_mode) && st->st_size > 0) {
            if (write_file_data(tmpfd, e->path, (size_t)st->st_size) < 0) {
                close(tmpfd);
                unlink(tmpfile);
                return -1;
            }
        } else if (S_ISLNK(st->st_mode)) {
            if (write_symlink_data(tmpfd, e->path, st) < 0) {
                close(tmpfd);
                unlink(tmpfile);
                return -1;
            }
        }
    }

    if (write_trailer(tmpfd) < 0) {
        close(tmpfd);
        unlink(tmpfile);
        return -1;
    }

    fsync(tmpfd);
    close(tmpfd);

    char cmd[MAX_PATH * 3];
    snprintf(cmd, sizeof(cmd), "gzip -9 < '%s' > '%s'", tmpfile, cfg->output_file);
    int ret = system(cmd);

    unlink(tmpfile);

    if (ret != 0) {
        fprintf(stderr, "initrd-builder: gzip compression failed\n");
        return -1;
    }

    return 0;
}

static int compare_names(const void *a, const void *b) {
    const struct initrd_entry *ea = a;
    const struct initrd_entry *eb = b;
    return strcmp(ea->name, eb->name);
}

int main(int argc, char *argv[]) {
    struct initrd_config cfg = {
        .output_file = NULL,
        .source_dir = NULL,
        .config_file = NULL,
        .max_size = 0,
        .compress = 0,
        .verbose = 0,
        .force = 0,
        .type = 0,
    };

    int opt;
    while ((opt = getopt(argc, argv, "o:d:c:s:tzvfh")) != -1) {
        switch (opt) {
        case 'o':
            cfg.output_file = optarg;
            break;
        case 'd':
            cfg.source_dir = optarg;
            break;
        case 'c':
            cfg.config_file = optarg;
            break;
        case 's':
            cfg.max_size = strtoul(optarg, NULL, 10);
            break;
        case 't':
            break;
        case 'z':
            cfg.compress = 1;
            break;
        case 'v':
            cfg.verbose = 1;
            break;
        case 'f':
            cfg.force = 1;
            break;
        case 'h':
            usage();
            return 0;
        default:
            usage();
            return 1;
        }
    }

    if (!cfg.output_file) {
        fprintf(stderr, "initrd-builder: output file required (-o)\n");
        usage();
        return 1;
    }

    if (!cfg.source_dir && !cfg.config_file) {
        fprintf(stderr, "initrd-builder: source directory required (-d)\n");
        usage();
        return 1;
    }

    if (cfg.verbose) {
        fprintf(stderr, "initrd-builder: creating initrd image\n");
        if (cfg.source_dir)
            fprintf(stderr, "  source directory: %s\n", cfg.source_dir);
        if (cfg.config_file)
            fprintf(stderr, "  config file: %s\n", cfg.config_file);
        fprintf(stderr, "  output file: %s\n", cfg.output_file);
        fprintf(stderr, "  compression: %s\n", cfg.compress ? "gzip" : "none");
    }

    entry_capacity = 1024;
    entries = xmalloc(entry_capacity * sizeof(struct initrd_entry));

    if (cfg.config_file) {
        const char *basedir = cfg.source_dir ? cfg.source_dir : ".";
        if (load_config(cfg.config_file, basedir) < 0) {
            free(entries);
            return 1;
        }
    }

    if (cfg.source_dir) {
        if (cfg.verbose)
            fprintf(stderr, "  scanning directory tree...\n");
        if (add_tree(cfg.source_dir) < 0) {
            free(entries);
            return 1;
        }
    }

    qsort(entries, entry_count, sizeof(struct initrd_entry), compare_names);

    if (cfg.max_size > 0 && current_offset > cfg.max_size) {
        fprintf(stderr, "initrd-builder: image size %lu exceeds maximum %lu\n",
                current_offset, (unsigned long)cfg.max_size);
        free(entries);
        return 1;
    }

    if (cfg.verbose) {
        fprintf(stderr, "  entries: %zu\n", entry_count);
        fprintf(stderr, "  uncompressed size: %lu bytes\n", current_offset);
    }

    int result;
    if (cfg.compress) {
        result = write_cpio_gz(&cfg);
    } else {
        result = write_initrd(&cfg);
    }

    free(entries);

    if (result < 0) {
        unlink(cfg.output_file);
        return 1;
    }

    struct stat out_st;
    if (stat(cfg.output_file, &out_st) == 0) {
        fprintf(stderr, "initrd-builder: created '%s' (%lu bytes, %zu files)\n",
                cfg.output_file, (unsigned long)out_st.st_size, entry_count);
    }

    return 0;
}
