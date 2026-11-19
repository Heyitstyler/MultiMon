/* hg_server: multiplayer server for hg_pc. It runs no game and needs no ROM.
 *
 *   hg_server [--port 7777] [--max 16] [--password secret] [--chat all|map] [--admin secret]
 *             [--log hg_server.log] [--bans hg_server_bans.txt] [--accounts hg_server_accounts.txt]
 *
 * --chat map: chat lines reach only players on the sender's map (default: everyone).
 * --admin: the password for the chat commands /kick, /ban, /unban, /mute, /unmute, /forget,
 *          /passwords (type /login secret in the game's chat first). /who and /help work for everyone.
 * --accounts: where player passwords are kept. /passwords on|off turns them on or off (saved in the
 *          file, so it lasts across restarts; off at first). While on, a name's first login sets its
 *          password (the game asks for one); after that, only someone with that password can use the
 *          name. The file holds a name, a random salt and a salted, stretched SHA-256 per line, never
 *          the password itself; turning passwords off keeps them. /forget NAME deletes an account
 *          (forgotten password). Five wrong passwords lock a name for a minute. Players change their
 *          own with /password in the game's chat (the game asks for the current and new one, NM_PASSWORD).
 * --log:   also append the log to this file (it always goes to stdout).
 * --bans:  banned IP addresses, one per line; /ban and /unban rewrite it.
 *
 * Players connect over TCP (pc_net_proto.h). The server checks their versions and the password,
 * tells everyone who is online, and relays each player's position to all others; each game decides
 * whom to draw. It also carries the games' wireless packets (trades, battles, Union Room), so all
 * players online are in wireless range of each other. Everyone on a server must have the same game data: the first player to join sets
 * it, later players with different data are refused (until the server is empty again).
 *
 * Against strangers: a player who doesn't log in within 10 s, or sends nothing for 30 s (games ping
 * every 5 s), is dropped; one who sends more than RATE_BYTES per second (link play peaks far lower)
 * is kicked; chat and invitations are rate-limited.
 */
#define _CRT_RAND_S /* Windows: rand_s, the system's secure random numbers */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET Sock;
#define BAD_SOCK INVALID_SOCKET
#define CloseSock closesocket
#define strcasecmp _stricmp
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Sock;
#define BAD_SOCK (-1)
#define CloseSock close
#endif
#include "pc_net_proto.h"

#define HELLO_MS     10000
#define SILENT_MS    30000
#define PING_MS      5000
#define RATE_BYTES   (64 * 1024)  /* per second, sustained */
#define BURST_BYTES  (256 * 1024)
#define CHAT_BURST   5            /* lines at once, then one per CHAT_MS */
#define CHAT_MS      2000
#define INVITE_MS    3000
#define MAX_LIST     256
#define MAX_ACCOUNTS 4096
#define HASH_ROUNDS  10000
#define LOCK_FAILS   5
#define LOCK_MS      60000

typedef struct {
    char name[NET_NAME_LEN + 1];
    unsigned char salt[16], hash[32];
    int fails;
    long long lockedUntil;
} Account;

typedef struct {
    Sock s;
    int welcomed, admin;
    char name[NET_NAME_LEN + 1];
    unsigned char in[4096];
    int inLen;
    NetPos pos;
    int hasPos;
    unsigned char mac[6]; /* of the player's game, learned from its wireless packets */
    struct in_addr ip;
    long long since, heard;
    long long bytes, bytesAt;   /* send allowance (token bucket) */
    long long chat, chatAt, invite, inviteAt;
    long long secBytes, secAt, peak;
    long long mapSince;   /* when they entered their current map (a sequence number) */
    int leads, leadMap;   /* leads the NPCs of leadMap (NM_LEAD) */
} Client;

static Client sClients[NET_MAX_PLAYERS];
static int sMax = NET_MAX_PLAYERS;
static const char *sPassword = "", *sAdmin = "", *sBanFile = "hg_server_bans.txt";
static int sChatMap;
static uint32_t sGame;
static FILE *sLogFile;
static struct in_addr sBans[MAX_LIST], sMutes[MAX_LIST];
static int sBanCount, sMuteCount;
static const char *sAccountFile = "hg_server_accounts.txt";
static int sAccountsOn; /* /passwords on|off */
static Account sAccounts[MAX_ACCOUNTS];
static int sAccountCount;

static void Log(const char *fmt, ...);

