// The input volume of a WASAPI capture endpoint (src/audio_volume.h) through IAudioEndpointVolume, as the WASAPI
// input of Chrome sets it (media/audio/win/audio_low_latency_input_win.cc): the master volume level scalar.
// The audio thread opens the endpoint and uses it for every call except the restore, which can come from the main
// thread at exit; the restore opens the endpoint again in its own thread, so that no COM pointer crosses threads.
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include "audio_volume.h"

// The GUIDs of mmdeviceapi.h and endpointvolume.h have their definitions in no library that C code links.
static const CLSID MMDEVICE_ENUMERATOR = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID MMDEVICE_ENUMERATOR_INTERFACE = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID ENDPOINT_VOLUME_INTERFACE = { 0x5CDF2C82, 0x841E, 0x4546, { 0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A } };

static struct {
    WCHAR id[64]; // the endpoint ID string, also of the default input, so that the restore finds the same device
    IAudioEndpointVolume *endpoint;
    float saved;
} input;

// The volume interface of the endpoint `id`, or of the default input without one; the device ID goes to `found_id`.
static IAudioEndpointVolume *open_endpoint(const WCHAR *id, WCHAR *found_id) {
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *device = NULL;
    IAudioEndpointVolume *endpoint = NULL;
    if (FAILED(CoCreateInstance(&MMDEVICE_ENUMERATOR, NULL, CLSCTX_ALL, &MMDEVICE_ENUMERATOR_INTERFACE, (void **)&enumerator)))
        return NULL;
    HRESULT result = id ? IMMDeviceEnumerator_GetDevice(enumerator, id, &device)
                        : IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eCapture, eConsole, &device);
    if (SUCCEEDED(result)) {
        WCHAR *device_id = NULL;
        if (found_id && SUCCEEDED(IMMDevice_GetId(device, &device_id))) {
            lstrcpynW(found_id, device_id, (int)(sizeof input.id / sizeof input.id[0]));
            CoTaskMemFree(device_id);
        }
        if (FAILED(IMMDevice_Activate(device, &ENDPOINT_VOLUME_INTERFACE, CLSCTX_ALL, NULL, (void **)&endpoint)))
            endpoint = NULL;
        IMMDevice_Release(device);
    }
    IMMDeviceEnumerator_Release(enumerator);
    return endpoint;
}

static bool volume_save(void *data) {
    return SUCCEEDED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(input.endpoint, &input.saved));
}

static bool volume_restore(void *data) {
    HRESULT com = CoInitializeEx(NULL, COINIT_MULTITHREADED); // RPC_E_CHANGED_MODE: COM runs in another mode
    IAudioEndpointVolume *endpoint = open_endpoint(input.id, NULL);
    bool ok = endpoint && SUCCEEDED(IAudioEndpointVolume_SetMasterVolumeLevelScalar(endpoint, input.saved, NULL));
    if (endpoint) IAudioEndpointVolume_Release(endpoint);
    if (SUCCEEDED(com)) CoUninitialize();
    return ok;
}

static bool volume_get(void *data, double *volume) {
    float level;
    if (FAILED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(input.endpoint, &level))) return false;
    *volume = level;
    return true;
}

static bool volume_set(void *data, double volume) {
    return SUCCEEDED(IAudioEndpointVolume_SetMasterVolumeLevelScalar(input.endpoint, (float)volume, NULL));
}

bool audio_volume_open(const ma_device_id *id, struct source_volume *volume) {
    // The audio thread keeps COM for its whole life.
    HRESULT com = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) return false;
    input.endpoint = open_endpoint(id ? id->wasapi : NULL, input.id);
    *volume = (struct source_volume){ volume_save, volume_restore, volume_get, volume_set, NULL };
    return input.endpoint != NULL;
}
