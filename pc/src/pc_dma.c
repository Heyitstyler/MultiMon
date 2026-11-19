/* Host DMA. The SDK programs four DMA channels through MIi_DmaSetParams*; on the host those
 * registers do nothing, so every transfer is performed here instead:
 *   - immediate and geometry-FIFO timing: at once (FIFO words go to the geometry engine);
 *   - VBlank / HBlank timing: kept per channel and run by pc_dma_vblank / pc_dma_hblank, which the
 *     renderer calls at those points (scanline effects such as wavy or window-shape tables).
 * The MI_Dma* front ends are host versions too, so async callbacks run before the call returns. */
#include <string.h>
#include <nitro.h>
#include "pc_platform.h"

#define CNT_ENABLE  0x80000000u
#define CNT_32BIT   0x04000000u
#define CNT_REPEAT  0x02000000u

typedef struct { u32 src, dest, ctrl; } Channel;
static Channel sCh[4];

/* One block transfer of `count` units; returns the updated src/dest per the address controls. */
static void Transfer(Channel *c) {
    u32 count = c->ctrl & 0x1FFFFF, unit = (c->ctrl & CNT_32BIT) ? 4 : 2;
    u32 dctl = (c->ctrl >> 21) & 3, sctl = (c->ctrl >> 23) & 3;
    s32 dstep = dctl == 1 ? -(s32)unit : dctl == 2 ? 0 : (s32)unit;
    s32 sstep = sctl == 1 ? -(s32)unit : sctl == 2 ? 0 : (s32)unit;
    if (count == 0) count = 0x200000;
    u32 s = c->src, d = c->dest;
    BOOL fifo = pc_g3_is_port((void *)d) || ((c->ctrl >> 27) & 7) == 7;
    for (u32 i = 0; i < count; i++, s += sstep, d += dstep) {
        if (fifo) pc_g3_send((const u32 *)s, 1);
        else if (unit == 4) *(vu32 *)d = *(vu32 *)s;
        else *(vu16 *)d = *(vu16 *)s;
    }
    c->src = s;
    if (dctl != 3) c->dest = d; /* 3 = increment and reload */
}

static void SetParams(u32 dmaNo, u32 src, u32 dest, u32 ctrl) {
    Channel *c = &sCh[dmaNo & 3];
    c->src = src;
    c->dest = dest;
    c->ctrl = ctrl;
    if (!(ctrl & CNT_ENABLE)) return;
    u32 timing = (ctrl >> 27) & 7;
    if (timing == 0 || timing == 7) { /* immediate; geometry FIFO runs as fast as it is fed */
        Transfer(c);
        c->ctrl &= ~CNT_ENABLE;
    }
    // shortcut: card, main-memory-display and GBA-slot timings are not used by HG.
}

void MIi_DmaSetParams(u32 dmaNo, u32 src, u32 dest, u32 ctrl) { SetParams(dmaNo, src, dest, ctrl); }
void MIi_DmaSetParams_wait(u32 dmaNo, u32 src, u32 dest, u32 ctrl) { SetParams(dmaNo, src, dest, ctrl); }
void MIi_DmaSetParams_noInt(u32 dmaNo, u32 src, u32 dest, u32 ctrl) { SetParams(dmaNo, src, dest, ctrl); }
void MIi_DmaSetParams_wait_noInt(u32 dmaNo, u32 src, u32 dest, u32 ctrl) { SetParams(dmaNo, src, dest, ctrl); }

static void RunTimed(u32 timing) {
    for (int i = 0; i < 4; i++) {
        Channel *c = &sCh[i];
        if (!(c->ctrl & CNT_ENABLE) || ((c->ctrl >> 27) & 7) != timing) continue;
        Transfer(c);
        if (!(c->ctrl & CNT_REPEAT)) c->ctrl &= ~CNT_ENABLE;
    }
}
void pc_dma_vblank(void) { RunTimed(1); }
void pc_dma_hblank(void) { RunTimed(2); }

