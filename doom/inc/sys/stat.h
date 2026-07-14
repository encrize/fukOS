#ifndef _DG_SYS_STAT_H
#define _DG_SYS_STAT_H
#include <sys/types.h>
struct stat {
    unsigned st_mode;
    long     st_size;
    long     st_mtime;
};
#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_ISDIR(m) (((m)&S_IFMT)==S_IFDIR)
#define S_ISREG(m) (((m)&S_IFMT)==S_IFREG)
int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, mode_t mode);
#endif
