/* DS geometry engine on the host.
 *
 * Commands arrive three ways, all ending in pc_g3_write(offset, value):
 *   - C code: the command ports and GXFIFO live on a read-only trap page (pc/include/nitro/hw/ARM9/
 *     ioreg_G3*.h). Each store faults; the handler single-steps the store, reads the value and
 *     runs the command. This keeps `&reg_G3X_GXFIFO`, port address arithmetic and multi-word
 *     writes through one pointer working without touching SDK or game sources.
 *   - Translated asm: IO-page stores go through pc_io_store (asm_rt.h ST8/16/32).
 *   - Bulk sends (DMA, MI_CpuSend32, GX_SendFifo*): pc_g3_send.
 *
 * Matrices are DS fixed point (20.12, row vectors, new = param x current) so read-back registers
 * (CLIPMTX_RESULT, VECMTX_RESULT, POS/VEC test, GXSTAT stack levels) match hardware. Polygons are
 * culled, clipped against the view volume, mapped to the viewport and handed to the rasterizer
 * (pc_g3r.c) at SWAP_BUFFERS. Behaviour follows GBATEK "DS 3D" and melonDS for lighting details.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#undef far /* windows.h macros */
#undef near
#else
#include <ucontext.h>
#endif
#include <nitro.h>
#include "pc_platform.h"

void pc_cp_div_update(void);
void pc_cp_sqrt_update(void);

#define TRAP_BASE 0x04800000u
#define IO32(off) (*(vu32 *)(0x04000000u + (off)))
#define IO16(off) (*(vu16 *)(0x04000000u + (off)))
#define MAX_POLYS 2048
#define MAX_VERTS 6144

typedef struct { s32 m[16]; } Mtx;

/* Parameter words per command; 0xFF = invalid (treated as a NOP). */
static u8 sParams[256];

static void InitParamTable(void) {
    memset(sParams, 0xFF, sizeof sParams);
    static const u8 t[][2] = {
        { 0x00, 0 }, { 0x10, 1 }, { 0x11, 0 }, { 0x12, 1 }, { 0x13, 1 }, { 0x14, 1 }, { 0x15, 0 },
        { 0x16, 16 }, { 0x17, 12 }, { 0x18, 16 }, { 0x19, 12 }, { 0x1A, 9 }, { 0x1B, 3 }, { 0x1C, 3 },
        { 0x20, 1 }, { 0x21, 1 }, { 0x22, 1 }, { 0x23, 2 }, { 0x24, 1 }, { 0x25, 1 }, { 0x26, 1 },
        { 0x27, 1 }, { 0x28, 1 }, { 0x29, 1 }, { 0x2A, 1 }, { 0x2B, 1 }, { 0x30, 1 }, { 0x31, 1 },
        { 0x32, 1 }, { 0x33, 1 }, { 0x34, 32 }, { 0x40, 1 }, { 0x41, 0 }, { 0x50, 1 }, { 0x60, 1 },
        { 0x70, 3 }, { 0x71, 2 }, { 0x72, 1 },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) sParams[t[i][0]] = t[i][1];
}

/* ---- state ---- */
static Mtx sProj, sPos, sVec, sTex, sClip;
static Mtx sProjStack, sPosStack[32], sVecStack[32], sTexStack;
static int sMode, sSp, sProjSp, sOverflow;

static s32 sVx, sVy, sVz;               /* current vertex position, 4.12 */
static int sColR = 31, sColG = 31, sColB = 31; /* current vertex colour, 5-bit */
static s16 sRawS, sRawT, sS, sT;        /* texcoords 12.4: raw and transformed */
static u32 sPolyAttrNext, sPolyAttr, sTexParam, sPltt;
static int sVpX1, sVpY1, sVpX2 = 255, sVpY2 = 191;

static s32 sLightDir[4][3];
static int sLightCol[4][3];
static int sDif[3], sAmb[3], sSpe[3], sEmi[3], sUseShine;
static u8 sShine[128];

typedef struct { s32 x, y, z, w; float r, g, b, s, t; } Vtx;
static int sPrim, sPrimCount, sStripOdd;
static Vtx sPrimV[4];

