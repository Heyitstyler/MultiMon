/* Host save memory: the cartridge's 512 KB flash, kept in a .sav file next to the ROM
 * (or at $HG_SAVE). Same raw layout as a DS flash dump, so saves can move between the two.
 * Every write and erase is written through to the file before the call returns. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nitro.h>
#include "pc_platform.h"

#define FLASH_SIZE (512 * 1024)

static u8 sFlash[FLASH_SIZE];
static FILE *sSaveFile;
static char sPath[4096];

const char *pc_backup_path(void) { return sPath[0] ? sPath : NULL; }

/* The whole save memory as it is now, to another file (the options menu's "Save a copy"). */
BOOL pc_backup_copy_to(const char *path) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) return FALSE;
    BOOL ok = fwrite(sFlash, 1, FLASH_SIZE, f) == FLASH_SIZE;
    return fclose(f) == 0 && ok;
}
static CARDResult sResult = CARD_RESULT_SUCCESS;

void pc_backup_open(const char *romPath) {
    char *path = sPath;
    const char *env = getenv("HG_SAVE");
    if (env == NULL || *env == 0) env = pc_cfg_save_path(); /* chosen in the options menu */
    if (env != NULL) {
        snprintf(path, sizeof(sPath), "%s", env);
    } else {
        snprintf(path, sizeof(sPath), "%s", romPath);
        char *dot = strrchr(path, '.');
        if (dot == NULL || strchr(dot, '/') != NULL) dot = path + strlen(path);
        snprintf(dot, sizeof(sPath) - (dot - path), ".sav");
    }
    sSaveFile = fopen(path, "r+b");
    if (sSaveFile != NULL) {
        size_t n = fread(sFlash, 1, FLASH_SIZE, sSaveFile);
        if (n < FLASH_SIZE) memset(sFlash + n, 0xFF, FLASH_SIZE - n); // short file: rest reads as erased
        return;
    }
    memset(sFlash, 0xFF, FLASH_SIZE); // erased flash
    sSaveFile = fopen(path, "w+b");
    if (sSaveFile == NULL || fwrite(sFlash, 1, FLASH_SIZE, sSaveFile) != FLASH_SIZE || fflush(sSaveFile) != 0) {
        perror(path);
        exit(1);
    }
    fprintf(stderr, "hg_pc: created new save file %s\n", path);
}

static BOOL Persist(u32 offset, u32 len) {
    if (fseek(sSaveFile, offset, SEEK_SET) == 0 && fwrite(sFlash + offset, 1, len, sSaveFile) == len && fflush(sSaveFile) == 0)
        return TRUE;
    perror("hg_pc: save write failed"); // the game then reports the save as failed
    return FALSE;
}

BOOL CARDi_RequestStreamCommand(u32 src, u32 dst, u32 len, MIDmaCallback callback, void *arg, BOOL is_async,
                                CARDRequest req_type, int req_retry, CARDRequestMode req_mode) {
    (void)is_async;
    (void)req_retry;
    (void)req_mode;
    BOOL toFlash = req_type != CARD_REQ_READ_BACKUP;
    u32 flashOffset = toFlash ? dst : src;
    sResult = CARD_RESULT_SUCCESS;
    if (flashOffset > FLASH_SIZE || len > FLASH_SIZE - flashOffset) {
        sResult = CARD_RESULT_FAILURE;
    } else {
        switch (req_type) {
        case CARD_REQ_READ_BACKUP:
            memcpy((void *)dst, sFlash + src, len);
            break;
        case CARD_REQ_WRITE_BACKUP:
        case CARD_REQ_PROGRAM_BACKUP:
            memcpy(sFlash + dst, (const void *)src, len);
            if (!Persist(dst, len)) sResult = CARD_RESULT_FAILURE;
            break;
        case CARD_REQ_VERIFY_BACKUP:
            if (memcmp(sFlash + dst, (const void *)src, len) != 0) sResult = CARD_RESULT_FAILURE;
            break;
        case CARD_REQ_ERASE_PAGE_BACKUP:
        case CARD_REQ_ERASE_SECTOR_BACKUP:
        case CARD_REQ_ERASE_SUBSECTOR_BACKUP:
            memset(sFlash + dst, 0xFF, len);
            if (!Persist(dst, len)) sResult = CARD_RESULT_FAILURE;
            break;
        default:
            fprintf(stderr, "hg_pc: unsupported backup request %d\n", (int)req_type);
            sResult = CARD_RESULT_FAILURE;
            break;
        }
    }
    if (callback != NULL) callback(arg);
    return sResult == CARD_RESULT_SUCCESS;
}

/* Write-and-verify variants of the sector API land here too. */
BOOL CARDi_RequestWriteSectorCommand(u32 src, u32 dst, u32 len, BOOL verify, MIDmaCallback callback, void *arg, BOOL is_async) {
    (void)verify; // the host write is exact
    return CARDi_RequestStreamCommand(src, dst, len, callback, arg, is_async, CARD_REQ_WRITE_BACKUP, 1, CARD_REQUEST_MODE_SEND);
}

BOOL CARD_IdentifyBackup(CARDBackupType type) { (void)type; return TRUE; }
CARDResult CARD_GetResultCode(void) { return sResult; }
void CARD_LockBackup(u16 lock_id) { (void)lock_id; }
void CARD_UnlockBackup(u16 lock_id) { (void)lock_id; }
BOOL CARD_TryWaitBackupAsync(void) { return TRUE; }
BOOL CARD_WaitBackupAsync(void) { return sResult == CARD_RESULT_SUCCESS; }
void CARD_CancelBackupAsync(void) {}
void CARD_SetCacheFlushThreshold(u32 icache, u32 dcache) { (void)icache; (void)dcache; }