/* SHA-256 (FIPS 180-4). */
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void Sha256(const unsigned char *msg, size_t len, unsigned char out[32]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    size_t blocks = (len + 9 + 63) / 64;
    for (size_t b = 0; b < blocks; b++) {
        unsigned char block[64];
        uint32_t w[64], s[8];
        for (int i = 0; i < 64; i++) {
            size_t at = b * 64 + i;
            block[i] = at < len ? msg[at] : at == len ? 0x80 : 0;
        }
        if (b == blocks - 1) {
            for (int i = 0; i < 8; i++) block[56 + i] = (unsigned char)((uint64_t)len * 8 >> (56 - 8 * i));
        }
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)block[4 * i] << 24 | block[4 * i + 1] << 16 | block[4 * i + 2] << 8 | block[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        memcpy(s, h, sizeof s);
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = s[7] + (ROR(s[4], 6) ^ ROR(s[4], 11) ^ ROR(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + k[i] + w[i];
            uint32_t t2 = (ROR(s[0], 2) ^ ROR(s[0], 13) ^ ROR(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
            memmove(s + 1, s, 7 * sizeof *s);
            s[4] += t1;
            s[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++) h[i] += s[i];
    }
    for (int i = 0; i < 32; i++) out[i] = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));
}

/* hash = SHA-256(salt + password), then HASH_ROUNDS - 1 times SHA-256(hash + salt): slow to guess. */
static void HashPassword(const unsigned char salt[16], const char *password, unsigned char out[32]) {
    unsigned char buf[16 + 64];
    size_t n = strnlen(password, 48);
    memcpy(buf, salt, 16);
    memcpy(buf + 16, password, n);
    Sha256(buf, 16 + n, out);
    for (int i = 1; i < HASH_ROUNDS; i++) {
        memcpy(buf, out, 32);
        memcpy(buf + 32, salt, 16);
        Sha256(buf, 48, out);
    }
}

static void RandomBytes(unsigned char *out, int n) {
#ifdef _WIN32
    for (int i = 0; i < n; i++) {
        unsigned int r;
        rand_s(&r);
        out[i] = (unsigned char)r;
    }
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f == NULL || fread(out, 1, n, f) != (size_t)n) {
        fprintf(stderr, "hg_server: no /dev/urandom\n");
        exit(1);
    }
    fclose(f);
#endif
}

static void Hex(const unsigned char *b, int n, char *out) {
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}
static int Unhex(const char *s, unsigned char *b, int n) {
    for (int i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return 0;
        b[i] = (unsigned char)v;
    }
    return 1;
}

static Account *FindAccount(const char *name) {
    for (int i = 0; i < sAccountCount; i++) if (!strcasecmp(sAccounts[i].name, name)) return &sAccounts[i];
    return NULL;
}

static void LoadAccounts(void) {
    char line[1024];
    FILE *f = fopen(sAccountFile, "r");
    if (f == NULL) { /* a crash between SaveAccounts' remove and rename leaves only the new copy */
        snprintf(line, sizeof line, "%s.tmp", sAccountFile);
        f = fopen(line, "r");
    }
    if (f == NULL) return;
    while (fgets(line, sizeof line, f) != NULL && sAccountCount < MAX_ACCOUNTS) {
        if (!strncmp(line, "passwords ", 10)) sAccountsOn = !strncmp(line + 10, "on", 2);
        char *salt = strchr(line, '\t'), *hash = salt != NULL ? strchr(salt + 1, '\t') : NULL;
        if (hash == NULL || salt - line > NET_NAME_LEN) continue;
        *salt++ = 0;
        *hash++ = 0;
        Account *a = &sAccounts[sAccountCount];
        memset(a, 0, sizeof *a);
        strcpy(a->name, line);
        if (Unhex(salt, a->salt, 16) && Unhex(hash, a->hash, 32) && FindAccount(a->name) == NULL) sAccountCount++;
    }
    fclose(f);
}

/* Rewrites the file through a temporary one, so a crash mid-write can't lose the accounts. */
static void SaveAccounts(void) {
    char tmp[1024], salt[33], hash[65];
    snprintf(tmp, sizeof tmp, "%s.tmp", sAccountFile);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) return Log("cannot write %s", tmp);
    fprintf(f, "passwords %s\n", sAccountsOn ? "on" : "off");
    for (int i = 0; i < sAccountCount; i++) {
        Hex(sAccounts[i].salt, 16, salt);
        Hex(sAccounts[i].hash, 32, hash);
        fprintf(f, "%s\t%s\t%s\n", sAccounts[i].name, salt, hash);
    }
    if (fclose(f) != 0) return Log("cannot write %s", tmp);
    remove(sAccountFile); /* Windows rename doesn't replace */
    if (rename(tmp, sAccountFile) != 0) Log("cannot write %s", sAccountFile);
}

