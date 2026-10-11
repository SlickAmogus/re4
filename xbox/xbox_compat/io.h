#pragma once

#include <stddef.h>
#include <stdio.h>

/* MSVC-style low-level I/O for nxdk (underscore prefix = the real names) */
int    _open(const char *path, int flags, ...);
int    _read(int fd, void *buf, unsigned int count);
int    _write(int fd, const void *buf, unsigned int count);
int    _close(int fd);
long   _lseek(int fd, long offset, int whence);
int    _access(const char *path, int mode);
/* _getcwd is provided by nxdk's direct.h (with size_t parameter) — not redeclared here */
long   _filelength(int fd);
int    _fstat(int fd, struct stat *st);
FILE  *_fdopen(int fd, const char *mode);

/* compat.h under _MSC_VER maps open→_open, etc., so no need to re-alias here */

#ifndef F_OK
# define F_OK 0
# define R_OK 4
# define W_OK 2
#endif
