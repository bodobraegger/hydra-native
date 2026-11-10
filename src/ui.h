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
    // Shares the code with the gallery server under `name`, after a screenshot of the next frame; false
    // when no gallery server is set. The result arrives in ui_share_done().
    bool (*share)(const char *code, const char *name);
};

// `file_path` may be NULL (no file) or a path that does not exist yet (Ctrl+S creates it).
bool ui_init(const char *font_path, const char *emoji_font_path, float font_size, const char *file_path,
             const struct ui_host *host);
bool ui_key(const SDL_KeyboardEvent *event); // true when the overlay used the key
void ui_text_input(const char *text);
void ui_text_editing(const char *text); // an IME composition that is not committed yet
void ui_mouse_button(float x, float y, bool down, int clicks, SDL_Keymod modifiers);
void ui_mouse_motion(float x, float y);
void ui_wheel(int lines);
void ui_draw(int width, int height, double now_ms);
void ui_set_visible(bool visible);
void ui_set_code(const char *code); // shows code that has no file (a sketch URL)
void ui_show_info(void);
void ui_enable_gallery(void); // shows the "share" button, as the web editor does with a gallery server
void ui_share_done(bool ok, const char *output); // the URL of the shared sketch, or the error
void ui_show_random_example(void); // like the web editor at a start without a sketch
void ui_show_message(const char *text, bool error);
void ui_console_message(const char *text); // console.warn and console.error output
const char *ui_code(void); // the text of the overlay
void ui_file_changed(void); // the file changed on disk (--watch)
// Code from the socket: it does not enter the editor buffer; errors show in the error bar.
char *ui_evaluate_socket(const char *code, size_t length); // returns NULL or a malloc'd error
