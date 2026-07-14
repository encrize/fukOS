/* Minimal freestanding C library for the DOOM port on fukOS.
   Compiled with -nostdinc against the private headers in doom/inc.
   Everything here backs the standard functions DOOM expects. */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#include "console.h"
#include "io.h"
#include "fat.h"

/* memcpy/memset/memcmp are implemented in the kernel's util.c; pull in their
   prototypes (GCC 14+ treats implicit declarations as hard errors). */
#include <string.h>

/* From dg_myos.c */
unsigned char dg_wait_key(void);

int errno = 0;

/* ================================================================== */
/* Console output                                                     */
/* ================================================================== */
static void putstr(const char *s) { while (*s) console_putc(*s++); }

/* ================================================================== */
/* Heap: first-fit free list with coalescing                          */
/* ================================================================== */
typedef struct block {
    size_t        size;    /* payload size in bytes */
    int           free;
    struct block *next;
    struct block *prev;
} block_t;

#define ALIGN8(x) (((x) + 7u) & ~((size_t)7u))
#define HDR ALIGN8(sizeof(block_t))

static uint8_t  *heap_base;
static uint8_t  *heap_end;    /* end of usable region */
static uint8_t  *heap_brk;    /* current top of allocated arena */
static block_t  *free_head;

void dg_heap_init(uintptr_t base, uintptr_t end)
{
    heap_base = (uint8_t *)base;
    heap_end  = (uint8_t *)end;
    heap_brk  = heap_base;
    free_head = NULL;
}

static block_t *find_fit(size_t need)
{
    for (block_t *b = free_head; b; b = b->next)
        if (b->free && b->size >= need) return b;
    return NULL;
}

static block_t *grow(size_t need)
{
    uint8_t *p = heap_brk;
    size_t total = HDR + need;
    if (p + total > heap_end) return NULL;   /* out of memory */
    heap_brk = p + total;
    block_t *b = (block_t *)p;
    b->size = need;
    b->free = 0;
    b->prev = NULL;
    b->next = free_head;
    if (free_head) free_head->prev = b;
    free_head = b;
    return b;
}

void *malloc(size_t n)
{
    if (n == 0) n = 1;
    n = ALIGN8(n);
    block_t *b = find_fit(n);
    if (b) {
        /* split if there is room for another block+min payload */
        if (b->size >= n + HDR + 16) {
            uint8_t *raw = (uint8_t *)b + HDR + n;
            block_t *nb = (block_t *)raw;
            nb->size = b->size - n - HDR;
            nb->free = 1;
            nb->prev = NULL;
            nb->next = free_head;
            if (free_head) free_head->prev = nb;
            free_head = nb;
            b->size = n;
        }
        b->free = 0;
        return (uint8_t *)b + HDR;
    }
    b = grow(n);
    if (!b) return NULL;
    return (uint8_t *)b + HDR;
}

void free(void *p)
{
    if (!p) return;
    block_t *b = (block_t *)((uint8_t *)p - HDR);
    b->free = 1;
    /* Coalesce with a physically-adjacent block that follows immediately. */
    uint8_t *after = (uint8_t *)b + HDR + b->size;
    if (after == heap_brk) {
        /* release back to the arena top */
        if (b->prev) b->prev->next = b->next; else free_head = b->next;
        if (b->next) b->next->prev = b->prev;
        heap_brk = (uint8_t *)b;
    }
}

void *calloc(size_t n, size_t sz)
{
    size_t total = n * sz;
    void *p = malloc(total);
    if (p) { uint8_t *d = p; for (size_t i = 0; i < total; i++) d[i] = 0; }
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }
    block_t *b = (block_t *)((uint8_t *)p - HDR);
    if (b->size >= n) return p;
    void *np = malloc(n);
    if (!np) return NULL;
    uint8_t *s = p, *d = np;
    for (size_t i = 0; i < b->size; i++) d[i] = s[i];
    free(p);
    return np;
}

