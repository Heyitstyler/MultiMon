/* Host options menu (Esc in the window), key bindings and their settings file.
 *
 * The menu pauses the game and draws over the last frame with SDL's built-in 8x8 debug font.
 * Up/Down pick a line, Left/Right change it, Enter (or the A key) activates, Esc (or the B key) goes back.
 *   Volume, game speed and fast-forward speed (0.25x-4x or Max; sound keeps normal speed),
 *   fullscreen, controls (rebind every button and the fast-forward key), multiplayer (server
 *   address, name, password, connect; who is online), load another save (restarts the game with
 *   it), save a copy of the loaded save, change ROM (restarts), quit.
 * With no ROM on the command line, in $HG_ROM or remembered as rom=, a file picker asks for one
 * at startup; the ROM in use is remembered.
 * Holding the fast-forward key (Tab by default) runs the game at the fast-forward speed. Esc always
 * opens the menu and can't be bound.
 *
 * Settings live in hg_pc.cfg next to the executable, as key=value lines:
 *   volume=0..100, speed= and ffspeed=0.25..4 in steps of 0.25 (0 = max), fullscreen=0|1,
 *   key_A=X ... (SDL key names), rom=<path of the .nds>, save=<path of the save to load>,
 *   server=host[:port], name=, password=, online=0|1 (connect at startup).
 * $HG_SAVE, when set, still wins over save= (the headless tests use it); likewise $HG_SERVER,
 * $HG_NAME and $HG_PASSWORD over server=, name= and password=.
 * HG_MENU_SHOT=file.bmp writes the menu as drawn at frame 900, and the controls page to
 * file.bmp.controls.bmp and the multiplayer page to file.bmp.multi.bmp (with SDL_VIDEODRIVER=offscreen,
 * no window).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nitro.h>
#include "pc_net_proto.h"
#include "pc_platform.h"
#ifdef PC_HAVE_SDL
#include <SDL3/SDL.h>
#endif
#ifdef _WIN32
#include <windows.h>
#undef far
#undef near
#else
#include <unistd.h>
#endif

char **pc_argv;

static int sVolume = 100;   /* percent */
static int sSpeedQ = 4;     /* game speed in quarters: 1-16 = 0.25x-4x, 0 = as fast as possible */
static int sFFQ = 0;        /* fast-forward speed, same units */
static int sFastForward;    /* fast-forward key held */
static int sFullscreen;
static char sSavePath[1024];
static char sRomPath[1024];
static char sServer[256], sName[NET_NAME_LEN + 1], sPassword[32];
static int sOnline;

int pc_cfg_volume(void) { return sVolume; }
const char *pc_cfg_rom(void) { return sRomPath[0] ? sRomPath : NULL; }
float pc_cfg_speed(void) { return (sFastForward ? sFFQ : sSpeedQ) / 4.0f; }
const char *pc_cfg_save_path(void) { return sSavePath[0] ? sSavePath : NULL; }
void pc_menu_fast_forward(int held) { sFastForward = held; }

const char *pc_cfg_server(void) { return getenv("HG_SERVER") ? getenv("HG_SERVER") : sServer; }
const char *pc_cfg_name(void) { return getenv("HG_NAME") ? getenv("HG_NAME") : sName[0] ? sName : "Player"; }
const char *pc_cfg_password(void) { return getenv("HG_PASSWORD") ? getenv("HG_PASSWORD") : sPassword; }
int pc_cfg_online(void) { return sOnline; }

#ifdef PC_HAVE_SDL
/* ---- key bindings, indexed by PC_ACT_* */
static const struct { const char *label, *cfg; SDL_Scancode def; } kActs[PC_ACT_COUNT] = {
    { "A", "A", SDL_SCANCODE_X },              { "B", "B", SDL_SCANCODE_Z },
    { "X", "X", SDL_SCANCODE_S },              { "Y", "Y", SDL_SCANCODE_A },
    { "L", "L", SDL_SCANCODE_Q },              { "R", "R", SDL_SCANCODE_W },
    { "Start", "Start", SDL_SCANCODE_RETURN }, { "Select", "Select", SDL_SCANCODE_BACKSPACE },
    { "Up", "Up", SDL_SCANCODE_UP },           { "Down", "Down", SDL_SCANCODE_DOWN },
    { "Left", "Left", SDL_SCANCODE_LEFT },     { "Right", "Right", SDL_SCANCODE_RIGHT },
    { "Fast forward", "FastForward", SDL_SCANCODE_TAB },
    { "Chat", "Chat", SDL_SCANCODE_T },
};
static SDL_Scancode sKey[PC_ACT_COUNT];

