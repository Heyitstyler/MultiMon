/* Host platform layer shared declarations (PC port). */
#ifndef PC_PLATFORM_H
#define PC_PLATFORM_H

#include <nitro/types.h>

/* pc_mem.c: reserve the DS address space (main RAM, IO, palette, VRAM, OAM) at its real addresses. */
void pc_mem_init(void);
void pc_mem_protect_page(u32 addr, BOOL writable); /* read-only trap page or writable */
BOOL pc_mem_copy_ok(const void *src, const void *dest, u32 size); /* FALSE (and logged) for junk pointers */

/* pc_rom.c: the user's own ROM, read at runtime for game data. */
void pc_rom_open(const char *path);
void pc_rom_read(u32 offset, void *dst, u32 size);

/* pc_backup.c: save flash backed by <rom>.sav (or $HG_SAVE). */
void pc_backup_open(const char *romPath);

/* pc_dgt.c: aborts if MD5/SHA-1 give wrong known answers. */
void pc_dgt_selftest(void);

/* pc_input.c: keypad registers and touch state. */
void pc_input_set(u16 heldPadKeys, BOOL touching, u16 x, u16 y);
void pc_input_frame(void);

/* pc_mwrt.c: Metrowerks runtime helpers (division, soft-float); nonzero if the self-test passes. */
int pc_mwrt_selftest(void);

/* pc_g3.c: geometry engine. Port offsets are DS ones (0x400 FIFO, 0x440-0x5FF commands). */
void pc_g3_init(void);
int pc_g3_selftest(void); /* nonzero if matrix ordering and the trap path work */
void pc_g3_write(u32 offset, u32 value);
BOOL pc_g3_is_port(const volatile void *addr); /* trap-page or real 0x04000400-0x040005FF */
void pc_g3_send(const u32 *words, u32 count); /* words in packed FIFO format */

/* pc_g3r.c: rasterizer. Renders a finished polygon list into the 3D frame buffer. */
typedef struct {
    float x, y, z, w;   /* screen x/y (pixels), depth value, w for perspective */
    float r, g, b;      /* vertex colour, 0-31 */
    float s, t;         /* texcoords in texels */
} PcG3Vertex;
typedef struct {
    PcG3Vertex v[10];
    int n;
    u32 attr, texParam, plttBase;
    u8 translucent, wbuffer;
    float ytop, ybottom;
} PcG3Poly;
void pc_g3r_render(PcG3Poly *polys, int count, BOOL manualSort, BOOL wbuffer);
const u16 *pc_g3r_color(void); /* 256x192 RGB555 */
const u8 *pc_g3r_alpha(void);  /* 256x192, 0 = no 3D pixel, 1-31 */

/* pc_cp.c: divider and square-root unit; nonzero if the self-test passes. */
int pc_cp_selftest(void);

/* pc_menu.c: options menu (Esc in the window) and hg_pc.cfg next to the executable. */
extern char **pc_argv;
void pc_cfg_load(void);
int pc_cfg_volume(void);           /* percent */
float pc_cfg_speed(void);          /* 0.25-4x, 0 = as fast as possible (the fast-forward speed while its key is held) */
/* Bindable actions: the DS buttons in this order, then fast forward and chat. */
enum { PC_ACT_A, PC_ACT_B, PC_ACT_X, PC_ACT_Y, PC_ACT_L, PC_ACT_R, PC_ACT_START, PC_ACT_SELECT,
       PC_ACT_UP, PC_ACT_DOWN, PC_ACT_LEFT, PC_ACT_RIGHT, PC_ACT_FF, PC_ACT_CHAT, PC_ACT_COUNT };
