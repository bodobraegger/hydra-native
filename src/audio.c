// Computes the same "loudness" feature as Meyda (used by hydra-synth's lib/audio.js):
// a 512-sample Hann window, the FFT amplitude spectrum, 24 Bark bands, specific[i] = sum^0.23.
// Then a.setScale/setCutoff values from browser sketches keep their meaning. Before that, src/audio_processing.c
// applies the processing of getUserMedia().
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "audio_input.h"
#include "audio_processing.h"
#include "native.h"

static char program_directory[1024]; // of hydra-native-capture, for the system audio of macOS

#define DEVICE_WAIT_MS 50

static void analyze(void);
static bool started;
static SDL_SpinLock lock;
static bool available;
static float latest_specific[BARK_BAND_COUNT];
static float latest_total;
static char device_name[256];
static SDL_AtomicInt device_changed; // audio_set_device() asks the thread to open the new device
static SDL_AtomicInt gain_percent = { 100 };

void audio_set_gain(int percent) { SDL_SetAtomicInt(&gain_percent, percent); }

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
    for (;;) {
        char device[sizeof device_name];
        SDL_LockSpinlock(&lock);
        snprintf(device, sizeof device, "%s", device_name);
        SDL_UnlockSpinlock(&lock);
        SDL_SetAtomicInt(&device_changed, 0);
        if (strcmp(device, AUDIO_NONE) != 0 && audio_input_open(device, program_directory)) {
            analyze(); // until the device fails or changes
            audio_input_close();
        }
        SDL_LockSpinlock(&lock);
        available = false;
        SDL_UnlockSpinlock(&lock);
        // A failed or switched-off input waits for the next change.
        while (!SDL_GetAtomicInt(&device_changed)) SDL_Delay(DEVICE_WAIT_MS);
    }
    return 0;
}

static void analyze(void) {
    int limits[BARK_BAND_COUNT + 1];
    compute_band_limits(limits);
    struct audio_processor processor = { 0 };
    // Processed 10 ms frames collect here until a buffer of the size of Meyda is full.
    float pending[AUDIO_BUFFER_SIZE + AUDIO_FRAME_SIZE], magnitude[SPECTRUM_SIZE];
    size_t pending_count = 0;
    while (!SDL_GetAtomicInt(&device_changed) && audio_input_read(pending + pending_count)) {
        // The gain of the settings, for quiet inputs such as the system output, which has no automatic gain.
        float gain = (float)SDL_GetAtomicInt(&gain_percent) / 100.0f;
        for (int i = 0; i < AUDIO_FRAME_SIZE; i++) pending[pending_count + i] *= gain;
        pending_count += AUDIO_FRAME_SIZE;
        if (pending_count < AUDIO_BUFFER_SIZE) continue;
        audio_analyze(&processor, pending, magnitude);
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
}

void audio_set_device(const char *device) {
    SDL_LockSpinlock(&lock);
    snprintf(device_name, sizeof device_name, "%s", device ? device : "");
    SDL_UnlockSpinlock(&lock);
    SDL_SetAtomicInt(&device_changed, 1);
}

int audio_devices(struct audio_device *devices, int capacity) { return audio_input_devices(devices, capacity); }

bool audio_start(const char *device, const char *directory) {
    if (started) return true;
    if (!audio_input_init()) return false;
    audio_set_device(device);
    snprintf(program_directory, sizeof program_directory, "%s", directory);
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