int pc_cfg_key(int action) { return sKey[action]; }

static void DefaultKeys(void) {
    for (int a = 0; a < PC_ACT_COUNT; a++) sKey[a] = kActs[a].def;
}

static char sCfgPath[1100];

static void CfgWrite(void) {
    FILE *f = fopen(sCfgPath, "w");
    if (f == NULL) return;
    fprintf(f, "volume=%d\nspeed=%.2f\nffspeed=%.2f\nfullscreen=%d\n", sVolume, sSpeedQ / 4.0, sFFQ / 4.0, sFullscreen);
    for (int a = 0; a < PC_ACT_COUNT; a++) fprintf(f, "key_%s=%s\n", kActs[a].cfg, SDL_GetScancodeName(sKey[a]));
    if (sRomPath[0]) fprintf(f, "rom=%s\n", sRomPath);
    if (sSavePath[0]) fprintf(f, "save=%s\n", sSavePath);
    fprintf(f, "server=%s\nname=%s\npassword=%s\nonline=%d\n", sServer, sName, sPassword, sOnline);
    fclose(f);
}

static int Quarters(const char *s, int dflt) {
    int q = (int)(atof(s) * 4 + 0.5);
    return q < 0 || q > 16 ? dflt : q;
}

void pc_cfg_load(void) {
    DefaultKeys();
    const char *base = SDL_GetBasePath(); /* the executable's folder, with a trailing separator */
    snprintf(sCfgPath, sizeof sCfgPath, "%shg_pc.cfg", base ? base : "");
    FILE *f = fopen(sCfgPath, "r");
    if (f == NULL) return;
    char line[1100];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "volume=", 7)) sVolume = atoi(line + 7);
        else if (!strncmp(line, "speed=", 6)) sSpeedQ = Quarters(line + 6, 4);
        else if (!strncmp(line, "ffspeed=", 8)) sFFQ = Quarters(line + 8, 0);
        else if (!strncmp(line, "fullscreen=", 11)) sFullscreen = atoi(line + 11);
        else if (!strncmp(line, "save=", 5)) snprintf(sSavePath, sizeof sSavePath, "%s", line + 5);
        else if (!strncmp(line, "rom=", 4)) snprintf(sRomPath, sizeof sRomPath, "%s", line + 4);
        else if (!strncmp(line, "server=", 7)) snprintf(sServer, sizeof sServer, "%s", line + 7);
        else if (!strncmp(line, "name=", 5)) snprintf(sName, sizeof sName, "%s", line + 5);
        else if (!strncmp(line, "password=", 9)) snprintf(sPassword, sizeof sPassword, "%s", line + 9);
        else if (!strncmp(line, "online=", 7)) sOnline = atoi(line + 7);
        else if (!strncmp(line, "key_", 4)) {
            char *eq = strchr(line, '=');
            if (eq == NULL) continue;
            *eq = 0;
            SDL_Scancode sc = SDL_GetScancodeFromName(eq + 1);
            for (int a = 0; a < PC_ACT_COUNT; a++) {
                if (!strcmp(line + 4, kActs[a].cfg) && sc != SDL_SCANCODE_UNKNOWN && sc != SDL_SCANCODE_ESCAPE) sKey[a] = sc;
            }
        }
    }
    fclose(f);
    if (sVolume < 0 || sVolume > 100) sVolume = 100;
}

/* Remember the ROM in use, so later starts (and the menu's restarts) need no argument.
 * Headless runs (tests) leave the settings file alone. */
void pc_cfg_set_rom(const char *path) {
    const char *headless = getenv("HG_HEADLESS");
    char full[1024];
#ifdef _WIN32
    if (_fullpath(full, path, sizeof full) == NULL) return;
#else
    if (realpath(path, full) == NULL) return;
#endif
    if ((headless != NULL && *headless != '0') || !strcmp(full, sRomPath)) return;
    snprintf(sRomPath, sizeof sRomPath, "%s", full);
    CfgWrite();
}

