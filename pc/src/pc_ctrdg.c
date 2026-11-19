/* GBA slot (Slot-2): always empty on PC. The SDK versions read cartridge
   space at 0x08000000, which is not mapped. Every public CTRDG query goes
   through CTRDG_IsExisting, so "no cartridge" there disables Pal Park
   migration and other Slot-2 features cleanly. */
#include <nitro.h>
#include "pc_platform.h"

void CTRDG_Init(void)
{
    CTRDGModuleInfo *cip = CTRDGi_GetModuleInfoAddr();
    cip->moduleID.raw = 0xffff; // SDK value for "nothing inserted"
    cip->isAgbCartridge = FALSE;
    cip->detectPullOut = FALSE;
    *(u8 *)HW_IS_CTRDG_EXIST = 0;
    *(u8 *)HW_SET_CTRDG_MODULE_INFO_ONCE = TRUE;
}

BOOL CTRDG_IsExisting(void)
{
    return FALSE;
}
