// The audio input of Linux (src/audio_input.h): the simple API of PulseAudio (or PipeWire). Source names are those
// of `pactl list sources`, also @DEFAULT_MONITOR@ for the system output.
#include <stdio.h>

#include <string.h>

#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

#include "audio_input.h"

#define LIST_TIMEOUT_ITERATIONS 200 // of the PulseAudio main loop, each of which waits for an event

static pa_simple *stream;

struct device_list {
    struct audio_device *devices;
    int capacity, count;
    bool done;
};

// The sources of the server; the monitors of the sinks are left out, as the system output covers them.
static void add_source(pa_context *context, const pa_source_info *info, int end, void *data) {
    struct device_list *list = data;
    if (end) {
        list->done = true;
        return;
    }
    if (info->monitor_of_sink != PA_INVALID_INDEX || list->count == list->capacity) return;
    struct audio_device *device = &list->devices[list->count++];
    snprintf(device->name, sizeof device->name, "%s", info->name);
    snprintf(device->label, sizeof device->label, "%s", info->description ? info->description : info->name);
}

int audio_input_devices(struct audio_device *devices, int capacity) {
    struct device_list list = { devices, capacity, 0, false };
    pa_mainloop *loop = pa_mainloop_new();
    pa_context *context = pa_context_new(pa_mainloop_get_api(loop), "hydra-native");
    if (context && pa_context_connect(context, NULL, PA_CONTEXT_NOFLAGS, NULL) >= 0) {
        int iterations = 0;
        pa_context_state_t state;
        while ((state = pa_context_get_state(context)) != PA_CONTEXT_READY && PA_CONTEXT_IS_GOOD(state) &&
               iterations++ < LIST_TIMEOUT_ITERATIONS)
            pa_mainloop_iterate(loop, 1, NULL);
        pa_operation *operation =
            state == PA_CONTEXT_READY ? pa_context_get_source_info_list(context, add_source, &list) : NULL;
        while (operation && !list.done && iterations++ < LIST_TIMEOUT_ITERATIONS) pa_mainloop_iterate(loop, 1, NULL);
        if (operation) pa_operation_unref(operation);
        pa_context_disconnect(context);
    }
    if (context) pa_context_unref(context);
    pa_mainloop_free(loop);
    if (list.count < capacity) {
        snprintf(devices[list.count].name, sizeof devices[0].name, "%s", AUDIO_SYSTEM_OUTPUT);
        snprintf(devices[list.count++].label, sizeof devices[0].label, "system output");
    }
    return list.count;
}
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
