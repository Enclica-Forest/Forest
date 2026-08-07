/*
 * sys/utsname.h - System name structure
 * 
 * POSIX compatible system identification for Fern libc.
 */
#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum length for utsname fields */
#define _UTSNAME_LENGTH 65

/* System identification structure */
struct utsname {
    /* Operating system name (e.g., "ForestOS") */
    char sysname[_UTSNAME_LENGTH];
    
    /* Network node hostname */
    char nodename[_UTSNAME_LENGTH];
    
    /* Operating system release (e.g., "0.2") */
    char release[_UTSNAME_LENGTH];
    
    /* Operating system version (e.g., "nightly") */
    char version[_UTSNAME_LENGTH];
    
    /* Hardware identifier (e.g., "x86_64") */
    char machine[_UTSNAME_LENGTH];
    
    /* NIS/YP domain name (Linux extension) */
    char domainname[_UTSNAME_LENGTH];
};

/* Get system identification */
int uname(struct utsname *buf);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_UTSNAME_H */
