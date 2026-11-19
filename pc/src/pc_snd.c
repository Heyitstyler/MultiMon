/* Sound: the DS's ARM7 sound driver and its 16-channel mixer, on the host.
 *
 * The driver is the NitroSDK's own ARM7 code (sequencer, channel allocator, command processing),
 * built by pc_build.py with an a7_ prefix on its symbols. This file stands in for its thread and
 * 5.2 ms timer (snd_main.c), carries PXI words between it and the ARM9 side, and plays the channel
 * registers it writes (moved to 0x04700400+, see pc/include/nitro/hw/ARM7/ioreg_SND.h) the way the
 * DS mixer does: PCM8, PCM16, IMA-ADPCM, PSG square and noise, no interpolation, 10-bit output.
 *
 * Output goes to SDL when windowed. HG_WAV=path also writes it to a WAV file (for headless runs).
 *
 * Game speed (windowed): music keeps real time, sound effects follow the game. The one driver runs
 * on two clocks: music ticks (the players of the BGM, jingles and radio, PC_SndMusicPlayers, and
 * their channels) as the audio device plays, effect ticks (every other player and channel, cries
 * included) as game frames pass. Effect channels also play their samples at the game speed, so at
 * 2x they sound twice as fast and an octave up; at Max they are silent. Headless runs keep one
 * clock, one frame of driver time per game frame, so they repeat exactly. HG_SND_LOG=1 logs the two
 * clocks once a second (windowed; SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=disk runs one unseen).
 * Not emulated: sound capture (reverb/effects that read the mixer output back) and the output
 * selectors that route channels 1 and 3 around the mixer.
 */
#include <nitro.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asm_rt.h"
#include "pc_platform.h"
#ifdef PC_HAVE_SDL
#include <SDL3/SDL.h>
#endif

/* ARM7 driver entry points (pc_build.py SND7). */
void a7_SND_CommandInit(void);
void a7_SND_ExChannelInit(void);
void a7_SND_SeqInit(void);
void a7_SND_AlarmInit(void);
void a7_SND_Enable(void);
void a7_SND_SetOutputSelector(int left, int right, int channel1, int channel3);
void a7_SND_SetMasterVolume(int volume);
void a7_SND_UpdateExChannel(void);
void a7_SND_CommandProc(void);
void a7_SND_SeqMain(BOOL doPeriodicProc);
void a7_SND_ExChannelMain(BOOL doPeriodicProc);
void a7_SND_UpdateSharedWork(void);
u16 a7_SND_CalcRandom(void);

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define REG7      0x04700000u
#define CH_REG(c) (REG7 + 0x400u + (u32)(c) * 0x10u)
#define CNT_E     0x80000000u

#define ARM7_HZ          33513982u
#define CYCLES_PER_TICK  (SND_PROC_INTERVAL * 64u) /* OS tick = 64 cycles; ~5.2 ms */
#define CYCLES_PER_FRAME 560190u                   /* 263 lines x 2130 cycles */
#define CYCLES_PER_SAMPLE 1024u                     /* mixer output rate ~32728 Hz */
#define OUT_HZ           (ARM7_HZ / CYCLES_PER_SAMPLE)

/* ---- ARM7 side of PXI and the driver thread ---- */

static PXIFifoCallback sArm7Recv;
static BOOL sInTick;

extern SNDWork a7_SNDi_Work;
u32 PC_SndMusicPlayers(void); /* src/sound_radio_sys.c */

enum { CLOCK_ALL, CLOCK_MUSIC, CLOCK_EFFECTS };
static u32 sMusicCh; /* channels that music players own, as of the last tick */
static float sEffectRate = 1; /* effect channels' playback speed */
static BOOL sEffectMute;

/* Channels of the players in `players` (a bit each), as a channel bit mask. */
static u32 PlayerChannels(u32 players) {
    u32 mask = 0;
    for (int p = 0; p < SND_PLAYER_NUM; p++) {
        SNDPlayer *pl = &a7_SNDi_Work.player[p];
        if (!(players >> p & 1) || !pl->active_flag) continue;
        for (int t = 0; t < SND_TRACK_NUM_PER_PLAYER; t++) {
            if (pl->tracks[t] == SND_INVALID_TRACK_INDEX) continue;
            for (SNDExChannel *ch = a7_SNDi_Work.track[pl->tracks[t]].channel_list; ch != NULL; ch = ch->nextLink) mask |= 1u << ch->myNo;
        }
    }
    return mask;
}

