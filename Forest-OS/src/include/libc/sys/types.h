/*
 * sys/types.h - Data types
 *
 * POSIX compatible data type definitions for Fern libc.
 */
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* Architecture detection */
#ifndef ARCH_64BIT
#if defined(__x86_64__) || defined(_M_X64) || defined(__LP64__)
#define ARCH_64BIT 1
#define ARCH_32BIT 0
#else
#define ARCH_64BIT 0
#define ARCH_32BIT 1
#endif
#endif

/* Size types */
#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef __SIZE_TYPE__ size_t;
#endif

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
#if ARCH_64BIT
typedef int64_t ssize_t;
#else
typedef int32_t ssize_t;
#endif
#endif

/* Process and user ID types */
#ifndef _PID_T_DEFINED
#define _PID_T_DEFINED
typedef int32_t pid_t;
#endif

#ifndef _UID_T_DEFINED
#define _UID_T_DEFINED
typedef uint32_t uid_t;
#endif

#ifndef _GID_T_DEFINED
#define _GID_T_DEFINED
typedef uint32_t gid_t;
#endif

/* File system types */
#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED
#if ARCH_64BIT
typedef int64_t off_t;
typedef int64_t off64_t;
#else
typedef int32_t off_t;
typedef int64_t off64_t;
#endif
#endif

#ifndef _MODE_T_DEFINED
#define _MODE_T_DEFINED
typedef uint32_t mode_t;
#endif

#ifndef _DEV_T_DEFINED
#define _DEV_T_DEFINED
typedef uint64_t dev_t;
#endif

#ifndef _INO_T_DEFINED
#define _INO_T_DEFINED
typedef uint64_t ino_t;
typedef uint64_t ino64_t;
#endif

#ifndef _NLINK_T_DEFINED
#define _NLINK_T_DEFINED
typedef uint32_t nlink_t;
#endif

#ifndef _BLKSIZE_T_DEFINED
#define _BLKSIZE_T_DEFINED
typedef int32_t blksize_t;
#endif

#ifndef _BLKCNT_T_DEFINED
#define _BLKCNT_T_DEFINED
typedef int64_t blkcnt_t;
typedef int64_t blkcnt64_t;
#endif

/* Time types */
#ifndef _TIME_T_DEFINED
#define _TIME_T_DEFINED
#if ARCH_64BIT
typedef int64_t time_t;
#else
typedef int32_t time_t;
#endif
#endif

#ifndef _CLOCK_T_DEFINED
#define _CLOCK_T_DEFINED
typedef int64_t clock_t;
#endif

#ifndef _CLOCKID_T_DEFINED
#define _CLOCKID_T_DEFINED
typedef int32_t clockid_t;
#endif

#ifndef _TIMER_T_DEFINED
#define _TIMER_T_DEFINED
typedef void* timer_t;
#endif

#ifndef _SUSECONDS_T_DEFINED
#define _SUSECONDS_T_DEFINED
typedef int32_t suseconds_t;
#endif

#ifndef _USECONDS_T_DEFINED
#define _USECONDS_T_DEFINED
typedef uint32_t useconds_t;
#endif

/* Key types */
#ifndef _KEY_T_DEFINED
#define _KEY_T_DEFINED
typedef int32_t key_t;
#endif

/* Socket types */
#ifndef _SOCKLEN_T_DEFINED
#define _SOCKLEN_T_DEFINED
typedef uint32_t socklen_t;
#endif

#ifndef _SA_FAMILY_T_DEFINED
#define _SA_FAMILY_T_DEFINED
typedef uint16_t sa_family_t;
#endif

#ifndef _IN_ADDR_T_DEFINED
#define _IN_ADDR_T_DEFINED
typedef uint32_t in_addr_t;
#endif

#ifndef _IN_PORT_T_DEFINED
#define _IN_PORT_T_DEFINED
typedef uint16_t in_port_t;
#endif

/* File descriptor types */
typedef int32_t fd_t;

/* Poll types */
#ifndef _NFDS_T_DEFINED
#define _NFDS_T_DEFINED
typedef unsigned long nfds_t;
#endif

/* Locale type (opaque) */
typedef struct __locale_struct *locale_t;

/* Register type for signals */
typedef int32_t register_t;

/* Compatibility types */
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;

/* BSD compatibility */
typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;

/* Quad type (BSD) */
typedef int64_t quad_t;
typedef uint64_t u_quad_t;

/* caddr_t for memory addresses */
typedef char *caddr_t;

/* daddr_t for disk addresses */
typedef int32_t daddr_t;

#ifdef __cplusplus
}
#endif

#endif /* _SYS_TYPES_H */
