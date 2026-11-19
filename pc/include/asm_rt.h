/* Runtime for C translated from the game's Thumb asm by pc/tools/asm2c.py.
 *
 * ARM registers live in one global register file. Translated functions are `void asm_NAME(void)`
 * and use the ARM calling convention on that register file and an emulated stack (sp = r13).
 * Every asm function also gets a C-ABI wrapper under its real name (ASM_EXPORT), so C code,
 * function pointers and data tables can call it like any C function.
 */
#ifndef ASM_RT_H
#define ASM_RT_H

#include <stdint.h>

typedef uint8_t au8;
typedef int8_t as8;
typedef uint16_t au16;
typedef int16_t as16;
typedef uint32_t au32;
typedef int32_t as32;
typedef uint64_t au64;

typedef struct {
    au32 r[16];
} PcCpu;
extern PcCpu pc_cpu;

#define R(n) (pc_cpu.r[n])
#define SP   (pc_cpu.r[13])

#define M8(a)   (*(volatile au8 *)(uintptr_t)(a))
#define M16(a)  (*(volatile au16 *)(uintptr_t)(a))
#define M32(a)  (*(volatile au32 *)(uintptr_t)(a))
#define MS8(a)  (*(volatile as8 *)(uintptr_t)(a))
#define MS16(a) (*(volatile as16 *)(uintptr_t)(a))

/* Stores. Those into the IO page (0x04000000-0x04000FFF) go to the host so registers with side
 * effects work: geometry commands, VRAMCNT, divider/sqrt (pc/src/pc_g3.c pc_io_store). */
void pc_io_store(au32 addr, au32 value, int size);
#define PC_STORE(acc, size, a, v) do { \
        au32 _sa = (au32)(a); \
        if (__builtin_expect((_sa >> 12) == 0x4000u, 0)) pc_io_store(_sa, (au32)(v), size); \
        else acc(_sa) = (v); \
    } while (0)
#define ST8(a, v)  PC_STORE(M8, 1, a, (au8)(v))
#define ST16(a, v) PC_STORE(M16, 2, a, (au16)(v))
#define ST32(a, v) PC_STORE(M32, 4, a, (au32)(v))

#define ADDR(sym) ((au32)(uintptr_t)(sym))

/* Value placed in lr for every call. `bx rX` with this value is a return (pret code often pops
 * the saved lr into a low register and returns with bx). */
#define PC_LR_RETURN 0xFFFFFFF1u

/* Flag-setting operations. n, z, c, v are locals of each translated function, so the compiler
 * drops flag updates nobody reads. */
#define LOGIC(d, x) do { au32 _r = (x); n = _r >> 31; z = _r == 0; (d) = _r; } while (0)
#define TST(x) do { au32 _r = (x); n = _r >> 31; z = _r == 0; } while (0)
#define ADDS(d, a, b) do { au32 _a = (a), _b = (b), _r = _a + _b; \
    c = _r < _a; v = ((_a ^ _r) & (_b ^ _r)) >> 31; n = _r >> 31; z = _r == 0; (d) = _r; } while (0)
#define SUBS(d, a, b) do { au32 _a = (a), _b = (b), _r = _a - _b; \
    c = _a >= _b; v = ((_a ^ _b) & (_a ^ _r)) >> 31; n = _r >> 31; z = _r == 0; (d) = _r; } while (0)
#define ADCS(d, a, b) do { au32 _a = (a), _b = (b); au64 _t = (au64)_a + _b + c; au32 _r = (au32)_t; \
    c = (au32)(_t >> 32); v = ((_a ^ _r) & (_b ^ _r)) >> 31; n = _r >> 31; z = _r == 0; (d) = _r; } while (0)
#define SBCS(d, a, b) do { au32 _a = (a), _b = (b), _nc = !c, _r = _a - _b - _nc; \
    c = (au64)_a >= (au64)_b + _nc; v = ((_a ^ _b) & (_a ^ _r)) >> 31; n = _r >> 31; z = _r == 0; (d) = _r; } while (0)
