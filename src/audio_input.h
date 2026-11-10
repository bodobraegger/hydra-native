// The audio input of src/audio.c: 10 ms frames of mono float samples at AUDIO_SAMPLE_RATE, and the volume of the
// capture device for the analog gain control. src/audio_input_pulse.c implements it with PulseAudio (Linux),
// src/audio_input_miniaudio.c with miniaudio (macOS, Windows). Only the thread of src/audio.c calls it.
#pragma once

#include <stdbool.h>

#include "analog_agc.h"
#include "audio_processing.h"

#define AUDIO_FRAME_SIZE (AUDIO_SAMPLE_RATE / 100) // the WebRTC audio processing takes 10 ms frames
// The name of the system output (loopback capture) on every system, as the monitor source of PulseAudio.
#define AUDIO_SYSTEM_OUTPUT "@DEFAULT_MONITOR@"

// Opens `device`: an empty name for the default input. Writes the reason of an error to stderr.
bool audio_input_open(const char *device);
// Waits for the next frame; false when the device fails, with a message on stderr.
bool audio_input_read(float frame[AUDIO_FRAME_SIZE]);
void audio_input_close(void);
// The volume of the open device for the analog gain control; false when the device has none that the program may
// change, for example the system output.
bool audio_input_volume(struct source_volume *volume);
