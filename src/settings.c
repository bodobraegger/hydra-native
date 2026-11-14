#define _GNU_SOURCE
#include "settings.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL3/SDL.h>

#include "quickjs.h"

#define SETTINGS_DIRECTORY "hydra-native"
#define SETTINGS_FILE "settings.json"
#define OLD_RECOVERY_FILE "recovery.js" // the one recovery file of older versions
#define RECOVERY_PREFIX "recovery-"
#define RECOVERY_TIME "%y%m%d-%H%M"
#define RECOVERY_EXTENSION ".js"
#define MAX_RECOVERY_NUMBER 1000 // of the files of the same minute
#define RECOVERY_HEADER "// hydra-native recovery of: " // then the path of the file, or nothing
#define JSON_INDENT 2

static const char *const style_names[] = { "list", "ghost" };
static const char *const trigger_names[] = { "manual", "typing" };
static const char *const signature_help_names[] = { "manual", "paren" };
static const char *const blend_names[] = { "normal", "difference" };
static const char *const whitespace_names[] = { "none", "boundary", "selection", "trailing", "all" };
static const char *const render_audio_names[] = { "sources", "file", "muted" };

const char *settings_whitespace_name(enum render_whitespace value) { return whitespace_names[value]; }

const char *settings_render_audio_name(enum render_audio value) { return render_audio_names[value]; }

void settings_defaults(struct settings *settings) {
    memset(settings, 0, sizeof *settings);
    snprintf(settings->font, sizeof settings->font, "%s", SETTINGS_DEFAULT_FONT);
    settings->font_size = SETTINGS_DEFAULT_FONT_SIZE;
    snprintf(settings->theme, sizeof settings->theme, "%s", SETTINGS_DEFAULT_THEME);
    // All invisible characters show by default, like Pulsar with showInvisibles.
    settings->render_whitespace = WHITESPACE_ALL;
    settings->word_wrap = true; // CodeMirror lineWrapping in the web editor
    settings->vsync = true;
    settings->code_background_opacity = SETTINGS_THEME_OPACITY;
    settings->completion_opacity = SETTINGS_DEFAULT_COMPLETION_OPACITY;
    settings->audio_gain = SETTINGS_DEFAULT_AUDIO_GAIN;
    settings->stream_rate = settings->stream_max_rate = SETTINGS_MAX_STREAM_RATE;
    settings->code_visible = true;
    settings->render_seconds = SETTINGS_DEFAULT_RENDER_SECONDS;
    settings->render_fps = SETTINGS_DEFAULT_RENDER_FPS;
    settings->render_width = SETTINGS_DEFAULT_RENDER_WIDTH;
    settings->render_height = SETTINGS_DEFAULT_RENDER_HEIGHT;
}

bool settings_parse_size(const char *text, int *width, int *height) {
    int parsed_width, parsed_height;
    char end;
    if (sscanf(text, " %d x %d %c", &parsed_width, &parsed_height, &end) != 2) return false;
    if (parsed_width <= 0 || parsed_height <= 0 || parsed_width % 2 || parsed_height % 2 ||
        parsed_width > SETTINGS_MAX_RENDER_SIZE || parsed_height > SETTINGS_MAX_RENDER_SIZE)
        return false;
    *width = parsed_width;
    *height = parsed_height;
    return true;
}

// XDG_CONFIG_HOME/hydra-native when XDG_CONFIG_HOME is set, on every system (the tests set it). Else Linux:
// ~/.config/hydra-native; macOS: ~/Library/Application Support/hydra-native; Windows: %APPDATA%\hydra-native.
// SDL_GetPrefPath gives the last two; on Linux it gives the data folder instead.
static bool settings_folder_file(const char *name, char *path, size_t size) {
    const char *config = getenv("XDG_CONFIG_HOME");
    if (config && config[0]) {
        snprintf(path, size, "%s/" SETTINGS_DIRECTORY "/%s", config, name);
        return true;
    }
#if defined(__linux__)
    const char *home = getenv("HOME");
    if (!home || !home[0]) return false;
    snprintf(path, size, "%s/.config/" SETTINGS_DIRECTORY "/%s", home, name);
#else
    char *folder = SDL_GetPrefPath(NULL, SETTINGS_DIRECTORY); // ends with the separator of the system
    if (!folder) return false;
    snprintf(path, size, "%s%s", folder, name);
    SDL_free(folder);
#endif
    return true;
}

