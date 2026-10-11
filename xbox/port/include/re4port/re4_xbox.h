/* re4_xbox.h — force-included (-include) into every game translation unit of the port.
 *
 * Purpose (see xbox/docs/07-COMPILER.md):
 *   1. announce RE4_PORT so the few guarded edits in src/ activate,
 *   2. provide the SDK types/macros the game headers expect without pulling in include/dolphin/,
 *   3. redirect the GameCube fixed memory map into the port's arena (GC_ADDR / VALID_PTR),
 *   4. rename symbols that clash on the new platform (main, OSReport ...).
 *
 * Keep it minimal: it is also seen by xbox_compat/ and the port layer itself. No standard headers.
 */
#ifndef RE4PORT_RE4_XBOX_H
#define RE4PORT_RE4_XBOX_H

#define RE4_PORT 1

#if defined(NXDK) || defined(_XBOX)
#define RE4_XBOX 1
#else
#define RE4_HOST 1
#endif

/* --- 1. Pointer width check: the game stores pointers in u32 and relocates file offsets in place. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ != 4
#error "RE4 port must be built 32-bit (nxdk is i386; use -m32 on the host)"
#endif

/* --- 2. GameCube memory map (main_mem.cpp, main_sub.cpp, snd.cpp, datactrl.cpp, dvd.cpp).
 * MEM1 is a 24 MB arena allocated at boot by the port; every literal 0x80xxxxxx the game uses for a
 * fixed region goes through GC_ADDR(). The arena base is a variable so the host can mmap anywhere. */
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char* re4_mem1_base;   /* 24 MB, 32-byte aligned, GPU-visible on Xbox */
extern unsigned char* re4_aram_base;   /* 16 MB "ARAM" */
extern void re4_log(const char* fmt, ...);
extern void re4_log_once(const char* key, const char* fmt, ...);
extern void re4_fatal(const char* file, int line, const char* msg);
#ifdef __cplusplus
}
#endif

#define RE4_MEM1_SIZE 0x01800000u   /* 24 MB */
#define RE4_ARAM_SIZE 0x01000000u   /* 16 MB */
#define GC_ADDR(a)    ((void*) (re4_mem1_base + ((unsigned) (a) - 0x80000000u)))
#define GC_OFS(p)     ((unsigned) ((unsigned char*) (p) - re4_mem1_base) + 0x80000000u)

/* include/main_mem.h defines VALID_PTR as the 0x80000000..0x82FFFFFF check; override after the fact. */
#define RE4_VALID_PTR(p) ((unsigned char*) (p) >= re4_mem1_base && (unsigned char*) (p) < re4_mem1_base + RE4_MEM1_SIZE)

/* --- 3. Symbol renames. The game's main() is called by the port's platform main. */
#define main re4_main

/* The SDK's paired-single matrix/vector routines (PS*) are PowerPC assembly; the game links the SDK's
 * own C versions (C_* in src/lib mtx.c, mtxvec.c, mtx44.c, vec.c, quat.c) under the PS names.
 * PSMTXReorder has no C version in the SDK: xbox/port/mtx/mtx_extra.c provides it. */
#define PSMTX44MultVec C_MTX44MultVec
#define PSMTXConcat C_MTXConcat
#define PSMTXCopy C_MTXCopy
#define PSMTXIdentity C_MTXIdentity
#define PSMTXInverse C_MTXInverse
#define PSMTXInvXpose C_MTXInvXpose
#define PSMTXMultVec C_MTXMultVec
#define PSMTXMultVecArray C_MTXMultVecArray
#define PSMTXMultVecSR C_MTXMultVecSR
#define PSMTXMultVecArraySR C_MTXMultVecArraySR
#define PSMTXQuat C_MTXQuat
#define PSMTXRotAxisRad C_MTXRotAxisRad
#define PSMTXRotRad C_MTXRotRad
#define PSMTXRotTrig C_MTXRotTrig
#define PSMTXScale C_MTXScale
#define PSMTXScaleApply C_MTXScaleApply
#define PSMTXTrans C_MTXTrans
#define PSMTXTransApply C_MTXTransApply
#define PSMTXTranspose C_MTXTranspose
#define PSMTX44Copy C_MTX44Copy
#define PSMTX44Concat C_MTX44Concat
#define PSMTX44Transpose C_MTX44Transpose
#define PSMTX44TransApply C_MTX44TransApply
#define PSMTX44ScaleApply C_MTX44ScaleApply
#define PSVECAdd C_VECAdd
#define PSVECCrossProduct C_VECCrossProduct
#define PSVECDistance C_VECDistance
#define PSVECDotProduct C_VECDotProduct
#define PSVECMag C_VECMag
#define PSVECNormalize C_VECNormalize
#define PSVECScale C_VECScale
#define PSVECSquareDistance C_VECSquareDistance
#define PSVECSquareMag C_VECSquareMag
#define PSVECSubtract C_VECSubtract

/* OSReport/OSPanic/HALT go to the port log (xbox/port/os/os_shim.cpp implements OSReport itself,
 * so no macro here; HALT is defined by the game in include/global.h and stays). */

/* --- 4. Compiler shims for GCC 2.95-era code under clang. */
/* nxdk's clang targets i386-pc-win32 (MSVC compatibility mode) and does not define __GNUC__;
 * include/dolphin/types.h and the game's GCC-only paths need it. Defined here (first header seen by
 * every unit) rather than with -fgnuc-version so nxdk's own libraries are unaffected. */
#if defined(__clang__) && !defined(__GNUC__)
#define __GNUC__ 4
#define __GNUC_MINOR__ 2
#endif
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunknown-pragmas"
#pragma clang diagnostic ignored "-Wwritable-strings"
#pragma clang diagnostic ignored "-Wregister"
#endif

/* The matching levers (`register T x asm("rN")`) are stripped by xbox/tools/prep.py before the
 * compiler sees the unit; `asm("" : : "r"(x))` statements are valid on x86 and stay. */

/* --- 5. GX immediate-mode FIFO (CT-08). include/gx.h declares the GameCube write-gather pipe
 * (`extern volatile WGPipe GXWGFifo[]`) and inline writers that store into it. Defining its guard here
 * skips that block (and the SDK's dolphin/gx/GXVert.h, should a unit include it) and re4port/gx_fifo.h
 * supplies a WGPipe-compatible proxy whose assignments append to the port's primitive buffer. C++ only:
 * every game unit is C++ and no C unit touches the FIFO. */
#ifdef __cplusplus
#define __GXVERT_H__ 1
#define _DOLPHIN_GX_GXVERT_H_ 1
#include "re4port/gx_fifo.h"
#endif

#endif /* RE4PORT_RE4_XBOX_H */
