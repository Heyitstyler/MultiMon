/* Frame output: SDL3 window (when built with PC_HAVE_SDL) or headless with PPM frame dumps.
 *
 * Window: both screens stacked (top over bottom), 256x384 scaled to the window, nearest filter.
 * Keys (defaults; rebind in the options menu): arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R,
 * Enter = Start, Backspace = Select, hold Tab = fast forward, Esc = options menu (pc_menu.c).
 * Left mouse on the bottom screen = touch.
 *
 * Env: HG_HEADLESS=1 forces no window (and no 60 Hz pacing; HG_PACE=1 paces it anyway).
 *      HG_DUMP=60,300 writes frame_00060.ppm ... (256x384) to HG_DUMP_DIR (default ".").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nitro.h>
#include "pc_platform.h"
#ifdef PC_HAVE_SDL
#include <SDL3/SDL.h>
#endif

#define W 256
#define H 192

static u32 sFrame[W * H * 2]; /* top screen, then bottom screen */
static unsigned long sFrameNo;
static int sWindowed;

#ifdef PC_HAVE_SDL
static SDL_Window *sWin;
static SDL_Renderer *sRen;
static SDL_Texture *sTex;

static int SdlOpen(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "hg_pc: no window (%s); running headless\n", SDL_GetError());
        return 0;
    }
    if (!SDL_CreateWindowAndRenderer("HeartGold (hg_pc)", W * 2, H * 4, SDL_WINDOW_RESIZABLE, &sWin, &sRen)) {
        fprintf(stderr, "hg_pc: window: %s\n", SDL_GetError());
        return 0;
    }
    SDL_SetRenderLogicalPresentation(sRen, W, H * 2, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    sTex = SDL_CreateTexture(sRen, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, W, H * 2);
    SDL_SetTextureScaleMode(sTex, SDL_SCALEMODE_NEAREST);
    pc_menu_window_ready(sWin);
    return 1;
}

static void SdlPresent(void) {
    SDL_UpdateTexture(sTex, NULL, sFrame, W * 4);
    SDL_RenderClear(sRen);
    SDL_RenderTexture(sRen, sTex, NULL, NULL);
    SDL_RenderPresent(sRen);
}

static void SdlPoll(void) {
    SDL_Event ev;
    if (pc_chat_is_open() && !SDL_TextInputActive(sWin)) SDL_StartTextInput(sWin); /* opened by the game (a password question) */
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) exit(0); /* saves are written through, nothing to flush */
        if (pc_chat_is_open()) { /* typing a chat line: keys go to it, not the game or the menu */
            if (ev.type == SDL_EVENT_TEXT_INPUT) pc_chat_type(ev.text.text);
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            SDL_Scancode k = ev.key.scancode;
            if (k == SDL_SCANCODE_BACKSPACE) pc_chat_backspace();
            if (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER) pc_chat_enter();
            if (k == SDL_SCANCODE_ESCAPE) pc_chat_close();
            if (!pc_chat_is_open()) SDL_StopTextInput(sWin);
            continue;
        }
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_ESCAPE && !ev.key.repeat) {
            pc_menu_run(sWin, sRen, sTex);
        }
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && (int)ev.key.scancode == pc_cfg_key(PC_ACT_CHAT) && pc_net_online()) {
            pc_chat_open();
            SDL_StartTextInput(sWin); /* the chat key's own character came before this, so it is not typed */
        }
    }
    if (pc_chat_is_open()) { /* the game sees no keys while typing */
        pc_menu_fast_forward(0);
        pc_input_set(0, FALSE, 0, 0);
        return;
    }
    static const u16 kPad[PC_ACT_FF] = { /* indexed by PC_ACT_* */
        PAD_BUTTON_A, PAD_BUTTON_B, PAD_BUTTON_X, PAD_BUTTON_Y, PAD_BUTTON_L, PAD_BUTTON_R,
        PAD_BUTTON_START, PAD_BUTTON_SELECT, PAD_KEY_UP, PAD_KEY_DOWN, PAD_KEY_LEFT, PAD_KEY_RIGHT,
    };
    const bool *ks = SDL_GetKeyboardState(NULL);
    u16 held = 0;
    for (int a = 0; a < PC_ACT_FF; a++) {
        if (ks[pc_cfg_key(a)]) held |= kPad[a];
    }
    pc_menu_fast_forward(ks[pc_cfg_key(PC_ACT_FF)]);
    float mx, my, lx, ly;
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
    SDL_RenderCoordinatesFromWindow(sRen, mx, my, &lx, &ly);
    BOOL touching = (buttons & SDL_BUTTON_LMASK) && lx >= 0 && lx < W && ly >= H && ly < H * 2;
    pc_input_set(held, touching, touching ? (u16)lx : 0, touching ? (u16)(ly - H) : 0);
}
#endif

int pc_video_windowed(void) { return sWindowed; }

