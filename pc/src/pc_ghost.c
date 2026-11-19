/* Other players in the overworld: everyone pc_net.c hears about, plus HG_GHOST's fake one.
 *
 * Each remote player is a map object (id PC_REMOTE_ID + slot) with the Ethan/Lyra sprite of their
 * gender, walking between tiles with the game's own movement commands, so it animates like an NPC.
 * Their following Pokemon, when their game shows one, is another (id PC_REMOTE_MON_ID + slot) with
 * that species' overworld sprite (form, gender and shininess too), walking to the tile it stands on
 * in their game. The local player's position and follower go to pc_net.c whenever they change.
 *
 * HG_GHOST=N   a fake remote player (slot 15) replays your own position stream N frames late
 *              (default 48, about one second): multiplayer spike S2, and a test without a server.
 * HG_GHOST_LOG=1 logs each creation, =2 also slot 15's position every 30 frames, =3 also each tile
 * this player moves to and each tile a remote figure walks onto (to compare paths across games).
 *
 * Lifecycle rules:
 *  - Objects are only touched while the player could move (FieldSystem_IsPlayerMovementAllowed): no
 *    script, menu, battle or map load in progress. Otherwise they are left alone, frozen.
 *  - A remote player lives on the map its stream names. On another map: removed, unless both maps
 *    are on the main matrix (outdoors, where maps join seamlessly; MAPOBJECTFLAG_KEEP stops the game
 *    removing it at a border). Same map and no object: created. More than JUMP tiles away: removed,
 *    then created at the target next frame (teleport). Gone from the server: removed. A changed look
 *    (gender, follower species...) also recreates it.
 *  - It walks the steps the player took, in order (PcRemote.path), running when 2 or more behind;
 *    more than PC_PATH - 4 behind, it starts again at the latest position. Its Pokemon likewise.
 *  - It doesn't appear on a tile another object stands on (the player, the following Pokemon, NPCs).
 *    When one is on its next step it waits, and after STUCK frames steps onto it anyway, so it can't
 *    stay stuck behind someone. A remote player and their own Pokemon may pass each other.
 *    Talking to them does nothing.
 *  - Not drawn in the Union Room and link rooms (maps 2..5): there the game shows players itself.
 *  - They are never written to the save: FieldSystem_SyncMapObjectsToSaveEx skips ids 208..239
 *    (src/map_object.c, SDK_PC). Battles and full-screen menus (Options) keep them in place.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "global.h"
#include "constants/global_fieldmap.h"
#include "constants/maps.h"
#include "constants/movements.h"
#include "constants/species.h"
#include "constants/sprites.h"
#include "field_system.h"
#include "follow_mon.h"
#include "map_header.h"
#include "map_object.h"
#include "player_avatar.h"
#include "unk_02062108.h"
#include "pc_net_proto.h"
#include "pc_platform.h"

FieldSystem *pc_field_system(void); /* src/field_system.c, SDK_PC */

#define TRAIL 256
#define JUMP  6

#define FAKE  (NET_MAX_PLAYERS - 1)

typedef struct {
    int map, x, z, dir, gender, state;
    int monSpecies, monForm, monFlags, monX, monZ, monDir; /* monSpecies 0: no follower shown */
} Sample;

static Sample sTrail[TRAIL];
static unsigned sHead, sCount;
static int sLog, sAny, sResend;
static Sample sSent;
static int sObjKey[2][NET_MAX_PLAYERS]; /* what each object was created as: [0] player gender, [1] follower */

/* Another map object (the player, a follower Pokemon, an NPC; not `pass`) stands on x,z. */
static BOOL Occupied(MapObjectManager *man, LocalMapObject *self, int x, int z, LocalMapObject *pass) {
    LocalMapObject *other = MapObjectManager_GetFirstObjectWithXAndZ(man, x, z, FALSE);
    return other != NULL && other != self && other != pass;
}

