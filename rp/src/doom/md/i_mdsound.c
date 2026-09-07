/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Based on rp2040-doom's pico/i_picosound.c:
 * Copyright(C) 1993-1996 Id Software, Inc.
 * Copyright(C) 2005-2014 Simon Howard
 * Copyright(C) 2021-2022 Graham Sanderson
 * GPL-2.0-or-later.
 *
 * File: i_mdsound.c
 * Description: Doom's sound-effect module on the MD framework. Keeps
 *              upstream's channel model (ADPCM blocks decoded 249
 *              samples at a time, 16.16 resampling, optional low-pass)
 *              and replaces the I2S buffer pool with the framework's
 *              per-VBL fill callback: mono 8-bit, at whichever rate and
 *              length the m68k asks for (STE DMA PCM or YM volume pairs).
 *              Mixing runs on Core 1 while it is otherwise idle, so it
 *              keeps up regardless of the frame rate. No music.
 */
#include "config.h"

#include <string.h>
#include <assert.h>
#include <doom/sounds.h>
#include <z_zone.h>

#include "deh_str.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"

#include "doomtype.h"
#include "i_picosound.h"
#include "audio.h"
#include "doom_sound.h"
#include "hardware/regs/addressmap.h"

#define ADPCM_BLOCK_SIZE 128

/* MD/DOOM: output rate follows the detected back-end. */
static uint32_t s_out_rate = DOOM_SOUND_RATE_DMA;
static audio_mode_t s_mode_for_steps = AUDIO_MODE_SILENT;
#define PICO_SOUND_SAMPLE_FREQ s_out_rate
#define ADPCM_SAMPLES_PER_BLOCK_SIZE 249
#define LOW_PASS_FILTER
#define MIX_MAX_VOLUME 128
typedef struct channel_s channel_t;


struct channel_s
{
    const uint8_t *data;
    const uint8_t *data_end;
    uint32_t offset;
    uint32_t step;
    uint16_t sample_freq; // MD/DOOM: for re-deriving step on a back-end change
    uint8_t left, right; // 0-255
    uint8_t decompressed_size;
#if SOUND_LOW_PASS
    uint8_t alpha256;
#endif
    int8_t decompressed[ADPCM_SAMPLES_PER_BLOCK_SIZE];
};


// ====== FROM ADPCM-LIB =====
#define CLIP(data, min, max) \
if ((data) > (max)) data = max; \
else if ((data) < (min)) data = min;

/* step table */
static const uint16_t step_table[89] = {
        7, 8, 9, 10, 11, 12, 13, 14,
        16, 17, 19, 21, 23, 25, 28, 31,
        34, 37, 41, 45, 50, 55, 60, 66,
        73, 80, 88, 97, 107, 118, 130, 143,
        157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658,
        724, 796, 876, 963, 1060, 1166, 1282, 1411,
        1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
        3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
        7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767
};

/* step index tables */
static const int index_table[] = {
        /* adpcm data size is 4 */
        -1, -1, -1, -1, 2, 4, 6, 8
};
// =============================


static boolean sound_initialized = false;
static channel_t channels[NUM_SOUND_CHANNELS];

static boolean use_sfx_prefix;

static inline bool is_channel_playing(int channel) {
    return channels[channel].decompressed_size != 0;
}

static inline void stop_channel(int channel) {
    channels[channel].decompressed_size = 0;
}

static bool check_and_init_channel(int channel) {
    return sound_initialized && ((uint)channel) < NUM_SOUND_CHANNELS;
}

