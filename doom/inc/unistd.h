#ifndef _DG_UNISTD_H
#define _DG_UNISTD_H
#include <stddef.h>
int access(const char *path, int mode);
int unlink(const char *path);
int usleep(unsigned usec);
unsigned sleep(unsigned sec);
int chdir(const char *path);
char *getcwd(char *buf, size_t sz);
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
#endif
