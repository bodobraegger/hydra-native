// Noise suppression and automatic gain control in the manner of the WebRTC audio processing module of Chrome.
// Noise suppression: a Wiener filter with the decision-directed estimate of the a priori SNR and a noise power
// that follows the minimum of the smoothed power of each bin. The gain floor is the 18 dB of the "high" level.
// Gain control: the adaptive digital controller of AGC2. It estimates the RMS level of active frames, moves the
// gain at most 3 dB per second towards the gain that brings this level to the target, keeps the gain below
// 30 dB and below the gain that lifts the noise above -50 dBFS, and limits the peaks.
// Echo cancellation is missing on purpose: the browser cancels only the sound that it plays itself, and hydra
// plays no sound, so the browser passes the microphone signal unchanged as well.
#include "audio_processing.h"

#include <math.h>

#define FRAME_SECONDS ((float)AUDIO_BUFFER_SIZE / AUDIO_SAMPLE_RATE)
#define POWER_SMOOTHING 0.7f
#define NOISE_RISE_DB_PER_SECOND 3.0f
#define PRIOR_SNR_SMOOTHING 0.98f // the decision-directed factor of WebRTC NS
#define SUPPRESSION_FLOOR_DB -18.0f
#define TARGET_LEVEL_DBFS -26.0f // headroom 6 dB plus the initial 20 dB of the AGC2 saturation protector
#define INITIAL_GAIN_DB 8.0f
#define MAX_GAIN_DB 30.0f
#define MAX_GAIN_CHANGE_DB_PER_SECOND 3.0f
#define MAX_OUTPUT_NOISE_DBFS -50.0f
#define ACTIVE_MARGIN_DB 10.0f // a frame this far above the noise counts as speech (AGC2 uses a neural VAD)
#define LEVEL_TIME_SECONDS 0.4f
#define LIMITER_DBFS -1.0f
#define SILENCE 1e-12f

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

static float to_db(float power) { return 10.0f * log10f(power + SILENCE); }
static float from_db(float db) { return powf(10.0f, db / 20.0f); }

// The RMS power of the frame from its windowed spectrum (Parseval, corrected for the Hann window).
static float frame_power(const float *power) {
    float sum = 0.0f;
    for (int bin = 1; bin < SPECTRUM_SIZE; bin++) sum += power[bin];
    const float hann_mean_square = 0.375f;
    return 2.0f * (sum + power[0] / 2.0f) / ((float)AUDIO_BUFFER_SIZE * AUDIO_BUFFER_SIZE * hann_mean_square);
}

void audio_analyze(struct audio_processor *processor, const float samples[AUDIO_BUFFER_SIZE],
                   float magnitude[SPECTRUM_SIZE]) {
    float real[AUDIO_BUFFER_SIZE], imaginary[AUDIO_BUFFER_SIZE], peak = 0.0f;
    for (int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
        real[i] = samples[i] * (0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (AUDIO_BUFFER_SIZE - 1)));
        imaginary[i] = 0.0f;
        peak = fmaxf(peak, fabsf(samples[i]));
    }
    fft(real, imaginary, AUDIO_BUFFER_SIZE);
    float power[SPECTRUM_SIZE], clean[SPECTRUM_SIZE];
    for (int bin = 0; bin < SPECTRUM_SIZE; bin++) {
        power[bin] = real[bin] * real[bin] + imaginary[bin] * imaginary[bin];
        magnitude[bin] = sqrtf(power[bin]);
    }
    if (!processor->started) {
        for (int bin = 0; bin < SPECTRUM_SIZE; bin++) {
            float initial_power = power[bin] + SILENCE;
            processor->noise[bin] = processor->smoothed_power[bin] = processor->previous_clean[bin] = initial_power;
        }
        processor->speech_level_db = TARGET_LEVEL_DBFS - INITIAL_GAIN_DB;
        processor->gain_db = INITIAL_GAIN_DB;
        processor->started = true;
    }

    float noise_rise = powf(10.0f, NOISE_RISE_DB_PER_SECOND * FRAME_SECONDS / 10.0f);
    float floor_gain = from_db(SUPPRESSION_FLOOR_DB);
    for (int bin = 0; bin < SPECTRUM_SIZE; bin++) {
        float *smoothed = &processor->smoothed_power[bin], *noise = &processor->noise[bin];
        *smoothed = POWER_SMOOTHING * *smoothed + (1.0f - POWER_SMOOTHING) * power[bin];
        *noise = *smoothed < *noise ? *smoothed : *noise * noise_rise;
        float posterior_snr = power[bin] / (*noise + SILENCE);
        float prior_snr = PRIOR_SNR_SMOOTHING * processor->previous_clean[bin] / (*noise + SILENCE) +
                          (1.0f - PRIOR_SNR_SMOOTHING) * fmaxf(posterior_snr - 1.0f, 0.0f);
        float gain = fmaxf(prior_snr / (1.0f + prior_snr), floor_gain);
        magnitude[bin] *= gain;
        clean[bin] = processor->previous_clean[bin] = power[bin] * gain * gain;
    }

    float level_db = to_db(frame_power(clean)), noise_db = to_db(frame_power(processor->noise));
    if (level_db > noise_db + ACTIVE_MARGIN_DB)
        processor->speech_level_db += (level_db - processor->speech_level_db) * (FRAME_SECONDS / LEVEL_TIME_SECONDS);
    float wanted_db = fminf(fmaxf(TARGET_LEVEL_DBFS - processor->speech_level_db, 0.0f), MAX_GAIN_DB);
    wanted_db = fmaxf(fminf(wanted_db, MAX_OUTPUT_NOISE_DBFS - noise_db), 0.0f);
    float step = MAX_GAIN_CHANGE_DB_PER_SECOND * FRAME_SECONDS;
    processor->gain_db += fminf(fmaxf(wanted_db - processor->gain_db, -step), step);

    float gain = from_db(processor->gain_db), limit = from_db(LIMITER_DBFS);
    if (peak * gain > limit) gain = limit / peak;
    for (int bin = 0; bin < SPECTRUM_SIZE; bin++) magnitude[bin] *= gain;
}