void pc_video_init(void) {
#ifdef PC_HAVE_SDL
    const char *headless = getenv("HG_HEADLESS");
    if (headless == NULL || *headless == '0') sWindowed = SdlOpen();
#endif
}

/* Headless test input: HG_INPUT="frame:BUTTON,..." holds BUTTON for 6 frames from that frame.
   BUTTON is A B X Y L R START SELECT UP DOWN LEFT RIGHT, or Tx.y to touch the bottom screen. */
/* HG_INPUT_FILE=path: the same list, re-read from the file every 30 frames, so a running test can
 * be steered by appending entries for frames still ahead (with HG_DUMP_EVERY to look first). */
static const char *ScriptList(void) {
    const char *path = getenv("HG_INPUT_FILE");
    if (path == NULL) return getenv("HG_INPUT");
    static char *buf;
    if (buf == NULL || sFrameNo % 30 == 0) {
        FILE *f = fopen(path, "rb");
        if (f != NULL) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *nb = malloc((size_t)n + 1);
            nb[fread(nb, 1, (size_t)n, f)] = 0;
            fclose(f);
            for (char *c = nb; *c; c++) if (*c == '\n' || *c == '\r' || *c == ' ') *c = ',';
            free(buf);
            buf = nb;
        }
    }
    return buf;
}

static void ScriptPoll(void) {
    const char *list = ScriptList();
    if (list == NULL) return;
    static const struct { const char *name; u16 key; } names[] = {
        { "A", PAD_BUTTON_A }, { "B", PAD_BUTTON_B }, { "X", PAD_BUTTON_X }, { "Y", PAD_BUTTON_Y },
        { "L", PAD_BUTTON_L }, { "R", PAD_BUTTON_R }, { "START", PAD_BUTTON_START }, { "SELECT", PAD_BUTTON_SELECT },
        { "UP", PAD_KEY_UP }, { "DOWN", PAD_KEY_DOWN }, { "LEFT", PAD_KEY_LEFT }, { "RIGHT", PAD_KEY_RIGHT },
    };
    u16 held = 0;
    BOOL touching = FALSE;
    unsigned tx = 0, ty = 0;
    for (const char *p = list; *p;) {
        char *end;
        unsigned long n = strtoul(p, &end, 10);
        if (end == p || *end != ':') break;
        const char *what = end + 1;
        size_t len = strcspn(what, ",");
        if (sFrameNo >= n && sFrameNo < n + 6) {
            if (what[0] == 'T' && sscanf(what + 1, "%u.%u", &tx, &ty) == 2) touching = TRUE;
            for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
                if (strlen(names[i].name) == len && strncmp(names[i].name, what, len) == 0) held |= names[i].key;
            }
        }
        p = what + len;
        if (*p == ',') p++;
    }
    pc_input_set(held, touching, (u16)tx, (u16)ty);
}

void pc_video_poll(void) {
#ifdef PC_HAVE_SDL
    if (sWindowed) {
        SdlPoll();
        if (ScriptList() != NULL) ScriptPoll(); /* a scripted test in a window (no window: SDL_VIDEO_DRIVER=offscreen) */
        return;
    }
#endif
    ScriptPoll();
}

