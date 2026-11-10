#include "window.h"

#include <stdio.h>

#if !defined(__linux__)
#if defined(__APPLE__)
#define EGL_LIBRARY "libEGL.dylib"
#define GLES_LIBRARY "libGLESv2.dylib"
#else
#define EGL_LIBRARY "libEGL.dll"
#define GLES_LIBRARY "libGLESv2.dll"
#endif

// SDL loads ANGLE from the program folder, the same files that the program links (rpath or DLL folder), so that
// SDL and the GL calls of the program use one ANGLE.
static void use_angle(void) {
    const char *base = SDL_GetBasePath();
    char path[1024];
    SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    snprintf(path, sizeof path, "%s%s", base ? base : "", EGL_LIBRARY);
    SDL_SetHint(SDL_HINT_EGL_LIBRARY, path);
    snprintf(path, sizeof path, "%s%s", base ? base : "", GLES_LIBRARY);
    SDL_SetHint(SDL_HINT_OPENGL_LIBRARY, path);
}
#endif

SDL_Window *window_create(const char *title, int width, int height, SDL_WindowFlags flags, bool vsync) {
#if defined(__linux__)
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
#else
    use_angle();
#endif
    SDL_SetAppMetadata("hydra-native", NULL, APP_ID);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return NULL;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_Window *window = SDL_CreateWindow(title, width, height, flags | SDL_WINDOW_OPENGL);
    if (!window || !SDL_GL_CreateContext(window)) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        return NULL;
    }
    SDL_GL_SetSwapInterval(vsync ? 1 : 0);
    return window;
}
