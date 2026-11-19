/* Multiplayer client: one TCP connection to hg_server (pc/server/hg_server.c, protocol in
 * pc_net_proto.h). Non-blocking, polled once per frame (and while the options menu is open), so it
 * never stalls the game. pc_ghost.c draws the other players from pc_net_remote().
 *
 * The server address and player name come from hg_pc.cfg (server=, name=; the Multiplayer page of
 * the options menu sets them), or from $HG_SERVER and $HG_NAME, which also connect at startup
 * (headless tests). A dropped connection is retried every 5 seconds until the player disconnects;
 * a refusal (wrong password, full, different ROM, kicked) is not retried. The game pings the server
 * every 5 seconds and gives up on it after 30 seconds of silence (10 while connecting), so a server
 * that vanished without closing the connection is noticed too. Losing the connection shows in chat.
 *
 * On a server with player passwords (hg_server --accounts), the server answers a login without the
 * name's password with NM_ACCOUNT; the game then asks for it in the chat line (a new name: typed twice)
 * and logs in again. The password is kept in memory for reconnects, for that server and name only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#undef far
#undef near
typedef SOCKET Sock;
#define BAD_SOCK INVALID_SOCKET
#define CloseSock closesocket
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Sock;
#define BAD_SOCK (-1)
#define CloseSock close
#endif
#include <nitro.h>
#include "pc_net_proto.h"
#include "pc_platform.h"

enum { ST_OFF, ST_CONNECTING, ST_HELLO, ST_ONLINE, ST_WAIT };

static int sState, sWanted, sMyId = -1;
static Sock sSock = BAD_SOCK;
static time_t sRetryAt, sHeardAt, sPingAt;
static char sStatus[80] = "Offline";
static unsigned char sIn[8192], sOut[65536]; /* out: about 2 s of link traffic */
static int sInLen, sOutLen;
static PcRemote sRemotes[NET_MAX_PLAYERS];
static int sLog, sQuietJoins; /* no "joined" notices for the players already online when we arrive */
/* Servers with player passwords: the name's password, typed this session (never saved), and the
 * server and name it belongs to, so it is never sent anywhere else. */
static char sAccount[32], sAccountFor[300], sFirst[32];
static int sAsking = -1; /* ACCOUNT_* while the player types it: no reconnecting meanwhile */

static void AccountKey(char *out) { snprintf(out, sizeof sAccountFor, "%s\n%s", pc_cfg_server(), pc_cfg_name()); }

const PcRemote *pc_net_remote(int id) {
    return id >= 0 && id < NET_MAX_PLAYERS && sRemotes[id].active ? &sRemotes[id] : NULL;
}
const char *pc_net_status(void) { return sStatus; }
int pc_net_wanted(void) { return sWanted; }

static void Close(const char *status) {
    if (sSock != BAD_SOCK) CloseSock(sSock);
    sSock = BAD_SOCK;
    sInLen = sOutLen = 0;
    sMyId = -1;
    memset(sRemotes, 0, sizeof sRemotes);
    pc_npc_lead(0, 0);
    snprintf(sStatus, sizeof sStatus, "%s", status);
    if (sLog) fprintf(stderr, "hg_pc: net: %s\n", status);
    if ((sState == ST_HELLO || sState == ST_ONLINE) && sAsking < 0) pc_chat_receive(NULL, status);
    sState = sWanted ? ST_WAIT : ST_OFF;
    sRetryAt = time(NULL) + 5;
}

static void Flush(void) {
    if (sOutLen == 0 || sSock == BAD_SOCK) return;
    int n = send(sSock, (const char *)sOut, sOutLen, 0);
    if (n > 0) {
        memmove(sOut, sOut + n, sOutLen - n);
        sOutLen -= n;
    }
}

static void Queue(int type, const void *body, int len) {
    if (sOutLen + 3 + len > (int)sizeof sOut) return Close("Connection lost (server not reading)");
    sOut[sOutLen] = (unsigned char)(len + 1);
    sOut[sOutLen + 1] = (unsigned char)((len + 1) >> 8);
    sOut[sOutLen + 2] = (unsigned char)type;
    memcpy(sOut + sOutLen + 3, body, len);
    sOutLen += 3 + len;
    Flush();
}

/* Same ROM for everyone: its header CRC and version byte (romhacks differ). */
static uint32_t GameId(void) {
    u16 crc;
    u8 ver;
    pc_rom_read(0x15E, &crc, 2);
    pc_rom_read(0x1E, &ver, 1);
    return crc | (uint32_t)ver << 16;
}