/* ---- file dialogs: SDL may call back on another thread, so hand the path over through an atomic */
enum { PICK_NONE, PICK_LOAD, PICK_COPY, PICK_ROM };
static SDL_AtomicInt sPicked; /* PICK_* once a path is in sPickedPath; -1 = cancelled (just closes the wait) */
static int sPicking;           /* which dialog is open */
static char sPickedPath[1024];
static char sStatus[64];

static void SDLCALL OnPick(void *userdata, const char *const *files, int filter) {
    (void)filter;
    if (files != NULL && files[0] != NULL) {
        snprintf(sPickedPath, sizeof sPickedPath, "%s", files[0]);
        SDL_SetAtomicInt(&sPicked, (int)(intptr_t)userdata);
    } else {
        SDL_SetAtomicInt(&sPicked, -1);
    }
}

static const SDL_DialogFileFilter kFilters[] = { { "DS save files", "sav;dsv" }, { "All files", "*" } };
static const SDL_DialogFileFilter kRomFilters[] = { { "DS ROM", "nds" }, { "All files", "*" } };

static void ShowRomDialog(SDL_Window *win) {
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, (void *)kRomFilters);
    SDL_SetNumberProperty(p, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
    SDL_SetPointerProperty(p, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, win);
    SDL_SetStringProperty(p, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Select your Pokemon HeartGold (US) ROM");
    if (sRomPath[0]) SDL_SetStringProperty(p, SDL_PROP_FILE_DIALOG_LOCATION_STRING, sRomPath);
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, OnPick, (void *)(intptr_t)PICK_ROM, p);
    SDL_DestroyProperties(p);
}

/* No ROM given or remembered: ask for one before the game starts. Returns its path. */
const char *pc_menu_pick_rom(void) {
    const char *headless = getenv("HG_HEADLESS");
    if ((headless != NULL && *headless != '0') || !SDL_Init(SDL_INIT_VIDEO)) return NULL;
    for (;;) {
        SDL_SetAtomicInt(&sPicked, PICK_NONE);
        ShowRomDialog(NULL);
        while (SDL_GetAtomicInt(&sPicked) == PICK_NONE) {
            pc_watchdog_kick();
            SDL_PumpEvents();
            SDL_Delay(10);
        }
        if (SDL_GetAtomicInt(&sPicked) != PICK_ROM) return NULL; /* cancelled */
        if (pc_rom_check(sPickedPath)) return sPickedPath;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "hg_pc",
                                 "That file is not Pokemon HeartGold (US) (game code IPKE). Pick the .nds of your own cartridge.", NULL);
    }
}

/* Start the game over: the game reads its ROM and save only at boot. No arguments, so the
 * settings file decides which ROM and save. */
static void Restart(void) {
    CfgWrite();
#ifdef _WIN32
    SetEnvironmentVariableA("HG_SAVE", NULL);
    SetEnvironmentVariableA("HG_ROM", NULL);
    _putenv("HG_SAVE=");
    _putenv("HG_ROM=");
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 3];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    _snwprintf(cmd, MAX_PATH + 3, L"\"%ls\"", exe);
    STARTUPINFOW si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    if (CreateProcessW(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        exit(0);
    }
#else
    unsetenv("HG_SAVE");
    unsetenv("HG_ROM");
    char *argv[] = { pc_argv[0], NULL };
    execv("/proc/self/exe", argv);
#endif
    snprintf(sStatus, sizeof sStatus, "Restart failed");
}

/* ---- drawing */
enum { ITEM_RESUME, ITEM_VOLUME, ITEM_SPEED, ITEM_FF, ITEM_FULLSCREEN, ITEM_CONTROLS, ITEM_MULTI, ITEM_LOAD, ITEM_COPY,
       ITEM_ROM, ITEM_QUIT, ITEM_COUNT };
enum { MP_SERVER, MP_NAME, MP_PASSWORD, MP_CONNECT, MP_BACK, MP_COUNT }; /* multiplayer page */
enum { CTRL_RESET = PC_ACT_COUNT, CTRL_BACK, CTRL_COUNT }; /* controls page: one row per action, then these */

