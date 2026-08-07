/*
 * sys/ioctl.h - I/O control
 * 
 * POSIX compatible ioctl definitions for Fern libc.
 */
#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/* ioctl direction bits */
#define _IOC_NRBITS     8
#define _IOC_TYPEBITS   8
#define _IOC_SIZEBITS   14
#define _IOC_DIRBITS    2

#define _IOC_NRMASK     ((1 << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK   ((1 << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK   ((1 << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK    ((1 << _IOC_DIRBITS) - 1)

#define _IOC_NRSHIFT    0
#define _IOC_TYPESHIFT  (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT  (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT   (_IOC_SIZESHIFT + _IOC_SIZEBITS)

/* Direction */
#define _IOC_NONE       0U
#define _IOC_WRITE      1U
#define _IOC_READ       2U

#define _IOC(dir, type, nr, size) \
    (((dir) << _IOC_DIRSHIFT) | \
     ((type) << _IOC_TYPESHIFT) | \
     ((nr) << _IOC_NRSHIFT) | \
     ((size) << _IOC_SIZESHIFT))

/* Construct ioctl numbers */
#define _IO(type, nr)           _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, size)    _IOC(_IOC_READ, (type), (nr), sizeof(size))
#define _IOW(type, nr, size)    _IOC(_IOC_WRITE, (type), (nr), sizeof(size))
#define _IOWR(type, nr, size)   _IOC(_IOC_READ|_IOC_WRITE, (type), (nr), sizeof(size))

/* Extract parts of ioctl number */
#define _IOC_DIR(nr)    (((nr) >> _IOC_DIRSHIFT) & _IOC_DIRMASK)
#define _IOC_TYPE(nr)   (((nr) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(nr)     (((nr) >> _IOC_NRSHIFT) & _IOC_NRMASK)
#define _IOC_SIZE(nr)   (((nr) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)

/* Terminal ioctl requests */
#define TCGETS          0x5401
#define TCSETS          0x5402
#define TCSETSW         0x5403
#define TCSETSF         0x5404
#define TCGETA          0x5405
#define TCSETA          0x5406
#define TCSETAW         0x5407
#define TCSETAF         0x5408
#define TCSBRK          0x5409
#define TCXONC          0x540A
#define TCFLSH          0x540B
#define TIOCEXCL        0x540C
#define TIOCNXCL        0x540D
#define TIOCSCTTY       0x540E
#define TIOCGPGRP       0x540F
#define TIOCSPGRP       0x5410
#define TIOCOUTQ        0x5411
#define TIOCSTI         0x5412
#define TIOCGWINSZ      0x5413
#define TIOCSWINSZ      0x5414
#define TIOCMGET        0x5415
#define TIOCMBIS        0x5416
#define TIOCMBIC        0x5417
#define TIOCMSET        0x5418
#define TIOCGSOFTCAR    0x5419
#define TIOCSSOFTCAR    0x541A
#define FIONREAD        0x541B
#define TIOCINQ         FIONREAD
#define TIOCLINUX       0x541C
#define TIOCCONS        0x541D
#define TIOCGSERIAL     0x541E
#define TIOCSSERIAL     0x541F
#define TIOCPKT         0x5420
#define FIONBIO         0x5421
#define TIOCNOTTY       0x5422
#define TIOCSETD        0x5423
#define TIOCGETD        0x5424
#define TCSBRKP         0x5425
#define TIOCSBRK        0x5427
#define TIOCCBRK        0x5428
#define TIOCGSID        0x5429
#define FIONCLEX        0x5450
#define FIOCLEX         0x5451
#define FIOASYNC        0x5452
#define TIOCSERCONFIG   0x5453
#define TIOCSERGWILD    0x5454
#define TIOCSERSWILD    0x5455
#define TIOCGLCKTRMIOS  0x5456
#define TIOCSLCKTRMIOS  0x5457
#define TIOCSERGSTRUCT  0x5458
#define TIOCSERGETLSR   0x5459
#define TIOCSERGETMULTI 0x545A
#define TIOCSERSETMULTI 0x545B
#define TIOCMIWAIT      0x545C
#define TIOCGICOUNT     0x545D
#define FIOQSIZE        0x5460

/* Window size structure */
struct winsize {
    unsigned short ws_row;      /* Rows */
    unsigned short ws_col;      /* Columns */
    unsigned short ws_xpixel;   /* Horizontal size, pixels */
    unsigned short ws_ypixel;   /* Vertical size, pixels */
};

/* File/socket ioctl requests */
#define FIOSETOWN       0x8901
#define SIOCSPGRP       0x8902
#define FIOGETOWN       0x8903
#define SIOCGPGRP       0x8904
#define SIOCATMARK      0x8905
#define SIOCGSTAMP      0x8906
#define SIOCGSTAMPNS    0x8907

/* Socket ioctl requests */
#define SIOCADDRT       0x890B  /* Add routing table entry */
#define SIOCDELRT       0x890C  /* Delete routing table entry */
#define SIOCRTMSG       0x890D  /* Send routing message */

/* ARP cache ioctl requests */
#define SIOCSARP        0x8955  /* Set ARP mapping */
#define SIOCGARP        0x8954  /* Get ARP mapping */
#define SIOCDARP        0x8953  /* Delete ARP mapping */

/* Device ioctl requests */
#define SIOCGIFNAME     0x8910  /* Get interface name */
#define SIOCSIFLINK     0x8911  /* Set interface channel */
#define SIOCGIFCONF     0x8912  /* Get interface list */
#define SIOCGIFFLAGS    0x8913  /* Get interface flags */
#define SIOCSIFFLAGS    0x8914  /* Set interface flags */
#define SIOCGIFADDR     0x8915  /* Get interface address */
#define SIOCSIFADDR     0x8916  /* Set interface address */
#define SIOCGIFDSTADDR  0x8917  /* Get destination address */
#define SIOCSIFDSTADDR  0x8918  /* Set destination address */
#define SIOCGIFBRDADDR  0x8919  /* Get broadcast address */
#define SIOCSIFBRDADDR  0x891A  /* Set broadcast address */
#define SIOCGIFNETMASK  0x891B  /* Get network mask */
#define SIOCSIFNETMASK  0x891C  /* Set network mask */
#define SIOCGIFMETRIC   0x891D  /* Get interface metric */
#define SIOCSIFMETRIC   0x891E  /* Set interface metric */
#define SIOCGIFMEM      0x891F  /* Get memory address */
#define SIOCSIFMEM      0x8920  /* Set memory address */
#define SIOCGIFMTU      0x8921  /* Get MTU */
#define SIOCSIFMTU      0x8922  /* Set MTU */
#define SIOCSIFNAME     0x8923  /* Set interface name */
#define SIOCSIFHWADDR   0x8924  /* Set hardware address */
#define SIOCGIFENCAP    0x8925  /* Get encapsulation type */
#define SIOCSIFENCAP    0x8926  /* Set encapsulation type */
#define SIOCGIFHWADDR   0x8927  /* Get hardware address */
#define SIOCGIFSLAVE    0x8929  /* Get slave device */
#define SIOCSIFSLAVE    0x8930  /* Set slave device */
#define SIOCADDMULTI    0x8931  /* Add multicast address */
#define SIOCDELMULTI    0x8932  /* Delete multicast address */
#define SIOCGIFINDEX    0x8933  /* Get interface index */

/* ioctl function */
int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IOCTL_H */
