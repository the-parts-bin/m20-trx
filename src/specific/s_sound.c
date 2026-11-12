// m20: the sound effects, mixed in software (in place of DirectSound's
// buffers): the platform asks S_Sound_Mix for signed 16-bit stereo at
// M20_AUDIO_RATE. The samples stay where the level loaded them (WAVE data
// in the game's memory); a channel plays one at its rate times the pitch,
// with DirectSound's volume and pan curves (TombATI's decibel table) as two
// linear gains.
#include "specific/s_sound.h"

#include "global/vars.h"
#include "log.h"
#include "m20/m20_platform.h"
#include "memory.h"
#include "specific/s_shell.h"

#include <math.h>
#include <string.h>

#define DECIBEL_LUT_SIZE 512
#define NUM_CHANNELS 32

void M20_AudioLock(bool lock);

typedef struct SAMPLE_DATA {
    const char *data;
    int32_t length;     // frames
    int16_t bits_per_sample;
    int16_t channels;
    int32_t sample_rate;
} SAMPLE_DATA;

typedef struct CHANNEL {
    const SAMPLE_DATA *sample;
    uint32_t pos;       // frame, 16.16
    uint32_t step;
    int32_t gain_l;     // 0 .. 4096
    int32_t gain_r;
    bool loop;
    volatile bool playing;
    uint32_t gen;       // bumped at each play: stale handles miss
} CHANNEL;

#pragma pack(push, 1)
typedef struct WAVE_FORMAT_CHUNK {
    char subchunk_id[4];
    int32_t subchunk_size;
    int16_t audio_format;
    int16_t num_channels;
    int32_t sample_rate;
    int32_t byte_rate;
    int16_t block_align;
    int16_t bits_per_sample;
} WAVE_FORMAT_CHUNK;

typedef struct WAVE_DATA_CHUNK {
    char subchunk_id[4];
    int32_t subchunk_size;
} WAVE_DATA_CHUNK;

typedef struct WAVE_FILE_HEADER {
    char chunk_id[4];
    int32_t chunk_size;
    char format[4];
    WAVE_FORMAT_CHUNK fmt_chunk;
    WAVE_DATA_CHUNK data_chunk;
} WAVE_FILE_HEADER;
#pragma pack(pop)

static int32_t m_NumSampleData = 0;
static SAMPLE_DATA *m_SampleData = NULL;
static CHANNEL m_Channels[NUM_CHANNELS];
static uint16_t m_GainLUT[DECIBEL_LUT_SIZE];   // 4096 = 1.0
static bool m_Ready;
static uint32_t m_Gen;

// A handle is the channel's index and its generation, never NULL: the game
// keeps handles after a one-shot ends, and the channel may then play
// something else, which a stale handle must not stop or pan.
static void *MakeHandle(int i, uint32_t gen)
{
    return (void *)(uintptr_t)((gen << 6) | (uint32_t)(i + 1));
}

static CHANNEL *FromHandle(void *handle)
{
    uintptr_t h = (uintptr_t)handle;
    int i = (int)(h & 63) - 1;
    if (!handle || i < 0 || i >= NUM_CHANNELS) {
        return NULL;
    }
    CHANNEL *ch = &m_Channels[i];
    return ch->playing && ch->gen == (uint32_t)(h >> 6) ? ch : NULL;
}

// TombATI's decibels (hundredths): -9000 + 1000 log2(i), as a linear gain
static void BuildGainLUT()
{
    m_GainLUT[0] = 0;
    for (int i = 1; i < DECIBEL_LUT_SIZE; i++) {
        double db = -9000.0 + 1000.0 * log2((double)i);
        double g = pow(10.0, db / 2000.0) * 4096.0;
        m_GainLUT[i] = g > 4096.0 ? 4096 : (uint16_t)g;
    }
}

static void SetGains(CHANNEL *ch, int32_t volume, uint16_t pan)
{
    int32_t g = m_GainLUT[(volume & 0x7FFF) >> 6];
    int32_t p = sin((pan / 32767.0) * M_PI) * (DECIBEL_LUT_SIZE / 2);
    int32_t l = g, r = g;
    if (p > 0) {
        l = l * m_GainLUT[DECIBEL_LUT_SIZE - p] >> 12;
    } else if (p < 0) {
        r = r * m_GainLUT[DECIBEL_LUT_SIZE + p] >> 12;
    }
    ch->gain_l = l;
    ch->gain_r = r;
}

bool S_Sound_Init()
{
    BuildGainLUT();
    memset(m_Channels, 0, sizeof(m_Channels));
    m_Ready = true;
    M20_AudioInit();
    return true;
}