static const char *Basename(const char *p) {
    const char *b = p;
    for (; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

static void SpeedText(char *out, size_t n, int q) {
    if (q == 0) snprintf(out, n, "Max");
    else snprintf(out, n, "%.2fx", q / 4.0);
}

static void Row(SDL_Renderer *ren, float y, int selected, const char *text) {
    if (selected) {
        SDL_SetRenderDrawColor(ren, 255, 210, 0, 255);
        SDL_RenderDebugText(ren, 16, y, ">");
    } else {
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    }
    SDL_RenderDebugText(ren, 32, y, text);
}

static void Begin(SDL_Renderer *ren, SDL_Texture *frame, float top, float height, const char *title) {
    SDL_RenderClear(ren);
    SDL_RenderTexture(ren, frame, NULL, NULL);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 200);
    SDL_FRect panel = { 8, top, 240, height };
    SDL_RenderFillRect(ren, &panel);
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_RenderDebugText(ren, 16, top + 8, title);
}

static void End(SDL_Renderer *ren, const char *shot) {
    if (shot != NULL) {
        SDL_Surface *s = SDL_RenderReadPixels(ren, NULL);
        if (s == NULL || !SDL_SaveBMP(s, shot)) fprintf(stderr, "hg_pc: menu shot: %s\n", SDL_GetError());
        SDL_DestroySurface(s);
    }
    SDL_RenderPresent(ren);
}

static void DrawMain(SDL_Renderer *ren, SDL_Texture *frame, int sel, const char *shot) {
    Begin(ren, frame, 88, 272, "OPTIONS");
    char line[64], speed[8];
    for (int i = 0; i < ITEM_COUNT; i++) {
        switch (i) {
        case ITEM_RESUME: snprintf(line, sizeof line, "Resume"); break;
        case ITEM_VOLUME: snprintf(line, sizeof line, "%-13s< %3d%% >", "Volume", sVolume); break;
        case ITEM_SPEED: SpeedText(speed, sizeof speed, sSpeedQ); snprintf(line, sizeof line, "%-13s< %-5s >", "Game speed", speed); break;
        case ITEM_FF: SpeedText(speed, sizeof speed, sFFQ); snprintf(line, sizeof line, "%-13s< %-5s >", "Fast forward", speed); break;
        case ITEM_FULLSCREEN: snprintf(line, sizeof line, "%-13s< %-5s >", "Fullscreen", sFullscreen ? "On" : "Off"); break;
        case ITEM_CONTROLS: snprintf(line, sizeof line, "Controls..."); break;
        case ITEM_MULTI: snprintf(line, sizeof line, "Multiplayer..."); break;
        case ITEM_LOAD: snprintf(line, sizeof line, "Load another save..."); break;
        case ITEM_COPY: snprintf(line, sizeof line, "Save a copy..."); break;
        case ITEM_ROM: snprintf(line, sizeof line, "Change ROM..."); break;
        case ITEM_QUIT: snprintf(line, sizeof line, "Quit"); break;
        }
        Row(ren, 116 + i * 16, i == sel, line);
    }
    SDL_SetRenderDrawColor(ren, 160, 160, 160, 255);
    const char *save = pc_backup_path();
    snprintf(line, sizeof line, "Save: %.22s", save ? Basename(save) : "-");
    SDL_RenderDebugText(ren, 16, 300, line);
    SDL_RenderDebugText(ren, 16, 316, sPicking ? "Waiting for the file dialog..." : sStatus);
    snprintf(line, sizeof line, "Hold %.12s: fast forward", SDL_GetScancodeName(sKey[PC_ACT_FF]));
    SDL_RenderDebugText(ren, 16, 332, line);
    End(ren, shot);
}

static void DrawControls(SDL_Renderer *ren, SDL_Texture *frame, int sel, int binding, const char *shot) {
    Begin(ren, frame, 16, 352, "CONTROLS");
    char line[64];
    for (int i = 0; i < CTRL_COUNT; i++) {
        if (i < PC_ACT_COUNT) {
            const char *key = i == binding ? "press a key..." : SDL_GetScancodeName(sKey[i]);
            snprintf(line, sizeof line, "%-13s%.15s", kActs[i].label, *key ? key : "(none)");
        } else {
            snprintf(line, sizeof line, i == CTRL_RESET ? "Reset to defaults" : "Back");
        }
        Row(ren, 40 + i * 16, i == sel, line);
    }
    SDL_SetRenderDrawColor(ren, 160, 160, 160, 255);
    SDL_RenderDebugText(ren, 16, 300, binding >= 0 ? "Esc cancels" : "Enter: change the key");
    SDL_RenderDebugText(ren, 16, 316, "A key already in use swaps");
    SDL_RenderDebugText(ren, 16, 332, "with the other action.");
    End(ren, shot);
}

/* Multiplayer page; `edit` is the row being typed into (-1: none), `text` its text so far. */
static void DrawMulti(SDL_Renderer *ren, SDL_Texture *frame, int sel, int edit, const char *text, const char *shot) {
    Begin(ren, frame, 16, 352, "MULTIPLAYER");
    char line[64], stars[33];
    for (int i = 0; i < MP_COUNT; i++) {
        const char *v = "";
        if (i == MP_SERVER) v = *pc_cfg_server() ? pc_cfg_server() : "(not set)";
        if (i == MP_NAME) v = pc_cfg_name();
        if (i == MP_PASSWORD) {
            size_t n = strlen(sPassword);
            memset(stars, '*', n);
            stars[n] = 0;
            v = n ? stars : "(none)";
        }
        if (i == edit) v = text;
        const char *cursor = i == edit ? "_" : "";
        switch (i) {
        case MP_SERVER: snprintf(line, sizeof line, "Server   %.19s%s", v, cursor); break;
        case MP_NAME: snprintf(line, sizeof line, "Name     %.16s%s", v, cursor); break;
        case MP_PASSWORD: snprintf(line, sizeof line, "Password %.19s%s", v, cursor); break;
        case MP_CONNECT: snprintf(line, sizeof line, pc_net_wanted() ? "Disconnect" : "Connect"); break;
        case MP_BACK: snprintf(line, sizeof line, "Back"); break;
        }
        Row(ren, 40 + i * 16, i == sel, line);
    }
    SDL_SetRenderDrawColor(ren, 160, 160, 160, 255);
    snprintf(line, sizeof line, "%.28s", pc_net_status());
    SDL_RenderDebugText(ren, 16, 136, line);
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_RenderDebugText(ren, 16, 160, "Players online:");
    int y = 176;
    for (int id = 0; id < NET_MAX_PLAYERS; id++) {
        const PcRemote *r = pc_net_remote(id);
        if (r == NULL) continue;
        if (r->hasPos) snprintf(line, sizeof line, "  %-16.16s map %d", r->name, r->map);
        else snprintf(line, sizeof line, "  %-16.16s", r->name);
        SDL_RenderDebugText(ren, 16, (float)y, line);
        y += 12;
    }
    if (y == 176) SDL_RenderDebugText(ren, 16, 176, pc_net_wanted() ? "  nobody else yet" : "  -");
    SDL_SetRenderDrawColor(ren, 160, 160, 160, 255);
    SDL_RenderDebugText(ren, 16, 348, edit >= 0 ? "Enter: keep  Esc: cancel" : "Enter: edit / connect");
    End(ren, shot);
}

/* ---- input */
static void StepSpeed(int *q, int dir) {
    /* 0.25x ... 4x, then Max at the top */
    if (*q == 0) *q = dir < 0 ? 16 : 0;
    else *q = *q + dir > 16 ? 0 : *q + dir < 1 ? 1 : *q + dir;
}

static void Change(SDL_Window *win, int item, int dir) {
    switch (item) {
    case ITEM_VOLUME:
        sVolume += dir * 5;
        sVolume = sVolume < 0 ? 0 : sVolume > 100 ? 100 : sVolume;
        pc_snd_set_volume(sVolume);
        break;
    case ITEM_SPEED: StepSpeed(&sSpeedQ, dir); break;
    case ITEM_FF: StepSpeed(&sFFQ, dir); break;
    case ITEM_FULLSCREEN:
        sFullscreen = !sFullscreen;
        SDL_SetWindowFullscreen(win, sFullscreen);
        break;
    default:
        return;
    }
    CfgWrite();
}

/* Give `action` key `sc`; an action that had that key gets this one's old key instead. */
static void Bind(int action, SDL_Scancode sc) {
    for (int a = 0; a < PC_ACT_COUNT; a++) {
        if (a != action && sKey[a] == sc) sKey[a] = sKey[action];
    }
    sKey[action] = sc;
    CfgWrite();
}

static int IsOk(SDL_Scancode sc) { return sc == SDL_SCANCODE_RETURN || sc == sKey[PC_ACT_A]; }
static int IsBack(SDL_Scancode sc) { return sc == SDL_SCANCODE_ESCAPE || sc == sKey[PC_ACT_B]; }

/* Controls page; returns when the player goes back. */
static void RunControls(SDL_Renderer *ren, SDL_Texture *frame) {
    int sel = 0, binding = -1;
    for (;;) {
        pc_watchdog_kick();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) exit(0);
            if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) continue;
            SDL_Scancode sc = ev.key.scancode;
            if (binding >= 0) {
                if (sc != SDL_SCANCODE_ESCAPE) Bind(binding, sc);
                binding = -1;
            } else if (sc == SDL_SCANCODE_UP) {
                sel = (sel + CTRL_COUNT - 1) % CTRL_COUNT;
            } else if (sc == SDL_SCANCODE_DOWN) {
                sel = (sel + 1) % CTRL_COUNT;
            } else if (IsOk(sc)) {
                if (sel == CTRL_BACK) return;
                if (sel == CTRL_RESET) {
                    DefaultKeys();
                    CfgWrite();
                } else {
                    binding = sel;
                }
            } else if (IsBack(sc)) {
                return;
            }
        }
        DrawControls(ren, frame, sel, binding, NULL);
        SDL_Delay(16);
    }
}

