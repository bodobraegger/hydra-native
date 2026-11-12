// The code overlay: an editor over the visuals, like the one of hydra.ojack.xyz.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <SDL3/SDL.h>

#include "audio_input.h"
#include "settings.h"

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
    // Shows the URL of the sketch, which the browser shows in the address bar, in the window title.
    void (*set_title)(const char *url);
    void (*set_vsync)(bool vsync);
    // The audio input list of the settings menu (src/audio_input.h), and the input of `a` ("" default, "none").
    int (*audio_devices)(struct audio_device *devices, int capacity);
    void (*set_audio)(const char *device);
    void (*set_audio_gain)(int percent);
    // Shows the open dialog (`save` false) or the save dialog for a .js file; the chosen path comes later through
    // ui_open_file() or ui_save_as(), on the main thread.
    void (*choose_file)(bool save);
    // The gallery and patch bay server ("" is off). Only this call sets it, so that code of a sketch cannot.
    void (*set_server)(const char *url);
};

// The settings of this run, with the changes of the settings menu (main.c reads "watch" from it).
const struct settings *ui_settings(void);

// `file_path` may be NULL (no file) or a path that does not exist yet (Ctrl+S creates it).
// `settings` is the content of the settings file at `settings_file` (NULL: none); the settings menu saves there.
bool ui_init(const struct settings *settings, const char *settings_file,
             const char *file_path, const struct ui_host *host);
bool ui_key(const SDL_KeyboardEvent *event); // true when the overlay used the key
void ui_text_input(const char *text);
void ui_text_editing(const char *text); // an IME composition that is not committed yet
// The cursor in drawable pixels, where the input method shows its candidate window; false when it is hidden.
bool ui_text_input_rect(int *x, int *y, int *width, int *height);
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
bool ui_modified(void); // the code has changes that no file holds, such as the restored ones of a recovery file
void ui_file_changed(void); // the file changed on disk, with "reload on file change"
// The open dialog of the "file" row answers here. This call, the commands that replace the code and ui_quit()
// first ask whether to save or discard the unsaved changes of the open file.
void ui_open_file(const char *path);
void ui_quit(void); // Ctrl+Q and the close button of the window
void ui_save_as(const char *path);
const char *ui_file_path(void); // the open file, or NULL
// Code from the socket: it does not enter the editor buffer; errors show in the error bar.
char *ui_evaluate_socket(const char *code, size_t length); // returns NULL or a malloc'd error
