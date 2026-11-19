/* Multiplayer wire protocol, shared by the game (pc/src/pc_net.c) and the server (pc/server/hg_server.c).
 *
 * One TCP connection per player. Every message is: u16 length (of type + body, little endian), u8 type,
 * body. Bodies are the packed structs below, little endian (every supported host is x86).
 *
 *   client -> server  NM_HELLO            first message: versions, name, password
 *   server -> client  NM_WELCOME / NM_REJECT
 *   server -> client  NM_JOIN, NM_LEAVE   who is online (on welcome: one NM_JOIN per player already there)
 *   both ways         NM_POS              where a player stands; the server fills in `id` and relays it
 *   both ways         NM_WM               a wireless packet of pc/src/pc_wm.c (trades, battles, Union Room).
 *                                         Body bytes 8..13 are the sender's MAC, 14..19 the receiver's
 *                                         (ff:ff:ff:ff:ff:ff = everyone); the server delivers it by MAC.
 *   both ways         NM_INVITE           trade/battle invitation (pc/src/pc_challenge.c): the client names
 *                                         the receiver in `id`, the server replaces it with the sender's id
 *   both ways         NM_CHAT             a chat line (pc/src/pc_chat.c); the server fills in `id` and relays
 *                                         it to everyone else, or to the sender's map only (--chat map).
 *                                         id NET_SERVER_ID: a notice from the server (command replies).
 *                                         Lines starting with '/' are server commands, never relayed.
 *   both ways         NM_PING             empty; each side sends one every 5 s and drops the other
 *                                         after 30 s without any message
 *   server -> client  NM_ACCOUNT          right before an NM_REJECT, on a server with player passwords
 *                                         (hg_server --accounts): the game should ask its player for the
 *                                         name's password (ACCOUNT_*) and log in again with it in `account`.
 *                                         ACCOUNT_CHANGED instead: an NM_PASSWORD worked
 *   client -> server  NM_PASSWORD         change the name's password (the game's /password)
 *   server -> client  NM_LEAD             you lead (or no longer lead) the NPCs of your map: of the players
 *                                         on a map, the one there longest leads it (pc/src/pc_npc.c)
 *   both ways         NM_NPC              where the NPCs of a map stand, from its leader; the server fills in
 *                                         `id`, drops it unless the sender leads that map, and relays it
 */
#ifndef PC_NET_PROTO_H
#define PC_NET_PROTO_H

#include <stdint.h>

#define NET_PROTO       10   /* bump on any change below; NetHello must keep `proto` first */
#define NET_PORT        7777
#define NET_MAX_PLAYERS 16    /* player ids 0..15 */
#define NET_NAME_LEN    16
#define NET_MAX_MSG     1024  /* type + body */
#define NET_CHAT_LEN    80    /* chat text, NUL included */
#define NET_WM_SRC      8     /* NM_WM: offsets of the MACs in the body */
#define NET_WM_DST      14
#define NET_SERVER_ID   255   /* NM_CHAT id of server notices */

enum { NM_HELLO = 1, NM_WELCOME, NM_REJECT, NM_JOIN, NM_LEAVE, NM_POS, NM_WM, NM_INVITE, NM_CHAT, NM_PING, NM_ACCOUNT, NM_PASSWORD, NM_LEAD, NM_NPC };
#define NET_NPC_MAX 64 /* objects per NM_NPC */
/* name unused: choose a password / enter it / wrong one; ACCOUNT_CHANGED: an NM_PASSWORD worked */
enum { ACCOUNT_NEW, ACCOUNT_ENTER, ACCOUNT_WRONG, ACCOUNT_CHANGED };
enum { INVITE_ASK, INVITE_YES, INVITE_NO, INVITE_CANCEL, INVITE_READY }; /* READY/CANCEL also: battle party chosen or not */

#pragma pack(push, 1)
typedef struct {
    uint32_t proto;    /* NET_PROTO */
    uint32_t game;     /* identifies the game data: everyone on a server must have the same */
    char name[NET_NAME_LEN];
    char password[32]; /* the server's */
    char account[32];  /* the player name's (servers with player passwords); empty: not known yet */
} NetHello;

typedef struct {
    uint8_t what;      /* ACCOUNT_* */
} NetAccount;

typedef struct {       /* client -> server: change the name's password; the reply is a notice */
    char password[32]; /* current (empty if the name has none) */
    char newPassword[32];
} NetPassword;

typedef struct {
    uint8_t id;
} NetWelcome;

typedef struct {
    char reason[64];
} NetReject;

typedef struct {
    uint8_t id;
    char name[NET_NAME_LEN];
} NetJoin;

typedef struct {
    uint8_t id;
} NetLeave;

typedef struct NetPos {
    uint8_t id;        /* set by the server */
    uint8_t dir;       /* DIR_NORTH.. */
    uint8_t gender;    /* 0 male, 1 female */
    uint8_t state;     /* PLAYER_STATE_* (walking, cycling, surfing...) */
    uint16_t map;      /* map id (constants/maps.h) */
    uint16_t x, z;     /* tile; outdoor maps share one coordinate space */
    uint16_t monSpecies; /* the following Pokemon, if shown; else 0 */
    uint8_t monForm;
    uint8_t monFlags;  /* bit 0 shiny, bits 1-2 gender (MON_MALE..) */
    uint16_t monX, monZ;
    uint8_t monDir;
} NetPos;

typedef struct {
    uint8_t lead;      /* 1: you lead the NPCs of `map` */
    uint16_t map;
} NetLead;

typedef struct {
    uint8_t obj;       /* map object id (the map's event id) */
    uint8_t dir;
    uint16_t x, z;
} NetNpcObj;

typedef struct {
    uint8_t id;        /* sender, set by the server */
    uint8_t count;
    uint16_t map;
    NetNpcObj obj[NET_NPC_MAX]; /* only `count` are sent */
} NetNpc;
#define NET_NPC_SIZE(n) (4 + 6 * (n))
typedef struct {
    uint8_t id;        /* receiver (client to server), sender (server to client) */
    uint8_t what;      /* INVITE_* */
    uint32_t trainer;  /* sender's in-game trainer ID */
    uint8_t kind;      /* PC_LINK_*: trade, battle, double battle */
} NetInvite;

typedef struct {
    uint8_t id;        /* set by the server */
    char text[NET_CHAT_LEN];
} NetChat;
#pragma pack(pop)

#endif
