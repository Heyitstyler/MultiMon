/* PC: on the ARM7 side, starting a channel calls the host mixer (pc/src/pc_snd.c) so a channel
 * stopped and restarted within one sound tick still restarts. */
#define SND_StartChannel SND_StartChannel_sdk
#include_next <nitro/snd/common/channel.h>
#undef SND_StartChannel
#ifdef SDK_ARM7
void pc_snd7_start_channel(int chNo);
#define SND_StartChannel pc_snd7_start_channel
#endif