/* One pass of the driver thread. A periodic pass advances time (sequences, envelopes, sweeps, LFOs):
 * for everything (CLOCK_ALL), or only for music or only for effects. The other side is held still for
 * the pass: its players look paused to the sequencer and its channels look idle to the channel
 * update, and both are restored right after. */
static void Tick(BOOL periodic, int clock) {
    sInTick = TRUE;
    a7_SND_UpdateExChannel();
    a7_SND_CommandProc();
    u32 musicPlayers = PC_SndMusicPlayers();
    if (periodic && clock != CLOCK_ALL) {
        u32 stillPlayers = clock == CLOCK_MUSIC ? ~musicPlayers : musicPlayers, heldPlayers = 0, heldCh = 0;
        for (int p = 0; p < SND_PLAYER_NUM; p++) {
            SNDPlayer *pl = &a7_SNDi_Work.player[p];
            if ((stillPlayers >> p & 1) && pl->active_flag && !pl->pause_flag) {
                pl->pause_flag = TRUE;
                heldPlayers |= 1u << p;
            }
        }
        a7_SND_SeqMain(TRUE);
        for (int p = 0; p < SND_PLAYER_NUM; p++) {
            if (heldPlayers >> p & 1) a7_SNDi_Work.player[p].pause_flag = FALSE;
        }
        sMusicCh = PlayerChannels(musicPlayers); /* after the sequencer: it links new notes' channels to their tracks */
        u32 stillCh = clock == CLOCK_MUSIC ? ~sMusicCh : sMusicCh;
        for (int c = 0; c < SND_CHANNEL_NUM; c++) {
            SNDExChannel *ch = &a7_SNDi_Work.channel[c];
            if ((stillCh >> c & 1) && ch->active_flag) {
                ch->active_flag = FALSE;
                heldCh |= 1u << c;
            }
        }
        a7_SND_ExChannelMain(TRUE);
        for (int c = 0; c < SND_CHANNEL_NUM; c++) {
            if (heldCh >> c & 1) a7_SNDi_Work.channel[c].active_flag = TRUE;
        }
    } else {
        a7_SND_SeqMain(periodic);
        sMusicCh = PlayerChannels(musicPlayers);
        a7_SND_ExChannelMain(periodic);
    }
    a7_SND_UpdateSharedWork();
    (void)a7_SND_CalcRandom();
    sInTick = FALSE;
}

void a7_PXI_SetFifoRecvCallback(int fifotag, PXIFifoCallback callback) {
    if (fifotag == PXI_FIFO_TAG_SOUND) sArm7Recv = callback;
}

int a7_PXI_SendWordByFifo(int fifotag, u32 data, BOOL err) { /* ARM7 -> ARM9 (sound alarms) */
    pc_pxi_to_arm9(fifotag, data, err);
    return PXI_FIFO_SUCCESS;
}

/* ARM9 -> ARM7: a command list address, or a request to process commands now. */
void pc_snd_pxi_from_arm9(u32 data) {
    if (sArm7Recv != NULL) sArm7Recv(PXI_FIFO_TAG_SOUND, data, FALSE);
}

/* The ARM9 asks for its commands to run now (it may be spinning until they finish), so run a
 * non-periodic pass like the ARM7 thread's wakeup message does. */
BOOL a7_SND_SendWakeupMessage(void) {
    if (!sInTick) Tick(FALSE, CLOCK_ALL);
    return TRUE;
}

void a7_SND_StartIntervalTimer(void) {}
void a7_SND_StopIntervalTimer(void) {}
void a7_SNDi_LockMutex(void) {}
void a7_SNDi_UnlockMutex(void) {}
void a7_PMi_ResetControl(int x) { (void)x; }
void a7_PMi_SetControl(int x) { (void)x; }
void a7_SVC_ResetSoundBias(u16 x) { (void)x; }
void a7_SVC_SetSoundBias(u16 x) { (void)x; }

/* ---- Mixer ---- */

typedef struct {
    BOOL active;
    BOOL restart; /* started (again) since the last mix */
    u32 pos, frac; /* sample index, 16.16 fraction */
    s32 adpcmSample, adpcmIndex;
    u32 adpcmPos; /* samples decoded so far */
    s32 loopSample, loopIndex;
    u16 lfsr;
    s16 noiseOut;
} Chan;

static Chan sCh[16];

void pc_snd7_start_channel(int chNo) {
    M32(CH_REG(chNo)) |= CNT_E;
    sCh[chNo].restart = TRUE;
}

