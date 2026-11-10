#pragma once

#include <stdbool.h>

#include <SDL3/SDL.h>

#define APP_ID "hydra-native" // the Wayland app id and the X11 class, the name of assets/hydra-native.desktop

// Starts SDL video and opens a window with an OpenGL ES 3.0 context: EGL with the system driver (Mesa) on Linux,
// EGL on ANGLE (Metal on macOS, Direct3D 11 on Windows) from the program folder elsewhere. NULL on an error,
// which goes to stderr.
SDL_Window *window_create(const char *title, int width, int height, SDL_WindowFlags flags, bool vsync);
