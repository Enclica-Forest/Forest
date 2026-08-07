/*
 * umount.c - Forest OS userspace umount implementation
 * Unmount filesystems using umount2() syscall.
 */

#define _GNU_SOURCE
#include <forest.h>
#include <sys/mount.h>

/* Options */
static int opt_all        = 0;  /* -a: unmount all filesystems */
static int opt_remount_ro = 0;  /* -r: remount read-only */
static int opt_force      = 0;  /* -f: force unmount */
static int opt_no_mtab    = 0;  /* -n: don't write /etc/mtab */
static int opt_verbose    = 0;  /* -v: verbose output */
static int opt_lazy       = 0;  /* -l: lazy unmount */

static const char *progname;
static const char *MTAB_PATH = "/etc/mtab";

/* Forward declarations */
static void usage(void);
static int unmount_one(const char *target);
static int remove_mtab_entry(const char *target);
static int show_mtab(void);
static int unmount_all(void);

static void usage(void) {
    fprintf(stderr, "Usage: %s [-a] [-r] [-f] [-n] [-v] [-l] [mountpoint...]\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -a        Unmount all filesystems (except /proc, /sys, /dev, /run)\n");
    fprintf(stderr, "  -r        Remount read-only\n");
    fprintf(stderr, "  -f        Force unmount\n");
    fprintf(stderr, "  -n        Don't write to /etc/mtab\n");
    fprintf(stderr, "  -v        Verbose output\n");
    fprintf(stderr, "  -l        Lazy unmount (detach filesystem now, clean up later)\n");
    exit(1);
}

/*
 * Remove an entry from /etc/mtab for the given mountpoint.
 */
static int remove_mtab_entry(const char *target) {
    FILE *fp;
    char *line = NULL;
    size_t len = 0;
    FILE *tmp_fp;
    char tmp_path[] = "/etc/mtab.XXXXXX";
    int tmp_fd;
    int removed = 0;

    if (opt_no_mtab)
        return 0;

    fp = fopen(MTAB_PATH, "r");
    if (!fp) {
        if (opt_verbose)
            fprintf(stderr, "%s: cannot open %s: %s\n", progname, MTAB_PATH, strerror(errno));
        return -1;
    }

    tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        fprintf(stderr, "%s: cannot create temporary file: %s\n", progname, strerror(errno));
        fclose(fp);
        return -1;
    }
    tmp_fp = fdopen(tmp_fd, "w");
    if (!tmp_fp) {
        fprintf(stderr, "%s: cannot open temporary file: %s\n", progname, strerror(errno));
        close(tmp_fd);
        unlink(tmp_path);
        fclose(fp);
        return -1;
    }

    while (getline(&line, &len, fp) != -1) {
        /* Check if this line contains the target mountpoint */
        /* mtab format: device mountpoint fstype options dump pass */
        char *p = line;
        char *mountpoint = NULL;

        /* Skip device field */
        while (*p && *p != ' ') p++;
        if (*p == ' ') { *p = '\0'; p++; }

        /* Get mountpoint field */
        mountpoint = p;
        while (*p && *p != ' ') p++;
        if (*p == '\0') goto write_line;
        *p = '\0';

        if (strcmp(mountpoint, target) != 0) {
write_line:
            fputs(line, tmp_fp);
        } else {
            removed = 1;
        }
    }

    fclose(fp);
    free(line);
    fclose(tmp_fp);

    if (removed) {
        if (rename(tmp_path, MTAB_PATH) < 0) {
            fprintf(stderr, "%s: cannot update %s: %s\n", progname, MTAB_PATH, strerror(errno));
            unlink(tmp_path);
            return -1;
        }
        if (opt_verbose)
            fprintf(stderr, "%s: removed %s from %s\n", progname, target, MTAB_PATH);
    } else {
        unlink(tmp_path);
    }

    return 0;
}

/*
 * Show currently mounted filesystems from /etc/mtab.
 */
static int show_mtab(void) {
    FILE *fp;
    char *line = NULL;
    size_t len = 0;

    fp = fopen(MTAB_PATH, "r");
    if (!fp) {
        fprintf(stderr, "%s: cannot open %s: %s\n", progname, MTAB_PATH, strerror(errno));
        return -1;
    }

    while (getline(&line, &len, fp) != -1) {
        /* Skip comments and empty lines */
        if (line[0] == '\n' || line[0] == '#')
            continue;
        fputs(line, stdout);
    }

    fclose(fp);
    free(line);
    return 0;
}

