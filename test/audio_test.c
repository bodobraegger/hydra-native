// Checks the gain control and noise suppression of src/audio_processing.c with a tone that starts and stops
// over a quiet noise, like speech. Run with `make test`.
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../src/analog_agc.h"
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

// The WebRTC audio processing of the system (build/libhydra-webrtc-apm.so): a quiet tone becomes louder.
static void check_webrtc_processing(void) {
    void *library = dlopen("build/libhydra-webrtc-apm.so", RTLD_NOW);
    if (!library) {
        fprintf(stderr, "no build/libhydra-webrtc-apm.so (%s), skipped\n", dlerror());
        return;
    }
    void *(*create)(int, bool) = (void *(*)(int, bool))dlsym(library, "webrtc_apm_create");
    bool (*process)(void *, float *, int, int *) = (bool (*)(void *, float *, int, int *))dlsym(library, "webrtc_apm_process");
    void *apm = create(AUDIO_SAMPLE_RATE, false);
    const int frame = AUDIO_SAMPLE_RATE / 100;
    float amplitude = sqrtf(2.0f) * powf(10.0f, -46.0f / 20.0f), samples[AUDIO_SAMPLE_RATE / 100];
    double input = 0, output = 0;
    long index = 0;
    for (int f = 0; f < 2000; f++) { // 20 s of speech-like bursts
        bool on = (f / 50) % 2 == 0;
        for (int i = 0; i < frame; i++, index++)
            samples[i] = noise_sample() + (on ? amplitude * sinf(2.0f * (float)M_PI * 440.0f * (float)index / AUDIO_SAMPLE_RATE) : 0);
        double before = 0;
        for (int i = 0; i < frame; i++) before += samples[i] * samples[i];
        EXPECT(process(apm, samples, AUDIO_SAMPLE_RATE, NULL), "ProcessStream failed");
        if (f >= 1800 && on) {
            input += before;
            for (int i = 0; i < frame; i++) output += samples[i] * samples[i];
        }
    }
    ((void (*)(void *))dlsym(library, "webrtc_apm_destroy"))(apm);
    double gain_db = 10 * log10(output / input);
    fprintf(stderr, "WebRTC processing: gain %.1f dB on a -46 dBFS tone after 18 s\n", gain_db);
    EXPECT(gain_db > 6, "the AGC2 of the system library gives %.1f dB", gain_db);
}

// A fake source volume: the test never touches a real microphone.
struct fake_source {
    double volume, saved;
    int saves, restores, sets;
};

static bool fake_save(void *data) {
    struct fake_source *source = data;
    source->saved = source->volume;
    source->saves++;
    return true;
}

static bool fake_restore(void *data) {
    struct fake_source *source = data;
    source->volume = source->saved;
    source->restores++;
    return true;
}

static bool fake_get(void *data, double *volume) {
    *volume = ((struct fake_source *)data)->volume;
    return true;
}

static bool fake_set(void *data, double volume) {
    struct fake_source *source = data;
    source->volume = volume;
    source->sets++;
    return true;
}

#define FAKE_START_VOLUME 0.2
#define SPEECH_DIRECTORY "/usr/share/sounds/alsa/"
#define MAX_SPEECH_SAMPLES (AUDIO_SAMPLE_RATE * 20)

// The spoken channel names of alsa-utils (16-bit mono WAV at 48 kHz): the voice detector of the analog gain
// control reacts to real speech only. Returns the number of samples, scaled to a peak of 1.
static size_t load_speech(float *speech) {
    static const char *const names[] = { "Front_Center", "Front_Left", "Front_Right", "Rear_Left", "Rear_Right", "Side_Left" };
    size_t count = 0;
    float peak = 0;
    for (size_t n = 0; n < sizeof names / sizeof names[0]; n++) {
        char path[256];
        snprintf(path, sizeof path, SPEECH_DIRECTORY "%s.wav", names[n]);
        FILE *file = fopen(path, "rb");
        if (!file) continue;
        unsigned char header[8];
        fseek(file, 12, SEEK_SET); // after RIFF, size, WAVE
        while (fread(header, 1, 8, file) == 8) {
            unsigned size = header[4] | header[5] << 8 | header[6] << 16 | (unsigned)header[7] << 24;
            if (memcmp(header, "data", 4) != 0) {
                fseek(file, size, SEEK_CUR);
                continue;
            }
            for (unsigned i = 0; i < size / 2 && count < MAX_SPEECH_SAMPLES; i++) {
                unsigned char bytes[2];
                if (fread(bytes, 1, 2, file) != 2) break;
                float value = (float)(int16_t)(bytes[0] | bytes[1] << 8) / 32768.0f;
                speech[count++] = value;
                if (fabsf(value) > peak) peak = fabsf(value);
            }
            break;
        }
        fclose(file);
    }
    for (size_t i = 0; i < count && peak > 0; i++) speech[i] /= peak;
    return count;
}

