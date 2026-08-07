/*
 * limits.h - Implementation-defined limits
 * 
 * C23 compatible implementation limits for Fern libc.
 */
#ifndef _LIMITS_H
#define _LIMITS_H

#define __STDC_VERSION_LIMITS_H__ 202311L

#ifdef __cplusplus
extern "C" {
#endif

/* Number of bits in a char */
#define CHAR_BIT 8

/* Minimum and maximum values for signed char */
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127

/* Maximum value for unsigned char */
#define UCHAR_MAX 255

/* Minimum and maximum values for char */
#ifdef __CHAR_UNSIGNED__
#define CHAR_MIN 0
#define CHAR_MAX UCHAR_MAX
#else
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#endif

/* Minimum and maximum values for short */
#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define USHRT_MAX 65535

/* Minimum and maximum values for int */
#define INT_MIN (-2147483647 - 1)
#define INT_MAX 2147483647
#define UINT_MAX 0xFFFFFFFFU

/* Minimum and maximum values for long */
#if defined(__x86_64__) || defined(_M_X64) || defined(__LP64__)
#define LONG_MIN (-9223372036854775807L - 1)
#define LONG_MAX 9223372036854775807L
#define ULONG_MAX 0xFFFFFFFFFFFFFFFFUL
#else
#define LONG_MIN (-2147483647L - 1)
#define LONG_MAX 2147483647L
#define ULONG_MAX 0xFFFFFFFFUL
#endif

/* Minimum and maximum values for long long */
#define LLONG_MIN (-9223372036854775807LL - 1)
#define LLONG_MAX 9223372036854775807LL
#define ULLONG_MAX 0xFFFFFFFFFFFFFFFFULL

/* Minimum value of 'bool' (C23) */
#define BOOL_WIDTH 8

/* Width of integer types (C23) */
#define CHAR_WIDTH CHAR_BIT
#define SCHAR_WIDTH CHAR_BIT
#define UCHAR_WIDTH CHAR_BIT
#define SHRT_WIDTH 16
#define USHRT_WIDTH 16
#define INT_WIDTH 32
#define UINT_WIDTH 32
#if defined(__x86_64__) || defined(_M_X64) || defined(__LP64__)
#define LONG_WIDTH 64
#define ULONG_WIDTH 64
#else
#define LONG_WIDTH 32
#define ULONG_WIDTH 32
#endif
#define LLONG_WIDTH 64
#define ULLONG_WIDTH 64

/* POSIX limits */
#define NAME_MAX 255
#define PATH_MAX 4096
#define PIPE_BUF 4096
#define ARG_MAX 131072
#define OPEN_MAX 1024
#define NGROUPS_MAX 32
#define HOST_NAME_MAX 64
#define LOGIN_NAME_MAX 256
#define TTY_NAME_MAX 32
#define SYMLINK_MAX 255
#define LINE_MAX 2048

/* Multi-byte character length */
#define MB_LEN_MAX 4

#ifdef __cplusplus
}
#endif

#endif /* _LIMITS_H */
