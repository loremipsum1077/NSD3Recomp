// The host's audio output, on SDL. The mixer (src/audio/fmod_host.cpp) is called
// from the audio device's own thread to fill each buffer.
//
// If no audio device can be opened, a thread calls the mixer at the same pace and
// throws the result away, so that sounds still start, advance and end on time.
#include <SDL.h>

#include <pthread.h>
#include <stdio.h>
#include <time.h>

namespace {

void (*g_mix)(float * interleavedStereo, int frames);
int g_rate;

void Callback(void *, Uint8 * stream, int bytes)
{
    g_mix(reinterpret_cast<float *>(stream), bytes / static_cast<int>(sizeof(float) * 2));
}

void * Silent(void *)
{
    float buffer[1024 * 2];
    timespec wait = {0, static_cast<long>(1024 * 1000000000ll / g_rate)};
    for (;;)
    {
        g_mix(buffer, 1024);
        nanosleep(&wait, nullptr);
    }
    return nullptr;
}

}  // namespace

extern "C" int d3_audio_start(int rate, void (*mix)(float * interleavedStereo, int frames))
{
    g_mix = mix;
    g_rate = rate;
    SDL_AudioSpec want = {}, got = {};
    want.freq = rate;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = Callback;
    SDL_AudioDeviceID device = 0;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
    {
        device = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
    }
    if (device)
    {
        SDL_PauseAudioDevice(device, 0);
        return 1;
    }
    fprintf(stderr, "sound: no audio device (%s); running silently\n", SDL_GetError());
    pthread_t thread;
    pthread_create(&thread, nullptr, Silent, nullptr);
    pthread_detach(thread);
    return 0;
}
