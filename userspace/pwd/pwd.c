#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *progname = "pwd";

static void usage(void) {
    fprintf(stderr, "Usage: %s [-L | -P]\n", progname);
    fprintf(stderr, "  -L, --logical   use PWD from environment\n");
    fprintf(stderr, "  -P, --physical  resolve symlinks (default)\n");
    exit(1);
}

int main(int argc, char *argv[]) {
    int logical = 0;
    int physical = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-L") == 0 || strcmp(argv[i], "--logical") == 0) {
            if (physical) {
                fprintf(stderr, "%s: the -L and -P options are mutually exclusive\n", progname);
                exit(1);
            }
            logical = 1;
        } else if (strcmp(argv[i], "-P") == 0 || strcmp(argv[i], "--physical") == 0) {
            if (logical) {
                fprintf(stderr, "%s: the -L and -P options are mutually exclusive\n", progname);
                exit(1);
            }
            physical = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage();
        } else {
            fprintf(stderr, "%s: invalid option -- '%s'\n", progname, argv[i]);
            usage();
        }
    }

    if (!logical && !physical)
        physical = 1;

    if (logical) {
        const char *pwd = getenv("PWD");
        if (pwd) {
            printf("%s\n", pwd);
            return 0;
        }
    }

    char buf[4096];
    if (getcwd(buf, sizeof(buf))) {
        printf("%s\n", buf);
        return 0;
    }

    perror(progname);
    return 1;
}
