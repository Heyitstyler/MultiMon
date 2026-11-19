/* VRAM banks A-I and their CPU-visible views.
 *
 * On the DS, VRAM is nine physical banks. VRAMCNT_A..I decide where each bank appears: the LCDC
 * window (0x06800000), BG/OBJ windows of engine A or B, or invisible slots (extended palettes,
 * textures). Games move banks around, for example map a bank to LCDC, copy an extended palette
 * in, then map it back to its palette slot. So the data must belong to the bank, not the address.
 *
 * Host model: each bank's data lives at exactly one place in the DS range: its current BG/OBJ view,
 * or else its LCDC window slot (0x06800000 + offset), which also serves the invisible slots (extended
 * palettes, textures) exposed to the renderer as pointers (pc_vram_bg_ext_pltt etc.). When VRAMCNT
 * changes, every bank's data is copied from its old place to its new one. (Aliasing by mapping views
 * is not portable: Windows maps views only at 64 KB granularity, banks F/G/I sit at 16 KB.)
 *
 * pc_vram_sync() re-reads VRAMCNT and moves the banks. It runs after every function in the SDK files that
 * write VRAMCNT (gx_vramcnt.c, gxstate.c), which pc_build.py compiles with -finstrument-functions.
 */
#include <string.h>
#include <nitro.h>
#include "pc_platform.h"

#define LCDC_BASE   0x06800000u
#define LCDC_SIZE   0x000A4000u

enum { BANK_A, BANK_B, BANK_C, BANK_D, BANK_E, BANK_F, BANK_G, BANK_H, BANK_I, NUM_BANKS };

static const u32 sBankOffset[NUM_BANKS] = { 0x00000, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000 };
static const u32 sBankSize[NUM_BANKS]   = { 0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000 };
static const u32 sCntAddr[NUM_BANKS]    = { 0x04000240, 0x04000241, 0x04000242, 0x04000243, 0x04000244,
                                            0x04000245, 0x04000246, 0x04000248, 0x04000249 };

static u8 sCnt[NUM_BANKS];
static u8 *sLoc[NUM_BANKS]; /* where each bank's data is now */
static u8 *sBgExtPltt[2][4]; /* [engine][slot], 8 KB each */
static u8 *sObjExtPltt[2];   /* [engine], 8 KB */
static u8 *sTexSlot[4];      /* 3D texture image slots, 128 KB each */
static u8 *sTexPlttSlot[6];  /* 3D texture palette slots, 16 KB each */

static u8 *Bank(int b) { return (u8 *)(LCDC_BASE + sBankOffset[b]); }

/* CPU view address of a bank for its current VRAMCNT value, or 0 if it has none.
 * Also records extended palette slots. Values from GBATEK "DS Memory Control - VRAM". */
static u32 ViewOf(int b, u8 cnt) {
    u32 mst = cnt & (b <= BANK_B ? 3 : 7), ofs = (cnt >> 3) & 3;
    switch (b) {
    case BANK_A: case BANK_B: case BANK_C: case BANK_D:
        if (mst == 1) return 0x06000000 + 0x20000 * ofs;
        if (mst == 2 && b <= BANK_B) return 0x06400000 + 0x20000 * (ofs & 1);
        if (mst == 4 && b == BANK_C) return 0x06200000;
        if (mst == 4 && b == BANK_D) return 0x06600000;
        if (mst == 3) sTexSlot[ofs] = Bank(b);
        return 0; /* LCDC, ARM7 */
    case BANK_E:
        if (mst == 1) return 0x06000000;
        if (mst == 2) return 0x06400000;
        if (mst == 3) for (int s = 0; s < 4; s++) sTexPlttSlot[s] = Bank(b) + 0x4000 * s;
        if (mst == 4) for (int s = 0; s < 4; s++) sBgExtPltt[0][s] = Bank(b) + 0x2000 * s;
        if (mst == 5) sObjExtPltt[0] = Bank(b);
        return 0;
    case BANK_F: case BANK_G:
        if (mst == 1) return 0x06000000 + 0x4000 * (ofs & 1) + 0x10000 * (ofs >> 1);
        if (mst == 2) return 0x06400000 + 0x4000 * (ofs & 1) + 0x10000 * (ofs >> 1);
        if (mst == 3) sTexPlttSlot[(ofs & 1) + (ofs >> 1) * 4] = Bank(b);
        if (mst == 4) {
            sBgExtPltt[0][(ofs & 1) * 2] = Bank(b);
            sBgExtPltt[0][(ofs & 1) * 2 + 1] = Bank(b) + 0x2000;
        }
        if (mst == 5) sObjExtPltt[0] = Bank(b);
        return 0;
    case BANK_H:
        if (mst == 1) return 0x06200000;
        if (mst == 2) for (int s = 0; s < 4; s++) sBgExtPltt[1][s] = Bank(b) + 0x2000 * s;
        return 0;
    case BANK_I:
        if (mst == 1) return 0x06208000;
        if (mst == 2) return 0x06600000;
        if (mst == 3) sObjExtPltt[1] = Bank(b);
        return 0;
    }
    return 0;
}

void pc_vram_init(void) {
    for (int b = 0; b < NUM_BANKS; b++) sLoc[b] = Bank(b);
    memset(sCnt, 0xFF, sizeof sCnt); /* force the first sync to build every view */
    pc_vram_sync();
}

void pc_vram_sync(void) {
    static u8 sSave[LCDC_SIZE];
    u8 cnt[NUM_BANKS];
    for (int b = 0; b < NUM_BANKS; b++) cnt[b] = *(vu8 *)sCntAddr[b];
    if (sLoc[0] == NULL || memcmp(cnt, sCnt, sizeof cnt) == 0) return;
    memcpy(sCnt, cnt, sizeof cnt);
    memset(sBgExtPltt, 0, sizeof sBgExtPltt);
    memset(sObjExtPltt, 0, sizeof sObjExtPltt);
    memset(sTexSlot, 0, sizeof sTexSlot);
    memset(sTexPlttSlot, 0, sizeof sTexPlttSlot);
    // Lift every bank out, clear the views it leaves (hardware shows nothing there), then place it.
    // shortcut: writes to window space no bank backs persist until a bank lands there; hardware drops them.
    for (int b = 0; b < NUM_BANKS; b++) memcpy(sSave + sBankOffset[b], sLoc[b], sBankSize[b]);
    for (int b = 0; b < NUM_BANKS; b++) {
        if (sLoc[b] != Bank(b)) memset(sLoc[b], 0, sBankSize[b]);
    }
    for (int b = 0; b < NUM_BANKS; b++) {
        u32 view = (cnt[b] & 0x80) ? ViewOf(b, cnt[b]) : 0; /* disabled banks keep their data in LCDC */
        sLoc[b] = view != 0 ? (u8 *)view : Bank(b);
        memcpy(sLoc[b], sSave + sBankOffset[b], sBankSize[b]);
    }
}

const u16 *pc_vram_bg_ext_pltt(int engine, int slot) { return (const u16 *)sBgExtPltt[engine][slot]; }
const u16 *pc_vram_obj_ext_pltt(int engine) { return (const u16 *)sObjExtPltt[engine]; }
const u8 *pc_vram_tex_slot(int slot) { return sTexSlot[slot]; }
const u8 *pc_vram_texpltt_slot(int slot) { return sTexPlttSlot[slot]; }

/* -finstrument-functions hooks, used only by the SDK files that write VRAMCNT. */
__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *site) { (void)fn; (void)site; }
__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *site) {
    (void)fn;
    (void)site;
    pc_vram_sync();
}