/* ================================================================== */
/* memory / string                                                    */
/* ================================================================== */
/* memset/memcpy/memcmp are provided by the kernel's util.o (do not redefine). */
void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst; const uint8_t *s = src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}
void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = s;
    while (n--) { if (*p == (uint8_t)c) return (void *)p; p++; }
    return NULL;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *strncpy(char *d, const char *s, size_t n)
{
    char *r = d;
    while (n && (*d = *s)) { d++; s++; n--; }
    while (n--) *d++ = 0;
    return r;
}
char *strcat(char *d, const char *s) { char *r = d; while (*d) d++; while ((*d++ = *s++)) ; return r; }
char *strncat(char *d, const char *s, size_t n)
{
    char *r = d; while (*d) d++;
    while (n-- && *s) *d++ = *s++;
    *d = 0; return r;
}
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
char *strchr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return (c == 0) ? (char *)s : NULL;
}
char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (; ; s++) { if (*s == (char)c) last = s; if (!*s) break; }
    return (char *)last;
}
char *strstr(const char *h, const char *n)
{
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return NULL;
}
size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    for (; s[n]; n++) { const char *a = accept; while (*a && *a != s[n]) a++; if (!*a) break; }
    return n;
}
size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    for (; s[n]; n++) { const char *r = reject; while (*r && *r != s[n]) r++; if (*r) break; }
    return n;
}
char *strpbrk(const char *s, const char *accept)
{
    for (; *s; s++) { const char *a = accept; while (*a) { if (*a == *s) return (char *)s; a++; } }
    return NULL;
}
char *strtok(char *s, const char *delim)
{
    static char *save;
    if (s) save = s;
    if (!save) return NULL;
    save += strspn(save, delim);
    if (!*save) { save = NULL; return NULL; }
    char *tok = save;
    save += strcspn(save, delim);
    if (*save) { *save = 0; save++; } else save = NULL;
    return tok;
}
char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
static int ci(int c) { if (c >= 'A' && c <= 'Z') c += 32; return c; }
int strcasecmp(const char *a, const char *b)
{
    while (*a && ci((unsigned char)*a) == ci((unsigned char)*b)) { a++; b++; }
    return ci((unsigned char)*a) - ci((unsigned char)*b);
}
int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n && *a && ci((unsigned char)*a) == ci((unsigned char)*b)) { a++; b++; n--; }
    if (n == 0) return 0;
    return ci((unsigned char)*a) - ci((unsigned char)*b);
}
char *strerror(int e) { (void)e; return "error"; }

/* ================================================================== */
/* ctype                                                              */
/* ================================================================== */
int isalpha(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
int isdigit(int c){ return c>='0'&&c<='9'; }
int isalnum(int c){ return isalpha(c)||isdigit(c); }
int isspace(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\v'||c=='\f'; }
int isupper(int c){ return c>='A'&&c<='Z'; }
int islower(int c){ return c>='a'&&c<='z'; }
int isprint(int c){ return c>=0x20&&c<0x7f; }
int iscntrl(int c){ return c<0x20||c==0x7f; }
int ispunct(int c){ return isprint(c)&&!isalnum(c)&&c!=' '; }
int isxdigit(int c){ return isdigit(c)||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
int isgraph(int c){ return c>0x20&&c<0x7f; }
int toupper(int c){ return islower(c)?c-32:c; }
int tolower(int c){ return isupper(c)?c+32:c; }

/* ================================================================== */
/* number parsing                                                     */
/* ================================================================== */
long strtol(const char *s, char **end, int base)
{
    const char *p = s; while (isspace((unsigned char)*p)) p++;
    int neg = 0; if (*p=='+'||*p=='-') { neg = (*p=='-'); p++; }
    if ((base==0||base==16) && p[0]=='0' && (p[1]=='x'||p[1]=='X')) { p+=2; base=16; }
    else if (base==0 && p[0]=='0') { base=8; }
    else if (base==0) base=10;
    long val = 0;
    for (;;) {
        int c = *p; int d;
        if (c>='0'&&c<='9') d=c-'0';
        else if (c>='a'&&c<='z') d=c-'a'+10;
        else if (c>='A'&&c<='Z') d=c-'A'+10;
        else break;
        if (d>=base) break;
        val = val*base + d; p++;
    }
    if (end) *end = (char *)p;
    return neg ? -val : val;
}
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtol(s,end,base); }
int atoi(const char *s){ return (int)strtol(s,NULL,10); }
long atol(const char *s){ return strtol(s,NULL,10); }
double strtod(const char *s, char **end)
{
    const char *p = s; while (isspace((unsigned char)*p)) p++;
    int neg = 0; if (*p=='+'||*p=='-'){ neg=(*p=='-'); p++; }
    double v = 0;
    while (isdigit((unsigned char)*p)) { v = v*10 + (*p-'0'); p++; }
    if (*p=='.') { p++; double f=0.1; while (isdigit((unsigned char)*p)) { v += (*p-'0')*f; f*=0.1; p++; } }
    if (end) *end=(char*)p;
    return neg ? -v : v;
}
double atof(const char *s){ return strtod(s,NULL); }
int abs(int x){ return x<0?-x:x; }
long labs(long x){ return x<0?-x:x; }

static unsigned long rnd_state = 1;
int rand(void){ rnd_state = rnd_state*1103515245u + 12345u; return (int)((rnd_state>>16)&0x7fff); }
void srand(unsigned s){ rnd_state = s; }

char *getenv(const char *name){ (void)name; return NULL; }
int atexit(void (*fn)(void)){ (void)fn; return 0; }
int system(const char *cmd){ (void)cmd; return -1; }

/* simple insertion/quick sort (qsort) */
static void swp(uint8_t *a, uint8_t *b, size_t sz){ while (sz--){ uint8_t t=*a; *a++=*b; *b++=t; } }
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void*,const void*))
{
    uint8_t *b = base;
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && cmp(b+(j-1)*sz, b+j*sz) > 0; j--)
            swp(b+(j-1)*sz, b+j*sz, sz);
}
void *bsearch(const void *key, const void *base, size_t n, size_t sz, int (*cmp)(const void*,const void*))
{
    size_t lo = 0, hi = n;
    const uint8_t *b = base;
    while (lo < hi) {
        size_t mid = (lo+hi)/2;
        int r = cmp(key, b+mid*sz);
        if (r == 0) return (void *)(b+mid*sz);
        if (r < 0) hi = mid; else lo = mid+1;
    }
    return NULL;
}

