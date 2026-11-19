/* Register file and helpers for C translated from the game's asm (see pc/include/asm_rt.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asm_rt.h"
#include "pc_platform.h"

PcCpu pc_cpu;

/* HG_TRACE=name,...: log entry r0-r3 and returned r0 of those asm functions (asm_rt.h PC_TRACE_*). */
int pc_trace_on;
static char sTraceList[1024];

void pc_trace(const char *name, int returning) {
    char key[128];
    snprintf(key, sizeof key, ",%s,", name);
    if (strstr(sTraceList, key) == NULL) return;
    static au32 sOut[64]; /* entry r3 per traced call depth: often an out-pointer worth showing on return */
    static int sDepth;
    if (!returning && sDepth < 64) sOut[sDepth++] = R(3);
    if (returning && sDepth > 0) {
        au32 o = sOut[--sDepth];
        if (o >= 0x02000000u && o < 0x08000000u) fprintf(stderr, "        (entry r3 %08x now holds %08x)\n", o, M32(o));
    }
    if (returning && R(0) >= 0x02000000u && R(0) < 0x02400000u) { /* pointer into main RAM: show what it points at */
        const au16 *p = (const au16 *)(uintptr_t)R(0);
        fprintf(stderr, "trace < %s = %08x -> %04x %04x %04x %04x %04x %04x %04x %04x\n", name, R(0),
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    } else if (returning) fprintf(stderr, "trace < %s = %08x\n", name, R(0));
    else fprintf(stderr, "trace > %s(%08x, %08x, %08x, %08x)\n", name, R(0), R(1), R(2), R(3));
}

#define ASM_STACK_TOP 0x07FFFF00u /* top of the emulated ARM stack, inside the reserved DS range */

void pc_cpu_init(void) {
    const char *trace = getenv("HG_TRACE");
    if (trace != NULL && *trace) {
        snprintf(sTraceList, sizeof sTraceList, ",%s,", trace);
        pc_trace_on = 1;
    }
    SP = ASM_STACK_TOP;
    R(14) = PC_LR_RETURN;

    /* Self-check of the shift macros (a shadowed temporary once made ROR_R return garbage). */
    au32 n = 0, z = 0, c = 0, v = 0, d;
    int ok = 1;
    ROR_R(d, 6u << 27, 27u); ok &= d == 6;
    ROR_R(d, 0x80000001u, 1u); ok &= d == 0xC0000000u && c == 1;
    LSL_R(d, 3u, 4u); ok &= d == 48;
    LSR_R(d, 0x80u, 7u); ok &= d == 1;
    ASR_R(d, 0x80000000u, 31u); ok &= d == 0xFFFFFFFFu;
    (void)n; (void)z; (void)v;
    if (!ok) {
        fprintf(stderr, "hg_pc: asm shift macro self-check failed\n");
        exit(1);
    }
}

void pc_asm_unsupported(const char *name, const char *why) {
    fprintf(stderr, "hg_pc: %s not translated (%s)\n", name, why);
    pc_stub_hit(name);
}
