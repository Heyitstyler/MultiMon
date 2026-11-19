/* Host replacements for NitroSDK OS functions whose DS versions are ARM asm or need the ARM7.
 * Everything here is single-threaded: there are no real interrupts, so "disable interrupts"
 * and the hardware locks are no-ops, and a VBlank wait is the host frame boundary. */
#include <stdio.h>
#include <stdlib.h>
#include <nitro.h>
#include "pc_platform.h"

/* One host thread. The SDK mutex and queue code (compiled unchanged) needs a current OSThread. */
OSThreadInfo OSi_ThreadInfo;
static OSThread sMainThread;

void OS_Init(void) {
    OS_InitArena();
    OS_InitArenaEx();
    OS_InitIrqTable();
    sMainThread.state = OS_THREAD_STATE_READY;
    sMainThread.priority = OS_THREAD_LAUNCHER_PRIORITY;
    OSi_ThreadInfo.current = &sMainThread;
    OSi_ThreadInfo.list = &sMainThread;
    // shortcut: PXI/ARM7, valarms, card, PM init skipped; add host versions when boot needs them.
}

/* Every host wait completes synchronously (DMA, card, backup), so nothing is ever pending:
 * sleeping returns at once and wakeups have no one to wake. */
void OS_SleepThread(OSThreadQueue *queue) { (void)queue; }
void OS_WakeupThread(OSThreadQueue *queue) { (void)queue; }
void OS_WakeupThreadDirect(OSThread *thread) { (void)thread; }

/* ---- interrupts: one host thread, nothing to mask ---- */
static OSIntrMode sIntrMode = OS_INTRMODE_IRQ_ENABLE;
OSIntrMode OS_EnableInterrupts(void) { OSIntrMode old = sIntrMode; sIntrMode = OS_INTRMODE_IRQ_ENABLE; return old; }
OSIntrMode OS_DisableInterrupts(void) { OSIntrMode old = sIntrMode; sIntrMode = OS_INTRMODE_IRQ_DISABLE; return old; }
OSIntrMode OS_RestoreInterrupts(OSIntrMode state) { OSIntrMode old = sIntrMode; sIntrMode = state; return old; }
OSProcMode OS_GetProcMode(void) { return OS_PROCMODE_SYS; }

/* ---- hardware timers 0-3, advanced one DS frame at a time ----
 * The game's play-time clock (src/timer3.c, timer 3) and OS_GetTick (timer 0) count timer overflows
 * in IRQ handlers plus the live counter. Here the timers advance by one DS frame of CPU cycles at each
 * frame boundary, so time follows frames: deterministic headless, real time when paced at 60 Hz.
 * SDK/game C writes the registers as plain memory, so they are polled: a counter value that differs
 * from what was last shown is a new reload value written by the game.
 * shortcut: counters move once per frame and cascade (count-up) mode is ignored; nothing in HG times
 * work within a frame or chains timers. */
#define DS_FRAME_CYCLES 560190u /* 33513982 Hz / 59.8261 Hz */
static u32 sTmAcc[4], sTmCount[4];
static u16 sTmReload[4], sTmShown[4], sTmPrevCtl[4];

static void TimersFrame(void) {
    static const u8 kShift[4] = { 0, 6, 8, 10 }; /* prescaler 1, 64, 256, 1024 */
    for (int t = 0; t < 4; t++) {
        vu16 *cnt = (vu16 *)(HW_REG_BASE + 0x100 + 4 * t), *ctl = cnt + 1;
        u16 c = *ctl;
        if (*cnt != sTmShown[t]) sTmReload[t] = sTmCount[t] = *cnt;
        if ((c & 0x80) && !(sTmPrevCtl[t] & 0x80)) {
            sTmCount[t] = sTmReload[t];
            sTmAcc[t] = 0;
        }
        sTmPrevCtl[t] = c;
        if ((c & 0x80) && !(c & 0x04)) {
            sTmAcc[t] += DS_FRAME_CYCLES;
            u32 ticks = sTmAcc[t] >> kShift[c & 3];
            sTmAcc[t] -= ticks << kShift[c & 3];
            sTmCount[t] += ticks;
            while (sTmCount[t] >= 0x10000) {
                sTmCount[t] -= 0x10000 - sTmReload[t];
                OSIrqFunction f = OS_IRQTable[3 + t]; /* index 3 = OS_IE_TIMER0 */
                if ((c & 0x40) && (reg_OS_IE & (OS_IE_TIMER0 << t)) && f != NULL) f();
            }
        }
        *cnt = sTmShown[t] = (u16)sTmCount[t];
    }
}

/* Each VBlank wait ends one 60 Hz frame, then runs the timer and VBlank handlers the game registered. */
void OS_WaitIrq(BOOL clear, OSIrqMask irqFlags) {
    (void)clear;
    (void)irqFlags;
    pc_frame_end();
    TimersFrame();
    pc_pxi_frame();
    pc_dma_vblank();
    OSIrqFunction vblank = OS_IRQTable[0]; // index 0 = OS_IE_V_BLANK
    if (vblank != NULL) {
        vblank();
    }
    pc_video_frame();
}
void OS_WaitVBlankIntr(void) { OS_WaitIrq(TRUE, OS_IE_V_BLANK); }
void OS_SpinWait(u32 cycle) { (void)cycle; }

/* ---- hardware locks between ARM9/ARM7: always granted ---- */
static u16 sNextLockID = OS_MAINP_LOCK_ID_START;
s32 OS_GetLockID(void) { return sNextLockID++; }
void OS_ReleaseLockID(u16 lockID) { (void)lockID; }
s32 OS_LockCartridge(u16 lockID) { (void)lockID; return OS_LOCK_SUCCESS; }
s32 OS_LockCard(u16 lockID) { (void)lockID; return OS_LOCK_SUCCESS; }
s32 OS_UnlockCartridge(u16 lockID) { (void)lockID; return OS_UNLOCK_SUCCESS; }
s32 OS_UnLockCartridge(u16 lockID) { (void)lockID; return OS_UNLOCK_SUCCESS; }
s32 OS_UnlockCard(u16 lockID) { (void)lockID; return OS_UNLOCK_SUCCESS; }
s32 OS_TryLockCartridge(u16 lockID) { (void)lockID; return OS_LOCK_SUCCESS; }
u16 OS_ReadOwnerOfLockWord(OSLockWord *lockp) { (void)lockp; return 0; }
BOOL OSi_TryLockVram(u16 bank, u16 lockId) { (void)bank; (void)lockId; return TRUE; }
void OSi_UnlockVram(u16 bank, u16 lockId) { (void)bank; (void)lockId; }

/* ---- caches and MPU: the host is coherent ---- */
void DC_FlushAll(void) {}
void DC_InvalidateRange(void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_StoreRange(const void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_FlushRange(const void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_WaitWriteBufferEmpty(void) {}
void IC_InvalidateAll(void) {}
void IC_InvalidateRange(void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void OS_SetProtectionRegion1(u32 param) { (void)param; }
void OS_SetProtectionRegion2(u32 param) { (void)param; }
void OS_SetDPermissionsForProtectionRegion(u32 setMask, u32 flags) { (void)setMask; (void)flags; }

u32 OS_GetDTCMAddress(void) { return HW_DTCM; }
void OS_InitAlarm(void) {}

void OS_Terminate(void) {
    pc_backtrace("OS_Terminate");
    exit(1);
}

void OS_ResetSystem(u32 parameter) {
    fprintf(stderr, "hg_pc: OS_ResetSystem(%u) - soft reset not supported yet\n", (unsigned)parameter);
    exit(0);
}