/* Multiplayer page; returns when the player goes back. */
static void RunMulti(SDL_Window *win, SDL_Renderer *ren, SDL_Texture *frame) {
    int sel = MP_SERVER, edit = -1;
    char text[256] = "";
    for (;;) {
        pc_watchdog_kick();
        pc_net_poll();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) exit(0);
            if (edit >= 0) {
                size_t max = edit == MP_SERVER ? sizeof sServer - 1 : edit == MP_NAME ? NET_NAME_LEN : sizeof sPassword - 1;
                if (ev.type == SDL_EVENT_TEXT_INPUT) {
                    for (const char *c = ev.text.text; *c; c++) {
                        size_t n = strlen(text);
                        if ((unsigned char)*c >= 32 && (unsigned char)*c < 127 && n < max) {
                            text[n] = *c;
                            text[n + 1] = 0;
                        }
                    }
                } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                    SDL_Scancode sc = ev.key.scancode;
                    if (sc == SDL_SCANCODE_BACKSPACE && text[0]) {
                        text[strlen(text) - 1] = 0;
                    } else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER || sc == SDL_SCANCODE_ESCAPE) {
                        if (sc != SDL_SCANCODE_ESCAPE) {
                            char *dst = edit == MP_SERVER ? sServer : edit == MP_NAME ? sName : sPassword;
                            snprintf(dst, max + 1, "%s", text);
                            CfgWrite();
                        }
                        edit = -1;
                        SDL_StopTextInput(win);
                    }
                }
                continue;
            }
            if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) continue;
            SDL_Scancode sc = ev.key.scancode;
            if (sc == SDL_SCANCODE_UP) {
                sel = (sel + MP_COUNT - 1) % MP_COUNT;
            } else if (sc == SDL_SCANCODE_DOWN) {
                sel = (sel + 1) % MP_COUNT;
            } else if (IsOk(sc)) {
                if (sel == MP_BACK) return;
                if (sel == MP_CONNECT) {
                    if (pc_net_wanted()) pc_net_disconnect();
                    else pc_net_connect();
                    sOnline = pc_net_wanted();
                    CfgWrite();
                } else {
                    edit = sel;
                    snprintf(text, sizeof text, "%s", sel == MP_SERVER ? sServer : sel == MP_NAME ? sName : sPassword);
                    SDL_StartTextInput(win);
                }
            } else if (IsBack(sc)) {
                return;
            }
        }
        DrawMulti(ren, frame, sel, edit, text, NULL);
        SDL_Delay(16);
    }
}