int pc_ghost_step(MapObjectManager *man, LocalMapObject *o, int x, int z, int dir, LocalMapObject *pass, int fast) {
    if (!MapObject_AreBitsSetForMovementScriptInit(o)) return 0; /* mid-step */
    MapObject_ClearHeldMovementIfActive(o);
    int ox = MapObject_GetXCoord(o), oz = MapObject_GetZCoord(o);
    int dx = x - ox, dz = z - oz, dist = abs(dx) + abs(dz);
    if (dist == 0) {
        if ((int)MapObject_GetFacingDirection(o) != dir) MapObject_SetHeldMovement(o, MOVEMENT_FACE_UP + dir);
        return 0;
    }
    if (dist > JUMP) return dist; /* the caller removes or moves it */
    int d = dx > 0 ? DIR_EAST : dx < 0 ? DIR_WEST : dz > 0 ? DIR_SOUTH : DIR_NORTH;
    int nx = ox + (d == DIR_EAST) - (d == DIR_WEST), nz = oz + (d == DIR_SOUTH) - (d == DIR_NORTH);
    if (Occupied(man, o, nx, nz, pass)) return -1; /* wait, e.g. behind the player and their Pokemon */
    MapObject_SetHeldMovement(o, (dist > 1 || fast ? MOVEMENT_RUN_UP : MOVEMENT_STEP_UP) + d);
    return dist;
}

/* Remote figures walk the way the player went, a tile at a time (PcRemote.path), not straight to the
 * latest position: a straight line can lead behind an NPC or the local player and leave them standing
 * there while the real player walked around. */
#define BEHIND_RUN 2  /* steps behind: run */
#define STUCK      40 /* frames blocked on one step: move onto it anyway */

typedef struct {
    const PcStep *path; /* NULL: no path, straight to the target */
    unsigned in, *out;  /* steps heard / walked */
    int *blocked;       /* frames waiting on the current step */
} Path;

static void Untalkable(LocalMapObject *o, int id) {
    MapObject_SetID(o, id);
    MapObject_SetType(o, 0);
    MapObject_SetEventFlag(o, 0);
    MapObject_SetScriptID(o, 0);
}

static LocalMapObject *CreateFigure(MapObjectManager *man, int id, int x, int z, int dir, int gender, int map) {
    /* shortcut: always the walking sprite; cycling/surfing players show walking */
    LocalMapObject *o = MapObject_Create(man, x, z, dir, gender ? SPRITE_HEROINE : SPRITE_HERO, 0, map);
    if (o == NULL) return NULL;
    Untalkable(o, id);
    MapObject_SetFlagsBits(o, MAPOBJECTFLAG_UNK19 | MAPOBJECTFLAG_KEEP); /* UNK19: not talkable (sub_0203DC64), else A runs script 0 */
    return o;
}

/* Like FollowMon_CreateMapObject (src/follow_mon.c), but standing still unless driven: movement 0,
 * not the follow-the-player movement. key = species | form << 16 | flags << 24. */
static LocalMapObject *CreateFollower(MapObjectManager *man, int id, int x, int z, int dir, int key, int map) {
    int species = key & 0xFFFF, form = (key >> 16) & 0xFF, flags = key >> 24;
    LocalMapObject *o = MapObject_Create(man, x, z, dir, FollowMon_GetSpriteID(species, form, (flags >> 1) & 3), 0, map);
    if (o == NULL) return NULL;
    Untalkable(o, id);
    MapObject_SetParam(o, 0, 2);
    FollowMon_SetObjectParams(o, species, form, flags & 1);
    MapObject_SetFlagsBits(o, MAPOBJECTFLAG_UNK13 | MAPOBJECTFLAG_KEEP | MAPOBJECTFLAG_UNK19);
    MapObject_ClearFlagsBits(o, MAPOBJECTFLAG_UNK8 | MAPOBJECTFLAG_UNK7);
    MapObject_SetFlag29(o, TRUE);
    sub_02069DC8(o, FALSE); /* shown (TRUE would hide it: the game shows its own follower later) */
    return o;
}

typedef LocalMapObject *(*CreateFn)(MapObjectManager *, int id, int x, int z, int dir, int key, int map);

/* Keeps object `id` at map/x/z/dir looking like `key`; key < 0: it shouldn't exist. With a path, it gets
 * there along the steps heard. */
