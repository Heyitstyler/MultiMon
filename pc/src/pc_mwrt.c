/* Host versions of the Metrowerks ARM runtime helpers that the game's asm calls.
 *
 * Translated asm calls these through pc_callc: arguments arrive as the ARM registers r0-r3
 * (one u32 each) and the u64 result goes back as r0 (low) : r1 (high). 64-bit integers and
 * doubles use r0 = low word, r1 = high word. Floats are raw IEEE bit patterns.
 *
 * Compare helpers (_fls, _dgr, ...) return the sign of a - b (-1, 0, 1); asm2c turns that into
 * the flags the DS helpers set (as for an unsigned `cmp a, b`).
 */
#include <stdint.h>
#include <string.h>

typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

static u64 pair(u32 lo, u32 hi) { return (u64)hi << 32 | lo; }
static float f32(u32 bits) { float f; memcpy(&f, &bits, 4); return f; }
static u32 bits32(float f) { u32 b; memcpy(&b, &f, 4); return b; }
static double f64(u32 lo, u32 hi) { u64 b = pair(lo, hi); double d; memcpy(&d, &b, 8); return d; }
static u64 bits64(double d) { u64 b; memcpy(&b, &d, 8); return b; }
static u64 cmp_result(int sign) { return (u32)(s32)sign; }

/* ---- integer ---- */
u64 _ll_mul(u32 alo, u32 ahi, u32 blo, u32 bhi) { return pair(alo, ahi) * pair(blo, bhi); }
u64 _ll_udiv(u32 alo, u32 ahi, u32 blo, u32 bhi) { u64 b = pair(blo, bhi); return b ? pair(alo, ahi) / b : 0; }
u64 _ll_sdiv(u32 alo, u32 ahi, u32 blo, u32 bhi) { s64 b = (s64)pair(blo, bhi); return b ? (u64)((s64)pair(alo, ahi) / b) : 0; }
u64 _ull_mod(u32 alo, u32 ahi, u32 blo, u32 bhi) { u64 b = pair(blo, bhi); return b ? pair(alo, ahi) % b : pair(alo, ahi); }

/* 32-bit division: quotient in r0, remainder in r1. Division by zero gives 0 remainder a. */
u64 _s32_div_f(u32 a, u32 b) {
    if (b == 0) return pair(0, a);
    if ((s32)a == INT32_MIN && (s32)b == -1) return pair(a, 0);
    return pair((u32)((s32)a / (s32)b), (u32)((s32)a % (s32)b));
}
u64 _u32_div_f(u32 a, u32 b) { return b ? pair(a / b, a % b) : pair(0, a); }

/* ---- float -> int conversions saturate like the ARM helpers; C casts would be undefined ---- */
static s32 sat_s32(double d) { return d != d ? 0 : d >= 2147483647.0 ? INT32_MAX : d <= -2147483648.0 ? INT32_MIN : (s32)d; }
static u32 sat_u32(double d) { return d != d || d <= 0 ? 0 : d >= 4294967295.0 ? UINT32_MAX : (u32)d; }

/* ---- float ---- */
u64 _fadd(u32 a, u32 b) { return bits32(f32(a) + f32(b)); }
u64 _fsub(u32 a, u32 b) { return bits32(f32(a) - f32(b)); }
u64 _fmul(u32 a, u32 b) { return bits32(f32(a) * f32(b)); }
u64 _fdiv(u32 a, u32 b) { return bits32(f32(a) / f32(b)); }
u64 _fflt(u32 a) { return bits32((float)(s32)a); }
u64 _ffltu(u32 a) { return bits32((float)a); }
u64 _ffix(u32 a) { return (u32)sat_s32(f32(a)); }
u64 _ffixu(u32 a) { return sat_u32(f32(a)); }
u64 _f_lltof(u32 lo, u32 hi) { return bits32((float)(s64)pair(lo, hi)); }
u64 _f_ulltof(u32 lo, u32 hi) { return bits32((float)pair(lo, hi)); }
static int fsign(u32 a, u32 b) { float x = f32(a), y = f32(b); return x < y ? -1 : x > y ? 1 : 0; }
u64 _fls(u32 a, u32 b) { return cmp_result(fsign(a, b)); }
u64 _fleq(u32 a, u32 b) { return cmp_result(fsign(a, b)); }
u64 _fgr(u32 a, u32 b) { return cmp_result(fsign(a, b)); }
u64 _fgeq(u32 a, u32 b) { return cmp_result(fsign(a, b)); }
u64 _feq(u32 a, u32 b) { return cmp_result(fsign(a, b)); }
u64 _fneq(u32 a, u32 b) { return cmp_result(fsign(a, b)); }

