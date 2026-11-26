// The part of FMOD the game uses, written for this port.
//
// The game was built against FMOD Studio's low-level API (74 functions). The decomp
// has FMOD's headers but not the library, so the functions are implemented here on a
// small software mixer whose output goes to the host's audio device
// (src/host/audio.cpp).
//
// What is real: sounds from Ogg Vorbis files, FMOD sample banks (FSB5, holding
// Vorbis or FMOD's ADPCM), raw PCM and WAV, and "user" streams that the game fills
// itself (video soundtracks); playing, pausing, stopping,
// looping, volume, pitch, channel groups, delays on the mixer clock, and simple
// distance attenuation and left/right placement for 3D sounds.
// What is not: effects (DSP units, reverb), occlusion, doppler, spread and priorities are accepted and
// ignored.
//
// Every sound is decoded to 16-bit samples when it is created, which is simple and
// costs memory; FMOD keeps most of them compressed.
//
// D3_SOUND_STATS=1 prints what kinds of sound the game asked for.
// D3_SOUND_DUMP=<directory> writes the first few decoded bank samples there as WAV.
// D3_NO_SOUND=1 makes start-up report "no sound device", which the game accepts.
#include "Contrib/Contrib/fmodstudiosrc11005/lowlevel_api/src/fmod.h"

#include "Contrib/Contrib/OggVorbis/libogg/include/ogg/ogg.h"
#include "Contrib/Contrib/OggVorbis/libvorbis/include/vorbis/codec.h"

#include <malloc/malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The Vorbis "setup" packets FMOD carries in the game's executable, written out at
// build time by tools/extract_vorbis_setups.py. Sample banks name theirs by id.
extern "C" const struct D3VorbisSetup { unsigned id, size; const unsigned char * data; } d3_vorbis_setups[];
extern "C" const unsigned d3_vorbis_setup_count;

extern "C" int d3_audio_start(int rate, void (*mix)(float * interleavedStereo, int frames));