/* ================================================================== */
/* math (minimal, DOOM barely uses these)                             */
/* ================================================================== */
double fabs(double x){ return x<0?-x:x; }
double floor(double x){ long i=(long)x; if ((double)i>x) i--; return (double)i; }
double ceil(double x){ long i=(long)x; if ((double)i<x) i++; return (double)i; }
double fmod(double a, double b){ if (b==0) return 0; long q=(long)(a/b); return a - (double)q*b; }
double sqrt(double x){ if (x<=0) return 0; double g=x; for (int i=0;i<40;i++) g=0.5*(g+x/g); return g; }
double sin(double x){
    while (x> 3.14159265358979) x-=6.28318530717959;
    while (x<-3.14159265358979) x+=6.28318530717959;
    double t=x, s=x; for (int i=1;i<10;i++){ t*=-x*x/((2*i)*(2*i+1)); s+=t; } return s;
}
double cos(double x){ return sin(x+1.5707963267949); }
double tan(double x){ double c=cos(x); return c==0?0:sin(x)/c; }
double atan(double x){ /* crude */ double s=x,t=x,x2=x*x; for(int i=1;i<12;i++){ t*=-x2; s+=t/(2*i+1);} return s; }
double atan2(double y, double x){ if (x>0) return atan(y/x); if (x<0) return atan(y/x)+3.14159265358979; return y>0?1.5707963:-1.5707963; }
double pow(double b, double e){ double r=1; int n=(int)e; for(int i=0;i<n;i++) r*=b; return r; }
double exp(double x){ double r=1,t=1; for(int i=1;i<20;i++){ t*=x/i; r+=t; } return r; }
double log(double x){ if (x<=0) return 0; double y=(x-1)/(x+1),y2=y*y,s=y,t=y; for(int i=1;i<20;i++){ t*=y2; s+=t/(2*i+1);} return 2*s; }

/* ================================================================== */
/* time                                                               */
/* ================================================================== */
long time(long *t){ extern uint32_t DG_GetTicksMs(void); long s = DG_GetTicksMs()/1000; if (t) *t=s; return s; }
long clock(void){ extern uint32_t DG_GetTicksMs(void); return DG_GetTicksMs(); }

/* ================================================================== */
/* Embedded WAD + in-RAM files                                        */
/* ================================================================== */
extern const uint8_t doom1_wad_start[];
extern const uint8_t doom1_wad_end[];

typedef struct { char name[64]; uint8_t *buf; long size; long cap; int used; } ramfile_t;
#define MAX_RAMFILES 16
static ramfile_t ramfiles[MAX_RAMFILES];

struct _DG_FILE {
    const uint8_t *rdata;   /* read source (wad or ramfile) */
    ramfile_t     *rf;      /* backing ram file when writable */
    uint8_t       *owned;   /* heap buffer loaded from FAT */
    long           size;
    long           pos;
    int            writable;
    int            std;     /* 1 stdout, 2 stderr */
    int            eof;
    int            used;
};
typedef struct _DG_FILE FILE;

static FILE file_pool[24];
static FILE std_out_obj = { .std = 1, .used = 1 };
static FILE std_err_obj = { .std = 2, .used = 1 };
static FILE std_in_obj  = { .used = 1 };
FILE *stdout = &std_out_obj;
FILE *stderr = &std_err_obj;
FILE *stdin  = &std_in_obj;

