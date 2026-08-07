/*
 * grp.h - Group database
 *
 * POSIX-compatible group database for Fern libc.
 */
#ifndef _GRP_H
#define _GRP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

struct group {
    char   *gr_name;       /* Group name */
    char   *gr_passwd;     /* Group password */
    gid_t   gr_gid;        /* Group ID */
    char  **gr_mem;        /* Null-terminated array of pointers to names in group */
};

struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);
int getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups);
int initgroups(const char *user, gid_t group);

#ifdef __cplusplus
}
#endif

#endif /* _GRP_H */