static long long Now(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
#endif
}

static void Log(const char *fmt, ...) {
    char t[32], line[512];
    time_t now = time(NULL);
    strftime(t, sizeof t, "%Y-%m-%d %H:%M:%S", localtime(&now));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("%s %s\n", t, line);
    fflush(stdout);
    if (sLogFile != NULL) {
        fprintf(sLogFile, "%s %s\n", t, line);
        fflush(sLogFile);
    }
}

static int InList(const struct in_addr *l, int n, struct in_addr ip) {
    for (int i = 0; i < n; i++) if (l[i].s_addr == ip.s_addr) return 1;
    return 0;
}
static void AddToList(struct in_addr *l, int *n, struct in_addr ip) {
    if (!InList(l, *n, ip) && *n < MAX_LIST) l[(*n)++] = ip;
}
static int RemoveFromList(struct in_addr *l, int *n, struct in_addr ip) {
    for (int i = 0; i < *n; i++) {
        if (l[i].s_addr == ip.s_addr) {
            l[i] = l[--*n];
            return 1;
        }
    }
    return 0;
}

static void LoadBans(void) {
    FILE *f = fopen(sBanFile, "r");
    char line[64];
    if (f == NULL) return;
    while (fgets(line, sizeof line, f) != NULL) {
        line[strcspn(line, " \t\r\n#")] = 0;
        struct in_addr ip;
        ip.s_addr = inet_addr(line);
        if (*line && ip.s_addr != INADDR_NONE) AddToList(sBans, &sBanCount, ip);
    }
    fclose(f);
}

static void SaveBans(void) {
    FILE *f = fopen(sBanFile, "w");
    if (f == NULL) return Log("cannot write %s", sBanFile);
    for (int i = 0; i < sBanCount; i++) fprintf(f, "%s\n", inet_ntoa(sBans[i]));
    fclose(f);
}

static void Drop(int id);

/* shortcut: a send that would block drops the player; fine while players read every frame */
static void Send(int id, int type, const void *body, int len) {
    unsigned char buf[3 + NET_MAX_MSG];
    buf[0] = (unsigned char)(len + 1);
    buf[1] = (unsigned char)((len + 1) >> 8);
    buf[2] = (unsigned char)type;
    if (len > 0) memcpy(buf + 3, body, len);
    if (sClients[id].s == BAD_SOCK) return;
    if (send(sClients[id].s, (const char *)buf, len + 3, 0) != len + 3) Drop(id);
}

static int Here(int i) { return sClients[i].s != BAD_SOCK && sClients[i].welcomed; }

static void Broadcast(int except, int type, const void *body, int len) {
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (i != except && Here(i)) Send(i, type, body, len);
    }
}

static int Online(void) {
    int n = 0;
    for (int i = 0; i < NET_MAX_PLAYERS; i++) n += Here(i);
    return n;
}

/* A chat line from the server to one player (shown as a notice). */
static void Notice(int id, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void Notice(int id, const char *fmt, ...) {
    NetChat m;
    memset(&m, 0, sizeof m);
    m.id = NET_SERVER_ID;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m.text, sizeof m.text, fmt, ap);
    va_end(ap);
    Send(id, NM_CHAT, &m, sizeof m);
}

/* NPC leaders: on each map with players, the one there longest (lowest mapSince) leads its NPCs.
 * Tells every player whose part changed. */
static void UpdateLeaders(void) {
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        Client *c = &sClients[i];
        if (!Here(i)) continue;
        int lead = c->hasPos;
        for (int j = 0; j < NET_MAX_PLAYERS && lead; j++) {
            Client *o = &sClients[j];
            if (j != i && Here(j) && o->hasPos && o->pos.map == c->pos.map && (o->mapSince < c->mapSince || (o->mapSince == c->mapSince && j < i))) lead = 0;
        }
        if (lead == c->leads && (!lead || c->leadMap == c->pos.map)) continue;
        c->leads = lead;
        c->leadMap = c->pos.map;
        NetLead l = { (uint8_t)lead, c->pos.map };
        Send(i, NM_LEAD, &l, sizeof l);
    }
}

