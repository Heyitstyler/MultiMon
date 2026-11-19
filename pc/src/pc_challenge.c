/* Overworld challenges: trade or battle with another online player by talking to them anywhere.
 *
 *   1. Facing another player (a pc_ghost.c object), press A: "Trade or battle with NAME?
 *      A: Trade  X: Battle  Y: Double  B: No" (Double needs two Pokemon that can fight).
 *   2. That goes through the server (NM_INVITE, with the kind). The other player sees
 *      "NAME wants to trade/battle" and answers with A or B within 30 seconds.
 *   3. On yes, both games form a link right there and trade or battle in the overworld, then carry
 *      on where they stood (pc_link.c: the inviter leads, the other joins).
 *
 * Prompts are a text box over the top screen (pc_video_overlay); while one waits for an answer it
 * takes the A, B, X and Y presses, so the game does not see them.
 *
 * pc_challenge_enter_union and pc_challenge_talk_figure remain as test aids (HG_WARP=union,
 * HG_UNION_TALK): the Union Room entry as the Pokemon Center reception script does it (scr_seq_0004,
 * before ScrCmd_286: the Union Room variables, then sub_02054030, which remembers this spot as the way
 * back out), and talking to a Union Room figure.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "global.h"
#include "constants/global_fieldmap.h"
#include "constants/maps.h"
#include "constants/vars.h"
#include "field_system.h"
#include "field_warp_tasks.h"
#include "map_header.h"
#include "map_object.h"
#include "player_avatar.h"
#include "player_data.h"
#include "script_manager.h"
#include "save_vars_flags.h"
#include "sys_flags.h"
#include "sys_vars.h"
#include "script_pokemon_util.h"
#include "task.h"
#include "pc_net_proto.h"
#include "pc_platform.h"

FieldSystem *pc_field_system(void); /* src/field_system.c, SDK_PC */
PlayerProfile *sub_02035798(int slot); /* asm/unk_02034B0C.s: Union Room slot's player, NULL if empty */

enum { IDLE, CONFIRM, WAITING, INVITED, NOTE };

static int sState, sPeer = -1, sKind;
static time_t sUntil;
static u16 sHeld, sNew; /* this frame's keys, and the ones newly pressed */

static BOOL EnterTask(TaskManager *tm) {
    FieldSystem *fs = TaskManager_GetFieldSystem(tm);
    u32 *state = TaskManager_GetStatePtr(tm);
    if (*state == 0) {
        SaveVarsFlags *vf = Save_VarsFlags_Get(fs->saveData);
        SetScriptVar(vf, VAR_UNK_4133, 2); /* as the reception script */
        SetFlag966(vf);
        sub_02054030(tm); /* runs as a sub-task; we resume when the room is loaded */
        (*state)++;
        return FALSE;
    }
    return TRUE;
}

void pc_challenge_enter_union(FieldSystem *fs) {
    FieldSystem_CreateTask(fs, EnterTask, NULL);
}

static const char *Name(int id) {
    const PcRemote *r = pc_net_remote(id);
    return r != NULL ? r->name : "?";
}

