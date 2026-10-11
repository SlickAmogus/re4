/* Force-included prefix header for nxdk Xbox port of jfduke3d.
 * Included via -include flag so it comes first in every translation unit. */

#pragma once

/* Pre-block PC Windows SDK headers that conflict with nxdk equivalents.
 * Must come BEFORE #include <windows.h> because windows.h transitively
 * includes these (PC SDK windows.h → sysinfoapi.h, etc.).
 * nxdk uses different guards (__SYSINFOAPI_H__ etc.) so its versions
 * are unaffected. */
#define _SYSINFOAPI_H_       /* block PC SDK sysinfoapi.h  */
#define _APISETMEMORY_       /* block PC SDK memoryapi.h   */
/* rpcndr.h defines 'boolean' as unsigned char which conflicts with
 * jfmact/types.h which typedefs boolean as int32. Block rpcndr.h;
 * nxdk has no rpcndr.h so this has no side effects. */
#define __RPCNDR_H__         /* block PC SDK rpcndr.h      */

/* Pull in base Windows types (nxdk's minimal winapi headers) */
#include <windows.h>

/* DWORDLONG: unsigned 64-bit — not in nxdk's windef.h */
#ifndef DWORDLONG
typedef unsigned long long DWORDLONG;
#endif

/* TCHAR: nxdk tchar.h only defines _TCHAR, not TCHAR */
#include <tchar.h>
#ifndef TCHAR
# define TCHAR _TCHAR
#endif
#ifndef MAX_PATH
# define MAX_PATH 260
#endif

/* Declare SHGetSpecialFolderPathA directly to avoid pulling in <shlobj.h>
 * (the PC Windows SDK version pulls in COM/OLE headers that conflict). */
#ifndef CSIDL_APPDATA
# define CSIDL_APPDATA 0x001A
#endif
#ifndef CSIDL_PERSONAL
# define CSIDL_PERSONAL 0x0005
#endif
BOOL SHGetSpecialFolderPathA(HWND hwnd, LPSTR pszPath, int csidl, BOOL fCreate);

/* SHGFP_TYPE_CURRENT: defined as an enum in PC SDK shlobj_core.h — use a
 * value-based macro only if not already defined as an enum. */
#ifndef SHGFP_TYPE_CURRENT
# define SHGFP_TYPE_CURRENT 0
#endif

/* Map SHGetFolderPathA → SHGetSpecialFolderPathA */
#ifndef SHGetFolderPathA
# define SHGetFolderPathA(hwnd, csidl, token, flags, path) \
         (SHGetSpecialFolderPathA(hwnd, path, csidl, FALSE) ? S_OK : E_FAIL)
#endif

/* MEMORYSTATUSEX / GlobalMemoryStatusEx stubs (Xbox has 64MB RAM).
 * The PC Windows SDK's sysinfoapi.h defines LPMEMORYSTATUSEX alongside the
 * struct; nxdk does not. Use that to detect whether it's already defined. */
#ifndef LPMEMORYSTATUSEX
typedef struct _MEMORYSTATUSEX {
    DWORD     dwLength;
    DWORD     dwMemoryLoad;
    DWORDLONG ullTotalPhys;
    DWORDLONG ullAvailPhys;
    DWORDLONG ullTotalPageFile;
    DWORDLONG ullAvailPageFile;
    DWORDLONG ullTotalVirtual;
    DWORDLONG ullAvailVirtual;
    DWORDLONG ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;

static inline BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX *ms)
{
    if (!ms) return FALSE;
    ms->ullTotalPhys    = 64ULL * 1024 * 1024;
    ms->ullAvailPhys    = 32ULL * 1024 * 1024;
    ms->ullTotalVirtual = 64ULL * 1024 * 1024;
    ms->ullAvailVirtual = 32ULL * 1024 * 1024;
    return TRUE;
}
#endif

/* GetModuleFileName stub — Xbox uses a fixed path */
#ifndef GetModuleFileName
# define GetModuleFileNameA(hModule, lpFilename, nSize) \
         ((void)(hModule), strncpy((lpFilename), "E:\\default.xbe", (nSize)), 16u)
# define GetModuleFileName GetModuleFileNameA
#endif

/* xbox_log(fmt, ...) — writes to on-screen debugPrint overlay AND E:\debug.log.
 * Use this instead of bare debugPrint in all Xbox-specific debug code so that
 * messages survive a crash when testing on real hardware. */
#ifdef _XBOX
#ifdef __cplusplus
extern "C" {
#endif
void xbox_log(const char *fmt, ...);
void xbox_log_open_file(void);
extern int xbox_logging_enabled;
#ifdef __cplusplus
}
#endif
#endif

/* INT64_C if not defined by stdint.h */
#ifndef INT64_C
# define INT64_C(x)  ((long long)(x))
# define UINT64_C(x) ((unsigned long long)(x))
#endif