BOOL MI_IsDmaBusy(u32 dmaNo) { return (sCh[dmaNo & 3].ctrl & CNT_ENABLE) != 0; }
void MI_WaitDma(u32 dmaNo) { (void)dmaNo; }
void MI_StopDma(u32 dmaNo) { sCh[dmaNo & 3].ctrl &= ~CNT_ENABLE; }

/* ---- front ends ---- */
static void Copy(const void *src, void *dest, u32 size) {
    if (pc_g3_is_port(dest)) pc_g3_send(src, size / 4); /* into GXFIFO */
    else if (pc_mem_copy_ok(src, dest, size)) memmove(dest, src, size);
}
static void Send(const void *src, volatile void *dest, u32 size, int unit) {
    if (pc_g3_is_port(dest)) { pc_g3_send(src, size / 4); return; }
    for (u32 i = 0; i < size / unit; i++) {
        if (unit == 4) *(vu32 *)dest = ((const u32 *)src)[i];
        else *(vu16 *)dest = ((const u16 *)src)[i];
    }
}
static void Fill32(void *dest, u32 data, u32 size) { u32 *d = dest; for (u32 i = 0; i < size / 4; i++) d[i] = data; }
static void Fill16(void *dest, u16 data, u32 size) { u16 *d = dest; for (u32 i = 0; i < size / 2; i++) d[i] = data; }
static void Done(MIDmaCallback cb, void *arg) { if (cb != NULL) cb(arg); }

void MI_DmaFill32(u32 n, void *d, u32 v, u32 size) { (void)n; Fill32(d, v, size); }
void MI_DmaFill16(u32 n, void *d, u16 v, u32 size) { (void)n; Fill16(d, v, size); }
void MI_DmaCopy32(u32 n, const void *s, void *d, u32 size) { (void)n; Copy(s, d, size); }
void MI_DmaCopy16(u32 n, const void *s, void *d, u32 size) { (void)n; Copy(s, d, size); }
void MI_DmaSend32(u32 n, const void *s, volatile void *d, u32 size) { (void)n; Send(s, d, size, 4); }
void MI_DmaSend16(u32 n, const void *s, volatile void *d, u32 size) { (void)n; Send(s, d, size, 2); }
void MIi_CardDmaCopy32(u32 n, const void *s, void *d, u32 size) { (void)n; memmove(d, s, size); }

void MI_DmaFill32Async(u32 n, void *d, u32 v, u32 size, MIDmaCallback cb, void *arg) { (void)n; Fill32(d, v, size); Done(cb, arg); }
void MI_DmaFill16Async(u32 n, void *d, u16 v, u32 size, MIDmaCallback cb, void *arg) { (void)n; Fill16(d, v, size); Done(cb, arg); }
void MI_DmaCopy32Async(u32 n, const void *s, void *d, u32 size, MIDmaCallback cb, void *arg) { (void)n; Copy(s, d, size); Done(cb, arg); }
void MI_DmaCopy16Async(u32 n, const void *s, void *d, u32 size, MIDmaCallback cb, void *arg) { (void)n; Copy(s, d, size); Done(cb, arg); }
void MI_DmaSend32Async(u32 n, const void *s, volatile void *d, u32 size, MIDmaCallback cb, void *arg) { (void)n; Send(s, d, size, 4); Done(cb, arg); }
void MI_DmaSend16Async(u32 n, const void *s, volatile void *d, u32 size, MIDmaCallback cb, void *arg) { (void)n; Send(s, d, size, 2); Done(cb, arg); }

/* Display lists into the geometry FIFO (commandLength in bytes). */
void MI_SendGXCommand(u32 n, const void *s, u32 len) { (void)n; pc_g3_send(s, len / 4); }
void MI_SendGXCommandFast(u32 n, const void *s, u32 len) { (void)n; pc_g3_send(s, len / 4); }
void MI_SendGXCommandAsync(u32 n, const void *s, u32 len, MIDmaCallback cb, void *arg) { (void)n; pc_g3_send(s, len / 4); Done(cb, arg); }
void MI_SendGXCommandAsyncFast(u32 n, const void *s, u32 len, MIDmaCallback cb, void *arg) { (void)n; pc_g3_send(s, len / 4); Done(cb, arg); }
