/*
 * stddef.h - Standard definitions
 * 
 * C23 compatible standard definitions for Fern libc.
 */
#ifndef _STDDEF_H
#define _STDDEF_H

#define __STDC_VERSION_STDDEF_H__ 202311L

#ifdef __cplusplus
extern "C" {
#endif

/* Size types */
#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef __SIZE_TYPE__ size_t;
#endif

/* Pointer difference type */
#ifndef _PTRDIFF_T_DEFINED
#define _PTRDIFF_T_DEFINED
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#endif

/* Wide character type */
#ifndef _WCHAR_T_DEFINED
#define _WCHAR_T_DEFINED
#ifndef __cplusplus
typedef __WCHAR_TYPE__ wchar_t;
#endif
#endif

/* Maximum alignment type */
typedef long double max_align_t;

/* Null pointer constant */
#ifndef NULL
#ifdef __cplusplus
#define NULL nullptr
#else
#define NULL ((void*)0)
#endif
#endif

/* Offset of a member in a structure */
#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

/* Unreachable code marker (C23) */
#ifndef unreachable
#define unreachable() __builtin_unreachable()
#endif

#ifdef __cplusplus
}
#endif

#endif /* _STDDEF_H */
