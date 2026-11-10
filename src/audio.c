// Computes the same "loudness" feature as Meyda (used by hydra-synth's lib/audio.js):
// a 512-sample Hann window, the FFT amplitude spectrum, 24 Bark bands, specific[i] = sum^0.23.
// Then a.setScale/setCutoff values from browser sketches keep their meaning.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <pulse/error.h>
#include <pulse/simple.h>

#include "native.h"

#define AUDIO_BUFFER_SIZE 512
#define AUDIO_SAMPLE_RATE 48000
#define SPECTRUM_SIZE (AUDIO_BUFFER_SIZE / 2)

static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool started;
static bool available;
static float latest_specific[BARK_BAND_COUNT];
static float latest_total;
static char device_name[256];

static void fft(float *real, float *imaginary, int size) {
    for (int i = 1, j = 0; i < size; i++) {
        int bit = size >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float swap = real[i]; real[i] = real[j]; real[j] = swap;
            swap = imaginary[i]; imaginary[i] = imaginary[j]; imaginary[j] = swap;
        }
    }
    for (int length = 2; length <= size; length <<= 1) {
        float angle = -2.0f * (float)M_PI / (float)length;
        for (int start = 0; start < size; start += length) {
            for (int k = 0; k < length / 2; k++) {
                float w_real = cosf(angle * k), w_imaginary = sinf(angle * k);
                int even = start + k, odd = start + k + length / 2;
                float odd_real = real[odd] * w_real - imaginary[odd] * w_imaginary;
                float odd_imaginary = real[odd] * w_imaginary + imaginary[odd] * w_real;
                real[odd] = real[even] - odd_real;
                imaginary[odd] = imaginary[even] - odd_imaginary;
                real[even] += odd_real;
                imaginary[even] += odd_imaginary;
            }
        }
    }
}

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
        .fragsize = AUDIO_BUFFER_SIZE * sizeof(float),
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
    float window[AUDIO_BUFFER_SIZE];
    for (int i = 0; i < AUDIO_BUFFER_SIZE; i++)
        window[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (AUDIO_BUFFER_SIZE - 1));

    float samples[AUDIO_BUFFER_SIZE], real[AUDIO_BUFFER_SIZE], imaginary[AUDIO_BUFFER_SIZE];
    while (pa_simple_read(stream, samples, sizeof samples, &error) >= 0) {
        for (int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
            real[i] = samples[i] * window[i];
            imaginary[i] = 0.0f;
        }
        fft(real, imaginary, AUDIO_BUFFER_SIZE);
        float specific[BARK_BAND_COUNT], total = 0.0f;
        for (int band = 0; band < BARK_BAND_COUNT; band++) {
            float sum = 0.0f;
            for (int bin = limits[band]; bin < limits[band + 1]; bin++)
                sum += sqrtf(real[bin] * real[bin] + imaginary[bin] * imaginary[bin]);
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
