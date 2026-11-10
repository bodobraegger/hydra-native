// The audio input of macOS and Windows (src/audio_input.h) through miniaudio, which uses Core Audio and WASAPI.
// A device name selects the first input whose name contains it. AUDIO_SYSTEM_OUTPUT captures the default output
// through the WASAPI loopback; macOS has no such capture, so there the name of a loopback device (for example
// BlackHole) selects the system output. The data callback of miniaudio writes into a ring buffer, and
// audio_input_read() waits for it in the thread of src/audio.c.
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL_atomic.h>

#include "audio_input.h"
#include "miniaudio.h"

#define RING_FRAMES AUDIO_SAMPLE_RATE // one second; when the reader falls behind, the newest frames are lost

static ma_context context;
static ma_device device;
static ma_pcm_rb ring;
static ma_event ready;
static ma_device_id device_id;
static bool named, loopback;
static SDL_AtomicInt stopped;

static void on_data(ma_device *source, void *output, const void *input, ma_uint32 frames) {
    const float *samples = input;
    while (frames > 0) {
        ma_uint32 count = frames;
        void *buffer;
        if (ma_pcm_rb_acquire_write(&ring, &count, &buffer) != MA_SUCCESS || count == 0) break;
        memcpy(buffer, samples, count * sizeof(float));
        ma_pcm_rb_commit_write(&ring, count);
        samples += count;
        frames -= count;
    }
    ma_event_signal(&ready);
}

static void on_notification(const ma_device_notification *notification) {
    if (notification->type != ma_device_notification_type_stopped) return;
    SDL_SetAtomicInt(&stopped, 1);
    ma_event_signal(&ready);
}

static bool find_input(const char *name) {
    ma_device_info *inputs;
    ma_uint32 count;
    if (ma_context_get_devices(&context, NULL, NULL, &inputs, &count) != MA_SUCCESS) return false;
    for (ma_uint32 i = 0; i < count; i++) {
        if (!strstr(inputs[i].name, name)) continue;
        device_id = inputs[i].id;
        return true;
    }
    fprintf(stderr, "audio: no input device with \"%s\" in its name; the inputs are:\n", name);
    for (ma_uint32 i = 0; i < count; i++) fprintf(stderr, "  %s\n", inputs[i].name);
    return false;
}

bool audio_input_open(const char *name) {
    if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS) {
        fprintf(stderr, "audio: no audio system\n");
        return false;
    }
    loopback = strcmp(name, AUDIO_SYSTEM_OUTPUT) == 0;
    named = name[0] && !loopback;
    ma_device_config config = ma_device_config_init(loopback ? ma_device_type_loopback : ma_device_type_capture);
    config.capture.pDeviceID = named ? &device_id : NULL;
    config.capture.format = ma_format_f32;
    config.capture.channels = 1;
    config.sampleRate = AUDIO_SAMPLE_RATE;
    config.periodSizeInFrames = AUDIO_FRAME_SIZE;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.dataCallback = on_data;
    config.notificationCallback = on_notification;
    bool ok = (!named || find_input(name)) &&
              ma_pcm_rb_init(ma_format_f32, 1, RING_FRAMES, NULL, NULL, &ring) == MA_SUCCESS;
    if (ok && ma_event_init(&ready) != MA_SUCCESS) {
        ma_pcm_rb_uninit(&ring);
        ok = false;
    }
    ma_result result = MA_ERROR;
    if (ok && (result = ma_device_init(&context, &config, &device)) == MA_SUCCESS &&
        (result = ma_device_start(&device)) != MA_SUCCESS)
        ma_device_uninit(&device);
    if (ok && result != MA_SUCCESS) {
        if (loopback && result == MA_DEVICE_TYPE_NOT_SUPPORTED)
            fprintf(stderr, "audio: this system cannot capture its output; give the name of a loopback device to "
                            "--audio, for example \"BlackHole\"\n");
        else fprintf(stderr, "audio: cannot open the input %s: %s\n", name[0] ? name : "(default)", ma_result_description(result));
        ma_event_uninit(&ready);
        ma_pcm_rb_uninit(&ring);
        ok = false;
    }
    if (!ok) ma_context_uninit(&context);
    return ok;
}

bool audio_input_read(float frame[AUDIO_FRAME_SIZE]) {
    ma_uint32 done = 0;
    while (done < AUDIO_FRAME_SIZE) {
        ma_uint32 count = AUDIO_FRAME_SIZE - done;
        void *buffer;
        if (ma_pcm_rb_acquire_read(&ring, &count, &buffer) == MA_SUCCESS && count > 0) {
            memcpy(frame + done, buffer, count * sizeof(float));
            ma_pcm_rb_commit_read(&ring, count);
            done += count;
        } else if (SDL_GetAtomicInt(&stopped)) {
            fprintf(stderr, "audio: the input device stopped\n");
            return false;
        } else {
            ma_event_wait(&ready);
        }
    }
    return true;
}

void audio_input_close(void) {
    ma_device_uninit(&device);
    ma_event_uninit(&ready);
    ma_pcm_rb_uninit(&ring);
    ma_context_uninit(&context);
}