static const s8 kAdpcmIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static const u16 kAdpcmStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767};

static void AdpcmStep(Chan *c, u32 sad, u32 loopStart) {
    if (c->adpcmPos == loopStart) {
        c->loopSample = c->adpcmSample;
        c->loopIndex = c->adpcmIndex;
    }
    u32 n = (M8(sad + 4 + (c->adpcmPos >> 1)) >> ((c->adpcmPos & 1) * 4)) & 0xF;
    s32 step = kAdpcmStep[c->adpcmIndex];
    s32 diff = step >> 3;
    if (n & 1) diff += step >> 2;
    if (n & 2) diff += step >> 1;
    if (n & 4) diff += step;
    c->adpcmSample = (n & 8) ? MAX(c->adpcmSample - diff, -0x7FFF) : MIN(c->adpcmSample + diff, 0x7FFF);
    c->adpcmIndex = MIN(MAX(c->adpcmIndex + kAdpcmIndex[n & 7], 0), 88);
    c->adpcmPos++;
}

static void StartChan(Chan *c, u32 reg) {
    u32 sad = M32(reg + 4) & ~3u; /* full 32 bits: sound data can sit in host memory above the DS map */
    memset(c, 0, sizeof *c);
    c->active = TRUE;
    c->adpcmSample = MS16(sad);
    c->adpcmIndex = MIN(M8(sad + 2) & 0x7F, 88);
    c->lfsr = 0x7FFF;
}

/* One output sample of channel c, then advance by step (16.16 source samples per output sample). */
static s32 ChanSample(int ch, Chan *c, u32 reg, u32 cnt, u32 step) {
    u32 format = (cnt >> 29) & 3, sad = M32(reg + 4) & ~3u;
    u32 pnt = M16(reg + 0xA), len = M32(reg + 0xC) & 0x3FFFFF;
    s32 s;
    if (format == 3) { /* PSG (8-13) / noise (14-15); other channels are silent in this mode */
        if (ch >= 8 && ch <= 13) {
            u32 duty = (cnt >> 24) & 7;
            s = duty != 7 && (c->pos & 7) <= duty ? 0x7FFF : -0x7FFF;
        } else if (ch >= 14) {
            s = c->noiseOut;
        } else {
            s = 0;
        }
        c->frac += step;
        for (u32 n = c->frac >> 16; n; n--) {
            c->pos++;
            if (ch >= 14) {
                BOOL carry = c->lfsr & 1;
                c->lfsr >>= 1;
                if (carry) c->lfsr ^= 0x6000;
                c->noiseOut = carry ? -0x7FFF : 0x7FFF;
            }
        }
        c->frac &= 0xFFFF;
        return s;
    }
    u32 total, loopStart;
    switch (format) {
    case 0: total = (pnt + len) * 4; loopStart = pnt * 4; s = (s32)MS8(sad + c->pos) << 8; break;
    case 1: total = (pnt + len) * 2; loopStart = pnt * 2; s = MS16(sad + c->pos * 2); break;
    default: /* IMA-ADPCM: 4-byte header word (counted in pnt), then nibbles */
        total = pnt + len > 0 ? (pnt + len - 1) * 8 : 0;
        loopStart = pnt > 0 ? (pnt - 1) * 8 : 0;
        while (c->adpcmPos <= c->pos && c->adpcmPos < total) AdpcmStep(c, sad, loopStart);
        s = c->adpcmSample;
        break;
    }
    c->frac += step;
    c->pos += c->frac >> 16;
    c->frac &= 0xFFFF;
    if (c->pos >= total) {
        u32 repeat = (cnt >> 27) & 3;
        u32 loopLen = total - loopStart;
        if (repeat == 2 || loopLen == 0) { /* one-shot: the channel stops itself */
            c->active = FALSE;
            M32(reg) &= ~CNT_E;
        } else {
            c->pos = loopStart + (c->pos - total) % loopLen;
            if (format == 2) { /* back to the decoder state saved at the loop start */
                c->adpcmSample = c->loopSample;
                c->adpcmIndex = c->loopIndex;
                c->adpcmPos = loopStart;
            }
        }
    }
    return s;
}

static s16 sMix[2 * 1024];
static int sMixCount;

