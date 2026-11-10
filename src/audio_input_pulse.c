// The audio input of Linux (src/audio_input.h): the simple API of PulseAudio (or PipeWire) for the samples, and a
// connection of the asynchronous API for the source volume. Source names are those of `pactl list sources`, also
// @DEFAULT_MONITOR@ for the system output.
#include <math.h>
#include <stdio.h>

#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

#include "audio_input.h"

static pa_simple *stream;
static char source_name[256];

bool audio_input_open(const char *device) {
    pa_sample_spec spec = { .format = PA_SAMPLE_FLOAT32LE, .rate = AUDIO_SAMPLE_RATE, .channels = 1 };
    pa_buffer_attr attributes = {
        .maxlength = (uint32_t)-1,
        .fragsize = AUDIO_FRAME_SIZE * sizeof(float),
    };
    int error = 0;
    snprintf(source_name, sizeof source_name, "%s", device);
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

// ---------------------------------------------------------------- the source volume

// A PulseAudio connection with its own thread; the functions below wait for each operation.
static struct {
    pa_threaded_mainloop *loop;
    pa_context *context;
    char source[256];
    pa_cvolume original;
    pa_cvolume current;
    bool found, monitor;
} pulse;

static void pulse_signal(pa_context *context, void *data) { pa_threaded_mainloop_signal(pulse.loop, 0); }

static void pulse_wait(pa_operation *operation) {
    if (!operation) return;
    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) pa_threaded_mainloop_wait(pulse.loop);
    pa_operation_unref(operation);
}

static void server_info(pa_context *context, const pa_server_info *info, void *data) {
    if (info && info->default_source_name) snprintf(pulse.source, sizeof pulse.source, "%s", info->default_source_name);
    pa_threaded_mainloop_signal(pulse.loop, 0);
}

static void source_info(pa_context *context, const pa_source_info *info, int last, void *data) {
    if (info) {
        pulse.current = info->volume;
        pulse.monitor = info->monitor_of_sink != PA_INVALID_INDEX;
        pulse.found = true;
    }
    pa_threaded_mainloop_signal(pulse.loop, 0);
}

static void operation_done(pa_context *context, int success, void *data) { pa_threaded_mainloop_signal(pulse.loop, 0); }

static bool pulse_read(void) {
    pulse.found = false;
    pulse_wait(pa_context_get_source_info_by_name(pulse.context, pulse.source, source_info, NULL));
    return pulse.found;
}

static bool pulse_write(const pa_cvolume *volume) {
    pa_operation *operation = pa_context_set_source_volume_by_name(pulse.context, pulse.source, volume, operation_done, NULL);
    pulse_wait(operation);
    return operation != NULL;
}

static bool volume_save(void *data) {
    pa_threaded_mainloop_lock(pulse.loop);
    bool ok = pulse_read();
    pulse.original = pulse.current;
    pa_threaded_mainloop_unlock(pulse.loop);
    return ok;
}

static bool volume_restore(void *data) {
    pa_threaded_mainloop_lock(pulse.loop);
    bool ok = pulse_write(&pulse.original);
    pa_threaded_mainloop_unlock(pulse.loop);
    return ok;
}

static bool volume_get(void *data, double *volume) {
    pa_threaded_mainloop_lock(pulse.loop);
    bool ok = pulse_read();
    if (ok) *volume = (double)pa_cvolume_avg(&pulse.current) / PA_VOLUME_NORM;
    pa_threaded_mainloop_unlock(pulse.loop);
    return ok;
}

// Like the PulseAudio input of Chrome: all channels get the same volume.
static bool volume_set(void *data, double volume) {
    pa_cvolume channels;
    pa_threaded_mainloop_lock(pulse.loop);
    pa_cvolume_set(&channels, pulse.original.channels, (pa_volume_t)lround(volume * PA_VOLUME_NORM));
    bool ok = pulse_write(&channels);
    pa_threaded_mainloop_unlock(pulse.loop);
    return ok;
}

// Connects to the server and finds the capture source (the default one without a name); false for a monitor
// source, whose volume is the loopback of a sink.
bool audio_input_volume(struct source_volume *volume) {
    snprintf(pulse.source, sizeof pulse.source, "%s", source_name);
    pulse.loop = pa_threaded_mainloop_new();
    if (!pulse.loop) return false;
    pulse.context = pa_context_new(pa_threaded_mainloop_get_api(pulse.loop), "hydra-native volume");
    pa_context_set_state_callback(pulse.context, pulse_signal, NULL);
    pa_threaded_mainloop_lock(pulse.loop);
    bool ready = pa_context_connect(pulse.context, NULL, PA_CONTEXT_NOFLAGS, NULL) >= 0 && pa_threaded_mainloop_start(pulse.loop) >= 0;
    while (ready) {
        pa_context_state_t state = pa_context_get_state(pulse.context);
        if (state == PA_CONTEXT_READY) break;
        if (!PA_CONTEXT_IS_GOOD(state)) ready = false;
        else pa_threaded_mainloop_wait(pulse.loop);
    }
    if (ready && !pulse.source[0]) pulse_wait(pa_context_get_server_info(pulse.context, server_info, NULL));
    ready = ready && pulse.source[0] && pulse_read() && !pulse.monitor;
    pa_threaded_mainloop_unlock(pulse.loop);
    *volume = (struct source_volume){ volume_save, volume_restore, volume_get, volume_set, NULL };
    return ready;
}
