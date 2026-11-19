/* Development aids that reach into game state. They act once, when the field is idle (no script
 * or menu running) after frame 1400, i.e. after Continue.
 *
 * HG_WARP=map,warp     warp to warp point `warp` of map `map` (constants/maps.h ids), or to tile
 * HG_WARP=map,-1,x,y   x,y, with the game's own warp task, so a scripted test can start anywhere.
 * HG_WARP=union        straight into the Union Room from wherever the save stands (pc_challenge.c).
 *                      E.g. HG_WARP=69,-1,6,6: Cherrygrove Pokemon Center 1F, below the Union Room desk.
 * HG_GIVEMON=species,level   add a Pokemon to the party (as the script command does), e.g. for a
 *                      trade test with a one-Pokemon save: HG_GIVEMON=16,5 (Pidgey).
 * HG_UNION_TALK=frame  in the Union Room, from that frame on (once, when idle): talk to another
 *                      player's figure as A in front of it would, wherever it has wandered.
 * HG_PARTY_LOG=frame   at that frame, log the party's species ("hg_pc: party: 152 161"), e.g. to
 *                      check a trade.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "global.h"
#include "field_system.h"
#include "field_warp_tasks.h"
#include "map_header.h"
#include "party.h"
#include "pokemon.h"
#include "script_pokemon_util.h"
#include "pc_platform.h"

FieldSystem *pc_field_system(void); /* src/field_system.c, SDK_PC */

void pc_debug_frame(unsigned long frame) {
    static int done, talked;
    const char *talk = getenv("HG_UNION_TALK");
    if (talk != NULL && !talked && frame >= strtoul(talk, NULL, 10)) {
        FieldSystem *fs = pc_field_system();
        if (fs != NULL && pc_challenge_talk_figure(fs)) {
            talked = 1;
            fprintf(stderr, "hg_pc: HG_UNION_TALK: talking to a figure at frame %lu\n", frame);
        }
    }
    const char *party = getenv("HG_PARTY_LOG");
    if (party != NULL && frame == strtoul(party, NULL, 10) && pc_field_system() != NULL) {
        Party *p = SaveArray_Party_Get(pc_field_system()->saveData);
        fprintf(stderr, "hg_pc: party:");
        for (int i = 0; i < Party_GetCount(p); i++) fprintf(stderr, " %d", (int)GetMonData(Party_GetMonByIndex(p, i), MON_DATA_SPECIES, NULL));
        fprintf(stderr, "\n");
    }
    const char *w = getenv("HG_WARP"), *g = getenv("HG_GIVEMON");
    if (done || (w == NULL && g == NULL) || frame < 1400) return;
    FieldSystem *fs = pc_field_system();
    if (fs == NULL || fs->taskman != NULL) return; /* wait until no field task (script, menu) runs */
    done = 1;
    int species, level;
    if (g != NULL && sscanf(g, "%d,%d", &species, &level) == 2) {
        BOOL ok = GiveMon(HEAP_ID_FIELD2, fs->saveData, species, level, 0, 0, 0, MapHeader_GetMapSec(fs->location->mapId), 24);
        fprintf(stderr, "hg_pc: HG_GIVEMON: species %d level %d %s\n", species, level, ok ? "added" : "failed (party full?)");
    }
    if (w != NULL && !strcmp(w, "union")) { /* HG_WARP=union: straight into the Union Room, as an accepted challenge does */
        pc_challenge_enter_union(fs);
        return;
    }
    int map, warp, x = -1, y = -1;
    if (w != NULL && sscanf(w, "%d,%d,%d,%d", &map, &warp, &x, &y) >= 2) {
        sub_020538C0(fs, (u32)map, warp, x, y, 0);
        fprintf(stderr, "hg_pc: HG_WARP: warping to map %d, warp %d (%d,%d)\n", map, warp, x, y);
    }
}