static PcG3Poly sPolys[MAX_POLYS];
static int sNumPolys, sNumVerts;

/* ---- matrices ---- */
static void Identity(Mtx *m) {
    memset(m, 0, sizeof *m);
    m->m[0] = m->m[5] = m->m[10] = m->m[15] = 0x1000;
}

static void MultInto(Mtx *dst, const Mtx *a) { /* dst = a * dst */
    Mtx r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            s64 sum = 0;
            for (int k = 0; k < 4; k++) sum += (s64)a->m[i * 4 + k] * dst->m[k * 4 + j];
            r.m[i * 4 + j] = (s32)(sum >> 12);
        }
    *dst = r;
}

static void FromParams(Mtx *m, const u32 *p, int rows, int cols) {
    Identity(m);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) m->m[r * 4 + c] = (s32)p[r * cols + c];
}

static void UpdateResults(void) {
    sClip = sProj;
    MultInto(&sClip, &sPos); /* clip = pos * proj */
    for (int i = 0; i < 16; i++) IO32(0x640 + i * 4) = (u32)sClip.m[i];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) IO32(0x680 + (r * 3 + c) * 4) = (u32)sVec.m[r * 4 + c];
}

static void UpdateStatus(void) {
    u32 st = IO32(0x600) & 0xC0000003u; /* keep IRQ condition and test result bits */
    st |= (u32)(sSp & 31) << 8 | (u32)(sProjSp & 1) << 13 | (u32)(sOverflow & 1) << 15;
    st |= 1u << 25 | 1u << 26; /* FIFO less than half full, empty; never busy */
    IO32(0x600) = st;
    IO16(0x604) = (u16)sNumPolys;
    IO16(0x606) = (u16)sNumVerts;
}

/* Matrices a command applies to in the current mode. Scale in mode 2 touches only position. */
static int Targets(Mtx **t, int scale) {
    switch (sMode) {
    case 0: t[0] = &sProj; return 1;
    case 1: t[0] = &sPos; return 1;
    case 2: t[0] = &sPos; t[1] = &sVec; return scale ? 1 : 2;
    default: t[0] = &sTex; return 1;
    }
}

/* ---- lighting (melonDS CalculateLighting, 5-bit colours) ---- */
static void Lighting(const s32 *n) {
    s32 nt[3];
    for (int i = 0; i < 3; i++)
        nt[i] = (s32)(((s64)n[0] * sVec.m[0 + i] + (s64)n[1] * sVec.m[4 + i] + (s64)n[2] * sVec.m[8 + i]) >> 12);
    int c[3] = { sEmi[0], sEmi[1], sEmi[2] };
    for (int l = 0; l < 4; l++) {
        if (!(sPolyAttr & (1u << l))) continue;
        s32 *L = sLightDir[l];
        s32 diff = -((L[0] * nt[0] + L[1] * nt[1] + L[2] * nt[2]) >> 10);
        if (diff < 0) diff = 0;
        if (diff > 255) diff = 255;
        s32 shine = -(((L[0] >> 1) * nt[0] + (L[1] >> 1) * nt[1] + ((L[2] - 0x200) >> 1) * nt[2]) >> 10);
        if (shine < 0) shine = 0;
        else if (shine > 255) shine = (0x100 - shine) & 0xFF;
        shine = ((shine * shine) >> 7) - 0x100;
        if (shine < 0) shine = 0;
        if (sUseShine) shine = sShine[(shine >> 1) & 127];
        for (int i = 0; i < 3; i++) {
            c[i] += (sSpe[i] * sLightCol[l][i] * shine) >> 13;
            c[i] += (sDif[i] * sLightCol[l][i] * diff) >> 13;
            c[i] += (sAmb[i] * sLightCol[l][i]) >> 5;
        }
    }
    sColR = c[0] > 31 ? 31 : c[0];
    sColG = c[1] > 31 ? 31 : c[1];
    sColB = c[2] > 31 ? 31 : c[2];
}

static void Rgb(u32 v, int *c) { c[0] = v & 31; c[1] = (v >> 5) & 31; c[2] = (v >> 10) & 31; }