int adpcm_decode_block_s8(int8_t *outbuf, const uint8_t *inbuf, int inbufsize)
{
#if 1
    int samples = 1, chunks;

    if (inbufsize < 4)
        return 0;

    int32_t pcmdata = (int16_t) (inbuf [0] | (inbuf [1] << 8));
    *outbuf++ = pcmdata>>8u;
    int index = inbuf[2];

    if (index < 0 || index > 88 || inbuf [3])     // sanitize the input a little...
        return 0;

    inbufsize -= 4;
    inbuf += 4;

    chunks = inbufsize / 4;
    samples += chunks * 8;

    while (chunks--) {
        for (int i = 0; i < 4; ++i) {
            int step = step_table[index], delta = step >> 3;

            if (*inbuf & 1) delta += (step >> 2);
            if (*inbuf & 2) delta += (step >> 1);
            if (*inbuf & 4) delta += step;
            if (*inbuf & 8) delta = -delta;

            pcmdata += delta;
            index += index_table [*inbuf & 0x7];
            CLIP(index, 0, 88);
            CLIP(pcmdata, -32768, 32767);
            outbuf [i * 2] = pcmdata>>8u;

            step = step_table[index], delta = step >> 3;

            if (*inbuf & 0x10) delta += (step >> 2);
            if (*inbuf & 0x20) delta += (step >> 1);
            if (*inbuf & 0x40) delta += step;
            if (*inbuf & 0x80) delta = -delta;

            pcmdata += delta;
            index += index_table[(*inbuf >> 4) & 0x7];
            CLIP(index, 0, 88);
            CLIP(pcmdata, -32768, 32767);
            outbuf [i * 2 + 1] = pcmdata>>8u;
            inbuf++;
        }

        outbuf += 8;
    }

    return samples;
#else
    extern int adpcm_decode_block (int16_t *outbuf, const uint8_t *inbuf, size_t inbufsize, int channels);
    static int16_t tmp[ADPCM_SAMPLES_PER_BLOCK_SIZE];
    int samples = adpcm_decode_block(tmp, inbuf, inbufsize, 1);
    for(int s=0;s<samples;s++) {
        outbuf[s] = tmp[s] / 256;
    }
    return samples;
#endif
}

static void decompress_buffer(channel_t *channel) {
    if (channel->data == channel->data_end) {
        channel->decompressed_size = 0;
    } else {
        int block_size = MIN(ADPCM_BLOCK_SIZE, channel->data_end - channel->data);
        channel->decompressed_size = adpcm_decode_block_s8(channel->decompressed, channel->data, block_size);
        assert(channel->decompressed_size && channel->decompressed_size <= sizeof(channel->decompressed));
        channel->data += block_size;
    }
}

static boolean init_channel_for_sfx(channel_t *ch, const sfxinfo_t *sfxinfo, int pitch)
{
    int lumpnum = sfx_mut(sfxinfo)->lumpnum;
    int lumplen = W_LumpLength(lumpnum);

    const uint8_t *data = W_CacheLumpNum(lumpnum, PU_STATIC); // we don't track because we assume in ROWAD anyway

    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x80) // note 0x80 i.e. only support compressed right now
    {
        return false;
    }

    // 16 bit sample rate field, 32 bit length field

//    int length = (data[7] << 24) | (data[6] << 16) | (data[5] << 8) | data[4];
//    length -= 40; // 8 for header, 32 because we didn't updated it in lump converter (which cuts of unused 16 bit leading/leadout)
//    if (length <= 0) {
//        return false;
//    }
    int length = lumplen - 8;
//    printf("channel %d lump %d size %d at %p len2 %d\n", (int)(ch-channels), lumpnum, lumplen, data, length);

    ch->data = data + 8;
    ch->data_end = ch->data + length;

    uint32_t sample_freq = (data[3] << 8) | data[2];
    ch->sample_freq = (uint16_t)sample_freq;
    if (pitch == NORM_PITCH)
        ch->step = sample_freq * 65536 / PICO_SOUND_SAMPLE_FREQ;
    else
        ch->step = (uint32_t)((sample_freq * pitch) * 65536ull / (PICO_SOUND_SAMPLE_FREQ * pitch));

    decompress_buffer(ch); // we need non-zero decompressed size if playing
    ch->offset = 0;

