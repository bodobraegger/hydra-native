// The audio processing of Chrome's getUserMedia() from the system library libwebrtc-audio-processing-1.so.3
// (version 1.3, BSD-3-Clause, see THIRD_PARTY.md). This file builds into build/libhydra-webrtc-apm.so, which
// src/audio.c opens with dlopen(), so that hydra-native also starts without the system library.
#include "modules/audio_processing/include/audio_processing.h"

#include "webrtc_apm.h"

extern "C" void *webrtc_apm_create(int sample_rate) {
    webrtc::AudioProcessing *apm = webrtc::AudioProcessingBuilder().Create();
    if (!apm) return nullptr;
    // The defaults of getUserMedia() in Chrome: echoCancellation, noiseSuppression (level high) and
    // autoGainControl (the adaptive digital controller of AGC2), and the high-pass filter.
    webrtc::AudioProcessing::Config config;
    config.high_pass_filter.enabled = true;
    config.echo_canceller.enabled = true;
    config.noise_suppression.enabled = true;
    config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
    config.gain_controller2.enabled = true;
    config.gain_controller2.adaptive_digital.enabled = true;
    apm->ApplyConfig(config);
    return apm;
}

extern "C" bool webrtc_apm_process(void *handle, float *samples, int sample_rate) {
    auto *apm = static_cast<webrtc::AudioProcessing *>(handle);
    webrtc::StreamConfig stream(sample_rate, 1);
    apm->set_stream_delay_ms(0); // no sound is played, so the echo path has no delay to report
    float *channels[1] = { samples };
    bool ok = apm->ProcessStream(channels, stream, stream, channels) == webrtc::AudioProcessing::kNoError;
    return ok;
}

extern "C" void webrtc_apm_destroy(void *handle) { delete static_cast<webrtc::AudioProcessing *>(handle); }
