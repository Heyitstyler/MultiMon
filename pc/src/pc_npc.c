/* NPCs move the same for every player on a map (multiplayer).
 *
 * Each game walks and turns its NPCs with its own random numbers, so without this they wander
 * differently for each player. The server names a leader for each map: of the players on it, the one
 * there longest (NM_LEAD). The leader's game sends where that map's NPCs stand and face (NM_NPC)
 * whenever one moves or turns, and every 2 seconds anyway. The other games on that map pause those
 * NPCs' own movement and walk them there with the same steps as remote players (pc_ghost_step),
 * teleporting them when more than a few tiles off. Without fresh data for a map (3 seconds: the leader
 * left, or is in a script or menu), its NPCs move on their own again.
 *
 * NPCs here are the map's event objects that move by themselves (movement type not 0): wanderers,
 * walkers and the ones that look around. Objects that stand still (boulders, story characters, item
 * balls) are left alone, and so are players, followers and other players' figures.
 * Objects are only touched while the player could move (as in pc_ghost.c): scripts run as usual,
 * including ones that walk a synced NPC; afterwards it walks back to where the leader has it.
 * Not in the Union Room and link rooms (maps 2..5), whose figures are other players.
 * HG_NPC_OFF=1 turns it off (scripted tests that rely on a game's own random NPC moves).
 * HG_NPC_LOG=1 logs leadership and what is sent; =2 also each followed NPC against the leader, once a second.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "global.h"
#include "constants/maps.h"
#include "field_system.h"
#include "map_object.h"
#include "player_avatar.h"
#include "pc_net_proto.h"
#include "pc_platform.h"

FieldSystem *pc_field_system(void); /* src/field_system.c, SDK_PC */

#define JUMP      6
#define RESEND    120 /* frames: send unchanged positions again */
#define FRESH     180 /* frames: follow data this old at most */
#define MAPS      4   /* maps followed at once (outdoors, neighbours are loaded too) */

static int sLead, sLeadMap, sLog = -1;
static unsigned long sFrame;
static struct {
    unsigned long at;
    NetNpc n;
} sFollow[MAPS];
static struct {
    int map, id, used;
} sHeld[NET_NPC_MAX * 2];
static int sHeldCount;
static NetNpc sSent;
static unsigned long sSentAt;

void pc_npc_lead(int lead, int map) {
    if (sLog > 0 && (lead != sLead || map != sLeadMap)) fprintf(stderr, "hg_pc: npc: %s map %d\n", lead ? "leading" : "not leading", map);
    sLead = lead;
    sLeadMap = map;
}

void pc_npc_receive(const void *body, int len) {
    NetNpc n;
    if (len < NET_NPC_SIZE(0) || len > (int)sizeof n) return;
    memcpy(&n, body, len);
    if (n.count > NET_NPC_MAX || len != NET_NPC_SIZE(n.count)) return;
    int slot = 0;
    for (int i = 0; i < MAPS; i++) {
        if (sFollow[i].at != 0 && sFollow[i].n.map == n.map) {
            slot = i;
            break;
        }
        if (sFollow[i].at < sFollow[slot].at) slot = i;
    }
    sFollow[slot].n = n;
    sFollow[slot].at = sFrame + 1; /* 0 means empty */
}

static BOOL Synced(LocalMapObject *o) {
    return MapObject_GetID(o) < PC_REMOTE_MON_ID && MapObject_GetMovement(o) != 0 && MapObject_GetMovement(o) != 0x30; /* 0x30: a following Pokemon */
}

static LocalMapObject *Find(MapObjectManager *man, int map, int id) {
    LocalMapObject *o;
    s32 i = 0;
    while (MapObjectManager_GetNextObjectWithFlagFromIndex(man, &o, &i, MAPOBJECTFLAG_ACTIVE)) {
        if ((int)MapObject_GetID(o) == id && (int)MapObject_GetMapID(o) == map && Synced(o)) return o;
    }
    return NULL;
}

/* Lets every NPC this game was holding move on its own again (those still loaded). */
static void ReleaseUnused(MapObjectManager *man) {
    int keep = 0;
    for (int i = 0; i < sHeldCount; i++) {
        if (sHeld[i].used) {
            sHeld[i].used = 0;
            sHeld[keep++] = sHeld[i];
            continue;
        }
        LocalMapObject *o = man != NULL ? Find(man, sHeld[i].map, sHeld[i].id) : NULL;
        if (o != NULL) MapObject_UnpauseMovement(o);
    }
    sHeldCount = keep;
}

