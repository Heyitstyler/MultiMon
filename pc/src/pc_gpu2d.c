/* Software renderer for the two DS 2D engines.
 *
 * Reads the hardware state the game wrote into the real DS address map (IO registers, palette,
 * VRAM views, OAM) and composes one frame per engine, scanline by scanline, like the DS PPU.
 * Covers BG modes 0-5 (text, affine, extended affine/bitmap), tiled and bitmap OBJ (incl. affine),
 * extended palettes, windows 0/1/OBJ, alpha/brightness blending and master brightness.
 * Behaviour follows GBATEK ("DS Video").
 *
 * The HBlank handler the game registered runs after each line, so per-line register effects work.
 */
#include <string.h>
#include <nitro.h>
#include "pc_platform.h"

#define W 256
#define H 192
#define OPAQUE 0x8000 /* in line buffers: colour | OPAQUE, 0 = transparent */

typedef struct {
    int id;          /* 0 = engine A (main), 1 = engine B (sub) */
    u8 *io;          /* 0x04000000 / 0x04001000 */
    const u16 *bgPal, *objPal;
    const u8 *oam;
    const u8 *bgVram;
    u32 bgMask;      /* BG window size - 1 (mirrors) */
    const u8 *objVram;
    u32 objMask;
    s32 refX[2], refY[2]; /* internal affine reference points of BG2, BG3 */
} Engine;

static Engine sEng[2];
static u32 sOut[2][W * H]; /* XRGB8888 per engine */

/* Per-line scratch. */
static u16 sBg[4][W];
static u16 sObj[W];
static u8 sObjPrio[W];
static u8 sObjAlpha[W];   /* 0 = normal, 1..16 = semi-transparent EVA (16 = use BLDALPHA) */
static u8 sObjWin[W];

static inline u16 Io16(const Engine *e, u32 off) { return *(const u16 *)(e->io + off); }
static inline u32 Io32(const Engine *e, u32 off) { return *(const u32 *)(e->io + off); }
static inline u8 BgByte(const Engine *e, u32 a) { return e->bgVram[a & e->bgMask]; }
static inline u16 BgHalf(const Engine *e, u32 a) { return *(const u16 *)(e->bgVram + (a & e->bgMask & ~1u)); }
static inline u8 ObjByte(const Engine *e, u32 a) { return e->objVram[a & e->objMask]; }
static inline u16 ObjHalf(const Engine *e, u32 a) { return *(const u16 *)(e->objVram + (a & e->objMask & ~1u)); }
static inline s32 Sext28(u32 v) { return (s32)(v << 4) >> 4; }

static u16 ExtBgColor(const Engine *e, int slot, u32 index) {
    const u16 *p = pc_vram_bg_ext_pltt(e->id, slot);
    return p ? p[index & 0xFFF] : 0;
}

/* ---- backgrounds ---- */

static void TextBg(const Engine *e, int bg, int y, u32 dispcnt) {
    u16 cnt = Io16(e, 0x08 + bg * 2);
    u32 hofs = Io16(e, 0x10 + bg * 4) & 0x1FF, vofs = Io16(e, 0x12 + bg * 4) & 0x1FF;
    u32 wide = (cnt >> 14) & 1, tall = (cnt >> 15) & 1;
    u32 wmask = wide ? 511 : 255, hmask = tall ? 511 : 255;
    u32 charBase = ((cnt >> 2) & 15) * 0x4000 + (e->id == 0 ? ((dispcnt >> 24) & 7) * 0x10000 : 0);
    u32 scrBase = ((cnt >> 8) & 31) * 0x800 + (e->id == 0 ? ((dispcnt >> 27) & 7) * 0x10000 : 0);
    int c256 = cnt & 0x80, ext = c256 && (dispcnt & (1u << 30));
    int slot = bg < 2 && (cnt & 0x2000) ? bg + 2 : bg;
    u32 py = (y + vofs) & hmask;
    for (int x = 0; x < W; x++) {
        u32 px = (x + hofs) & wmask;
        u32 block = (px >> 8) + (py >> 8) * (wide ? 2 : 1);
        u16 ent = BgHalf(e, scrBase + block * 0x800 + ((py & 255) >> 3) * 64 + ((px & 255) >> 3) * 2);
        u32 tx = (ent & 0x400) ? 7 - (px & 7) : px & 7, ty = (ent & 0x800) ? 7 - (py & 7) : py & 7;
        u32 tile = ent & 0x3FF, pal = ent >> 12, idx;
        if (c256) {
            idx = BgByte(e, charBase + tile * 64 + ty * 8 + tx);
            if (idx) sBg[bg][x] = (ext ? ExtBgColor(e, slot, pal * 256 + idx) : e->bgPal[idx]) | OPAQUE;
        } else {
            u8 b = BgByte(e, charBase + tile * 32 + ty * 4 + tx / 2);
            idx = (tx & 1) ? b >> 4 : b & 15;
            if (idx) sBg[bg][x] = e->bgPal[pal * 16 + idx] | OPAQUE;
        }
    }
}

