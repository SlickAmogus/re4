/* POSIX/MSVC file I/O compatibility layer for nxdk
 * Implements _open/_read/_write/_close/_lseek/_stat/_fstat/_access/_fdopen
 * on top of the nxdk Xbox kernel (NtCreateFile etc.).
 *
 * KEY DESIGN: nxdk's CreateFileA / GetFileAttributesA pass paths directly to
 * NT with ObDosDevicesDirectory() as root.  That means ONLY "X:\..." paths
 * with a recognised drive letter work — relative paths (".", "./foo", "foo")
 * fail because "." is not a DOS device.
 *
 * We call XConvertDOSFilenameToXBOX() first, which converts any relative or
 * drive-letter path into an absolute NT path ("\Device\Harddisk0\...") and
 * then pass that absolute path to NtCreateFile / NtQueryFullAttributesFile
 * with RootDirectory = NULL (absolute NT paths require a NULL root).
 */

#include <hal/fileio.h>          /* XConvertDOSFilenameToXBOX */
#include <xboxkrnl/xboxkrnl.h>   /* NtCreateFile, NtQueryFullAttributesFile ... */
#include <windows.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include "fcntl.h"
#include "sys/stat.h"

#define MAX_FDS       256
#define MAX_PATH_LEN  512

typedef struct {
    HANDLE handle;
    char   path[MAX_PATH_LEN];  /* original path (for _fdopen) */
} fd_entry_t;

static fd_entry_t fd_table[MAX_FDS];
static int        fd_init = 0;

static void init_fds(void)
{
    if (fd_init) return;
    for (int i = 0; i < MAX_FDS; i++) {
        fd_table[i].handle = INVALID_HANDLE_VALUE;
        fd_table[i].path[0] = '\0';
    }
    fd_init = 1;
}

static int alloc_fd(HANDLE h, const char *path)
{
    for (int i = 3; i < MAX_FDS; i++) {
        if (fd_table[i].handle == INVALID_HANDLE_VALUE) {
            fd_table[i].handle = h;
            if (path) {
                strncpy(fd_table[i].path, path, MAX_PATH_LEN - 1);
                fd_table[i].path[MAX_PATH_LEN - 1] = '\0';
            } else {
                fd_table[i].path[0] = '\0';
            }
            return i;
        }
    }
    return -1;
}

static void map_ntstatus(NTSTATUS status)
{
    switch (status) {
        case STATUS_OBJECT_NAME_NOT_FOUND:
        case STATUS_OBJECT_PATH_NOT_FOUND:  errno = ENOENT;  break;
        case STATUS_ACCESS_DENIED:          errno = EACCES;  break;
        case STATUS_OBJECT_NAME_COLLISION:  errno = EEXIST;  break;
        /* STATUS_TOO_MANY_OPEN_FILES not always defined in nxdk headers */
        default:                            errno = EIO;     break;
    }
}

/* Convert a DOS/relative path to an absolute Xbox NT path.
 * Returns 0 on success, -1 on failure. */
static int xbox_resolve(const char *src, char *dst)
{
    if (XConvertDOSFilenameToXBOX(src, dst) == STATUS_SUCCESS)
        return 0;
    errno = ENOENT;
    return -1;
}

/* Open an NT file given an already-converted absolute Xbox NT path.
 * Uses NULL RootDirectory so the path is treated as absolute. */
