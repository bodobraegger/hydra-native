// The `native` object of the bundle (js/native.js): the functions of the program for JavaScript.
#pragma once

#include "quickjs.h"

// `audio_device` is the audio input of the settings, or NULL for the default; it must stay valid.
JSValue bindings_create(JSContext *ctx, const char *audio_device);
