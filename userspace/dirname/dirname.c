#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DIRNAME_VERSION "1.0.0"

static const char *progname = "dirname";

static void usage(void) {
    fprintf(stderr, "Usage: %s NAME\n", progname);
    fprintf(stderr, "Print NAME with its trailing /component removed.\n");
}

static void help(void) {
    printf("Usage: %s NAME\n", progname);
    printf("Print NAME with its trailing /component removed.\n");
    printf("If NAME contains no /'s, output '.' (meaning current directory).\n");
    printf("\n");
    printf("  --help        display this help and exit\n");
    printf("  --version     display version information and exit\n");
}

static void version(void) {
    printf("%s (Forest OS coreutils) %s\n", progname, DIRNAME_VERSION);
    printf("Copyright (C) 2026 Forest OS Contributors.\n");
    printf("License: MIT\n");
}

static void print_dirname(const char *name) {
    char *work;
    char *last_slash;
    size_t len;

    if (!name || !*name) {
        printf(".\n");
        return;
    }

    work = strdup(name);
    if (!work) {
        fprintf(stderr, "%s: memory allocation failed\n", progname);
        exit(1);
    }

    len = strlen(work);

    /* Strip trailing slashes */
    while (len > 1 && work[len - 1] == '/')
        work[--len] = '\0';

    /* Find last slash */
    last_slash = strrchr(work, '/');

    if (!last_slash) {
        /* No slash found - single component */
        printf(".\n");
    } else if (last_slash == work) {
        /* Only one slash at the start - root dir */
        /* Already stripped trailing slashes, so if length > 0 it is "/" */
        printf("%s\n", work);
    } else {
        /* Found slash - strip everything after it */
        *last_slash = '\0';
        printf("%s\n", work);
    }

    free(work);
}

int main(int argc, char *argv[]) {
    int i;
    progname = argv[0];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            help();
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            version();
            return 0;
        }
    }

    if (argc < 2) {
        usage();
        return 1;
    }

    print_dirname(argv[1]);

    return 0;
}