static void Drop(int id) {
    Client *c = &sClients[id];
    if (c->s == BAD_SOCK) return;
    CloseSock(c->s);
    c->s = BAD_SOCK;
    if (c->welcomed) {
        if (c->secBytes > c->peak) c->peak = c->secBytes;
        Log("%s left (player %d, peak %lld KB/s)", c->name, id, c->peak / 1024);
        NetLeave l = { (uint8_t)id };
        Broadcast(id, NM_LEAVE, &l, sizeof l);
    }
    c->welcomed = 0;
    UpdateLeaders();
}

/* Tells the player why, then disconnects them; their game doesn't reconnect after this. */
static void Reject(int id, const char *why) {
    NetReject r;
    memset(&r, 0, sizeof r);
    snprintf(r.reason, sizeof r.reason, "%s", why);
    Send(id, NM_REJECT, &r, sizeof r);
    if (!sClients[id].welcomed) Log("refused a player from %s: %s", inet_ntoa(sClients[id].ip), why);
    else Log("%s (player %d) removed: %s", sClients[id].name, id, why);
    Drop(id);
}

/* Token bucket: one token earned per `ms`, at most `max` saved; spends one if there is one. */
static int Allow(long long *tokens, long long *at, long long now, long long ms, long long max) {
    *tokens += (now - *at) / ms;
    *at += (now - *at) / ms * ms;
    if (*tokens > max) *tokens = max;
    if (*tokens < 1) return 0;
    --*tokens;
    return 1;
}

/* The game must ask its player for the name's password: it reconnects once they typed it. */
static void Ask(int id, int what) {
    NetAccount a = { (uint8_t)what };
    Send(id, NM_ACCOUNT, &a, sizeof a);
    Reject(id, what == ACCOUNT_NEW ? "choose a password for this name" : what == ACCOUNT_WRONG ? "wrong password for this name" : "password needed for this name");
}

/* Right password? Five wrong ones in a row lock the name for LOCK_MS (callers check lockedUntil). */
static int CheckPassword(Account *a, const char *pw, long long now) {
    unsigned char hash[32];
    HashPassword(a->salt, pw, hash);
    if (memcmp(hash, a->hash, 32) == 0) {
        a->fails = 0;
        return 1;
    }
    if (++a->fails >= LOCK_FAILS) {
        a->fails = 0;
        a->lockedUntil = now + LOCK_MS;
    }
    return 0;
}

/* Creates the name's account, or gives it a new password (new salt too), and saves; 0: no room. */
static int SetPassword(const char *name, const char *pw) {
    Account *a = FindAccount(name);
    if (a == NULL) {
        if (sAccountCount >= MAX_ACCOUNTS) return 0;
        a = &sAccounts[sAccountCount++];
        memset(a, 0, sizeof *a);
        strcpy(a->name, name);
    }
    RandomBytes(a->salt, 16);
    HashPassword(a->salt, pw, a->hash);
    SaveAccounts();
    return 1;
}

/* A player online changes their name's password (sets one if it has none). */
static void ChangePassword(int id, const char *old, const char *pw, long long now) {
    Client *c = &sClients[id];
    Account *a = FindAccount(c->name);
    if (*pw == 0) return Notice(id, "The new password is empty");
    if (a != NULL && now < a->lockedUntil) return Notice(id, "Too many wrong passwords, wait a minute");
    if (a != NULL && !CheckPassword(a, old, now)) {
        Log("%s: wrong password when changing it (%s)", c->name, inet_ntoa(c->ip));
        return Notice(id, "Wrong current password");
    }
    if (!SetPassword(c->name, pw)) return Notice(id, "No room for new names on this server");
    Log("%s %s their password", c->name, a != NULL ? "changed" : "set");
    NetAccount done = { ACCOUNT_CHANGED };
    Send(id, NM_ACCOUNT, &done, sizeof done);
    Notice(id, "Password saved");
}

