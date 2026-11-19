/* DS 3D rasterizer (software, float).
 *
 * Draws the polygon list from SWAP_BUFFERS into a 256x192 colour/alpha buffer that the 2D
 * renderer shows as BG0. Opaque polygons first, then translucent ones (sorted by Y unless the
 * game asked for manual order). Per pixel: depth test (less, or equal within tolerance),
 * texture (all 7 formats incl. 4x4 compressed), modulate/decal/toon/highlight, alpha test,
 * alpha blending, translucent polygon-ID rule, fog. Registers read here: DISP3DCNT, clear
 * colour/depth, toon table, alpha-test ref, fog colour/offset/table.
 *
 * shortcut: no edge marking, anti-aliasing, shadow polygons, rear-plane bitmap or 1-dot depth
 * cull; add when a scene needs them. Rasterisation uses pixel centres, not the DS's exact rules.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <nitro.h>
#include "pc_platform.h"

#define W 256
#define H 192
#define IO16(off) (*(vu16 *)(0x04000000u + (off)))
#define IO32(off) (*(vu32 *)(0x04000000u + (off)))

static u16 sColor[W * H];
static u8 sAlpha[W * H];
static float sDepth[W * H];
static u8 sTransId[W * H]; /* polygon ID of the last translucent write, 0xFF = none */
static u8 sFog[W * H];

const u16 *pc_g3r_color(void) { return sColor; }
const u8 *pc_g3r_alpha(void) { return sAlpha; }

/* ---- texture memory ---- */
static inline u8 TexByte(u32 a) {
    const u8 *s = pc_vram_tex_slot((a >> 17) & 3);
    return s ? s[a & 0x1FFFF] : 0;
}
static inline u16 TexHalf(u32 a) { return (u16)(TexByte(a) | TexByte(a + 1) << 8); }
static inline u16 PalColor(u32 a) {
    const u8 *s = pc_vram_texpltt_slot((a >> 14) % 6);
    return s ? (u16)(s[a & 0x3FFF] | s[(a + 1) & 0x3FFF] << 8) : 0;
}

static int Wrap(int v, int size, int repeat, int flip) {
    if (!repeat) return v < 0 ? 0 : v >= size ? size - 1 : v;
    if (flip) {
        v &= 2 * size - 1;
        return v >= size ? 2 * size - 1 - v : v;
    }
    return v & (size - 1);
}

static u16 Mix(u16 a, u16 b, int wa, int wb, int shift) {
    int r = ((a & 31) * wa + (b & 31) * wb) >> shift;
    int g = (((a >> 5) & 31) * wa + ((b >> 5) & 31) * wb) >> shift;
    int bl = (((a >> 10) & 31) * wa + ((b >> 10) & 31) * wb) >> shift;
    return (u16)(r | g << 5 | bl << 10);
}