static const char *basename_of(const char *p)
{
    const char *b = p;
    for (const char *s = p; *s; s++) if (*s=='/'||*s=='\\') b = s+1;
    return b;
}
static int ends_wad(const char *p)
{
    size_t n = strlen(p);
    if (n < 4) return 0;
    return p[n-4]=='.' && ci(p[n-3])=='w' && ci(p[n-2])=='a' && ci(p[n-1])=='d';
}

static const char *fat_name_of(const char *path)
{
    /* fukOS FAT helpers operate in the current directory.  Doom passes paths
       such as /.savegame/doomsav0.dsg; store those by basename in FAT root/current
       dir so saves and screenshots survive reboot without needing chdir support. */
    return basename_of(path);
}
static ramfile_t *ram_find(const char *name)
{
    const char *b = basename_of(name);
    for (int i=0;i<MAX_RAMFILES;i++)
        if (ramfiles[i].used && strcasecmp(ramfiles[i].name, b)==0) return &ramfiles[i];
    return NULL;
}
static ramfile_t *ram_create(const char *name)
{
    ramfile_t *rf = ram_find(name);
    if (rf) { rf->size = 0; return rf; }
    for (int i=0;i<MAX_RAMFILES;i++) if (!ramfiles[i].used) {
        rf = &ramfiles[i]; rf->used=1;
        const char *b = basename_of(name);
        size_t j=0; for (; b[j] && j<63; j++) rf->name[j]=b[j]; rf->name[j]=0;
        rf->buf=NULL; rf->size=0; rf->cap=0; return rf;
    }
    return NULL;
}
static FILE *file_alloc(void)
{
    for (unsigned i=0;i<sizeof(file_pool)/sizeof(file_pool[0]);i++)
        if (!file_pool[i].used) { FILE *f=&file_pool[i]; memset(f,0,sizeof(*f)); f->used=1; return f; }
    return NULL;
}

FILE *fopen(const char *path, const char *mode)
{
    if (!path || !mode) return NULL;
    int write = 0, append = 0;
    for (const char *m=mode; *m; m++){ if (*m=='w') write=1; if (*m=='a') append=1; }

    if (!write && !append) {
        /* read */
        if (ends_wad(path)) {
            FILE *f = file_alloc(); if (!f) return NULL;
            f->rdata = doom1_wad_start;
            f->size  = (long)(doom1_wad_end - doom1_wad_start);
            f->pos = 0; f->writable = 0;
            return f;
        }
        ramfile_t *rf = ram_find(path);
        if (rf) {
            FILE *f = file_alloc(); if (!f) return NULL;
            f->rdata = rf->buf; f->rf = rf; f->size = rf->size; f->pos = 0; f->writable = 0;
            return f;
        }
        if (fat_mounted()) {
            fat_file ff;
            const char *name = fat_name_of(path);
            if (fat_open(name, &ff)) {
                FILE *f = file_alloc(); if (!f) return NULL;
                uint8_t *buf = malloc(ff.size ? ff.size : 1);
                if (!buf) { f->used = 0; return NULL; }
                uint32_t got = fat_read(&ff, buf, ff.size);
                if (got != ff.size) { free(buf); f->used = 0; return NULL; }
                f->owned = buf; f->rdata = buf; f->size = (long)got;
                f->pos = 0; f->writable = 0;
                return f;
            }
        }
        return NULL;
    }

    /* write / append -> RAM file */
    ramfile_t *rf = append ? ram_create(path) : ram_create(path);
    if (!rf) return NULL;
    if (!append) rf->size = 0;
    FILE *f = file_alloc(); if (!f) return NULL;
    f->rf = rf; f->writable = 1; f->pos = append ? rf->size : 0; f->size = rf->size;
    return f;
}

int fclose(FILE *f)
{
    if (!f || f->std) return 0;
    if (f->writable && f->rf && fat_mounted()) {
        const char *name = fat_name_of(f->rf->name);
        if (!fat_write_file(name, f->rf->buf, (uint32_t)f->rf->size)) {
            f->used = 0;
            return -1;
        }
    }
    if (f->owned) {
        free(f->owned);
        f->owned = NULL;
    }
    f->used = 0;
    return 0;
}

size_t fread(void *ptr, size_t sz, size_t n, FILE *f)
{
    if (!f || f->writable) return 0;
    const uint8_t *src = f->writable ? (f->rf?f->rf->buf:NULL) : f->rdata;
    if (f->rf && !f->writable) { src = f->rf->buf; f->size = f->rf->size; }
    long want = (long)(sz*n);
    long avail = f->size - f->pos;
    if (want > avail) want = avail;
    if (want <= 0) { f->eof = 1; return 0; }
    memcpy(ptr, src + f->pos, (size_t)want);
    f->pos += want;
    return (size_t)(want / (sz ? sz : 1));
}

