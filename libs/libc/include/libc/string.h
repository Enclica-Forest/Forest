/*
 * string.h - String handling functions
 * 
 * C23 compatible string functions for Fern libc.
 */
#ifndef _STRING_H
#define _STRING_H

#define __STDC_VERSION_STRING_H__ 202311L

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* Copying functions */
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);

/* Concatenation functions */
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);

/* Comparison functions */
int memcmp(const void *s1, const void *s2, size_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
int strcoll(const char *s1, const char *s2);
size_t strxfrm(char *dest, const char *src, size_t n);

/* Search functions */
void *memchr(const void *s, int c, size_t n);
char *strchr(const char *s, int c);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
char *strrchr(const char *s, int c);
size_t strspn(const char *s, const char *accept);
char *strstr(const char *haystack, const char *needle);
char *strtok(char *str, const char *delim);

/* Miscellaneous functions */
void *memset(void *s, int c, size_t n);
char *strerror(int errnum);
size_t strlen(const char *s);

/* POSIX extensions */
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
size_t strnlen(const char *s, size_t maxlen);
char *strtok_r(char *str, const char *delim, char **saveptr);
int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);
char *strsep(char **stringp, const char *delim);
void *memrchr(const void *s, int c, size_t n);
void *memmem(const void *haystack, size_t haystacklen,
             const void *needle, size_t needlelen);
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);

/* BSD extension */
void bzero(void *s, size_t n);
void bcopy(const void *src, void *dest, size_t n);
int bcmp(const void *s1, const void *s2, size_t n);
char *index(const char *s, int c);
char *rindex(const char *s, int c);

/* Explicit memory functions (C23) */
void *memset_explicit(void *s, int c, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* _STRING_H */