/* ---- polygons ---- */

/* Clip polygon (homogeneous, doubles) against one plane: keep side where sign*comp <= w. */
typedef struct { double x, y, z, w, r, g, b, s, t; } CV;

static int ClipPlane(CV *in, int n, CV *out, int axis, double sign) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        CV *a = &in[i], *b = &in[(i + 1) % n];
        double da = a->w - sign * (&a->x)[axis], db = b->w - sign * (&b->x)[axis];
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0)) {
            double t = da / (da - db);
            double *pa = &a->x, *pb = &b->x, *po = &out[m].x;
            for (int k = 0; k < 9; k++) po[k] = pa[k] + (pb[k] - pa[k]) * t;
            m++;
        }
    }
    return m;
}

static void EmitPolygon(Vtx **v, int n) {
    u32 attr = sPolyAttr;
    /* Culling: sign of the plane through the vertices in (x, y, w); negative = front (clockwise on screen). */
    s64 ax = v[0]->x - v[1]->x, ay = v[0]->y - v[1]->y, aw = v[0]->w - v[1]->w;
    s64 bx = v[2]->x - v[1]->x, by = v[2]->y - v[1]->y, bw = v[2]->w - v[1]->w;
    double nx = (double)ay * bw - (double)aw * by, ny = (double)aw * bx - (double)ax * bw, nw = (double)ax * by - (double)ay * bx;
    double dot = nx * v[1]->x + ny * v[1]->y + nw * v[1]->w;
    if (dot < 0 && !(attr & 0x80)) return;
    if (dot > 0 && !(attr & 0x40)) return;

    CV a[10], b[10];
    int far = 0;
    for (int i = 0; i < n; i++) {
        a[i] = (CV){ v[i]->x, v[i]->y, v[i]->z, v[i]->w, v[i]->r, v[i]->g, v[i]->b, v[i]->s, v[i]->t };
        if (v[i]->z > v[i]->w) far = 1;
    }
    if (far && !(attr & 0x1000)) return; /* far-plane crossing polygons hidden unless enabled */
    n = ClipPlane(a, n, b, 2, -1);  /* near */
    n = ClipPlane(b, n, a, 2, 1);   /* far */
    n = ClipPlane(a, n, b, 0, 1);
    n = ClipPlane(b, n, a, 0, -1);
    n = ClipPlane(a, n, b, 1, 1);
    n = ClipPlane(b, n, a, 1, -1);
    if (n < 3) return;
    if (sNumPolys >= MAX_POLYS || sNumVerts + n > MAX_VERTS) return;
    if (n > 10) n = 10;

    PcG3Poly *p = &sPolys[sNumPolys++];
    sNumVerts += n;
    p->n = n;
    p->attr = attr;
    p->texParam = sTexParam;
    p->plttBase = sPltt;
    u32 alpha = (attr >> 16) & 31, fmt = (sTexParam >> 26) & 7;
    p->translucent = (alpha != 0 && alpha != 31) || fmt == 1 || fmt == 6;
    p->ytop = 1e9f;
    p->ybottom = -1e9f;
    double vw = sVpX2 - sVpX1 + 1, vh = sVpY2 - sVpY1 + 1;
    for (int i = 0; i < n; i++) {
        CV *c = &a[i];
        double w = c->w > 1e-3 ? c->w : 1e-3;
        PcG3Vertex *o = &p->v[i];
        o->x = (float)((c->x + w) * vw / (2 * w) + sVpX1);
        o->y = (float)((w - c->y) * vh / (2 * w) + (191 - sVpY2));
        o->z = (float)(((c->z / w) * 0x4000 + 0x3FFF) * 0x200); /* 24-bit Z-buffer value */
        o->w = (float)w;
        o->r = (float)c->r; o->g = (float)c->g; o->b = (float)c->b;
        o->s = (float)c->s; o->t = (float)c->t;
        if (o->y < p->ytop) p->ytop = o->y;
        if (o->y > p->ybottom) p->ybottom = o->y;
    }
}