static int ram_ensure(ramfile_t *rf, long need)
{
    if (need <= rf->cap) return 1;
    long ncap = rf->cap ? rf->cap*2 : 4096;
    while (ncap < need) ncap *= 2;
    uint8_t *nb = realloc(rf->buf, (size_t)ncap);
    if (!nb) return 0;
    rf->buf = nb; rf->cap = ncap;
    return 1;
}

size_t fwrite(const void *ptr, size_t sz, size_t n, FILE *f)
{
    if (!f) return 0;
    long len = (long)(sz*n);
    if (f->std) { const char *p=ptr; for (long i=0;i<len;i++) console_putc(p[i]); return n; }
    if (!f->writable || !f->rf) return 0;
    if (!ram_ensure(f->rf, f->pos + len)) return 0;
    memcpy(f->rf->buf + f->pos, ptr, (size_t)len);
    f->pos += len;
    if (f->pos > f->rf->size) f->rf->size = f->pos;
    f->size = f->rf->size;
    return n;
}

int fseek(FILE *f, long off, int whence)
{
    if (!f) return -1;
    long base = 0;
    if (whence == 0) base = 0;
    else if (whence == 1) base = f->pos;
    else if (whence == 2) base = (f->rf && f->writable) ? f->rf->size : f->size;
    f->pos = base + off;
    f->eof = 0;
    return 0;
}
long ftell(FILE *f){ return f?f->pos:-1; }
void rewind(FILE *f){ if (f){ f->pos=0; f->eof=0; } }
int feof(FILE *f){ return f?f->eof:1; }
int ferror(FILE *f){ (void)f; return 0; }
int fflush(FILE *f){ (void)f; return 0; }
void setbuf(FILE *f, char *b){ (void)f; (void)b; }
int fileno(FILE *f){ return f?(f->std?f->std-1:3):-1; }
int remove(const char *p){ ramfile_t *rf=ram_find(p); if (rf){ rf->used=0; if (rf->buf) free(rf->buf); rf->buf=NULL; rf->cap=0; } if (fat_mounted() && fat_exists(fat_name_of(p))) fat_delete_file(fat_name_of(p)); return 0; }
int rename(const char *a, const char *b){ ramfile_t *rf=ram_find(a); if (rf){ const char *nb=basename_of(b); size_t j=0; for(; nb[j]&&j<63;j++) rf->name[j]=nb[j]; rf->name[j]=0; } if (fat_mounted()) { const char *fa=fat_name_of(a), *fb=fat_name_of(b); if (fat_exists(fb)) fat_delete_file(fb); if (fat_exists(fa)) return fat_move_file(fa, fb) ? 0 : -1; } return rf ? 0 : -1; }

int fgetc(FILE *f)
{
    unsigned char c;
    if (fread(&c,1,1,f) != 1) return -1;
    return c;
}
int getc(FILE *f){ return fgetc(f); }
char *fgets(char *buf, int n, FILE *f)
{
    int i=0;
    while (i < n-1) { int c=fgetc(f); if (c<0){ if (i==0) return NULL; break; } buf[i++]=(char)c; if (c=='\n') break; }
    buf[i]=0; return buf;
}

/* file open/close/stat stubs (unix path, unused by compiled DOOM set) */
int open(const char *p, int fl, ...){ (void)p;(void)fl; return -1; }
int close(int fd){ (void)fd; return 0; }
int access(const char *p, int m){ (void)m; if (ends_wad(p)||ram_find(p)) return 0; if (fat_mounted() && fat_exists(fat_name_of(p))) return 0; return -1; }
int unlink(const char *p){ return remove(p); }
int mkdir(const char *p, unsigned m){ (void)p;(void)m; return 0; }
int stat(const char *p, void *st){ (void)p;(void)st; return -1; }
int chdir(const char *p){ (void)p; return 0; }
char *getcwd(char *b, size_t n){ if (b&&n){ b[0]='/'; b[1]=0; } return b; }

/* ================================================================== */
/* printf family                                                      */
/* ================================================================== */
typedef struct { char *buf; size_t cap; size_t len; int to_console; } sink_t;
static void sink_c(sink_t *s, char c)
{
    if (s->to_console) { console_putc(c); s->len++; return; }
    if (s->len + 1 < s->cap) s->buf[s->len] = c;
    s->len++;
}
static void sink_s(sink_t *s, const char *str){ while (*str) sink_c(s,*str++); }

