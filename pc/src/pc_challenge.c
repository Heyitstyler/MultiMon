/* Overworld challenges: trade or battle with another online player by talking to them anywhere.
 *
 *   1. Facing another player (a pc_ghost.c object), press A: the game's own message box asks "What
 *      would you like to do with NAME?" beside the game's field choice menu (TRADE, BATTLE, DOUBLE
 *      BATTLE, CANCEL: the item texts of the standard menu bank, msg_0191). Double needs two Pokemon
 *      that can fight.
 *   2. That goes through the server (NM_INVITE, with the kind). Once the other player stands still,
 *      their game says "NAME wants to trade. Is that OK?" with the game's YES/NO box (30 seconds to
 *      answer). A player who isn't free within a few seconds (in a menu, battle or script) declines.
 *   3. On yes, both games form a link right there and trade or battle in the overworld, then carry
 *      on where they stood (pc_link.c: the inviter leads, the other joins).
 *
 * The boxes run in a field task, as a script's would, so the player can't walk away meanwhile.
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
#include "system.h"
#include "constants/charcode.h"
#include "constants/global_fieldmap.h"
#include "constants/maps.h"
#include "constants/player_avatar.h"
#include "constants/vars.h"
#include "bg_window.h"
#include "dialog_box.h"
#include "field_system.h"
#include "field_warp_tasks.h"
#include "list_menu.h"
#include "list_menu_2d.h"
#include "map_header.h"
#include "map_object.h"
#include "message_format.h"
#include "overlay_01.h"
#include "player_avatar.h"
#include "player_data.h"
#include "pm_string.h"
#include "render_window.h"
#include "script_manager.h"
#include "save_vars_flags.h"
#include "sys_flags.h"
#include "sys_vars.h"
#include "script_pokemon_util.h"
#include "task.h"
#include "text.h"
#include "pc_net_proto.h"
#include "pc_platform.h"

FieldSystem *pc_field_system(void); /* src/field_system.c, SDK_PC */
PlayerProfile *sub_02035798(int slot); /* asm/unk_02034B0C.s: Union Room slot's player, NULL if empty */

enum { IDLE, ASKED, BUSY }; /* ASKED: an invitation waits for this player to stand still */

/* Task steps: the inviter's, then the invited player's, then both. */
enum { T_ASK, T_ASK_PRINT, T_MENU, T_WAIT, I_ASK, I_ASK_PRINT, I_YESNO, T_NOTE, T_END };

/* msg_0191 (the field menu's standard bank) item texts */
#define MSG_TRADE  23
#define MSG_BATTLE 52
#define MSG_DOUBLE 7
#define MSG_CANCEL 24

#define MENU_PENDING 0xEEEE /* the field menu's result until something is chosen; B gives 0xFFFE */
#define FRAME_TILE   0x3D9  /* the standard window frame, as ScrCmd_YesNo loads it */

static int sState, sPeer = -1, sKind, sReply, sStep;
static time_t sUntil;
static u16 sHeld, sNew; /* this frame's keys, and the ones newly pressed */

static Window sWin;
static String *sText;
static MessageFormat *sFmt;
static u8 sPrinter;
static u16 sChoice;
static struct UnkStruct_ov01_021EDC28 *sMenu;
static struct ListMenu2D *sYesNo;

static const WindowTemplate sYesNoTemplate = { /* ScrCmd_YesNo's */
    .bgId = 3, .left = 25, .top = 13, .width = 6, .height = 4, .palette = 13, .baseTile = 0x021F,
};

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

#define E_ACUTE "\x01" /* the e in Pokemon */

/* ASCII to the game's characters (tools/msgenc/charmap.txt); anything else shows as '?'. */
static u16 Glyph(char c) {
    static const char punct[] = " !?,./'\"()+-*#=&~:;@%$_\n\x01";
    static const u16 codes[] = { 0x1DE, 0x1AB, 0x1AC, 0x1AD, 0x1AE, 0x1B1, 0x1B3, 0x1B4, 0x1B9, 0x1BA, 0x1BD, 0x1BE, 0x1BF,
                                 0x1C0, 0x1C1, 0x1C2, 0x1C3, 0x1C4, 0x1C5, 0x1D0, 0x1D2, 0x1A8, 0x1E9, 0xE000, 0x188 };
    _Static_assert(sizeof punct - 1 == sizeof codes / sizeof codes[0], "one code per character");
    if (c >= 'A' && c <= 'Z') return 0x12B + (c - 'A');
    if (c >= 'a' && c <= 'z') return 0x145 + (c - 'a');
    if (c >= '0' && c <= '9') return 0x121 + (c - '0');
    const char *p = c != '\0' ? strchr(punct, c) : NULL;
    return p != NULL ? codes[p - punct] : 0x1AC;
}

