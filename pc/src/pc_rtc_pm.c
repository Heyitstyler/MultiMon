/* Host real-time clock (local time) and power management (no-ops: backlights always on). */
#include <stdlib.h>
#include <time.h>
#include <nitro.h>
#include "pc_platform.h"

void RTC_Init(void) {}

static void HostDateTime(RTCDate *date, RTCTime *clock) {
    time_t now = time(NULL);
    struct tm t;
    t = *localtime(&now); /* single-threaded */
    if (date != NULL) {
        date->year = (u32)(t.tm_year % 100); // DS RTC stores 2000-2099 as 0-99
        date->month = (u32)t.tm_mon + 1;
        date->day = (u32)t.tm_mday;
        date->week = (RTCWeek)t.tm_wday; // RTC_WEEK_SUNDAY == 0
    }
    if (clock != NULL) {
        clock->hour = (u32)t.tm_hour;
        clock->minute = (u32)t.tm_min;
        clock->second = (u32)(t.tm_sec > 59 ? 59 : t.tm_sec);
    }
}

RTCResult RTC_GetDateTimeAsync(RTCDate *date, RTCTime *time, RTCCallback callback, void *arg) {
    HostDateTime(date, time);
    if (callback != NULL) callback(RTC_RESULT_SUCCESS, arg);
    return RTC_RESULT_SUCCESS;
}
RTCResult RTC_GetDateTime(RTCDate *date, RTCTime *time) { HostDateTime(date, time); return RTC_RESULT_SUCCESS; }
RTCResult RTC_GetDate(RTCDate *date) { HostDateTime(date, NULL); return RTC_RESULT_SUCCESS; }
RTCResult RTC_GetTime(RTCTime *time) { HostDateTime(NULL, time); return RTC_RESULT_SUCCESS; }

u32 PM_GetBackLight(PMBackLightSwitch *top, PMBackLightSwitch *bottom) {
    if (top != NULL) *top = PM_BACKLIGHT_ON;
    if (bottom != NULL) *bottom = PM_BACKLIGHT_ON;
    return PM_RESULT_SUCCESS;
}
u32 PM_SetBackLight(PMLCDTarget target, PMBackLightSwitch status) { (void)target; (void)status; return PM_RESULT_SUCCESS; }
u32 PM_SetAmp(PMAmpSwitch sw) { (void)sw; return PM_RESULT_SUCCESS; }
u32 PM_SetAmpGain(PMAmpGain gain) { (void)gain; return PM_RESULT_SUCCESS; }
void PM_GoSleepMode(PMWakeUpTrigger trigger, PMLogic logic, u16 keyPattern) { (void)trigger; (void)logic; (void)keyPattern; }

/* Sleep mode never happens on the host, so sleep callbacks are never run. */
void PM_AppendPreSleepCallback(PMSleepCallbackInfo *info) { (void)info; }
void PM_PrependPreSleepCallback(PMSleepCallbackInfo *info) { (void)info; }
void PM_AppendPostSleepCallback(PMSleepCallbackInfo *info) { (void)info; }
void PM_DeletePreSleepCallback(PMSleepCallbackInfo *info) { (void)info; }
void PM_DeletePostSleepCallback(PMSleepCallbackInfo *info) { (void)info; }
u32 PM_ForceToPowerOff(void) { exit(0); }

void MIC_Init(void) {} /* shortcut: no microphone input; Chatot recording stays silent */

/* Compressed overlay images: the overlay code is native on the host, so the DS copy is never run. */
void MIi_UncompressBackward(void *bottom) { (void)bottom; }

/* shortcut: streamed audio is off until the sound backend exists (M5). */
void NNS_SndArcStrmStopAll(int fadeFrame) { (void)fadeFrame; }
void NNSi_SndArcStrmMain(void) {}
