// The C interface of src/webrtc_apm.cc. Each process call takes one 10 ms frame of mono float samples in place.
// With analog gain control, `analog_level` is the volume of the source (0 to WEBRTC_APM_ANALOG_LEVEL_MAXIMUM)
// before the call and the recommended volume after it; NULL without analog gain control.
#pragma once

#include <stdbool.h>

#define WEBRTC_APM_ANALOG_LEVEL_MAXIMUM 255

#ifdef __cplusplus
extern "C" {
#endif
void *webrtc_apm_create(int sample_rate, bool analog_gain);
bool webrtc_apm_process(void *apm, float *samples, int sample_rate, int *analog_level);
void webrtc_apm_destroy(void *apm);
#ifdef __cplusplus
}
#endif
