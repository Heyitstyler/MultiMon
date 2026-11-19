/* Host replacement for PXI, the ARM9<->ARM7 FIFO. There is no ARM7: every ARM7 service reports
 * ready and sent words are dropped, except sound, whose ARM7 driver runs on the host (pc_snd.c),
 * and wireless power (WVR), which is answered "done" one frame later. The game treats a failed or
 * repeated wireless power-up as fatal (OS_Terminate), and the main menu powers it up to look for
 * Mystery Gift beacons. The radio itself (WM) is answered by pc_wm.c.
 * The other services the game needs (touch, power, RTC) get host versions at their own SDK API
 * level instead of answering PXI messages. */
#include <stdio.h>
#include <stdlib.h>
#include <nitro.h>
#include <nitro/wvr/common/wvr_common.h>
#include "pc_platform.h"

static u32 sWvrReply; /* 0, or the reply word owed to the ARM9 */

void PXI_Init(void) {}
void PXI_InitFifo(void) {}

void PXI_SetFifoSendCallback(PXIFifoEmtpyCallback callback) { (void)callback; }

int PXI_SendWordByFifo(int fifotag, u32 data, BOOL err) {
    (void)err;
    if (fifotag == PXI_FIFO_TAG_SOUND) pc_snd_pxi_from_arm9(data); /* to the host ARM7 sound driver */
    else if (fifotag == PXI_FIFO_TAG_WVR) {
        if (getenv("HG_WM_LOG")) fprintf(stderr, "hg_pc: wvr: request %08x\n", (unsigned)data);
        sWvrReply = (data & 0xFFFF0000u) | WVR_RESULT_SUCCESS;
    }
    else if (fifotag == PXI_FIFO_TAG_WM) pc_wm_command(data); /* the host ARM7 radio (pc_wm.c) */
    return PXI_FIFO_SUCCESS;
}

/* Receive callbacks the ARM9 side registered; the host ARM7 sound driver sends to them. */
static PXIFifoCallback sRecv[PXI_MAX_FIFO_TAG];

void PXI_SetFifoRecvCallback(int fifotag, PXIFifoCallback callback) { sRecv[fifotag] = callback; }

/* Every ARM7 service is ready. For the ARM9 side, say whether a receive callback is registered:
 * the SDK registers its own (e.g. WVR's) only when this says there is none. */
BOOL PXI_IsCallbackReady(int fifotag, PXIProc proc) {
    return proc == PXI_PROC_ARM7 || sRecv[fifotag] != NULL;
}

/* Once per frame: deliver the ARM7 answers owed. */
void pc_pxi_frame(void) {
    pc_wm_frame();
    if (sWvrReply != 0) {
        u32 reply = sWvrReply;
        sWvrReply = 0;
        pc_pxi_to_arm9(PXI_FIFO_TAG_WVR, reply, FALSE);
    }
}

void pc_pxi_to_arm9(int fifotag, u32 data, BOOL err) {
    if (sRecv[fifotag] != NULL) sRecv[fifotag]((PXIFifoTag)fifotag, data, err);
}
