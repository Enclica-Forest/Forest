/*
 * stdarg.h - Variable argument handling
 * 
 * C23 compatible variable argument macros for Fern libc.
 * Uses GCC/Clang built-ins for proper implementation.
 */
#ifndef _STDARG_H
#define _STDARG_H

#define __STDC_VERSION_STDARG_H__ 202311L

#ifdef __cplusplus
extern "C" {
#endif

/* Variable argument list type */
typedef __builtin_va_list va_list;

/* Variable argument macros */
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)
#define va_copy(dest, src) __builtin_va_copy(dest, src)

#ifdef __cplusplus
}
#endif

#endif /* _STDARG_H */