namespace {

// In this program malloc is the game's, and it draws on heaps of a fixed size that
// were never meant to hold decoded sound. Everything here comes from the host.
void * HostAlloc(size_t size) { return malloc_zone_malloc(malloc_default_zone(), size); }
void * HostZero(size_t size) { return malloc_zone_calloc(malloc_default_zone(), 1, size); }
void * HostRealloc(void * p, size_t size) { return malloc_zone_realloc(malloc_default_zone(), p, size); }
void HostFree(void * p) { malloc_zone_free(malloc_default_zone(), p); }

const int kRate = 48000;
const int kMaxChannels = 256;
// The game sizes its own table of playing sounds by the channel count it passes to
// FMOD_System_Init (128) and indexes it with FMOD_Channel_GetIndex, so no channel
// index may reach that count.
int g_channelLimit = kMaxChannels;

// ---- sounds ----

struct Sound {
    int channels;
    int rate;
    int16_t * pcm;              // interleaved, owned; null for user streams and silence
    unsigned frames;
    FMOD_MODE mode;
    int loopCount;              // -1 forever
    void * userData;
    float defaultFrequency;
    int defaultPriority;
    volatile int state;         // an FMOD_OPENSTATE value
    FMOD_SOUND_PCMREAD_CALLBACK read;       // user streams: the game supplies the samples
    Sound ** subSounds;
    int subSoundCount;
    Sound * parent;
    bool borrowed;              // pcm belongs to the parent
    unsigned loopStart, loopEnd;
};

struct FileSystem {
    FMOD_FILE_OPEN_CALLBACK open;
    FMOD_FILE_CLOSE_CALLBACK close;
    FMOD_FILE_READ_CALLBACK read;
    FMOD_FILE_SEEK_CALLBACK seek;
} g_files;

struct Group {
    float volume;
};

struct Channel {
    Sound * sound;
    Group * group;
    double position;            // in frames of the sound
    float volume, frequency;
    bool playing, paused, is3d;
    int loopsLeft;
    unsigned generation;
    unsigned loopStart, loopEnd;
    unsigned long long delayStart, delayEnd;
    void * userData;
    FMOD_VECTOR where;
    float minDistance, maxDistance, level3d;
    float gainLeft, gainRight;  // from SetMixLevelsOutput, for 2D sounds
};

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
Channel g_channels[kMaxChannels];
Group g_master = {1.0f};
unsigned long long g_clock;     // frames mixed since start: FMOD's "DSP clock"
FMOD_VECTOR g_listener, g_listenerForward = {0, 0, 1}, g_listenerUp = {0, 1, 0};
bool g_started;
int g_softwareRate = kRate;
FMOD_REVERB_PROPERTIES g_reverb[FMOD_REVERB_MAXINSTANCES];

float g_statPeak;
double g_statEnergy;
unsigned long long g_statMixed;
int g_statVoices;
unsigned g_statVorbis, g_statRaw, g_statWav, g_statUser, g_statBankVorbis, g_statBankAdpcm, g_statBankOther, g_statUnknown, g_statFailed, g_statPlayed;

struct Lock {
    Lock() { pthread_mutex_lock(&g_lock); }
    ~Lock() { pthread_mutex_unlock(&g_lock); }
};

// A channel handle is its slot number and a generation, so that a handle the game
// kept for a sound that has finished does not act on whatever plays in the slot now.
FMOD_CHANNEL * Handle(int index)
{
    return reinterpret_cast<FMOD_CHANNEL *>((static_cast<uintptr_t>(g_channels[index].generation) << 12) | static_cast<uintptr_t>(index + 1));
}

Channel * Find(FMOD_CHANNEL * handle)
{
    uintptr_t v = reinterpret_cast<uintptr_t>(handle);
    uintptr_t index = (v & 0xfff) - 1;
    if (index >= kMaxChannels || g_channels[index].generation != (v >> 12))
    {
        return nullptr;
    }
    return &g_channels[index];
}

// ---- reading files through the game's own file callbacks ----

bool ReadAll(const char * name, const FMOD_CREATESOUNDEXINFO * info, uint8_t ** data, unsigned * size)
{
    FMOD_FILE_OPEN_CALLBACK open = info && info->fileuseropen ? info->fileuseropen : g_files.open;
    FMOD_FILE_CLOSE_CALLBACK close = info && info->fileuserclose ? info->fileuserclose : g_files.close;
    FMOD_FILE_READ_CALLBACK read = info && info->fileuserread ? info->fileuserread : g_files.read;
    FMOD_FILE_SEEK_CALLBACK seek = info && info->fileuserseek ? info->fileuserseek : g_files.seek;
    void * user = info ? info->fileuserdata : nullptr;
    if (!open || !read)
    {
        return false;
    }
    unsigned fileSize = 0;
    void * handle = nullptr;
    if (open(name, &fileSize, &handle, user) != FMOD_OK)
    {
        return false;
    }
    unsigned offset = info ? info->fileoffset : 0;
    unsigned want = info && info->length ? info->length : (fileSize > offset ? fileSize - offset : 0);
    if (offset + want > fileSize)
    {
        want = fileSize > offset ? fileSize - offset : 0;
    }
    uint8_t * buffer = static_cast<uint8_t *>(HostAlloc(want ? want : 1));
    unsigned got = 0;
    bool ok = buffer && (!offset || !seek || seek(handle, offset, user) == FMOD_OK);
    while (ok && got < want)
    {
        unsigned n = 0;
        FMOD_RESULT r = read(handle, buffer + got, want - got, &n, user);
        got += n;
        if (r != FMOD_OK || n == 0)
        {
            break;
        }
    }
    if (close)
    {
        close(handle, user);
    }
    if (!ok || got == 0)
    {
        HostFree(buffer);
        return false;
    }
    *data = buffer;
    *size = got;
    return true;
}

// ---- decoding ----

bool DecodeVorbis(const uint8_t * data, unsigned size, Sound * s)
{
    ogg_sync_state sync;
    ogg_stream_state stream;
    ogg_page page;
    ogg_packet packet;
    vorbis_info info;
    vorbis_comment comment;
    vorbis_dsp_state dsp;
    vorbis_block block;
    ogg_sync_init(&sync);
    char * in = ogg_sync_buffer(&sync, size);
    memcpy(in, data, size);
    ogg_sync_wrote(&sync, size);
    vorbis_info_init(&info);
    vorbis_comment_init(&comment);

    bool haveStream = false, ok = true;
    int headers = 0;
    size_t capacity = 0, count = 0;
    int16_t * out = nullptr;
    bool decoding = false;
    while (ok && ogg_sync_pageout(&sync, &page) == 1)
    {
        if (!haveStream)
        {
            ogg_stream_init(&stream, ogg_page_serialno(&page));
            haveStream = true;
        }
        if (ogg_stream_pagein(&stream, &page) < 0)
        {
            continue;       // a page of some other stream
        }
        while (ogg_stream_packetout(&stream, &packet) == 1)
        {
            if (headers < 3)
            {
                if (vorbis_synthesis_headerin(&info, &comment, &packet) < 0)
                {
                    ok = false;
                    break;
                }
                if (++headers == 3)
                {
                    vorbis_synthesis_init(&dsp, &info);
                    vorbis_block_init(&dsp, &block);
                    decoding = true;
                }
                continue;
            }
            if (vorbis_synthesis(&block, &packet) == 0)
            {
                vorbis_synthesis_blockin(&dsp, &block);
            }
            float ** pcm;
            int n;
            while ((n = vorbis_synthesis_pcmout(&dsp, &pcm)) > 0)
            {
                size_t need = count + static_cast<size_t>(n) * info.channels;
                if (need > capacity)
                {
                    capacity = need * 2 + 65536;
                    out = static_cast<int16_t *>(HostRealloc(out, capacity * sizeof(int16_t)));
                }
                for (int i = 0; i < n; i++)
                {
                    for (int c = 0; c < info.channels; c++)
                    {
                        float v = pcm[c][i] * 32767.0f;
                        out[count++] = static_cast<int16_t>(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
                    }
                }
                vorbis_synthesis_read(&dsp, n);
            }
        }
    }
    ok = ok && decoding && count > 0;
    if (ok)
    {
        s->channels = info.channels;
        s->rate = static_cast<int>(info.rate);
        s->pcm = out;
        s->frames = static_cast<unsigned>(count / info.channels);
    }
    else
    {
        HostFree(out);
    }
    if (decoding)
    {
        vorbis_block_clear(&block);
        vorbis_dsp_clear(&dsp);
    }
    if (haveStream)
    {
        ogg_stream_clear(&stream);
    }
    vorbis_comment_clear(&comment);
    vorbis_info_clear(&info);
    ogg_sync_clear(&sync);
    return ok;
}

// Plain samples: 8, 16 or 24 bits a sample, or 32-bit float, to 16-bit.
void ConvertPcm(const uint8_t * data, unsigned size, int format, int channels, int rate, Sound * s)
{
    int bytes = format == FMOD_SOUND_FORMAT_PCM8 ? 1 : format == FMOD_SOUND_FORMAT_PCM24 ? 3
              : (format == FMOD_SOUND_FORMAT_PCM32 || format == FMOD_SOUND_FORMAT_PCMFLOAT) ? 4 : 2;
    channels = channels > 0 ? channels : 1;
    unsigned samples = size / bytes;
    int16_t * out = static_cast<int16_t *>(HostAlloc((samples ? samples : 1) * sizeof(int16_t)));
    for (unsigned i = 0; i < samples; i++)
    {
        const uint8_t * p = data + i * bytes;
        if (bytes == 1)
        {
            out[i] = static_cast<int16_t>((p[0] - 128) << 8);
        }
        else if (bytes == 2)
        {
            out[i] = static_cast<int16_t>(p[0] | p[1] << 8);
        }
        else if (bytes == 3)
        {
            out[i] = static_cast<int16_t>(p[1] | p[2] << 8);
        }
        else if (format == FMOD_SOUND_FORMAT_PCMFLOAT)
        {
            float f;
            memcpy(&f, p, 4);
            out[i] = static_cast<int16_t>(f > 1 ? 32767 : f < -1 ? -32768 : f * 32767);
        }
        else
        {
            out[i] = static_cast<int16_t>(p[2] | p[3] << 8);
        }
    }
    s->pcm = out;
    s->channels = channels;
    s->rate = rate > 0 ? rate : 44100;
    s->frames = samples / channels;
}

bool DecodeWav(const uint8_t * data, unsigned size, Sound * s)
{
    unsigned at = 12;
    int channels = 0, rate = 0, bits = 0, tag = 0;
    while (at + 8 <= size)
    {
        unsigned chunk = data[at + 4] | data[at + 5] << 8 | data[at + 6] << 16 | static_cast<unsigned>(data[at + 7]) << 24;
        const uint8_t * body = data + at + 8;
        if (at + 8 + chunk > size)
        {
            chunk = size - at - 8;
        }
        if (memcmp(data + at, "fmt ", 4) == 0 && chunk >= 16)
        {
            tag = body[0] | body[1] << 8;
            channels = body[2] | body[3] << 8;
            rate = body[4] | body[5] << 8 | body[6] << 16 | body[7] << 24;
            bits = body[14] | body[15] << 8;
        }
        else if (memcmp(data + at, "data", 4) == 0 && channels)
        {
            int format = tag == 3 ? FMOD_SOUND_FORMAT_PCMFLOAT : bits == 8 ? FMOD_SOUND_FORMAT_PCM8
                       : bits == 24 ? FMOD_SOUND_FORMAT_PCM24 : bits == 32 ? FMOD_SOUND_FORMAT_PCM32 : FMOD_SOUND_FORMAT_PCM16;
            if (tag != 1 && tag != 3)
            {
                return false;
            }
            ConvertPcm(body, chunk, format, channels, rate, s);
            return true;
        }
        at += 8 + chunk + (chunk & 1);
    }
    return false;
}

// ---- FMOD sample banks (FSB5) ----

struct Pcm {
    int16_t * data;
    size_t count, capacity;
};

void Drain(vorbis_dsp_state * dsp, int channels, Pcm * o)
{
    float ** pcm;
    int n;
    while ((n = vorbis_synthesis_pcmout(dsp, &pcm)) > 0)
    {
        size_t need = o->count + static_cast<size_t>(n) * channels;
        if (need > o->capacity)
        {
            o->capacity = need * 2 + 65536;
            o->data = static_cast<int16_t *>(HostRealloc(o->data, o->capacity * sizeof(int16_t)));
        }
        for (int i = 0; i < n; i++)
        {
            for (int c = 0; c < channels; c++)
            {
                float v = pcm[c][i] * 32767.0f;
                o->data[o->count++] = static_cast<int16_t>(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
            }
        }
        vorbis_synthesis_read(dsp, n);
    }
}

// Vorbis in a bank is bare packets, each after a 16-bit size. The three header
// packets a decoder needs first are not stored: the first two are made up here from
// the sample's rate and channel count, and the third comes from FMOD's table.
bool DecodeBankVorbis(const uint8_t * data, unsigned size, int channels, int rate, unsigned setupId, unsigned frames, Sound * s)
{
    const unsigned char * setup = nullptr;
    unsigned setupSize = 0;
    for (unsigned i = 0; i < d3_vorbis_setup_count; i++)
    {
        if (d3_vorbis_setups[i].id == setupId)
        {
            setup = d3_vorbis_setups[i].data;
            setupSize = d3_vorbis_setups[i].size;
        }
    }
    if (!setup)
    {
        return false;
    }
    unsigned char id[30] = {1, 'v', 'o', 'r', 'b', 'i', 's'};
    id[11] = static_cast<unsigned char>(channels);
    id[12] = static_cast<unsigned char>(rate);
    id[13] = static_cast<unsigned char>(rate >> 8);
    id[14] = static_cast<unsigned char>(rate >> 16);
    id[28] = 0xb8;          // block sizes 256 and 2048, which is what FMOD encodes with
    id[29] = 1;
    unsigned char comment[16] = {3, 'v', 'o', 'r', 'b', 'i', 's'};
    comment[15] = 1;

    vorbis_info info;
    vorbis_comment tags;
    vorbis_info_init(&info);
    vorbis_comment_init(&tags);
    ogg_packet packet;
    memset(&packet, 0, sizeof(packet));
    packet.packet = id;
    packet.bytes = sizeof(id);
    packet.b_o_s = 1;
    bool ok = vorbis_synthesis_headerin(&info, &tags, &packet) == 0;
    packet.b_o_s = 0;
    packet.packet = comment;
    packet.bytes = sizeof(comment);
    packet.packetno = 1;
    ok = ok && vorbis_synthesis_headerin(&info, &tags, &packet) == 0;
    packet.packet = const_cast<unsigned char *>(setup);
    packet.bytes = setupSize;
    packet.packetno = 2;
    ok = ok && vorbis_synthesis_headerin(&info, &tags, &packet) == 0;
    Pcm out = {nullptr, 0, 0};
    if (ok)
    {
        vorbis_dsp_state dsp;
        vorbis_block block;
        vorbis_synthesis_init(&dsp, &info);
        vorbis_block_init(&dsp, &block);
        unsigned at = 0;
        while (at + 2 <= size)
        {
            unsigned n = data[at] | data[at + 1] << 8;
            at += 2;
            if (n == 0 || at + n > size)
            {
                break;
            }
            packet.packet = const_cast<unsigned char *>(data + at);
            packet.bytes = n;
            packet.packetno++;
            packet.granulepos = -1;
            if (vorbis_synthesis(&block, &packet) == 0)
            {
                vorbis_synthesis_blockin(&dsp, &block);
            }
            Drain(&dsp, channels, &out);
            at += n;
        }
        vorbis_block_clear(&block);
        vorbis_dsp_clear(&dsp);
    }
    vorbis_comment_clear(&tags);
    vorbis_info_clear(&info);
    if (!out.count)
    {
        HostFree(out.data);
        return false;
    }
    s->pcm = out.data;
    s->channels = channels;
    s->rate = rate;
    s->frames = static_cast<unsigned>(out.count / channels);
    if (frames && frames < s->frames)
    {
        s->frames = frames;
    }
    return true;
}

// FMOD's ADPCM: 140-byte blocks of 256 samples for one channel, the channels taking
// turns. A block starts with a predictor choice and a scale for each of its eight
// runs of 32 samples, then the two samples before it, then 4 bits a sample.
bool DecodeBankAdpcm(const uint8_t * data, unsigned size, int channels, int rate, unsigned frames, Sound * s)
{
    static const int kPredict[7][2] = {{0, 0}, {60, 0}, {122, 60}, {115, 52}, {98, 55}, {0, 0}, {0, 0}};
    unsigned blocks = size / (140u * channels);
    unsigned total = blocks * 256;
    if (frames && frames < total)
    {
        total = frames;
    }
    if (!total)
    {
        return false;
    }
    int16_t * out = static_cast<int16_t *>(HostAlloc(static_cast<size_t>(total) * channels * sizeof(int16_t)));
    for (unsigned b = 0; b < blocks; b++)
    {
        for (int c = 0; c < channels; c++)
        {
            const uint8_t * p = data + (static_cast<size_t>(b) * channels + c) * 140;
            uint32_t choices, scales;
            int16_t before[2];
            memcpy(&choices, p, 4);
            memcpy(&scales, p + 4, 4);
            memcpy(before, p + 8, 4);
            int h1 = before[0], h2 = before[1];
            for (int run = 0; run < 8; run++)
            {
                int which = ((choices >> (run * 4)) & 15) % 7;
                int shift = 22 - ((scales >> (run * 4)) & 15);
                for (int word = 0; word < 4; word++)
                {
                    uint32_t nibbles;
                    memcpy(&nibbles, p + 12 + run * 16 + word * 4, 4);
                    for (int k = 0; k < 8; k++)
                    {
                        int32_t v = static_cast<int32_t>(((nibbles >> (k * 4)) & 15) << 28) >> shift;
                        v = (v - h2 * kPredict[which][1] + h1 * kPredict[which][0]) >> 6;
                        v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
                        unsigned frame = b * 256 + run * 32 + word * 8 + k;
                        if (frame < total)
                        {
                            out[static_cast<size_t>(frame) * channels + c] = static_cast<int16_t>(v);
                        }
                        h2 = h1;
                        h1 = v;
                    }
                }
            }
        }
    }
    s->pcm = out;
    s->channels = channels;
    s->rate = rate;
    s->frames = total;
    return true;
}

// D3_SOUND_DUMP=<directory>: write the first few decoded sounds of each kind as WAV.
void Dump(const char * kind, unsigned * counter, const Sound * s)
{
    const char * dir = getenv("D3_SOUND_DUMP");
    if (!dir || !s->pcm || *counter >= 8)
    {
        return;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s_%u.wav", dir, kind, (*counter)++);
    FILE * f = fopen(path, "wb");
    if (!f)
    {
        return;
    }
    unsigned bytes = s->frames * s->channels * 2;
    unsigned header[11] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 1u | static_cast<unsigned>(s->channels) << 16,
                           static_cast<unsigned>(s->rate), static_cast<unsigned>(s->rate * s->channels * 2),
                           static_cast<unsigned>(s->channels * 2) | 16u << 16, 0x61746164, bytes};
    fwrite(header, 4, 11, f);
    fwrite(s->pcm, 1, bytes, f);
    fclose(f);
}

Sound * NewChild(Sound * parent)
{
    Sound * child = static_cast<Sound *>(HostZero(sizeof(Sound)));
    child->parent = parent;
    child->mode = parent->mode;
    child->channels = 1;
    child->rate = 44100;
    child->defaultFrequency = 44100.0f;
    child->defaultPriority = 128;
    child->loopCount = -1;
    child->state = FMOD_OPENSTATE_READY;
    return child;
}

unsigned U32(const uint8_t * p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | static_cast<unsigned>(p[3]) << 24;
}

// A bank is a header, one description per sample, their names, then their data.
// Each sample becomes a sub-sound; a bank of one sample also plays as itself.
bool DecodeBank(const uint8_t * d, unsigned size, Sound * s)
{
    if (size < 0x40)
    {
        return false;
    }
    unsigned version = U32(d + 4), count = U32(d + 8), headers = U32(d + 12), names = U32(d + 16), dataSize = U32(d + 20), codec = U32(d + 24);
    unsigned base = version == 0 ? 0x40 : 0x3c;
    unsigned dataStart = base + headers + names;
    if (count == 0 || count > 65536 || dataStart > size)
    {
        return false;
    }
    if (dataSize > size - dataStart)
    {
        dataSize = size - dataStart;
    }
    struct Entry {
        unsigned offset, frames, setup, loopStart, loopEnd;
        int channels, rate;
    };
    static const int kRates[16] = {4000, 8000, 11000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 96000, 44100, 44100, 44100, 44100, 44100};
    static const int kChannels[4] = {1, 2, 6, 8};
    Entry * entries = static_cast<Entry *>(HostZero(count * sizeof(Entry)));
    unsigned at = base, end = base + headers;
    bool ok = true;
    for (unsigned i = 0; i < count && ok; i++)
    {
        if (at + 8 > end)
        {
            ok = false;
            break;
        }
        unsigned a = U32(d + at), b = U32(d + at + 4);
        at += 8;
        Entry & e = entries[i];
        e.rate = kRates[(a >> 1) & 15];
        e.channels = kChannels[(a >> 5) & 3];
        e.offset = (((b & 3) << 25) | (a >> 7)) << 5;
        e.frames = b >> 2;
        bool more = a & 1;
        while (more && at + 4 <= end)
        {
            unsigned chunk = U32(d + at);
            at += 4;
            more = chunk & 1;
            unsigned length = (chunk >> 1) & 0xffffff, kind = chunk >> 25;
            if (at + length > end)
            {
                ok = false;
                break;
            }
            if (kind == 1 && length >= 1) e.channels = d[at];
            if (kind == 2 && length >= 4) e.rate = static_cast<int>(U32(d + at));
            if (kind == 3 && length >= 8) { e.loopStart = U32(d + at); e.loopEnd = U32(d + at + 4); }
            if (kind == 11 && length >= 4) e.setup = U32(d + at);
            at += length;
        }
    }
    bool any = false;
    if (ok)
    {
        s->subSounds = static_cast<Sound **>(HostZero(count * sizeof(Sound *)));
        s->subSoundCount = static_cast<int>(count);
        for (unsigned i = 0; i < count; i++)
        {
            const Entry & e = entries[i];
            unsigned next = i + 1 < count ? entries[i + 1].offset : dataSize;
            Sound * child = NewChild(s);
            s->subSounds[i] = child;
            if (e.offset >= dataSize || next <= e.offset || next > dataSize || e.channels < 1 || e.channels > 2)
            {
                g_statFailed++;
                continue;
            }
            const uint8_t * body = d + dataStart + e.offset;
            unsigned length = next - e.offset;
            bool made = false;
            if (codec == 15)
            {
                g_statBankVorbis++;
                made = DecodeBankVorbis(body, length, e.channels, e.rate, e.setup, e.frames, child);
            }
            else if (codec == 16)
            {
                g_statBankAdpcm++;
                made = DecodeBankAdpcm(body, length, e.channels, e.rate, e.frames, child);
            }
            else if (codec == 2)
            {
                g_statBankOther++;
                ConvertPcm(body, length, FMOD_SOUND_FORMAT_PCM16, e.channels, e.rate, child);
                made = true;
            }
            else
            {
                g_statBankOther++;
            }
            if (!made)
            {
                g_statFailed++;
                continue;
            }
            any = true;
            static unsigned dumpedVorbis, dumpedAdpcm;
            Dump(codec == 15 ? "bank_vorbis" : "bank_adpcm", codec == 15 ? &dumpedVorbis : &dumpedAdpcm, child);
            child->defaultFrequency = static_cast<float>(child->rate);
            child->loopStart = e.loopStart;
            child->loopEnd = e.loopEnd;
        }
        if (count == 1 && s->subSounds[0]->pcm)
        {
            Sound * only = s->subSounds[0];
            s->pcm = only->pcm;
            only->borrowed = true;
            s->channels = only->channels;
            s->rate = only->rate;
            s->frames = only->frames;
            s->loopStart = only->loopStart;
            s->loopEnd = only->loopEnd;
        }
    }
    HostFree(entries);
    return any;
}

// Fills in a sound from the bytes of its file. False if the kind is not handled,
// in which case the sound stays silent.
bool Decode(const uint8_t * data, unsigned size, FMOD_MODE mode, const FMOD_CREATESOUNDEXINFO * info, Sound * s)
{
    if (mode & FMOD_OPENRAW)
    {
        g_statRaw++;
        ConvertPcm(data, size, info ? info->format : FMOD_SOUND_FORMAT_PCM16, info ? info->numchannels : 1,
                   info ? info->defaultfrequency : 44100, s);
        return true;
    }
    if (size >= 4 && memcmp(data, "OggS", 4) == 0)
    {
        g_statVorbis++;
        return DecodeVorbis(data, size, s);
    }
    if (size >= 12 && memcmp(data, "RIFF", 4) == 0)
    {
        g_statWav++;
        return DecodeWav(data, size, s);
    }
    if (size >= 4 && memcmp(data, "FSB5", 4) == 0)
    {
        return DecodeBank(data, size, s);
    }
    g_statUnknown++;
    return false;
}

struct LoadJob {
    Sound * sound;
    char * name;
    const uint8_t * memory;
    unsigned memorySize;
    FMOD_MODE mode;
    FMOD_CREATESOUNDEXINFO info;
    bool haveInfo;
    LoadJob * next;
};

void * Load(void * p)
{
    LoadJob * job = static_cast<LoadJob *>(p);
    const FMOD_CREATESOUNDEXINFO * info = job->haveInfo ? &job->info : nullptr;
    uint8_t * data = nullptr;
    unsigned size = 0;
    bool ok = false;
    if (job->memory)
    {
        ok = Decode(job->memory, job->memorySize, job->mode, info, job->sound);
    }
    else if (job->name && ReadAll(job->name, info, &data, &size))
    {
        ok = Decode(data, size, job->mode, info, job->sound);
        HostFree(data);
    }
    // A sound that could not be decoded stays silent, and still "opens".
    Sound * s = job->sound;
    if (!ok && !(job->mode & FMOD_OPENUSER))
    {
        g_statFailed += s->subSoundCount ? 0 : 1;
    }
    s->defaultFrequency = static_cast<float>(s->rate);
    s->state = FMOD_OPENSTATE_READY;
    if (job->haveInfo && job->info.nonblockcallback)
    {
        job->info.nonblockcallback(reinterpret_cast<FMOD_SOUND *>(s), FMOD_OK);
    }
    HostFree(job->name);
    HostFree(job);
    return nullptr;
}

// Sounds opened "non-blocking" are loaded one at a time on one thread, as FMOD does.
pthread_mutex_t g_queueLock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_queueWake = PTHREAD_COND_INITIALIZER;
LoadJob * g_queueFirst, * g_queueLast;
bool g_loaderStarted;

void * Loader(void *)
{
    for (;;)
    {
        pthread_mutex_lock(&g_queueLock);
        while (!g_queueFirst)
        {
            pthread_cond_wait(&g_queueWake, &g_queueLock);
        }
        LoadJob * job = g_queueFirst;
        g_queueFirst = job->next;
        if (!g_queueFirst)
        {
            g_queueLast = nullptr;
        }
        pthread_mutex_unlock(&g_queueLock);
        Load(job);
    }
    return nullptr;
}

void Queue(LoadJob * job)
{
    pthread_mutex_lock(&g_queueLock);
    if (g_queueLast)
    {
        g_queueLast->next = job;
    }
    else
    {
        g_queueFirst = job;
    }
    g_queueLast = job;
    if (!g_loaderStarted)
    {
        g_loaderStarted = true;
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1024 * 1024);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&thread, &attr, Loader, nullptr);
        pthread_attr_destroy(&attr);
    }
    pthread_cond_signal(&g_queueWake);
    pthread_mutex_unlock(&g_queueLock);
}

FMOD_RESULT Create(const char * nameOrData, FMOD_MODE mode, FMOD_CREATESOUNDEXINFO * info, FMOD_SOUND ** out)
{
    Sound * s = static_cast<Sound *>(HostZero(sizeof(Sound)));
    s->mode = mode;
    s->channels = info && info->numchannels ? info->numchannels : 1;
    s->rate = info && info->defaultfrequency ? info->defaultfrequency : 44100;
    s->defaultFrequency = static_cast<float>(s->rate);
    s->defaultPriority = 128;
    s->loopCount = -1;
    s->userData = info ? info->userdata : nullptr;
    *out = reinterpret_cast<FMOD_SOUND *>(s);
    if (mode & FMOD_OPENUSER)
    {
        // The game supplies the samples through a callback, as 16-bit PCM.
        g_statUser++;
        s->read = info ? info->pcmreadcallback : nullptr;
        s->frames = info && info->length ? info->length / (2 * s->channels) : 0;
        s->state = FMOD_OPENSTATE_READY;
        return FMOD_OK;
    }
    LoadJob * job = static_cast<LoadJob *>(HostZero(sizeof(LoadJob)));
    job->sound = s;
    job->mode = mode;
    job->haveInfo = info != nullptr;
    if (info)
    {
        job->info = *info;
    }
    if (mode & (FMOD_OPENMEMORY | FMOD_OPENMEMORY_POINT))
    {
        job->memory = reinterpret_cast<const uint8_t *>(nameOrData);
        job->memorySize = info ? info->length : 0;
    }
    else
    {
        if (nameOrData)
        {
            job->name = static_cast<char *>(HostAlloc(strlen(nameOrData) + 1));
            strcpy(job->name, nameOrData);
        }
    }
    s->state = FMOD_OPENSTATE_LOADING;
    if ((mode & FMOD_NONBLOCKING) && !job->memory)
    {
        Queue(job);
    }
    else
    {
        Load(job);
    }
    return FMOD_OK;
}

// ---- mixing ----

float Attenuation(const Channel & c, float * pan)
{
    *pan = 0.0f;
    if (!c.is3d || c.level3d <= 0.0f)
    {
        return 1.0f;
    }
    float dx = c.where.x - g_listener.x, dy = c.where.y - g_listener.y, dz = c.where.z - g_listener.z;
    float distance = sqrtf(dx * dx + dy * dy + dz * dz);
    float minD = c.minDistance > 0.01f ? c.minDistance : 1.0f;
    float maxD = c.maxDistance > minD ? c.maxDistance : minD * 10000.0f;
    float gain = distance <= minD ? 1.0f : distance >= maxD ? 0.0f : 1.0f - (distance - minD) / (maxD - minD);
    // Left/right from where the sound is relative to the way the listener faces.
    float rx = g_listenerUp.y * g_listenerForward.z - g_listenerUp.z * g_listenerForward.y;
    float ry = g_listenerUp.z * g_listenerForward.x - g_listenerUp.x * g_listenerForward.z;
    float rz = g_listenerUp.x * g_listenerForward.y - g_listenerUp.y * g_listenerForward.x;
    if (distance > 0.001f)
    {
        *pan = (dx * rx + dy * ry + dz * rz) / distance;
    }
    return 1.0f + (gain - 1.0f) * c.level3d;
}

void Mix(float * out, int frames)
{
    memset(out, 0, sizeof(float) * 2 * frames);
    Lock lock;
    for (int i = 0; i < kMaxChannels; i++)
    {
        Channel & c = g_channels[i];
        Sound * s = c.sound;
        if (!c.playing || c.paused || !s || s->state != FMOD_OPENSTATE_READY)
        {
            continue;
        }
        float pan;
        float gain = c.volume * (c.group ? c.group->volume : 1.0f) * g_master.volume * Attenuation(c, &pan) / 32768.0f;
        float left = gain * (c.is3d ? (pan > 0 ? 1.0f - pan : 1.0f) : c.gainLeft);
        float right = gain * (c.is3d ? (pan < 0 ? 1.0f + pan : 1.0f) : c.gainRight);
        double step = c.frequency / kRate;
        int start = 0;
        if (c.delayStart > g_clock)
        {
            unsigned long long wait = c.delayStart - g_clock;
            if (wait >= static_cast<unsigned long long>(frames))
            {
                continue;
            }
            start = static_cast<int>(wait);
        }
        if (s->read)
        {
            // A stream the game fills: ask for exactly what is needed, at the sound's own rate.
            int16_t chunk[2048];
            int f = start;
            while (f < frames)
            {
                int want = frames - f;
                int source = static_cast<int>(want * step) + 1;
                if (source * s->channels > 2048)
                {
                    source = 2048 / s->channels;
                }
                memset(chunk, 0, sizeof(chunk));
                pthread_mutex_unlock(&g_lock);          // the callback is the game's code
                s->read(reinterpret_cast<FMOD_SOUND *>(s), chunk, static_cast<unsigned>(source * s->channels * 2));
                pthread_mutex_lock(&g_lock);
                if (c.sound != s || !c.playing)
                {
                    break;
                }
                int made = static_cast<int>(source / step);
                made = made < 1 ? 1 : made > want ? want : made;
                for (int k = 0; k < made; k++)
                {
                    int at = static_cast<int>(k * step);
                    at = at >= source ? source - 1 : at;
                    float l = chunk[at * s->channels];
                    float r = s->channels > 1 ? chunk[at * s->channels + 1] : l;
                    out[(f + k) * 2] += l * left;
                    out[(f + k) * 2 + 1] += r * right;
                }
                f += made;
                c.position += source;
            }
            continue;
        }
        if (!s->pcm || !s->frames)
        {
            c.playing = false;      // silence: over at once
            c.generation++;
            continue;
        }
        unsigned end = c.loopEnd && c.loopEnd < s->frames ? c.loopEnd + 1 : s->frames;
        for (int f = start; f < frames; f++)
        {
            if (c.delayEnd && g_clock + f >= c.delayEnd)
            {
                c.playing = false;
                break;
            }
            if (c.position >= end)
            {
                bool loop = (s->mode & FMOD_LOOP_NORMAL) && c.loopsLeft != 0;
                if (!loop)
                {
                    c.playing = false;
                    break;
                }
                if (c.loopsLeft > 0)
                {
                    c.loopsLeft--;
                }
                c.position = c.loopStart < end ? c.loopStart : 0;
            }
            unsigned at = static_cast<unsigned>(c.position);
            const int16_t * p = s->pcm + static_cast<size_t>(at) * s->channels;
            float l = p[0];
            float r = s->channels > 1 ? p[1] : l;
            out[f * 2] += l * left;
            out[f * 2 + 1] += r * right;
            c.position += step;
        }
        if (!c.playing)
        {
            c.generation++;
        }
    }
    g_clock += frames;
    int voices = 0;
    for (int i = 0; i < kMaxChannels; i++)
    {
        voices += g_channels[i].playing && !g_channels[i].paused;
    }
    g_statVoices = voices > g_statVoices ? voices : g_statVoices;
    for (int i = 0; i < frames * 2; i++)
    {
        float v = out[i] < 0 ? -out[i] : out[i];
        g_statPeak = v > g_statPeak ? v : g_statPeak;
        g_statEnergy += static_cast<double>(v) * v;
        out[i] = out[i] > 1.0f ? 1.0f : out[i] < -1.0f ? -1.0f : out[i];
    }
    g_statMixed += static_cast<unsigned long long>(frames) * 2;
}

void PrintStats()
{
    fprintf(stderr, "sound: made %u from Ogg Vorbis files, %u from bank Vorbis, %u from bank ADPCM, %u other bank samples, %u raw, %u WAV, "
                    "%u game-filled streams, %u of unknown kind; %u could not be decoded; %u plays\n",
            g_statVorbis, g_statBankVorbis, g_statBankAdpcm, g_statBankOther, g_statRaw, g_statWav, g_statUser, g_statUnknown, g_statFailed, g_statPlayed);
    // Since the last line: how loud the mix was (1.0 is full scale) and the most sounds playing at once.
    fprintf(stderr, "sound: output peak %.2f, average level %.3f, up to %d at once\n", g_statPeak,
            g_statMixed ? sqrt(g_statEnergy / static_cast<double>(g_statMixed)) : 0.0, g_statVoices);
    g_statPeak = 0;
    g_statEnergy = 0;
    g_statMixed = 0;
    g_statVoices = 0;
}

}  // namespace