static void Start(void) {
    const char *addr = pc_cfg_server();
    if (addr == NULL || *addr == 0) {
        sWanted = 0;
        Close("No server address set");
        return;
    }
    char host[256], port[16];
    snprintf(host, sizeof host, "%s", addr);
    snprintf(port, sizeof port, "%d", NET_PORT);
    char *colon = strrchr(host, ':');
    if (colon != NULL) {
        snprintf(port, sizeof port, "%s", colon + 1);
        *colon = 0;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || res == NULL) {
        Close("Server address not found");
        return;
    }
    pc_wm_online();
    sSock = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1, big = 1 << 18;
    setsockopt(sSock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    setsockopt(sSock, SOL_SOCKET, SO_SNDBUF, (const char *)&big, sizeof big);
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(sSock, FIONBIO, &nb);
#else
    fcntl(sSock, F_SETFL, fcntl(sSock, F_GETFL) | O_NONBLOCK);
#endif
    connect(sSock, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    sState = ST_CONNECTING;
    sHeardAt = time(NULL);
    snprintf(sStatus, sizeof sStatus, "Connecting to %.40s...", addr);
}

static void Ask(int what);

/* The player typed the name's password (NULL: Esc, stay offline). */
static void Answer(const char *text) {
    if (text == NULL) {
        pc_net_disconnect();
        return;
    }
    if (*text == 0) return Ask(sAsking);
    if (sAsking == ACCOUNT_NEW && sFirst[0] == 0) {
        snprintf(sFirst, sizeof sFirst, "%s", text);
        pc_chat_ask("Type it again:", Answer);
        return;
    }
    if (sAsking == ACCOUNT_NEW && strcmp(text, sFirst) != 0) {
        memset(sFirst, 0, sizeof sFirst);
        pc_chat_ask("They didn't match. Choose a\npassword for this name:", Answer);
        return;
    }
    snprintf(sAccount, sizeof sAccount, "%s", text);
    AccountKey(sAccountFor);
    memset(sFirst, 0, sizeof sFirst);
    sAsking = -1;
    if (sWanted && sState == ST_WAIT) Start();
}

/* /password: current, new, new again; then NM_PASSWORD. The new one replaces sAccount once the
 * server says it worked (ACCOUNT_CHANGED). */
static char sOldPw[32], sNewPw[32];
static int sChangeStep;

static void ChangeAnswer(const char *text) {
    if (text == NULL || sState != ST_ONLINE) { /* Esc, or the connection went meanwhile */
        memset(sOldPw, 0, sizeof sOldPw);
        memset(sNewPw, 0, sizeof sNewPw);
        return;
    }
    if (sChangeStep == 0) {
        snprintf(sOldPw, sizeof sOldPw, "%s", text);
        sChangeStep = 1;
        pc_chat_ask("New password:", ChangeAnswer);
    } else if (sChangeStep == 1) {
        if (*text == 0) return pc_chat_ask("New password:", ChangeAnswer);
        snprintf(sNewPw, sizeof sNewPw, "%s", text);
        sChangeStep = 2;
        pc_chat_ask("Type it again:", ChangeAnswer);
    } else if (strcmp(text, sNewPw) != 0) {
        sChangeStep = 1;
        pc_chat_ask("They didn't match.\nNew password:", ChangeAnswer);
    } else {
        NetPassword p;
        memset(&p, 0, sizeof p);
        memcpy(p.password, sOldPw, sizeof p.password);
        memcpy(p.newPassword, sNewPw, sizeof p.newPassword);
        Queue(NM_PASSWORD, &p, sizeof p);
        memset(&p, 0, sizeof p);
        memset(sOldPw, 0, sizeof sOldPw);
    }
}

void pc_net_change_password(void) {
    if (sState != ST_ONLINE) return;
    sChangeStep = 0;
    pc_chat_ask("Current password\n(Enter if you have none):", ChangeAnswer);
}

static void Ask(int what) {
    sAsking = what;
    memset(sFirst, 0, sizeof sFirst);
    pc_chat_ask(what == ACCOUNT_NEW ? "Choose a password for your\nname on this server:"
                : what == ACCOUNT_WRONG ? "Wrong password. Try again:"
                : "Your password on this server:", Answer);
}

static void Message(int type, const unsigned char *b, int len) {
    if (type == NM_WELCOME && len == sizeof(NetWelcome)) {
        sMyId = ((const NetWelcome *)b)->id;
        sState = ST_ONLINE;
        snprintf(sStatus, sizeof sStatus, "Online as %.16s", pc_cfg_name());
        if (sLog) fprintf(stderr, "hg_pc: net: online, player %d\n", sMyId);
        sQuietJoins = 1;
        pc_ghost_resend();
    } else if (type == NM_REJECT && len == sizeof(NetReject)) {
        char why[80];
        snprintf(why, sizeof why, "Refused: %.63s", ((const NetReject *)b)->reason);
        if (sAsking >= 0) return Close("Type your password in the game"); /* reconnects once typed */
        sWanted = 0;
        Close(why);
    } else if (type == NM_ACCOUNT && len == sizeof(NetAccount) && ((const NetAccount *)b)->what == ACCOUNT_CHANGED) {
        memcpy(sAccount, sNewPw, sizeof sAccount); /* reconnects use the new one */
        AccountKey(sAccountFor);
        memset(sNewPw, 0, sizeof sNewPw);
    } else if (type == NM_ACCOUNT && len == sizeof(NetAccount) && ((const NetAccount *)b)->what <= ACCOUNT_WRONG) {
        Ask(((const NetAccount *)b)->what);
    } else if (type == NM_JOIN && len == sizeof(NetJoin)) {
        const NetJoin *j = (const NetJoin *)b;
        if (j->id >= NET_MAX_PLAYERS) return;
        memset(&sRemotes[j->id], 0, sizeof sRemotes[j->id]);
        sRemotes[j->id].active = 1;
        memcpy(sRemotes[j->id].name, j->name, NET_NAME_LEN);
        if (sLog) fprintf(stderr, "hg_pc: net: %s joined\n", sRemotes[j->id].name);
        if (!sQuietJoins) {
            char t[40];
            snprintf(t, sizeof t, "%s joined", sRemotes[j->id].name);
            pc_chat_receive(NULL, t);
        }
    } else if (type == NM_LEAVE && len == sizeof(NetLeave)) {
        const NetLeave *l = (const NetLeave *)b;
        if (l->id >= NET_MAX_PLAYERS) return;
        if (sLog) fprintf(stderr, "hg_pc: net: %s left\n", sRemotes[l->id].name);
        if (sRemotes[l->id].active) {
            char t[40];
            snprintf(t, sizeof t, "%s left", sRemotes[l->id].name);
            pc_chat_receive(NULL, t);
        }
        memset(&sRemotes[l->id], 0, sizeof sRemotes[l->id]);
    } else if (type == NM_POS && len == sizeof(NetPos)) {
        const NetPos *p = (const NetPos *)b;
        if (p->id >= NET_MAX_PLAYERS || !sRemotes[p->id].active) return;
        PcRemote *r = &sRemotes[p->id];
        if (!r->hasPos || r->x != p->x || r->z != p->z || r->dir != (p->dir & 3)) {
            r->path[r->pathIn++ % PC_PATH] = (PcStep){ (short)p->x, (short)p->z, (unsigned char)(p->dir & 3) };
        }
        if (r->monX != p->monX || r->monZ != p->monZ || r->monDir != (p->monDir & 3)) {
            r->monPath[r->monPathIn++ % PC_PATH] = (PcStep){ (short)p->monX, (short)p->monZ, (unsigned char)(p->monDir & 3) };
        }
        r->hasPos = 1;
        r->map = p->map;
        r->x = p->x;
        r->z = p->z;
        r->dir = p->dir & 3;
        r->gender = p->gender & 1;
        r->state = p->state;
        r->monSpecies = p->monSpecies;
        r->monForm = p->monForm;
        r->monFlags = p->monFlags;
        r->monX = p->monX;
        r->monZ = p->monZ;
        r->monDir = p->monDir & 3;
    } else if (type == NM_LEAD && len == sizeof(NetLead)) {
        pc_npc_lead(((const NetLead *)b)->lead, ((const NetLead *)b)->map);
    } else if (type == NM_NPC) {
        pc_npc_receive(b, len);
    } else if (type == NM_INVITE && len == sizeof(NetInvite)) {
        const NetInvite *v = (const NetInvite *)b;
        if (v->id < NET_MAX_PLAYERS && sRemotes[v->id].active) pc_challenge_message(v->id, v->what, v->trainer, v->kind);
    } else if (type == NM_CHAT && len == sizeof(NetChat)) {
        const NetChat *c = (const NetChat *)b;
        char text[NET_CHAT_LEN];
        memcpy(text, c->text, sizeof text);
        text[sizeof text - 1] = 0;
        if (c->id == NET_SERVER_ID) pc_chat_receive(NULL, text);
        else if (c->id < NET_MAX_PLAYERS && sRemotes[c->id].active) pc_chat_receive(sRemotes[c->id].name, text);
    } else if (type == NM_WM) {
        pc_wm_deliver(b, len);
    }
}

static void Receive(void) {
    for (;;) {
        int n = recv(sSock, (char *)sIn + sInLen, (int)sizeof sIn - sInLen, 0);
        if (n == 0) return Close("Server closed the connection");
        if (n < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK) return;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
#endif
            return Close("Connection lost");
        }
        sInLen += n;
        sHeardAt = time(NULL);
        int at = 0;
        while (sInLen - at >= 3) {
            int len = sIn[at] | sIn[at + 1] << 8;
            if (len < 1 || len > NET_MAX_MSG) return Close("Bad data from server");
            if (sInLen - at < 2 + len) break;
            Message(sIn[at + 2], sIn + at + 3, len - 1);
            if (sSock == BAD_SOCK) return;
            at += 2 + len;
        }
        memmove(sIn, sIn + at, sInLen - at);
        sInLen -= at;
        sQuietJoins = 0; /* shortcut: the welcome's NM_JOINs arrive in the same read as the welcome */
    }
}

void pc_net_connect(void) {
    if ((sState != ST_OFF && sState != ST_WAIT) || sAsking >= 0) return; /* asking: connects once typed */
    sWanted = 1;
    Start();
}

void pc_net_disconnect(void) {
    sWanted = 0;
    if (sAsking >= 0) { /* drop the password question too */
        sAsking = -1;
        pc_chat_close();
    }
    Close("Offline");
}

void pc_net_poll(void) {
    static int init;
    if (!init) {
        init = 1;
        sLog = getenv("HG_NET_LOG") != NULL;
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        if (getenv("HG_SERVER") != NULL || pc_cfg_online()) pc_net_connect();
    }
    if (sState == ST_WAIT && sWanted && sAsking < 0 && time(NULL) >= sRetryAt) Start();
    if (sSock == BAD_SOCK) return;
    if (sState == ST_CONNECTING) {
        fd_set wr, ex;
        FD_ZERO(&wr);
        FD_ZERO(&ex);
        FD_SET(sSock, &wr);
        FD_SET(sSock, &ex);
        struct timeval tv = { 0, 0 };
        if (select((int)sSock + 1, NULL, &wr, &ex, &tv) <= 0) {
            if (time(NULL) - sHeardAt > 10) Close("Could not reach the server");
            return;
        }
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(sSock, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
        if (err != 0 || FD_ISSET(sSock, &ex)) return Close("Could not reach the server");
        NetHello h;
        memset(&h, 0, sizeof h);
        h.proto = NET_PROTO;
        h.game = GameId();
        snprintf(h.name, sizeof h.name, "%s", pc_cfg_name());
        snprintf(h.password, sizeof h.password, "%s", pc_cfg_password());
        char key[sizeof sAccountFor];
        AccountKey(key);
        if (!strcmp(key, sAccountFor)) memcpy(h.account, sAccount, sizeof h.account);
        sState = ST_HELLO;
        snprintf(sStatus, sizeof sStatus, "Joining...");
        Queue(NM_HELLO, &h, sizeof h);
        if (sSock == BAD_SOCK) return;
    }
    Flush();
    Receive(); /* before the silence check: after a long stall (a window drag) the data is waiting */
    if (sSock == BAD_SOCK) return;
    time_t now = time(NULL);
    if (now - sHeardAt > (sState == ST_ONLINE ? 30 : 10)) return Close("Connection lost (no reply)");
    if (sState == ST_ONLINE && now >= sPingAt) {
        sPingAt = now + 5;
        Queue(NM_PING, "", 0);
    }
}

void pc_net_send_pos(const NetPos *p) {
    if (sState == ST_ONLINE) Queue(NM_POS, p, sizeof *p);
}

void pc_net_send_npc(const void *body, int len) {
    if (sState == ST_ONLINE) Queue(NM_NPC, body, len);
}

void pc_net_send_wm(const void *pkt, int len) {
    if (sState == ST_ONLINE && len < NET_MAX_MSG) Queue(NM_WM, pkt, len);
}

void pc_net_send_invite(int id, int what, u32 trainer, int kind) {
    NetInvite v = { (uint8_t)id, (uint8_t)what, (uint32_t)trainer, (uint8_t)kind };
    if (sState == ST_ONLINE) Queue(NM_INVITE, &v, sizeof v);
}

int pc_net_online(void) { return sState == ST_ONLINE; }

int pc_net_send_chat(const char *text) {
    if (sState != ST_ONLINE) return 0;
    NetChat c;
    memset(&c, 0, sizeof c);
    snprintf(c.text, sizeof c.text, "%s", text);
    Queue(NM_CHAT, &c, sizeof c);
    return 1;
}