bool settings_path(char *path, size_t size) { return settings_folder_file(SETTINGS_FILE, path, size); }


// ---------------------------------------------------------------- JSON through QuickJS

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    if (length < 0) length = 0; // a directory or a pipe
    fseek(file, 0, SEEK_SET);
    char *text = malloc((size_t)length + 1);
    size_t read = fread(text, 1, (size_t)length, file);
    text[read] = '\0';
    fclose(file);
    return text;
}

// Parses `text`; on an error, writes the message of QuickJS (with the line) to `error` and returns an exception.
static JSValue parse(JSContext *ctx, const char *text, const char *path, char *error, size_t size) {
    JSValue value = JS_ParseJSON(ctx, text, strlen(text), path);
    if (!JS_IsException(value)) return value;
    JSValue exception = JS_GetException(ctx);
    const char *message = JS_ToCString(ctx, exception);
    JSValue line = JS_GetPropertyStr(ctx, exception, "lineNumber");
    int32_t number = 0;
    JS_ToInt32(ctx, &number, line);
    if (number > 0) snprintf(error, size, "%s: %s (line %d)", path, message ? message : "error", number);
    else snprintf(error, size, "%s: %s", path, message ? message : "error");
    JS_FreeCString(ctx, message);
    JS_FreeValue(ctx, line);
    JS_FreeValue(ctx, exception);
    return JS_EXCEPTION;
}

static void read_string(JSContext *ctx, JSValueConst object, const char *key, char *out, size_t size) {
    JSValue value = JS_GetPropertyStr(ctx, object, key);
    if (JS_IsString(value)) {
        const char *text = JS_ToCString(ctx, value);
        snprintf(out, size, "%s", text);
        JS_FreeCString(ctx, text);
    }
    JS_FreeValue(ctx, value);
}

static void read_bool(JSContext *ctx, JSValueConst object, const char *key, bool *out) {
    JSValue value = JS_GetPropertyStr(ctx, object, key);
    if (JS_IsBool(value)) *out = JS_ToBool(ctx, value);
    JS_FreeValue(ctx, value);
}

static void read_number(JSContext *ctx, JSValueConst object, const char *key, double *out) {
    JSValue value = JS_GetPropertyStr(ctx, object, key);
    if (JS_IsNumber(value)) JS_ToFloat64(ctx, out, value);
    JS_FreeValue(ctx, value);
}

// The index of the value of `key` in `names`, or `current` without such a value.
static int read_choice(JSContext *ctx, JSValueConst object, const char *key, const char *const *names, size_t count,
                       int current) {
    char text[SETTINGS_NAME] = "";
    read_string(ctx, object, key, text, sizeof text);
    for (size_t i = 0; i < count; i++)
        if (!strcmp(names[i], text)) return (int)i;
    return current;
}

// A number from `minimum` to `maximum`, rounded; a negative or missing number keeps `*out`.
static void read_integer(JSContext *ctx, JSValueConst object, const char *key, int minimum, int maximum, int *out) {
    double number = *out;
    read_number(ctx, object, key, &number);
    if (number >= 0) *out = number > maximum ? maximum : number < minimum ? minimum : (int)(number + 0.5);
}

