// What src/main.c gives to the other files of the program.
#pragma once

#include <limits.h>

#include <SDL3/SDL.h>

extern SDL_Window *window;
extern char executable_path_directory[PATH_MAX]; // the folder of the program, the bundle and the capture tool
void app_quit(int code); // ends the main loop; `code` is the exit status
