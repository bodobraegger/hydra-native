// The audio input of Linux (src/audio_input.h): the simple API of PulseAudio (or PipeWire). Source names are those
// of `pactl list sources`, also @DEFAULT_MONITOR@ for the system output.
#include <stdio.h>

#include <pulse/error.h>
#include <pulse/simple.h>

#include "audio_input.h"

static pa_simple *stream;
bool audio_input_init(void) { return true; }

bool audio_input_open(const char *device, const char *program_directory) {
    pa_sample_spec spec = { .format = PA_SAMPLE_FLOAT32LE, .rate = AUDIO_SAMPLE_RATE, .channels = 1 };
    pa_buffer_attr attributes = {
        .maxlength = (uint32_t)-1,
        .fragsize = AUDIO_FRAME_SIZE * sizeof(float),
    };
    int error = 0;
    stream = pa_simple_new(NULL, "hydra-native", PA_STREAM_RECORD, device[0] ? device : NULL, "audio fft", &spec, NULL,
                           &attributes, &error);
    if (!stream) fprintf(stderr, "audio: cannot open source %s: %s\n", device[0] ? device : "(default)", pa_strerror(error));
    return stream != NULL;
}

bool audio_input_read(float frame[AUDIO_FRAME_SIZE]) {
    int error = 0;
    if (pa_simple_read(stream, frame, AUDIO_FRAME_SIZE * sizeof(float), &error) >= 0) return true;
    fprintf(stderr, "audio: read failed: %s\n", pa_strerror(error));
    return false;
}

void audio_input_close(void) {
    pa_simple_free(stream);
    stream = NULL;
}