static void Drive(FieldSystem *fs, int id, int key, int *objKey, int map, int x, int z, int dir, const Sample *me, CreateFn create, LocalMapObject *pass,
                  const Path *pa) {
    MapObjectManager *man = fs->mapObjectManager;
    LocalMapObject *o = MapObjectManager_GetFirstActiveObjectByID(man, id);
    if (o != NULL && key != *objKey) {
        MapObject_Remove(o);
        return;
    }
    if (key < 0) return;
    /* Maps on the main matrix (the outdoor world) share coordinates and join seamlessly. */
    if (map != me->map && !(MapHeader_MapIsOnMainMatrix(map) && MapHeader_MapIsOnMainMatrix(me->map))) {
        if (o != NULL) MapObject_Remove(o);
        return;
    }
    if (o == NULL) {
        if (Occupied(man, NULL, x, z, pass)) return;
        if (create(man, id, x, z, dir, key, me->map) == NULL) return; /* every object slot of this map is taken */
        sAny = 1;
        *objKey = key;
        if (pa->path != NULL) *pa->out = pa->in; /* it starts at the latest position */
        if (sLog) fprintf(stderr, "hg_pc: ghost: object %d created on map %d at %d,%d\n", id, map, x, z);
        return;
    }
    static int n;
    if (sLog > 1 && id == PC_REMOTE_ID + FAKE && ++n % 30 == 0) {
        fprintf(stderr, "hg_pc: ghost: at %d,%d target %d,%d flags %08x ready %d\n", (int)MapObject_GetXCoord(o), (int)MapObject_GetZCoord(o), x, z,
            (unsigned)MapObject_GetFlags(o), MapObject_AreBitsSetForMovementScriptInit(o));
    }
    int fast = 0;
    if (pa->path != NULL) {
        unsigned *out = pa->out;
        if (*out > pa->in) *out = pa->in; /* the remote's list restarted (reconnected) */
        if (pa->in - *out > PC_PATH - 4) { /* far behind: the ring overwrote steps; start again at the latest */
            MapObject_Remove(o);
            return;
        }
        if (!MapObject_AreBitsSetForMovementScriptInit(o)) return; /* mid-step */
        int ox = MapObject_GetXCoord(o), oz = MapObject_GetZCoord(o);
        while (*out != pa->in && pa->path[*out % PC_PATH].x == ox && pa->path[*out % PC_PATH].z == oz) (*out)++; /* reached */
        if (*out != pa->in) {
            const PcStep *s = &pa->path[*out % PC_PATH];
            x = s->x;
            z = s->z;
            dir = s->dir;
            fast = pa->in - *out >= BEHIND_RUN;
        }
    }
    int left = pc_ghost_step(man, o, x, z, dir, pass, fast);
    if (left > JUMP) {
        MapObject_Remove(o);
    } else if (left < 0 && pa->path != NULL && ++*pa->blocked > STUCK) { /* someone stands on the step: step onto it anyway */
        MapObject_SetPositionFromXYZAndDirection(o, x, MapObject_GetYCoord(o), z, dir);
        *pa->blocked = 0;
    } else if (left >= 0 && pa->path != NULL) {
        *pa->blocked = 0;
    }
}

/* A remote follower's look, or -1 for none; other games' data is checked (a bad species would read
 * past the sprite tables). */
static int MonKey(const Sample *t) {
    if (t->monSpecies <= 0 || t->monSpecies > NATIONAL_DEX_COUNT) return -1;
    int form = t->monForm <= OverworldModelLookupFormCount(t->monSpecies) ? t->monForm : 0;
    return t->monSpecies | form << 16 | (t->monFlags & 7) << 24;
}

static void DriveSlot(FieldSystem *fs, int slot, const Sample *t, const Sample *me, const PcRemote *r) {
    static unsigned sOut[2][NET_MAX_PLAYERS];
    static int sBlocked[2][NET_MAX_PLAYERS];
    MapObjectManager *man = fs->mapObjectManager;
    LocalMapObject *player = MapObjectManager_GetFirstActiveObjectByID(man, PC_REMOTE_ID + slot);
    LocalMapObject *mon = MapObjectManager_GetFirstActiveObjectByID(man, PC_REMOTE_MON_ID + slot);
    Sample none = { 0 };
    if (t == NULL) t = &none;
    int playerKey = t == &none ? -1 : t->gender, monKey = t == &none ? -1 : MonKey(t);
    Path figurePath = { r != NULL ? r->path : NULL, r != NULL ? r->pathIn : 0, &sOut[0][slot], &sBlocked[0][slot] };
    Path monPath = { r != NULL ? r->monPath : NULL, r != NULL ? r->monPathIn : 0, &sOut[1][slot], &sBlocked[1][slot] };
    Drive(fs, PC_REMOTE_ID + slot, playerKey, &sObjKey[0][slot], t->map, t->x, t->z, t->dir, me, CreateFigure, mon, &figurePath);
    Drive(fs, PC_REMOTE_MON_ID + slot, monKey, &sObjKey[1][slot], t->map, t->monX, t->monZ, t->monDir, me, CreateFollower, player, &monPath);
}