// Calls `pair` for each property of an object whose value is a string (or, for arrays of strings, each item).
static void read_pairs(JSContext *ctx, JSValueConst object,
                       void (*pair)(void *data, const char *key, const char *value), void *data) {
    JSPropertyEnum *properties;
    uint32_t count;
    if (!JS_IsObject(object) ||
        JS_GetOwnPropertyNames(ctx, &properties, &count, object, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return;
    for (uint32_t i = 0; i < count; i++) {
        const char *key = JS_AtomToCString(ctx, properties[i].atom);
        JSValue value = JS_GetProperty(ctx, object, properties[i].atom);
        bool array = JS_IsArray(ctx, value);
        int64_t length = 1;
        if (array) {
            JSValue length_value = JS_GetPropertyStr(ctx, value, "length");
            JS_ToInt64(ctx, &length, length_value);
            JS_FreeValue(ctx, length_value);
        }
        for (int64_t k = 0; k < length; k++) {
            JSValue item = array ? JS_GetPropertyUint32(ctx, value, (uint32_t)k) : JS_DupValue(ctx, value);
            if (JS_IsString(item)) {
                const char *text = JS_ToCString(ctx, item);
                pair(data, key, text);
                JS_FreeCString(ctx, text);
            }
            JS_FreeValue(ctx, item);
        }
        JS_FreeValue(ctx, value);
        JS_FreeCString(ctx, key);
        JS_FreeAtom(ctx, properties[i].atom);
    }
    js_free(ctx, properties);
}

static void add_keybinding(void *data, const char *key, const char *value) {
    struct settings *settings = data;
    if (settings->keybinding_count == MAX_KEYBINDINGS) return;
    struct settings_pair *pair = &settings->keybindings[settings->keybinding_count++];
    snprintf(pair->key, sizeof pair->key, "%s", key);
    snprintf(pair->value, sizeof pair->value, "%s", value);
}

static void add_theme_color(void *data, const char *key, const char *value) {
    struct custom_theme *theme = data;
    if (!strcmp(key, "base")) {
        snprintf(theme->base, sizeof theme->base, "%s", value);
    } else if (theme->color_count < MAX_THEME_COLORS) {
        struct settings_pair *pair = &theme->colors[theme->color_count++];
        snprintf(pair->key, sizeof pair->key, "%s", key);
        snprintf(pair->value, sizeof pair->value, "%s", value);
    }
}

static void read_themes(JSContext *ctx, JSValueConst root, struct settings *settings) {
    JSValue themes = JS_GetPropertyStr(ctx, root, "themes");
    JSPropertyEnum *properties;
    uint32_t count;
    if (JS_IsObject(themes) &&
        JS_GetOwnPropertyNames(ctx, &properties, &count, themes, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) >= 0) {
        for (uint32_t i = 0; i < count; i++) {
            if (settings->theme_count < MAX_CUSTOM_THEMES) {
                struct custom_theme *theme = &settings->themes[settings->theme_count++];
                const char *name = JS_AtomToCString(ctx, properties[i].atom);
                snprintf(theme->name, sizeof theme->name, "%s", name);
                JS_FreeCString(ctx, name);
                JSValue colors = JS_GetProperty(ctx, themes, properties[i].atom);
                read_pairs(ctx, colors, add_theme_color, theme);
                JS_FreeValue(ctx, colors);
            }
            JS_FreeAtom(ctx, properties[i].atom);
        }
        js_free(ctx, properties);
    }
    JS_FreeValue(ctx, themes);
}

bool settings_load(struct settings *settings, const char *path, char *error, size_t size) {
    error[0] = '\0';
    char *text = read_file(path);
    if (!text) return true; // no file: the defaults
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(runtime);
    JSValue root = parse(ctx, text, path, error, size);
    bool ok = !JS_IsException(root);
    if (ok && !JS_IsObject(root)) {
        snprintf(error, size, "%s: the settings must be a JSON object", path);
        ok = false;
    }
    if (ok) {
#define READ_CHOICE(key, names, field) \
    settings->field = (typeof(settings->field))read_choice(ctx, root, key, names, sizeof names / sizeof names[0], \
                                                           (int)settings->field)
        double number;
        read_string(ctx, root, "font", settings->font, sizeof settings->font);
        number = settings->font_size;
        read_number(ctx, root, "fontSize", &number);
        if (number > 0) settings->font_size = (float)number;
        read_string(ctx, root, "theme", settings->theme, sizeof settings->theme);
        READ_CHOICE("renderWhitespace", whitespace_names, render_whitespace);
        READ_CHOICE("autocompleteStyle", style_names, autocomplete_style);
        READ_CHOICE("autocompleteTrigger", trigger_names, autocomplete_trigger);
        READ_CHOICE("signatureHelp", signature_help_names, signature_help);
        read_integer(ctx, root, "autocompleteOpacity", 0, 100, &settings->completion_opacity);
        read_integer(ctx, root, "codeBackgroundOpacity", 0, 100, &settings->code_background_opacity);
        read_integer(ctx, root, "audioGain", 0, SETTINGS_MAX_AUDIO_GAIN, &settings->audio_gain);
        read_integer(ctx, root, "streamRate", 1, SETTINGS_MAX_STREAM_RATE, &settings->stream_rate);
        read_integer(ctx, root, "streamMaxRate", 1, SETTINGS_MAX_STREAM_RATE, &settings->stream_max_rate);
        READ_CHOICE("codeBackgroundBlend", blend_names, code_background_blend);
        READ_CHOICE("codeTextBlend", blend_names, code_text_blend);
        READ_CHOICE("renderAudio", render_audio_names, render_audio);
#undef READ_CHOICE
        read_bool(ctx, root, "wordWrap", &settings->word_wrap);
        read_bool(ctx, root, "codeVisible", &settings->code_visible);
        read_bool(ctx, root, "fullscreen", &settings->fullscreen);
        read_bool(ctx, root, "vsync", &settings->vsync);
        read_bool(ctx, root, "watch", &settings->watch);
        read_string(ctx, root, "audioSource", settings->audio_source, sizeof settings->audio_source);
        read_string(ctx, root, "language", settings->language, sizeof settings->language);
        read_string(ctx, root, "galleryServer", settings->gallery_server, sizeof settings->gallery_server);
        read_string(ctx, root, "l10nUrl", settings->l10n_url, sizeof settings->l10n_url);
        read_string(ctx, root, "socket", settings->socket, sizeof settings->socket);
        read_string(ctx, root, "setupFile", settings->setup_file, sizeof settings->setup_file);
        read_string(ctx, root, "renderFile", settings->render_file, sizeof settings->render_file);
        read_integer(ctx, root, "renderLength", 1, SETTINGS_MAX_RENDER_SECONDS, &settings->render_seconds);
        read_integer(ctx, root, "renderFps", 1, SETTINGS_MAX_RENDER_FPS, &settings->render_fps);
        char render_size[SETTINGS_NAME] = "";
        read_string(ctx, root, "renderSize", render_size, sizeof render_size);
        settings_parse_size(render_size, &settings->render_width, &settings->render_height);
        read_string(ctx, root, "renderAudioFile", settings->render_audio_file,
                    sizeof settings->render_audio_file);
        read_themes(ctx, root, settings);
        JSValue keybindings = JS_GetPropertyStr(ctx, root, "keybindings");
        read_pairs(ctx, keybindings, add_keybinding, settings);
        JS_FreeValue(ctx, keybindings);
    }
    JS_FreeValue(ctx, root);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    free(text);
    return ok;
}

// Creates the directories of `path` (like mkdir -p of its parent). SDL_GetPrefPath already made the folder of a
// path with the separators of Windows.
static bool make_parent_directories(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return true;
    char directory[SETTINGS_TEXT * 2];
    snprintf(directory, sizeof directory, "%.*s", (int)(slash - path), path);
    return SDL_CreateDirectory(directory);
}

// Writes `text`, then `end`, into a temporary file and renames it to `path`: a reader never sees half a file.
static bool write_atomically(const char *path, const char *text, const char *end) {
    char temporary[SETTINGS_TEXT * 2 + 8];
    snprintf(temporary, sizeof temporary, "%s.tmp", path);
    FILE *file = make_parent_directories(path) ? fopen(temporary, "wb") : NULL;
    bool ok = file && fputs(text, file) >= 0 && fputs(end, file) >= 0;
    if (file && fclose(file) != 0) ok = false;
    if (ok && !SDL_RenamePath(temporary, path)) ok = false; // rename() of Windows fails on an existing file
    if (!ok && file) {
        int error = errno; // for the message of the caller
        remove(temporary);
        errno = error;
    }
    return ok;
}

static void set_string(JSContext *ctx, JSValueConst object, const char *key, const char *value) {
    JS_SetPropertyStr(ctx, object, key, JS_NewString(ctx, value));
}

bool settings_save(const struct settings *settings, const char *path, char *error, size_t size) {
    error[0] = '\0';
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(runtime);
    char *text = read_file(path);
    JSValue root = text ? parse(ctx, text, path, error, size) : JS_NewObject(ctx);
    free(text);
    bool ok = !JS_IsException(root) && JS_IsObject(root);
    if (!ok && !error[0]) snprintf(error, size, "%s: the settings must be a JSON object", path);
    if (ok) {
        set_string(ctx, root, "font", settings->font);
        JS_SetPropertyStr(ctx, root, "fontSize", JS_NewFloat64(ctx, settings->font_size));
        set_string(ctx, root, "theme", settings->theme);
        set_string(ctx, root, "renderWhitespace", whitespace_names[settings->render_whitespace]);
        set_string(ctx, root, "autocompleteStyle", style_names[settings->autocomplete_style]);
        set_string(ctx, root, "autocompleteTrigger", trigger_names[settings->autocomplete_trigger]);
        set_string(ctx, root, "signatureHelp", signature_help_names[settings->signature_help]);
        JS_SetPropertyStr(ctx, root, "autocompleteOpacity", JS_NewInt32(ctx, settings->completion_opacity));
        JSAtom opacity = JS_NewAtom(ctx, "codeBackgroundOpacity");
        if (settings->code_background_opacity == SETTINGS_THEME_OPACITY) JS_DeleteProperty(ctx, root, opacity, 0);
        else JS_SetProperty(ctx, root, opacity, JS_NewInt32(ctx, settings->code_background_opacity));
        JS_FreeAtom(ctx, opacity);
        set_string(ctx, root, "codeBackgroundBlend", blend_names[settings->code_background_blend]);
        set_string(ctx, root, "codeTextBlend", blend_names[settings->code_text_blend]);
        JS_SetPropertyStr(ctx, root, "wordWrap", JS_NewBool(ctx, settings->word_wrap));
        JS_SetPropertyStr(ctx, root, "codeVisible", JS_NewBool(ctx, settings->code_visible));
        JS_SetPropertyStr(ctx, root, "fullscreen", JS_NewBool(ctx, settings->fullscreen));
        JS_SetPropertyStr(ctx, root, "vsync", JS_NewBool(ctx, settings->vsync));
        JS_SetPropertyStr(ctx, root, "watch", JS_NewBool(ctx, settings->watch));
        set_string(ctx, root, "audioSource", settings->audio_source);
        JS_SetPropertyStr(ctx, root, "audioGain", JS_NewInt32(ctx, settings->audio_gain));
        JS_SetPropertyStr(ctx, root, "streamRate", JS_NewInt32(ctx, settings->stream_rate));
        JS_SetPropertyStr(ctx, root, "streamMaxRate", JS_NewInt32(ctx, settings->stream_max_rate));
        set_string(ctx, root, "language", settings->language);
        set_string(ctx, root, "galleryServer", settings->gallery_server);
        set_string(ctx, root, "l10nUrl", settings->l10n_url);
        set_string(ctx, root, "setupFile", settings->setup_file);
        set_string(ctx, root, "renderFile", settings->render_file);
        JS_SetPropertyStr(ctx, root, "renderLength", JS_NewInt32(ctx, settings->render_seconds));
        JS_SetPropertyStr(ctx, root, "renderFps", JS_NewInt32(ctx, settings->render_fps));
        char render_size[SETTINGS_NAME];
        snprintf(render_size, sizeof render_size, "%dx%d", settings->render_width, settings->render_height);
        set_string(ctx, root, "renderSize", render_size);
        set_string(ctx, root, "renderAudio", render_audio_names[settings->render_audio]);
        set_string(ctx, root, "renderAudioFile", settings->render_audio_file);
        JSValue json = JS_JSONStringify(ctx, root, JS_UNDEFINED, JS_NewInt32(ctx, JSON_INDENT));
        const char *output = JS_ToCString(ctx, json);
        ok = output && write_atomically(path, output, "\n");
        if (!ok) snprintf(error, size, "cannot write %s: %s", path, strerror(errno));
        JS_FreeCString(ctx, output);
        JS_FreeValue(ctx, json);
    }
    JS_FreeValue(ctx, root);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    return ok;
}

bool recovery_create(char *path, size_t size, time_t time) {
    char name[SETTINGS_NAME], base[PATH_MAX];
    strftime(name, sizeof name, RECOVERY_PREFIX RECOVERY_TIME, localtime(&time));
    if (!settings_folder_file(name, base, sizeof base) || !make_parent_directories(base)) return false;
    for (int number = 1; number <= MAX_RECOVERY_NUMBER; number++) {
        if (number == 1) snprintf(path, size, "%s" RECOVERY_EXTENSION, base);
        else snprintf(path, size, "%s-%d" RECOVERY_EXTENSION, base, number);
        FILE *file = fopen(path, "wx"); // fails when another run has the name
        if (file) return fclose(file) == 0;
        if (errno != EEXIST) return false;
    }
    return false;
}

static bool is_recovery_name(const char *name) {
    size_t length = strlen(name), prefix = strlen(RECOVERY_PREFIX), extension = strlen(RECOVERY_EXTENSION);
    if (!strcmp(name, OLD_RECOVERY_FILE)) return true;
    return length > prefix + extension && !strncmp(name, RECOVERY_PREFIX, prefix) &&
           !strcmp(name + length - extension, RECOVERY_EXTENSION);
}

struct recovery_entry {
    char *path;
    time_t modified;
};

static int newest_first(const void *first, const void *second) {
    const struct recovery_entry *a = first, *b = second;
    return a->modified != b->modified ? (a->modified < b->modified ? 1 : -1) : strcmp(b->path, a->path);
}

char **recovery_list(size_t *count) {
    *count = 0;
    char folder[PATH_MAX];
    DIR *directory = settings_folder_file("", folder, sizeof folder) ? opendir(folder) : NULL;
    if (!directory) return NULL;
    struct recovery_entry *entries = NULL;
    size_t capacity = 0;
    struct dirent *item;
    struct stat status;
    while ((item = readdir(directory))) {
        char *path;
        if (!is_recovery_name(item->d_name) || asprintf(&path, "%s%s", folder, item->d_name) < 0) continue;
        if (*count == capacity) {
            capacity = capacity ? capacity * 2 : 8;
            entries = realloc(entries, capacity * sizeof *entries);
        }
        entries[(*count)++] = (struct recovery_entry){ path, stat(path, &status) == 0 ? status.st_mtime : 0 };
    }
    closedir(directory);
    if (entries) qsort(entries, *count, sizeof *entries, newest_first);
    char **paths = malloc((*count ? *count : 1) * sizeof *paths);
    for (size_t index = 0; index < *count; index++) paths[index] = entries[index].path;
    free(entries);
    return paths;
}

bool recovery_save(const char *path, const char *file, const char *code) {
    char header[sizeof RECOVERY_HEADER + PATH_MAX + 1];
    snprintf(header, sizeof header, RECOVERY_HEADER "%s\n", file);
    return write_atomically(path, header, code);
}

char *recovery_load(const char *path, char **file) {
    char *text = read_file(path);
    size_t header = strlen(RECOVERY_HEADER);
    char *end = text && !strncmp(text, RECOVERY_HEADER, header) ? strchr(text + header, '\n') : NULL;
    if (!end) {
        free(text);
        *file = NULL;
        return NULL;
    }
    *file = strndup(text + header, (size_t)(end - text) - header);
    char *code = strdup(end + 1);
    free(text);
    return code;
}
