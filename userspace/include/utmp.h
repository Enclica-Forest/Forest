#ifndef _UTMP_H
#define _UTMP_H

#include <sys/types.h>

#define BOOT_TIME 2
#define RUN_LEVEL 1

struct exit_status {
    short int e_termination;
    short int e_exit;
};

struct utmp {
    short   ut_type;
    pid_t   ut_pid;
    char    ut_line[32];
    char    ut_id[4];
    char    ut_user[32];
    char    ut_host[256];
    struct  exit_status ut_exit;
    long    ut_session;
    struct {
        long tv_sec;
        long tv_usec;
    } ut_tv;
    int32_t ut_addr_v6[4];
    char    __unused[20];
};

#endif
