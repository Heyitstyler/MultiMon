/* PC: SND_CalcDecibel(128) reads one entry past SNDi_DecibelTable. The game does this on purpose
 * (GF_SndHandleMoveVolume(0, 128, 15) in field_control.c and field_warp_tasks.c). On the DS the
 * next halfword in ROM is PitchTable[0] = 0, i.e. 0 dB; on the host it is whatever the linker put
 * there (it made the field music +21.6 dB). */
#ifndef PC_SND_UTIL_H
#define PC_SND_UTIL_H
#define SND_CalcDecibel SND_CalcDecibel_sdk
#include_next <nitro/snd/common/util.h>
#undef SND_CalcDecibel
static inline s16 SND_CalcDecibel(int scale) {
    return scale == SND_DECIBEL_TABLE_SIZE ? 0 : SNDi_DecibelTable[scale];
}
#endif
