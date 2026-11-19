/* Force-included first in every PC-port translation unit (gcc -m32). */
#ifndef PC_COMPAT_H
#define PC_COMPAT_H

#define SDK_PC 1

#if defined(__MINGW32__) && defined(PC_DS_SOURCE)
/* Game and SDK sources have Windows-only paths (GameSpy's nonport.h includes windows.h). Read the C
 * library headers with _WIN32 set, as mingw-w64 needs, then hide it from the DS code. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <ctype.h>
#include <assert.h>
#include <time.h>
#include <malloc.h>
#undef _WIN32
#undef __WIN32
#undef __WIN32__
#undef WIN32
#endif

/* math.h: use the compiler builtin instead of the ARM `clz` inline asm. */
#define PLATFORM_INTRINSIC_FUNCTION_BIT_CLZ32(x) ((x) ? (u32)__builtin_clz(x) : 32u)

/* mwcc __declspec(...) -> gcc attributes. */
#define __declspec(x) __pc_declspec_##x
#ifdef __MINGW32__
#define __pc_declspec_weak /* PE can't resolve unused weak definitions; pc_build.py weakens the overridden ones */
#else
#define __pc_declspec_weak __attribute__((weak))
#endif
#define __pc_declspec_force_export __attribute__((used))
#ifdef __MINGW32__ /* the ones the mingw-w64 headers use */
#define __pc_declspec_dllimport __attribute__((dllimport))
#define __pc_declspec_dllexport __attribute__((dllexport))
#define __pc_declspec_selectany __attribute__((selectany))
#define __pc_declspec_noreturn __attribute__((noreturn))
#define __pc_declspec_deprecated __attribute__((deprecated))
#define __pc_declspec_nothrow __attribute__((nothrow))
#define __pc_declspec_noinline __attribute__((noinline))
#define __pc_declspec_restrict
#define __pc_declspec_noalias
#define __pc_declspec_thread __thread
#define __pc_declspec_align(n) __attribute__((aligned(n)))
#endif

/* SDK libraries/gx/include/gxasm.h declares GX_SendFifo36B extern, then defines it static inline,
 * which gcc rejects. Skip that header; these are provided by pc/src/pc_fx.c. */
#define NITRO_GXASM_H_
void GX_SendFifo36B(const void *pSrc, void *pDest);
void GX_SendFifo48B(const void *pSrc, void *pDest);
void GX_SendFifo64B(const void *pSrc, void *pDest);
void GX_SendFifo128B(const void *pSrc, void *pDest);

#endif