int pc_cfg_key(int action);        /* SDL scancode bound to the action */
void pc_menu_fast_forward(int held);
const char *pc_cfg_rom(void);      /* remembered ROM path, or NULL */
void pc_cfg_set_rom(const char *path);
const char *pc_menu_pick_rom(void); /* startup file picker; NULL if cancelled or headless */
BOOL pc_rom_check(const char *path); /* pc_rom.c: is it HeartGold (US)? */
const char *pc_cfg_save_path(void); /* NULL if none chosen */
void pc_menu_run(void *window, void *renderer, void *frameTexture); /* SDL objects; pauses until closed */
void pc_menu_window_ready(void *window);
void pc_menu_shot(void *renderer, void *frameTexture, const char *path); /* HG_MENU_SHOT test */
void pc_watchdog_kick(void);       /* pc_main.c: a paused-but-alive frame */
const char *pc_backup_path(void);  /* pc_backup.c: the save file in use */
BOOL pc_backup_copy_to(const char *path);
void pc_snd_set_volume(int percent);

/* pc_debug.c: development aids (HG_WARP), run at each frame end. */
void pc_debug_frame(unsigned long frame);
/* pc_link.c: trades and battles in the overworld over a club link (no link room) */
void pc_link_frame(unsigned long frame);
enum { PC_LINK_TRADE, PC_LINK_SINGLE, PC_LINK_DOUBLE };
int pc_link_start(int kind, int leader, int peer); /* 0: one is already going; starts when the field is idle */
void pc_link_partner(int from, int what);          /* the partner's NM_INVITE during a link */
int pc_link_busy(void);
u16 pc_link_keys(u16 held);              /* answers the club prompts while the link forms */

/* pc_ghost.c: other players drawn in the overworld, run at each frame end. Their map objects use ids
 * PC_REMOTE_ID..PC_REMOTE_ID+15, their following Pokemon PC_REMOTE_MON_ID..+15; src/map_object.c
 * keeps 208..239 out of the save by the same numbers. */
#define PC_REMOTE_ID     224
#define PC_REMOTE_MON_ID 208
void pc_ghost_frame(void);
void pc_ghost_resend(void); /* send the local position again (just joined) */
struct FieldSystem;
struct MapObjectManager;
struct LocalMapObject;
/* One step of `o` toward x,z (or a turn to dir there); waits while another object is in the way,
 * except `pass`. Returns the distance left. */
int pc_ghost_step(struct MapObjectManager *man, struct LocalMapObject *o, int x, int z, int dir, struct LocalMapObject *pass);
/* pc_npc.c: NPCs move the same for players on one map: its leader's game sends, the others follow. */
void pc_npc_frame(void);
void pc_npc_lead(int lead, int map);           /* NM_LEAD */
void pc_npc_receive(const void *body, int len); /* NM_NPC */
/* pc_challenge.c: invite a player you face to the Union Room (multiplayer phase 3) */
void pc_challenge_enter_union(struct FieldSystem *fs);
BOOL pc_challenge_talk_figure(struct FieldSystem *fs); /* any other player's Union Room figure (HG_UNION_TALK) */
void pc_challenge_frame(void);                 /* each frame end */
u16 pc_challenge_keys(u16 held);               /* filters the keys the game sees */
void pc_challenge_message(int from, int what, u32 trainerId, int kind); /* NM_INVITE from the server */
void pc_video_overlay(const char *text);       /* pc_video.c: text box over the top screen; NULL hides it */
/* pc_chat.c: text chat (multiplayer phase 4) */
#define PC_CHAT_COLS 29
void pc_chat_receive(const char *name, const char *text); /* name NULL: a notice */
int pc_chat_is_open(void);
void pc_chat_open(void);
void pc_chat_close(void);
void pc_chat_type(const char *utf8);
void pc_chat_backspace(void);
void pc_chat_enter(void);                      /* send the line and close */
/* Opens the input line for a password: shown as stars, never sent as chat. `answer` gets the text
 * on Enter, NULL on Esc. `question` may hold '\n's. */
void pc_chat_ask(const char *question, void (*answer)(const char *text));
int pc_chat_lines(char out[][PC_CHAT_COLS + 1], int max);
void pc_chat_frame(unsigned long frame);