/* Affine BGs (bg 2 or 3). kind: 0 = 8-bit map, 1 = 16-bit map (ext), 2 = 256-colour bitmap,
 * 3 = direct-colour bitmap. */
static void AffineBg(Engine *e, int bg, u32 dispcnt, int kind) {
    u16 cnt = Io16(e, 0x08 + bg * 2);
    u32 r = 0x20 + (bg - 2) * 0x10;
    s32 pa = (s16)Io16(e, r), pc = (s16)Io16(e, r + 4);
    int a = bg - 2;
    u32 sz = (cnt >> 14) & 3, wrap = cnt & 0x2000;
    u32 w, h;
    if (kind <= 1) {
        w = h = 128u << sz;
    } else {
        static const u16 bw[4] = { 128, 256, 512, 512 }, bh[4] = { 128, 256, 256, 512 };
        w = bw[sz];
        h = bh[sz];
    }
    u32 charBase = ((cnt >> 2) & 15) * 0x4000 + (e->id == 0 ? ((dispcnt >> 24) & 7) * 0x10000 : 0);
    u32 scrBase = ((cnt >> 8) & 31) * 0x800 + (e->id == 0 ? ((dispcnt >> 27) & 7) * 0x10000 : 0);
    u32 bmpBase = ((cnt >> 8) & 31) * 0x4000;
    int ext = dispcnt & (1u << 30);
    s32 fx = e->refX[a], fy = e->refY[a];
    for (int x = 0; x < W; x++, fx += pa, fy += pc) {
        s32 tx = fx >> 8, ty = fy >> 8;
        if (wrap) {
            tx &= w - 1;
            ty &= h - 1;
        } else if (tx < 0 || ty < 0 || (u32)tx >= w || (u32)ty >= h) {
            continue;
        }
        u16 col;
        if (kind == 0) {
            u8 tile = BgByte(e, scrBase + (ty >> 3) * (w >> 3) + (tx >> 3));
            u8 idx = BgByte(e, charBase + tile * 64 + (ty & 7) * 8 + (tx & 7));
            if (!idx) continue;
            col = e->bgPal[idx];
        } else if (kind == 1) {
            u16 ent = BgHalf(e, scrBase + ((ty >> 3) * (w >> 3) + (tx >> 3)) * 2);
            u32 px = (ent & 0x400) ? 7 - (tx & 7) : tx & 7, py = (ent & 0x800) ? 7 - (ty & 7) : ty & 7;
            u8 idx = BgByte(e, charBase + (ent & 0x3FF) * 64 + py * 8 + px);
            if (!idx) continue;
            col = ext ? ExtBgColor(e, bg, (ent >> 12) * 256 + idx) : e->bgPal[idx];
        } else if (kind == 2) {
            u8 idx = BgByte(e, bmpBase + ty * w + tx);
            if (!idx) continue;
            col = e->bgPal[idx];
        } else {
            col = BgHalf(e, bmpBase + (ty * w + tx) * 2);
            if (!(col & 0x8000)) continue;
        }
        sBg[bg][x] = (col & 0x7FFF) | OPAQUE;
    }
}

/* ---- objects ---- */

static const u8 sObjW[4][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 }, { 0 } };
static const u8 sObjH[4][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 }, { 0 } };

