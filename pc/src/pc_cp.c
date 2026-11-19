/* DS math coprocessor (divider, square root) computed on the host.
 * The SDK setters are wrapped (pc/include/nitro/cp/) to call these after writing the operands,
 * so the result registers hold the answer before anything reads them. Results follow GBATEK
 * "DS Maths": truncating division, remainder takes the numerator's sign, divide-by-zero gives
 * quotient -1/+1 and remainder = numerator, square root is floor(sqrt). */
#include <nitro.h>
#include "pc_platform.h"

#define DIVCNT        (*(vu16 *)0x04000280)
#define DIV_NUMER     (*(vs64 *)0x04000290)
#define DIV_DENOM     (*(vs64 *)0x04000298)
#define DIV_RESULT    (*(vs64 *)0x040002A0)
#define DIVREM_RESULT (*(vs64 *)0x040002A8)
#define SQRTCNT       (*(vu16 *)0x040002B0)
#define SQRT_RESULT   (*(vu32 *)0x040002B4)
#define SQRT_PARAM    (*(vu64 *)0x040002B8)

void pc_cp_div_update(void) {
    u32 mode = (DIVCNT >> 0) & 3;
    s64 n = mode == 0 ? (s64)(s32)DIV_NUMER : DIV_NUMER;
    s64 d = mode == 2 || mode == 3 ? DIV_DENOM : (s64)(s32)DIV_DENOM;
    s64 q, r;
    DIVCNT = (u16)((DIVCNT & ~0xC000) | (DIV_DENOM == 0 ? 0x4000 : 0)); /* div-by-0 flag, never busy */
    if (d == 0) {
        q = n >= 0 ? -1 : 1;
        r = n;
        if (mode == 0) q ^= (s64)0xFFFFFFFF00000000LL; /* 32-bit mode: upper word inverted */
    } else if (d == -1 && n == (s64)0x8000000000000000ULL) { /* overflow: no host trap */
        q = n;
        r = 0;
    } else {
        q = n / d;
        r = n % d;
    }
    DIV_RESULT = q;
    DIVREM_RESULT = r;
}

void pc_cp_sqrt_update(void) {
    u64 v = (SQRTCNT & 1) ? SQRT_PARAM : (u32)SQRT_PARAM;
    u64 res = 0, bit = 1ULL << 62;
    while (bit > v) bit >>= 2;
    while (bit) { /* bitwise integer square root */
        if (v >= res + bit) { v -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    SQRTCNT &= (u16)~0x8000;
    SQRT_RESULT = (u32)res;
}

int pc_cp_selftest(void) {
    CP_SetDiv32_32((u32)-7, 2);
    if (CP_GetDivResult32() != -3 || CP_GetDivRemainder32() != -1) return 0;
    CP_SetDiv64_32((u64)FX32_ONE << 32, (u32)FX32_CONST(2.0)); /* FX_Inv(2.0) path */
    if (CP_GetDivResult64() != ((s64)FX32_ONE << 32) / FX32_CONST(2.0)) return 0;
    if (FX_Div(FX32_CONST(3.0), FX32_CONST(1.5)) != FX32_CONST(2.0)) return 0;
    CP_SetSqrt64(1ULL << 40);
    if (CP_GetSqrtResult32() != 1u << 20) return 0;
    CP_SetSqrt32(99);
    if (CP_GetSqrtResult32() != 9) return 0;
    return FX_Sqrt(FX32_CONST(4.0)) == FX32_CONST(2.0);
}