static void SubmitVertex(void) {
    const s32 *c = sClip.m;
    Vtx v;
    s64 x = sVx, y = sVy, z = sVz;
    v.x = (s32)((x * c[0] + y * c[4] + z * c[8] + 4096LL * c[12]) >> 12);
    v.y = (s32)((x * c[1] + y * c[5] + z * c[9] + 4096LL * c[13]) >> 12);
    v.z = (s32)((x * c[2] + y * c[6] + z * c[10] + 4096LL * c[14]) >> 12);
    v.w = (s32)((x * c[3] + y * c[7] + z * c[11] + 4096LL * c[15]) >> 12);
    if ((sTexParam >> 30) == 3) { /* texcoords from vertex position */
        const s32 *t = sTex.m;
        sS = (s16)(((x * t[0] + y * t[4] + z * t[8]) >> 24) + sRawS);
        sT = (s16)(((x * t[1] + y * t[5] + z * t[9]) >> 24) + sRawT);
    }
    v.r = sColR; v.g = sColG; v.b = sColB;
    v.s = sS / 16.0f; v.t = sT / 16.0f;

    sPrimV[sPrimCount++] = v;
    Vtx *q[4];
    switch (sPrim) {
    case 0: /* triangles */
        if (sPrimCount == 3) {
            q[0] = &sPrimV[0]; q[1] = &sPrimV[1]; q[2] = &sPrimV[2];
            EmitPolygon(q, 3);
            sPrimCount = 0;
        }
        break;
    case 1: /* quads */
        if (sPrimCount == 4) {
            q[0] = &sPrimV[0]; q[1] = &sPrimV[1]; q[2] = &sPrimV[2]; q[3] = &sPrimV[3];
            EmitPolygon(q, 4);
            sPrimCount = 0;
        }
        break;
    case 2: /* triangle strip: every other triangle has its first two vertices swapped */
        if (sPrimCount == 3) {
            q[0] = &sPrimV[sStripOdd ? 1 : 0]; q[1] = &sPrimV[sStripOdd ? 0 : 1]; q[2] = &sPrimV[2];
            EmitPolygon(q, 3);
            sPrimV[0] = sPrimV[1];
            sPrimV[1] = sPrimV[2];
            sPrimCount = 2;
            sStripOdd ^= 1;
        }
        break;
    case 3: /* quad strip: v0 v1 v3 v2 */
        if (sPrimCount == 4) {
            q[0] = &sPrimV[0]; q[1] = &sPrimV[1]; q[2] = &sPrimV[3]; q[3] = &sPrimV[2];
            EmitPolygon(q, 4);
            sPrimV[0] = sPrimV[2];
            sPrimV[1] = sPrimV[3];
            sPrimCount = 2;
        }
        break;
    }
}

static s32 Sext(u32 v, int bits) { return (s32)(v << (32 - bits)) >> (32 - bits); }

static void BoxTest(const u32 *p) {
    s32 x = (s16)p[0], y = (s16)(p[0] >> 16), z = (s16)p[1];
    s32 w = (s16)(p[1] >> 16), h = (s16)p[2], d = (s16)(p[2] >> 16);
    int outside[6] = { 1, 1, 1, 1, 1, 1 };
    const s32 *c = sClip.m;
    for (int i = 0; i < 8; i++) {
        s64 vx = x + (i & 1 ? w : 0), vy = y + (i & 2 ? h : 0), vz = z + (i & 4 ? d : 0);
        s64 cc[4];
        for (int k = 0; k < 4; k++) cc[k] = (vx * c[k] + vy * c[4 + k] + vz * c[8 + k] + 4096LL * c[12 + k]) >> 12;
        for (int k = 0; k < 3; k++) {
            if (cc[k] <= cc[3]) outside[k * 2] = 0;
            if (cc[k] >= -cc[3]) outside[k * 2 + 1] = 0;
        }
    }
    // shortcut: corner test (true unless all corners lie outside one plane); hardware tests the
    // clipped faces, so a box straddling a frustum corner reads "visible" here. Only over-draws.
    int in = !(outside[0] | outside[1] | outside[2] | outside[3] | outside[4] | outside[5]);
    IO32(0x600) = (IO32(0x600) & ~3u) | (in ? 2u : 0u);
}

