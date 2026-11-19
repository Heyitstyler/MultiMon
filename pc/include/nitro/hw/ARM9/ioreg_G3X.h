/* PC port: the geometry FIFO port (0x04000400) moved onto the trap page, like the command ports
 * (see pc/include/nitro/hw/ARM9/ioreg_G3.h). Status and result registers stay at their DS addresses;
 * pc/src/pc_g3.c keeps them current. */
#ifndef PC_NITRO_IOREG_G3X_H
#define PC_NITRO_IOREG_G3X_H
#include_next <nitro/hw/ARM9/ioreg_G3X.h>

#ifndef PC_G3_TRAP_BASE
#define PC_G3_TRAP_BASE 0x04800000u
#endif
#undef REG_GXFIFO_ADDR
#define REG_GXFIFO_ADDR (PC_G3_TRAP_BASE + REG_GXFIFO_OFFSET)

#endif