#if SOUND_LOW_PASS
//    const float dt = 1.0f / PICO_SOUND_SAMPLE_FREQ;
//    const float rc = 1.0f / (3.14f * sample_freq);
//    const float alpha = dt / (rc + dt);
//    ch->alpha256 = (int)(256*alpha);
    ch->alpha256 = 256u * 201u * sample_freq / (201u * sample_freq + 64u * (uint)PICO_SOUND_SAMPLE_FREQ);
#endif
    return true;
}

static void GetSfxLumpName(const sfxinfo_t *sfx, char *buf, size_t buf_len)
{
    // Linked sfx lumps? Get the lump number for the sound linked to.
    if (sfx->link != NULL)
    {
        sfx = sfx->link;
    }

    // Doom adds a DS* prefix to sound lumps; Heretic and Hexen don't
    // do this.

    if (use_sfx_prefix)
    {
        M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
    }
    else
    {
        M_StringCopy(buf, DEH_String(sfx->name), buf_len);
    }
}

static void I_Pico_PrecacheSounds(should_be_const sfxinfo_t *sounds, int num_sounds)
{
    // no-op
}

static int I_Pico_GetSfxLumpNum(should_be_const sfxinfo_t *sfx)
{
    char namebuf[9];
    GetSfxLumpName(sfx, namebuf, sizeof(namebuf));
    return W_GetNumForName(namebuf);
}

static void I_Pico_UpdateSoundParams(int handle, int vol, int sep)
{
    int left, right;

    if (!sound_initialized || handle < 0 || handle >= NUM_SOUND_CHANNELS)
    {
        return;
    }

    // todo graham seems unnecessary
    left = ((254 - sep) * vol) / 127;
    right = ((sep) * vol) / 127;

    if (left < 0) left = 0;
    else if ( left > 255) left = 255;
    if (right < 0) right = 0;
    else if (right > 255) right = 255;

    channels[handle].left = left;
    channels[handle].right = right;
}

static int I_Pico_StartSound(should_be_const sfxinfo_t *sfxinfo, int channel, int vol, int sep, int pitch)
{
    if (!check_and_init_channel(channel)) return -1;

    stop_channel(channel);
    channel_t *ch = &channels[channel];
    if (!init_channel_for_sfx(ch, sfxinfo, pitch)) {
        assert(!is_channel_playing(channel)); // don't expect to have to mark it sotpped
    }
    I_Pico_UpdateSoundParams(channel, vol, sep);
    return channel;
}

static void I_Pico_StopSound(int channel)
{
    if (check_and_init_channel(channel)) {

    }
}

static boolean I_Pico_SoundIsPlaying(int channel)
{
    if (!check_and_init_channel(channel)) return false;
    return is_channel_playing(channel);
}

/* MD/DOOM: the engine's polled update is a no-op; the mix happens in the
 * fill callback below, driven by the framework's audio pacing. */
static void I_Pico_UpdateSound(void)
{
}

/* Per-VBL fill: mix every playing channel onto `bytes` of output in the
 * detected back-end's format. Runs on Core 1 (see fb_core1_loop). */
