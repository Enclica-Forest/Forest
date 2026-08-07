/*
 * types.h - Fern fundamental types
 * 
 * This header provides the basic integer types used throughout Fern.
 * Compatible with Linux/POSIX systems.
 */
#ifndef FOREST_TYPES_H
#define FOREST_TYPES_H

#include <stdbool.h>

/* Fixed-width integer types */
typedef signed char int8;
typedef unsigned char uint8;

typedef signed short int16;
typedef unsigned short uint16;

typedef signed int int32;
typedef unsigned int uint32;

typedef signed long long int64;
typedef unsigned long long uint64;

/* Standard C integer types (C99) */
typedef signed char int8_t;
typedef unsigned char uint8_t;

typedef signed short int16_t;
typedef unsigned short uint16_t;

typedef signed int int32_t;
typedef unsigned int uint32_t;

typedef signed long long int64_t;
typedef unsigned long long uint64_t;

/* Pointer-sized types */
#ifndef ARCH_64BIT
#if defined(__x86_64__) || defined(_M_X64) || defined(__LP64__)
#define ARCH_64BIT 1
#define ARCH_32BIT 0
#else
#define ARCH_64BIT 0
#define ARCH_32BIT 1
#endif
#endif

#if ARCH_64BIT
typedef int64_t intptr_t;
typedef uint64_t uintptr_t;
typedef int64_t ptrdiff_t;
#else
typedef int32_t intptr_t;
typedef uint32_t uintptr_t;
typedef int32_t ptrdiff_t;
#endif

/* Size types */
typedef uintptr_t size_t;

#if ARCH_64BIT
typedef int64_t ssize_t;
typedef int64_t off_t;
#else
typedef int32_t ssize_t;
typedef int32_t off_t;
#endif

/* Process types */
typedef int32_t pid_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef uint32_t mode_t;
typedef uint32_t dev_t;
typedef uint32_t ino_t;
typedef uint32_t nlink_t;
typedef int32_t blksize_t;
typedef int64_t blkcnt_t;
typedef int32_t clockid_t;
typedef int32_t time_t;
typedef int32_t suseconds_t;

/* String type alias */
typedef char* string;

/* Utility macros */
#define low_16(address) ((uint16)((address) & 0xFFFF))
#define high_16(address) ((uint16)(((address) >> 16) & 0xFFFF))

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#endif /* FOREST_TYPES_H */
