// The input volume of a capture device of miniaudio for the analog gain control (src/analog_agc.h): Core Audio on
// macOS (src/audio_volume_coreaudio.c), IAudioEndpointVolume on Windows (src/audio_volume_wasapi.c). A NULL id is
// the default input of the system. False when the device has no volume that the program can set.
#pragma once

#include <stdbool.h>

#include "analog_agc.h"
#include "miniaudio.h"

#if defined(__APPLE__) || defined(_WIN32)
bool audio_volume_open(const ma_device_id *id, struct source_volume *volume);
#else
// Linux uses miniaudio only in the test build HYDRA_MINIAUDIO_LIBREMIDI, without the analog gain control.
static inline bool audio_volume_open(const ma_device_id *id, struct source_volume *volume) { return false; }
#endif
