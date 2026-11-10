// The settings of the settings menu, in a key=value file: $XDG_CONFIG_HOME/hydra-native/settings, else
// ~/.config/hydra-native/settings. Command line options override them for one run.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SETTINGS_TEXT 512
#define SETTINGS_DEFAULT_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
#define SETTINGS_DEFAULT_FONT_SIZE 20.0f
#define SETTINGS_DEFAULT_THEME "hydra"

enum autocomplete_style { AUTOCOMPLETE_LIST, AUTOCOMPLETE_GHOST };
enum autocomplete_trigger { AUTOCOMPLETE_MANUAL, AUTOCOMPLETE_TYPING };

struct settings {
    char font[SETTINGS_TEXT]; // a font file
    float font_size;
    char theme[SETTINGS_TEXT];
    enum autocomplete_style autocomplete_style;
    enum autocomplete_trigger autocomplete_trigger;
};

void settings_defaults(struct settings *settings);
bool settings_path(char *path, size_t size); // false without XDG_CONFIG_HOME and HOME
// Reads the known keys of the file into `settings`; other keys and a missing file leave it as it was.
bool settings_load(struct settings *settings, const char *path);
bool settings_save(const struct settings *settings, const char *path); // creates the directory
