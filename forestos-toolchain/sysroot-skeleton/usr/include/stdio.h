#ifndef _STDIO_H
#define _STDIO_H

#include <sys/types.h>
#include <forestos/syscalls.h>

/* Forest OS standard I/O library */

/* File I/O */
typedef struct {
    int fd;
    int flags;
    char *buffer;
    size_t buffer_size;
    size_t buffer_pos;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* File operations */
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fflush(FILE *stream);

/* Character I/O */
int getchar(void);
int putchar(int c);
int getc(FILE *stream);
int putc(int c, FILE *stream);
int fgetc(FILE *stream);
int fputc(int c, FILE *stream);

/* String I/O */
char *gets(char *s);
int puts(const char *s);
char *fgets(char *s, int size, FILE *stream);
int fputs(const char *s, FILE *stream);

/* Formatted I/O */
int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int sprintf(char *str, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int scanf(const char *format, ...);
int fscanf(FILE *stream, const char *format, ...);
int sscanf(const char *str, const char *format, ...);

/* File positioning */
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);

/* Constants */
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 1024

#endif /* _STDIO_H */