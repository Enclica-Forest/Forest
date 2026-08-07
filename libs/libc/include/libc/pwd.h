/*
 * pwd.h - User database
 *
 * POSIX-compatible user database for Fern libc.
 */
#ifndef _PWD_H
#define _PWD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

struct passwd {
    char   *pw_name;       /* Username */
    char   *pw_passwd;     /* User password */
    uid_t   pw_uid;        /* User ID */
    gid_t   pw_gid;        /* Group ID */
    char   *pw_gecos;      /* Real name */
    char   *pw_dir;        /* Home directory */
    char   *pw_shell;      /* Shell program */
};

struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);
int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result);
int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result);

#ifdef __cplusplus
}
#endif

#endif /* _PWD_H */
