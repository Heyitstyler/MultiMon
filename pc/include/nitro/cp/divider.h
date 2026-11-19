/* PC port: wraps the SDK's divider setters so the result registers are filled at once.
 * The DS divider computes in hardware after any write to DIVCNT/NUMER/DENOM; on the host those
 * registers are plain memory, so each setter is followed by pc_cp_div_update() (pc/src/pc_cp.c).
 * The SDK originals are kept under *_ds names; getters and direct register reads are unchanged. */
#ifndef PC_NITRO_CP_DIVIDER_H
#define PC_NITRO_CP_DIVIDER_H

#define CP_SetDivControl      CP_SetDivControl_ds
#define CP_SetDivImm32_32_NS_ CP_SetDivImm32_32_NS__ds
#define CP_SetDivImm64_32_NS_ CP_SetDivImm64_32_NS__ds
#define CP_SetDivImm64_64_NS_ CP_SetDivImm64_64_NS__ds
#define CP_SetDivImm32_32     CP_SetDivImm32_32_ds
#define CP_SetDivImm64_32     CP_SetDivImm64_32_ds
#define CP_SetDivImm64_64     CP_SetDivImm64_64_ds
#define CP_SetDiv32_32        CP_SetDiv32_32_ds
#define CP_SetDiv64_32        CP_SetDiv64_32_ds
#define CP_SetDiv64_64        CP_SetDiv64_64_ds
#include_next <nitro/cp/divider.h>
#undef CP_SetDivControl
#undef CP_SetDivImm32_32_NS_
#undef CP_SetDivImm64_32_NS_
#undef CP_SetDivImm64_64_NS_
#undef CP_SetDivImm32_32
#undef CP_SetDivImm64_32
#undef CP_SetDivImm64_64
#undef CP_SetDiv32_32
#undef CP_SetDiv64_32
#undef CP_SetDiv64_64

void pc_cp_div_update(void);

static inline void CP_SetDivControl(u16 p) { CP_SetDivControl_ds(p); pc_cp_div_update(); }
#define PC_CP_DIV_WRAP(name, TN, TD) \
    static inline void name(TN n, TD d) { name##_ds(n, d); pc_cp_div_update(); }
PC_CP_DIV_WRAP(CP_SetDivImm32_32_NS_, u32, u32)
PC_CP_DIV_WRAP(CP_SetDivImm64_32_NS_, u64, u32)
PC_CP_DIV_WRAP(CP_SetDivImm64_64_NS_, u64, u64)
PC_CP_DIV_WRAP(CP_SetDivImm32_32, u32, u32)
PC_CP_DIV_WRAP(CP_SetDivImm64_32, u64, u32)
PC_CP_DIV_WRAP(CP_SetDivImm64_64, u64, u64)
PC_CP_DIV_WRAP(CP_SetDiv32_32, u32, u32)
PC_CP_DIV_WRAP(CP_SetDiv64_32, u64, u32)
PC_CP_DIV_WRAP(CP_SetDiv64_64, u64, u64)
#undef PC_CP_DIV_WRAP

#endif
