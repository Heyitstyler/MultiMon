/* Host input: keypad registers and the touch screen.
 *
 * The game reads buttons from REG_KEYINPUT (A, B, Select, Start, D-pad, R, L) and from the word
 * the ARM7 mirrors at HW_BUTTON_XY_BUF (X, Y, lid). Both are active-low: a set bit means released.
 * Touch goes through the SDK TP API, answered here from host state. Calibration is the identity,
 * so raw and screen coordinates are the same (0-255, 0-191 on the bottom screen).
 */
#include <nitro.h>
#include "pc_platform.h"

#define KEYINPUT_ALL_RELEASED 0x03FF
#define XYBUF_ALL_RELEASED    0x0C00 /* X (bit 10) and Y (bit 11) released; lid (bit 15) open */

static u16 sHeldKeys;  /* PAD_* bits currently held */
static TPData sTouch;  /* x, y, touch, validity */

/* Called by the frontend (SDL, later) to report state for the next frame. */
void pc_input_set(u16 heldPadKeys, BOOL touching, u16 x, u16 y) {
    sHeldKeys = pc_link_keys(pc_challenge_keys(heldPadKeys));
    sTouch.touch = touching ? TP_TOUCH_ON : TP_TOUCH_OFF;
    sTouch.validity = TP_VALIDITY_VALID;
    sTouch.x = x;
    sTouch.y = y;
}

/* Publish held keys to the registers the game polls. Runs at init and every frame. */
void pc_input_frame(void) {
    *(vu16 *)REG_KEYINPUT_ADDR = (u16)(KEYINPUT_ALL_RELEASED & ~(sHeldKeys & 0x03FF));
    *(vu16 *)HW_BUTTON_XY_BUF = (u16)(XYBUF_ALL_RELEASED & ~(sHeldKeys & 0x0C00));
}

void TP_Init(void) {}
void TP_SetCalibrateParam(const TPCalibrateParam *param) { (void)param; }
BOOL TP_GetUserInfo(TPCalibrateParam *calibrate) { (void)calibrate; return TRUE; }
void TP_RequestSamplingAsync(void) {}
void TP_WaitBusy(TPRequestCommandFlag command_flgs) { (void)command_flgs; }
u32 TP_CheckError(TPRequestCommandFlag command) { (void)command; return 0; }
u32 TP_WaitRawResult(TPData *result) { *result = sTouch; return 0; }
u32 TP_WaitCalibratedResult(TPData *result) { *result = sTouch; return 0; }
u32 TP_GetCalibratedResult(TPData *result) { *result = sTouch; return 0; }
void TP_GetLatestRawPointInAuto(TPData *result) { *result = sTouch; }
void TP_GetCalibratedPoint(TPData *disp, const TPData *raw) { *disp = *raw; }
void TP_RequestAutoSamplingStartAsync(u16 vcount, u16 frequence, TPData samplingBufs[], u16 bufSize) {
    (void)vcount; (void)frequence; (void)samplingBufs; (void)bufSize;
}
void TP_RequestAutoSamplingStopAsync(void) {}
void TP_RequestSetStabilityAsync(u8 retry, u16 range) { (void)retry; (void)range; }