#define CMP(a, b) do { au32 _cd; SUBS(_cd, a, b); } while (0)
#define CMN(a, b) do { au32 _cd; ADDS(_cd, a, b); } while (0)

/* Shifts by immediate (Thumb encodes lsr/asr #32 as #0) and by register (low byte, ARM rules). */
#define LSL_I(d, a, s) do { au32 _a = (a); if ((s) == 0) { LOGIC(d, _a); } \
    else { c = (_a >> (32 - (s))) & 1; LOGIC(d, _a << (s)); } } while (0)
#define LSR_I(d, a, s) do { au32 _a = (a); if ((s) == 0 || (s) == 32) { c = _a >> 31; LOGIC(d, 0u); } \
    else { c = (_a >> ((s) - 1)) & 1; LOGIC(d, _a >> (s)); } } while (0)
#define ASR_I(d, a, s) do { au32 _a = (a); if ((s) == 0 || (s) == 32) { c = _a >> 31; LOGIC(d, (au32)((as32)_a >> 31)); } \
    else { c = (_a >> ((s) - 1)) & 1; LOGIC(d, (au32)((as32)_a >> (s))); } } while (0)
#define LSL_R(d, a, b) do { au32 _a = (a), _s = (b) & 0xFF; if (_s == 0) { LOGIC(d, _a); } \
    else if (_s < 32) { c = (_a >> (32 - _s)) & 1; LOGIC(d, _a << _s); } \
    else { c = _s == 32 ? (_a & 1) : 0; LOGIC(d, 0u); } } while (0)
#define LSR_R(d, a, b) do { au32 _a = (a), _s = (b) & 0xFF; if (_s == 0) { LOGIC(d, _a); } \
    else if (_s < 32) { c = (_a >> (_s - 1)) & 1; LOGIC(d, _a >> _s); } \
    else { c = _s == 32 ? (_a >> 31) : 0; LOGIC(d, 0u); } } while (0)
#define ASR_R(d, a, b) do { au32 _a = (a), _s = (b) & 0xFF; if (_s == 0) { LOGIC(d, _a); } \
    else if (_s < 32) { c = (_a >> (_s - 1)) & 1; LOGIC(d, (au32)((as32)_a >> _s)); } \
    else { c = _a >> 31; LOGIC(d, (au32)((as32)_a >> 31)); } } while (0)
#define ROR_R(d, a, b) do { au32 _a = (a), _s = (b) & 0xFF; if (_s == 0) { LOGIC(d, _a); } \
    else { _s &= 31; au32 _o = _s ? (_a >> _s) | (_a << (32 - _s)) : _a; c = _o >> 31; LOGIC(d, _o); } } while (0)
/* (temporaries passed into LOGIC/TST must not be named _r: theirs would shadow it) */

/* ARM barrel-shifter values for operand 2 (no flags): register amounts use the low byte. */
static inline au32 pc_lslv(au32 a, au32 s) { s &= 0xFF; return s >= 32 ? 0 : a << s; }
static inline au32 pc_lsrv(au32 a, au32 s) { s &= 0xFF; return s >= 32 ? 0 : a >> s; }
static inline au32 pc_asrv(au32 a, au32 s) { s &= 0xFF; return (au32)((as32)a >> (s >= 32 ? 31 : s)); }
static inline au32 pc_rorv(au32 a, au32 s) { s &= 31; return s ? (a >> s) | (a << (32 - s)) : a; }

void pc_stub_hit(const char *name);

/* Opt-in call tracing (HG_TRACE=name,...): entry args and returned r0 of the named asm functions. */
extern int pc_trace_on;
void pc_trace(const char *name, int returning);
#define PC_TRACE_IN(name) do { if (__builtin_expect(pc_trace_on, 0)) pc_trace(name, 0); } while (0)
#define PC_TRACE_OUT(name) do { if (__builtin_expect(pc_trace_on, 0)) pc_trace(name, 1); } while (0)
void pc_asm_unsupported(const char *name, const char *why);

/* Call any C-ABI function (C code, or an asm function's wrapper) with the ARM convention:
 * r0-r3, then stack words. Passing more words than the callee takes is harmless in i386 cdecl.
 * A 64-bit result comes back in r0:r1. */