static void Hello(int id, const NetHello *h) {
    Client *c = &sClients[id];
    if (h->proto != NET_PROTO) return Reject(id, "different hg_pc version");
    if (InList(sBans, sBanCount, c->ip)) return Reject(id, "banned from this server");
    if (strncmp(h->password, sPassword, sizeof h->password) != 0) return Reject(id, "wrong password");
    if (Online() >= sMax) return Reject(id, "server full");
    if (Online() == 0) sGame = h->game;
    if (h->game != sGame) return Reject(id, "different game data (ROM) than the players online");
    memcpy(c->name, h->name, NET_NAME_LEN);
    c->name[NET_NAME_LEN] = 0;
    for (char *p = c->name; *p; p++) if (*p < 32 || *p > 126) *p = '?';
    if (c->name[0] == 0) strcpy(c->name, "Player");
    if (sAccountsOn) {
        char pw[sizeof h->account + 1];
        memcpy(pw, h->account, sizeof h->account);
        pw[sizeof h->account] = 0;
        Account *a = FindAccount(c->name);
        long long now = Now();
        if (a == NULL && pw[0] == 0) return Ask(id, ACCOUNT_NEW);
        if (a == NULL) {
            if (!SetPassword(c->name, pw)) return Reject(id, "no room for new names on this server");
            Log("new account %s (%s)", c->name, inet_ntoa(c->ip));
        } else if (now < a->lockedUntil) {
            return Reject(id, "too many wrong passwords, wait a minute");
        } else if (pw[0] == 0) {
            return Ask(id, ACCOUNT_ENTER);
        } else if (!CheckPassword(a, pw, now)) {
            Log("%s: wrong account password from %s", a->name, inet_ntoa(c->ip));
            return Ask(id, ACCOUNT_WRONG);
        }
        for (int i = 0; i < NET_MAX_PLAYERS; i++) { /* the password proves it's them: an older connection is stale */
            if (i != id && Here(i) && !strcasecmp(sClients[i].name, c->name)) {
                Log("%s: replacing an older connection", c->name);
                Drop(i);
            }
        }
    }
    c->welcomed = 1;
    c->hasPos = 0;
    NetWelcome w = { (uint8_t)id };
    Send(id, NM_WELCOME, &w, sizeof w);
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (i == id || !Here(i)) continue;
        NetJoin j = { (uint8_t)i };
        memcpy(j.name, sClients[i].name, NET_NAME_LEN);
        Send(id, NM_JOIN, &j, sizeof j);
        if (sClients[i].hasPos) Send(id, NM_POS, &sClients[i].pos, sizeof sClients[i].pos);
    }
    NetJoin j = { (uint8_t)id };
    memcpy(j.name, c->name, NET_NAME_LEN);
    Broadcast(id, NM_JOIN, &j, sizeof j);
    Log("%s joined (player %d, %s)", c->name, id, inet_ntoa(c->ip));
}

/* A player by id number or name (any case), or -1. */
static int Find(const char *who) {
    char *end;
    long n = strtol(who, &end, 10);
    if (*who && *end == 0) return n >= 0 && n < NET_MAX_PLAYERS && Here((int)n) ? (int)n : -1;
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (Here(i) && !strcasecmp(sClients[i].name, who)) return i;
    }
    return -1;
}

