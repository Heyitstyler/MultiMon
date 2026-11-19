/* Host replacements for the NitroSDK card (cartridge) layer. On the DS, ROM reads go through
 * the ARM7 and a card thread; here they read the user's ROM file directly and finish at once.
 * The SDK's FS archive code (compiled unchanged) sits on top of these. */
#include <nitro.h>
#include "pc_platform.h"

void CARD_Init(void) {}
BOOL CARD_IsPulledOut(void) { return FALSE; }
void CARD_LockRom(u16 lock_id) { (void)lock_id; }
void CARD_UnlockRom(u16 lock_id) { (void)lock_id; }
BOOL CARD_TryWaitRomAsync(void) { return TRUE; }
void CARD_WaitRomAsync(void) {}

void CARDi_ReadRom(u32 dma, const void *src, void *dst, u32 len, MIDmaCallback callback, void *arg, BOOL is_async) {
    (void)dma;
    (void)is_async;
    pc_rom_read((u32)src, dst, len);
    if (callback != NULL) {
        callback(arg);
    }
}