/* Fetch texel: returns RGB555 in *col and alpha 0-31 (0 = transparent). */
static void TexFetch(u32 tp, u32 pltt, int s, int t, u16 *col, int *alpha) {
    u32 fmt = (tp >> 26) & 7;
    int w = 8 << ((tp >> 20) & 7), h = 8 << ((tp >> 23) & 7);
    s = Wrap(s, w, tp & (1 << 16), tp & (1 << 18));
    t = Wrap(t, h, tp & (1 << 17), tp & (1 << 19));
    u32 base = (tp & 0xFFFF) << 3, texel = (u32)(t * w + s);
    int idx;
    *alpha = 31;
    switch (fmt) {
    case 1: { /* A3I5 */
        u8 b = TexByte(base + texel);
        int a3 = b >> 5;
        *col = PalColor(pltt * 16 + (b & 31) * 2);
        *alpha = a3 * 4 + a3 / 2;
        return;
    }
    case 2: /* 4 colours */
        idx = (TexByte(base + texel / 4) >> ((texel & 3) * 2)) & 3;
        *col = PalColor(pltt * 8 + idx * 2);
        if (idx == 0 && (tp & (1 << 29))) *alpha = 0;
        return;
    case 3: /* 16 colours */
        idx = (TexByte(base + texel / 2) >> ((texel & 1) * 4)) & 15;
        *col = PalColor(pltt * 16 + idx * 2);
        if (idx == 0 && (tp & (1 << 29))) *alpha = 0;
        return;
    case 4: /* 256 colours */
        idx = TexByte(base + texel);
        *col = PalColor(pltt * 16 + idx * 2);
        if (idx == 0 && (tp & (1 << 29))) *alpha = 0;
        return;
    case 5: { /* 4x4 compressed */
        u32 blk = (u32)((t >> 2) * (w >> 2) + (s >> 2));
        u32 word = (u32)TexHalf(base + blk * 4) | (u32)TexHalf(base + blk * 4 + 2) << 16;
        int v = (word >> (((t & 3) * 4 + (s & 3)) * 2)) & 3;
        u32 pidx = 0x20000 + ((base & 0x1FFFF) >> 1) + (base >= 0x40000 ? 0x10000 : 0) + blk * 2;
        u16 pe = TexHalf(pidx);
        u32 pa = pltt * 16 + (pe & 0x3FFF) * 4;
        int mode = pe >> 14;
        u16 c0 = PalColor(pa), c1 = PalColor(pa + 2);
        switch (v) {
        case 0: *col = c0; return;
        case 1: *col = c1; return;
        case 2:
            if (mode == 1) *col = Mix(c0, c1, 1, 1, 1);
            else if (mode == 3) *col = Mix(c0, c1, 5, 3, 3);
            else *col = PalColor(pa + 4);
            return;
        default:
            if (mode == 2) *col = PalColor(pa + 6);
            else if (mode == 3) *col = Mix(c0, c1, 3, 5, 3);
            else *alpha = 0;
            return;
        }
    }
    case 6: { /* A5I3 */
        u8 b = TexByte(base + texel);
        *col = PalColor(pltt * 16 + (b & 7) * 2);
        *alpha = b >> 3;
        return;
    }
    case 7: { /* direct colour */
        u16 c = TexHalf(base + texel * 2);
        *col = c & 0x7FFF;
        *alpha = (c & 0x8000) ? 31 : 0;
        return;
    }
    default:
        *col = 0x7FFF;
        return;
    }
}

/* ---- per-pixel ---- */
typedef struct {
    u32 attr, tp, pltt, disp3d, alphaRef, polyAlpha, id, mode;
    int textured, translucent, wbuffer;
    const u16 *toon;
} PolyCtx;

static inline int Mod(int a, int b) { return ((a + 1) * (b + 1) - 1) >> 5; }

static void Pixel(const PolyCtx *c, int i, float depth, float r, float g, float b, float s, float t) {
    float old = sDepth[i];
    if (c->attr & (1u << 14)) { if (fabsf(depth - old) > 0x200) return; }
    else if (!(depth < old)) return;

    int vr = (int)(r + 0.5f), vg = (int)(g + 0.5f), vb = (int)(b + 0.5f);
    vr = vr < 0 ? 0 : vr > 31 ? 31 : vr;
    vg = vg < 0 ? 0 : vg > 31 ? 31 : vg;
    vb = vb < 0 ? 0 : vb > 31 ? 31 : vb;
    int va = c->polyAlpha;
    u16 tc = 0x7FFF;
    int ta = 31;
    if (c->textured) TexFetch(c->tp, c->pltt, (int)floorf(s), (int)floorf(t), &tc, &ta);
    int tr = tc & 31, tg = (tc >> 5) & 31, tb = (tc >> 10) & 31;
    int R, G, B, A;
    switch (c->mode) {
    case 1: /* decal */
        if (ta == 0) { R = vr; G = vg; B = vb; }
        else if (ta == 31) { R = tr; G = tg; B = tb; }
        else { R = (tr * ta + vr * (31 - ta)) >> 5; G = (tg * ta + vg * (31 - ta)) >> 5; B = (tb * ta + vb * (31 - ta)) >> 5; }
        A = va;
        break;
    case 2: { /* toon / highlight: table indexed by the vertex red level */
        u16 tt = c->toon[vr];
        int sr = tt & 31, sg = (tt >> 5) & 31, sb = (tt >> 10) & 31;
        if (c->disp3d & 2) { /* highlight */
            R = Mod(tr, vr) + sr; G = Mod(tg, vr) + sg; B = Mod(tb, vr) + sb;
            if (R > 31) R = 31;
            if (G > 31) G = 31;
            if (B > 31) B = 31;
        } else {
            R = Mod(tr, sr); G = Mod(tg, sg); B = Mod(tb, sb);
        }
        A = Mod(ta, va);
        break;
    }
    default: /* modulate */
        R = Mod(tr, vr); G = Mod(tg, vg); B = Mod(tb, vb);
        A = Mod(ta, va);
        break;
    }
    if (A == 0) return;
    if ((c->disp3d & 4) && A <= (int)c->alphaRef) return; /* alpha test */

    u16 col = (u16)(R | G << 5 | B << 10);
    if (A < 31) { /* translucent pixel */
        if (sTransId[i] == c->id) return;
        int da = sAlpha[i];
        if ((c->disp3d & 8) && da > 0) {
            sColor[i] = Mix(col, sColor[i], A + 1, 31 - A, 5);
            sAlpha[i] = (u8)(A > da ? A : da);
        } else {
            sColor[i] = col;
            sAlpha[i] = (u8)A;
        }
        sTransId[i] = (u8)c->id;
        sFog[i] &= (c->attr >> 15) & 1;
        if (c->attr & (1u << 11)) sDepth[i] = depth;
    } else {
        sColor[i] = col;
        sAlpha[i] = 31;
        sDepth[i] = depth;
        sTransId[i] = 0xFF;
        sFog[i] = (c->attr >> 15) & 1;
    }
}