/* Prints a new message in the game's message box at the bottom, opening it if needed. */
static void Say(FieldSystem *fs, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void Say(FieldSystem *fs, const char *fmt, ...) {
    char text[160];
    u16 chars[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    int n = 0;
    for (; text[n] != '\0'; n++) chars[n] = Glyph(text[n]);
    chars[n] = EOS;
    CopyU16ArrayToString(sText, chars);
    Options *options = Save_PlayerData_GetOptionsAddr(fs->saveData);
    if (!WindowIsInUse(&sWin)) {
        DialogBox_AddWindowToLayer3(fs->bgConfig, &sWin, GF_BG_LYR_MAIN_3);
        DialogBox_LoadFrame(&sWin, options);
        fs->textbox_open = TRUE;
    } else if (!DialogBox_IsPrintFinished(sPrinter)) {
        RemoveTextPrinter(sPrinter);
    }
    FillWindowPixelBuffer(&sWin, 15);
    sPrinter = DialogBox_PrintMessage(&sWin, sText, options, TRUE);
    if (getenv("HG_NET_LOG") != NULL) {
        for (char *c = text; *c; c++) if (*c == '\n') *c = ' ';
        fprintf(stderr, "hg_pc: challenge: %s\n", text);
    }
}

static void Send(int id, int what) {
    FieldSystem *fs = pc_field_system();
    pc_net_send_invite(id, what, fs != NULL ? PlayerProfile_GetTrainerID(Save_PlayerData_GetProfile(fs->saveData)) : 0, sKind);
}

static const char *KindText(int kind) { return kind == PC_LINK_TRADE ? "trade" : kind == PC_LINK_DOUBLE ? "have a double battle" : "battle"; }

/* Pokemon that can fight now: a double battle needs two. */
static int Able(FieldSystem *fs) { return fs != NULL ? CountAlivePokemon(SaveArray_Party_Get(fs->saveData)) : 0; }

static BOOL Printed(void) { return DialogBox_IsPrintFinished(sPrinter); }

static BOOL ChallengeTask(TaskManager *tm) {
    FieldSystem *fs = TaskManager_GetFieldSystem(tm);
    u16 keys = gSystem.newKeys;
    BOOL gone = pc_net_remote(sPeer) == NULL;
    switch (sStep) {
    case T_ASK:
        Say(fs, "What would you like to do\nwith %.16s?", Name(sPeer));
        sStep = T_ASK_PRINT;
        break;
    case T_ASK_PRINT:
        if (!Printed()) break;
        sChoice = MENU_PENDING;
        sMenu = ov01_021EDF78(fs, 19, 9, 0, TRUE, &sChoice, sFmt, &sWin, NULL);
        ov01_021EDC7C(sMenu, MSG_TRADE, PC_LINK_TRADE);
        ov01_021EDC7C(sMenu, MSG_BATTLE, PC_LINK_SINGLE);
        ov01_021EDC7C(sMenu, MSG_DOUBLE, PC_LINK_DOUBLE);
        ov01_021EDC7C(sMenu, MSG_CANCEL, MENU_PENDING - 1);
        ov01_021EDC84(sMenu);
        sStep = T_MENU;
        break;
    case T_MENU:
        if (sChoice == MENU_PENDING) {
            if (!gone) break;
            ov01_021EDF38(sMenu);
            sMenu = NULL;
            Say(fs, "The other player went offline.");
            sStep = T_NOTE;
            break;
        }
        sMenu = NULL; /* it closed itself */
        if (sChoice != PC_LINK_TRADE && sChoice != PC_LINK_SINGLE && sChoice != PC_LINK_DOUBLE) {
            sStep = T_END;
            break;
        }
        sKind = sChoice;
        if (sKind == PC_LINK_DOUBLE && Able(fs) < 2) {
            Say(fs, "A double battle needs two\nPok" E_ACUTE "mon that can fight.");
            sStep = T_NOTE;
            break;
        }
        sReply = 0;
        Send(sPeer, INVITE_ASK);
        Say(fs, "Waiting for %.16s...\nPress B to cancel.", Name(sPeer));
        sUntil = time(NULL) + 30;
        sStep = T_WAIT;
        break;
    case T_WAIT:
        if (gone) {
            Say(fs, "The other player went offline.");
            sStep = T_NOTE;
        } else if (sReply == INVITE_YES) {
            pc_link_start(sKind, 1, sPeer); /* the inviter leads the link; it starts once this task ends */
            sStep = T_END;
        } else if (sReply == INVITE_NO) {
            Say(fs, "%.16s can't come now.", Name(sPeer));
            sStep = T_NOTE;
        } else if (keys & PAD_BUTTON_B) {
            Send(sPeer, INVITE_CANCEL);
            sStep = T_END;
        } else if (time(NULL) >= sUntil) {
            Send(sPeer, INVITE_CANCEL);
            Say(fs, "%.16s didn't answer.", Name(sPeer));
            sStep = T_NOTE;
        }
        break;
    case I_ASK:
        Say(fs, "%.16s wants to\n%s. Is that OK?", Name(sPeer), KindText(sKind));
        sStep = I_ASK_PRINT;
        break;
    case I_ASK_PRINT:
        if (!Printed()) break;
        LoadUserFrameGfx1(fs->bgConfig, GF_BG_LYR_MAIN_3, FRAME_TILE, 11, 0, HEAP_ID_FIELD1);
        sYesNo = Std_CreateYesNoMenu(fs->bgConfig, &sYesNoTemplate, FRAME_TILE, 11, HEAP_ID_FIELD1);
        sStep = I_YESNO;
        break;
    case I_YESNO: {
        if (gone || sReply == INVITE_CANCEL || time(NULL) >= sUntil) {
            Clear2dMenuWindowAndDelete(sYesNo, HEAP_ID_FIELD1);
            sYesNo = NULL;
            if (!gone) Send(sPeer, INVITE_NO); /* harmless after a cancel; tells the inviter on a timeout */
            if (gone) Say(fs, "The other player went offline.");
            else if (sReply == INVITE_CANCEL) Say(fs, "%.16s took back\nthe invitation.", Name(sPeer));
            sStep = gone || sReply == INVITE_CANCEL ? T_NOTE : T_END;
            break;
        }
        int r = Handle2dMenuInput_DeleteOnFinish(sYesNo, HEAP_ID_FIELD1);
        if (r == LIST_NOTHING_CHOSEN) break;
        sYesNo = NULL;
        if (r == 0 && sKind == PC_LINK_DOUBLE && Able(fs) < 2) {
            Send(sPeer, INVITE_NO);
            Say(fs, "A double battle needs two\nPok" E_ACUTE "mon that can fight.");
            sStep = T_NOTE;
        } else if (r == 0) {
            Send(sPeer, INVITE_YES);
            pc_link_start(sKind, 0, sPeer); /* joins the inviter's link once this task ends */
            sStep = T_END;
        } else {
            Send(sPeer, INVITE_NO);
            sStep = T_END;
        }
        break;
    }
    case T_NOTE:
        if (Printed() && (keys & (PAD_BUTTON_A | PAD_BUTTON_B))) sStep = T_END;
        break;
    case T_END:
        if (WindowIsInUse(&sWin)) {
            if (!Printed()) RemoveTextPrinter(sPrinter);
            ClearFrameAndWindow2(&sWin, FALSE);
            RemoveWindow(&sWin);
        }
        fs->textbox_open = FALSE;
        String_Delete(sText);
        MessageFormat_Delete(sFmt);
        sState = IDLE;
        sPeer = -1;
        return TRUE;
    }
    return FALSE;
}

static void StartTask(FieldSystem *fs, int step) {
    InitWindow(&sWin);
    sText = String_New(160, HEAP_ID_FIELD1);
    sFmt = MessageFormat_New(HEAP_ID_FIELD1);
    sStep = step;
    sState = BUSY;
    FieldSystem_CreateTask(fs, ChallengeTask, NULL);
}

/* From pc_input_set: the keys the game will see (all of them; the boxes are the game's own). */
u16 pc_challenge_keys(u16 held) {
    sNew = held & ~sHeld;
    sHeld = held;
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
        if (sState != IDLE || pc_link_busy() || kind < PC_LINK_TRADE || kind > PC_LINK_DOUBLE) { /* busy */
            int keep = sKind;
            sKind = kind;
            Send(from, INVITE_NO);
            sKind = keep;
            return;
        }
        sPeer = from;
        sKind = kind;
        sReply = 0;
        sState = ASKED;
        sUntil = time(NULL) + 3; /* to become free */
    } else if (from == sPeer && sState == ASKED && what == INVITE_CANCEL) {
        sState = IDLE;
        sPeer = -1;
    } else if (from == sPeer && sState == BUSY) {
        sReply = what;
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
    if (sState == BUSY || (sState == IDLE && !(pressed & PAD_BUTTON_A))) return;
    FieldSystem *fs = pc_field_system();
    /* free: the overworld runs with nothing else going on, and the player stands on a tile */
    BOOL ready = fs != NULL && fs->processManager != NULL && fs->taskman == NULL && FieldSystem_IsPlayerMovementAllowed(fs) &&
                fs->location->mapId > MAP_WIFI_MULTI_BATTLE_AREA && !pc_link_busy();
    if (ready) {
        u32 move = PlayerAvatar_GetPlayerMoveState(fs->playerAvatar);
        ready = move == PLAYER_MOVE_STATE_END || move == PLAYER_MOVE_STATE_NONE;
    }
    if (sState == IDLE) {
        int id = ready ? Facing(fs) : -1;
        if (id < 0) return;
        sPeer = id;
        StartTask(fs, T_ASK);
    } else if (sState == ASKED) {
        if (pc_net_remote(sPeer) == NULL) {
            sState = IDLE;
            sPeer = -1;
        } else if (ready) {
            sUntil = time(NULL) + 30; /* to answer */
            StartTask(fs, I_ASK);
        } else if (time(NULL) >= sUntil) {
            Send(sPeer, INVITE_NO);
            sState = IDLE;
            sPeer = -1;
        }
    }
}
