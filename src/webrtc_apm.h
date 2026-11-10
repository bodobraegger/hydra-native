// The C interface of src/webrtc_apm.cc. Each process call takes one 10 ms frame of mono float samples in place.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
void *webrtc_apm_create(int sample_rate);
bool webrtc_apm_process(void *apm, float *samples, int sample_rate);
void webrtc_apm_destroy(void *apm);
#ifdef __cplusplus
}
#endif
