// The analog gain control of Chrome on Linux: the WebRTC audio processing recommends a volume for the capture
// source, and this module reads and sets the source volume through `struct source_volume`, which the backends of
// src/audio_input.h implement for each system and test/audio_test.c with a fake. The original volume comes back at
// the end.
#pragma once

#include <SDL3/SDL_atomic.h>
#include <stdbool.h>

// Volumes are fractions of the normal volume of the source (PA_VOLUME_NORM of PulseAudio, the scalar volume of
// Core Audio and of IAudioEndpointVolume are 1).
struct source_volume {
    bool (*save)(void *data);    // remembers the volume of all channels
    bool (*restore)(void *data); // sets the remembered volume again
    bool (*get)(void *data, double *volume);
    bool (*set)(void *data, double volume);
    void *data;
};

// All fields start zeroed.
struct analog_agc {
    struct source_volume volume;
    SDL_SpinLock lock;
    bool active;
    int level;              // the last level that was read or set
    unsigned frames_to_read; // frames until the volume is read again (the user may change it)
};

bool analog_agc_start(struct analog_agc *agc, struct source_volume volume); // saves the volume; false on errors
// The level for the next frame (0 to WEBRTC_APM_ANALOG_LEVEL_MAXIMUM); -1 when the control is not active.
int analog_agc_level(struct analog_agc *agc);
void analog_agc_apply(struct analog_agc *agc, int recommended); // sets the source volume when the level changes
void analog_agc_stop(struct analog_agc *agc); // restores the saved volume once; safe to call again