static void Hold(LocalMapObject *o, int map, int id) {
    if (!MapObject_CheckMovementPaused(o)) MapObject_PauseMovement(o); /* again after a script let everyone go */
    for (int i = 0; i < sHeldCount; i++) {
        if (sHeld[i].map == map && sHeld[i].id == id) {
            sHeld[i].used = 1;
            return;
        }
    }
    if (sHeldCount < (int)(sizeof sHeld / sizeof sHeld[0])) {
        sHeld[sHeldCount].map = map;
        sHeld[sHeldCount].id = id;
        sHeld[sHeldCount++].used = 1;
    }
}

static void Send(MapObjectManager *man, int map) {
    NetNpc n;
    memset(&n, 0, sizeof n);
    n.map = (uint16_t)map;
    LocalMapObject *o;
    s32 i = 0;
    while (n.count < NET_NPC_MAX && MapObjectManager_GetNextObjectWithFlagFromIndex(man, &o, &i, MAPOBJECTFLAG_ACTIVE)) {
        if ((int)MapObject_GetMapID(o) != map || !Synced(o)) continue;
        n.obj[n.count++] = (NetNpcObj){ (uint8_t)MapObject_GetID(o), (uint8_t)MapObject_GetFacingDirection(o), (uint16_t)MapObject_GetXCoord(o), (uint16_t)MapObject_GetZCoord(o) };
    }
    int len = NET_NPC_SIZE(n.count);
    if (len == NET_NPC_SIZE(sSent.count) && !memcmp(&n, &sSent, len) && sFrame - sSentAt < RESEND) return;
    if (sLog > 1 || (sLog > 0 && sFrame - sSentAt >= RESEND)) fprintf(stderr, "hg_pc: npc: sending %d on map %d\n", n.count, map);
    pc_net_send_npc(&n, len);
    sSent = n;
    sSentAt = sFrame;
}

void pc_npc_frame(void) {
    sFrame++;
    static int off = -1;
    if (off < 0) off = getenv("HG_NPC_OFF") != NULL;
    if (off) return;
    if (sLog < 0) sLog = getenv("HG_NPC_LOG") ? atoi(getenv("HG_NPC_LOG")) : 0;
    FieldSystem *fs = pc_field_system();
    if (fs == NULL || fs->processManager == NULL || !FieldSystem_IsPlayerMovementAllowed(fs)) return;
    MapObjectManager *man = fs->mapObjectManager;
    int myMap = fs->location->mapId;
    if (!pc_net_online() || myMap <= MAP_WIFI_MULTI_BATTLE_AREA) { /* Union Room, link rooms: their figures are players */
        if (sHeldCount > 0) ReleaseUnused(man);
        return;
    }
    if (sLead && sLeadMap == myMap) Send(man, myMap);
    for (int f = 0; f < MAPS; f++) {
        const NetNpc *n = &sFollow[f].n;
        if (sFollow[f].at == 0 || sFrame - sFollow[f].at > FRESH || (sLead && n->map == sLeadMap)) continue;
        int followed = 0;
        for (int k = 0; k < n->count; k++) {
            const NetNpcObj *e = &n->obj[k];
            LocalMapObject *o = Find(man, n->map, e->obj);
            if (o == NULL) continue; /* hidden or gone in this save */
            Hold(o, n->map, e->obj);
            followed++;
            if (pc_ghost_step(man, o, e->x, e->z, e->dir & 3, NULL, 0) > JUMP) {
                MapObject_SetPositionFromXYZAndDirection(o, e->x, MapObject_GetYCoord(o), e->z, e->dir & 3);
            }
            if (sLog > 1 && sFrame % 60 == 0) {
                fprintf(stderr, "hg_pc: npc: %d at %d,%d facing %d, leader %d,%d facing %d\n", e->obj, (int)MapObject_GetXCoord(o),
                        (int)MapObject_GetZCoord(o), (int)MapObject_GetFacingDirection(o), e->x, e->z, e->dir);
            }
        }
        if (sLog > 1 && sFrame % RESEND == 0) fprintf(stderr, "hg_pc: npc: following %d on map %d\n", followed, n->map);
    }
    ReleaseUnused(man);
}
