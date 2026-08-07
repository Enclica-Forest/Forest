/*
 * termios.h - Terminal I/O
 *
 * POSIX terminal I/O for Fern libc.
 */
#ifndef _TERMIOS_H
#define _TERMIOS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/* tcflag_t type */
#ifndef _TCFLAG_T_DEFINED
#define _TCFLAG_T_DEFINED
typedef unsigned int tcflag_t;
#endif

/* cc_t type */
#ifndef _CC_T_DEFINED
#define _CC_T_DEFINED
typedef unsigned char cc_t;
#endif

/* speed_t type */
#ifndef _SPEED_T_DEFINED
#define _SPEED_T_DEFINED
typedef unsigned int speed_t;
#endif

/* c_iflag bits */
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IUCLC   0001000
#define IXON    0002000
#define IXANY   0004000
#define IXOFF   0010000
#define IMAXBEL 0020000
#define IUTF8   0040000

/* c_oflag bits */
#define OPOST   0000001
#define OLCUC   0000002
#define ONLCR   0000004
#define OCRNL   0000010
#define ONOCR   0000020
#define ONLRET  0000040
#define OFILL   0000100
#define OFDEL   0000200

/* c_cflag bits */
#define B0      0000000
#define B50     0000001
#define B75     0000002
#define B110    0000003
#define B134    0000004
#define B150    0000005
#define B200    0000006
#define B300    0000007
#define B600    0000010
#define B1200   0000011
#define B1800   0000012
#define B2400   0000013
#define B4800   0000014
#define B9600   0000015
#define B19200  0000016
#define B38400  0000017
#define CS5     0000000
#define CS6     0000020
#define CS7     0000040
#define CS8     0000060
#define CSTOPB  0000100
#define CREAD   0000200
#define PARENB  0000400
#define PARODD  0001000
#define HUPCL   0002000
#define CLOCAL  0004000

/* c_lflag bits */
#define ISIG    0000001
#define ICANON  0000002
#define XCASE   0000004
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define ECHOCTL 0001000
#define ECHOPRT 0002000
#define ECHOKE  0004000
#define FLUSHO  0010000
#define PENDIN  0040000
#define IEXTEN  0100000

/* tcsetattr() actions */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* tcflush() actions */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

/* tcflow() actions */
#define TCOOFF 0
#define TCOON  1
#define TCIOFF 2
#define TCION  3

/* Terminal structure */
struct termios {
    unsigned int c_iflag;   /* Input modes */
    unsigned int c_oflag;   /* Output modes */
    unsigned int c_cflag;   /* Control modes */
    unsigned int c_lflag;   /* Local modes */
    unsigned char c_line;   /* Line discipline */
    unsigned char c_cc[32]; /* Control characters */
    speed_t c_ispeed;       /* Input speed */
    speed_t c_ospeed;       /* Output speed */
};

/* Termios functions */
int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);
int tcsendbreak(int fd, int duration);
int tcdrain(int fd);
int tcflush(int fd, int queue_selector);
int tcflow(int fd, int action);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);

/* Speed conversion */
speed_t cfgetospeed(const struct termios *termios_p);
speed_t cfgetispeed(const struct termios *termios_p);
int cfsetospeed(struct termios *termios_p, speed_t speed);
int cfsetispeed(struct termios *termios_p, speed_t speed);

/* Canonical input processing */
#define VEOF     0
#define VEOL     1
#define VERASE   2
#define VKILL    3
#define VMIN     4
#define VTIME    5
#define VSWTC    6
#define VSTART   7
#define VSTOP    8
#define VSUSP    9
#define VEOL2    10
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define NCCS     20

#ifdef __cplusplus
}
#endif

#endif /* _TERMIOS_H */
