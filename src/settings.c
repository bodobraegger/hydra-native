#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SETTINGS_DIRECTORY "hydra-native"
#define SETTINGS_FILE "settings"

static const char *const style_names[] = { "list", "ghost" };
static const char *const trigger_names[] = { "manual", "typing" };

void settings_defaults(struct settings *settings) {
    *settings = (struct settings){ SETTINGS_DEFAULT_FONT, SETTINGS_DEFAULT_FONT_SIZE, SETTINGS_DEFAULT_THEME, AUTOCOMPLETE_LIST, AUTOCOMPLETE_MANUAL };
}

bool settings_path(char *path, size_t size) {
    const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (config && config[0]) snprintf(path, size, "%s/" SETTINGS_DIRECTORY "/" SETTINGS_FILE, config);
    else if (home && home[0]) snprintf(path, size, "%s/.config/" SETTINGS_DIRECTORY "/" SETTINGS_FILE, home);
    else return false;
    return true;
}

static int name_index(const char *const *names, size_t count, const char *value) {
    for (size_t i = 0; i < count; i++)
        if (!strcmp(names[i], value)) return (int)i;
    return -1;
}

bool settings_load(struct settings *settings, const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return false;
    char line[SETTINGS_TEXT + 64];
    while (fgets(line, sizeof line, file)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *value = strchr(line, '=');
        if (line[0] == '#' || !value) continue;
        *value++ = '\0';
        int index;
        if (!strcmp(line, "font")) snprintf(settings->font, sizeof settings->font, "%s", value);
        else if (!strcmp(line, "font_size") && atof(value) > 0) settings->font_size = (float)atof(value);
        else if (!strcmp(line, "theme")) snprintf(settings->theme, sizeof settings->theme, "%s", value);
        else if (!strcmp(line, "autocomplete_style") && (index = name_index(style_names, 2, value)) >= 0)
            settings->autocomplete_style = (enum autocomplete_style)index;
        else if (!strcmp(line, "autocomplete_trigger") && (index = name_index(trigger_names, 2, value)) >= 0)
            settings->autocomplete_trigger = (enum autocomplete_trigger)index;
    }
    fclose(file);
    return true;
}

// Creates the directories of `path` (like mkdir -p of its parent).
static bool make_parent_directories(const char *path) {
    char directory[SETTINGS_TEXT * 2];
    snprintf(directory, sizeof directory, "%s", path);
    for (char *slash = strchr(directory + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        if (mkdir(directory, 0755) != 0 && errno != EEXIST) return false;
        *slash = '/';
    }
    return true;
}

bool settings_save(const struct settings *settings, const char *path) {
    if (!make_parent_directories(path)) return false;
    FILE *file = fopen(path, "w");
    if (!file) return false;
    fprintf(file, "# hydra-native settings; the settings menu in the toolbar writes this file.\n");
    fprintf(file, "font=%s\nfont_size=%g\ntheme=%s\nautocomplete_style=%s\nautocomplete_trigger=%s\n", settings->font,
            (double)settings->font_size, settings->theme, style_names[settings->autocomplete_style],
            trigger_names[settings->autocomplete_trigger]);
    return fclose(file) == 0;
}
