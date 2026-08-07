/*
 * errno.c - Error number handling for Fern libc
 * 
 * This file provides the errno variable and thread-safe errno access.
 * In a multithreaded environment, each thread would have its own errno.
 */

#include <errno.h>
#include <string.h>
#include <unistd.h>

/* Global errno variable */
int __errno_storage = 0;

/* Thread-safe errno access */
int *__errno_location(void) {
    /* In a full implementation with TLS, this would return thread-local storage */
    return &__errno_storage;
}

/* perror - print error message */
void perror(const char *s) {
    const char *err_str = strerror(errno);
    
    if (s && *s) {
        write(STDERR_FILENO, s, strlen(s));
        write(STDERR_FILENO, ": ", 2);
    }
    write(STDERR_FILENO, err_str, strlen(err_str));
    write(STDERR_FILENO, "\n", 1);
}
