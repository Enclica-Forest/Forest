/*
 * stdbool.h - Boolean type and values
 * 
 * C23 compatible boolean definitions for Fern libc.
 */
#ifndef _STDBOOL_H
#define _STDBOOL_H

#define __STDC_VERSION_STDBOOL_H__ 202311L

#ifndef __cplusplus

#ifndef __bool_true_false_are_defined
#define __bool_true_false_are_defined 1

/* In C23, bool is a keyword. For older standards, we define it. */
#if __STDC_VERSION__ >= 202311L
/* bool, true, false are keywords in C23 */
#else
#define bool _Bool
#define true 1
#define false 0
#endif

#endif /* __bool_true_false_are_defined */

#endif /* __cplusplus */

#endif /* _STDBOOL_H */
