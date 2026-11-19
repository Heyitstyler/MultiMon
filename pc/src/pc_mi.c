/* Host replacements for NitroSDK MI memory and decompression routines (ARM asm on the DS). */
#include <string.h>
#include <nitro.h>
#include "pc_platform.h"

void MIi_CpuCopy32(const void *srcp, void *destp, u32 size) {
    if (pc_g3_is_port(destp)) pc_g3_send(srcp, size / 4);
    else if (pc_mem_copy_ok(srcp, destp, size)) memmove(destp, srcp, size);
}
void MIi_CpuCopy16(const void *srcp, void *destp, u32 size) { if (pc_mem_copy_ok(srcp, destp, size)) memmove(destp, srcp, size); }
void MIi_CpuCopyFast(const void *srcp, void *destp, u32 size) { if (pc_mem_copy_ok(srcp, destp, size)) memmove(destp, srcp, size); }
void MI_CpuCopy8(const void *src, void *dest, u32 size) { if (pc_mem_copy_ok(src, dest, size)) memmove(dest, src, size); }
void MI_Copy32B(const void *pSrc, void *pDest) { memmove(pDest, pSrc, 32); }
void MI_Copy36B(const void *pSrc, void *pDest) { memmove(pDest, pSrc, 36); }
void MI_Copy64B(const void *pSrc, void *pDest) { memmove(pDest, pSrc, 64); }
void MI_Zero36B(void *pDest) { memset(pDest, 0, 36); }
void MI_CpuFill8(void *dest, u8 data, u32 size) { memset(dest, data, size); }

void MIi_CpuClear32(u32 data, void *destp, u32 size) {
    u32 *d = destp;
    for (u32 i = 0; i < size / 4; i++) d[i] = data;
}
void MIi_CpuClearFast(u32 data, void *destp, u32 size) { MIi_CpuClear32(data, destp, size); }
void MIi_CpuClear16(u16 data, void *destp, u32 size) {
    u16 *d = destp;
    for (u32 i = 0; i < size / 2; i++) d[i] = data;
}

/* Writes every word to the same address (a hardware FIFO port). */
void MIi_CpuSend32(const void *srcp, volatile void *destp, u32 size) {
    const u32 *s = srcp;
    if (pc_g3_is_port(destp)) { pc_g3_send(s, size / 4); return; } /* GXFIFO: run commands */
    for (u32 i = 0; i < size / 4; i++) *(volatile u32 *)destp = s[i];
}

/* LZ77 (type 0x10). Header: u32 = type | decompressed size << 8. Each flag byte covers 8 blocks,
 * MSB first: 0 = literal byte, 1 = 2-byte back-reference (length 3-18, distance 1-4096). */
static void UncompressLZ(const u8 *src, u8 *dst, BOOL halfwords) {
    u32 size = (src[0] | src[1] << 8 | src[2] << 16 | src[3] << 24) >> 8;
    u32 out = 0;
    u16 pending = 0;
    src += 4;
    // VRAM needs 16-bit writes, so the LZ16 variant assembles byte pairs before storing.
    #define PUT(b) do { \
        if (!halfwords) dst[out] = (b); \
        else if (out & 1) *(u16 *)(dst + out - 1) = (u16)(pending | (b) << 8); \
        else pending = (b); \
        out++; } while (0)
    #define GET(i) (halfwords && ((i) & 1) == 0 && (i) == out - 1 ? (u8)pending : dst[i])
    while (out < size) {
        u8 flags = *src++;
        for (int bit = 0; bit < 8 && out < size; bit++, flags <<= 1) {
            if (!(flags & 0x80)) {
                PUT(*src++);
            } else {
                u32 len = (src[0] >> 4) + 3;
                u32 dist = (((src[0] & 0xF) << 8) | src[1]) + 1;
                src += 2;
                for (u32 i = 0; i < len && out < size; i++) {
                    u8 b = GET(out - dist);
                    PUT(b);
                }
            }
        }
    }
    #undef PUT
    #undef GET
}
void MI_UncompressLZ8(const void *srcp, void *destp) { UncompressLZ(srcp, destp, FALSE); }
void MI_UncompressLZ16(const void *srcp, void *destp) { UncompressLZ(srcp, destp, TRUE); }

u8 MATH_CountPopulation(u32 x) { return (u8)__builtin_popcount(x); }
