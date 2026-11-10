#pragma once

#include "quickjs.h"

// Adds the canvas* functions of the 2D canvas to the `native` object.
void canvas2d_install(JSContext *ctx, JSValue native);

#include <stdbool.h>
#include <stddef.h>
// true when the download was an image of the canvas (new Image(), createElement('img')).
bool canvas2d_fetch_done(int fetch, bool ok, const char *data, size_t length);
// The pixels of a canvas or image as RGBA rows from the top (malloc'd), straight or premultiplied alpha.
unsigned char *canvas2d_rgba(int id, bool premultiplied, int *width, int *height);
