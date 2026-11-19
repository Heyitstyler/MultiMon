/* Text chat (multiplayer phase 4).
 *
 * The chat key (T by default; Options > Controls) opens a one-line text box while online. Typing goes
 * to it instead of the game; Enter sends, Esc closes, Backspace deletes. Messages go to the server
 * (NM_CHAT), which relays them to everyone or to the same map only (hg_server --chat map).
 * Lines show at the top of the top screen for 10 seconds; while typing, the last few stay visible.
 * pc_video.c reads the lines and draws them into the frame, so headless dumps show them too.
 *
 * HG_CHAT="frame:text;frame:text"  send a message at a frame (headless tests). HG_NET_LOG=1 logs chat.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <nitro.h>
#include "pc_net_proto.h"
#include "pc_platform.h"

#define HISTORY 8
#define SHOW_SECONDS 10
#define COLS PC_CHAT_COLS

static struct {
    char text[NET_NAME_LEN + NET_CHAT_LEN + 4];
    time_t at;
} sLines[HISTORY];
static int sCount;
static int sOpen;
static char sInput[NET_CHAT_LEN];

static void Add(const char *text) {
    memmove(sLines, sLines + 1, sizeof sLines - sizeof sLines[0]);
    snprintf(sLines[HISTORY - 1].text, sizeof sLines[0].text, "%s", text);
    sLines[HISTORY - 1].at = time(NULL);
    if (sCount < HISTORY) sCount++;
    if (getenv("HG_NET_LOG") != NULL) fprintf(stderr, "hg_pc: chat: %s\n", text);
}

/* A message from the network: `name` NULL for notices (joins, leaves). Only printable ASCII is kept
 * (the font has nothing else, and it keeps other players' bytes harmless). */
void pc_chat_receive(const char *name, const char *text) {
    char clean[NET_CHAT_LEN], line[sizeof sLines[0].text];
    size_t n = 0;
    for (const char *c = text; *c && n + 1 < sizeof clean; c++) clean[n++] = *c >= 32 && *c < 127 ? *c : '?';
    clean[n] = 0;
    if (name != NULL) snprintf(line, sizeof line, "%.16s: %s", name, clean);
    else snprintf(line, sizeof line, "* %s", clean);
    Add(line);
}

int pc_chat_is_open(void) { return sOpen; }

void pc_chat_open(void) {
    sOpen = 1;
    sInput[0] = 0;
}

static char sQuestion[80]; /* non-empty: the input line answers it (a password), it isn't chat */
static void (*sAnswer)(const char *);

void pc_chat_ask(const char *question, void (*answer)(const char *)) {
    snprintf(sQuestion, sizeof sQuestion, "%s", question);
    sAnswer = answer;
    pc_chat_open();
}

/* Ends a question; the answer may ask the next one. */
static void Answer(int enter) {
    char text[sizeof sInput];
    memcpy(text, sInput, sizeof text);
    memset(sInput, 0, sizeof sInput);
    sQuestion[0] = 0;
    sOpen = 0;
    sAnswer(enter ? text : NULL);
    memset(text, 0, sizeof text);
}

void pc_chat_close(void) {
    if (sQuestion[0]) return Answer(0);
    sOpen = 0;
}

void pc_chat_type(const char *utf8) {
    size_t n = strlen(sInput);
    for (const char *c = utf8; *c && n + 1 < sizeof sInput; c++) {
        if (*c >= 32 && *c < 127) sInput[n++] = *c; /* shortcut: ASCII only, like the font */
    }
    sInput[n] = 0;
}

void pc_chat_backspace(void) {
    size_t n = strlen(sInput);
    if (n > 0) sInput[n - 1] = 0;
}

static void Send(const char *text) {
    if (*text == 0) return;
    if (!strcmp(text, "/password")) return pc_net_change_password(); /* asks here, masked: never typed as chat */
    if (!pc_net_send_chat(text)) {
        Add("* Not connected");
        return;
    }
    if (*text != '/') pc_chat_receive(pc_cfg_name(), text); /* commands (/login password) aren't echoed */
}

void pc_chat_enter(void) {
    if (sQuestion[0]) return Answer(1);
    sOpen = 0; /* first: Send may open a question (/password) */
    Send(sInput);
}

/* Lines to draw, oldest first, wrapped to the box width; returns how many (at most `max`). The input
 * line, if typing, is the last one. */
int pc_chat_lines(char out[][COLS + 1], int max) {
    char wrapped[HISTORY * 4][COLS + 1];
    int n = 0;
    time_t now = time(NULL);
    for (int i = HISTORY - sCount; i < HISTORY; i++) {
        if (!sOpen && now - sLines[i].at >= SHOW_SECONDS) continue;
        for (const char *p = sLines[i].text; *p && n < HISTORY * 4;) {
            int take = (int)strnlen(p, COLS);
            if (p[take] != 0) { /* break at the last space that fits, if any */
                int sp = take;
                while (sp > 0 && p[sp] != ' ') sp--;
                if (sp > 0) take = sp;
            }
            snprintf(wrapped[n++], COLS + 1, "%.*s", take, p);
            p += take;
            while (*p == ' ') p++;
        }
    }
    int asked = 0;
    for (const char *q = sQuestion; *q; q += strcspn(q, "\n"), q += *q == '\n') asked++;
    int keep = sOpen ? max - 1 - asked : max, first = n > keep ? n - keep : 0, k = 0;
    for (int i = first; i < n; i++) memcpy(out[k++], wrapped[i], COLS + 1);
    for (const char *q = sQuestion; *q; q += *q == '\n') {
        int len = (int)strcspn(q, "\n");
        snprintf(out[k++], COLS + 1, "* %.*s", len, q);
        q += len;
    }
    if (sOpen) { /* the tail of the input, so the cursor end is always visible */
        size_t len = strlen(sInput);
        char stars[sizeof sInput];
        memset(stars, '*', len);
        stars[len] = 0;
        const char *show = sQuestion[0] ? stars : sInput;
        snprintf(out[k++], COLS + 1, "> %s_", len > COLS - 3 ? show + len - (COLS - 3) : show);
    }
    return k;
}

/* Each frame end: HG_CHAT test messages. */
void pc_chat_frame(unsigned long frame) {
    const char *list = getenv("HG_CHAT");
    if (list == NULL) return;
    for (const char *p = list; *p;) {
        char *end;
        unsigned long f = strtoul(p, &end, 10);
        if (end == p || *end != ':') break;
        size_t len = strcspn(end + 1, ";");
        if (f == frame) {
            char text[NET_CHAT_LEN];
            snprintf(text, sizeof text, "%.*s", (int)len, end + 1);
            if (sQuestion[0]) { /* answers an open question, as if typed */
                snprintf(sInput, sizeof sInput, "%s", text);
                pc_chat_enter();
            } else {
                Send(text);
            }
        }
        p = end + 1 + len;
        if (*p == ';') p++;
    }
}