static void Objects(const Engine *e, int y, u32 dispcnt) {
    const u16 *extPal = pc_vram_obj_ext_pltt(e->id);
    int ext = (dispcnt & (1u << 31)) && extPal != NULL;
    for (int i = 0; i < 128; i++) {
        const u16 *o = (const u16 *)(e->oam + i * 8);
        u16 a0 = o[0], a1 = o[1], a2 = o[2];
        int affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue; /* hidden */
        int mode = (a0 >> 10) & 3, shape = a0 >> 14, size = a1 >> 14;
        if (shape == 3) continue;
        int w = sObjW[shape][size], h = sObjH[shape][size];
        int bw = w, bh = h;
        if (affine && (a0 & 0x200)) { bw *= 2; bh *= 2; } /* double-size */
        int iy = (y - (a0 & 0xFF)) & 0xFF;
        if (iy >= bh) continue;
        int ox = a1 & 0x1FF;
        if (ox >= 256) ox -= 512;
        int prio = (a2 >> 10) & 3;
        s32 pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            const u16 *p = (const u16 *)(e->oam + ((a1 >> 9) & 31) * 32);
            pa = (s16)p[3]; pb = (s16)p[7]; pc = (s16)p[11]; pd = (s16)p[15];
        }
        int c256 = a0 & 0x2000;
        u32 tile = a2 & 0x3FF;
        for (int ix = 0; ix < bw; ix++) {
            int sx = ox + ix;
            if (sx < 0 || sx >= W) continue;
            if (mode != 2 && sObj[sx] && sObjPrio[sx] <= prio) continue; /* lower index wins ties */
            int tx, ty;
            if (affine) {
                int cx = ix - bw / 2, cy = iy - bh / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - ix : ix;
                ty = (a1 & 0x2000) ? h - 1 - iy : iy;
            }
            u16 col;
            if (mode == 3) { /* bitmap OBJ */
                if ((a2 >> 12) == 0) break;
                u32 base, stride;
                if (dispcnt & 0x40) {
                    base = tile * (128u << ((dispcnt >> 22) & 1));
                    stride = w * 2;
                } else if (dispcnt & 0x20) {
                    base = (tile & 0x1F) * 0x10 + (tile & 0x3E0) * 0x80;
                    stride = 512;
                } else {
                    base = (tile & 0x0F) * 0x10 + (tile & 0x3F0) * 0x80;
                    stride = 256;
                }
                col = ObjHalf(e, base + ty * stride + tx * 2);
                if (!(col & 0x8000)) continue;
                col &= 0x7FFF;
            } else {
                u32 tbytes = c256 ? 64 : 32, addr;
                if (dispcnt & 0x10) { /* 1D mapping */
                    addr = tile * (32u << ((dispcnt >> 20) & 3)) + ((ty >> 3) * (w >> 3) + (tx >> 3)) * tbytes;
                } else {
                    addr = tile * 32 + (ty >> 3) * 1024 + (tx >> 3) * tbytes;
                }
                u32 idx;
                if (c256) {
                    idx = ObjByte(e, addr + (ty & 7) * 8 + (tx & 7));
                    if (!idx) continue;
                    col = ext ? extPal[((a2 >> 12) * 256 + idx) & 0xFFF] : e->objPal[idx];
                } else {
                    u8 b = ObjByte(e, addr + (ty & 7) * 4 + (tx & 7) / 2);
                    idx = (tx & 1) ? b >> 4 : b & 15;
                    if (!idx) continue;
                    col = e->objPal[(a2 >> 12) * 16 + idx];
                }
            }
            if (mode == 2) {
                sObjWin[sx] = 1;
                continue;
            }
            sObj[sx] = (col & 0x7FFF) | OPAQUE;
            sObjPrio[sx] = prio;
            sObjAlpha[sx] = mode == 1 ? 16 : mode == 3 ? (a2 >> 12) + 1 : 0;
        }
    }
}

/* ---- composition ---- */

static inline u16 Blend(u16 a, u16 b, u32 eva, u32 evb) {
    u32 r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    u32 g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    u32 bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    return (u16)((r > 31 ? 31 : r) | (g > 31 ? 31 : g) << 5 | (bl > 31 ? 31 : bl) << 10);
}

static inline u16 Brighten(u16 c, u32 f, int up) {
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    if (up) { r += ((31 - r) * f) >> 4; g += ((31 - g) * f) >> 4; b += ((31 - b) * f) >> 4; }
    else { r -= (r * f) >> 4; g -= (g * f) >> 4; b -= (b * f) >> 4; }
    return (u16)(r | g << 5 | b << 10);
}

static inline u32 ToRgb(u16 c) {
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | r << 16 | g << 8 | b;
}

static int InRange(u32 v, u32 lo, u32 hi) { return lo <= hi ? v >= lo && v < hi : v >= lo || v < hi; }

