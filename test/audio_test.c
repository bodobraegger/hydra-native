// Checks the gain control and noise suppression of src/audio_processing.c with a tone that starts and stops
// over a quiet noise, like speech. Run with `make test`.
#include <math.h>
#include <stdio.h>

#include "../src/audio_processing.h"

#define TONE_HZ 1000.0f
#define BURST_SECONDS 0.5f
#define NOISE_AMPLITUDE 0.0003f // about -80 dBFS RMS

static int failures;

#define EXPECT(condition, ...)                                         \
    do {                                                               \
        if (!(condition)) {                                            \
            fprintf(stderr, "%s:%d: %s: ", __FILE__, __LINE__, #condition); \
            fprintf(stderr, __VA_ARGS__);                              \
            fprintf(stderr, "\n");                                     \
            failures++;                                                \
        }                                                              \
    } while (0)

static unsigned random_state = 1;
static float noise_sample(void) {
    random_state = random_state * 1103515245u + 12345u;
    return NOISE_AMPLITUDE * ((float)(random_state >> 8) / (float)(1u << 24) * 2.0f - 1.0f) * sqrtf(3.0f);
}

static long sample_index;

// Runs `seconds` of bursts of a tone with RMS level `level_dbfs`; returns the mean magnitude of the tone bin
// in the last burst and the mean magnitude of the bins above the tone in the last pause.
static void run(struct audio_processor *processor, float seconds, float level_dbfs, float *tone, float *noise) {
    float amplitude = sqrtf(2.0f) * powf(10.0f, level_dbfs / 20.0f), samples[AUDIO_BUFFER_SIZE], magnitude[SPECTRUM_SIZE];
    int tone_bin = (int)lroundf(TONE_HZ * AUDIO_BUFFER_SIZE / AUDIO_SAMPLE_RATE), noise_bin = tone_bin * 3;
    long frames = (long)(seconds * AUDIO_SAMPLE_RATE / AUDIO_BUFFER_SIZE);
    for (long frame = 0; frame < frames; frame++) {
        bool on = fmodf((float)sample_index / AUDIO_SAMPLE_RATE, 2 * BURST_SECONDS) < BURST_SECONDS;
        for (int i = 0; i < AUDIO_BUFFER_SIZE; i++, sample_index++)
            samples[i] = noise_sample() + (on ? amplitude * sinf(2.0f * (float)M_PI * TONE_HZ * (float)sample_index / AUDIO_SAMPLE_RATE) : 0.0f);
        audio_analyze(processor, samples, magnitude);
        if (on) *tone = magnitude[tone_bin];
        else {
            *noise = 0;
            for (int bin = noise_bin; bin < SPECTRUM_SIZE; bin++) *noise += magnitude[bin] / (SPECTRUM_SIZE - noise_bin);
        }
    }
}

int main(void) {
    struct audio_processor processor = { 0 };
    float tone = 0, noise = 0;
    run(&processor, 1.0f, -46.0f, &tone, &noise);
    fprintf(stderr, "after 1 s: gain %.1f dB, speech level %.1f dBFS\n", processor.gain_db, processor.speech_level_db);
    EXPECT(processor.gain_db < 11.5f, "the gain rises at most 3 dB/s from 8 dB, got %.1f", processor.gain_db);
    run(&processor, 20.0f, -46.0f, &tone, &noise);
    fprintf(stderr, "after 21 s: gain %.1f dB, speech level %.1f dBFS, tone %.1f, noise %.4f\n", processor.gain_db,
            processor.speech_level_db, tone, noise);
    EXPECT(fabsf(processor.speech_level_db + 46.0f) < 2.0f, "the level estimate is %.1f dBFS", processor.speech_level_db);
    EXPECT(fabsf(processor.gain_db - 20.0f) < 1.5f, "the gain brings -46 dBFS to the target -26, got %.1f", processor.gain_db);

    // Suppression: in the pauses, the noise is about 18 dB below the noise with the same gain and no suppression.
    // The mean magnitude of windowed white noise is sqrt(pi/4 * N * mean(hann^2)) times its RMS.
    float unsuppressed = NOISE_AMPLITUDE * sqrtf((float)M_PI / 4 * AUDIO_BUFFER_SIZE * 0.375f) * powf(10.0f, processor.gain_db / 20);
    fprintf(stderr, "noise %.4f, without suppression %.4f\n", noise, unsuppressed);
    EXPECT(noise < unsuppressed * 0.25f, "noise %.4f, without suppression %.4f", noise, unsuppressed);

    // A loud voice: the gain falls slowly, the limiter keeps the peaks below -1 dBFS at once.
    float gain_before = processor.gain_db;
    run(&processor, 2.0f, -10.0f, &tone, &noise);
    fprintf(stderr, "after 2 s loud: gain %.1f dB, tone %.1f\n", processor.gain_db, tone);
    EXPECT(processor.gain_db < gain_before && processor.gain_db > gain_before - 6.5f, "gain %.1f", processor.gain_db);
    float limited = sqrtf(2.0f) * powf(10.0f, -10.0f / 20.0f) * AUDIO_BUFFER_SIZE / 4.0f * powf(10.0f, -1.0f / 20.0f) /
                    (sqrtf(2.0f) * powf(10.0f, -10.0f / 20.0f));
    EXPECT(tone <= limited * 1.05f, "tone %.1f, limit %.1f", tone, limited);

    if (failures) {
        fprintf(stderr, "audio_test: %d failures\n", failures);
        return 1;
    }
    fprintf(stderr, "audio_test: ok\n");
    return 0;
}