static void emit_ull(sink_t *s, unsigned long long v, int base, int upper,
                     int width, int zero, int left, int plus, int space, int neg,
                     int prec)
{
    char tmp[32]; int n=0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    /* A precision of 0 with a value of 0 produces no digits. */
    if (v == 0) { if (prec != 0) tmp[n++]='0'; }
    else while (v) { tmp[n++]=dig[v%base]; v/=base; }
    /* Precision = minimum number of digits (zero-padded). It also disables
       the '0' flag per the C standard. */
    int zeros = (prec > n) ? (prec - n) : 0;
    if (prec >= 0) zero = 0;
    char sign = 0;
    if (neg) sign='-'; else if (plus) sign='+'; else if (space) sign=' ';
    int total = n + zeros + (sign?1:0);
    if (!left && !zero) for (int i=total;i<width;i++) sink_c(s,' ');
    if (sign) sink_c(s,sign);
    if (!left && zero) for (int i=total;i<width;i++) sink_c(s,'0');
    for (int i=0;i<zeros;i++) sink_c(s,'0');
    while (n) sink_c(s,tmp[--n]);
    if (left) for (int i=total;i<width;i++) sink_c(s,' ');
}

static void emit_double(sink_t *s, double v, int prec, int width, int zero, int left)
{
    if (prec < 0) prec = 6;
    int neg = 0; if (v < 0) { neg=1; v=-v; }
    /* integer part */
    unsigned long long ip = (unsigned long long)v;
    double frac = v - (double)ip;
    char ib[32]; int in=0; if (ip==0) ib[in++]='0'; while (ip){ ib[in++]='0'+(int)(ip%10); ip/=10; }
    char fb[40]; int fn=0;
    for (int i=0;i<prec;i++){ frac*=10; int d=(int)frac; if (d>9)d=9; fb[fn++]='0'+d; frac-=d; }
    int total = in + (neg?1:0) + (prec?prec+1:0);
    if (!left && !zero) for (int i=total;i<width;i++) sink_c(s,' ');
    if (neg) sink_c(s,'-');
    if (!left && zero) for (int i=total;i<width;i++) sink_c(s,'0');
    while (in) sink_c(s, ib[--in]);
    if (prec) { sink_c(s,'.'); for (int i=0;i<fn;i++) sink_c(s,fb[i]); }
    if (left) for (int i=total;i<width;i++) sink_c(s,' ');
}

