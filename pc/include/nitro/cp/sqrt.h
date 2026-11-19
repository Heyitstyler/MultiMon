/* PC port: wraps the SDK's square-root setters so SQRT_RESULT is filled at once
 * (see pc/include/nitro/cp/divider.h and pc/src/pc_cp.c). */
#ifndef PC_NITRO_CP_SQRT_H
#define PC_NITRO_CP_SQRT_H

#define CP_SetSqrtControl   CP_SetSqrtControl_ds
#define CP_SetSqrtImm64_NS_ CP_SetSqrtImm64_NS__ds
#define CP_SetSqrtImm32_NS_ CP_SetSqrtImm32_NS__ds
#define CP_SetSqrtImm64     CP_SetSqrtImm64_ds
#define CP_SetSqrtImm32     CP_SetSqrtImm32_ds
#define CP_SetSqrt64        CP_SetSqrt64_ds
#define CP_SetSqrt32        CP_SetSqrt32_ds
#include_next <nitro/cp/sqrt.h>
#undef CP_SetSqrtControl
#undef CP_SetSqrtImm64_NS_
#undef CP_SetSqrtImm32_NS_
#undef CP_SetSqrtImm64
#undef CP_SetSqrtImm32
#undef CP_SetSqrt64
#undef CP_SetSqrt32

void pc_cp_sqrt_update(void);

#define PC_CP_SQRT_WRAP(name, T) \
    static inline void name(T p) { name##_ds(p); pc_cp_sqrt_update(); }
PC_CP_SQRT_WRAP(CP_SetSqrtControl, u16)
PC_CP_SQRT_WRAP(CP_SetSqrtImm64_NS_, u64)
PC_CP_SQRT_WRAP(CP_SetSqrtImm32_NS_, u32)
PC_CP_SQRT_WRAP(CP_SetSqrtImm64, u64)
PC_CP_SQRT_WRAP(CP_SetSqrtImm32, u32)
PC_CP_SQRT_WRAP(CP_SetSqrt64, u64)
PC_CP_SQRT_WRAP(CP_SetSqrt32, u32)
#undef PC_CP_SQRT_WRAP

#endif