/* ---- command execution ---- */
static void Exec(u8 cmd, const u32 *p) {
    Mtx m, *t[2];
    int nt;
    switch (cmd) {
    case 0x10: sMode = p[0] & 3; break;
    case 0x11: /* MTX_PUSH */
        if (sMode == 0) { if (sProjSp) sOverflow = 1; sProjStack = sProj; sProjSp = 1; }
        else if (sMode == 3) sTexStack = sTex;
        else {
            if (sSp >= 31) sOverflow = 1;
            sPosStack[sSp & 31] = sPos;
            sVecStack[sSp & 31] = sVec;
            sSp = (sSp + 1) & 63;
        }
        break;
    case 0x12: /* MTX_POP */
        if (sMode == 0) { sProjSp = 0; sProj = sProjStack; }
        else if (sMode == 3) sTex = sTexStack;
        else {
            sSp = (sSp - Sext(p[0], 6)) & 63;
            if (sSp >= 31) sOverflow = 1;
            sPos = sPosStack[sSp & 31];
            sVec = sVecStack[sSp & 31];
        }
        break;
    case 0x13: /* MTX_STORE */
        if (sMode == 0) sProjStack = sProj;
        else if (sMode == 3) sTexStack = sTex;
        else { u32 i = p[0] & 31; if (i == 31) sOverflow = 1; sPosStack[i] = sPos; sVecStack[i] = sVec; }
        break;
    case 0x14: /* MTX_RESTORE */
        if (sMode == 0) sProj = sProjStack;
        else if (sMode == 3) sTex = sTexStack;
        else { u32 i = p[0] & 31; if (i == 31) sOverflow = 1; sPos = sPosStack[i]; sVec = sVecStack[i]; }
        break;
    case 0x15: nt = Targets(t, 0); for (int i = 0; i < nt; i++) Identity(t[i]); break;
    case 0x16: FromParams(&m, p, 4, 4); nt = Targets(t, 0); for (int i = 0; i < nt; i++) *t[i] = m; break;
    case 0x17: FromParams(&m, p, 4, 3); nt = Targets(t, 0); for (int i = 0; i < nt; i++) *t[i] = m; break;
    case 0x18: FromParams(&m, p, 4, 4); nt = Targets(t, 0); for (int i = 0; i < nt; i++) MultInto(t[i], &m); break;
    case 0x19: FromParams(&m, p, 4, 3); nt = Targets(t, 0); for (int i = 0; i < nt; i++) MultInto(t[i], &m); break;
    case 0x1A: FromParams(&m, p, 3, 3); nt = Targets(t, 0); for (int i = 0; i < nt; i++) MultInto(t[i], &m); break;
    case 0x1B: /* MTX_SCALE */
        Identity(&m); m.m[0] = (s32)p[0]; m.m[5] = (s32)p[1]; m.m[10] = (s32)p[2];
        nt = Targets(t, 1); for (int i = 0; i < nt; i++) MultInto(t[i], &m);
        break;
    case 0x1C: /* MTX_TRANS */
        Identity(&m); m.m[12] = (s32)p[0]; m.m[13] = (s32)p[1]; m.m[14] = (s32)p[2];
        nt = Targets(t, 0); for (int i = 0; i < nt; i++) MultInto(t[i], &m);
        break;
    case 0x20: { int c[3]; Rgb(p[0], c); sColR = c[0]; sColG = c[1]; sColB = c[2]; } break;
    case 0x21: { /* NORMAL */
        s32 n[3] = { Sext(p[0], 10), Sext(p[0] >> 10, 10), Sext(p[0] >> 20, 10) };
        if ((sTexParam >> 30) == 2) {
            const s32 *tm = sTex.m;
            sS = (s16)((((s64)n[0] * tm[0] + (s64)n[1] * tm[4] + (s64)n[2] * tm[8]) >> 21) + sRawS);
            sT = (s16)((((s64)n[0] * tm[1] + (s64)n[1] * tm[5] + (s64)n[2] * tm[9]) >> 21) + sRawT);
        }
        Lighting(n);
        break;
    }
    case 0x22: /* TEXCOORD */
        sRawS = (s16)p[0];
        sRawT = (s16)(p[0] >> 16);
        if ((sTexParam >> 30) == 1) {
            const s32 *tm = sTex.m;
            sS = (s16)(((s64)sRawS * tm[0] + (s64)sRawT * tm[4] + tm[8] + tm[12]) >> 12);
            sT = (s16)(((s64)sRawS * tm[1] + (s64)sRawT * tm[5] + tm[9] + tm[13]) >> 12);
        } else {
            sS = sRawS;
            sT = sRawT;
        }
        break;
    case 0x23: sVx = (s16)p[0]; sVy = (s16)(p[0] >> 16); sVz = (s16)p[1]; SubmitVertex(); break;
    case 0x24: sVx = Sext(p[0], 10) << 6; sVy = Sext(p[0] >> 10, 10) << 6; sVz = Sext(p[0] >> 20, 10) << 6; SubmitVertex(); break;
    case 0x25: sVx = (s16)p[0]; sVy = (s16)(p[0] >> 16); SubmitVertex(); break;
    case 0x26: sVx = (s16)p[0]; sVz = (s16)(p[0] >> 16); SubmitVertex(); break;
    case 0x27: sVy = (s16)p[0]; sVz = (s16)(p[0] >> 16); SubmitVertex(); break;
    case 0x28: /* VTX_DIFF: 10-bit signed deltas in raw 4.12 units (as melonDS; model data confirms) */
        sVx = (s16)(sVx + Sext(p[0], 10));
        sVy = (s16)(sVy + Sext(p[0] >> 10, 10));
        sVz = (s16)(sVz + Sext(p[0] >> 20, 10));
        SubmitVertex();
        break;
    case 0x29: sPolyAttrNext = p[0]; break;
    case 0x2A: sTexParam = p[0]; break;
    case 0x2B: sPltt = p[0] & 0x1FFF; break;
    case 0x30:
        Rgb(p[0], sDif);
        Rgb(p[0] >> 16, sAmb);
        if (p[0] & 0x8000) { sColR = sDif[0]; sColG = sDif[1]; sColB = sDif[2]; }
        break;
    case 0x31: Rgb(p[0], sSpe); Rgb(p[0] >> 16, sEmi); sUseShine = (p[0] >> 15) & 1; break;
    case 0x32: { /* LIGHT_VECTOR, transformed by the vector matrix */
        int l = p[0] >> 30;
        s32 d[3] = { Sext(p[0], 10), Sext(p[0] >> 10, 10), Sext(p[0] >> 20, 10) };
        for (int i = 0; i < 3; i++)
            sLightDir[l][i] = (s32)(((s64)d[0] * sVec.m[0 + i] + (s64)d[1] * sVec.m[4 + i] + (s64)d[2] * sVec.m[8 + i]) >> 12);
        break;
    }
    case 0x33: Rgb(p[0], sLightCol[p[0] >> 30]); break;
    case 0x34: for (int i = 0; i < 32; i++) memcpy(&sShine[i * 4], &p[i], 4); break;
    case 0x40: sPrim = p[0] & 3; sPrimCount = 0; sStripOdd = 0; sPolyAttr = sPolyAttrNext; break;
    case 0x41: break;
    case 0x50: /* SWAP_BUFFERS */
        pc_g3r_render(sPolys, sNumPolys, p[0] & 1, (p[0] >> 1) & 1);
        sNumPolys = sNumVerts = 0;
        break;
    case 0x60:
        sVpX1 = p[0] & 0xFF; sVpY1 = (p[0] >> 8) & 0xFF; sVpX2 = (p[0] >> 16) & 0xFF; sVpY2 = p[0] >> 24;
        break;
    case 0x70: BoxTest(p); break;
    case 0x71: { /* POS_TEST: also sets the current vertex */
        sVx = (s16)p[0]; sVy = (s16)(p[0] >> 16); sVz = (s16)p[1];
        const s32 *c = sClip.m;
        for (int k = 0; k < 4; k++)
            IO32(0x620 + k * 4) = (u32)(((s64)sVx * c[k] + (s64)sVy * c[4 + k] + (s64)sVz * c[8 + k] + 4096LL * c[12 + k]) >> 12);
        break;
    }
    case 0x72: { /* VEC_TEST: 1.9 vector through the vector matrix, result 4.12 */
        s32 d[3] = { Sext(p[0], 10), Sext(p[0] >> 10, 10), Sext(p[0] >> 20, 10) };
        for (int i = 0; i < 3; i++)
            IO16(0x630 + i * 2) = (u16)(((s64)d[0] * sVec.m[0 + i] + (s64)d[1] * sVec.m[4 + i] + (s64)d[2] * sVec.m[8 + i]) >> 9);
        break;
    }
    default: return; /* NOP / invalid */
    }
    if (cmd >= 0x10 && cmd <= 0x1C) UpdateResults();
    UpdateStatus();
}

