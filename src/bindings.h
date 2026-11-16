// The `native` object of the bundle (js/native.js): the functions of the program for JavaScript.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

JSValue bindings_create(JSContext *ctx);
// Gives the result of a download of fetchStart or httpRequest to the callback of native.onFetchDone.
void bindings_fetch_done(JSContext *ctx, int id, bool ok, const char *data, size_t length);
