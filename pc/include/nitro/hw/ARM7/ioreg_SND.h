/* PC: the ARM7 sound registers sit at 0x04700400+ instead of 0x04000400+, which on the ARM9 side
 * is the geometry FIFO. Only the ARM7 sound driver (pc_build.py 'snd7' files) includes this header;
 * pc/src/pc_snd.c's mixer reads the channel registers there. */
#include_next <nitro/hw/ARM7/ioreg_SND.h>
#include <nitro/hw/ARM7/mmap_global.h> /* define HW_REG_BASE now so the override below sticks */
#undef HW_REG_BASE
#define HW_REG_BASE 0x04700000