void pc_ghost_resend(void) { sResend = 1; }

void pc_ghost_frame(void) {
    static int delay = -1;
    if (delay < 0) {
        const char *e = getenv("HG_GHOST");
        delay = e == NULL ? 0 : atoi(e) > 0 ? atoi(e) : 48;
        if (delay >= TRAIL) delay = TRAIL - 1;
        sLog = getenv("HG_GHOST_LOG") ? atoi(getenv("HG_GHOST_LOG")) : 0;
    }
    if (delay == 0 && !sAny && !pc_net_wanted()) return; /* single player: touch nothing */
    FieldSystem *fs = pc_field_system();
    if (fs == NULL || fs->processManager == NULL || !FieldSystem_IsPlayerMovementAllowed(fs)) return;
    PlayerAvatar *pa = fs->playerAvatar;
    Sample me = { fs->location->mapId, PlayerAvatar_GetXCoord(pa), PlayerAvatar_GetZCoord(pa), PlayerAvatar_GetFacingDirection(pa),
                  PlayerAvatar_GetGender(pa), PlayerAvatar_GetState(pa) };
    LocalMapObject *follower = FollowMon_GetMapObject(fs);
    if (follower != NULL && FollowMon_IsVisible(fs)) {
        me.monSpecies = fs->followMon.species;
        me.monForm = fs->followMon.form;
        me.monFlags = (fs->followMon.shiny ? 1 : 0) | (fs->followMon.gender & 3) << 1;
        me.monX = MapObject_GetXCoord(follower);
        me.monZ = MapObject_GetZCoord(follower);
        me.monDir = MapObject_GetFacingDirection(follower);
    }
    if (sResend || memcmp(&me, &sSent, sizeof me) != 0) {
        if (sLog > 2 && (me.x != sSent.x || me.z != sSent.z)) fprintf(stderr, "hg_pc: ghost: me %d,%d\n", me.x, me.z);
        sResend = 0;
        sSent = me;
        NetPos p = { 0, (uint8_t)me.dir, (uint8_t)me.gender, (uint8_t)me.state, (uint16_t)me.map, (uint16_t)me.x, (uint16_t)me.z,
                     (uint16_t)me.monSpecies, (uint8_t)me.monForm, (uint8_t)me.monFlags, (uint16_t)me.monX, (uint16_t)me.monZ, (uint8_t)me.monDir };
        pc_net_send_pos(&p);
    }
    for (int slot = 0; slot < NET_MAX_PLAYERS; slot++) {
        const PcRemote *r = pc_net_remote(slot);
        Sample t, *tp = NULL;
        if (slot == FAKE && delay > 0) {
            r = NULL; /* replays every frame's position: no path needed */
            sTrail[sHead++ % TRAIL] = me;
            if (sCount < TRAIL) sCount++;
            if (sCount > (unsigned)delay) tp = &sTrail[(sHead - 1 - delay) % TRAIL];
        } else if (r != NULL && r->hasPos && me.map > MAP_WIFI_MULTI_BATTLE_AREA) { /* Union Room, link rooms: the game draws players itself */
            t = (Sample){ r->map, r->x, r->z, r->dir, r->gender, r->state, r->monSpecies, r->monForm, r->monFlags, r->monX, r->monZ, r->monDir };
            tp = &t;
        }
        DriveSlot(fs, slot, tp, &me, tp != NULL ? r : NULL);
        if (sLog > 2) { /* HG_GHOST_LOG=3: each tile a remote figure walks onto */
            static int lastX[NET_MAX_PLAYERS], lastZ[NET_MAX_PLAYERS];
            LocalMapObject *o = MapObjectManager_GetFirstActiveObjectByID(fs->mapObjectManager, PC_REMOTE_ID + slot);
            if (o != NULL && ((int)MapObject_GetXCoord(o) != lastX[slot] || (int)MapObject_GetZCoord(o) != lastZ[slot])) {
                lastX[slot] = MapObject_GetXCoord(o);
                lastZ[slot] = MapObject_GetZCoord(o);
                fprintf(stderr, "hg_pc: ghost: figure %d %d,%d\n", slot, lastX[slot], lastZ[slot]);
            }
        }
    }
}
