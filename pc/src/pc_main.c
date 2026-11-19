/* PC port entry point and runtime helpers.
 *
 * A watchdog fires when no frame finished for WATCHDOG_SECONDS (usually a busy-wait on a DS hardware
 * register), and crashes print a backtrace too. Addresses are raw; resolve with:
 *   addr2line -f -e build/pc/hg_pc <addresses>      (Windows: build/pc-win/hg_pc.exe, fixed image base) */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc_platform.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <execinfo.h>
#include <unistd.h>
#endif

#define WATCHDOG_SECONDS 5

static volatile unsigned long sFrames;
static volatile unsigned long sAlive; /* Windows watchdog: bumped each frame and while the options menu is open */

#ifdef _WIN32
#include <stdarg.h>
/* Crash and hang reports bypass stdio: msvcrt's unbuffered stderr writes a character at a time under
 * the stream lock, and the game thread can be stopped holding it (a report once never appeared). */
static void ErrOut(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    DWORD done;
    if (n > 0) WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, n < (int)sizeof buf ? (DWORD)n : sizeof buf - 1, &done, NULL);
}

/* Windows builds keep frame pointers (pc_build.py), so stacks are an EBP chain. */
static void PrintFrames(void **frames, int n) {
    for (int i = 0; i < n; i++) ErrOut("  [0x%08x]\n", (unsigned)(uintptr_t)frames[i]);
}
#define BACKTRACE(frames, max) ((int)CaptureStackBackTrace(1, (max), (frames), NULL))

/* Walk another context's EBP chain; ReadProcessMemory stops cleanly at a bad pointer. */
static void PrintContext(const CONTEXT *c) {
    void *frames[32];
    int n = 0;
    DWORD ebp = c->Ebp, next[2];
    frames[n++] = (void *)(uintptr_t)c->Eip;
    while (n < 32 && ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)ebp, next, sizeof next, NULL) && next[0] > ebp) {
        frames[n++] = (void *)(uintptr_t)next[1];
        ebp = next[0];
    }
    PrintFrames(frames, n);
}

static LONG WINAPI OnCrash(EXCEPTION_POINTERS *ep) {
    ErrOut("\nhg_pc: exception 0x%08lx at 0x%08lx, backtrace:\n", ep->ExceptionRecord->ExceptionCode,
           (unsigned long)(uintptr_t)ep->ExceptionRecord->ExceptionAddress);
    PrintContext(ep->ContextRecord);
    _exit(3);
}

static HANDLE sMainThread;

static DWORD WINAPI Watchdog(void *arg) {
    (void)arg;
    unsigned long last = sAlive;
    for (int idle = 0;; ) {
        Sleep(1000);
        idle = sAlive == last ? idle + 1 : 0;
        last = sAlive;
        if (idle < WATCHDOG_SECONDS) continue;
        SuspendThread(sMainThread);
        CONTEXT c = { .ContextFlags = CONTEXT_CONTROL };
        GetThreadContext(sMainThread, &c);
        ErrOut("\nhg_pc: no frame finished in time (hang), backtrace:\n");
        PrintContext(&c);
        _exit(3);
    }
}

static void WatchdogStart(void) {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sMainThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    SetUnhandledExceptionFilter(OnCrash);
    CloseHandle(CreateThread(NULL, 0, Watchdog, NULL, 0, NULL));
}
#define WATCHDOG_KICK() ((void)sAlive++) /* the watchdog thread watches sAlive */
#else
#define PrintFrames(frames, n) backtrace_symbols_fd((frames), (n), 2)
#define BACKTRACE(frames, max) backtrace((frames), (max))

static void OnHangOrCrash(int sig) {
    void *frames[32];
    int n = backtrace(frames, 32);
    fprintf(stderr, "hg_pc: %s, backtrace:\n", sig == SIGALRM ? "no frame finished in time (hang)" : strsignal(sig));
    backtrace_symbols_fd(frames, n, 2);
    _exit(3);
}

static void WatchdogStart(void) {
    signal(SIGALRM, OnHangOrCrash);
    signal(SIGSEGV, OnHangOrCrash);
    signal(SIGBUS, OnHangOrCrash);
    signal(SIGFPE, OnHangOrCrash);
    alarm(WATCHDOG_SECONDS);
}
#define WATCHDOG_KICK() alarm(WATCHDOG_SECONDS)
#endif

void NitroMain(void);

void pc_watchdog_kick(void) { WATCHDOG_KICK(); }

