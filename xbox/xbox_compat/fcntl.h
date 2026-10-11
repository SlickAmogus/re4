#pragma once

/* MSVC-style (underscore) constants — these are the "real" ones under _MSC_VER */
#define _O_RDONLY  0
#define _O_WRONLY  1
#define _O_RDWR    2
#define _O_APPEND  8
#define _O_CREAT   0x100
#define _O_TRUNC   0x200
#define _O_EXCL    0x400
#define _O_BINARY  0
#define _O_TEXT    0

/* POSIX aliases (compat.h redefines these as _O_* under _MSC_VER, which is fine) */
#define O_RDONLY  _O_RDONLY
#define O_WRONLY  _O_WRONLY
#define O_RDWR    _O_RDWR
#define O_APPEND  _O_APPEND
#define O_CREAT   _O_CREAT
#define O_TRUNC   _O_TRUNC
#define O_EXCL    _O_EXCL
#define O_BINARY  _O_BINARY
#define O_TEXT    _O_TEXT

#ifndef SEEK_SET
# define SEEK_SET 0
# define SEEK_CUR 1
# define SEEK_END 2
#endif

int _open(const char *path, int flags, ...);