/*
 * Unmount all filesystems (except protected ones).
 */
static int unmount_all(void) {
    FILE *fp;
    char *line = NULL;
    size_t len = 0;
    int ret = 0;

    fp = fopen(MTAB_PATH, "r");
    if (!fp) {
        fprintf(stderr, "%s: cannot open %s: %s\n", progname, MTAB_PATH, strerror(errno));
        return -1;
    }

    /* Read all lines into memory first (mtab may change during unmount) */
    char **entries = NULL;
    int entries_cap = 0;
    int entries_count = 0;

    while (getline(&line, &len, fp) != -1) {
        if (line[0] == '\n' || line[0] == '#')
            continue;

        if (entries_count >= entries_cap) {
            entries_cap = entries_cap ? entries_cap * 2 : 64;
            char **new_entries = realloc(entries, entries_cap * sizeof(char *));
            if (!new_entries) {
                fprintf(stderr, "%s: out of memory\n", progname);
                fclose(fp);
                free(line);
                for (int i = 0; i < entries_count; i++)
                    free(entries[i]);
                free(entries);
                return -1;
            }
            entries = new_entries;
        }
        entries[entries_count] = strdup(line);
        entries_count++;
    }
    fclose(fp);
    free(line);

    /* Unmount in reverse order */
    for (int i = entries_count - 1; i >= 0; i--) {
        /* Parse mtab line: device mountpoint fstype options dump pass */
        char *line_copy = entries[i];
        char *mountpoint = NULL;
        char *p = line_copy;

        /* Get mountpoint */
        mountpoint = p;
        while (*p && *p != ' ') p++;
        *p = '\0';

        /* Skip protected filesystems */
        if (strcmp(mountpoint, "/") == 0 ||
            strcmp(mountpoint, "/proc") == 0 ||
            strcmp(mountpoint, "/sys") == 0 ||
            strcmp(mountpoint, "/dev") == 0 ||
            strcmp(mountpoint, "/run") == 0) {
            if (opt_verbose)
                fprintf(stderr, "%s: skipping protected mountpoint %s\n", progname, mountpoint);
            continue;
        }

        if (unmount_one(mountpoint) < 0)
            ret = 1;

        free(entries[i]);
    }
    free(entries);

    return ret;
}

/*
 * Unmount a single filesystem.
 */
static int unmount_one(const char *target) {
    int flags = 0;

    if (opt_remount_ro)
        flags |= MNT_FORCE;
    if (opt_force)
        flags |= MNT_FORCE;
    if (opt_lazy)
        flags |= MNT_DETACH;

    if (opt_verbose)
        fprintf(stderr, "%s: unmounting %s\n", progname, target);

    if (umount2(target, flags) < 0) {
        fprintf(stderr, "%s: cannot unmount %s: %s\n", progname, target, strerror(errno));
        return -1;
    }

    if (opt_verbose)
        fprintf(stderr, "%s: %s unmounted\n", progname, target);

    remove_mtab_entry(target);

    return 0;
}

int main(int argc, char *argv[]) {
    int opt;
    int exit_code = 0;

    progname = argv[0];
    if (strncmp(progname, "./", 2) == 0)
        progname += 2;

    while ((opt = getopt(argc, argv, "arfnvl")) != -1) {
        switch (opt) {
        case 'a':
            opt_all = 1;
            break;
        case 'r':
            opt_remount_ro = 1;
            break;
        case 'f':
            opt_force = 1;
            break;
        case 'n':
            opt_no_mtab = 1;
            break;
        case 'v':
            opt_verbose = 1;
            break;
        case 'l':
            opt_lazy = 1;
            break;
        default:
            usage();
        }
    }

    /* If -a is specified, unmount all */
    if (opt_all) {
        if (optind < argc)
            fprintf(stderr, "%s: warning: arguments ignored with -a\n", progname);
        exit_code = unmount_all();
        return exit_code;
    }

    /* No arguments: show mounted filesystems */
    if (optind >= argc) {
        show_mtab();
        return 0;
    }

    /* Unmount each specified target */
    for (int i = optind; i < argc; i++) {
        if (unmount_one(argv[i]) < 0)
            exit_code = 1;
    }

    return exit_code;
}