/* ---- double ---- */
u64 _dadd(u32 alo, u32 ahi, u32 blo, u32 bhi) { return bits64(f64(alo, ahi) + f64(blo, bhi)); }
u64 _dsub(u32 alo, u32 ahi, u32 blo, u32 bhi) { return bits64(f64(alo, ahi) - f64(blo, bhi)); }
u64 _dmul(u32 alo, u32 ahi, u32 blo, u32 bhi) { return bits64(f64(alo, ahi) * f64(blo, bhi)); }
u64 _ddiv(u32 alo, u32 ahi, u32 blo, u32 bhi) { return bits64(f64(alo, ahi) / f64(blo, bhi)); }
u64 _dflt(u32 a) { return bits64((double)(s32)a); }
u64 _dfltu(u32 a) { return bits64((double)a); }
u64 _dfix(u32 lo, u32 hi) { return (u32)sat_s32(f64(lo, hi)); }
u64 _dfixu(u32 lo, u32 hi) { return sat_u32(f64(lo, hi)); }
u64 _f2d(u32 a) { return bits64((double)f32(a)); }
u64 _d2f(u32 lo, u32 hi) { return bits32((float)f64(lo, hi)); }
static int dsign(u32 alo, u32 ahi, u32 blo, u32 bhi) {
    double x = f64(alo, ahi), y = f64(blo, bhi);
    return x < y ? -1 : x > y ? 1 : 0;
}
u64 _dls(u32 alo, u32 ahi, u32 blo, u32 bhi) { return cmp_result(dsign(alo, ahi, blo, bhi)); }
u64 _dleq(u32 alo, u32 ahi, u32 blo, u32 bhi) { return cmp_result(dsign(alo, ahi, blo, bhi)); }
u64 _dgr(u32 alo, u32 ahi, u32 blo, u32 bhi) { return cmp_result(dsign(alo, ahi, blo, bhi)); }
u64 _dgeq(u32 alo, u32 ahi, u32 blo, u32 bhi) { return cmp_result(dsign(alo, ahi, blo, bhi)); }
u64 _deq(u32 alo, u32 ahi, u32 blo, u32 bhi) { return cmp_result(dsign(alo, ahi, blo, bhi)); }
u64 _dneq(u32 alo, u32 ahi, u32 blo, u32 bhi) { return cmp_result(dsign(alo, ahi, blo, bhi)); }

/* Known-answer check, run once at startup. */
int pc_mwrt_selftest(void) {
    u64 q = _s32_div_f((u32)-7, 2);
    float three = 3.0f;
    u32 b3;
    memcpy(&b3, &three, 4);
    return (s32)(u32)q == -3 && (s32)(u32)(q >> 32) == -1
        && _ll_mul(0xFFFFFFFF, 0, 2, 0) == 0x1FFFFFFFEull
        && (u32)_fadd(b3, b3) == 0x40C00000u                  /* 6.0f */
        && (u32)_ffix(0x40C00000u) == 6
        && (s32)(u32)_fls(b3, 0x40C00000u) == -1
        && _dflt(1) == 0x3FF0000000000000ull                  /* 1.0 */
        && (u32)_dfix(0x9999999Au, 0x40C19999u) == 9011;      /* 9011.2 -> 9011 */
}