/* A chat line starting with '/': never relayed. */
static void Command(int id, char *line) {
    Client *c = &sClients[id];
    char *arg = strchr(line, ' ');
    if (arg != NULL) {
        *arg++ = 0;
        while (*arg == ' ') arg++;
    } else {
        arg = line + strlen(line);
    }
    if (!strcasecmp(line, "/who")) {
        for (int i = 0; i < NET_MAX_PLAYERS; i++) {
            if (!Here(i)) continue;
            if (c->admin) Notice(id, "%d %s %s%s", i, sClients[i].name, inet_ntoa(sClients[i].ip), InList(sMutes, sMuteCount, sClients[i].ip) ? " (muted)" : "");
            else Notice(id, "%d %s", i, sClients[i].name);
        }
        return;
    }
    if (!strcasecmp(line, "/login")) {
        if (*sAdmin == 0) return Notice(id, "This server has no admin password");
        if (strcmp(arg, sAdmin) != 0) {
            Log("%s (player %d): wrong admin password", c->name, id);
            return Notice(id, "Wrong admin password");
        }
        c->admin = 1;
        Log("%s (player %d) logged in as admin", c->name, id);
        return Notice(id, "Logged in as admin");
    }
    int isAdminCmd = !strcasecmp(line, "/kick") || !strcasecmp(line, "/ban") || !strcasecmp(line, "/unban") ||
                     !strcasecmp(line, "/mute") || !strcasecmp(line, "/unmute") || !strcasecmp(line, "/forget") || !strcasecmp(line, "/passwords");
    if (!isAdminCmd) {
        return Notice(id, c->admin ? "/who /kick /ban /unban IP /mute /unmute /forget /passwords on/off" : "/who, /password (yours), /login (hosts)");
    }
    if (!c->admin) return Notice(id, "Hosts only: /login password first");
    if (!strcasecmp(line, "/passwords")) { /* players already online stay; it applies to the next logins */
        if (strcasecmp(arg, "on") != 0 && strcasecmp(arg, "off") != 0) {
            return Notice(id, "Player passwords are %s (/passwords on or off)", sAccountsOn ? "on" : "off");
        }
        sAccountsOn = !strcasecmp(arg, "on");
        SaveAccounts();
        Log("%s turned player passwords %s", c->name, sAccountsOn ? "on" : "off");
        return Notice(id, "Player passwords %s", sAccountsOn ? "on" : "off");
    }
    if (!strcasecmp(line, "/forget")) { /* a name's password, so its owner can choose a new one */
        Account *a = FindAccount(arg);
        if (a == NULL) return Notice(id, "No account %.40s", arg);
        char name[NET_NAME_LEN + 1];
        strcpy(name, a->name);
        *a = sAccounts[--sAccountCount];
        SaveAccounts();
        Log("%s deleted the account %s", c->name, name);
        return Notice(id, "Forgot the password of %s", name);
    }
    if (!strcasecmp(line, "/unban")) {
        struct in_addr ip;
        ip.s_addr = inet_addr(arg);
        if (ip.s_addr == INADDR_NONE || !RemoveFromList(sBans, &sBanCount, ip)) return Notice(id, "Not banned: %.40s", arg);
        SaveBans();
        Log("%s unbanned %s", c->name, arg);
        return Notice(id, "Unbanned %.40s", arg);
    }
    int t = Find(arg);
    if (t < 0) return Notice(id, "No player %.40s (see /who)", arg);
    Client *v = &sClients[t];
    char name[NET_NAME_LEN + 1];
    strcpy(name, v->name);
    if (!strcasecmp(line, "/mute") || !strcasecmp(line, "/unmute")) {
        int mute = !strcasecmp(line, "/mute");
        if (mute) AddToList(sMutes, &sMuteCount, v->ip);
        else RemoveFromList(sMutes, &sMuteCount, v->ip);
        Log("%s %s %s", c->name, mute ? "muted" : "unmuted", name);
        return Notice(id, "%s %s", mute ? "Muted" : "Unmuted", name);
    }
    if (!strcasecmp(line, "/ban")) {
        AddToList(sBans, &sBanCount, v->ip);
        SaveBans();
        Log("%s banned %s (%s)", c->name, name, inet_ntoa(v->ip));
        Reject(t, "banned by the host");
    } else {
        Log("%s kicked %s", c->name, name);
        Reject(t, "kicked by the host");
    }
    if (Here(id)) Notice(id, "Removed %s", name);
}