static void Mix(int samples) {
    static const u8 kShift[4] = {0, 1, 2, 4};
    u32 soundcnt = M16(REG7 + 0x500);
    s32 master = soundcnt & 0x7F;
    if (master == 127) master = 128;
    if (!(soundcnt & 0x8000)) master = 0;
    u32 steps[16];
    for (int ch = 0; ch < 16; ch++) {
        u32 reg = CH_REG(ch), cnt = M32(reg);
        Chan *c = &sCh[ch];
        if (!(cnt & CNT_E)) c->active = FALSE;
        else if (c->restart || !c->active) StartChan(c, reg);
        c->restart = FALSE;
        u32 tmr = M16(reg + 8);
        /* the channel timer counts at ARM7_HZ/2: source samples per output sample = 512 / (0x10000 - tmr) */
        steps[ch] = (u32)(((u64)512 << 16) / (0x10000u - tmr));
        if (sEffectRate != 1 && !(sMusicCh >> ch & 1)) steps[ch] = (u32)(steps[ch] * sEffectRate); /* effects follow the game speed */
    }
    for (int i = 0; i < samples && sMixCount < 1024; i++) {
        s32 l = 0, r = 0;
        for (int ch = 0; ch < 16; ch++) {
            Chan *c = &sCh[ch];
            if (!c->active) continue;
            u32 reg = CH_REG(ch), cnt = M32(reg);
            s32 vol = cnt & 0x7F, pan = (cnt >> 16) & 0x7F;
            if (vol == 127) vol = 128;
            if (pan == 127) pan = 128;
            s32 s = (ChanSample(ch, c, reg, cnt, steps[ch]) * vol >> 7) >> kShift[(cnt >> 8) & 3];
            if (sEffectMute && !(sMusicCh >> ch & 1)) continue; /* still advanced, so it ends on time */
            l += s * (128 - pan) >> 7;
            r += s * pan >> 7;
        }
        /* master volume, then the DS's 10-bit output stage (one full channel is 1/4 of full scale) */
        l = (l * master >> 7) >> 2;
        r = (r * master >> 7) >> 2;
        sMix[sMixCount * 2] = (s16)MIN(MAX(l, -32768), 32767);
        sMix[sMixCount * 2 + 1] = (s16)MIN(MAX(r, -32768), 32767);
        sMixCount++;
    }
}

/* ---- Output ---- */

static FILE *sWav;
static u32 sWavBytes;

static void WavHeader(void) {
    u32 hdr[11] = {0x46464952, 36 + sWavBytes, 0x45564157, 0x20746D66, 16, 0x00020001, OUT_HZ, OUT_HZ * 4,
                   0x00100004, 0x61746164, sWavBytes};
    fseek(sWav, 0, SEEK_SET);
    fwrite(hdr, sizeof hdr, 1, sWav);
    fseek(sWav, 0, SEEK_END);
}

static void WavClose(void) {
    WavHeader();
    fclose(sWav);
}

#ifdef PC_HAVE_SDL
static SDL_AudioStream *sStream;
#endif

void pc_snd_init(void) {
    a7_SND_CommandInit();
    a7_SND_ExChannelInit();
    a7_SND_SeqInit();
    a7_SND_AlarmInit();
    a7_SND_Enable();
    a7_SND_SetOutputSelector(SND_OUTPUT_MIXER, SND_OUTPUT_MIXER, SND_CHANNEL_OUT_MIXER, SND_CHANNEL_OUT_MIXER);
    a7_SND_SetMasterVolume(SND_MASTER_VOLUME_MAX);

    const char *wav = getenv("HG_WAV");
    if (wav != NULL && *wav && (sWav = fopen(wav, "wb")) != NULL) {
        WavHeader();
        atexit(WavClose);
    }
#ifdef PC_HAVE_SDL
    if (pc_video_windowed()) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16LE, 2, OUT_HZ};
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256"); /* small device buffer: menu sounds feel late otherwise */
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO) ||
            (sStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL)) == NULL) {
            fprintf(stderr, "hg_pc: no audio (%s)\n", SDL_GetError());
        } else {
            SDL_ResumeAudioStreamDevice(sStream);
            SDL_AudioSpec dev;
            int devFrames = 0;
            SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(sStream), &dev, &devFrames);
            fprintf(stderr, "hg_pc: audio via %s, device %d Hz, %d-sample buffer\n", SDL_GetCurrentAudioDriver(), dev.freq, devFrames);
            pc_snd_set_volume(pc_cfg_volume());
        }
    }
#endif
}

void pc_snd_set_volume(int percent) {
#ifdef PC_HAVE_SDL
    if (sStream != NULL) SDL_SetAudioStreamGain(sStream, percent / 100.0f);
#else
    (void)percent;
#endif
}

