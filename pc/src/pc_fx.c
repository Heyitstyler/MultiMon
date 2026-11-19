/* Host versions of the SDK's asm-only matrix, copy and FIFO-send functions.
 * The C parts of fx_mtx*.c, mi_memory.c and g3*.c compile unchanged (pc_build.py strips their
 * mwcc `asm` functions); these replace the stripped ones. Matrix layouts follow the asm:
 * row-vector convention, rotations as in fx_mtx33.c (RotX: _11 = cos, _12 = sin, _21 = -sin). */
#include <string.h>
#include <nitro.h>
#include "pc_platform.h"

#define ONE FX32_ONE

static void Set(fx32 *d, const fx32 *s, int n) { memcpy(d, s, n * sizeof(fx32)); }

/* ---- 2x2 ---- */
void MTX_Identity22_(MtxFx22 *d) { fx32 m[4] = { ONE, 0, 0, ONE }; Set(&d->_00, m, 4); }
void MTX_Transpose22_(const MtxFx22 *s, MtxFx22 *d) { fx32 m[4] = { s->_00, s->_10, s->_01, s->_11 }; Set(&d->_00, m, 4); }
void MTX_Scale22_(MtxFx22 *d, fx32 x, fx32 y) { fx32 m[4] = { x, 0, 0, y }; Set(&d->_00, m, 4); }
void MTX_Rot22_(MtxFx22 *d, fx32 s, fx32 c) { fx32 m[4] = { c, s, -s, c }; Set(&d->_00, m, 4); }

/* ---- 3x3 ---- */
void MTX_Identity33_(MtxFx33 *d) { fx32 m[9] = { ONE, 0, 0, 0, ONE, 0, 0, 0, ONE }; Set(&d->_00, m, 9); }
void MTX_Scale33_(MtxFx33 *d, fx32 x, fx32 y, fx32 z) { fx32 m[9] = { x, 0, 0, 0, y, 0, 0, 0, z }; Set(&d->_00, m, 9); }
void MTX_RotX33_(MtxFx33 *d, fx32 s, fx32 c) { fx32 m[9] = { ONE, 0, 0, 0, c, s, 0, -s, c }; Set(&d->_00, m, 9); }
void MTX_RotY33_(MtxFx33 *d, fx32 s, fx32 c) { fx32 m[9] = { c, 0, -s, 0, ONE, 0, s, 0, c }; Set(&d->_00, m, 9); }
void MTX_RotZ33_(MtxFx33 *d, fx32 s, fx32 c) { fx32 m[9] = { c, s, 0, -s, c, 0, 0, 0, ONE }; Set(&d->_00, m, 9); }
void MTX_Transpose33_(const MtxFx33 *s, MtxFx33 *d) {
    const fx32 *a = &s->_00;
    fx32 m[9] = { a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8] };
    Set(&d->_00, m, 9);
}
void MTX_Copy33To43_(const MtxFx33 *s, MtxFx43 *d) { fx32 m[12] = { 0 }; Set(m, &s->_00, 9); Set(&d->_00, m, 12); }
void MTX_Copy33To44_(const MtxFx33 *s, MtxFx44 *d) {
    const fx32 *a = &s->_00;
    fx32 m[16] = { a[0], a[1], a[2], 0, a[3], a[4], a[5], 0, a[6], a[7], a[8], 0, 0, 0, 0, ONE };
    Set(&d->_00, m, 16);
}

/* ---- 4x3 (3x3 plus translation row) ---- */
static void From33(fx32 *d, const fx32 *m33) { fx32 m[12] = { 0 }; Set(m, m33, 9); Set(d, m, 12); }
void MTX_Identity43_(MtxFx43 *d) { MtxFx33 t; MTX_Identity33_(&t); From33(&d->_00, &t._00); }
void MTX_Scale43_(MtxFx43 *d, fx32 x, fx32 y, fx32 z) { MtxFx33 t; MTX_Scale33_(&t, x, y, z); From33(&d->_00, &t._00); }
void MTX_RotX43_(MtxFx43 *d, fx32 s, fx32 c) { MtxFx33 t; MTX_RotX33_(&t, s, c); From33(&d->_00, &t._00); }
void MTX_RotY43_(MtxFx43 *d, fx32 s, fx32 c) { MtxFx33 t; MTX_RotY33_(&t, s, c); From33(&d->_00, &t._00); }
void MTX_RotZ43_(MtxFx43 *d, fx32 s, fx32 c) { MtxFx33 t; MTX_RotZ33_(&t, s, c); From33(&d->_00, &t._00); }
void MTX_Transpose43_(const MtxFx43 *s, MtxFx43 *d) {
    /* SDK: transpose the 3x3 part; the translation row becomes zero (fx_mtx43.c). */
    MtxFx33 t;
    MTX_Transpose33_((const MtxFx33 *)s, &t);
    From33(&d->_00, &t._00);
}
void MTX_Copy43To44_(const MtxFx43 *s, MtxFx44 *d) {
    const fx32 *a = &s->_00;
    fx32 m[16] = { a[0], a[1], a[2], 0, a[3], a[4], a[5], 0, a[6], a[7], a[8], 0, a[9], a[10], a[11], ONE };
    Set(&d->_00, m, 16);
}