typedef au64 (*PcCFn)(au32, au32, au32, au32, au32, au32, au32, au32, au32, au32, au32, au32, au32, au32, au32, au32);
static inline void pc_callc(const void *fn) {
    au32 sp = SP; /* 16 words: the most any game/SDK function takes (CreateSpriteResourcesHeader has 15) */
    au64 res = ((PcCFn)(uintptr_t)fn)(R(0), R(1), R(2), R(3), M32(sp), M32(sp + 4), M32(sp + 8), M32(sp + 12),
                                       M32(sp + 16), M32(sp + 20), M32(sp + 24), M32(sp + 28),
                                       M32(sp + 32), M32(sp + 36), M32(sp + 40), M32(sp + 44));
    R(0) = (au32)res;
    R(1) = (au32)(res >> 32);
}

/* Call a C function that returns a SIZE-byte struct by value. Typing the call with a 32-byte struct
 * return makes gcc pass a hidden buffer and expect the callee's pop of it, as i386 does for any size.
 * ARM: SIZE <= 4 comes back in r0; larger goes to the buffer r0 points at, with real args from r1. */
typedef struct { au32 w[8]; } PcStructRet;
typedef PcStructRet (*PcCFnS)(au32, au32, au32, au32, au32, au32, au32, au32, au32, au32, au32, au32, au32, au32, au32, au32);
static inline void pc_callc_struct(const void *fn, unsigned size) {
    au32 sp = SP;
    PcStructRet res;
    if (size <= 4) {
        res = ((PcCFnS)(uintptr_t)fn)(R(0), R(1), R(2), R(3), M32(sp), M32(sp + 4), M32(sp + 8), M32(sp + 12),
                                       M32(sp + 16), M32(sp + 20), M32(sp + 24), M32(sp + 28),
                                       M32(sp + 32), M32(sp + 36), M32(sp + 40), M32(sp + 44));
        R(0) = res.w[0];
    } else {
        res = ((PcCFnS)(uintptr_t)fn)(R(1), R(2), R(3), M32(sp), M32(sp + 4), M32(sp + 8), M32(sp + 12),
                                       M32(sp + 16), M32(sp + 20), M32(sp + 24), M32(sp + 28),
                                       M32(sp + 32), M32(sp + 36), M32(sp + 40), M32(sp + 44), 0);
        __builtin_memcpy((void *)(uintptr_t)R(0), &res, size);
    }
}

/* C-ABI entry for asm function NAME: args 1-4 into r0-r3, args 5-16 onto the emulated stack.
 * FIX runs on the incoming args first: it narrows sub-word params (asm2c NARROW_PARAMS), since mwcc
 * callers extend them to 32 bits and the asm relies on it, but i386 callers needn't. */
#define ASM_EXPORT(name) ASM_EXPORT_FIX(name, )
#define ASM_EXPORT_FIX(name, fix)                                                                     \
    void asm_##name(void);                                                                            \
    au64 name(au32 a0, au32 a1, au32 a2, au32 a3, au32 s0, au32 s1, au32 s2, au32 s3, au32 s4, au32 s5, \
               au32 s6, au32 s7, au32 s8, au32 s9, au32 s10, au32 s11) {                              \
        fix                                                                                           \
        au32 sp = SP;                                                                                 \
        SP = sp - 48;                                                                                 \
        M32(SP) = s0; M32(SP + 4) = s1; M32(SP + 8) = s2; M32(SP + 12) = s3;                          \
        M32(SP + 16) = s4; M32(SP + 20) = s5; M32(SP + 24) = s6; M32(SP + 28) = s7;                   \
        M32(SP + 32) = s8; M32(SP + 36) = s9; M32(SP + 40) = s10; M32(SP + 44) = s11;                 \
        R(0) = a0; R(1) = a1; R(2) = a2; R(3) = a3;                                                   \
        asm_##name();                                                                                 \
        PC_TRACE_OUT(#name);                                                                          \
        SP = sp;                                                                                      \
        return R(0) | ((au64)R(1) << 32);                                                             \
    }

#endif