/* Returns when the menu closes. The game is paused meanwhile. */
void pc_menu_run(void *window, void *renderer, void *frameTexture) {
    SDL_Window *win = window;
    SDL_Renderer *ren = renderer;
    int sel = ITEM_RESUME;
    sStatus[0] = 0;
    sFastForward = 0;
    for (;;) {
        pc_watchdog_kick();
        pc_net_poll();
        int picked = SDL_GetAtomicInt(&sPicked);
        if (picked != PICK_NONE) {
            SDL_SetAtomicInt(&sPicked, PICK_NONE);
            sPicking = PICK_NONE;
            if (picked == PICK_LOAD) {
                snprintf(sSavePath, sizeof sSavePath, "%s", sPickedPath);
                Restart();
            } else if (picked == PICK_COPY) {
                snprintf(sStatus, sizeof sStatus, pc_backup_copy_to(sPickedPath) ? "Copied to %.20s" : "Copy failed: %.18s",
                         Basename(sPickedPath));
            } else if (picked == PICK_ROM) {
                if (pc_rom_check(sPickedPath)) {
                    snprintf(sRomPath, sizeof sRomPath, "%s", sPickedPath);
                    Restart();
                } else {
                    snprintf(sStatus, sizeof sStatus, "Not HeartGold (US) ROM");
                }
            }
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) exit(0);
            if (ev.type != SDL_EVENT_KEY_DOWN || sPicking) continue;
            SDL_Scancode sc = ev.key.scancode;
            if (sc == SDL_SCANCODE_UP) {
                sel = (sel + ITEM_COUNT - 1) % ITEM_COUNT;
            } else if (sc == SDL_SCANCODE_DOWN) {
                sel = (sel + 1) % ITEM_COUNT;
            } else if (sc == SDL_SCANCODE_LEFT) {
                Change(win, sel, -1);
            } else if (sc == SDL_SCANCODE_RIGHT) {
                Change(win, sel, 1);
            } else if (IsOk(sc)) {
                if (sel == ITEM_RESUME) return;
                if (sel == ITEM_QUIT) exit(0); /* saves are written through */
                if (sel == ITEM_CONTROLS) {
                    RunControls(ren, frameTexture);
                } else if (sel == ITEM_MULTI) {
                    RunMulti(win, ren, frameTexture);
                } else if (sel == ITEM_LOAD) {
                    sPicking = PICK_LOAD;
                    SDL_ShowOpenFileDialog(OnPick, (void *)(intptr_t)PICK_LOAD, win, kFilters, 2, pc_backup_path(), false);
                } else if (sel == ITEM_COPY) {
                    sPicking = PICK_COPY;
                    SDL_ShowSaveFileDialog(OnPick, (void *)(intptr_t)PICK_COPY, win, kFilters, 2, pc_backup_path());
                } else if (sel == ITEM_ROM) {
                    sPicking = PICK_ROM;
                    ShowRomDialog(win);
                } else {
                    Change(win, sel, 1);
                }
            } else if (IsBack(sc)) {
                return;
            }
        }
        DrawMain(ren, frameTexture, sel, NULL);
        SDL_Delay(16);
    }
}

void pc_menu_shot(void *renderer, void *frameTexture, const char *path) {
    char controls[1100];
    DrawMain(renderer, frameTexture, ITEM_FF, path);
    snprintf(controls, sizeof controls, "%s.controls.bmp", path);
    DrawControls(renderer, frameTexture, PC_ACT_FF, -1, controls);
    snprintf(controls, sizeof controls, "%s.multi.bmp", path);
    DrawMulti(renderer, frameTexture, MP_NAME, -1, "", controls);
}

/* Apply the saved fullscreen setting once the window exists. */
void pc_menu_window_ready(void *window) {
    if (sFullscreen) SDL_SetWindowFullscreen((SDL_Window *)window, true);
}
#else
void pc_cfg_load(void) {}
void pc_cfg_set_rom(const char *path) { (void)path; }
const char *pc_menu_pick_rom(void) { return NULL; }
#endif