/* ---- command decoding ---- */
static u32 sFifoCmds;
static int sFifoLeft, sFifoNeed, sFifoHave;
static u8 sFifoCmd;
static u32 sFifoParams[32];

static void NextFifoCommand(void) {
    sFifoNeed = sFifoHave = 0;
    while (sFifoLeft > 0) {
        u8 c = sFifoCmds & 0xFF;
        sFifoCmds >>= 8;
        sFifoLeft--;
        int n = sParams[c] == 0xFF ? 0 : sParams[c];
        if (n == 0) { Exec(c, NULL); continue; }
        sFifoCmd = c;
        sFifoNeed = n;
        return;
    }
}

static void FifoWrite(u32 w) {
    if (sFifoNeed) {
        sFifoParams[sFifoHave++] = w;
        if (sFifoHave == sFifoNeed) {
            Exec(sFifoCmd, sFifoParams);
            NextFifoCommand();
        }
        return;
    }
    sFifoCmds = w; /* packed: up to four command bytes, lowest first */
    sFifoLeft = 4;
    NextFifoCommand();
}

static u8 sPortCmd;
static int sPortHave;
static u32 sPortParams[32];

static void PortWrite(u8 cmd, u32 v) {
    int n = sParams[cmd] == 0xFF ? 0 : sParams[cmd];
    if (n == 0) { Exec(cmd, NULL); return; }
    if (cmd != sPortCmd) { sPortCmd = cmd; sPortHave = 0; }
    sPortParams[sPortHave++] = v;
    if (sPortHave == n) {
        Exec(cmd, sPortParams);
        sPortHave = 0;
        sPortCmd = 0;
    }
}