void I_MD_SoundFill(uint8_t *buf, uint32_t bytes)
{
    const audio_mode_t mode = audio_get_mode();
    if (!sound_initialized || mode == AUDIO_MODE_SILENT) {
        memset(buf, 0, bytes);
        return;
    }
    const bool ym = (mode == AUDIO_MODE_YM);
    const uint32_t nsamp = ym ? bytes / 2u : bytes;
    if (mode != s_mode_for_steps) {
        s_out_rate = ym ? DOOM_SOUND_RATE_YM : DOOM_SOUND_RATE_DMA;
        for (int ch = 0; ch < NUM_SOUND_CHANNELS; ch++) {
            if (is_channel_playing(ch) && channels[ch].sample_freq) {
                channels[ch].step = channels[ch].sample_freq * 65536 / s_out_rate;
            }
        }
        s_mode_for_steps = mode;
    }

    /* MD/DOOM: 1 KB at the bottom of the USB DPRAM, which nothing else
     * uses (the renderer's vpatch lists sit at +0x400). */
    int16_t *mix = (int16_t *)USBCTRL_DPRAM_BASE;
    if (nsamp > 512u) return;
    memset(mix, 0, nsamp * sizeof(mix[0]));

    for (int ch = 0; ch < NUM_SOUND_CHANNELS; ch++) {
        if (!is_channel_playing(ch)) continue;
        channel_t *channel = &channels[ch];
        /* Mono: the louder side, so a sound to one side keeps its level. */
        int vol = channel->left > channel->right ? channel->left : channel->right;
        uint offset_end = channel->decompressed_size * 65536;
        if (channel->offset >= offset_end) {
            stop_channel(ch);
            continue;
        }
#if SOUND_LOW_PASS
        int alpha256 = channel->alpha256;
        int beta256 = 256 - alpha256;
        int sample = channel->decompressed[channel->offset >> 16];
#endif
        for (uint32_t s = 0; s < nsamp; s++) {
#if !SOUND_LOW_PASS
            int sample = channel->decompressed[channel->offset >> 16];
#else
            sample = (beta256 * sample + alpha256 * channel->decompressed[channel->offset >> 16]) / 256;
#endif
            mix[s] += (int16_t)((sample * vol) >> 8);
            channel->offset += channel->step;
            if (channel->offset >= offset_end) {
                channel->offset -= offset_end;
                decompress_buffer(channel);
                offset_end = channel->decompressed_size * 65536;
                if (channel->offset >= offset_end) {
                    stop_channel(ch);
                    break;
                }
            }
        }
    }

    for (uint32_t s = 0; s < nsamp; s++) {
        int v = mix[s];
        if (v > 127) v = 127;
        else if (v < -127) v = -127;
        if (ym) {
            const uint8_t k = (uint8_t)((v + 128) >> 2);
            buf[2u * s] = doom_sound_ghost_lut[k][0];
            buf[2u * s + 1u] = doom_sound_ghost_lut[k][1];
        } else {
            buf[s] = (uint8_t)(int8_t)v;
        }
    }
}

static void I_Pico_ShutdownSound(void)
{
    if (!sound_initialized)
    {
        return;
    }
    sound_initialized = false;
}

static boolean I_Pico_InitSound(boolean _use_sfx_prefix)
{
    use_sfx_prefix = _use_sfx_prefix;
    for (int i = 0; i < NUM_SOUND_CHANNELS; i++) stop_channel(i);
    audio_set_fill_callback(I_MD_SoundFill);
    sound_initialized = true;
    return true;
}

/* MD/DOOM: silence everything, e.g. before the pack the samples live in
 * is reprogrammed. */
void I_MD_SoundStopAll(void)
{
    for (int i = 0; i < NUM_SOUND_CHANNELS; i++) stop_channel(i);
}

static snddevice_t sound_pico_devices[] =
{
    SNDDEVICE_SB,
};

sound_module_t sound_md_module =
{
    sound_pico_devices,
    arrlen(sound_pico_devices),
    I_Pico_InitSound,
    I_Pico_ShutdownSound,
    I_Pico_GetSfxLumpNum,
    I_Pico_UpdateSound,
    I_Pico_UpdateSoundParams,
    I_Pico_StartSound,
    I_Pico_StopSound,
    I_Pico_SoundIsPlaying,
    I_Pico_PrecacheSounds,
};


/* MD/DOOM: the renderer toggles this around calls that must not restart a
 * song; with no music module it is just a flag nobody reads. */
uint8_t restart_song_state;

/* MD/DOOM: OPL driver selection is meaningless without the OPL. */
void I_SetOPLDriverVer(opl_driver_ver_t ver) { (void)ver; }
