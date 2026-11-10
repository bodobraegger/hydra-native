// The code overlay: an editor over the visuals, like the one of hydra.ojack.xyz.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <SDL.h>

struct ui_host {
    // Evaluates code in the global scope; returns NULL or a malloc'd error message.
    char *(*evaluate)(const char *code, size_t length, const char *name);
    void (*toggle_fullscreen)(void);
    bool (*is_fullscreen)(void);
    void (*quit)(void);
    // Calls a global JS function with one string; returns its string result (malloc'd) or NULL after an error.
    char *(*call)(const char *function, const char *argument);
    void (*screencap)(void); // saves the next frame (without the code) and the code
};

// `file_path` may be NULL (no file) or a path that does not exist yet (Ctrl+S creates it).
bool ui_init(const char *font_path, float font_size, const char *file_path, const struct ui_host *host);
bool ui_key(const SDL_KeyboardEvent *event); // true when the overlay used the key
void ui_text_input(const char *text);
void ui_mouse_button(float x, float y, bool down, int clicks, bool shift);
void ui_mouse_motion(float x, float y);
void ui_wheel(int lines);
void ui_draw(int width, int height, double now_ms);
void ui_set_visible(bool visible);
void ui_show_message(const char *text, bool error);
void ui_console_message(const char *text); // console.warn and console.error output
const char *ui_code(void); // the text of the overlay
void ui_file_changed(void); // the file changed on disk (--watch)
// Code from the socket: it does not enter the editor buffer; errors show in the error bar.
char *ui_evaluate_socket(const char *code, size_t length); // returns NULL or a malloc'd error
