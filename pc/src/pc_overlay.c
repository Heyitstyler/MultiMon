/* Host overlays. Every overlay's code and data is linked into the PC binary, so "loading" one
 * copies nothing and runs no DS-side initializers. FS_LoadOverlayInfo (SDK, unchanged) still
 * reads the overlay table from the ROM, so overlay IDs and sizes stay meaningful.
 *
 * Loads and unloads are still recorded: several overlays share each RAM region, and calls the
 * disassembly labelled with one of them really go to whichever is loaded (asm2c's ovl_dispatch.c).
 *
 * shortcut: overlay .data/.bss is not reset when an overlay is reloaded, and capsule_menu's
 * NitroStaticInit is not run; add a per-overlay pristine copy + init call if either shows up.
 */
#include <nitro.h>
#include "pc_platform.h"

/* Per overlay ID: 0 if not loaded, else a sequence number of its load (latest load wins a region). */
u32 pc_ovl_seq[PC_OVL_MAX];
static u32 sOvlLoads;
static void Loaded(u32 id) { if (id < PC_OVL_MAX) pc_ovl_seq[id] = ++sOvlLoads; }
static void Unloaded(u32 id) { if (id < PC_OVL_MAX) pc_ovl_seq[id] = 0; }

BOOL FS_LoadOverlayImage(FSOverlayInfo *p_ovi) { (void)p_ovi; return TRUE; }
/* Callers wait on and close p_file afterwards, so it must be a real open file. */
BOOL FS_LoadOverlayImageAsync(FSOverlayInfo *p_ovi, FSFile *p_file) {
    return FS_OpenFileFast(p_file, FS_GetOverlayFileID(p_ovi));
}
void FS_StartOverlay(FSOverlayInfo *p_ovi) { Loaded(p_ovi->header.id); }
void FS_EndOverlay(FSOverlayInfo *p_ovi) { (void)p_ovi; }
void FS_ClearOverlayImage(FSOverlayInfo *p_ovi) { (void)p_ovi; }
BOOL FS_UnloadOverlayImage(FSOverlayInfo *p_ovi) { (void)p_ovi; return TRUE; }
BOOL FS_LoadOverlay(MIProcessor target, FSOverlayID id) { (void)target; Loaded(id); return TRUE; }
BOOL FS_UnloadOverlay(MIProcessor target, FSOverlayID id) { (void)target; Unloaded(id); return TRUE; }