static int do_format(sink_t *s, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') { sink_c(s,*fmt); continue; }
        fmt++;
        int left=0, zero=0, plus=0, space=0, alt=0;
        for (;;) {
            if (*fmt=='-') left=1; else if (*fmt=='0') zero=1;
            else if (*fmt=='+') plus=1; else if (*fmt==' ') space=1;
            else if (*fmt=='#') alt=1; else break; fmt++;
        }
        int width=0;
        if (*fmt=='*') { width=va_arg(ap,int); fmt++; if (width<0){ left=1; width=-width; } }
        else while (*fmt>='0'&&*fmt<='9') width=width*10+(*fmt++-'0');
        int prec=-1;
        if (*fmt=='.') { fmt++; prec=0; if (*fmt=='*'){ prec=va_arg(ap,int); fmt++; } else while (*fmt>='0'&&*fmt<='9') prec=prec*10+(*fmt++-'0'); }
        int lng=0;
        while (*fmt=='l'){ lng++; fmt++; }
        while (*fmt=='h'){ fmt++; }
        if (*fmt=='z'||*fmt=='t'||*fmt=='j'){ lng=1; fmt++; }
        char c=*fmt;
        switch (c) {
            case 'd': case 'i': {
                long long v = lng>=2 ? va_arg(ap,long long) : (lng==1 ? va_arg(ap,long) : va_arg(ap,int));
                int neg = v<0; unsigned long long uv = neg ? (unsigned long long)(-v) : (unsigned long long)v;
                emit_ull(s,uv,10,0,width,zero,left,plus,space,neg,prec); break; }
            case 'u': { unsigned long long v = lng>=2?va_arg(ap,unsigned long long):(lng==1?va_arg(ap,unsigned long):va_arg(ap,unsigned int)); emit_ull(s,v,10,0,width,zero,left,0,0,0,prec); break; }
            case 'x': { unsigned long long v = lng>=2?va_arg(ap,unsigned long long):(lng==1?va_arg(ap,unsigned long):va_arg(ap,unsigned int)); if(alt&&v) sink_s(s,"0x"); emit_ull(s,v,16,0,width,zero,left,0,0,0,prec); break; }
            case 'X': { unsigned long long v = lng>=2?va_arg(ap,unsigned long long):(lng==1?va_arg(ap,unsigned long):va_arg(ap,unsigned int)); if(alt&&v) sink_s(s,"0X"); emit_ull(s,v,16,1,width,zero,left,0,0,0,prec); break; }
            case 'o': { unsigned long long v = lng>=2?va_arg(ap,unsigned long long):(lng==1?va_arg(ap,unsigned long):va_arg(ap,unsigned int)); emit_ull(s,v,8,0,width,zero,left,0,0,0,prec); break; }
            case 'p': { uintptr_t v=(uintptr_t)va_arg(ap,void*); sink_s(s,"0x"); emit_ull(s,v,16,0,width,zero,left,0,0,0,-1); break; }
            case 'c': { char ch=(char)va_arg(ap,int); if(!left) for(int i=1;i<width;i++) sink_c(s,' '); sink_c(s,ch); if(left) for(int i=1;i<width;i++) sink_c(s,' '); break; }
            case 's': { const char *str=va_arg(ap,const char*); if(!str) str="(null)"; int len=0; while(str[len]&&(prec<0||len<prec)) len++; if(!left) for(int i=len;i<width;i++) sink_c(s,' '); for(int i=0;i<len;i++) sink_c(s,str[i]); if(left) for(int i=len;i<width;i++) sink_c(s,' '); break; }
            case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': { double v=va_arg(ap,double); emit_double(s,v,prec,width,zero,left); break; }
            case '%': sink_c(s,'%'); break;
            case 0: return (int)s->len;
            default: sink_c(s,'%'); sink_c(s,c); break;
        }
    }
    return (int)s->len;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    sink_t s = { buf, n, 0, 0 };
    int r = do_format(&s, fmt, ap);
    if (n) buf[s.len < n ? s.len : n-1] = 0;
    return r;
}
int vsprintf(char *buf, const char *fmt, va_list ap){ return vsnprintf(buf, (size_t)0x7fffffff, fmt, ap); }
int snprintf(char *buf, size_t n, const char *fmt, ...)
{ va_list ap; va_start(ap,fmt); int r=vsnprintf(buf,n,fmt,ap); va_end(ap); return r; }
int sprintf(char *buf, const char *fmt, ...)
{ va_list ap; va_start(ap,fmt); int r=vsprintf(buf,fmt,ap); va_end(ap); return r; }
int vfprintf(FILE *f, const char *fmt, va_list ap)
{ sink_t s = { NULL, 0, 0, 1 }; (void)f; return do_format(&s, fmt, ap); }
int fprintf(FILE *f, const char *fmt, ...)
{ va_list ap; va_start(ap,fmt); int r=vfprintf(f,fmt,ap); va_end(ap); return r; }
int vprintf(const char *fmt, va_list ap){ sink_t s={NULL,0,0,1}; return do_format(&s,fmt,ap); }
int printf(const char *fmt, ...)
{ va_list ap; va_start(ap,fmt); int r=vprintf(fmt,ap); va_end(ap); return r; }
int puts(const char *s){ putstr(s); console_putc('\n'); return 0; }
int fputs(const char *s, FILE *f){ if (f&&f->std) putstr(s); else if (f&&f->writable) fwrite(s,1,strlen(s),f); return 0; }
int fputc(int c, FILE *f){ char ch=(char)c; if (f&&f->std) console_putc(ch); else if (f&&f->writable) fwrite(&ch,1,1,f); return c; }
int putc(int c, FILE *f){ return fputc(c,f); }
int putchar(int c){ console_putc((char)c); return c; }

/* ================================================================== */
/* exit / abort / assert                                              */
/* ================================================================== */
void dg_return_to_shell(void);   /* dg_myos.c: unwind back into the shell */

static void reboot_now(void)
{
    putstr("\n[press any key to reboot]\n");
    dg_wait_key();
    /* Try several reset mechanisms; fall through if one is a no-op. */
    outb(0xCF9, 0x02); outb(0xCF9, 0x0E);            /* chipset reset (0xCF9)  */
    for (volatile int i = 0; i < 1000000; i++) { }
    for (int i = 0; i < 10000; i++) { if ((inb(0x64) & 0x02) == 0) break; }
    outb(0x64, 0xFE);                                /* 8042 keyboard reset    */
    for (volatile int i = 0; i < 1000000; i++) { }
    { struct { unsigned short limit; unsigned int base; } __attribute__((packed)) idt0 = { 0, 0 };
      __asm__ volatile ("lidt %0" :: "m"(idt0));
      __asm__ volatile ("int3"); }                   /* triple fault (guaranteed) */
    for (;;) __asm__ volatile ("hlt");
}
void exit(int code){ (void)code; dg_return_to_shell(); reboot_now(); for(;;){} }
void abort(void){ putstr("\naborted\n"); reboot_now(); for(;;){} }
void __dg_assert_fail(const char *e, const char *file, int line)
{ char b[128]; snprintf(b,sizeof(b),"assert failed: %s at %s:%d\n",e,file,line); putstr(b); reboot_now(); }

