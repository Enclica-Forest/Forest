/*
 * shadow.h - Shadow password database
 *
 * Shadow password database for Fern libc.
 */
#ifndef _SHADOW_H
#define _SHADOW_H

#ifdef __cplusplus
extern "C" {
#endif

struct spwd {
    char *sp_namp;    /* Login name */
    char *sp_pwdp;    /* Encrypted password */
    long  sp_lstchg;  /* Date of last change */
    long  sp_min;     /* Minimum number of days between changes */
    long  sp_max;     /* Maximum number of days between changes */
    long  sp_warn;    /* Number of days before expiry to warn */
    long  sp_inact;   /* Number of days before account is disabled */
    long  sp_expire;  /* Number of days since account is disabled */
    unsigned long sp_flag; /* Reserved */
};

struct spwd *getspnam(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* _SHADOW_H */
