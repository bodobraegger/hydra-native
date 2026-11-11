// The `native` object of the bundle (js/native.js): the functions of the program for JavaScript.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

// `audio_device` is the audio input of the settings, or NULL for the default; it must stay valid.
JSValue bindings_create(JSContext *ctx, const char *audio_device);
// Gives the result of a download of fetchStart or httpRequest to the callback of native.onFetchDone.
void bindings_fetch_done(JSContext *ctx, int id, bool ok, const char *data, size_t length);
