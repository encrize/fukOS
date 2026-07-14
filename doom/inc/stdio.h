#ifndef _DG_STDIO_H
#define _DG_STDIO_H
#include <stddef.h>
#include <stdarg.h>

typedef struct _DG_FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 8192

int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int sscanf(const char *str, const char *fmt, ...);
int vsscanf(const char *str, const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);
int puts(const char *s);
int fputs(const char *s, FILE *f);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int fgetc(FILE *f);
int getc(FILE *f);
char *fgets(char *buf, int n, FILE *f);
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *ptr, size_t sz, size_t n, FILE *f);
size_t fwrite(const void *ptr, size_t sz, size_t n, FILE *f);
int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
int fflush(FILE *f);
int remove(const char *path);
int rename(const char *a, const char *b);
void setbuf(FILE *f, char *buf);
int fileno(FILE *f);
#endif
