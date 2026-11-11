// The audio input of Linux (src/audio_input.h): PulseAudio (or PipeWire) through a main loop that the audio thread
// runs itself, so that a read ends after READ_TIMEOUT_MS also when the source sends nothing (a suspended monitor).
// Source names are those of `pactl list sources`, also @DEFAULT_MONITOR@ for the system output.
#include <stdio.h>
#include <string.h>

#include <pulse/error.h>
#include <pulse/pulseaudio.h>

#include "audio_input.h"

#define LIST_TIMEOUT_ITERATIONS 200 // of the PulseAudio main loop, each of which waits for an event
#define READ_TIMEOUT_MS 200
#define MAX_WAITING_FRAMES 16 // older samples go when the thread reads too slowly

static pa_mainloop *input_loop;
static pa_context *input_context;
static pa_stream *input_stream;
static float waiting[MAX_WAITING_FRAMES * AUDIO_FRAME_SIZE]; // samples of the server that no frame took yet
static size_t waiting_count;

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

// Runs the main loop until `ready` says so; false when the connection or the stream failed, or after too many events.
static bool wait_until(bool (*ready)(void)) {
    for (int iterations = 0; iterations < LIST_TIMEOUT_ITERATIONS; iterations++) {
        if (ready()) return true;
        if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(input_context))) return false;
        if (input_stream && !PA_STREAM_IS_GOOD(pa_stream_get_state(input_stream))) return false;
        if (pa_mainloop_iterate(input_loop, 1, NULL) < 0) return false;
    }
    return false;
}

static bool context_ready(void) { return pa_context_get_state(input_context) == PA_CONTEXT_READY; }
static bool stream_ready(void) { return pa_stream_get_state(input_stream) == PA_STREAM_READY; }

bool audio_input_open(const char *device, const char *program_directory) {
    pa_sample_spec spec = { .format = PA_SAMPLE_FLOAT32LE, .rate = AUDIO_SAMPLE_RATE, .channels = 1 };
    pa_buffer_attr attributes = {
        .maxlength = (uint32_t)-1,
        .fragsize = AUDIO_FRAME_SIZE * sizeof(float),
    };
    waiting_count = 0;
    input_loop = pa_mainloop_new();
    input_context = pa_context_new(pa_mainloop_get_api(input_loop), "hydra-native");
    bool ok = input_context && pa_context_connect(input_context, NULL, PA_CONTEXT_NOFLAGS, NULL) >= 0 &&
              wait_until(context_ready);
    if (ok) {
        input_stream = pa_stream_new(input_context, "audio fft", &spec, NULL);
        const char *source = device[0] ? device : NULL;
        ok = input_stream &&
             pa_stream_connect_record(input_stream, source, &attributes, PA_STREAM_ADJUST_LATENCY) >= 0 &&
             wait_until(stream_ready);
    }
    if (!ok) {
        fprintf(stderr, "audio: cannot open source %s: %s\n", device[0] ? device : "(default)",
                input_context ? pa_strerror(pa_context_errno(input_context)) : "no connection");
        audio_input_close();
    }
    return ok;
}

// Moves what the server sent into `waiting`; a hole in the stream counts as silence.
static void take_samples(void) {
    const void *data;
    size_t size;
    while (pa_stream_readable_size(input_stream) > 0 && pa_stream_peek(input_stream, &data, &size) >= 0 && size > 0) {
        size_t count = size / sizeof(float), capacity = sizeof waiting / sizeof waiting[0];
        if (count > capacity) count = capacity;
        if (waiting_count + count > capacity) { // keep the newest samples
            size_t excess = waiting_count + count - capacity;
            memmove(waiting, waiting + excess, (waiting_count - excess) * sizeof(float));
            waiting_count -= excess;
        }
        if (data) memcpy(waiting + waiting_count, data, count * sizeof(float));
        else memset(waiting + waiting_count, 0, count * sizeof(float));
        waiting_count += count;
        pa_stream_drop(input_stream);
    }
}

bool audio_input_read(float frame[AUDIO_FRAME_SIZE]) {
    pa_usec_t deadline = pa_rtclock_now() + READ_TIMEOUT_MS * PA_USEC_PER_MSEC;
    for (;;) {
        take_samples();
        if (waiting_count >= AUDIO_FRAME_SIZE) {
            memcpy(frame, waiting, AUDIO_FRAME_SIZE * sizeof(float));
            waiting_count -= AUDIO_FRAME_SIZE;
            memmove(waiting, waiting + AUDIO_FRAME_SIZE, waiting_count * sizeof(float));
            return true;
        }
        if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(input_context)) ||
            !PA_STREAM_IS_GOOD(pa_stream_get_state(input_stream))) {
            fprintf(stderr, "audio: read failed: %s\n", pa_strerror(pa_context_errno(input_context)));
            return false;
        }
        pa_usec_t now = pa_rtclock_now();
        if (now >= deadline) { // a source that sends nothing is silent
            memset(frame, 0, AUDIO_FRAME_SIZE * sizeof(float));
            return true;
        }
        if (pa_mainloop_prepare(input_loop, (int)((deadline - now) / PA_USEC_PER_MSEC) + 1) < 0 ||
            pa_mainloop_poll(input_loop) < 0 || pa_mainloop_dispatch(input_loop) < 0)
            return false;
    }
}

void audio_input_close(void) {
    if (input_stream) {
        pa_stream_disconnect(input_stream);
        pa_stream_unref(input_stream);
    }
    if (input_context) {
        pa_context_disconnect(input_context);
        pa_context_unref(input_context);
    }
    if (input_loop) pa_mainloop_free(input_loop);
    input_stream = NULL;
    input_context = NULL;
    input_loop = NULL;
}
