// Computes the same "loudness" feature as Meyda (used by hydra-synth's lib/audio.js):
// a 512-sample Hann window, the FFT amplitude spectrum, 24 Bark bands, specific[i] = sum^0.23.
// Then a.setScale/setCutoff values from browser sketches keep their meaning. Before that, the WebRTC audio
// processing of Chrome (src/webrtc_apm.cc) or, without that library, src/audio_processing.c applies the
// processing of getUserMedia().
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "audio_input.h"
#include "audio_processing.h"
#include "native.h"
#include "webrtc_apm.h"

// The audio processing of Chrome (src/webrtc_apm.cc), when the library is there.
static struct {
    void *apm;
    bool (*process)(void *apm, float *samples, int sample_rate);
} webrtc;

// WEBRTC_APM_LIBRARY_NAME is empty on macOS and Windows, which have no such library.
static void load_webrtc_processing(void) {
    if (!WEBRTC_APM_LIBRARY_NAME[0]) return;
    char library[1024];
    snprintf(library, sizeof library, "%s" WEBRTC_APM_LIBRARY_NAME, SDL_GetBasePath());
    SDL_SharedObject *handle = SDL_LoadObject(library);
    if (!handle) {
        fprintf(stderr, "audio: no WebRTC audio processing (%s), using the own noise suppression and gain control\n",
                SDL_GetError());
        return;
    }
    void *(*create)(int) = (void *(*)(int))SDL_LoadFunction(handle, "webrtc_apm_create");
    webrtc.process = (bool (*)(void *, float *, int))SDL_LoadFunction(handle, "webrtc_apm_process");
    webrtc.apm = create && webrtc.process ? create(AUDIO_SAMPLE_RATE) : NULL;
}

static bool started;
static SDL_SpinLock lock;
static bool available;
static float latest_specific[BARK_BAND_COUNT];
static float latest_total;
static char device_name[256];

static void compute_band_limits(int limits[BARK_BAND_COUNT + 1]) {
    float bark[SPECTRUM_SIZE];
    for (int i = 0; i < SPECTRUM_SIZE; i++) {
        float frequency = (float)i * AUDIO_SAMPLE_RATE / AUDIO_BUFFER_SIZE;
        bark[i] = 13.0f * atanf(frequency / 1315.8f) + 3.5f * atanf(powf(frequency / 7518.0f, 2.0f));
    }
    float last = bark[SPECTRUM_SIZE - 1];
    float band_end = last / BARK_BAND_COUNT;
    int band = 1;
    limits[0] = 0;
    for (int i = 0; i < SPECTRUM_SIZE && band < BARK_BAND_COUNT; i++) {
        while (bark[i] > band_end && band < BARK_BAND_COUNT) {
            limits[band++] = i;
            band_end = band * last / BARK_BAND_COUNT;
        }
    }
    limits[BARK_BAND_COUNT] = SPECTRUM_SIZE - 1;
}

static int capture(void *argument) {
    (void)argument;
    if (!audio_input_open(device_name)) return 0;
    int limits[BARK_BAND_COUNT + 1];
    compute_band_limits(limits);
    load_webrtc_processing();
    struct audio_processor processor = { 0 };
    // Processed 10 ms frames collect here until a buffer of the size of Meyda is full.
    float pending[AUDIO_BUFFER_SIZE + AUDIO_FRAME_SIZE], magnitude[SPECTRUM_SIZE];
    size_t pending_count = 0;
    while (audio_input_read(pending + pending_count)) {
        if (webrtc.apm) webrtc.process(webrtc.apm, pending + pending_count, AUDIO_SAMPLE_RATE);
        pending_count += AUDIO_FRAME_SIZE;
        if (pending_count < AUDIO_BUFFER_SIZE) continue;
        audio_analyze(webrtc.apm ? NULL : &processor, pending, magnitude);
        pending_count -= AUDIO_BUFFER_SIZE;
        memmove(pending, pending + AUDIO_BUFFER_SIZE, pending_count * sizeof(float));
        float specific[BARK_BAND_COUNT], total = 0.0f;
        for (int band = 0; band < BARK_BAND_COUNT; band++) {
            float sum = 0.0f;
            for (int bin = limits[band]; bin < limits[band + 1]; bin++)
                sum += magnitude[bin];
            specific[band] = powf(sum, 0.23f);
            total += specific[band];
        }
        SDL_LockSpinlock(&lock);
        memcpy(latest_specific, specific, sizeof specific);
        latest_total = total;
        available = true;
        SDL_UnlockSpinlock(&lock);
    }
    audio_input_close();
    return 0;
}

bool audio_start(const char *device) {
    if (started) return true;
    if (!audio_input_init()) return false;
    snprintf(device_name, sizeof device_name, "%s", device ? device : "");
    SDL_Thread *thread = SDL_CreateThread(capture, "audio", NULL);
    started = thread != NULL;
    SDL_DetachThread(thread);
    return started;
}

bool audio_features(float specific[BARK_BAND_COUNT], float *total) {
    SDL_LockSpinlock(&lock);
    bool result = available;
    if (result) {
        memcpy(specific, latest_specific, sizeof latest_specific);
        *total = latest_total;
    }
    SDL_UnlockSpinlock(&lock);
    return result;
}
