#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "quickjs.h"

#define SETTINGS_DIRECTORY "hydra-native"
#define SETTINGS_FILE "settings.json"
#define JSON_INDENT 2

static const char *const style_names[] = { "list", "ghost" };
static const char *const trigger_names[] = { "manual", "typing" };
static const char *const whitespace_names[] = { "none", "boundary", "selection", "trailing", "all" };

const char *settings_whitespace_name(enum render_whitespace value) { return whitespace_names[value]; }

void settings_defaults(struct settings *settings) {
    memset(settings, 0, sizeof *settings);
    snprintf(settings->font, sizeof settings->font, "%s", SETTINGS_DEFAULT_FONT);
    settings->font_size = SETTINGS_DEFAULT_FONT_SIZE;
    snprintf(settings->theme, sizeof settings->theme, "%s", SETTINGS_DEFAULT_THEME);
    // Pulsar shows invisible characters (showInvisibles) in the setup of the user, so all of them show by default.
    settings->render_whitespace = WHITESPACE_ALL;
    settings->word_wrap = true; // CodeMirror lineWrapping in the web editor
    settings->code_visible = true;
    settings->vsync = true;
    settings->analog_agc = true;
    settings->window_width = SETTINGS_DEFAULT_WIDTH;
    settings->window_height = SETTINGS_DEFAULT_HEIGHT;
}

bool settings_path(char *path, size_t size) {
    const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (config && config[0]) snprintf(path, size, "%s/" SETTINGS_DIRECTORY "/" SETTINGS_FILE, config);
    else if (home && home[0]) snprintf(path, size, "%s/.config/" SETTINGS_DIRECTORY "/" SETTINGS_FILE, home);
    else return false;
    return true;
}

// ---------------------------------------------------------------- JSON through QuickJS

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
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

static void read_choice(JSContext *ctx, JSValueConst object, const char *key, const char *const *names, size_t count, int *out) {
    char text[SETTINGS_NAME] = "";
    read_string(ctx, object, key, text, sizeof text);
    for (size_t i = 0; i < count; i++)
        if (!strcmp(names[i], text)) *out = (int)i;
}

// Calls `pair` for each property of an object whose value is a string (or, for arrays of strings, each item).
static void read_pairs(JSContext *ctx, JSValueConst object, void (*pair)(void *data, const char *key, const char *value), void *data) {
    JSPropertyEnum *properties;
    uint32_t count;
    if (!JS_IsObject(object) || JS_GetOwnPropertyNames(ctx, &properties, &count, object, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
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
    if (JS_IsObject(themes) && JS_GetOwnPropertyNames(ctx, &properties, &count, themes, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) >= 0) {
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
        double number;
        read_string(ctx, root, "font", settings->font, sizeof settings->font);
        number = settings->font_size;
        read_number(ctx, root, "fontSize", &number);
        if (number > 0) settings->font_size = (float)number;
        read_string(ctx, root, "theme", settings->theme, sizeof settings->theme);
        int choice = (int)settings->render_whitespace;
        read_choice(ctx, root, "renderWhitespace", whitespace_names, 5, &choice);
        settings->render_whitespace = (enum render_whitespace)choice;
        choice = (int)settings->autocomplete_style;
        read_choice(ctx, root, "autocompleteStyle", style_names, 2, &choice);
        settings->autocomplete_style = (enum autocomplete_style)choice;
        choice = (int)settings->autocomplete_trigger;
        read_choice(ctx, root, "autocompleteTrigger", trigger_names, 2, &choice);
        settings->autocomplete_trigger = (enum autocomplete_trigger)choice;
        read_bool(ctx, root, "wordWrap", &settings->word_wrap);
        read_bool(ctx, root, "codeVisible", &settings->code_visible);
        read_bool(ctx, root, "fullscreen", &settings->fullscreen);
        read_bool(ctx, root, "vsync", &settings->vsync);
        read_bool(ctx, root, "watch", &settings->watch);
        read_bool(ctx, root, "analogAgc", &settings->analog_agc);
        number = settings->window_width;
        read_number(ctx, root, "windowWidth", &number);
        if (number > 0) settings->window_width = (int)number;
        number = settings->window_height;
        read_number(ctx, root, "windowHeight", &number);
        if (number > 0) settings->window_height = (int)number;
        read_string(ctx, root, "audioSource", settings->audio_source, sizeof settings->audio_source);
        read_string(ctx, root, "language", settings->language, sizeof settings->language);
        read_string(ctx, root, "galleryServer", settings->gallery_server, sizeof settings->gallery_server);
        read_string(ctx, root, "l10nUrl", settings->l10n_url, sizeof settings->l10n_url);
        read_string(ctx, root, "socket", settings->socket, sizeof settings->socket);
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
        JS_SetPropertyStr(ctx, root, "wordWrap", JS_NewBool(ctx, settings->word_wrap));
        JSValue json = JS_JSONStringify(ctx, root, JS_UNDEFINED, JS_NewInt32(ctx, JSON_INDENT));
        const char *output = JS_ToCString(ctx, json);
        // A temporary file and rename(): a reader never sees half a file.
        char temporary[SETTINGS_TEXT * 2 + 8];
        snprintf(temporary, sizeof temporary, "%s.tmp", path);
        FILE *file = make_parent_directories(path) && output ? fopen(temporary, "w") : NULL;
        ok = file && fputs(output, file) >= 0 && fputc('\n', file) != EOF;
        if (file && fclose(file) != 0) ok = false;
        if (ok && rename(temporary, path) != 0) ok = false;
        if (!ok) {
            snprintf(error, size, "cannot write %s: %s", path, strerror(errno));
            if (file) unlink(temporary);
        }
        JS_FreeCString(ctx, output);
        JS_FreeValue(ctx, json);
    }
    JS_FreeValue(ctx, root);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    return ok;
}
