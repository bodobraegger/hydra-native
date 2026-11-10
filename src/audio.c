// Computes the same "loudness" feature as Meyda (used by hydra-synth's lib/audio.js):
// a 512-sample Hann window, the FFT amplitude spectrum, 24 Bark bands, specific[i] = sum^0.23.
// Then a.setScale/setCutoff values from browser sketches keep their meaning. Before that, the WebRTC audio
// processing of Chrome (src/webrtc_apm.cc) or, without the system library, src/audio_processing.c applies the
// processing of getUserMedia().
#include <dlfcn.h>
#include <limits.h>
#include <libgen.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include <pulse/error.h>
#include <pulse/simple.h>

#include "audio_processing.h"
#include "native.h"

#define APM_FRAME_SIZE (AUDIO_SAMPLE_RATE / 100) // the WebRTC audio processing takes 10 ms frames
#define APM_LIBRARY_NAME "libhydra-webrtc-apm.so"

// The audio processing of Chrome (src/webrtc_apm.cc), when the system library is installed.
static struct {
    void *apm;
    bool (*process)(void *apm, float *samples, int sample_rate);
} webrtc;

static void load_webrtc_processing(void) {
    char path[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", path, sizeof path - 1);
    if (length <= 0) return;
    path[length] = '\0';
    char library[PATH_MAX + sizeof APM_LIBRARY_NAME];
    snprintf(library, sizeof library, "%s/" APM_LIBRARY_NAME, dirname(path));
    void *handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "audio: no WebRTC audio processing (%s), using the own noise suppression and gain control\n", dlerror());
        return;
    }
    void *(*create)(int) = (void *(*)(int))dlsym(handle, "webrtc_apm_create");
    webrtc.process = (bool (*)(void *, float *, int))dlsym(handle, "webrtc_apm_process");
    webrtc.apm = create && webrtc.process ? create(AUDIO_SAMPLE_RATE) : NULL;
}

static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool started;
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

static void *capture(void *argument) {
    (void)argument;
    pa_sample_spec spec = { .format = PA_SAMPLE_FLOAT32LE, .rate = AUDIO_SAMPLE_RATE, .channels = 1 };
    pa_buffer_attr attributes = {
        .maxlength = (uint32_t)-1,
        .fragsize = APM_FRAME_SIZE * sizeof(float),
    };
    int error = 0;
    pa_simple *stream = pa_simple_new(NULL, "hydra-native", PA_STREAM_RECORD, device_name[0] ? device_name : NULL,
                                      "audio fft", &spec, NULL, &attributes, &error);
    if (!stream) {
        fprintf(stderr, "audio: cannot open source %s: %s\n", device_name[0] ? device_name : "(default)",
                pa_strerror(error));
        return NULL;
    }

    int limits[BARK_BAND_COUNT + 1];
    compute_band_limits(limits);
    load_webrtc_processing();
    struct audio_processor processor = { 0 };
    // Processed 10 ms frames collect here until a buffer of the size of Meyda is full.
    float pending[AUDIO_BUFFER_SIZE + APM_FRAME_SIZE], magnitude[SPECTRUM_SIZE];
    size_t pending_count = 0;
    while (pa_simple_read(stream, pending + pending_count, APM_FRAME_SIZE * sizeof(float), &error) >= 0) {
        if (webrtc.apm) webrtc.process(webrtc.apm, pending + pending_count, AUDIO_SAMPLE_RATE);
        pending_count += APM_FRAME_SIZE;
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
        pthread_mutex_lock(&lock);
        memcpy(latest_specific, specific, sizeof specific);
        latest_total = total;
        available = true;
        pthread_mutex_unlock(&lock);
    }
    fprintf(stderr, "audio: read failed: %s\n", pa_strerror(error));
    pa_simple_free(stream);
    return NULL;
}

bool audio_start(const char *device) {
    if (started) return true;
    snprintf(device_name, sizeof device_name, "%s", device ? device : "");
    started = pthread_create(&thread, NULL, capture, NULL) == 0;
    if (started) pthread_detach(thread);
    return started;
}

bool audio_features(float specific[BARK_BAND_COUNT], float *total) {
    pthread_mutex_lock(&lock);
    bool result = available;
    if (result) {
        memcpy(specific, latest_specific, sizeof latest_specific);
        *total = latest_total;
    }
    pthread_mutex_unlock(&lock);
    return result;
}
