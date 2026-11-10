// All settings of hydra-native in one JSON file: $XDG_CONFIG_HOME/hydra-native/settings.json, else
// ~/.config/hydra-native/settings.json. Command line options override them for one run; the settings menu writes
// the file. QuickJS parses and writes the JSON, and keys that hydra-native does not know stay in the file.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SETTINGS_TEXT 512
#define SETTINGS_NAME 64
#define SETTINGS_DEFAULT_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
#define SETTINGS_DEFAULT_FONT_SIZE 20.0f
#define SETTINGS_DEFAULT_THEME "hydra"
#define SETTINGS_DEFAULT_WIDTH 1280
#define SETTINGS_DEFAULT_HEIGHT 720
#define MAX_CUSTOM_THEMES 16
#define MAX_THEME_COLORS 48
#define MAX_KEYBINDINGS 128

enum autocomplete_style { AUTOCOMPLETE_LIST, AUTOCOMPLETE_GHOST };
enum autocomplete_trigger { AUTOCOMPLETE_MANUAL, AUTOCOMPLETE_TYPING };
enum signature_help { SIGNATURE_HELP_MANUAL, SIGNATURE_HELP_PAREN }; // on its key, or also after typing "("
enum code_blend { CODE_BLEND_NORMAL, CODE_BLEND_DIFFERENCE };
#define SETTINGS_THEME_OPACITY (-1) // codeBackgroundOpacity: the alpha of the theme
// editor.renderWhitespace of VS Code
enum render_whitespace { WHITESPACE_NONE, WHITESPACE_BOUNDARY, WHITESPACE_SELECTION, WHITESPACE_TRAILING, WHITESPACE_ALL };

struct settings_pair {
    char key[SETTINGS_NAME];
    char value[SETTINGS_NAME * 2];
};

// A theme of the "themes" object: named colors ("#rrggbb" or "#rrggbbaa") over a built-in theme ("base").
struct custom_theme {
    char name[SETTINGS_NAME];
    char base[SETTINGS_NAME];
    struct settings_pair colors[MAX_THEME_COLORS];
    size_t color_count;
};

struct settings {
    char font[SETTINGS_TEXT];
    float font_size;
    char theme[SETTINGS_NAME];
    enum render_whitespace render_whitespace;
    enum autocomplete_style autocomplete_style;
    enum autocomplete_trigger autocomplete_trigger;
    enum signature_help signature_help;
    int code_background_opacity; // 0 to 100 percent, or SETTINGS_THEME_OPACITY
    enum code_blend code_background_blend, code_text_blend;
    bool word_wrap;
    bool code_visible;
    bool fullscreen;
    bool vsync;
    bool watch;
    bool analog_agc;
    int window_width, window_height;
    char audio_source[SETTINGS_TEXT];
    char language[SETTINGS_NAME];
    char gallery_server[SETTINGS_TEXT];
    char l10n_url[SETTINGS_TEXT];
    char socket[SETTINGS_TEXT];
    struct custom_theme themes[MAX_CUSTOM_THEMES];
    size_t theme_count;
    struct settings_pair keybindings[MAX_KEYBINDINGS]; // command name, key
    size_t keybinding_count;
};

void settings_defaults(struct settings *settings);
bool settings_path(char *path, size_t size); // false without XDG_CONFIG_HOME and HOME
// Reads the file into `settings` (a missing file leaves the defaults). On a JSON error it returns false, keeps the
// defaults and writes the message with its line into `error`.
bool settings_load(struct settings *settings, const char *path, char *error, size_t size);
// Writes the settings into the file, atomically, with the other keys of the file kept. It refuses to replace a
// file that does not parse.
bool settings_save(const struct settings *settings, const char *path, char *error, size_t size);
const char *settings_whitespace_name(enum render_whitespace value);
