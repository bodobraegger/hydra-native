#include "window.h"

#include <stdio.h>
#include <string.h>

#if !defined(__linux__)
#include <GLES3/gl3.h>
#if defined(__APPLE__)
#include <dlfcn.h>
#define EGL_LIBRARY "libEGL.dylib"
#define GLES_LIBRARY "libGLESv2.dylib"
#else
#include <windows.h>
#define EGL_LIBRARY "libEGL.dll"
#define GLES_LIBRARY "libGLESv2.dll"
#endif

// The folder of the libGLESv2 that the program links (through its rpath or the DLL search), with the separator.
static void angle_folder(char *folder, size_t size) {
    folder[0] = '\0';
#if defined(__APPLE__)
    Dl_info info;
    if (dladdr((const void *)glGetString, &info) && info.dli_fname) snprintf(folder, size, "%s", info.dli_fname);
    char *separator = strrchr(folder, '/');
#else
    HMODULE module = GetModuleHandleA(GLES_LIBRARY);
    if (module) GetModuleFileNameA(module, folder, (DWORD)size);
    char *separator = strrchr(folder, '\\');
#endif
    if (separator) separator[1] = '\0';
    else folder[0] = '\0';
}

// SDL loads the ANGLE that the program links, from its folder, so that SDL and the GL calls of the program use
// one ANGLE.
static void use_angle(void) {
    char base[1024];
    angle_folder(base, sizeof base);
    char path[1100];
    SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    snprintf(path, sizeof path, "%s%s", base, EGL_LIBRARY);
    SDL_SetHint(SDL_HINT_EGL_LIBRARY, path);
    snprintf(path, sizeof path, "%s%s", base, GLES_LIBRARY);
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