static void Message(int id, int type, const unsigned char *body, int len, long long now) {
    Client *c = &sClients[id];
    if (!c->welcomed) {
        uint32_t proto = 0;
        if (type == NM_HELLO && len >= 4) memcpy(&proto, body, 4); /* first, so older games learn why */
        if (type == NM_HELLO && proto != NET_PROTO) Reject(id, "different hg_pc version");
        else if (type == NM_HELLO && len == sizeof(NetHello)) Hello(id, (const NetHello *)body);
        else Reject(id, "bad hello");
        return;
    }
    if (type == NM_POS && len == sizeof(NetPos)) {
        static long long sMapSeq;
        int newMap = !c->hasPos || ((const NetPos *)body)->map != c->pos.map;
        memcpy(&c->pos, body, sizeof c->pos);
        c->pos.id = (uint8_t)id;
        c->hasPos = 1;
        if (newMap) c->mapSince = ++sMapSeq;
        Broadcast(id, NM_POS, &c->pos, sizeof c->pos);
        if (newMap) UpdateLeaders();
    } else if (type == NM_NPC && len >= NET_NPC_SIZE(0) && len <= (int)sizeof(NetNpc)) {
        NetNpc n;
        memcpy(&n, body, len);
        if (len != NET_NPC_SIZE(n.count) || !c->leads || n.map != c->leadMap) return; /* only a map's leader speaks for its NPCs */
        n.id = (uint8_t)id;
        Broadcast(id, NM_NPC, &n, len);
    } else if (type == NM_CHAT && len == sizeof(NetChat)) {
        NetChat m = *(const NetChat *)body;
        m.id = (uint8_t)id;
        m.text[NET_CHAT_LEN - 1] = 0;
        for (char *p = m.text; *p; p++) if (*p < 32 || *p > 126) *p = '?';
        if (m.text[0] == '/') return Command(id, m.text);
        if (InList(sMutes, sMuteCount, c->ip)) return Notice(id, "You are muted");
        if (!Allow(&c->chat, &c->chatAt, now, CHAT_MS, CHAT_BURST)) return Notice(id, "Slow down");
        Log("chat %s: %s", c->name, m.text);
        for (int i = 0; i < NET_MAX_PLAYERS; i++) {
            if (i == id || !Here(i)) continue;
            if (sChatMap && !(c->hasPos && sClients[i].hasPos && sClients[i].pos.map == c->pos.map)) continue;
            Send(i, NM_CHAT, &m, sizeof m);
        }
    } else if (type == NM_PASSWORD && len == sizeof(NetPassword)) {
        NetPassword p = *(const NetPassword *)body;
        p.password[sizeof p.password - 1] = p.newPassword[sizeof p.newPassword - 1] = 0;
        if (Allow(&c->chat, &c->chatAt, now, CHAT_MS, CHAT_BURST)) ChangePassword(id, p.password, p.newPassword, now);
        else Notice(id, "Slow down");
        memset(&p, 0, sizeof p);
    } else if (type == NM_INVITE && len == sizeof(NetInvite)) {
        NetInvite v = *(const NetInvite *)body;
        int to = v.id;
        if (to == id || to >= NET_MAX_PLAYERS || !Here(to)) return;
        if (v.what == INVITE_ASK && !Allow(&c->invite, &c->inviteAt, now, INVITE_MS, 1)) {
            NetInvite no = { (uint8_t)to, INVITE_NO, 0 }; /* too soon: the inviter sees "can't come now" */
            return Send(id, NM_INVITE, &no, sizeof no);
        }
        v.id = (uint8_t)id;
        if (v.what == INVITE_ASK) Log("%s invites %s to the Union Room", c->name, sClients[to].name);
        Send(to, NM_INVITE, &v, sizeof v);
    } else if (type == NM_WM && len >= NET_WM_DST + 6) {
        static const unsigned char all[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        const unsigned char *dst = body + NET_WM_DST;
        memcpy(c->mac, body + NET_WM_SRC, 6);
        if (!memcmp(dst, all, 6)) return Broadcast(id, NM_WM, body, len);
        for (int i = 0; i < NET_MAX_PLAYERS; i++) {
            if (i != id && Here(i) && !memcmp(sClients[i].mac, dst, 6)) return Send(i, NM_WM, body, len);
        }
    }
}

static void Receive(int id) {
    Client *c = &sClients[id];
    int n = recv(c->s, (char *)c->in + c->inLen, (int)sizeof c->in - c->inLen, 0);
    if (n <= 0) return Drop(id);
    long long now = Now();
    c->heard = now;
    c->secBytes += n;
    if (now - c->secAt >= 1000) {
        if (c->secBytes > c->peak) c->peak = c->secBytes;
        c->secBytes = 0;
        c->secAt = now;
    }
    c->bytes += (now - c->bytesAt) * RATE_BYTES / 1000;
    c->bytesAt = now;
    if (c->bytes > BURST_BYTES) c->bytes = BURST_BYTES;
    if ((c->bytes -= n) < 0) return Reject(id, "sending too much data");
    c->inLen += n;
    int at = 0;
    while (c->inLen - at >= 3) {
        int len = c->in[at] | c->in[at + 1] << 8;
        if (len < 1 || len > NET_MAX_MSG) return Drop(id);
        if (c->inLen - at < 2 + len) break;
        Message(id, c->in[at + 2], c->in + at + 3, len - 1, now);
        if (c->s == BAD_SOCK) return;
        at += 2 + len;
    }
    memmove(c->in, c->in + at, c->inLen - at);
    c->inLen -= at;
}

/* About once a second: pings, and players who never logged in or went silent. */
static void Tick(void) {
    static long long pingAt;
    long long now = Now();
    int ping = now - pingAt >= PING_MS;
    if (ping) pingAt = now;
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        Client *c = &sClients[i];
        if (c->s == BAD_SOCK) continue;
        if (!c->welcomed && now - c->since > HELLO_MS) {
            Reject(i, "took too long to join");
        } else if (c->welcomed && now - c->heard > SILENT_MS) {
            Log("%s (player %d) timed out", c->name, i);
            Drop(i);
        } else if (c->welcomed && ping) {
            Send(i, NM_PING, NULL, 0);
        }
    }
}

