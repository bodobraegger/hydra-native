#pragma once

#include "quickjs.h"

// Adds the canvas* functions of the 2D canvas to the `native` object.
void canvas2d_install(JSContext *ctx, JSValue native);