int gettimeofday(void *tv, void *tz){ (void)tv;(void)tz; return -1; }

/* ================================================================== */
/* sscanf                                                             */
/* ================================================================== */
static int isspc(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
int vsscanf(const char *str, const char *fmt, va_list ap)
{
    const char *s = str;
    int count = 0;
    while (*fmt) {
        if (isspc((unsigned char)*fmt)) { while (isspc((unsigned char)*s)) s++; fmt++; continue; }
        if (*fmt != '%') { if (*s != *fmt) break; s++; fmt++; continue; }
        fmt++;
        if (*fmt == '%') { if (*s != '%') break; s++; fmt++; continue; }
        int suppress = 0; if (*fmt == '*') { suppress = 1; fmt++; }
        int width = 0, haswidth = 0;
        while (*fmt >= '0' && *fmt <= '9') { haswidth = 1; width = width*10 + (*fmt++ - '0'); }
        int lng = 0; while (*fmt == 'l' || *fmt == 'h') { if (*fmt=='l') lng++; fmt++; }
        char c = *fmt++;
        if (c != 'c') while (isspc((unsigned char)*s)) s++;
        switch (c) {
            case 'd': case 'i': case 'u': case 'x': case 'o': {
                int base = (c=='x')?16:(c=='o')?8:(c=='i')?0:10;
                const char *start = s;
                int neg = 0; if (*s=='+'||*s=='-') { neg=(*s=='-'); s++; }
                if ((base==0||base==16) && s[0]=='0' && (s[1]=='x'||s[1]=='X')) { s+=2; base=16; }
                else if (base==0 && s[0]=='0') base=8; else if (base==0) base=10;
                long long val=0; int any=0, cnt=0;
                for (;;) {
                    if (haswidth && cnt>=width) break;
                    int d, ch=*s;
                    if (ch>='0'&&ch<='9') d=ch-'0';
                    else if (ch>='a'&&ch<='f') d=ch-'a'+10;
                    else if (ch>='A'&&ch<='F') d=ch-'A'+10;
                    else break;
                    if (d>=base) break;
                    val=val*base+d; s++; any=1; cnt++;
                }
                if (!any) { s=start; goto done; }
                if (neg) val=-val;
                if (!suppress) {
                    if (lng>=2) *va_arg(ap,long long*)=val;
                    else if (lng==1) *va_arg(ap,long*)=(long)val;
                    else if (c=='u'||c=='x'||c=='o') *va_arg(ap,unsigned*)=(unsigned)val;
                    else *va_arg(ap,int*)=(int)val;
                    count++;
                }
                break;
            }
            case 'f': case 'e': case 'g': {
                const char *start=s; int neg=0; if (*s=='+'||*s=='-'){ neg=(*s=='-'); s++; }
                double v=0; int any=0;
                while (*s>='0'&&*s<='9'){ v=v*10+(*s-'0'); s++; any=1; }
                if (*s=='.'){ s++; double f=0.1; while (*s>='0'&&*s<='9'){ v+=(*s-'0')*f; f*=0.1; s++; any=1; } }
                if (!any){ s=start; goto done; }
                if (neg) v=-v;
                if (!suppress){ if (lng>=1) *va_arg(ap,double*)=v; else *va_arg(ap,float*)=(float)v; count++; }
                break;
            }
            case 's': {
                char *out = suppress?NULL:va_arg(ap,char*); int cnt=0;
                if (!*s) goto done;
                while (*s && !isspc((unsigned char)*s)) { if (haswidth&&cnt>=width) break; if (out) *out++=*s; s++; cnt++; }
                if (out) *out=0; if (!suppress && cnt) count++;
                break;
            }
            case 'c': {
                int n = haswidth?width:1; char *out = suppress?NULL:va_arg(ap,char*);
                for (int i=0;i<n;i++){ if (!*s) goto done; if (out) *out++=*s; s++; }
                if (!suppress) count++;
                break;
            }
            default: goto done;
        }
    }
done:
    return count;
}
int sscanf(const char *str, const char *fmt, ...)
{ va_list ap; va_start(ap,fmt); int r=vsscanf(str,fmt,ap); va_end(ap); return r; }
