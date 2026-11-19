/* Host stand-in for the ARM7 half of the DS wireless driver (WM).
 *
 * The SDK's ARM9 WM library (compiled unchanged) sends each request as a buffer address over the
 * PXI FIFO and expects the ARM7 to answer with WMCallback-shaped results, keeping the shared
 * WMStatus up to date (the ARM9 checks status->state before every call). This file plays that
 * ARM7: requests are copied when sent (pc_wm_command) and answered at the next frame boundary
 * (pc_wm_frame), following the SDK's own ARM7 code (libraries/wm/src/ARM7/req_*.c) for states,
 * bitmaps and callback contents. Everything above WM (the game's comm layer, trades, battles)
 * runs unchanged.
 *
 * The "airwaves" are the multiplayer server (pc_net.c, NM_WM): everyone online hears everyone, as if
 * they stood in one room. Connecting gives this console a random MAC address (pc_wm_online).
 * For local tests they can be UDP datagrams between hg_pc copies instead (spike S1 of MULTIPLAYER_PLAN.md):
 *   HG_WM_PORT=5001                local UDP port; also makes this console's MAC address unique
 *   HG_WM_PEERS=127.0.0.1:5002,... the other copies
 *   HG_WM_DELAY=n                  hold every received packet n frames (latency test)
 *   HG_WM_LOG=1                    log requests, answers and packets
 * Offline and without HG_WM_PORT the radio works but hears nobody: scans find no one.
 *
 * shortcut: channels are honoured but there is no loss, retransmission or MP timing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#undef far
#undef near
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <nitro.h>
#include "pc_platform.h"

#define LOG(...) do { if (sLog) fprintf(stderr, "hg_pc: wm: " __VA_ARGS__); } while (0)

static int sLog;
static u32 sFrame;

/* ---------------------------------------------------------------- medium (UDP) */
enum { P_BEACON = 1, P_ASSOC_REQ, P_ASSOC_RESP, P_DATA, P_MP_READY, P_DISASSOC, P_PING };

typedef struct {
    u32 magic;
    u16 type, len; /* payload length */
    u8 src[6], dst[6];
} PktHdr;

typedef struct {
    u16 channel, gameInfoLength;
    WMGameInfo gameInfo;
} Beacon;

typedef struct {
    u16 port, seqNo, srcAid, size;
    u8 data[WM_SIZE_MP_DATA_MAX];
} DataPkt;

#define MAGIC 0x4D574748u /* "HGWM" */
#define MAX_PEERS 8
#define MAX_PKT (sizeof(PktHdr) + sizeof(DataPkt))

#ifdef _WIN32
typedef SOCKET Sock;
#define BAD_SOCK INVALID_SOCKET
#else
typedef int Sock;
#define BAD_SOCK (-1)
#endif
static Sock sSock = BAD_SOCK;
static struct sockaddr_in sPeers[MAX_PEERS];
static int sNumPeers;
static u8 sMac[6];
static const u8 kBroadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static void SetMac(const u8 *mac);

/* At startup, before the game can read the MAC address. */
void pc_wm_init(void) {
    const char *port = getenv("HG_WM_PORT");
    sLog = getenv("HG_WM_LOG") != NULL;
    /* WM_GetAllowedChannel (WM_PRECALC_ALLOWEDCHANNEL) reads this word after the MAC; the DS ARM7
     * writes it at boot. 0 means "no channel" and the game reports a comm error. */
    *(u16 *)((u8 *)OS_GetSystemWork()->nvramUserInfo + ((sizeof(NVRAMConfig) + 3) & ~3u) + 6) = 0x1041; /* 1, 7, 13 */
    if (port == NULL || *port == 0) return;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
#endif
    sSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sSock == BAD_SOCK) return;
    struct sockaddr_in me = { 0 };
    me.sin_family = AF_INET;
    me.sin_port = htons((u16)atoi(port));
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sSock, (struct sockaddr *)&me, sizeof me) != 0) {
        fprintf(stderr, "hg_pc: wm: cannot bind UDP port %s\n", port);
        sSock = BAD_SOCK;
        return;
    }
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(sSock, FIONBIO, &nb);
#else
    fcntl(sSock, F_SETFL, fcntl(sSock, F_GETFL) | O_NONBLOCK);