void pc_g3_write(u32 offset, u32 value) {
    offset &= 0xFFF;
    if (offset < 0x440) FifoWrite(value);
    else if (offset < 0x600) PortWrite((u8)((offset - 0x400) >> 2), value);
}

void pc_g3_send(const u32 *words, u32 count) {
    for (u32 i = 0; i < count; i++) FifoWrite(words[i]);
}

BOOL pc_g3_is_port(const volatile void *addr) {
    u32 a = (u32)(uintptr_t)addr, page = a & ~0xFFFu, off = a & 0xFFF;
    return (page == TRAP_BASE || page == 0x04000000u) && off >= 0x400 && off < 0x600;
}

void pc_io_store(u32 a, u32 v, int size) {
    u32 off = a & 0xFFF;
    if (off >= 0x400 && off < 0x600) {
        pc_g3_write(off, v);
        return;
    }
    if (size == 4) *(vu32 *)a = v;
    else if (size == 2) *(vu16 *)a = (u16)v;
    else *(vu8 *)a = (u8)v;
    if (off >= 0x240 && off < 0x24A) pc_vram_sync();
    else if (off >= 0x280 && off < 0x2B0) pc_cp_div_update();
    else if (off >= 0x2B0 && off < 0x2C0) pc_cp_sqrt_update();
}

/* ---- trap page: C stores to the ports fault here ---- */
static volatile u32 sTrapAddr;