static HANDLE nt_open_file(const char *xboxPath, DWORD access, ULONG createDisp, ULONG createFlags)
{
    NTSTATUS status;
    HANDLE handle;
    ANSI_STRING ntPath;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;

    RtlInitAnsiString(&ntPath, xboxPath);
    InitializeObjectAttributes(&oa, &ntPath, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtCreateFile(&handle,
                          access | SYNCHRONIZE,
                          &oa, &iosb,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          createDisp,
                          createFlags | FILE_SYNCHRONOUS_IO_NONALERT);

    if (!NT_SUCCESS(status)) {
        map_ntstatus(status);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

int _open(const char *path, int flags, ...)
{
    init_fds();

    char xboxPath[MAX_PATH_LEN];
    if (xbox_resolve(path, xboxPath) < 0) return -1;

    DWORD access;
    ULONG createDisp, createFlags = FILE_NON_DIRECTORY_FILE;

    int rwflags = flags & (_O_RDONLY | _O_WRONLY | _O_RDWR);
    if (rwflags == _O_WRONLY)      access = GENERIC_WRITE;
    else if (rwflags == _O_RDWR)   access = GENERIC_READ | GENERIC_WRITE;
    else                           access = GENERIC_READ;

    if (flags & _O_CREAT) {
        if (flags & _O_EXCL)         createDisp = FILE_CREATE;
        else if (flags & _O_TRUNC)   createDisp = FILE_OVERWRITE_IF;
        else                         createDisp = FILE_OPEN_IF;
    } else {
        if (flags & _O_TRUNC)        createDisp = FILE_OVERWRITE;
        else                         createDisp = FILE_OPEN;
    }

    HANDLE h = nt_open_file(xboxPath, access, createDisp, createFlags);
    if (h == INVALID_HANDLE_VALUE) return -1;

    if (flags & _O_APPEND)
        SetFilePointer(h, 0, NULL, FILE_END);

    int fd = alloc_fd(h, path);
    if (fd < 0) { NtClose(h); errno = EMFILE; return -1; }
    return fd;
}

int _read(int fd, void *buf, unsigned int count)
{
    init_fds();
    if (fd < 0 || fd >= MAX_FDS || fd_table[fd].handle == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    DWORD nr = 0;
    if (!ReadFile(fd_table[fd].handle, buf, count, &nr, NULL)) { errno = EIO; return -1; }
    return (int)nr;
}

int _write(int fd, const void *buf, unsigned int count)
{
    init_fds();
    if (fd < 0 || fd >= MAX_FDS || fd_table[fd].handle == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    DWORD nw = 0;
    if (!WriteFile(fd_table[fd].handle, buf, count, &nw, NULL)) { errno = EIO; return -1; }
    return (int)nw;
}

int _close(int fd)
{
    init_fds();
    if (fd < 0 || fd >= MAX_FDS || fd_table[fd].handle == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    NtClose(fd_table[fd].handle);
    fd_table[fd].handle = INVALID_HANDLE_VALUE;
    fd_table[fd].path[0] = '\0';
    return 0;
}

long _lseek(int fd, long offset, int whence)
{
    init_fds();
    if (fd < 0 || fd >= MAX_FDS || fd_table[fd].handle == INVALID_HANDLE_VALUE) { errno = EBADF; return -1L; }
    DWORD method = (whence == 1) ? FILE_CURRENT : (whence == 2) ? FILE_END : FILE_BEGIN;
    DWORD result = SetFilePointer(fd_table[fd].handle, offset, NULL, method);
    if (result == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) { errno = EIO; return -1L; }
    return (long)result;
}

/* Pseudo-fd range used by _fileno() in xbox_stubs.c */
#define FILENO_MAGIC 0x7F000000
extern long _filelength_from_file(int pseudo_fd);

long _filelength(int fd)
{
    if (fd >= FILENO_MAGIC) return _filelength_from_file(fd);

    init_fds();
    if (fd < 0 || fd >= MAX_FDS || fd_table[fd].handle == INVALID_HANDLE_VALUE) { errno = EBADF; return -1L; }
    DWORD size = GetFileSize(fd_table[fd].handle, NULL);
    if (size == INVALID_FILE_SIZE) { errno = EIO; return -1L; }
    return (long)size;
}

int _fstat(int fd, struct stat *st)
{
    init_fds();
    if (fd < 0 || fd >= MAX_FDS || fd_table[fd].handle == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(fd_table[fd].handle, &info)) { errno = EIO; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_size  = (long)info.nFileSizeLow;
    st->st_mode  = _S_IFREG | _S_IREAD | _S_IWRITE;
    return 0;
}

/* Query file attributes using absolute Xbox NT path + NULL root directory. */
static int nt_query_attrs(const char *xboxPath, FILE_NETWORK_OPEN_INFORMATION *out)
{
    ANSI_STRING ntPath;
    OBJECT_ATTRIBUTES oa;
    RtlInitAnsiString(&ntPath, xboxPath);
    InitializeObjectAttributes(&oa, &ntPath, OBJ_CASE_INSENSITIVE, NULL, NULL);
    NTSTATUS status = NtQueryFullAttributesFile(&oa, out);
    if (!NT_SUCCESS(status)) { map_ntstatus(status); return -1; }
    return 0;
}

int _stat(const char *path, struct stat *st)
{
    char xboxPath[MAX_PATH_LEN];
    if (xbox_resolve(path, xboxPath) < 0) return -1;

    FILE_NETWORK_OPEN_INFORMATION info;
    if (nt_query_attrs(xboxPath, &info) < 0) return -1;

    memset(st, 0, sizeof(*st));
    st->st_size = (long)info.EndOfFile.LowPart;
    if (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        st->st_mode = _S_IFDIR | _S_IREAD | _S_IWRITE;
    else
        st->st_mode = _S_IFREG | _S_IREAD | _S_IWRITE;
    return 0;
}

int _access(const char *path, int mode)
{
    (void)mode;
    char xboxPath[MAX_PATH_LEN];
    if (xbox_resolve(path, xboxPath) < 0) return -1;

    FILE_NETWORK_OPEN_INFORMATION info;
    return nt_query_attrs(xboxPath, &info);
}

/* _getcwd is provided by nxdk's xboxrt/libc_extensions/direct.c — don't redefine */

/* _fdopen: re-open the file as FILE* at the current position.
 * We stored the original path when _open() was called. */
FILE *_fdopen(int fd, const char *mode)
{
    init_fds();
    if (fd < 0 || fd >= MAX_FDS || fd_table[fd].handle == INVALID_HANDLE_VALUE) { errno = EBADF; return NULL; }
    if (!fd_table[fd].path[0]) { errno = EBADF; return NULL; }

    long pos = _lseek(fd, 0, 1 /* SEEK_CUR */);

    FILE *f = fopen(fd_table[fd].path, mode);
    if (!f) return NULL;

    if (pos > 0) fseek(f, pos, 0 /* SEEK_SET */);
    return f;
}