/* Scanline fill of a convex polygon with perspective-correct attributes. */
static void Raster(const PcG3Poly *p, const PolyCtx *c) {
    int n = p->n;
    float iw[10], ar[10], ag[10], ab[10], as[10], at[10], dz[10];
    for (int k = 0; k < n; k++) {
        const PcG3Vertex *v = &p->v[k];
        iw[k] = 1.0f / v->w;
        ar[k] = v->r * iw[k]; ag[k] = v->g * iw[k]; ab[k] = v->b * iw[k];
        as[k] = v->s * iw[k]; at[k] = v->t * iw[k];
        dz[k] = v->z;
    }
    int y0 = (int)ceilf(p->ytop - 0.5f), y1 = (int)ceilf(p->ybottom - 0.5f);
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    int wire = ((c->attr >> 16) & 31) == 0;
    for (int y = y0; y < y1; y++) {
        float yc = y + 0.5f;
        float xl = 1e9f, xr = -1e9f, L[7] = { 0 }, Rr[7] = { 0 };
        for (int k = 0; k < n; k++) {
            int j = (k + 1) % n;
            float ya = p->v[k].y, yb = p->v[j].y;
            if ((yc < ya) == (yc < yb)) continue;
            float tt = (yc - ya) / (yb - ya);
            float x = p->v[k].x + (p->v[j].x - p->v[k].x) * tt;
            float a[7] = {
                iw[k] + (iw[j] - iw[k]) * tt, ar[k] + (ar[j] - ar[k]) * tt, ag[k] + (ag[j] - ag[k]) * tt,
                ab[k] + (ab[j] - ab[k]) * tt, as[k] + (as[j] - as[k]) * tt, at[k] + (at[j] - at[k]) * tt,
                dz[k] + (dz[j] - dz[k]) * tt,
            };
            if (x < xl) { xl = x; memcpy(L, a, sizeof a); }
            if (x > xr) { xr = x; memcpy(Rr, a, sizeof a); }
        }
        if (xr < xl) continue;
        int x0 = (int)ceilf(xl - 0.5f), x1 = (int)ceilf(xr - 0.5f);
        if (x1 == x0) x1 = x0 + 1; /* thin polygons still cover a pixel */
        float span = xr - xl > 1e-6f ? xr - xl : 1e-6f;
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < W; x++) {
            if (wire && x != x0 && x != x1 - 1 && y != y0 && y != y1 - 1) continue;
            float tt = ((x + 0.5f) - xl) / span;
            if (tt < 0) tt = 0;
            if (tt > 1) tt = 1;
            float a[7];
            for (int q = 0; q < 7; q++) a[q] = L[q] + (Rr[q] - L[q]) * tt;
            float wv = 1.0f / a[0];
            float depth = c->wbuffer ? wv : a[6];
            Pixel(c, y * W + x, depth, a[1] * wv, a[2] * wv, a[3] * wv, a[4] * wv, a[5] * wv);
        }
    }
}