// This file is built with everything hidden (see CMakeLists.txt, d3_sound); the FMOD
// functions are what the game links against.
#pragma GCC visibility push(default)
extern "C" {

// ---- memory and the system object ----

FMOD_RESULT FMOD_Memory_Initialize(void *, int, FMOD_MEMORY_ALLOC_CALLBACK, FMOD_MEMORY_REALLOC_CALLBACK, FMOD_MEMORY_FREE_CALLBACK, FMOD_MEMORY_TYPE)
{
    return getenv("D3_NO_SOUND") ? FMOD_ERR_OUTPUT_INIT : FMOD_OK;
}

FMOD_RESULT FMOD_Memory_GetStats(int * current, int * peak, FMOD_BOOL)
{
    if (current) *current = 0;
    if (peak) *peak = 0;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_Create(FMOD_SYSTEM ** system)
{
    static char one;
    *system = reinterpret_cast<FMOD_SYSTEM *>(&one);
    for (int i = 0; i < kMaxChannels; i++)
    {
        g_channels[i].generation = 1;
    }
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_Release(FMOD_SYSTEM *) { return FMOD_OK; }

FMOD_RESULT FMOD_System_SetFileSystem(FMOD_SYSTEM *, FMOD_FILE_OPEN_CALLBACK open, FMOD_FILE_CLOSE_CALLBACK close, FMOD_FILE_READ_CALLBACK read,
                                      FMOD_FILE_SEEK_CALLBACK seek, FMOD_FILE_ASYNCREAD_CALLBACK, FMOD_FILE_ASYNCCANCEL_CALLBACK, int)
{
    g_files.open = open;
    g_files.close = close;
    g_files.read = read;
    g_files.seek = seek;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_SetOutput(FMOD_SYSTEM *, FMOD_OUTPUTTYPE) { return FMOD_OK; }

FMOD_RESULT FMOD_System_GetNumDrivers(FMOD_SYSTEM *, int * count)
{
    *count = 1;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_SetDriver(FMOD_SYSTEM *, int) { return FMOD_OK; }
FMOD_RESULT FMOD_System_SetSoftwareChannels(FMOD_SYSTEM *, int) { return FMOD_OK; }

FMOD_RESULT FMOD_System_SetSoftwareFormat(FMOD_SYSTEM *, int rate, FMOD_SPEAKERMODE, int)
{
    g_softwareRate = rate > 0 ? rate : kRate;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetSoftwareFormat(FMOD_SYSTEM *, int * rate, FMOD_SPEAKERMODE * mode, int * speakers)
{
    if (rate) *rate = kRate;
    if (mode) *mode = FMOD_SPEAKERMODE_STEREO;
    if (speakers) *speakers = 2;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_SetStreamBufferSize(FMOD_SYSTEM *, unsigned int, FMOD_TIMEUNIT) { return FMOD_OK; }
FMOD_RESULT FMOD_System_SetCallback(FMOD_SYSTEM *, FMOD_SYSTEM_CALLBACK, FMOD_SYSTEM_CALLBACK_TYPE) { return FMOD_OK; }

FMOD_RESULT FMOD_System_Init(FMOD_SYSTEM *, int maxchannels, FMOD_INITFLAGS, void *)
{
    if (maxchannels > 0)
    {
        g_channelLimit = maxchannels < kMaxChannels ? maxchannels : kMaxChannels;
    }
    if (!g_started)
    {
        g_started = true;
        d3_audio_start(kRate, Mix);
        if (getenv("D3_SOUND_STATS"))
        {
            atexit(PrintStats);
        }
    }
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_Update(FMOD_SYSTEM *)
{
    static unsigned calls;
    if (getenv("D3_SOUND_STATS") && ++calls % 600 == 0)
    {
        PrintStats();
    }
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_Set3DSettings(FMOD_SYSTEM *, float, float, float) { return FMOD_OK; }

FMOD_RESULT FMOD_System_Set3DListenerAttributes(FMOD_SYSTEM *, int, const FMOD_VECTOR * pos, const FMOD_VECTOR *, const FMOD_VECTOR * forward,
                                                const FMOD_VECTOR * up)
{
    Lock lock;
    if (pos) g_listener = *pos;
    if (forward) g_listenerForward = *forward;
    if (up) g_listenerUp = *up;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_Set3DRolloffCallback(FMOD_SYSTEM *, FMOD_3D_ROLLOFF_CALLBACK) { return FMOD_OK; }

FMOD_RESULT FMOD_System_GetVersion(FMOD_SYSTEM *, unsigned int * version)
{
    *version = FMOD_VERSION;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_CreateSound(FMOD_SYSTEM *, const char * nameOrData, FMOD_MODE mode, FMOD_CREATESOUNDEXINFO * info, FMOD_SOUND ** sound)
{
    return Create(nameOrData, mode, info, sound);
}

FMOD_RESULT FMOD_System_CreateStream(FMOD_SYSTEM *, const char * nameOrData, FMOD_MODE mode, FMOD_CREATESOUNDEXINFO * info, FMOD_SOUND ** sound)
{
    return Create(nameOrData, mode | FMOD_CREATESTREAM, info, sound);
}

// Effects are accepted and do nothing.
FMOD_RESULT FMOD_System_CreateDSP(FMOD_SYSTEM *, const FMOD_DSP_DESCRIPTION *, FMOD_DSP ** dsp)
{
    *dsp = reinterpret_cast<FMOD_DSP *>(HostZero(16));
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_CreateDSPByType(FMOD_SYSTEM *, FMOD_DSP_TYPE, FMOD_DSP ** dsp)
{
    *dsp = reinterpret_cast<FMOD_DSP *>(HostZero(16));
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_CreateChannelGroup(FMOD_SYSTEM *, const char *, FMOD_CHANNELGROUP ** group)
{
    Group * g = static_cast<Group *>(HostZero(sizeof(Group)));
    g->volume = 1.0f;
    *group = reinterpret_cast<FMOD_CHANNELGROUP *>(g);
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_PlaySound(FMOD_SYSTEM *, FMOD_SOUND * sound, FMOD_CHANNELGROUP * group, FMOD_BOOL paused, FMOD_CHANNEL ** channel)
{
    Sound * s = reinterpret_cast<Sound *>(sound);
    Lock lock;
    for (int i = 0; i < g_channelLimit; i++)
    {
        Channel & c = g_channels[i];
        if (c.playing)
        {
            continue;
        }
        unsigned generation = c.generation + 1;
        memset(&c, 0, sizeof(c));
        c.generation = generation;
        c.sound = s;
        c.group = reinterpret_cast<Group *>(group);
        c.volume = 1.0f;
        c.frequency = s->defaultFrequency > 0 ? s->defaultFrequency : static_cast<float>(s->rate);
        c.playing = true;
        c.paused = paused != 0;
        c.is3d = (s->mode & FMOD_3D) != 0;
        c.loopsLeft = s->loopCount;
        c.loopStart = s->loopStart;
        c.loopEnd = s->loopEnd;
        c.minDistance = 1.0f;
        c.maxDistance = 10000.0f;
        c.level3d = 1.0f;
        c.gainLeft = c.gainRight = 1.0f;
        g_statPlayed++;
        if (channel)
        {
            *channel = Handle(i);
        }
        return FMOD_OK;
    }
    return FMOD_ERR_CHANNEL_ALLOC;
}

FMOD_RESULT FMOD_System_GetChannel(FMOD_SYSTEM *, int id, FMOD_CHANNEL ** channel)
{
    if (id < 0 || id >= kMaxChannels)
    {
        return FMOD_ERR_INVALID_PARAM;
    }
    *channel = Handle(id);
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetMasterChannelGroup(FMOD_SYSTEM *, FMOD_CHANNELGROUP ** group)
{
    *group = reinterpret_cast<FMOD_CHANNELGROUP *>(&g_master);
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_SetReverbProperties(FMOD_SYSTEM *, int instance, const FMOD_REVERB_PROPERTIES * prop)
{
    if (instance >= 0 && instance < FMOD_REVERB_MAXINSTANCES && prop)
    {
        g_reverb[instance] = *prop;
    }
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetReverbProperties(FMOD_SYSTEM *, int instance, FMOD_REVERB_PROPERTIES * prop)
{
    if (instance >= 0 && instance < FMOD_REVERB_MAXINSTANCES && prop)
    {
        *prop = g_reverb[instance];
    }
    return FMOD_OK;
}

FMOD_RESULT FMOD_NX_SetThreadAffinity() { return FMOD_OK; }

// ---- sounds ----

FMOD_RESULT FMOD_Sound_Release(FMOD_SOUND * sound)
{
    Sound * s = reinterpret_cast<Sound *>(sound);
    if (!s || s->parent)
    {
        return FMOD_OK;         // sub-sounds go with their parent
    }
    {
        Lock lock;
        for (int i = 0; i < kMaxChannels; i++)
        {
            Channel & c = g_channels[i];
            bool mine = c.sound == s;
            for (int k = 0; !mine && k < s->subSoundCount; k++)
            {
                mine = c.sound == s->subSounds[k];
            }
            if (mine)
            {
                c.playing = false;
                c.sound = nullptr;
                c.generation++;
            }
        }
    }
    for (int k = 0; k < s->subSoundCount; k++)
    {
        if (s->subSounds[k])
        {
            if (!s->subSounds[k]->borrowed)
            {
                HostFree(s->subSounds[k]->pcm);
            }
            HostFree(s->subSounds[k]);
        }
    }
    HostFree(s->subSounds);
    HostFree(s->pcm);
    HostFree(s);
    return FMOD_OK;
}

FMOD_RESULT FMOD_Sound_SetDefaults(FMOD_SOUND * sound, float frequency, int priority)
{
    Sound * s = reinterpret_cast<Sound *>(sound);
    s->defaultFrequency = frequency;
    s->defaultPriority = priority;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Sound_GetDefaults(FMOD_SOUND * sound, float * frequency, int * priority)
{
    Sound * s = reinterpret_cast<Sound *>(sound);
    if (frequency) *frequency = s->defaultFrequency > 0 ? s->defaultFrequency : static_cast<float>(s->rate);
    if (priority) *priority = s->defaultPriority;
    return FMOD_OK;
}

// The samples of a bank. One that was not in the bank, or that could not be decoded,
// is an empty sound that ends as soon as it is played.
FMOD_RESULT FMOD_Sound_GetSubSound(FMOD_SOUND * sound, int index, FMOD_SOUND ** sub)
{
    Sound * s = reinterpret_cast<Sound *>(sound);
    if (index < 0 || index > 65535)
    {
        return FMOD_ERR_INVALID_PARAM;
    }
    if (index >= s->subSoundCount)
    {
        s->subSounds = static_cast<Sound **>(HostRealloc(s->subSounds, sizeof(Sound *) * (index + 1)));
        for (int k = s->subSoundCount; k <= index; k++)
        {
            s->subSounds[k] = nullptr;
        }
        s->subSoundCount = index + 1;
    }
    if (!s->subSounds[index])
    {
        Sound * child = NewChild(s);
        s->subSounds[index] = child;
    }
    *sub = reinterpret_cast<FMOD_SOUND *>(s->subSounds[index]);
    return FMOD_OK;
}

FMOD_RESULT FMOD_Sound_GetOpenState(FMOD_SOUND * sound, FMOD_OPENSTATE * state, unsigned int * percent, FMOD_BOOL * starving, FMOD_BOOL * busy)
{
    Sound * s = reinterpret_cast<Sound *>(sound);
    if (state) *state = static_cast<FMOD_OPENSTATE>(s->state);
    if (percent) *percent = 100;
    if (starving) *starving = 0;
    if (busy) *busy = 0;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Sound_SetMode(FMOD_SOUND * sound, FMOD_MODE mode)
{
    Sound * s = reinterpret_cast<Sound *>(sound);
    const FMOD_MODE loops = FMOD_LOOP_OFF | FMOD_LOOP_NORMAL | FMOD_LOOP_BIDI, space = FMOD_2D | FMOD_3D;
    if (mode & loops) s->mode = (s->mode & ~loops) | (mode & loops);
    if (mode & space) s->mode = (s->mode & ~space) | (mode & space);
    return FMOD_OK;
}

FMOD_RESULT FMOD_Sound_SetLoopCount(FMOD_SOUND * sound, int count)
{
    reinterpret_cast<Sound *>(sound)->loopCount = count;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Sound_SetUserData(FMOD_SOUND * sound, void * data)
{
    reinterpret_cast<Sound *>(sound)->userData = data;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Sound_GetUserData(FMOD_SOUND * sound, void ** data)
{
    *data = reinterpret_cast<Sound *>(sound)->userData;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Sound_GetLength(FMOD_SOUND * sound, unsigned int * length, FMOD_TIMEUNIT unit)
{
    Sound * s = reinterpret_cast<Sound *>(sound);
    unsigned frames = s->frames;
    *length = unit == FMOD_TIMEUNIT_MS ? static_cast<unsigned>(frames * 1000.0 / (s->rate > 0 ? s->rate : 44100))
            : unit == FMOD_TIMEUNIT_PCMBYTES ? frames * 2 * s->channels : frames;
    return FMOD_OK;
}

// ---- channels ----
// A handle for a sound that has finished answers FMOD_ERR_INVALID_HANDLE, as FMOD does.

#define CHANNEL(c)                          \
    Lock lock;                              \
    Channel * c = Find(channel);            \
    if (!c)                                 \
    {                                       \
        return FMOD_ERR_INVALID_HANDLE;     \
    }

FMOD_RESULT FMOD_Channel_Stop(FMOD_CHANNEL * channel)
{
    CHANNEL(c)
    c->playing = false;
    c->generation++;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_SetPaused(FMOD_CHANNEL * channel, FMOD_BOOL paused)
{
    CHANNEL(c)
    c->paused = paused != 0;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_SetVolume(FMOD_CHANNEL * channel, float volume)
{
    CHANNEL(c)
    c->volume = volume;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_GetVolume(FMOD_CHANNEL * channel, float * volume)
{
    CHANNEL(c)
    *volume = c->volume;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_SetFrequency(FMOD_CHANNEL * channel, float frequency)
{
    CHANNEL(c)
    c->frequency = frequency;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_GetFrequency(FMOD_CHANNEL * channel, float * frequency)
{
    CHANNEL(c)
    *frequency = c->frequency;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_SetReverbProperties(FMOD_CHANNEL * channel, int, float)
{
    CHANNEL(c)
    (void)c;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_SetDelay(FMOD_CHANNEL * channel, unsigned long long start, unsigned long long end, FMOD_BOOL)
{
    CHANNEL(c)
    c->delayStart = start;
    c->delayEnd = end;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_GetDSPClock(FMOD_CHANNEL * channel, unsigned long long * clock, unsigned long long * parent)
{
    CHANNEL(c)
    (void)c;
    if (clock) *clock = g_clock;
    if (parent) *parent = g_clock;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_AddDSP(FMOD_CHANNEL * channel, int, FMOD_DSP *)
{
    CHANNEL(c)
    (void)c;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_RemoveDSP(FMOD_CHANNEL * channel, FMOD_DSP *)
{
    CHANNEL(c)
    (void)c;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_SetMode(FMOD_CHANNEL * channel, FMOD_MODE mode)
{
    CHANNEL(c)
    if (mode & FMOD_3D) c->is3d = true;
    if (mode & FMOD_2D) c->is3d = false;
    if (mode & FMOD_LOOP_OFF) c->loopsLeft = 0;
    if (mode & FMOD_LOOP_NORMAL) c->loopsLeft = c->sound ? c->sound->loopCount : -1;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_Set3DAttributes(FMOD_CHANNEL * channel, const FMOD_VECTOR * pos, const FMOD_VECTOR *, const FMOD_VECTOR *)
{
    CHANNEL(c)
    if (pos) c->where = *pos;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_Get3DAttributes(FMOD_CHANNEL * channel, FMOD_VECTOR * pos, FMOD_VECTOR * vel, FMOD_VECTOR * alt)
{
    CHANNEL(c)
    if (pos) *pos = c->where;
    if (vel) memset(vel, 0, sizeof(*vel));
    if (alt) memset(alt, 0, sizeof(*alt));
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_Set3DMinMaxDistance(FMOD_CHANNEL * channel, float minDistance, float maxDistance)
{
    CHANNEL(c)
    c->minDistance = minDistance;
    c->maxDistance = maxDistance;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_Set3DOcclusion(FMOD_CHANNEL * channel, float, float)
{
    CHANNEL(c)
    (void)c;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_Set3DSpread(FMOD_CHANNEL * channel, float)
{
    CHANNEL(c)
    (void)c;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_Set3DLevel(FMOD_CHANNEL * channel, float level)
{
    CHANNEL(c)
    c->level3d = level;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_Set3DDopplerLevel(FMOD_CHANNEL * channel, float)
{
    CHANNEL(c)
    (void)c;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_SetUserData(FMOD_CHANNEL * channel, void * data)
{
    CHANNEL(c)
    c->userData = data;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_GetUserData(FMOD_CHANNEL * channel, void ** data)
{
    CHANNEL(c)
    *data = c->userData;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_SetChannelGroup(FMOD_CHANNEL * channel, FMOD_CHANNELGROUP * group)
{
    CHANNEL(c)
    c->group = reinterpret_cast<Group *>(group);
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_SetPosition(FMOD_CHANNEL * channel, unsigned int position, FMOD_TIMEUNIT unit)
{
    CHANNEL(c)
    int rate = c->sound && c->sound->rate > 0 ? c->sound->rate : 44100;
    int channels = c->sound ? c->sound->channels : 1;
    c->position = unit == FMOD_TIMEUNIT_MS ? position * (rate / 1000.0) : unit == FMOD_TIMEUNIT_PCMBYTES ? position / (2.0 * channels) : position;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_GetPosition(FMOD_CHANNEL * channel, unsigned int * position, FMOD_TIMEUNIT unit)
{
    CHANNEL(c)
    int rate = c->sound && c->sound->rate > 0 ? c->sound->rate : 44100;
    int channels = c->sound ? c->sound->channels : 1;
    *position = unit == FMOD_TIMEUNIT_MS ? static_cast<unsigned>(c->position * 1000.0 / rate)
              : unit == FMOD_TIMEUNIT_PCMBYTES ? static_cast<unsigned>(c->position) * 2 * channels : static_cast<unsigned>(c->position);
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_SetLoopCount(FMOD_CHANNEL * channel, int count)
{
    CHANNEL(c)
    c->loopsLeft = count;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_SetLoopPoints(FMOD_CHANNEL * channel, unsigned int start, FMOD_TIMEUNIT startUnit, unsigned int end, FMOD_TIMEUNIT endUnit)
{
    CHANNEL(c)
    double perMs = (c->sound && c->sound->rate > 0 ? c->sound->rate : 44100) / 1000.0;
    c->loopStart = startUnit == FMOD_TIMEUNIT_MS ? static_cast<unsigned>(start * perMs) : start;
    c->loopEnd = endUnit == FMOD_TIMEUNIT_MS ? static_cast<unsigned>(end * perMs) : end;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_SetPriority(FMOD_CHANNEL * channel, int)
{
    CHANNEL(c)
    (void)c;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_SetMixLevelsOutput(FMOD_CHANNEL * channel, float frontLeft, float frontRight, float, float, float, float, float, float)
{
    CHANNEL(c)
    c->gainLeft = frontLeft;
    c->gainRight = frontRight;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_GetIndex(FMOD_CHANNEL * channel, int * index)
{
    CHANNEL(c)
    *index = static_cast<int>(c - g_channels);
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_IsPlaying(FMOD_CHANNEL * channel, FMOD_BOOL * playing)
{
    Lock lock;
    Channel * c = Find(channel);
    *playing = c && c->playing;
    return c ? FMOD_OK : FMOD_ERR_INVALID_HANDLE;
}

// ---- groups and effects ----

FMOD_RESULT FMOD_ChannelGroup_SetVolume(FMOD_CHANNELGROUP * group, float volume)
{
    Lock lock;
    reinterpret_cast<Group *>(group)->volume = volume;
    return FMOD_OK;
}

FMOD_RESULT FMOD_ChannelGroup_AddDSP(FMOD_CHANNELGROUP *, int, FMOD_DSP *) { return FMOD_OK; }
FMOD_RESULT FMOD_DSP_Release(FMOD_DSP * dsp) { HostFree(dsp); return FMOD_OK; }
FMOD_RESULT FMOD_DSP_SetBypass(FMOD_DSP *, FMOD_BOOL) { return FMOD_OK; }
FMOD_RESULT FMOD_DSP_SetParameterFloat(FMOD_DSP *, int, float) { return FMOD_OK; }
FMOD_RESULT FMOD_DSP_SetParameterInt(FMOD_DSP *, int, int) { return FMOD_OK; }
FMOD_RESULT FMOD_DSP_SetParameterBool(FMOD_DSP *, int, FMOD_BOOL) { return FMOD_OK; }
FMOD_RESULT FMOD_DSP_GetSystemObject(FMOD_DSP *, FMOD_SYSTEM ** system)
{
    static char one;
    *system = reinterpret_cast<FMOD_SYSTEM *>(&one);
    return FMOD_OK;
}

}  // extern "C"
#pragma GCC visibility pop