/* pc_net.c: multiplayer client (pc_net_proto.h). */
typedef struct {
    int active, hasPos;
    char name[17];
    int map, x, z, dir, gender, state;
    int monSpecies, monForm, monFlags, monX, monZ, monDir; /* following Pokemon; monSpecies 0: none shown */
} PcRemote;
void pc_net_poll(void);
void pc_net_connect(void);
void pc_net_disconnect(void);
void pc_net_change_password(void); /* the chat's /password: asks the current and new password */
int pc_net_wanted(void);
const char *pc_net_status(void);
const PcRemote *pc_net_remote(int id); /* NULL: nobody with that id */
struct NetPos;
void pc_net_send_pos(const struct NetPos *p);
void pc_net_send_npc(const void *body, int len);
void pc_net_send_invite(int id, int what, u32 trainerId, int kind);
int pc_net_send_chat(const char *text); /* 0 if not online */
int pc_net_online(void);
void pc_net_send_wm(const void *pkt, int len); /* a pc_wm.c radio packet for the server to deliver */
/* pc_menu.c: multiplayer settings (server=, name=, password=, online= in hg_pc.cfg; $HG_SERVER etc. win) */
const char *pc_cfg_server(void);
const char *pc_cfg_name(void);
const char *pc_cfg_password(void);
int pc_cfg_online(void);

/* pc_wm.c: host ARM7 half of the wireless driver; requests come in over PXI, answers go out per frame. */
void pc_wm_init(void);
void pc_wm_command(u32 addr);
void pc_wm_frame(void);
void pc_wm_online(void);                     /* give this console a MAC address of its own, before connecting */
void pc_wm_deliver(const void *pkt, int len); /* a radio packet from the server */

/* pc_pxi.c: deliver ARM7 replies owed to the ARM9 (wireless power), once per frame. */
void pc_pxi_frame(void);

/* pc_main.c: print why and the call stack to stderr. */
void pc_backtrace(const char *why);

/* pc_overlay.c: load sequence number per overlay ID, 0 = not loaded (read by asm2c's ovl_dispatch.c). */
#define PC_OVL_MAX 512
extern u32 pc_ovl_seq[PC_OVL_MAX];

/* pc_crypto_selftest.c: the game's ARM-mode RC4/RSA code; nonzero if the self-test passes. */
int pc_crypto_selftest(void);

/* pc_cpu.c: ARM register file for translated asm. */
void pc_cpu_init(void);

/* pc_vram.c: VRAM banks behind their CPU views; sync re-reads VRAMCNT after the SDK changes it. */
void pc_vram_init(void);
void pc_vram_sync(void);
const u16 *pc_vram_bg_ext_pltt(int engine, int slot); /* NULL if no bank holds that slot */
const u16 *pc_vram_obj_ext_pltt(int engine);
const u8 *pc_vram_tex_slot(int slot);     /* 3D texture image slot 0-3 (128 KB), NULL if unmapped */
const u8 *pc_vram_texpltt_slot(int slot); /* 3D texture palette slot 0-5 (16 KB), NULL if unmapped */

/* pc_dma.c: run VBlank- and HBlank-timed DMA channels. */
void pc_dma_vblank(void);
void pc_dma_hblank(void);

/* pc_gpu2d.c: render both 2D engines into two 256x192 XRGB8888 images. */
void pc_gpu2d_render(u32 *top, u32 *bottom);

/* pc_video.c: window (SDL3) or headless output, keyboard/mouse input, 60 Hz pacing. */
void pc_video_init(void);
void pc_video_poll(void);
void pc_video_frame(void);

int pc_video_windowed(void);

/* pc_snd.c: the ARM7 sound driver and DS mixer; pc_pxi.c carries words between it and the ARM9. */
void pc_snd_init(void);
void pc_snd_frame(void);
void pc_snd_pxi_from_arm9(u32 data);
void pc_pxi_to_arm9(int fifotag, u32 data, BOOL err);

/* pc_main.c */
void pc_stub_hit(const char *name);
void pc_frame_end(void); /* one 60 Hz frame boundary: present, pump input, pace */

#endif