static void WritePpm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H * 2);
    for (int i = 0; i < W * H * 2; i++) {
        u8 rgb[3] = { (u8)(sFrame[i] >> 16), (u8)(sFrame[i] >> 8), (u8)sFrame[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void Dump(void) {
    const char *every = getenv("HG_DUMP_EVERY"); /* live view: latest.ppm + latest.txt (frame number) */
    if (every != NULL && atoi(every) > 0 && sFrameNo % (unsigned long)atoi(every) == 0) {
        const char *dir = getenv("HG_DUMP_DIR");
        char path[512];
        snprintf(path, sizeof path, "%s/latest.tmp", dir ? dir : ".");
        WritePpm(path);
        char final[512];
        snprintf(final, sizeof final, "%s/latest.ppm", dir ? dir : ".");
#ifdef _WIN32
        remove(final); /* Windows rename() does not replace */
#endif
        rename(path, final);
        snprintf(path, sizeof path, "%s/latest.txt", dir ? dir : ".");
        FILE *t = fopen(path, "w");
        if (t != NULL) { fprintf(t, "%lu\n", sFrameNo); fclose(t); }
    }
    const char *list = getenv("HG_DUMP");
    if (list == NULL) return;
    for (const char *p = list; *p;) {
        char *end;
        unsigned long n = strtoul(p, &end, 10);
        if (end == p) break;
        if (n == sFrameNo) {
            const char *dir = getenv("HG_DUMP_DIR");
            char path[512];
            snprintf(path, sizeof path, "%s/frame_%05lu.ppm", dir ? dir : ".", n);
            FILE *f = fopen(path, "wb");
            if (f == NULL) { perror(path); return; }
            fprintf(f, "P6\n%d %d\n255\n", W, H * 2);
            for (int i = 0; i < W * H * 2; i++) {
                u8 rgb[3] = { (u8)(sFrame[i] >> 16), (u8)(sFrame[i] >> 8), (u8)sFrame[i] };
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
            fprintf(stderr, "hg_pc: wrote %s\n", path);
            for (int e = 0; e < 2; e++) { /* display registers, for diagnosing a dumped frame */
                const u16 *io = (const u16 *)(e ? 0x04001000 : 0x04000000);
                fprintf(stderr, "  %c: DISPCNT=%08x BGCNT=%04x %04x %04x %04x BLD=%04x %04x %04x WIN=%04x %04x %04x %04x %04x %04x MB=%04x\n",
                        'A' + e, *(const u32 *)io, io[4], io[5], io[6], io[7], io[0x28], io[0x29], io[0x2A],
                        io[0x20], io[0x21], io[0x22], io[0x23], io[0x24], io[0x25], io[0x36]);
            }
        }
        p = *end ? end + 1 : end;
    }
}

#ifdef PC_HAVE_SDL
/* Keep 60 Hz times the game speed against SDL's monotonic clock; if far behind (debugger, slow
 * frame, the paused menu), resync. Speed 0 = no pacing. */
static void Pace(void) {
    static Uint64 next;
    float speed = pc_cfg_speed();
    if (speed == 0) { next = 0; return; }
    Uint64 now = SDL_GetTicksNS();
    next = next == 0 ? now : next + (Uint64)(16666667 / speed);
    if (now > next + 50000000) { next = now; return; }
    if (next > now) SDL_DelayPrecise(next - now);
}
#endif

/* A text box over the bottom of the top screen (pc_challenge.c prompts), drawn into the frame
 * itself so headless dumps show it too: SDL's 8x8 debug font through a software renderer. */
static char sOverlay[256];

void pc_video_overlay(const char *text) {
    snprintf(sOverlay, sizeof sOverlay, "%s", text != NULL ? text : "");
}

static void DrawOverlay(void) {
#ifdef PC_HAVE_SDL
    static SDL_Renderer *r;
    static int failed;
    char chat[8][PC_CHAT_COLS + 1];
    int chatLines = pc_chat_lines(chat, 8);
    if ((sOverlay[0] == 0 && chatLines == 0) || failed) return;
    if (r == NULL) {
        SDL_Surface *s = SDL_CreateSurfaceFrom(W, H, SDL_PIXELFORMAT_XRGB8888, sFrame, W * 4);
        r = s != NULL ? SDL_CreateSoftwareRenderer(s) : NULL;
        if (r == NULL) {
            fprintf(stderr, "hg_pc: no text overlay (%s)\n", SDL_GetError());
            failed = 1;
            return;
        }
    }
    if (chatLines > 0) { /* chat: dark see-through box at the top */
        SDL_FRect cb = { 4, 4, W - 8, (float)(chatLines * 10 + 6) };
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 170);
        SDL_RenderFillRect(r, &cb);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        for (int i = 0; i < chatLines; i++) {
            BOOL input = pc_chat_is_open() && i == chatLines - 1, notice = chat[i][0] == '*';
            SDL_SetRenderDrawColor(r, input ? 255 : notice ? 170 : 255, input ? 255 : notice ? 200 : 255, input ? 120 : 255, 255);
            SDL_RenderDebugText(r, cb.x + 4, cb.y + 4 + i * 10, chat[i]);
        }
    }
    if (sOverlay[0] == 0) {
        SDL_FlushRenderer(r);
        return;
    }
    int lines = 1;
    for (const char *c = sOverlay; *c; c++) lines += *c == '\n';
    SDL_FRect box = { 4, (float)(H - 4 - (lines * 10 + 10)), W - 8, (float)(lines * 10 + 10) };
    SDL_SetRenderDrawColor(r, 248, 248, 248, 255);
    SDL_RenderFillRect(r, &box);
    SDL_SetRenderDrawColor(r, 64, 64, 80, 255);
    SDL_RenderRect(r, &box);
    char line[64];
    const char *p = sOverlay;
    for (int i = 0; i < lines; i++) {
        size_t n = strcspn(p, "\n");
        snprintf(line, sizeof line, "%.*s", (int)n, p);
        SDL_RenderDebugText(r, box.x + 8, box.y + 6 + i * 10, line);
        p += n + (p[n] == '\n');
    }
    SDL_FlushRenderer(r);
#endif
}

/* Called right after the VBlank handler: the frame the DS would scan out next. */
void pc_video_frame(void) {
    pc_gpu2d_render(sFrame, sFrame + W * H);
    DrawOverlay();
    sFrameNo++;
    Dump();
#ifdef PC_HAVE_SDL
    if (sWindowed) {
        SdlPresent();
        const char *shot = getenv("HG_MENU_SHOT");
        if (shot != NULL && sFrameNo == 900) pc_menu_shot(sRen, sTex, shot);
        Pace();
    } else if (getenv("HG_PACE") != NULL) {
        Pace(); /* headless at 60 Hz: linked copies keep step with each other */
    }
#endif
}