static void Line(Engine *e, int y) {
    u32 dispcnt = Io32(e, 0x00);
    u32 *out = sOut[e->id] + y * W;
    u32 dmode = (dispcnt >> 16) & 3;
    if (e->id == 1) dmode &= 1;
    if (dmode == 0 || (dispcnt & 0x80)) { /* display off / forced blank: white */
        for (int x = 0; x < W; x++) out[x] = 0xFFFFFFFFu;
        return;
    }
    if (dmode == 2) { /* engine A VRAM display: a bank in the LCDC window as a 256x192 bitmap */
        const u16 *src = (const u16 *)(0x06800000 + ((dispcnt >> 18) & 3) * 0x20000) + y * W;
        for (int x = 0; x < W; x++) out[x] = ToRgb(src[x]);
        return;
    }
    if (dmode == 3) { /* main memory display FIFO: not used by this game */
        memset(out, 0, W * 4);
        return;
    }

    u32 bgmode = dispcnt & 7;
    memset(sBg, 0, sizeof sBg);
    int bg0is3d = e->id == 0 && (dispcnt & 8);
    u8 alpha3d[W];
    for (int bg = 0; bg < 4; bg++) {
        if (!(dispcnt & (0x100u << bg))) continue;
        if (bg == 0 && bg0is3d) { /* 3D layer, scrolled by BG0HOFS */
            const u16 *col = pc_g3r_color() + y * W;
            const u8 *al = pc_g3r_alpha() + y * W;
            int hofs = (s16)(Io16(e, 0x10) << 7) >> 7;
            for (int x = 0; x < W; x++) {
                int sx = x + hofs;
                alpha3d[x] = sx >= 0 && sx < W ? al[sx] : 0;
                if (alpha3d[x]) sBg[0][x] = (col[sx] & 0x7FFF) | OPAQUE;
            }
            continue;
        }
        if (bg < 2 || (bg == 2 && bgmode <= 1) || (bg == 3 && bgmode == 0)) {
            TextBg(e, bg, y, dispcnt);
        } else {
            int affine = (bg == 2 && (bgmode == 2 || bgmode == 4)) || (bg == 3 && (bgmode == 1 || bgmode == 2));
            int extended = (bg == 3 && bgmode >= 3) || (bg == 2 && bgmode == 5);
            if (affine) {
                AffineBg(e, bg, dispcnt, 0);
            } else if (extended) {
                u16 cnt = Io16(e, 0x08 + bg * 2);
                AffineBg(e, bg, dispcnt, !(cnt & 0x80) ? 1 : (cnt & 4) ? 3 : 2);
            } // shortcut: mode 6 large bitmap not drawn; HG does not use it.
        }
    }
    for (int a = 0; a < 2; a++) { /* advance affine reference points by PB/PD */
        e->refX[a] += (s16)Io16(e, 0x22 + a * 0x10);
        e->refY[a] += (s16)Io16(e, 0x26 + a * 0x10);
    }

    memset(sObj, 0, sizeof sObj);
    memset(sObjWin, 0, sizeof sObjWin);
    if (dispcnt & 0x1000) Objects(e, y, dispcnt);

    /* Window mask per pixel: bits 0-3 BG, 4 OBJ, 5 colour effects. */
    u8 winMask[W];
    if (dispcnt & 0xE000) {
        u16 winin = Io16(e, 0x48), winout = Io16(e, 0x4A);
        int in0 = (dispcnt & 0x2000) && InRange(y, Io16(e, 0x44) >> 8, Io16(e, 0x44) & 0xFF);
        int in1 = (dispcnt & 0x4000) && InRange(y, Io16(e, 0x46) >> 8, Io16(e, 0x46) & 0xFF);
        u16 h0 = Io16(e, 0x40), h1 = Io16(e, 0x42);
        for (int x = 0; x < W; x++) {
            if (in0 && InRange(x, h0 >> 8, h0 & 0xFF)) winMask[x] = winin & 0x3F;
            else if (in1 && InRange(x, h1 >> 8, h1 & 0xFF)) winMask[x] = (winin >> 8) & 0x3F;
            else if ((dispcnt & 0x8000) && sObjWin[x]) winMask[x] = (winout >> 8) & 0x3F;
            else winMask[x] = winout & 0x3F;
        }
    } else {
        memset(winMask, 0x3F, sizeof winMask);
    }

    u16 bldcnt = Io16(e, 0x50), bldalpha = Io16(e, 0x52);
    u32 eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = Io16(e, 0x54) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    u32 effect = (bldcnt >> 6) & 3;
    u16 backdrop = e->bgPal[0] & 0x7FFF;
    u8 bgPrio[4];
    for (int bg = 0; bg < 4; bg++) bgPrio[bg] = Io16(e, 0x08 + bg * 2) & 3;

    for (int x = 0; x < W; x++) {
        /* Find the top two visible layers. Layer ids: 0-3 BG, 4 OBJ, 5 backdrop. */
        int layer[2] = { 5, 5 }, n = 0;
        u16 col[2] = { backdrop, backdrop };
        for (int p = 0; p < 4 && n < 2; p++) {
            if (sObj[x] && sObjPrio[x] == p && (winMask[x] & 0x10)) {
                layer[n] = 4;
                col[n++] = sObj[x] & 0x7FFF;
            }
            for (int bg = 0; bg < 4 && n < 2; bg++) {
                if (sBg[bg][x] && bgPrio[bg] == p && (winMask[x] & (1 << bg))) {
                    layer[n] = bg;
                    col[n++] = sBg[bg][x] & 0x7FFF;
                }
            }
        }
        u16 c = col[0];
        int secondIsTarget = (bldcnt >> 8) & (1 << layer[1]);
        if (layer[0] == 0 && bg0is3d && alpha3d[x] < 31 && secondIsTarget) {
            /* translucent 3D pixel over a 2nd target: blends with its own alpha */
            u32 a = (alpha3d[x] + 1) >> 1;
            c = Blend(col[0], col[1], a, 16 - a);
        } else if (layer[0] == 4 && sObjAlpha[x] && secondIsTarget) {
            /* semi-transparent / bitmap OBJ: always alpha, overrides the effect mode */
            u32 a = sObjAlpha[x] == 16 ? eva : sObjAlpha[x], b = sObjAlpha[x] == 16 ? evb : 16 - sObjAlpha[x];
            c = Blend(col[0], col[1], a, b);
        } else if ((winMask[x] & 0x20) && (bldcnt & (1 << layer[0]))) {
            if (effect == 1 && secondIsTarget) c = Blend(col[0], col[1], eva, evb);
            else if (effect == 2) c = Brighten(c, evy, 1);
            else if (effect == 3) c = Brighten(c, evy, 0);
        }
        out[x] = c; /* converted after master brightness */
    }

    u16 mb = Io16(e, 0x6C);
    u32 mbMode = (mb >> 14) & 3, mbF = mb & 31;
    if (mbF > 16) mbF = 16;
    for (int x = 0; x < W; x++) {
        u16 c = (u16)out[x];
        if (mbF && (mbMode == 1 || mbMode == 2)) c = Brighten(c, mbF, mbMode == 1);
        out[x] = ToRgb(c);
    }
}