/* Called by every generated stub (build/pc/stubs.c) for code not yet ported. */
void pc_stub_hit(const char *name) {
    // Exploration mode: log each unported function once and return (garbage result), to see how far boot gets.
    if (getenv("HG_STUB_CONTINUE") != NULL) {
        static const char *seen[4096];
        static int nSeen;
        for (int i = 0; i < nSeen; i++) {
            if (seen[i] == name) return;
        }
        if (nSeen < 4096) seen[nSeen++] = name;
        fprintf(stderr, "hg_pc: stub %s\n", name);
        return;
    }
    void *frames[32];
    int n = BACKTRACE(frames, 32);
    fprintf(stderr, "hg_pc: reached unported function %s, backtrace:\n", name);
    PrintFrames(frames, n);
    exit(2);
}

/* Print why and the call stack (OS_Terminate and other fatal paths). */
void pc_backtrace(const char *why) {
    void *frames[32];
    int n = BACKTRACE(frames, 32);
    fprintf(stderr, "hg_pc: %s, backtrace:\n", why);
    PrintFrames(frames, n);
}

/* GF_ASSERT failure (retail builds ignore these): with HG_ASSERT_LOG set, print the call stack. */
void pc_assert_hit(void) {
    if (getenv("HG_ASSERT_LOG") == NULL) return;
    void *frames[8];
    int n = BACKTRACE(frames, 8);
    fprintf(stderr, "hg_pc: GF_ASSERT failed, backtrace:\n");
    PrintFrames(frames, n);
}

/* Online / anti-piracy library calls (generated offline stubs): log each name once, then return 0. */
void pc_offline_call(const char *name) {
    static const char *seen[512];
    static int nSeen;
    for (int i = 0; i < nSeen; i++) {
        if (seen[i] == name) return;
    }
    if (nSeen < 512) seen[nSeen++] = name;
    fprintf(stderr, "hg_pc: offline: %s\n", name);
}

void pc_frame_end(void) {
    pc_video_poll();
    pc_input_frame();
    pc_snd_frame();
    WATCHDOG_KICK();
    pc_debug_frame(sFrames + 1);
    pc_link_frame(sFrames + 1);
    pc_net_poll();
    pc_ghost_frame();
    pc_npc_frame();
    pc_challenge_frame();
    pc_chat_frame(sFrames + 1);
    if (++sFrames % 60 == 0) {
        fprintf(stderr, "hg_pc: %lu frames\n", sFrames);
    }
    const char *limit = getenv("HG_MAX_FRAMES");
    if (limit != NULL && sFrames >= strtoul(limit, NULL, 10)) {
        fprintf(stderr, "hg_pc: HG_MAX_FRAMES reached\n");
        exit(0);
    }
}

int main(int argc, char **argv) {
    pc_mem_init(); /* first: nothing else may take the DS range */
    pc_argv = argv;
    pc_cfg_load();
    WatchdogStart();
    /* ROM: command line, $HG_ROM, the remembered one, or ask (windowed only). */
    const char *rom = argc > 1 ? argv[1] : getenv("HG_ROM");
    if (rom == NULL || *rom == 0) rom = pc_cfg_rom();
    if (rom == NULL) rom = pc_menu_pick_rom();
    if (rom == NULL) {
        fprintf(stderr, "usage: %s <your HeartGold (US) .nds>\n", argv[0]);
        return 1;
    }
    pc_cfg_set_rom(rom);
    pc_dgt_selftest();
    if (!pc_mwrt_selftest()) {
        fprintf(stderr, "hg_pc: compiler runtime self-test failed\n");
        return 1;
    }
    if (!pc_cp_selftest()) { /* needs the IO page mapped */
        fprintf(stderr, "hg_pc: divider/sqrt self-test failed\n");
        return 1;
    }
    pc_vram_init();
    pc_g3_init();
    if (!pc_g3_selftest()) {
        fprintf(stderr, "hg_pc: geometry engine self-test failed\n");
        return 1;
    }
    if (getenv("HG_CRASHTEST") != NULL) *(volatile u32 *)0x354 = 1; /* exercises the crash report */
    pc_video_init();
    pc_cpu_init();
    pc_wm_init();
    if (!pc_crypto_selftest()) { /* needs the emulated stack */
        fprintf(stderr, "hg_pc: ARM-mode crypto self-test failed\n");
        return 1;
    }
    pc_snd_init();
    pc_rom_open(rom);
    pc_backup_open(rom);
    pc_input_frame();
    NitroMain();
    return 0;
}