/* ---- 4x4 ---- */
static void From33To44(fx32 *d, const fx32 *a) {
    fx32 m[16] = { a[0], a[1], a[2], 0, a[3], a[4], a[5], 0, a[6], a[7], a[8], 0, 0, 0, 0, ONE };
    Set(d, m, 16);
}
void MTX_Identity44_(MtxFx44 *d) { MtxFx33 t; MTX_Identity33_(&t); From33To44(&d->_00, &t._00); }
void MTX_Scale44_(MtxFx44 *d, fx32 x, fx32 y, fx32 z) { MtxFx33 t; MTX_Scale33_(&t, x, y, z); From33To44(&d->_00, &t._00); }
void MTX_RotX44_(MtxFx44 *d, fx32 s, fx32 c) { MtxFx33 t; MTX_RotX33_(&t, s, c); From33To44(&d->_00, &t._00); }
void MTX_RotY44_(MtxFx44 *d, fx32 s, fx32 c) { MtxFx33 t; MTX_RotY33_(&t, s, c); From33To44(&d->_00, &t._00); }
void MTX_RotZ44_(MtxFx44 *d, fx32 s, fx32 c) { MtxFx33 t; MTX_RotZ33_(&t, s, c); From33To44(&d->_00, &t._00); }
void MTX_Transpose44_(const MtxFx44 *s, MtxFx44 *d) {
    fx32 m[16];
    const fx32 *a = &s->_00;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) m[r * 4 + c] = a[c * 4 + r];
    Set(&d->_00, m, 16);
}
void MTX_Copy44To33_(const MtxFx44 *s, MtxFx33 *d) {
    const fx32 *a = &s->_00;
    fx32 m[9] = { a[0], a[1], a[2], a[4], a[5], a[6], a[8], a[9], a[10] };
    Set(&d->_00, m, 9);
}
void MTX_Copy44To43_(const MtxFx44 *s, MtxFx43 *d) {
    const fx32 *a = &s->_00;
    fx32 m[12] = { a[0], a[1], a[2], a[4], a[5], a[6], a[8], a[9], a[10], a[12], a[13], a[14] };
    Set(&d->_00, m, 12);
}

/* ---- memory ---- */
void MI_Copy16B(const void *s, void *d) { memmove(d, s, 16); }
void MI_Copy48B(const void *s, void *d) { memmove(d, s, 48); }
void MI_Copy128B(const void *s, void *d) { memmove(d, s, 128); }
void MI_Zero32B(void *d) { memset(d, 0, 32); }
void MI_Zero48B(void *d) { memset(d, 0, 48); }
void MI_Zero64B(void *d) { memset(d, 0, 64); }
u32 MI_SwapWord(u32 v, vu32 *p) { u32 old = *p; *p = v; return old; }
u8 MI_SwapByte(u32 v, vu8 *p) { u8 old = *p; *p = (u8)v; return old; }

/* ---- geometry FIFO sends (dest is the FIFO port) ---- */
static void SendFifo(const void *src, void *dest, u32 words) {
    if (pc_g3_is_port(dest)) pc_g3_send(src, words);
    else memmove(dest, src, words * 4);
}
void GX_SendFifo36B(const void *s, void *d) { SendFifo(s, d, 9); }
void GX_SendFifo48B(const void *s, void *d) { SendFifo(s, d, 12); }
void GX_SendFifo64B(const void *s, void *d) { SendFifo(s, d, 16); }
void GX_SendFifo128B(const void *s, void *d) { SendFifo(s, d, 32); }

/* g3x.c: clears the FIFO with 128 NOP words. */
void GXi_NopClearFifo128_(void *dest) {
    static const u32 zeros[128];
    SendFifo(zeros, dest, 128);
}
