// The input volume of a Core Audio device (src/audio_volume.h), set as Chrome sets it in
// media/audio/mac/audio_low_latency_input_mac.cc: kAudioDevicePropertyVolumeScalar of the main element when it is
// settable, else of each channel that has one. The volume is the mean of these elements.
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include "audio_volume.h"

#define MAIN_ELEMENT 0 // kAudioObjectPropertyElementMain, which has this name from macOS 12 on
#define MAX_CHANNELS 32

static struct {
    AudioObjectID device;
    UInt32 elements[MAX_CHANNELS]; // the elements with a settable volume
    int count;
    Float32 saved[MAX_CHANNELS];
} input;

static AudioObjectPropertyAddress volume_address(UInt32 element) {
    return (AudioObjectPropertyAddress){ kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeInput, element };
}

static bool is_settable(UInt32 element) {
    AudioObjectPropertyAddress address = volume_address(element);
    Boolean settable = false;
    return AudioObjectHasProperty(input.device, &address) &&
           AudioObjectIsPropertySettable(input.device, &address, &settable) == noErr && settable;
}

static bool read_element(UInt32 element, Float32 *value) {
    AudioObjectPropertyAddress address = volume_address(element);
    UInt32 size = sizeof *value;
    return AudioObjectGetPropertyData(input.device, &address, 0, NULL, &size, value) == noErr;
}

static bool write_element(UInt32 element, Float32 value) {
    AudioObjectPropertyAddress address = volume_address(element);
    return AudioObjectSetPropertyData(input.device, &address, 0, NULL, sizeof value, &value) == noErr;
}

static bool volume_save(void *data) {
    bool ok = true;
    for (int i = 0; i < input.count; i++) ok = read_element(input.elements[i], &input.saved[i]) && ok;
    return ok;
}

static bool volume_restore(void *data) {
    bool ok = true;
    for (int i = 0; i < input.count; i++) ok = write_element(input.elements[i], input.saved[i]) && ok;
    return ok;
}

static bool volume_get(void *data, double *volume) {
    double sum = 0;
    for (int i = 0; i < input.count; i++) {
        Float32 value;
        if (!read_element(input.elements[i], &value)) return false;
        sum += value;
    }
    *volume = sum / input.count;
    return true;
}

static bool volume_set(void *data, double volume) {
    bool ok = true;
    for (int i = 0; i < input.count; i++) ok = write_element(input.elements[i], (Float32)volume) && ok;
    return ok;
}

// The device of a UID of miniaudio (kAudioDevicePropertyDeviceUID), or the default input without one.
static AudioObjectID find_device(const ma_device_id *id) {
    AudioObjectID device = kAudioObjectUnknown;
    AudioObjectPropertyAddress address = { kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal,
                                           MAIN_ELEMENT };
    UInt32 size = sizeof device;
    if (!id) {
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &device) != noErr)
            return kAudioObjectUnknown;
        return device;
    }
    CFStringRef uid = CFStringCreateWithCString(NULL, id->coreaudio, kCFStringEncodingUTF8);
    if (!uid) return kAudioObjectUnknown;
    AudioValueTranslation translation = { &uid, sizeof uid, &device, sizeof device };
    address.mSelector = kAudioHardwarePropertyDeviceForUID;
    size = sizeof translation;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &translation) != noErr)
        device = kAudioObjectUnknown;
    CFRelease(uid);
    return device;
}

bool audio_volume_open(const ma_device_id *id, struct source_volume *volume) {
    input.device = find_device(id);
    input.count = 0;
    if (input.device == kAudioObjectUnknown) return false;
    if (is_settable(MAIN_ELEMENT)) input.elements[input.count++] = MAIN_ELEMENT;
    else
        for (UInt32 channel = 1; channel <= MAX_CHANNELS && input.count < MAX_CHANNELS; channel++)
            if (is_settable(channel)) input.elements[input.count++] = channel;
    *volume = (struct source_volume){ volume_save, volume_restore, volume_get, volume_set, NULL };
    return input.count > 0;
}