int main(int argc, char **argv) {
    int port = NET_PORT;
    const char *logPath = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) sMax = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--password") && i + 1 < argc) sPassword = argv[++i];
        else if (!strcmp(argv[i], "--chat") && i + 1 < argc) sChatMap = !strcmp(argv[++i], "map");
        else if (!strcmp(argv[i], "--admin") && i + 1 < argc) sAdmin = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) logPath = argv[++i];
        else if (!strcmp(argv[i], "--bans") && i + 1 < argc) sBanFile = argv[++i];
        else if (!strcmp(argv[i], "--accounts") && i + 1 < argc) sAccountFile = argv[++i];
        else {
            fprintf(stderr, "usage: hg_server [--port %d] [--max %d] [--password secret] [--chat all|map]\n"
                            "                 [--admin secret] [--log file] [--bans file] [--accounts file]\n", NET_PORT, NET_MAX_PLAYERS);
            return 2;
        }
    }
    if (sMax < 1 || sMax > NET_MAX_PLAYERS) sMax = NET_MAX_PLAYERS;
    if (logPath != NULL && (sLogFile = fopen(logPath, "a")) == NULL) {
        fprintf(stderr, "hg_server: cannot open %s\n", logPath);
        return 1;
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    LoadBans();
    LoadAccounts();
    for (int i = 0; i < NET_MAX_PLAYERS; i++) sClients[i].s = BAD_SOCK;
    Sock ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (ls == BAD_SOCK || bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 8) != 0) {
        fprintf(stderr, "hg_server: cannot listen on port %d\n", port);
        return 1;
    }
    Log("hg_server: listening on port %d, up to %d players%s%s, %d banned", port, sMax, *sPassword ? ", password set" : "",
        *sAdmin ? ", admin password set" : "", sBanCount);
    Log("player passwords %s: %d accounts in %s", sAccountsOn ? "on" : "off", sAccountCount, sAccountFile);
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(ls, &rd);
        Sock top = ls;
        for (int i = 0; i < NET_MAX_PLAYERS; i++) {
            if (sClients[i].s == BAD_SOCK) continue;
            FD_SET(sClients[i].s, &rd);
            if (sClients[i].s > top) top = sClients[i].s;
        }
        struct timeval tv = { 1, 0 };
        int ready = select((int)top + 1, &rd, NULL, NULL, &tv);
        Tick();
        if (ready <= 0) continue;
        if (FD_ISSET(ls, &rd)) {
            struct sockaddr_in from;
            socklen_t fromLen = sizeof from;
            Sock s = accept(ls, (struct sockaddr *)&from, &fromLen);
            int id = 0;
            while (id < NET_MAX_PLAYERS && sClients[id].s != BAD_SOCK) id++;
            if (s == BAD_SOCK) {
            } else if (id == NET_MAX_PLAYERS) {
                CloseSock(s);
            } else {
                int big = 1 << 18; /* link traffic: a player that stalls a moment must not be dropped */
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
                setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&big, sizeof big);
#ifdef _WIN32
                u_long nb = 1;
                ioctlsocket(s, FIONBIO, &nb);
#else
                fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#endif
                Client *c = &sClients[id];
                long long now = Now();
                memset(c, 0, sizeof *c);
                c->s = s;
                c->ip = from.sin_addr;
                c->since = c->heard = c->bytesAt = c->secAt = c->chatAt = c->inviteAt = now;
                c->bytes = BURST_BYTES;
                c->chat = CHAT_BURST;
                c->invite = 1;
            }
        }
        for (int i = 0; i < NET_MAX_PLAYERS; i++) {
            if (sClients[i].s != BAD_SOCK && FD_ISSET(sClients[i].s, &rd)) Receive(i);
        }
    }
}