void S_Sound_LoadSamples(char **sample_pointers, int32_t num_samples)
{
    if (!SoundIsActive) {
        return;
    }
    M20_AudioLock(true);
    for (int i = 0; i < NUM_CHANNELS; i++) {
        m_Channels[i].playing = false;
    }
    M20_AudioLock(false);
    if (m_SampleData) {
        Memory_Free(m_SampleData);
    }
    m_NumSampleData = num_samples;
    m_SampleData = Memory_Alloc(sizeof(SAMPLE_DATA) * num_samples);
    for (int i = 0; i < num_samples; i++) {
        const WAVE_FILE_HEADER *hdr = (const WAVE_FILE_HEADER *)sample_pointers[i];
        if (strncmp(hdr->chunk_id, "RIFF", 4)) {
            S_Shell_ExitSystem("Samples must be in WAVE format.");
        }
        SAMPLE_DATA *s = &m_SampleData[i];
        s->data = sample_pointers[i] + sizeof(WAVE_FILE_HEADER);
        s->bits_per_sample = hdr->fmt_chunk.bits_per_sample;
        s->channels = hdr->fmt_chunk.num_channels;
        s->sample_rate = hdr->fmt_chunk.sample_rate;
        // TombATI's length (the data chunk's size less the header's)
        int32_t bytes = hdr->data_chunk.subchunk_size - sizeof(WAVE_FILE_HEADER);
        int32_t align = s->channels * s->bits_per_sample / 8;
        s->length = bytes > 0 && align > 0 ? bytes / align : 0;
    }
}

void *S_Sound_PlaySample(
    int32_t sample_id, int32_t volume, int16_t pitch, uint16_t pan, bool loop)
{
    if (!SoundIsActive || !m_Ready || sample_id < 0
        || sample_id >= m_NumSampleData) {
        return NULL;
    }
    const SAMPLE_DATA *s = &m_SampleData[sample_id];
    if (!s->length) {
        return NULL;
    }
    int idx = -1;
    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (!m_Channels[i].playing) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return NULL;
    }
    CHANNEL *ch = &m_Channels[idx];
    int32_t rate = s->sample_rate;
    if (pitch != -1) {
        rate = rate * pitch / 100;
    }
    M20_AudioLock(true);
    ch->sample = s;
    ch->pos = 0;
    ch->step = (uint32_t)(((uint64_t)rate << 16) / M20_AUDIO_RATE);
    ch->loop = loop;
    SetGains(ch, volume, pan);
    m_Gen = (m_Gen + 1) & 0x3FFFFFF;
    ch->gen = m_Gen;
    ch->playing = true;
    M20_AudioLock(false);
    return MakeHandle(idx, m_Gen);
}

bool S_Sound_SampleIsPlaying(void *handle)
{
    if (!SoundIsActive) {
        return false;
    }
    M20_AudioLock(true);
    bool playing = FromHandle(handle) != NULL;
    M20_AudioLock(false);
    return playing;
}

void S_Sound_StopAllSamples()
{
    M20_AudioLock(true);
    for (int i = 0; i < NUM_CHANNELS; i++) {
        m_Channels[i].playing = false;
    }
    M20_AudioLock(false);
}

void S_Sound_StopSample(void *handle)
{
    M20_AudioLock(true);
    CHANNEL *ch = FromHandle(handle);
    if (ch) {
        ch->playing = false;
    }
    M20_AudioLock(false);
}

void S_Sound_SetPanAndVolume(void *handle, int16_t pan, int16_t volume)
{
    if (!SoundIsActive || !handle) {
        return;
    }
    M20_AudioLock(true);
    CHANNEL *ch = FromHandle(handle);
    if (ch) {
        SetGains(ch, volume, pan);
    }
    M20_AudioLock(false);
}

// The mix: frames of stereo, signed 16-bit
void S_Sound_Mix(int16_t *out, int frames)
{
    static int32_t acc[2 * 1024];
    while (frames > 0) {
        int n = frames > 1024 ? 1024 : frames;
        memset(acc, 0, n * 2 * sizeof(int32_t));
        for (int c = 0; c < NUM_CHANNELS; c++) {
            CHANNEL *ch = &m_Channels[c];
            if (!ch->playing) {
                continue;
            }
            const SAMPLE_DATA *s = ch->sample;
            uint32_t len = (uint32_t)s->length << 16;
            for (int i = 0; i < n; i++) {
                if (ch->pos >= len) {
                    if (!ch->loop) {
                        ch->playing = false;
                        break;
                    }
                    ch->pos -= len;
                }
                uint32_t f = ch->pos >> 16;
                int32_t v;
                if (s->bits_per_sample == 16) {
                    v = ((const int16_t *)s->data)[f * s->channels];
                } else {
                    v = ((int32_t)(uint8_t)s->data[f * s->channels] - 128) << 8;
                }
                acc[2 * i] += v * ch->gain_l >> 12;
                acc[2 * i + 1] += v * ch->gain_r >> 12;
                ch->pos += ch->step;
            }
        }
        for (int i = 0; i < 2 * n; i++) {
            int32_t v = acc[i];
            out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        }
        out += 2 * n;
        frames -= n;
    }
}
