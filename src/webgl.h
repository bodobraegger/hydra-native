#pragma once

#include "quickjs.h"

// Adds the gl* functions of WebGL contexts (js/webgl.js) to the `native` object.
void webgl_install(JSContext *ctx, JSValue native);
// Gives the GL state back to the hydra passes and the overlay; call before they draw.
void webgl_release(void);
