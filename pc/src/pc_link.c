/* Trades and battles in the overworld, without a link room (multiplayer).
 *
 * After an invitation is accepted (pc_challenge.c), both games form a two-player link with the
 * Wireless Club's own group code (src/overlay_03/communication_club.c), started here from a field
 * task instead of the Pokemon Center desk: the inviter leads a group (ov03_02255C18, "1 more
 * needed"), the other joins it (ov03_02255BB0, a list of groups). Its prompts show over the overworld
 * and are answered here by pressing A (join the group shown, accept the member); B still cancels, and
 * after 30 seconds without a link it cancels by itself. Then, as the link room scripts do:
 *  - a sync point, as the desk's script after connecting;
 *  - trade: a fade to black, the game's trade task (CallTask_WirelessTrade: pick, trade, evolve; it
 *    leaves the overworld and ends the link when both quit), the overworld restored and faded in;
 *  - battle (link type SINGLE_BATTLE or DOUBLE_BATTLE): a fade to black and the game's battle party
 *    picker (the party menu as the Union Room opens it, PARTY_MENU_CONTEXT_UNION_ROOM_BATTLE_SELECT,
 *    but 1-6 Pokemon for a single battle and 2-6 for a double, no level limit), the overworld
 *    restored and faded in; each game tells the other through the server whether its player chose
 *    or cancelled (NM_INVITE INVITE_READY / INVITE_CANCEL, as the Union Room's own link message
 *    0x65 does); when both chose, the game's link battle task (CallTask_020508B8: transition, battle
 *    with the chosen Pokemon in the chosen order, results, overworld restored), then a fade in;
 *  - the link closed (ScrCmd_283's sub_02058284).
 * The club turns on a check that the link keeps 2 players; the trade screen and battle end the link
 * themselves, so only the general error check stays on (a real disconnect still shows the game's
 * "A communication error has occurred").
 *
 * HG_LINK=frame:leader|member[:trade|single|double]   start one at that frame (tests; the partner
 * is the first other player online). HG_LINK_LOG=1 logs each step.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "global.h"
#include "constants/battle.h"
#include "constants/comm_type.h"
#include "encounter.h"
#include "field_system.h"
#include "launch_application.h"
#include "overlay_03.h"
#include "party_menu.h"
#include "screen_fade.h"
#include "task.h"
#include "unk_02034B0C.h"
#include "unk_020379A0.h"
#include "unk_020552A4.h"
#include "unk_02058034.h"
#include "pc_net_proto.h"
#include "pc_platform.h"

/* not unk_02037C94.h / unk_02035900.h: their Wi-Fi headers pull in windows.h on Windows, which
 * clashes with ScrollWindow */
void sub_020398D4(s8 arg0, s8 arg1); /* link error checks: count, general */
BOOL sub_02037D78(void);             /* link running */
int sub_02037454(void);              /* players linked */

FieldSystem *pc_field_system(void); /* src/field_system.c, SDK_PC */

#define SYNC_POINT    96   /* the desk script's sync point after connecting */
#define GROUP_TIMEOUT 1800 /* frames without a link before giving up */
#define PICK_DONE     6    /* PartyMenuArgs.partySlot after the picker: chosen */
#define PICK_CANCEL   7    /* ... cancelled; other values: a Pokemon's summary was asked for */

enum {
    L_START, L_WAIT_GROUP, L_SYNC, L_WAIT_SYNC, L_FADE_OUT, L_RESTORE, L_FADE_IN, L_CLOSE, L_WAIT_CLOSE,
    L_PICK, L_PICK_WAIT, L_SUMMARY_WAIT, L_PICKED, L_PICKED_FADE, L_WAIT_PARTNER,
};

static int sKind = -1, sLeader, sPeer = -1, sQueued, sRunning, sLog = -1;
static int sPartner; /* the partner's pick: 0 not yet, INVITE_READY or INVITE_CANCEL */
static u32 sLinkState; /* LinkTask's state, for pc_link_keys */
static unsigned long sFrame, sGroupAt;
static PartyMenuArgs *sPick;
static PokemonSummaryArgs *sSummary;
static u8 sPartySlots[PARTY_SIZE]; /* 1-based party slots in battle order, 0 after the last */