static void InitEngine(Engine *e, int id) {
    e->id = id;
    e->io = (u8 *)(id ? 0x04001000 : 0x04000000);
    e->bgPal = (const u16 *)(id ? 0x05000400 : 0x05000000);
    e->objPal = e->bgPal + 0x100;
    e->oam = (const u8 *)(id ? 0x07000400 : 0x07000000);
    e->bgVram = (const u8 *)(id ? 0x06200000 : 0x06000000);
    e->bgMask = id ? 0x1FFFF : 0x7FFFF;
    e->objVram = (const u8 *)(id ? 0x06600000 : 0x06400000);
    e->objMask = id ? 0x1FFFF : 0x3FFFF;
}

/* Render one frame of both engines. top/bottom receive XRGB8888 256x192 images; POWCNT1 bit 15
 * decides which engine goes to which screen. */
void pc_gpu2d_render(u32 *top, u32 *bottom) {
    if (sEng[0].io == NULL) {
        InitEngine(&sEng[0], 0);
        InitEngine(&sEng[1], 1);
    }
    for (int i = 0; i < 2; i++) { /* reference points latch at the start of the frame */
        Engine *e = &sEng[i];
        for (int a = 0; a < 2; a++) {
            e->refX[a] = Sext28(Io32(e, 0x28 + a * 0x10));
            e->refY[a] = Sext28(Io32(e, 0x2C + a * 0x10));
        }
    }
    vu16 *vcount = (vu16 *)0x04000006, *dispstat = (vu16 *)0x04000004;
    for (int y = 0; y < H; y++) {
        *vcount = (u16)y;
        Line(&sEng[0], y);
        Line(&sEng[1], y);
        /* HBlank DMA and IRQ between lines (scanline effects). */
        pc_dma_hblank();
        OSIrqFunction hblank = OS_IRQTable[1];
        if (hblank != NULL && (*dispstat & 0x10) && (*(vu32 *)0x04000210 & OS_IE_H_BLANK)) hblank();
    }
    *vcount = 192;
    int aOnTop = (*(vu16 *)0x04000304 >> 15) & 1;
    memcpy(aOnTop ? top : bottom, sOut[0], sizeof sOut[0]);
    memcpy(aOnTop ? bottom : top, sOut[1], sizeof sOut[1]);
}