static int CmpTranslucent(const void *a, const void *b) {
    const PcG3Poly *p = *(PcG3Poly *const *)a, *q = *(PcG3Poly *const *)b;
    if (p->ybottom != q->ybottom) return p->ybottom < q->ybottom ? -1 : 1;
    if (p->ytop != q->ytop) return p->ytop < q->ytop ? -1 : 1;
    return p < q ? -1 : 1; /* stable */
}

static void FogPass(u16 disp3d, int wbuffer) {
    (void)wbuffer;
    u32 fc = IO32(0x358);
    u16 fogCol = fc & 0x7FFF;
    int fogA = (fc >> 16) & 31, alphaOnly = disp3d & 0x40;
    int offset = IO16(0x35C) & 0x7FFF, step = 0x400 >> ((disp3d >> 8) & 15);
    const u8 *table = (const u8 *)0x04000360;
    if (step == 0) step = 1;
    for (int i = 0; i < W * H; i++) {
        if (!sFog[i]) continue;
        int d15 = (int)(sDepth[i] / 512.0f);
        float pos = (float)(d15 - offset) / step - 1.0f;
        int dens;
        if (pos <= 0) dens = table[0] & 127;
        else if (pos >= 31) dens = table[31] & 127;
        else {
            int k = (int)pos;
            float f = pos - k;
            dens = (int)((table[k] & 127) * (1 - f) + (table[k + 1] & 127) * f);
        }
        if (dens >= 127) dens = 128;
        if (!alphaOnly) sColor[i] = Mix(fogCol, sColor[i], dens, 128 - dens, 7);
        sAlpha[i] = (u8)((fogA * dens + sAlpha[i] * (128 - dens)) >> 7);
    }
}

void pc_g3r_render(PcG3Poly *polys, int count, BOOL manualSort, BOOL wbuffer) {
    u16 disp3d = IO16(0x060);
    u32 cc = IO32(0x350);
    u16 cd = IO16(0x354) & 0x7FFF;
    float clearDepth = (float)(cd * 0x200 + (cd == 0x7FFF ? 0x1FF : 0));
    for (int i = 0; i < W * H; i++) {
        sColor[i] = cc & 0x7FFF;
        sAlpha[i] = (cc >> 16) & 31;
        sDepth[i] = clearDepth;
        sTransId[i] = 0xFF;
        sFog[i] = (cc >> 15) & 1;
    }
    static PcG3Poly *order[2048];
    int no = 0, nt = 0;
    for (int i = 0; i < count; i++) if (!polys[i].translucent) order[no++] = &polys[i];
    for (int i = 0; i < count; i++) if (polys[i].translucent) order[no + nt++] = &polys[i];
    if (!manualSort) qsort(order + no, nt, sizeof order[0], CmpTranslucent);

    PolyCtx c;
    c.disp3d = disp3d;
    c.alphaRef = IO16(0x340) & 31;
    c.toon = (const u16 *)0x04000380;
    c.wbuffer = wbuffer;
    for (int i = 0; i < no + nt; i++) {
        PcG3Poly *p = order[i];
        c.attr = p->attr;
        c.mode = (p->attr >> 4) & 3;
        if (c.mode == 3) continue; // shortcut: shadow polygons not drawn
        c.tp = p->texParam;
        c.pltt = p->plttBase;
        c.textured = (disp3d & 1) && ((p->texParam >> 26) & 7) != 0;
        c.polyAlpha = (p->attr >> 16) & 31;
        if (c.polyAlpha == 0) c.polyAlpha = 31; /* wireframe: edges drawn opaque */
        c.id = (p->attr >> 24) & 63;
        c.translucent = p->translucent;
        if (wbuffer) /* W-buffer compares raw w (20.12) */
            for (int k = 0; k < p->n; k++) p->v[k].z = p->v[k].w;
        Raster(p, &c);
    }
    if (disp3d & 0x80) FogPass(disp3d, wbuffer);
}