static void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void Log(const char *fmt, ...) {
    if (sLog <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "hg_pc: link: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static const char *KindName(int kind) { return kind == PC_LINK_TRADE ? "trade" : kind == PC_LINK_DOUBLE ? "double battle" : "battle"; }

static const char *PeerName(void) {
    const PcRemote *r = pc_net_remote(sPeer);
    return r != NULL ? r->name : "The other player";
}

/* As the script command FadeScreen 6, 1, type, RGB_BLACK. */
static void Fade(enum FadeType type) {
    BeginNormalPaletteFade(FADE_BOTH_SCREENS, type, type, RGB_BLACK, 6, 1, HEAP_ID_FIELD1);
    ResetVisibleHardwareWindows(PM_LCD_TOP);
    ResetVisibleHardwareWindows(PM_LCD_BOTTOM);
}

/* The battle party picker, as TaskManager_LaunchPartyMenu_UnionRoomBattleSelect sets it up
 * (PartyMenu_CreateArgs is static there), with this game's limits. */
static void OpenPicker(FieldSystem *fs) {
    if (sPick == NULL) {
        sPick = Heap_Alloc(HEAP_ID_FIELD3, sizeof(PartyMenuArgs));
        MI_CpuClearFast(sPick, sizeof(PartyMenuArgs));
        sPick->party = SaveArray_Party_Get(fs->saveData);
        sPick->bag = Save_Bag_Get(fs->saveData);
        sPick->mailbox = Save_Mailbox_Get(fs->saveData);
        sPick->options = Save_PlayerData_GetOptionsAddr(fs->saveData);
        sPick->context = PARTY_MENU_CONTEXT_UNION_ROOM_BATTLE_SELECT;
        sPick->fieldSystem = fs;
        sPick->menuInputStatePtr = &fs->menuInputState;
        sPick->minMonsToSelect = sKind == PC_LINK_DOUBLE ? 2 : 1;
        sPick->maxMonsToSelect = PARTY_SIZE;
        sPick->maxLevel = 100;
        sPick->linkBattleRuleset = NULL;
    }
    FieldSystem_LaunchApplication(fs, &gOverlayTemplate_PartyMenu, sPick);
}

static void TellPartner(int what) {
    if (sPeer >= 0) pc_net_send_invite(sPeer, what, 0, sKind);
}

static BOOL LinkTaskStep(TaskManager *tm);

static BOOL LinkTask(TaskManager *tm) {
    BOOL done = LinkTaskStep(tm);
    sLinkState = done ? L_START : *TaskManager_GetStatePtr(tm);
    return done;
}

static BOOL LinkTaskStep(TaskManager *tm) {
    FieldSystem *fs = TaskManager_GetFieldSystem(tm);
    u32 *state = TaskManager_GetStatePtr(tm);
    switch (*state) {
    case L_START: {
        u32 commType = sKind == PC_LINK_TRADE ? COMM_TYPE_TRADE : sKind == PC_LINK_DOUBLE ? COMM_TYPE_DOUBLE_BATTLE : COMM_TYPE_SINGLE_BATTLE;
        if (sLeader) ov03_02255C18(fs, commType, 0, 0);
        else ov03_02255BB0(fs, commType, 0, 0);
        Log("%s a %s group", sLeader ? "leading" : "joining", KindName(sKind));
        sGroupAt = sFrame;
        *state = L_WAIT_GROUP;
        break;
    }
    case L_WAIT_GROUP: {
        u32 ret = sLeader ? ov03_02255C64() : ov03_02255BFC();
        if (ret == 0) break;
        Log("group result %u", (unsigned)ret);
        if (ret != 2) { /* 1: retry, 3/4: no partner or cancelled (the desk script's cases) */
            sRunning = 0;
            return TRUE;
        }
        *state = L_SYNC;
        break;
    }
    case L_SYNC:
        sub_02037AC0(SYNC_POINT);
        *state = L_WAIT_SYNC;
        break;
    case L_WAIT_SYNC:
        if (sub_02037454() >= 2 && !sub_02037B38(SYNC_POINT)) break;
        Log("synced (%d connected)", sub_02037454());
        sub_020398D4(0, 1); /* see the top comment */
        Fade(FADE_TYPE_BRIGHTNESS_OUT);
        *state = sKind == PC_LINK_TRADE ? L_FADE_OUT : L_PICK;
        break;
    case L_FADE_OUT:
        if (!IsPaletteFadeFinished()) break;
        CallTask_WirelessTrade(tm); /* a sub-task; we resume here when it ends */
        *state = L_RESTORE;
        break;
    case L_RESTORE:
        CallTask_RestoreOverworld(tm);
        *state = L_FADE_IN;
        break;
    case L_PICK:
        if (!IsPaletteFadeFinished()) break;
        OpenPicker(fs);
        *state = L_PICK_WAIT;
        break;
    case L_PICK_WAIT:
        if (FieldSystem_ApplicationIsRunning(fs)) break;
        if (sPick->partySlot == PICK_DONE || sPick->partySlot == PICK_CANCEL) {
            *state = L_PICKED;
        } else { /* "Summary" on a Pokemon: show it, then the picker again */
            sSummary = PokemonSummary_CreateArgs(fs, HEAP_ID_FIELD3, 0);
            sSummary->partySlot = sPick->partySlot;
            PokemonSummary_LearnForget_LaunchApp(fs, sSummary);
            *state = L_SUMMARY_WAIT;
        }
        break;
    case L_SUMMARY_WAIT:
        if (FieldSystem_ApplicationIsRunning(fs)) break;
        sPick->partySlot = sSummary->partySlot;
        Heap_Free(sSummary);
        sSummary = NULL;
        OpenPicker(fs);
        *state = L_PICK_WAIT;
        break;
    case L_PICKED: {
        BOOL chose = sPick->partySlot == PICK_DONE;
        MI_CpuCopy8(sPick->selectedOrder, sPartySlots, sizeof sPartySlots);
        Heap_Free(sPick);
        sPick = NULL;
        Log("battle party %s: %d %d %d %d %d %d", chose ? "chosen" : "cancelled", sPartySlots[0], sPartySlots[1], sPartySlots[2],
            sPartySlots[3], sPartySlots[4], sPartySlots[5]);
        TellPartner(chose ? INVITE_READY : INVITE_CANCEL);
        if (!chose) sPartySlots[0] = 0xFF; /* marks "cancelled" until L_WAIT_PARTNER */
        /* The link idles until both decided, and if either cancelled the other may close it first:
         * no error check meanwhile (the server tells if the partner leaves). */
        sub_020398D4(0, 0);
        CallTask_RestoreOverworld(tm);
        *state = L_PICKED_FADE;
        break;
    }
    case L_PICKED_FADE:
        Fade(FADE_TYPE_BRIGHTNESS_IN);
        *state = L_WAIT_PARTNER;
        break;
    case L_WAIT_PARTNER: {
        if (!IsPaletteFadeFinished()) break;
        int partnerGone = sPeer >= 0 && pc_net_remote(sPeer) == NULL;
        if (sPartner == 0 && !partnerGone) { /* both must be out of the picker before the link closes */
            char text[80];
            snprintf(text, sizeof text, "Waiting for %.16s\nto choose...", PeerName());
            pc_video_overlay(text);
            break;
        }
        if (sPartySlots[0] == 0xFF || sPartner == INVITE_CANCEL || partnerGone) {
            char text[80];
            if (sPartySlots[0] == 0xFF) snprintf(text, sizeof text, "Battle cancelled");
            else snprintf(text, sizeof text, "%.16s cancelled the battle", PeerName());
            pc_video_overlay(NULL);
            pc_chat_receive(NULL, text);
            Log("battle cancelled (%s)", sPartySlots[0] == 0xFF ? "here" : "by the partner");
            *state = L_CLOSE;
            break;
        }
        pc_video_overlay(NULL);
        sub_020398D4(0, 1); /* both chose: the general error check again for the battle */
        CallTask_020508B8(tm, sPartySlots, sKind == PC_LINK_DOUBLE ? BATTLE_TYPE_TRAINER | BATTLE_TYPE_LINK | BATTLE_TYPE_DOUBLES : BATTLE_TYPE_TRAINER | BATTLE_TYPE_LINK);
        *state = L_FADE_IN; /* the battle task restores the overworld itself */
        break;
    }
    case L_FADE_IN:
        Log("%s done, back to the overworld", KindName(sKind));
        Fade(FADE_TYPE_BRIGHTNESS_IN);
        *state = L_CLOSE;
        break;
    case L_CLOSE:
        if (!IsPaletteFadeFinished()) break;
        sub_02058284();
        *state = L_WAIT_CLOSE;
        break;
    case L_WAIT_CLOSE:
        if (sub_02037D78() == TRUE || sub_02035650() == TRUE) break;
        Log("link closed");
        sRunning = 0;
        pc_video_overlay(NULL);
        return TRUE;
    }
    return FALSE;
}

int pc_link_start(int kind, int leader, int peer) {
    if (sQueued || sRunning) return 0;
    sKind = kind;
    sLeader = leader;
    sPeer = peer;
    sPartner = 0;
    sQueued = 1;
    return 1;
}

int pc_link_busy(void) { return sQueued || sRunning; }

/* From pc_challenge.c: the partner's NM_INVITE while a link runs (its battle party pick). */
void pc_link_partner(int from, int what) {
    if (from == sPeer && (what == INVITE_READY || what == INVITE_CANCEL)) sPartner = what;
}

/* From pc_input via pc_challenge_keys: while the group forms, press A now and then (join the group
 * shown, say yes to the member); after GROUP_TIMEOUT, B (cancel). */
u16 pc_link_keys(u16 held) {
    if (!sRunning || sLinkState != L_WAIT_GROUP) return held;
    unsigned long t = sFrame - sGroupAt;
    if (t % 40 >= 4) return held;
    return held | (t > GROUP_TIMEOUT ? PAD_BUTTON_B : t > 60 ? PAD_BUTTON_A : 0);
}

/* Each frame end. */
void pc_link_frame(unsigned long frame) {
    sFrame = frame;
    if (sLog < 0) {
        sLog = getenv("HG_LINK_LOG") != NULL;
        const char *e = getenv("HG_LINK");
        char *end, role[16] = "", kind[16] = "trade";
        if (e != NULL) {
            unsigned long at = strtoul(e, &end, 10);
            if (*end == ':' && sscanf(end + 1, "%15[a-z]:%15[a-z]", role, kind) >= 1) {
                sGroupAt = at; /* reused as the start frame until it starts */
                pc_link_start(!strcmp(kind, "double") ? PC_LINK_DOUBLE : !strcmp(kind, "single") ? PC_LINK_SINGLE : PC_LINK_TRADE, !strcmp(role, "leader"), -1);
                sQueued = 2; /* wait for that frame */
            }
        }
    }
    if (sQueued == 2 && frame < sGroupAt) return;
    if (!sQueued) return;
    FieldSystem *fs = pc_field_system();
    if (fs == NULL || fs->taskman != NULL || fs->processManager == NULL || !FieldSystem_IsPlayerMovementAllowed(fs)) return;
    if (sPeer < 0) { /* HG_LINK: the first other player online */
        for (int i = 0; i < NET_MAX_PLAYERS && sPeer < 0; i++) if (pc_net_remote(i) != NULL) sPeer = i;
    }
    sQueued = 0;
    sRunning = 1;
    FieldSystem_CreateTask(fs, LinkTask, NULL);
}
