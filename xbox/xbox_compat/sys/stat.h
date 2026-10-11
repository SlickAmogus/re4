#pragma once

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "sys/types.h"

#define _S_IFMT   0xF000
#define _S_IFDIR  0x4000
#define _S_IFREG  0x8000
#define _S_IREAD  0x0100
#define _S_IWRITE 0x0080
#define _S_IEXEC  0x0040

#define S_IFMT    _S_IFMT
#define S_IFDIR   _S_IFDIR
#define S_IFREG   _S_IFREG
#define S_IREAD   _S_IREAD
#define S_IWRITE  _S_IWRITE
#define S_IEXEC   _S_IEXEC

#define S_IRUSR   _S_IREAD
#define S_IWUSR   _S_IWRITE
#define S_IXUSR   _S_IEXEC
#define S_IRWXU   (_S_IREAD|_S_IWRITE|_S_IEXEC)

/* Groups/others are not meaningful on Xbox, map to user perms */
#define S_IRGRP   _S_IREAD
#define S_IWGRP   _S_IWRITE
#define S_IXGRP   _S_IEXEC
#define S_IRWXG   (_S_IREAD|_S_IWRITE|_S_IEXEC)

#define S_IROTH   _S_IREAD
#define S_IWOTH   _S_IWRITE
#define S_IXOTH   _S_IEXEC
#define S_IRWXO   (_S_IREAD|_S_IWRITE|_S_IEXEC)

#define S_IFIFO   0x1000
#define S_IFCHR   0x2000
#define S_IFBLK   0x6000

#define S_ISDIR(m)  (((m) & _S_IFMT) == _S_IFDIR)
#define S_ISREG(m)  (((m) & _S_IFMT) == _S_IFREG)

/* Define as _stat so both 'struct stat' and 'struct _stat' work.
 * compat.h maps stat→_stat under _MSC_VER, so code uses 'struct _stat'. */
struct _stat {
    unsigned int   st_dev;
    unsigned int   st_ino;
    unsigned short st_mode;
    short          st_nlink;
    short          st_uid;
    short          st_gid;
    unsigned int   st_rdev;
    long           st_size;
    long           st_atime;
    long           st_mtime;
    long           st_ctime;
};
/* Alias so both names work */
typedef struct _stat stat_t;
#define stat _stat

int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);

/* MSVC underscore-prefixed stat functions (compat.h maps stat→_stat etc.) */
int _stat(const char *path, struct stat *st);
int _fstat(int fd, struct stat *st);
