// The amplitude spectrum that the loudness feature of Meyda uses, after the processing that getUserMedia() of a
// browser applies to the microphone by default (noiseSuppression and autoGainControl). It has no PulseAudio
// dependency, so test/audio_test.c can check it alone.
#pragma once

#include <stdbool.h>

#define AUDIO_BUFFER_SIZE 512
#define AUDIO_SAMPLE_RATE 48000
#define SPECTRUM_SIZE (AUDIO_BUFFER_SIZE / 2)

struct audio_processor {
    bool started;
    float noise[SPECTRUM_SIZE];          // the noise power of each bin
    float smoothed_power[SPECTRUM_SIZE]; // the power of each bin, smoothed for the noise tracking
    float previous_clean[SPECTRUM_SIZE]; // the power after suppression of the last frame
    float speech_level_db;               // the RMS level of active frames, in dBFS
    float gain_db;
};

// The amplitude spectrum of the Hann-windowed frame, with noise suppression and gain. Start with a zeroed processor;
// without a processor (NULL), the spectrum stays unprocessed.
void audio_analyze(struct audio_processor *processor, const float samples[AUDIO_BUFFER_SIZE], float magnitude[SPECTRUM_SIZE]);