#endif
    const char *peers = getenv("HG_WM_PEERS");
    while (peers != NULL && *peers && sNumPeers < MAX_PEERS) {
        char host[64];
        unsigned p = 0;
        size_t n = strcspn(peers, ":");
        if (n >= sizeof host || peers[n] != ':') break;
        memcpy(host, peers, n);
        host[n] = 0;
        p = (unsigned)strtoul(peers + n + 1, NULL, 10);
        struct sockaddr_in *a = &sPeers[sNumPeers++];
        memset(a, 0, sizeof *a);
        a->sin_family = AF_INET;
        a->sin_port = htons((u16)p);
        a->sin_addr.s_addr = inet_addr(host);
        peers += strcspn(peers, ",");
        if (*peers == ',') peers++;
    }
    u32 id = (u32)atoi(port);
    u8 mac[6] = { 0x00, 0x09, 0xbf, (u8)(id >> 16), (u8)(id >> 8), (u8)id };
    SetMac(mac);
    fprintf(stderr, "hg_pc: wm: radio on UDP port %s, %d peer(s), MAC %02x:%02x:%02x:%02x:%02x:%02x\n", port, sNumPeers,
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* The console's MAC address: OS_GetMacAddress reads it here (written by the ARM7 at boot on a DS). */
static void SetMac(const u8 *mac) {
    memcpy(sMac, mac, 6);
    memcpy((u8 *)OS_GetSystemWork()->nvramUserInfo + ((sizeof(NVRAMConfig) + 3) & ~3u), mac, 6);
}

void pc_wm_online(void) {
    static const u8 zero[6];
    if (memcmp(sMac, zero, 6)) return;
    /* Not the game's RNG: that would change the game. */
    u32 r = (u32)time(NULL) * 2654435761u ^ (u32)clock() * 40503u ^ (u32)(uintptr_t)&r;
    u8 mac[6] = { 0x00, 0x09, 0xbf, (u8)(r >> 16), (u8)(r >> 8), (u8)r };
    SetMac(mac);
    LOG("MAC %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void Send(int type, const u8 *dst, const void *payload, u16 len) {
    u8 buf[MAX_PKT];
    PktHdr *h = (PktHdr *)buf;
    h->magic = MAGIC;
    h->type = (u16)type;
    h->len = len;
    memcpy(h->src, sMac, 6);
    memcpy(h->dst, dst, 6);
    memcpy(buf + sizeof *h, payload, len);
    if (sSock == BAD_SOCK) return pc_net_send_wm(buf, (int)(sizeof *h + len));
    for (int i = 0; i < sNumPeers; i++) {
        sendto(sSock, (const char *)buf, (int)(sizeof *h + len), 0, (struct sockaddr *)&sPeers[i], sizeof sPeers[i]);
    }
}

/* Received packets wait here HG_WM_DELAY frames before they count as arrived. */
typedef struct {
    u32 due;
    u16 n;
    u8 buf[MAX_PKT];
} Held;
static Held sHeld[256];
static int sHeldHead, sHeldTail;

/* ---------------------------------------------------------------- shared state */
static WMStatus *sSt;
static WMArm7Buf *sW7;
static u32 sCb[WM_FIFO_BUF_SIZE / 4 * 2] __attribute__((aligned(32))); /* one answer at a time */

static void Post(const void *cb, size_t n) {
    memcpy(sCb, cb, n);
    LOG("-> apiid %u err %u\n", ((const u16 *)cb)[0], ((const u16 *)cb)[1]);
    pc_pxi_to_arm9(PXI_FIFO_TAG_WM, (u32)(uintptr_t)sCb, FALSE);
}

static void PostSimple(u16 apiid, u16 err) {
    WMCallback cb = { apiid, err, 0, 0 };
    Post(&cb, sizeof cb);
}

/* parent */
static u8 sChildMac[WM_NUM_MAX_CHILD + 1][6]; /* by aid */
static u32 sChildSeen[WM_NUM_MAX_CHILD + 1];
static WMGameInfo sGameInfo;
static u16 sGameInfoLen;
/* child */
static u8 sParentMac[6];
static u32 sParentSeen;
/* beacons heard */
static struct {
    u8 mac[6];
    u32 frame;
    Beacon b;
} sHeard[8];
/* scan in progress */
static struct {
    int active;
    u16 channel;
    WMBssDesc *buf;
    u32 deadline;
    u8 bssid[6];
} sScan;
/* connect in progress */
static struct {
    int active;
    u8 mac[6], ssid[WM_SIZE_CHILD_SSID];
    u32 deadline;
    u16 powerSave;
    WMGameInfo gi;
} sConn;
/* MP send queue (WM_SetMPDataToPortEx) */
typedef struct {
    const u16 *data;
    u16 size, dest, port, prio;
    WMCallbackFunc cb;
    void *arg;
} QItem;
static QItem sQ[WM_SEND_QUEUE_NUM];
static int sQn;
static u16 sSeq[WM_NUM_MAX_CHILD + 1][WM_NUM_OF_SEQ_PORT];
static u16 sRxData[WM_SIZE_MP_DATA_MAX / 2 + 2];

static void SetSizes(u16 parentMax, u16 childMax) {
    /* WMSP_SetParentMaxSize / WMSP_SetChildMaxSize */
    BOOL parent = sSt->aid == 0;
    sSt->mp_parentSize = sSt->mp_parentMaxSize = parentMax;
    sSt->mp_childSize = sSt->mp_childMaxSize = childMax;
    u16 send = (u16)(parent ? parentMax + WM_HEADER_PARENT_MAX_SIZE : childMax + WM_HEADER_CHILD_MAX_SIZE);
    u16 recv = (u16)(parent ? childMax + WM_HEADER_CHILD_MAX_SIZE : parentMax + WM_HEADER_PARENT_MAX_SIZE);
    sSt->mp_maxSendSize = sSt->mp_sendSize = send;
    sSt->mp_maxRecvSize = sSt->mp_recvSize = recv;
}

static void BuildGameInfo(const u16 *user, u16 userLen, u32 ggid, u16 tgid, u8 attr) {
    memset(&sGameInfo, 0, sizeof sGameInfo);
    sGameInfo.magicNumber = WM_GAMEINFO_MAGIC_NUMBER;
    sGameInfo.ver = WM_GAMEINFO_VERSION_NUMBER;
    sGameInfo.platform = WM_GAMEINFO_PLATFORM_ID_NITRO;
    sGameInfo.ggid = ggid;
    sGameInfo.tgid = tgid;
    sGameInfo.userGameInfoLength = (u8)userLen;
    sGameInfo.attribute = attr;
    sGameInfo.parentMaxSize = sSt->pparam.parentMaxSize;
    sGameInfo.childMaxSize = sSt->pparam.childMaxSize;
    if (user != NULL && userLen) memcpy(sGameInfo.userGameInfo, user, userLen);
    sGameInfoLen = (u16)(WM_GAMEINFO_LENGTH_MIN + userLen);
}

static u8 ParamAttr(const WMParentParam *p) {
    return (u8)((p->entryFlag ? WM_ATTR_FLAG_ENTRY : 0) | (p->multiBootFlag ? WM_ATTR_FLAG_MB : 0) |
                (p->KS_Flag ? WM_ATTR_FLAG_KS : 0) | (p->CS_Flag ? WM_ATTR_FLAG_CS : 0));
}

static BOOL AmParent(void) { return sSt->state == WM_STATE_PARENT || sSt->state == WM_STATE_MP_PARENT; }
static BOOL AmChild(void) { return sSt->state == WM_STATE_CHILD || sSt->state == WM_STATE_MP_CHILD; }

/* ---------------------------------------------------------------- connection events */
static void ParentLostChild(u16 aid, u16 state, u16 reason) {
    if (!(sSt->child_bitmap & (1u << aid))) return;
    sSt->child_bitmap &= (u16)~(1u << aid);
    sSt->mp_readyBitmap &= (u16)~(1u << aid);
    WMStartParentCallback cb = { 0 };
    cb.apiid = WM_APIID_START_PARENT;
    cb.errcode = WM_ERRCODE_SUCCESS;
    cb.state = state;
    memcpy(cb.macAddress, sChildMac[aid], 6);
    cb.aid = aid;
    cb.reason = reason;
    cb.parentSize = sSt->mp_parentSize;
    cb.childSize = sSt->mp_childSize;
    Post(&cb, sizeof cb);
}

static void FlushQueue(u16 err) {
    while (sQn > 0) {
        QItem q = sQ[0];
        memmove(sQ, sQ + 1, --sQn * sizeof sQ[0]);
        WMPortSendCallback cb = { 0 };
        cb.apiid = WM_APIID_PORT_SEND;
        cb.errcode = err;
        cb.state = WM_STATECODE_PORT_SEND;
        cb.port = q.port;
        cb.destBitmap = q.dest;
        cb.restBitmap = q.dest;
        cb.data = q.data;
        cb.size = q.size;
        cb.seqNo = 0xffff;
        cb.callback = q.cb;
        cb.arg = q.arg;
        Post(&cb, sizeof cb);
    }
}

static void ChildLost(u16 state, u16 reason) {
    if (!AmChild()) return;
    FlushQueue(WM_ERRCODE_SEND_FAILED);
    sSt->mp_flag = FALSE;
    sSt->child_bitmap = 0;
    sSt->mp_readyBitmap = 0;
    sSt->state = WM_STATE_IDLE;
    sSt->beaconIndicateFlag = 0;
    WMStartConnectCallback cb = { 0 };
    cb.apiid = WM_APIID_START_CONNECT;
    cb.errcode = WM_ERRCODE_SUCCESS;
    cb.state = state;
    cb.aid = sSt->aid;
    cb.reason = reason;
    memcpy(cb.macAddress, sParentMac, 6);
    cb.parentSize = sSt->mp_parentSize;
    cb.childSize = sSt->mp_childSize;
    Post(&cb, sizeof cb);
}

static void DropAllChildren(u16 state) {
    for (u16 aid = 1; aid <= WM_NUM_MAX_CHILD; aid++) {
        if (sSt->child_bitmap & (1u << aid)) {
            u16 r[2] = { aid, WM_DISCONNECT_REASON_DISASSOC_LEAVING };
            Send(P_DISASSOC, sChildMac[aid], r, sizeof r);
            ParentLostChild(aid, state, WM_DISCONNECT_REASON_FROM_MYSELF);
        }
    }
}

static void LeaveParent(void) {
    if (!AmChild()) return;
    u16 r[2] = { sSt->aid, WM_DISCONNECT_REASON_DISASSOC_LEAVING };
    Send(P_DISASSOC, sParentMac, r, sizeof r);
    ChildLost(WM_STATECODE_DISCONNECTED_FROM_MYSELF, WM_DISCONNECT_REASON_FROM_MYSELF);
}

/* ---------------------------------------------------------------- requests */
typedef struct {
    u32 w[WM_FIFO_BUF_SIZE / 4];
} Cmd;
static Cmd sCmds[64];
static int sCmdHead, sCmdTail;

/* Called from PXI_SendWordByFifo: take a copy and release the buffer to the ARM9 at once. */
void pc_wm_command(u32 addr) {
    Cmd *c = &sCmds[sCmdTail];
    memcpy(c->w, (void *)(uintptr_t)addr, sizeof c->w);
    *(u16 *)(uintptr_t)addr |= WM_API_REQUEST_ACCEPTED;
    sCmdTail = (sCmdTail + 1) % 64;
    if (sCmdTail == sCmdHead) fprintf(stderr, "hg_pc: wm: request queue overflow\n");
}

static void SendQueued(void);

static void Handle(const Cmd *c) {
    u16 id = (u16)(c->w[0] & 0x7fff);
    const u32 *a = c->w; /* WMi_SendCommand parameters are a[1..] */
    LOG("<- apiid %u (%08x %08x %08x %08x) state %u\n", id, a[1], a[2], a[3], a[4], sSt ? sSt->state : 0);
    switch (id) {
    case WM_APIID_INITIALIZE:
    case WM_APIID_ENABLE:
        sW7 = (WMArm7Buf *)(uintptr_t)a[1];
        sSt = (WMStatus *)(uintptr_t)a[2];
        sW7->status = sSt;
        sW7->fifo7to9 = (u32 *)(uintptr_t)a[3];
        sSt->miscFlags = a[4];
        memcpy(sSt->MacAddress, sMac, 6);
        sSt->allowedChannel = 0x1041; /* channels 1, 7 and 13 */
        sSt->enableChannel = 0x1fff;
        sSt->linkLevel = WM_LINK_LEVEL_3;
        sSt->state = id == WM_APIID_INITIALIZE ? WM_STATE_IDLE : WM_STATE_STOP;
        PostSimple(id, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_POWER_ON: sSt->state = WM_STATE_IDLE; PostSimple(id, WM_ERRCODE_SUCCESS); break;
    case WM_APIID_POWER_OFF: sSt->state = WM_STATE_STOP; PostSimple(id, WM_ERRCODE_SUCCESS); break;
    case WM_APIID_DISABLE: sSt->state = WM_STATE_READY; PostSimple(id, WM_ERRCODE_SUCCESS); break;
    case WM_APIID_RESET:
    case WM_APIID_END:
        FlushQueue(WM_ERRCODE_SEND_FAILED);
        if (AmParent()) DropAllChildren(WM_STATECODE_DISCONNECTED_FROM_MYSELF);
        if (AmChild()) LeaveParent();
        sSt->mp_flag = FALSE;
        sScan.active = sConn.active = 0;
        sSt->child_bitmap = sSt->mp_readyBitmap = 0;
        sSt->aid = 0;
        sSt->beaconIndicateFlag = 0;
        sSt->state = id == WM_APIID_RESET ? WM_STATE_IDLE : WM_STATE_READY;
        PostSimple(id, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_SET_P_PARAM: {
        const WMParentParam *p = (const WMParentParam *)(uintptr_t)a[1];
        sSt->pparam = *p;
        BuildGameInfo(p->userGameInfo, p->userGameInfoLength, p->ggid, p->tgid, ParamAttr(p));
        PostSimple(id, WM_ERRCODE_SUCCESS);
        break;
    }
    case WM_APIID_SET_GAMEINFO:
        BuildGameInfo((const u16 *)(uintptr_t)a[1], (u16)a[2], a[3], (u16)a[4], (u8)a[5]);
        PostSimple(id, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_SET_ENTRY:
        sSt->pparam.entryFlag = (u16)a[1];
        sGameInfo.attribute = (u8)((sGameInfo.attribute & ~WM_ATTR_FLAG_ENTRY) | (a[1] ? WM_ATTR_FLAG_ENTRY : 0));
        PostSimple(id, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_START_PARENT: {
        WMStartParentCallback cb = { 0 };
        cb.apiid = id;
        if (sSt->state != WM_STATE_IDLE || (sSt->miscFlags & WM_MISC_FLAG_LISTEN_ONLY)) {
            cb.errcode = WM_ERRCODE_ILLEGAL_STATE;
        } else {
            sSt->aid = 0;
            sSt->child_bitmap = sSt->mp_readyBitmap = 0;
            SetSizes(sSt->pparam.parentMaxSize, sSt->pparam.childMaxSize);
            sSt->state = WM_STATE_PARENT;
            sSt->beaconIndicateFlag = 1; /* as req_StartParent.c: BEACON_SENT from now on */
            cb.errcode = WM_ERRCODE_SUCCESS;
        }
        cb.state = WM_STATECODE_PARENT_START;
        cb.parentSize = sSt->mp_parentSize;
        cb.childSize = sSt->mp_childSize;
        Post(&cb, sizeof cb);
        break;
    }
    case WM_APIID_END_PARENT:
        FlushQueue(WM_ERRCODE_SEND_FAILED);
        DropAllChildren(WM_STATECODE_DISCONNECTED_FROM_MYSELF);
        sSt->mp_flag = FALSE;
        sSt->state = WM_STATE_IDLE;
        sSt->beaconIndicateFlag = 0;
        PostSimple(id, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_START_SCAN: {
        const WMStartScanReq *r = (const WMStartScanReq *)c->w;
        if (sSt->state != WM_STATE_IDLE && sSt->state != WM_STATE_CLASS1 && sSt->state != WM_STATE_SCAN) {
            WMStartScanCallback cb = { 0 };
            cb.apiid = id;
            cb.errcode = WM_ERRCODE_ILLEGAL_STATE;
            cb.state = WM_STATECODE_PARENT_NOT_FOUND;
            Post(&cb, sizeof cb);
            break;
        }
        sSt->state = WM_STATE_SCAN;
        sScan.active = 1;
        sScan.channel = r->channel;
        sScan.buf = r->scanBuf;
        sScan.deadline = sFrame + 1 + r->maxChannelTime * 60u / 1000u;
        memcpy(sScan.bssid, r->bssid, 6);
        break;
    }
    case WM_APIID_END_SCAN:
        sScan.active = 0;
        sSt->state = WM_STATE_IDLE;
        PostSimple(id, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_START_CONNECT: {
        const WMStartConnectReq *r = (const WMStartConnectReq *)c->w;
        WMStartConnectCallback cb = { 0 };
        cb.apiid = id;
        cb.state = WM_STATECODE_CONNECT_START;
        if (sSt->state != WM_STATE_IDLE || (sSt->miscFlags & WM_MISC_FLAG_LISTEN_ONLY)) {
            cb.errcode = WM_ERRCODE_ILLEGAL_STATE;
            Post(&cb, sizeof cb);
            break;
        }
        cb.errcode = WM_ERRCODE_SUCCESS;
        Post(&cb, sizeof cb);
        sSt->state = WM_STATE_CLASS1;
        sConn.active = 1;
        memcpy(sConn.mac, r->pInfo->bssid, 6);
        memcpy(sConn.ssid, r->ssid, WM_SIZE_CHILD_SSID);
        sConn.gi = r->pInfo->gameInfo;
        sConn.powerSave = (u16)r->powerSave;
        sConn.deadline = sFrame + 120;
        Send(P_ASSOC_REQ, sConn.mac, sConn.ssid, WM_SIZE_CHILD_SSID);
        break;
    }
    case WM_APIID_DISCONNECT: {
        u16 bitmap = (u16)a[1];
        WMDisconnectCallback cb = { 0 };
        cb.apiid = id;
        cb.tryBitmap = bitmap;
        if (AmParent()) {
            for (u16 aid = 1; aid <= WM_NUM_MAX_CHILD; aid++) {
                if (bitmap & sSt->child_bitmap & (1u << aid)) {
                    u16 rr[2] = { aid, WM_DISCONNECT_REASON_DISASSOC_LEAVING };
                    Send(P_DISASSOC, sChildMac[aid], rr, sizeof rr);
                    ParentLostChild(aid, WM_STATECODE_DISCONNECTED_FROM_MYSELF, WM_DISCONNECT_REASON_FROM_MYSELF);
                    cb.disconnectedBitmap |= (u16)(1u << aid);
                }
            }
        } else if (AmChild()) {
            LeaveParent();
            cb.disconnectedBitmap = 1;
        }
        cb.errcode = WM_ERRCODE_SUCCESS;
        Post(&cb, sizeof cb);
        break;
    }
    case WM_APIID_START_MP: {
        const WMStartMPReq *r = (const WMStartMPReq *)c->w;
        WMStartMPCallback cb = { 0 };
        cb.apiid = id;
        cb.state = WM_STATECODE_MP_START;
        if (sSt->state != WM_STATE_PARENT && sSt->state != WM_STATE_CHILD) {
            cb.errcode = WM_ERRCODE_ILLEGAL_STATE;
        } else {
            sSt->mp_recvBuf[0] = sSt->mp_recvBuf[1] = (WMMpRecvBuf *)r->recvBuf;
            sSt->mp_recvBufSize = (u16)r->recvBufSize;
            sSt->mp_sendBuf = r->sendBuf;
            sSt->mp_sendBufSize = (u16)r->sendBufSize;
            sSt->state = sSt->state == WM_STATE_CHILD ? WM_STATE_MP_CHILD : WM_STATE_MP_PARENT;
            sSt->mp_flag = TRUE;
            cb.errcode = WM_ERRCODE_SUCCESS;
            if (sSt->state == WM_STATE_MP_CHILD) Send(P_MP_READY, sParentMac, &sSt->aid, 2);
        }
        Post(&cb, sizeof cb);
        break;
    }
    case WM_APIID_END_MP:
        SendQueued(); /* the DS radio sends during the frame, before a later end request: no assert in the game's send callback */
        FlushQueue(WM_ERRCODE_SEND_FAILED);
        sSt->mp_flag = FALSE;
        if (sSt->state == WM_STATE_MP_CHILD) sSt->state = WM_STATE_CHILD;
        else if (sSt->state == WM_STATE_MP_PARENT) sSt->state = WM_STATE_PARENT;
        PostSimple(id, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_SET_MP_DATA: {
        QItem q = { (const u16 *)(uintptr_t)a[1], (u16)a[2], (u16)a[3], (u16)a[4], (u16)a[5],
                    (WMCallbackFunc)(uintptr_t)a[6], (void *)(uintptr_t)a[7] };
        if (sQn < WM_SEND_QUEUE_NUM) {
            sQ[sQn++] = q;
        } else {
            WMPortSendCallback cb = { 0 };
            cb.apiid = WM_APIID_PORT_SEND;
            cb.errcode = WM_ERRCODE_SEND_QUEUE_FULL;
            cb.port = q.port;
            cb.data = q.data;
            cb.size = q.size;
            cb.callback = q.cb;
            cb.arg = q.arg;
            Post(&cb, sizeof cb);
        }
        break;
    }
    case WM_APIID_MEASURE_CHANNEL: {
        const WMMeasureChannelReq *r = (const WMMeasureChannelReq *)c->w;
        WMMeasureChannelCallback cb = { 0 };
        cb.apiid = id;
        cb.errcode = WM_ERRCODE_SUCCESS;
        cb.channel = r->channel;
        cb.ccaBusyRatio = 0;
        Post(&cb, sizeof cb);
        break;
    }
    default: /* lifetime, beacon period/indication, WEP, power-save, MP parameters, counters... */
        if (id >= WM_APIID_ASYNC_KIND_MAX) fprintf(stderr, "hg_pc: wm: unexpected request %u\n", id);
        PostSimple(id, WM_ERRCODE_SUCCESS);
        break;
    }
}

/* ---------------------------------------------------------------- packets */
static void Receive(const PktHdr *h, const u8 *p) {
    BOOL forMe = !memcmp(h->dst, sMac, 6) || !memcmp(h->dst, kBroadcast, 6);
    if (!forMe || sSt == NULL) return;
    switch (h->type) {
    case P_BEACON: {
        int slot = 0;
        for (int i = 0; i < 8; i++) {
            if (!memcmp(sHeard[i].mac, h->src, 6)) { slot = i; break; }
            if (sHeard[i].frame < sHeard[slot].frame) slot = i;
        }
        memcpy(sHeard[slot].mac, h->src, 6);
        sHeard[slot].frame = sFrame;
        memcpy(&sHeard[slot].b, p, sizeof(Beacon));
        LOG("beacon ch %u ggid %08x tgid %u attr %02x\n", sHeard[slot].b.channel, (unsigned)sHeard[slot].b.gameInfo.ggid,
            sHeard[slot].b.gameInfo.tgid, sHeard[slot].b.gameInfo.attribute);
        if (AmChild() && !memcmp(h->src, sParentMac, 6)) {
            sParentSeen = sFrame;
            if (sSt->beaconIndicateFlag) { /* as WmspIndicateMlmeBeaconRecv; shortcut: no auto-disconnect on a TGID change */
                const Beacon *b = &sHeard[slot].b;
                WMBeaconRecvIndCallback cb = { 0 };
                cb.apiid = WM_APIID_INDICATION;
                cb.errcode = WM_ERRCODE_SUCCESS;
                cb.state = WM_STATECODE_BEACON_RECV;
                cb.tgid = b->gameInfo.tgid;
                cb.wmstate = sSt->state;
                cb.gameInfoLength = b->gameInfoLength;
                if (b->gameInfoLength <= 128) memcpy(&cb.gameInfo, &b->gameInfo, (b->gameInfoLength + 1u) & ~1u);
                Post(&cb, sizeof cb);
            }
        }
        break;
    }
    case P_ASSOC_REQ: {
        u16 resp[3] = { 0, sSt->mp_parentSize, sSt->mp_childSize };
        if (AmParent() && sSt->pparam.entryFlag) {
            for (u16 aid = 1; aid <= sSt->pparam.maxEntry && aid <= WM_NUM_MAX_CHILD; aid++) {
                if ((sSt->child_bitmap & (1u << aid)) && !memcmp(sChildMac[aid], h->src, 6)) { resp[0] = aid; break; }
            }
            for (u16 aid = 1; !resp[0] && aid <= sSt->pparam.maxEntry && aid <= WM_NUM_MAX_CHILD; aid++) {
                if (!(sSt->child_bitmap & (1u << aid))) resp[0] = aid;
            }
        }
        Send(P_ASSOC_RESP, h->src, resp, sizeof resp);
        if (resp[0] && !(sSt->child_bitmap & (1u << resp[0]))) {
            u16 aid = resp[0];
            memcpy(sChildMac[aid], h->src, 6);
            sChildSeen[aid] = sFrame;
            sSt->child_bitmap |= (u16)(1u << aid);
            sSt->mp_readyBitmap &= (u16)~(1u << aid);
            for (int i = 0; i < WM_NUM_OF_SEQ_PORT; i++) sSeq[aid][i] = 0;
            WMStartParentCallback cb = { 0 };
            cb.apiid = WM_APIID_START_PARENT;
            cb.errcode = WM_ERRCODE_SUCCESS;
            cb.state = WM_STATECODE_CONNECTED;
            memcpy(cb.macAddress, h->src, 6);
            cb.aid = aid;
            memcpy(cb.ssid, p, WM_SIZE_CHILD_SSID);
            cb.parentSize = sSt->mp_parentSize;
            cb.childSize = sSt->mp_childSize;
            Post(&cb, sizeof cb);
        }
        break;
    }
    case P_ASSOC_RESP: {
        if (!sConn.active || memcmp(h->src, sConn.mac, 6)) break;
        const u16 *resp = (const u16 *)p;
        sConn.active = 0;
        WMStartConnectCallback cb = { 0 };
        cb.apiid = WM_APIID_START_CONNECT;
        memcpy(cb.macAddress, h->src, 6);
        if (resp[0] == 0) {
            sSt->state = WM_STATE_IDLE;
            cb.errcode = WM_ERRCODE_NO_ENTRY;
            cb.state = WM_STATECODE_CONNECT_START;
            Post(&cb, sizeof cb);
            break;
        }
        sSt->aid = resp[0];
        memcpy(sParentMac, h->src, 6);
        memcpy(sSt->parentMacAddress, h->src, 6);
        sParentSeen = sFrame;
        sSt->child_bitmap = sSt->mp_readyBitmap = 1;
        sSt->linkLevel = WM_LINK_LEVEL_3;
        sSt->pwrMgtMode = sConn.powerSave;
        sSt->state = WM_STATE_CHILD;
        sSt->beaconIndicateFlag = 1; /* as req_StartConnect.c */
        SetSizes(sConn.gi.parentMaxSize, sConn.gi.childMaxSize);
        for (int i = 0; i < WM_NUM_OF_SEQ_PORT; i++) sSeq[0][i] = 0;
        cb.errcode = WM_ERRCODE_SUCCESS;
        cb.state = WM_STATECODE_CONNECTED;
        cb.aid = sSt->aid;
        cb.parentSize = sSt->mp_parentSize;
        cb.childSize = sSt->mp_childSize;
        Post(&cb, sizeof cb);
        break;
    }
    case P_MP_READY:
        if (AmParent()) sSt->mp_readyBitmap |= (u16)(1u << *(const u16 *)p);
        break;
    case P_PING:
        break;
    case P_DISASSOC: {
        const u16 *r = (const u16 *)p;
        if (AmParent()) ParentLostChild(r[0], WM_STATECODE_DISCONNECTED, r[1]);
        else if (AmChild() && !memcmp(h->src, sParentMac, 6)) ChildLost(WM_STATECODE_DISCONNECTED, r[1]);
        break;
    }
    case P_DATA: {
        const DataPkt *d = (const DataPkt *)p;
        if (!sSt->mp_flag) break;
        if (AmChild() && memcmp(h->src, sParentMac, 6)) break;
        if (AmParent() && (d->srcAid == 0 || d->srcAid > WM_NUM_MAX_CHILD || memcmp(sChildMac[d->srcAid], h->src, 6))) break;
        memcpy(sRxData, d->data, d->size);
        WMPortRecvCallback cb = { 0 };
        cb.apiid = WM_APIID_PORT_RECV;
        cb.errcode = WM_ERRCODE_SUCCESS;
        cb.state = WM_STATECODE_PORT_RECV;
        cb.port = d->port;
        cb.recvBuf = sSt->mp_recvBuf[0];
        cb.data = sRxData;
        cb.length = d->size;
        cb.aid = d->srcAid;
        memcpy(cb.macAddress, h->src, 6);
        cb.seqNo = d->seqNo;
        cb.myAid = sSt->aid;
        cb.maxSendDataSize = sSt->aid == 0 ? sSt->mp_parentSize : sSt->mp_childSize;
        cb.maxRecvDataSize = sSt->aid == 0 ? sSt->mp_childSize : sSt->mp_parentSize;
        Post(&cb, sizeof cb);
        break;
    }
    }
    if (AmParent()) {
        for (u16 aid = 1; aid <= WM_NUM_MAX_CHILD; aid++) {
            if ((sSt->child_bitmap & (1u << aid)) && !memcmp(sChildMac[aid], h->src, 6)) sChildSeen[aid] = sFrame;
        }
    } else if (AmChild() && !memcmp(h->src, sParentMac, 6)) {
        sParentSeen = sFrame;
    }
}

/* A packet off the air (UDP or the server): it counts as arrived HG_WM_DELAY frames from now. */
void pc_wm_deliver(const void *pkt, int n) {
    static int delay = -1;
    if (delay < 0) delay = getenv("HG_WM_DELAY") ? atoi(getenv("HG_WM_DELAY")) : 0;
    const PktHdr *h = (const PktHdr *)pkt;
    if (n < (int)sizeof *h || n > (int)MAX_PKT) return;
    if (h->magic != MAGIC || h->len + sizeof *h != (size_t)n || !memcmp(h->src, sMac, 6)) return;
    if ((sHeldTail + 1) % 256 == sHeldHead) return; /* full: lost, like a missed radio packet */
    Held *e = &sHeld[sHeldTail];
    memcpy(e->buf, pkt, n);
    e->n = (u16)n;
    e->due = sFrame + (u32)delay;
    sHeldTail = (sHeldTail + 1) % 256;
}

static void Poll(void) {
    for (; sSock != BAD_SOCK;) {
        u8 buf[MAX_PKT];
        int n = (int)recv(sSock, (char *)buf, sizeof buf, 0);
        if (n < (int)sizeof(PktHdr)) break;
        pc_wm_deliver(buf, n);
    }
    while (sHeldHead != sHeldTail && (s32)(sFrame - sHeld[sHeldHead].due) >= 0) {
        const Held *e = &sHeld[sHeldHead];
        const PktHdr *h = (const PktHdr *)e->buf;
        LOG("rx type %u from %02x:%02x len %u\n", h->type, h->src[4], h->src[5], h->len);
        Receive(h, e->buf + sizeof *h);
        sHeldHead = (sHeldHead + 1) % 256;
    }
}

/* One MP cycle: every queued send goes out, and its sender hears it was sent. */
static void SendQueued(void) {
    if (!sSt->mp_flag) return;
    while (sQn > 0) {
        QItem q = sQ[0];
        memmove(sQ, sQ + 1, --sQn * sizeof sQ[0]);
        DataPkt d;
        d.port = q.port;
        d.srcAid = sSt->aid;
        d.size = q.size > WM_SIZE_MP_DATA_MAX ? WM_SIZE_MP_DATA_MAX : q.size;
        memcpy(d.data, q.data, d.size);
        u16 sent = 0;
        u16 len = (u16)(offsetof(DataPkt, data) + d.size);
        if (AmParent()) {
            for (u16 aid = 1; aid <= WM_NUM_MAX_CHILD; aid++) {
                if (!(q.dest & sSt->child_bitmap & (1u << aid))) continue;
                d.seqNo = q.port < WM_NUM_OF_SEQ_PORT ? (u16)(sSeq[aid][q.port]++ << 1) : 0xffff;
                Send(P_DATA, sChildMac[aid], &d, len);
                sent |= (u16)(1u << aid);
            }
        } else if (q.dest & 1) {
            d.seqNo = q.port < WM_NUM_OF_SEQ_PORT ? (u16)(sSeq[0][q.port]++ << 1) : 0xffff;
            Send(P_DATA, sParentMac, &d, len);
            sent = 1;
        }
        WMPortSendCallback cb = { 0 };
        cb.apiid = WM_APIID_PORT_SEND;
        /* As wmsp_port.c: failed only if a still-connected destination missed it; partners that
         * left drop out of the bitmaps. */
        u16 rest = (u16)(q.dest & (AmParent() ? sSt->child_bitmap : 1) & ~sent);
        cb.errcode = rest == 0 ? WM_ERRCODE_SUCCESS : WM_ERRCODE_SEND_FAILED;
        cb.state = WM_STATECODE_PORT_SEND;
        cb.port = q.port;
        cb.destBitmap = q.dest;
        cb.restBitmap = rest;
        cb.sentBitmap = sent;
        cb.data = q.data;
        cb.size = q.size;
        cb.seqNo = d.seqNo;
        cb.callback = q.cb;
        cb.arg = q.arg;
        cb.maxSendDataSize = sSt->aid == 0 ? sSt->mp_parentSize : sSt->mp_childSize;
        cb.maxRecvDataSize = sSt->aid == 0 ? sSt->mp_childSize : sSt->mp_parentSize;
        Post(&cb, sizeof cb);
    }
}

static void FinishScan(void) {
    if (!sScan.active) return;
    int found = -1;
    for (int i = 0; i < 8; i++) {
        if (sHeard[i].frame == 0 || sFrame - sHeard[i].frame > 60 || sHeard[i].b.channel != sScan.channel) continue;
        if (memcmp(sScan.bssid, kBroadcast, 6) && memcmp(sScan.bssid, sHeard[i].mac, 6)) continue;
        found = i;
        break;
    }
    if (found < 0 && (s32)(sFrame - sScan.deadline) < 0) return;
    sScan.active = 0;
    WMStartScanCallback cb = { 0 };
    cb.apiid = WM_APIID_START_SCAN;
    cb.errcode = WM_ERRCODE_SUCCESS;
    cb.channel = sScan.channel;
    LOG("scan ch %u: %s\n", sScan.channel, found < 0 ? "nobody" : "found a parent");
    if (found < 0) {
        cb.state = WM_STATECODE_PARENT_NOT_FOUND;
        Post(&cb, sizeof cb);
        return;
    }
    const Beacon *b = &sHeard[found].b;
    WMBssDesc *d = sScan.buf;
    memset(d, 0, sizeof *d);
    d->length = sizeof(WMBssDesc) / 2;
    d->rssi = 0x30;
    memcpy(d->bssid, sHeard[found].mac, 6);
    d->ssidLength = WM_SIZE_SSID;
    memcpy(d->ssid, &b->gameInfo.ggid, 4);
    memcpy(d->ssid + 4, &b->gameInfo.tgid, 2);
    d->capaInfo = 0x0021;
    d->rateSet.basic = 0x0003;
    d->rateSet.support = 0x0003;
    d->beaconPeriod = WM_DEFAULT_BEACON_PERIOD;
    d->dtimPeriod = 1;
    d->channel = b->channel;
    d->gameInfoLength = b->gameInfoLength;
    d->gameInfo = b->gameInfo;
    cb.state = WM_STATECODE_PARENT_FOUND;
    memcpy(cb.macAddress, d->bssid, 6);
    cb.linkLevel = WM_LINK_LEVEL_3;
    cb.ssidLength = d->ssidLength;
    memcpy(cb.ssid, d->ssid, WM_SIZE_SSID);
    cb.gameInfoLength = b->gameInfoLength;
    cb.gameInfo = b->gameInfo;
    Post(&cb, sizeof cb);
}

/* ---------------------------------------------------------------- per frame */
void pc_wm_frame(void) {
    sFrame++;
    for (int n = 0; n < 64 && sCmdHead != sCmdTail; n++) { /* answers can trigger new requests */
        Cmd c = sCmds[sCmdHead];
        sCmdHead = (sCmdHead + 1) % 64;
        Handle(&c);
    }
    if (sSt == NULL) return;
    Poll();
    if (AmParent() && sFrame % 6 == 0) {
        Beacon b = { 0 };
        b.channel = sSt->pparam.channel;
        b.gameInfoLength = sGameInfoLen;
        b.gameInfo = sGameInfo;
        Send(P_BEACON, kBroadcast, &b, sizeof b);
        if (sSt->beaconIndicateFlag) { /* as WmspIndicateMlmeBeaconSend */
            WMStartParentCallback cb = { 0 };
            cb.apiid = WM_APIID_START_PARENT;
            cb.errcode = WM_ERRCODE_SUCCESS;
            cb.state = WM_STATECODE_BEACON_SENT;
            Post(&cb, sizeof cb);
        }
    }
    if (AmChild() && sFrame % 30 == 0) Send(P_PING, sParentMac, NULL, 0);
    FinishScan();
    if (sConn.active && (s32)(sFrame - sConn.deadline) >= 0) {
        sConn.active = 0;
        sSt->state = WM_STATE_IDLE;
        WMStartConnectCallback cb = { 0 };
        cb.apiid = WM_APIID_START_CONNECT;
        cb.errcode = WM_ERRCODE_FAILED;
        cb.state = WM_STATECODE_CONNECT_START;
        Post(&cb, sizeof cb);
    }
    SendQueued();
    /* A partner silent for 10 s is gone (a closed copy sends no goodbye). */
    if (AmParent()) {
        for (u16 aid = 1; aid <= WM_NUM_MAX_CHILD; aid++) {
            if ((sSt->child_bitmap & (1u << aid)) && sFrame - sChildSeen[aid] > 600) {
                ParentLostChild(aid, WM_STATECODE_DISCONNECTED, WM_DISCONNECT_REASON_MP_LIFETIME);
            }
        }
    } else if (AmChild() && sFrame - sParentSeen > 600) {
        ChildLost(WM_STATECODE_DISCONNECTED, WM_DISCONNECT_REASON_MP_LIFETIME);
    }
}