static void Show(int state, int seconds, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void Show(int state, int seconds, const char *fmt, ...) {
    char text[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    sState = state;
    sUntil = time(NULL) + seconds;
    pc_video_overlay(text);
    if (getenv("HG_NET_LOG") != NULL) {
        for (char *c = text; *c; c++) if (*c == '\n') *c = ' ';
        fprintf(stderr, "hg_pc: challenge: %s\n", text);
    }
}

static void Close(void) {
    sState = IDLE;
    sPeer = -1;
    pc_video_overlay(NULL);
}

static void Send(int id, int what) {
    FieldSystem *fs = pc_field_system();
    pc_net_send_invite(id, what, fs != NULL ? PlayerProfile_GetTrainerID(Save_PlayerData_GetProfile(fs->saveData)) : 0, sKind);
}

static const char *KindText(int kind) { return kind == PC_LINK_TRADE ? "trade" : kind == PC_LINK_DOUBLE ? "have a double battle" : "battle"; }

/* Pokemon that can fight now: a double battle needs two. */
static int Able(FieldSystem *fs) { return fs != NULL ? CountAlivePokemon(SaveArray_Party_Get(fs->saveData)) : 0; }

/* From pc_input_set: the keys the game will see. A prompt waiting for an answer keeps A, B, X, Y. */
u16 pc_challenge_keys(u16 held) {
    sNew = held & ~sHeld;
    sHeld = held;
    if (sState == CONFIRM || sState == WAITING || sState == INVITED) held &= ~(PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y);
    return held;
}

/* From pc_net.c: NM_INVITE from player `from`. */
void pc_challenge_message(int from, int what, u32 trainerId, int kind) {
    (void)trainerId;
    if (pc_link_busy() && from != sPeer && (what == INVITE_READY || what == INVITE_CANCEL)) {
        pc_link_partner(from, what); /* the link partner's battle party pick */
        return;
    }
    if (what == INVITE_ASK) {
        if ((sState != IDLE && sState != NOTE) || pc_link_busy() || kind < PC_LINK_TRADE || kind > PC_LINK_DOUBLE) { /* busy */
            int keep = sKind;
            sKind = kind;
            Send(from, INVITE_NO);
            sKind = keep;
            return;
        }
        sPeer = from;
        sKind = kind;
        Show(INVITED, 30, "%.16s wants to\n%s.\n\nA: OK   B: No", Name(from), KindText(kind));
    } else if (from == sPeer && sState == WAITING && what == INVITE_YES) {
        pc_link_start(sKind, 1, sPeer); /* the inviter leads the link */
        Close();
    } else if (from == sPeer && sState == WAITING && what == INVITE_NO) {
        Show(NOTE, 3, "%.16s can't come now.", Name(from));
    } else if (from == sPeer && sState == INVITED && what == INVITE_CANCEL) {
        Show(NOTE, 3, "%.16s took back\nthe invitation.", Name(from));
    }
}

/* The other player this one faces, or -1. */
static int Facing(FieldSystem *fs) {
    PlayerAvatar *pa = fs->playerAvatar;
    int x = PlayerAvatar_GetXCoord(pa), z = PlayerAvatar_GetZCoord(pa), dir = PlayerAvatar_GetFacingDirection(pa);
    x += (dir == DIR_EAST) - (dir == DIR_WEST);
    z += (dir == DIR_SOUTH) - (dir == DIR_NORTH);
    LocalMapObject *o = MapObjectManager_GetFirstObjectWithXAndZ(fs->mapObjectManager, x, z, FALSE);
    if (o == NULL) return -1;
    int id = (int)MapObject_GetID(o) - PC_REMOTE_ID;
    return id >= 0 && id < NET_MAX_PLAYERS && pc_net_remote(id) != NULL ? id : -1;
}

/* Talks to a Union Room figure as A in front of it would (its script, with it as the one talked to):
 * trainer `tid`'s, or any other player's if `anyone`. FALSE: not possible right now. */
static BOOL TalkFigure(FieldSystem *fs, BOOL anyone, u32 tid) {
    if (fs == NULL || fs->processManager == NULL || !FieldSystem_IsPlayerMovementAllowed(fs) || fs->unk80 == NULL ||
        !MapHeader_MapIsUnionRoom(fs->location->mapId)) {
        return FALSE;
    }
    for (int slot = 0; slot < 16; slot++) {
        PlayerProfile *p = sub_02035798(slot);
        if (p == NULL || (!anyone && PlayerProfile_GetTrainerID(p) != tid)) continue;
        LocalMapObject *o = MapObjectManager_GetFirstActiveObjectByID(fs->mapObjectManager, slot + 1); /* figure id: sub_0205A200 */
        if (o == NULL) continue;
        StartMapSceneScript(fs, (u16)MapObject_GetScriptID(o), o);
        return TRUE;
    }
    return FALSE;
}

BOOL pc_challenge_talk_figure(FieldSystem *fs) { return TalkFigure(fs, TRUE, 0); }

/* Each frame end, after pc_ghost_frame. */
void pc_challenge_frame(void) {
    u16 pressed = sNew;
    sNew = 0;
    if (sState == IDLE && pressed == 0) return;
    FieldSystem *fs = pc_field_system();
    BOOL idle = fs != NULL && fs->processManager != NULL && FieldSystem_IsPlayerMovementAllowed(fs); /* as pc_ghost.c */
    if (sPeer >= 0 && pc_net_remote(sPeer) == NULL && sState != NOTE && sState != IDLE) {
        Show(NOTE, 3, "The other player went offline.");
        return;
    }
    switch (sState) {
    case IDLE: {
        if (!(pressed & PAD_BUTTON_A) || !idle || fs->location->mapId <= MAP_WIFI_MULTI_BATTLE_AREA) break;
        int id = Facing(fs);
        if (id < 0) break;
        if (pc_link_busy()) break;
        sPeer = id;
        Show(CONFIRM, 0, "Trade or battle\nwith %.16s?\n\nA: Trade   X: Battle\nY: Double  B: No", Name(id));
        break;
    }
    case CONFIRM:
        if (pressed & (PAD_BUTTON_A | PAD_BUTTON_X | PAD_BUTTON_Y)) {
            sKind = (pressed & PAD_BUTTON_A) ? PC_LINK_TRADE : (pressed & PAD_BUTTON_X) ? PC_LINK_SINGLE : PC_LINK_DOUBLE;
            if (sKind == PC_LINK_DOUBLE && Able(fs) < 2) {
                Show(NOTE, 3, "A double battle needs two\nPokemon that can fight.");
                break;
            }
            Send(sPeer, INVITE_ASK);
            Show(WAITING, 30, "Waiting for %.16s...\n\nB: Cancel", Name(sPeer));
        } else if (pressed & PAD_BUTTON_B) {
            Close();
        }
        break;
    case WAITING:
        if (pressed & PAD_BUTTON_B) {
            Send(sPeer, INVITE_CANCEL);
            Close();
        } else if (time(NULL) >= sUntil) {
            Send(sPeer, INVITE_CANCEL);
            Show(NOTE, 3, "%.16s didn't answer.", Name(sPeer));
        }
        break;
    case INVITED:
        if ((pressed & PAD_BUTTON_A) && sKind == PC_LINK_DOUBLE && Able(fs) < 2) {
            Send(sPeer, INVITE_NO);
            Show(NOTE, 3, "A double battle needs two\nPokemon that can fight.");
        } else if (pressed & PAD_BUTTON_A) {
            Send(sPeer, INVITE_YES);
            pc_link_start(sKind, 0, sPeer); /* joins the inviter's link */
            Close();
        } else if ((pressed & PAD_BUTTON_B) || time(NULL) >= sUntil) {
            Send(sPeer, INVITE_NO);
            Close();
        }
        break;
    case NOTE:
        if (time(NULL) >= sUntil) Close();
        break;
    }
}