/* The faulting store was single-stepped into the page: re-arm the trap and run the command. */
static void TrapStepped(void) {
    u32 a = sTrapAddr;
    u32 v = *(vu32 *)(a & ~3u);
    pc_mem_protect_page(TRAP_BASE, FALSE);
    sTrapAddr = 0;
    pc_g3_write(a & 0xFFF, v);
}

#ifdef _WIN32
static LONG CALLBACK OnException(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        u32 a = (u32)er->ExceptionInformation[1];
        if ((a & ~0xFFFu) == TRAP_BASE && sTrapAddr == 0) {
            sTrapAddr = a;
            pc_mem_protect_page(TRAP_BASE, TRUE);
            ep->ContextRecord->EFlags |= 0x100; /* single-step the store */
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        fprintf(stderr, "hg_pc: fault address %08x at eip [0x%08x]\n", a, (u32)ep->ContextRecord->Eip);
        return EXCEPTION_CONTINUE_SEARCH; /* pc_main's unhandled-exception filter prints the backtrace */
    }
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && sTrapAddr != 0) {
        ep->ContextRecord->EFlags &= ~0x100;
        TrapStepped();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void TrapInit(void) {
    pc_mem_protect_page(TRAP_BASE, FALSE); /* inside the DS range reserved by pc_mem_init */
    AddVectoredExceptionHandler(1, OnException);
}
#else
static struct sigaction sOldSegv;

static void OnSegv(int sig, siginfo_t *si, void *ctx) {
    u32 a = (u32)(uintptr_t)si->si_addr;
    if ((a & ~0xFFFu) == TRAP_BASE && sTrapAddr == 0) {
        sTrapAddr = a;
        pc_mem_protect_page(TRAP_BASE, TRUE);
        ((ucontext_t *)ctx)->uc_mcontext.gregs[REG_EFL] |= 0x100; /* single-step the store */
        return;
    }
    /* a real crash: the backtrace loses the faulting frame, so print it here */
    fprintf(stderr, "hg_pc: fault address %08x at eip [0x%08x]\n", a, (u32)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_EIP]);
    if (sOldSegv.sa_flags & SA_SIGINFO) sOldSegv.sa_sigaction(sig, si, ctx);
    else if (sOldSegv.sa_handler != SIG_DFL && sOldSegv.sa_handler != SIG_IGN) sOldSegv.sa_handler(sig);
    else { signal(sig, SIG_DFL); raise(sig); }
}

static void OnTrap(int sig, siginfo_t *si, void *ctx) {
    (void)sig;
    (void)si;
    ((ucontext_t *)ctx)->uc_mcontext.gregs[REG_EFL] &= ~0x100;
    if (sTrapAddr != 0) TrapStepped();
}

static void TrapInit(void) {
    pc_mem_protect_page(TRAP_BASE, FALSE); /* inside the DS range reserved by pc_mem_init */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sa.sa_sigaction = OnSegv;
    sigaction(SIGSEGV, &sa, &sOldSegv);
    sa.sa_sigaction = OnTrap;
    sigaction(SIGTRAP, &sa, NULL);
}
#endif

void pc_g3_init(void) {
    InitParamTable();
    Identity(&sProj); Identity(&sPos); Identity(&sVec); Identity(&sTex);
    UpdateResults();
    UpdateStatus();
    TrapInit();
}

/* Self-check of matrix ordering and the trap path: identity projection, translate (1,2,3), then a
 * position test of the origin must give (1,2,3,1). Run after pc_g3_init. */
int pc_g3_selftest(void) {
    volatile u32 *mode = (volatile u32 *)(TRAP_BASE + 0x440), *ident = (volatile u32 *)(TRAP_BASE + 0x454);
    volatile u32 *trans = (volatile u32 *)(TRAP_BASE + 0x470), *pos = (volatile u32 *)(TRAP_BASE + 0x5C4);
    *mode = 0; *ident = 0; *mode = 2; *ident = 0;
    *trans = 0x1000; *trans = 0x2000; *trans = 0x3000;
    *pos = 0; *pos = 0;
    int ok = IO32(0x620) == 0x1000 && IO32(0x624) == 0x2000 && IO32(0x628) == 0x3000 && IO32(0x62C) == 0x1000;
    *ident = 0;
    return ok;
}