// The analog gain control of the WebRTC library with a fake source whose volume scales the signal, as the gain
// of a microphone does: a quiet voice raises the volume, a clipping one lowers it, and the end restores it.
static void check_analog_gain(void) {
    void *library = dlopen("build/libhydra-webrtc-apm.so", RTLD_NOW);
    static float speech[MAX_SPEECH_SAMPLES];
    size_t speech_count = load_speech(speech);
    if (!library || !speech_count) {
        fprintf(stderr, "no WebRTC library or no speech samples in " SPEECH_DIRECTORY ", analog gain skipped\n");
        return;
    }
    void *(*create)(int, bool) = (void *(*)(int, bool))dlsym(library, "webrtc_apm_create");
    bool (*process)(void *, float *, int, int *) = (bool (*)(void *, float *, int, int *))dlsym(library, "webrtc_apm_process");
    void *apm = create(AUDIO_SAMPLE_RATE, true);
    struct fake_source source = { FAKE_START_VOLUME, 0, 0, 0, 0 };
    struct analog_agc agc = { .lock = PTHREAD_MUTEX_INITIALIZER };
    EXPECT(analog_agc_start(&agc, (struct source_volume){ fake_save, fake_restore, fake_get, fake_set, &source }), "start failed");
    const int frame = AUDIO_SAMPLE_RATE / 100;
    float samples[AUDIO_SAMPLE_RATE / 100];
    size_t index = 0;
    double lowest = 1, highest = 0;
    for (int f = 0; f < 6000; f++) { // 30 s of a quiet voice, then 30 s of a clipping one
        bool loud = f >= 3000;
        float gain = (float)((loud ? 4.0 : 0.02) * source.volume / FAKE_START_VOLUME);
        for (int i = 0; i < frame; i++, index++) {
            float value = noise_sample() + gain * speech[index % speech_count];
            samples[i] = value > 1 ? 1 : value < -1 ? -1 : value;
        }
        int level = analog_agc_level(&agc);
        process(apm, samples, AUDIO_SAMPLE_RATE, &level);
        analog_agc_apply(&agc, level);
        if (getenv("AGC_TRACE") && f % 200 == 0) fprintf(stderr, "frame %d level %d volume %.3f\n", f, level, source.volume);
        if (f == 2999) highest = source.volume;
        if (f >= 3000 && source.volume < lowest) lowest = source.volume;
    }
    fprintf(stderr, "analog gain: quiet voice %.3f -> %.3f, clipping voice -> %.3f, %d volume changes\n", FAKE_START_VOLUME,
            highest, lowest, source.sets);
    EXPECT(highest > FAKE_START_VOLUME, "a quiet voice does not raise the volume: %.3f", highest);
    EXPECT(lowest < highest, "a clipping voice does not lower the volume: %.3f", lowest);
    analog_agc_stop(&agc);
    analog_agc_stop(&agc);
    EXPECT(source.volume == FAKE_START_VOLUME && source.saves == 1 && source.restores == 1, "volume %.3f, %d restores",
           source.volume, source.restores);
    analog_agc_apply(&agc, 100);
    EXPECT(source.volume == FAKE_START_VOLUME, "a stopped control changed the volume");
    ((void (*)(void *))dlsym(library, "webrtc_apm_destroy"))(apm);
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

    check_webrtc_processing();
    check_analog_gain();
    if (failures) {
        fprintf(stderr, "audio_test: %d failures\n", failures);
        return 1;
    }
    fprintf(stderr, "audio_test: ok\n");
    return 0;
}
