#ifndef _DG_STDLIB_H
#define _DG_STDLIB_H
#include <stddef.h>
void *malloc(size_t n);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void  free(void *p);
int   atoi(const char *s);
long  atol(const char *s);
double atof(const char *s);
long  strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
double strtod(const char *s, char **end);
int   abs(int x);
long  labs(long x);
void  qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));
void  exit(int code);
void  abort(void);
char *getenv(const char *name);
int   atexit(void (*fn)(void));
int   system(const char *cmd);
int   rand(void);
void  srand(unsigned s);
void *bsearch(const void *key, const void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));
#define RAND_MAX 0x7fffffff
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#endif
