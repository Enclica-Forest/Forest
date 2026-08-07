/*
 * ctype.h - Character classification and mapping
 * 
 * C23 compatible character handling for Fern libc.
 */
#ifndef _CTYPE_H
#define _CTYPE_H

#include <sys/types.h>

#define __STDC_VERSION_CTYPE_H__ 202311L

#ifdef __cplusplus
extern "C" {
#endif

/* Character classification functions */

/* Test for alphabetic character */
static inline int isalpha(int c) {
    return ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}

/* Test for decimal digit */
static inline int isdigit(int c) {
    return (c >= '0' && c <= '9');
}

/* Test for alphanumeric character */
static inline int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

/* Test for whitespace character */
static inline int isspace(int c) {
    return (c == ' ' || c == '\t' || c == '\n' || 
            c == '\r' || c == '\f' || c == '\v');
}

/* Test for uppercase letter */
static inline int isupper(int c) {
    return (c >= 'A' && c <= 'Z');
}

/* Test for lowercase letter */
static inline int islower(int c) {
    return (c >= 'a' && c <= 'z');
}

/* Test for control character */
static inline int iscntrl(int c) {
    return ((c >= 0 && c <= 31) || c == 127);
}

/* Test for printable character (including space) */
static inline int isprint(int c) {
    return (c >= 32 && c <= 126);
}

/* Test for printable character (excluding space) */
static inline int isgraph(int c) {
    return (c > 32 && c <= 126);
}

/* Test for punctuation character */
static inline int ispunct(int c) {
    return isgraph(c) && !isalnum(c);
}

/* Test for hexadecimal digit */
static inline int isxdigit(int c) {
    return isdigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

/* Test for blank character (space or tab) */
static inline int isblank(int c) {
    return (c == ' ' || c == '\t');
}

/* Test for any character with high bit set (non-ASCII) */
static inline int isascii(int c) {
    return ((unsigned int)c <= 127);
}

/* Character conversion functions */

/* Convert to lowercase */
static inline int tolower(int c) {
    return isupper(c) ? (c - 'A' + 'a') : c;
}

/* Convert to uppercase */
static inline int toupper(int c) {
    return islower(c) ? (c - 'a' + 'A') : c;
}

/* Clear high bit (make ASCII) */
static inline int toascii(int c) {
    return (c & 0x7F);
}

/* Locale-specific versions (same as above for C locale) */
int isalpha_l(int c, locale_t locale);
int isdigit_l(int c, locale_t locale);
int isalnum_l(int c, locale_t locale);
int isspace_l(int c, locale_t locale);
int isupper_l(int c, locale_t locale);
int islower_l(int c, locale_t locale);
int iscntrl_l(int c, locale_t locale);
int isprint_l(int c, locale_t locale);
int isgraph_l(int c, locale_t locale);
int ispunct_l(int c, locale_t locale);
int isxdigit_l(int c, locale_t locale);
int isblank_l(int c, locale_t locale);
int tolower_l(int c, locale_t locale);
int toupper_l(int c, locale_t locale);

#ifdef __cplusplus
}
#endif

#endif /* _CTYPE_H */
