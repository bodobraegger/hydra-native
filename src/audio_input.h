// The audio input of src/audio.c: 10 ms frames of mono float samples at AUDIO_SAMPLE_RATE.
// src/audio_input_pulse.c implements it with PulseAudio (Linux), src/audio_input_sdl.c with SDL3 (macOS, Windows).
// audio_input_init() runs on the main thread; the other functions run on the thread of src/audio.c.
#pragma once

#include <stdbool.h>

#include "audio_processing.h"

#define AUDIO_FRAME_SIZE (AUDIO_SAMPLE_RATE / 100) // the WebRTC audio processing takes 10 ms frames
// The name of the system output (loopback capture) on every system, as the monitor source of PulseAudio.
#define AUDIO_SYSTEM_OUTPUT "@DEFAULT_MONITOR@"
#define AUDIO_NONE "none" // the setting "audioSource" for no audio input

// An entry of the audio input list of the settings menu: the name of the setting "audioSource" and a label.
struct audio_device {
    char name[256];
    char label[256];
};

// The inputs of the system for the settings menu, also the system output (AUDIO_SYSTEM_OUTPUT) where the backend
// records it; returns their count. Runs on the main thread.
int audio_input_devices(struct audio_device *devices, int capacity);
// Prepares the audio system before the thread of src/audio.c starts; false when there is none.
bool audio_input_init(void);
// Opens `device`: an empty name for the default input. `program_directory` holds hydra-native-capture. Writes the
// reason of an error to stderr.
bool audio_input_open(const char *device, const char *program_directory);
// Waits for the next frame; false when the device fails, with a message on stderr.
bool audio_input_read(float frame[AUDIO_FRAME_SIZE]);
void audio_input_close(void);
