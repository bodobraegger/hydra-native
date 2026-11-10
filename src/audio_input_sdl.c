// The audio input of macOS and Windows (src/audio_input.h): the recording devices of SDL3 (Core Audio, WASAPI). A
// device name selects the first recording device whose name contains it. SDL converts the samples to mono floats at
// AUDIO_SAMPLE_RATE and calls on_data() when it has added some; audio_input_read() waits for a full frame.
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "audio_input.h"

#define WAIT_MS 100 // a wait for samples ends after this time, so that the loop sees an error of the stream

static SDL_AudioStream *stream;
static SDL_Semaphore *data_ready;

static void SDLCALL on_data(void *data, SDL_AudioStream *source, int additional_amount, int total_amount) {
    SDL_SignalSemaphore(data_ready);
}

static SDL_AudioDeviceID find_device(const char *name) {
    int count = 0;
    SDL_AudioDeviceID *devices = SDL_GetAudioRecordingDevices(&count);
    SDL_AudioDeviceID found = 0;
    for (int i = 0; i < count && !found; i++) {
        const char *device_name = SDL_GetAudioDeviceName(devices[i]);
        if (device_name && strstr(device_name, name)) found = devices[i];
    }
    if (!found) {
        fprintf(stderr, "audio: no recording device with \"%s\" in its name; the devices are:\n", name);
        for (int i = 0; i < count; i++) fprintf(stderr, "  %s\n", SDL_GetAudioDeviceName(devices[i]));
    }
    SDL_free(devices);
    return found;
}

bool audio_input_init(void) {
    if (SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO)) return true;
    fprintf(stderr, "audio: %s\n", SDL_GetError());
    return false;
}

bool audio_input_open(const char *name) {
    if (!strcmp(name, AUDIO_SYSTEM_OUTPUT)) {
        fprintf(stderr, "audio: this system has no capture of its output\n");
        return false;
    }
    SDL_AudioDeviceID device = name[0] ? find_device(name) : SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (!device) return false;
    const SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, AUDIO_SAMPLE_RATE };
    data_ready = SDL_CreateSemaphore(0);
    stream = data_ready ? SDL_OpenAudioDeviceStream(device, &spec, on_data, NULL) : NULL;
    if (stream && SDL_ResumeAudioStreamDevice(stream)) return true;
    fprintf(stderr, "audio: cannot open the input %s: %s\n", name[0] ? name : "(default)", SDL_GetError());
    audio_input_close();
    return false;
}

bool audio_input_read(float frame[AUDIO_FRAME_SIZE]) {
    const int size = AUDIO_FRAME_SIZE * (int)sizeof(float);
    for (;;) {
        int available = SDL_GetAudioStreamAvailable(stream);
        if (available < 0) break;
        if (available >= size) return SDL_GetAudioStreamData(stream, frame, size) == size;
        SDL_WaitSemaphoreTimeout(data_ready, WAIT_MS);
    }
    fprintf(stderr, "audio: read failed: %s\n", SDL_GetError());
    return false;
}

void audio_input_close(void) {
    SDL_DestroyAudioStream(stream); // also closes the device
    SDL_DestroySemaphore(data_ready);
    stream = NULL;
    data_ready = NULL;
}
