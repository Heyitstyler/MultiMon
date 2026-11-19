/* Reserve the DS memory map at its real addresses inside the 32-bit process.
 *
 * Game and SDK code (and later translated asm) use fixed DS addresses: main RAM and the
 * system work area at 0x02xxxxxx, DTCM at 0x027E0000, IO registers at 0x04000000, palette,
 * VRAM and OAM at 0x05-0x07xxxxxx. Backing them with plain memory lets register and VRAM
 * writes land somewhere the host renderer can read. Registers with side effects are handled
 * by host replacements of the SDK functions that touch them.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "pc_platform.h"

#define DS_MAP_START 0x01000000u /* covers ITCM 0x01FF8000 */
#define DS_MAP_END   0x08000000u /* end of OAM; the host binary loads at 0x08048000 (Windows: 0x08000000) */

void pc_mem_init(void) {
    void *want = (void *)DS_MAP_START;
#ifdef _WIN32
    // Must run before anything (SDL, big mallocs) can take this range; the exe has a fixed base above it.
    void *got = VirtualAlloc(want, DS_MAP_END - DS_MAP_START, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (getenv("HG_MEM_FAKEFAIL") != NULL && getenv("HG_MEM_RETRY") == NULL) got = NULL; /* tests the retry below */
    if (got != want) {
        fprintf(stderr, "hg_pc: cannot reserve DS address range 0x01000000-0x08000000 (error %lu)\n", GetLastError());
        MEMORY_BASIC_INFORMATION mi;
        for (uintptr_t a = DS_MAP_START; a < DS_MAP_END && VirtualQuery((void *)a, &mi, sizeof mi); a = (uintptr_t)mi.BaseAddress + mi.RegionSize) {
            if (mi.State == MEM_FREE) continue;
            char name[MAX_PATH] = "";
            if (mi.Type == MEM_IMAGE) GetModuleFileNameA((HMODULE)mi.AllocationBase, name, sizeof name);
            fprintf(stderr, "hg_pc:   in use: %p +0x%lx (%s) %s\n", mi.BaseAddress, (unsigned long)mi.RegionSize,
                    mi.Type == MEM_IMAGE ? "image" : mi.Type == MEM_MAPPED ? "mapped" : "private", name);
        }
        /* Rare, and placed differently in every new process: start again (twice at most), same command
         * line, and pass on its exit code. */
        char tries[8] = "";
        GetEnvironmentVariableA("HG_MEM_RETRY", tries, sizeof tries);
        if (atoi(tries) < 2) {
            snprintf(tries, sizeof tries, "%d", atoi(tries) + 1);
            SetEnvironmentVariableA("HG_MEM_RETRY", tries);
            STARTUPINFOW si = { sizeof si };
            PROCESS_INFORMATION pi;
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(NULL, exe, MAX_PATH);
            if (CreateProcessW(exe, GetCommandLineW(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
                fprintf(stderr, "hg_pc: starting again (try %s)\n", tries);
                DWORD code = 1;
                WaitForSingleObject(pi.hProcess, INFINITE);
                GetExitCodeProcess(pi.hProcess, &code);
                exit((int)code);
            }
        }
        exit(1);
    }
#else
    void *got = mmap(want, DS_MAP_END - DS_MAP_START, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (got != want) {
        perror("hg_pc: cannot reserve DS address range 0x01000000-0x08000000");
        exit(1);
    }
#endif
}

/* End of the host image's .bss, from the default linker scripts (plain `end` is taken by a game variable). */
#ifdef _WIN32
extern char _bss_end__[]; /* PE: __bss_end__ */
#define IMAGE_END ((u32)(uintptr_t)_bss_end__)
#else
extern char _end[];
#define IMAGE_END ((u32)(uintptr_t)_end)
#endif

/* Game code only ever copies within the DS range, the host image (static data), or the stack. A
 * pointer anywhere else is junk, typically read from a struct a VBlank callback uses after the
 * screen's teardown freed it (the game clears the callback a frame late in places). The DS reads
 * junk from such an address harmlessly; here it would fault, so the copy is skipped and logged. */
static BOOL Reachable(const void *p, u32 size) {
    u32 a = (u32)(uintptr_t)p;
    char here;
    u32 sp = (u32)(uintptr_t)&here;
    if (a >= DS_MAP_START && a + size >= a && a + size <= IMAGE_END) return TRUE;
    return (u64)a + size - (sp - 0x1000) <= 0x801000 && a >= sp - 0x1000; /* this or a caller's frame (8 MB) */
}

BOOL pc_mem_copy_ok(const void *src, const void *dest, u32 size) {
    if (size == 0 || (Reachable(src, size) && Reachable(dest, size))) return TRUE;
    static int sLogged;
    if (sLogged < 8) {
        sLogged++;
        fprintf(stderr, "hg_pc: skipped a %u-byte copy %08x -> %08x outside game memory (stale pointer)\n",
                (unsigned)size, (unsigned)(uintptr_t)src, (unsigned)(uintptr_t)dest);
    }
    return FALSE;
}

/* Make one page of the DS range read-only (trap) or writable again. */
void pc_mem_protect_page(u32 addr, BOOL writable) {
#ifdef _WIN32
    DWORD old;
    VirtualProtect((void *)addr, 0x1000, writable ? PAGE_READWRITE : PAGE_READONLY, &old);
#else
    mprotect((void *)addr, 0x1000, writable ? PROT_READ | PROT_WRITE : PROT_READ);
#endif
}
