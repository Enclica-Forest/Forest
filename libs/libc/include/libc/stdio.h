/*
 * stdio.h - Input/output
 * 
 * C23 compatible standard I/O functions for Fern libc.
 */
#ifndef _STDIO_H
#define _STDIO_H

#define __STDC_VERSION_STDIO_H__ 202311L

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>
#include <stdarg.h>

/* End-of-file and error indicators */
#define EOF (-1)

/* Seek constants */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Buffering modes */
#define _IONBF 0    /* Unbuffered */
#define _IOLBF 1    /* Line buffered */
#define _IOFBF 2    /* Fully buffered */

/* Default buffer size */
#define BUFSIZ 8192

/* Maximum filename length */
#define FILENAME_MAX 4096

/* Maximum number of files that can be open simultaneously */
#define FOPEN_MAX 1024

/* Recommended size for tmpnam buffer */
#define L_tmpnam 20

/* Maximum number of unique temporary file names */
#define TMP_MAX 238328

/* File position type */
typedef long fpos_t;

/* File structure */
typedef struct _FILE {
    int fd;             /* File descriptor */
    int flags;          /* Mode and status flags */
    char *buffer;       /* Buffer pointer */
    size_t buf_size;    /* Buffer size */
    size_t buf_pos;     /* Current position in buffer */
    size_t buf_len;     /* Valid data length in buffer */
    int error;          /* Error indicator */
    int eof;            /* End-of-file indicator */
    int unget;          /* Character pushed back by ungetc (-1 if none) */
} FILE;

/* Standard streams */
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* File operations */
FILE *fopen(const char *filename, const char *mode);
FILE *freopen(const char *filename, const char *mode, FILE *stream);
int fclose(FILE *stream);
int fflush(FILE *stream);
void setbuf(FILE *stream, char *buf);
int setvbuf(FILE *stream, char *buf, int mode, size_t size);

/* Formatted input/output */
int fprintf(FILE *stream, const char *format, ...);
int fscanf(FILE *stream, const char *format, ...);
int printf(const char *format, ...);
int scanf(const char *format, ...);
int snprintf(char *s, size_t n, const char *format, ...);
int sprintf(char *s, const char *format, ...);
int sscanf(const char *s, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list ap);
int vfscanf(FILE *stream, const char *format, va_list ap);
int vprintf(const char *format, va_list ap);
int vscanf(const char *format, va_list ap);
int vsnprintf(char *s, size_t n, const char *format, va_list ap);
int vsprintf(char *s, const char *format, va_list ap);
int vsscanf(const char *s, const char *format, va_list ap);

/* Character input/output */
int fgetc(FILE *stream);
char *fgets(char *s, int n, FILE *stream);
int fputc(int c, FILE *stream);
int fputs(const char *s, FILE *stream);
int getc(FILE *stream);
int getchar(void);
int putc(int c, FILE *stream);
int putchar(int c);
int puts(const char *s);
int ungetc(int c, FILE *stream);

/* Deprecated but still supported */
char *gets(char *s);  /* DEPRECATED: Use fgets instead */

/* Direct input/output */
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

/* File positioning */
int fgetpos(FILE *stream, fpos_t *pos);
int fseek(FILE *stream, long offset, int whence);
int fsetpos(FILE *stream, const fpos_t *pos);
long ftell(FILE *stream);
void rewind(FILE *stream);

/* Error handling */
void clearerr(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void perror(const char *s);

/* File removal and renaming */
int remove(const char *filename);
int rename(const char *old, const char *new);

/* Temporary files */
FILE *tmpfile(void);
char *tmpnam(char *s);

/* POSIX extensions */
int fileno(FILE *stream);
FILE *fdopen(int fd, const char *mode);
FILE *popen(const char *command, const char *type);
int pclose(FILE *stream);
ssize_t getline(char **lineptr, size_t *n, FILE *stream);
ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream);
int dprintf(int fd, const char *format, ...);
int vdprintf(int fd, const char *format, va_list ap);

/* GNU extensions */
int asprintf(char **strp, const char *fmt, ...);
int vasprintf(char **strp, const char *fmt, va_list ap);

/* fseeko/ftello for large file support */
int fseeko(FILE *stream, off_t offset, int whence);
off_t ftello(FILE *stream);

/* Locking (POSIX) */
void flockfile(FILE *stream);
int ftrylockfile(FILE *stream);
void funlockfile(FILE *stream);

/* Unlocked I/O (faster but not thread-safe) */
int getc_unlocked(FILE *stream);
int getchar_unlocked(void);
int putc_unlocked(int c, FILE *stream);
int putchar_unlocked(int c);
void clearerr_unlocked(FILE *stream);
int feof_unlocked(FILE *stream);
int ferror_unlocked(FILE *stream);
int fileno_unlocked(FILE *stream);
int fflush_unlocked(FILE *stream);
int fgetc_unlocked(FILE *stream);
int fputc_unlocked(int c, FILE *stream);
size_t fread_unlocked(void *ptr, size_t size, size_t n, FILE *stream);
size_t fwrite_unlocked(const void *ptr, size_t size, size_t n, FILE *stream);
char *fgets_unlocked(char *s, int n, FILE *stream);
int fputs_unlocked(const char *s, FILE *stream);

/* Macros for compatibility */
#define getc(stream) fgetc(stream)
#define putc(c, stream) fputc(c, stream)

#ifdef __cplusplus
}
#endif

#endif /* _STDIO_H */