static u32 ActiveChannels(void) {
    u32 mask = 0;
    for (int c = 0; c < SND_CHANNEL_NUM; c++) mask |= (u32)a7_SNDi_Work.channel[c].active_flag << c;
    return mask;
}

/* Runs the driver ticks that fall in `cycles` of ARM7 time, mixing between them. */
static void Advance(u32 cycles, int clock) {
    static u32 sCycles, sSampleCycles;
    for (sCycles += cycles; sCycles >= CYCLES_PER_TICK; sCycles -= CYCLES_PER_TICK) {
        Tick(TRUE, clock);
        sSampleCycles += CYCLES_PER_TICK;
        Mix(sSampleCycles / CYCLES_PER_SAMPLE);
        sSampleCycles %= CYCLES_PER_SAMPLE;
    }
}

static void Flush(void) {
    if (sWav != NULL) {
        fwrite(sMix, 4, sMixCount, sWav);
        sWavBytes += sMixCount * 4;
    }
#ifdef PC_HAVE_SDL
    if (sStream != NULL) SDL_PutAudioStreamData(sStream, sMix, sMixCount * 4);
#endif
    sMixCount = 0;
}

/* Once per game frame.
 * Windowed: music ticks run as the audio device plays, keeping just enough queued to last until the
 * next game frame: about 1.5 frame gaps, at least 1.25 frames at 60 Hz (less starves on WSLg). This
 * frame's worth of effect ticks is spread evenly between them. Sounds the game starts still start on
 * its frame.
 * Headless: one frame of driver time per game frame, so runs (HG_WAV, regression dumps) repeat exactly. */
void pc_snd_frame(void) {
    sMixCount = 0;
#ifdef PC_HAVE_SDL
    if (sStream != NULL) {
        static Uint64 last;
        static float gap = 16.67e6f; /* ns between calls, smoothed */
        static u32 sEffectCycles, sEffectBusy; /* busy: music ticks with an effect channel sounding (HG_SND_LOG) */
        Uint64 now = SDL_GetTicksNS();
        if (last != 0) gap = gap * 0.9f + MIN((float)(now - last), 100e6f) * 0.1f;
        last = now;
        float speed = pc_cfg_speed();
        sEffectMute = speed == 0;
        sEffectRate = speed > 0 ? speed : 1;
        sEffectCycles += CYCLES_PER_FRAME;
        int effectTicks = (int)(sEffectCycles / CYCLES_PER_TICK);
        sEffectCycles %= CYCLES_PER_TICK;
        const float target = MAX(1.25f * 16.67e6f, 1.5f * gap) * (OUT_HZ * 4 / 1e9f); /* bytes */
        const float tickBytes = (float)CYCLES_PER_TICK / CYCLES_PER_SAMPLE * 4;
        int queued = SDL_GetAudioStreamQueued(sStream);
        int musicTicks = queued < target ? (int)((target - queued) / tickBytes) + 1 : 0;
        for (int i = 0, done = 0; i < musicTicks || done < effectTicks; i++) {
            int due = musicTicks > 0 ? effectTicks * (i + 1) / musicTicks : effectTicks;
            for (; done < due && done < effectTicks; done++) Tick(TRUE, CLOCK_EFFECTS);
            if (i >= musicTicks) continue;
            Advance(CYCLES_PER_TICK, CLOCK_MUSIC);
            if (sMixCount > 512) Flush(); /* sMix holds 1024 */
            sEffectBusy += (ActiveChannels() & ~sMusicCh) != 0;
        }
        Flush();
        static int sLog = -1, sMusicTicks, sEffectTicks;
        static Uint64 sLogAt;
        if (sLog < 0) sLog = getenv("HG_SND_LOG") != NULL;
        sMusicTicks += musicTicks;
        sEffectTicks += effectTicks;
        if (sLog && now - sLogAt >= 1000000000u) { /* HG_SND_LOG: the two clocks, once a second */
            fprintf(stderr, "hg_pc: snd: %d music ticks, %d effect ticks, effects sounding %d ms; music players %04x channels %04x\n",
                    sMusicTicks, sEffectTicks, (int)(sEffectBusy * 1000.0f * CYCLES_PER_TICK / ARM7_HZ), PC_SndMusicPlayers(), sMusicCh);
            sMusicTicks = sEffectTicks = sEffectBusy = 0;
            sLogAt = now;
        }
        return;
    }
#endif
    Advance(CYCLES_PER_FRAME, CLOCK_ALL);
    Flush();
}
