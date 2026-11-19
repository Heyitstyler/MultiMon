/* The user's own ROM: opened once, read on demand. Nothing from it is bundled with the port. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nitro.h>
#include "pc_platform.h"

static FILE *sRom;

void pc_rom_read(u32 offset, void *dst, u32 size) {
    if (fseek(sRom, offset, SEEK_SET) != 0 || fread(dst, 1, size, sRom) != size) {
        fprintf(stderr, "hg_pc: ROM read failed at 0x%08x (+0x%x)\n", (unsigned)offset, (unsigned)size);
        exit(1);
    }
}

BOOL pc_rom_check(const char *path) {
    FILE *f = fopen(path, "rb");
    u8 code[4];
    BOOL ok = f != NULL && fseek(f, 0x0C, SEEK_SET) == 0 && fread(code, 1, 4, f) == 4 && memcmp(code, "IPKE", 4) == 0;
    if (f != NULL) fclose(f);
    return ok;
}

void pc_rom_open(const char *path) {
    sRom = fopen(path, "rb");
    if (sRom == NULL) {
        perror(path);
        exit(1);
    }
    u8 header[0x160];
    pc_rom_read(0, header, sizeof(header));
    if (memcmp(header + 0x0C, "IPKE", 4) != 0) {
        fprintf(stderr, "hg_pc: %s is not Pokemon HeartGold (US), game code IPKE\n", path);
        exit(1);
    }
    // The DS boot ROM leaves a copy of the cartridge header in the system work area; the SDK reads it.
    memcpy((void *)HW_ROM_HEADER_BUF, header, sizeof(header));
    memcpy((void *)HW_CARD_ROM_HEADER, header, sizeof(header));
}
