// All keys of hydra-native in one table. Change a key here and run `make`.
// The order matters: the first matching binding wins.
#include "keymap.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const struct key_binding default_keymap[] = {
    // while the autocomplete list is open
    { SDLK_DOWN, 0, COMMAND_COMPLETION_NEXT, .context = CONTEXT_COMPLETION },
    { SDLK_UP, 0, COMMAND_COMPLETION_PREVIOUS, .context = CONTEXT_COMPLETION },
    { SDLK_RETURN, 0, COMMAND_COMPLETION_ACCEPT, .context = CONTEXT_COMPLETION },
    { SDLK_TAB, 0, COMMAND_COMPLETION_ACCEPT, .context = CONTEXT_COMPLETION },

    // while a ghost-text suggestion shows (the inline suggestion keys of VS Code); Escape dismisses it
    { SDLK_TAB, 0, COMMAND_COMPLETION_ACCEPT, .context = CONTEXT_GHOST },
    { SDLK_RIGHTBRACKET, KEY_ALT, COMMAND_COMPLETION_NEXT, .context = CONTEXT_GHOST },
    { SDLK_LEFTBRACKET, KEY_ALT, COMMAND_COMPLETION_PREVIOUS, .context = CONTEXT_GHOST },

    // while the drop-down list of the toolbar is open; Escape closes it
    { SDLK_DOWN, 0, COMMAND_MENU_NEXT, .context = CONTEXT_MENU },
    { SDLK_UP, 0, COMMAND_MENU_PREVIOUS, .context = CONTEXT_MENU },
    { SDLK_RETURN, 0, COMMAND_MENU_ACCEPT, .context = CONTEXT_MENU },
    { SDLK_KP_ENTER, 0, COMMAND_MENU_ACCEPT, .context = CONTEXT_MENU },

    // while a number of the settings menu is a text field
    { SDLK_RETURN, 0, COMMAND_NUMBER_ACCEPT, .context = CONTEXT_NUMBER },
    { SDLK_KP_ENTER, 0, COMMAND_NUMBER_ACCEPT, .context = CONTEXT_NUMBER },
    { SDLK_ESCAPE, 0, COMMAND_NUMBER_CANCEL, .context = CONTEXT_NUMBER },
    { SDLK_UP, 0, COMMAND_NUMBER_UP, .context = CONTEXT_NUMBER },
    { SDLK_DOWN, 0, COMMAND_NUMBER_DOWN, .context = CONTEXT_NUMBER },
    { SDLK_UP, KEY_SHIFT, COMMAND_NUMBER_UP_LARGE, .context = CONTEXT_NUMBER },
    { SDLK_DOWN, KEY_SHIFT, COMMAND_NUMBER_DOWN_LARGE, .context = CONTEXT_NUMBER },
    { SDLK_BACKSPACE, 0, COMMAND_NUMBER_BACKSPACE, .context = CONTEXT_NUMBER },

    // while an inserted call has tab stops on its arguments, as the snippets of VS Code
    { SDLK_TAB, 0, COMMAND_SNIPPET_NEXT, .context = CONTEXT_SNIPPET },
    { SDLK_TAB, KEY_SHIFT, COMMAND_SNIPPET_PREVIOUS, .context = CONTEXT_SNIPPET },
    { SDLK_ESCAPE, 0, COMMAND_SNIPPET_END, .context = CONTEXT_SNIPPET },

    // while find, go to line or the command palette is open
    { SDLK_RETURN, 0, COMMAND_PROMPT_ACCEPT, .context = CONTEXT_PROMPT },
    { SDLK_KP_ENTER, 0, COMMAND_PROMPT_ACCEPT, .context = CONTEXT_PROMPT },
    { SDLK_RETURN, KEY_SHIFT, COMMAND_PROMPT_ACCEPT_BACKWARD, .context = CONTEXT_PROMPT },
    { SDLK_TAB, 0, COMMAND_NONE, .context = CONTEXT_PROMPT }, // does not indent the code behind the prompt
    { SDLK_UP, 0, COMMAND_PROMPT_UP, .context = CONTEXT_PROMPT },
    { SDLK_DOWN, 0, COMMAND_PROMPT_DOWN, .context = CONTEXT_PROMPT },
    { SDLK_BACKSPACE, 0, COMMAND_PROMPT_BACKSPACE, .context = CONTEXT_PROMPT },

    // overlay, evaluation and files (the keys of hydra.ojack.xyz and of the Pulsar package atom-hydra)
    { SDLK_H, KEY_CTRL | KEY_SHIFT, COMMAND_TOGGLE_CODE, .global = true },
    { SDLK_RETURN, KEY_CTRL | KEY_ALT, COMMAND_EVAL_ALL, .global = true },
    { SDLK_RETURN, KEY_CTRL | KEY_SHIFT, COMMAND_EVAL_ALL, .global = true },
    { SDLK_RETURN, KEY_CTRL, COMMAND_EVAL_LINE, .global = true },
    { SDLK_RETURN, KEY_SHIFT, COMMAND_EVAL_LINE, .global = true },
    { SDLK_RETURN, KEY_ALT, COMMAND_EVAL_BLOCK, .global = true },
    { SDLK_S, KEY_CTRL, COMMAND_SAVE, .global = true },
    { SDLK_S, KEY_CTRL | KEY_SHIFT, COMMAND_SCREENCAP, .global = true },
    { SDLK_L, KEY_CTRL | KEY_SHIFT, COMMAND_SAVE_TO_URL, .global = true },
    { SDLK_O, KEY_CTRL | KEY_SHIFT, COMMAND_OPEN_URL, .global = true },
    { SDLK_G, KEY_CTRL | KEY_SHIFT, COMMAND_GALLERY_SHARE, .global = true },
    { SDLK_F11, 0, COMMAND_FULLSCREEN, .global = true },
    { SDLK_Q, KEY_CTRL, COMMAND_QUIT, .global = true },
    { SDLK_ESCAPE, 0, COMMAND_ESCAPE, .global = true },
    { SDLK_F1, 0, COMMAND_TOGGLE_HELP, .global = true },
    { SDLK_COMMA, KEY_CTRL, COMMAND_TOGGLE_SETTINGS, .global = true }, // the settings key of VS Code
    { SDLK_I, KEY_CTRL | KEY_ALT, COMMAND_COPY_PATCH_BAY_ID, .global = true },

    // toolbar actions of hydra.ojack.xyz (the web editor has no keys for them)
    { SDLK_DELETE, KEY_CTRL | KEY_SHIFT, COMMAND_CLEAR_ALL, .global = true },
    { SDLK_F7, 0, COMMAND_SHOW_EXAMPLE, .global = true },
    { SDLK_F8, 0, COMMAND_MUTATE, .global = true },
    { SDLK_F8, KEY_CTRL, COMMAND_MUTATE_TRANSFORM, .global = true },
    { SDLK_F8, KEY_SHIFT, COMMAND_MUTATE_UNDO, .global = true },

    // editing
    { SDLK_Z, KEY_CTRL, COMMAND_UNDO },
    { SDLK_Z, KEY_CTRL | KEY_SHIFT, COMMAND_REDO },
    { SDLK_Y, KEY_CTRL, COMMAND_REDO },
    { SDLK_C, KEY_CTRL, COMMAND_COPY },
    { SDLK_X, KEY_CTRL, COMMAND_CUT },
    { SDLK_V, KEY_CTRL, COMMAND_PASTE },
    { SDLK_A, KEY_CTRL, COMMAND_SELECT_ALL },
    { SDLK_SLASH, KEY_CTRL, COMMAND_TOGGLE_COMMENT },
    { SDLK_7, KEY_CTRL | KEY_SHIFT, COMMAND_TOGGLE_COMMENT }, // Ctrl+/ on the Swiss (ch) layout
    { SDLK_UP, KEY_ALT, COMMAND_MOVE_LINE_UP },
    { SDLK_DOWN, KEY_ALT, COMMAND_MOVE_LINE_DOWN },
    { SDLK_DOWN, KEY_ALT | KEY_SHIFT, COMMAND_DUPLICATE_LINE },
    { SDLK_D, KEY_CTRL | KEY_SHIFT, COMMAND_DUPLICATE_LINE }, // the sublime keymap of the web editor
    { SDLK_F, KEY_ALT | KEY_SHIFT, COMMAND_FORMAT },
    { SDLK_F, KEY_CTRL | KEY_SHIFT, COMMAND_FORMAT }, // the key of hydra.ojack.xyz
    { SDLK_D, KEY_CTRL, COMMAND_SELECT_NEXT_OCCURRENCE },
    { SDLK_F, KEY_CTRL, COMMAND_FIND },
    { SDLK_F3, 0, COMMAND_FIND_NEXT },
    { SDLK_F3, KEY_SHIFT, COMMAND_FIND_PREVIOUS },
    { SDLK_G, KEY_CTRL, COMMAND_GO_TO_LINE },
    { SDLK_P, KEY_CTRL | KEY_SHIFT, COMMAND_COMMAND_PALETTE, .global = true },
    { SDLK_Z, KEY_ALT, COMMAND_TOGGLE_WRAP },
    { SDLK_SPACE, KEY_CTRL, COMMAND_TRIGGER_COMPLETION },
    { SDLK_L, KEY_CTRL, COMMAND_SELECT_LINE },
    { SDLK_K, KEY_CTRL | KEY_SHIFT, COMMAND_DELETE_LINE },
    { SDLK_J, KEY_CTRL, COMMAND_JOIN_LINES },
    { SDLK_F9, 0, COMMAND_SORT_LINES },
    { SDLK_F9, KEY_CTRL, COMMAND_SORT_LINES_INSENSITIVE },
    { SDLK_F9, KEY_SHIFT, COMMAND_SORT_LINES_REVERSE },
    { SDLK_F9, KEY_CTRL | KEY_SHIFT, COMMAND_SORT_LINES_REVERSE_INSENSITIVE },
    { SDLK_F2, KEY_CTRL, COMMAND_TOGGLE_BOOKMARK },
    { SDLK_F2, 0, COMMAND_NEXT_BOOKMARK },
    { SDLK_F2, KEY_SHIFT, COMMAND_PREVIOUS_BOOKMARK },
    { SDLK_F2, KEY_CTRL | KEY_SHIFT, COMMAND_CLEAR_BOOKMARKS },
    { SDLK_F2, KEY_ALT, COMMAND_SELECT_BOOKMARKS },
    { SDLK_F3, KEY_CTRL, COMMAND_FIND_UNDER },
    { SDLK_F3, KEY_CTRL | KEY_SHIFT, COMMAND_FIND_UNDER_PREVIOUS },
    { SDLK_F3, KEY_ALT, COMMAND_FIND_ALL_UNDER },
    { SDLK_SPACE, KEY_CTRL | KEY_SHIFT, COMMAND_SIGNATURE_HELP }, // in sublime: select scope, which the palette keeps
    { SDLK_Q, KEY_ALT, COMMAND_NONE }, // wrapLines of the sublime keymap needs the hardwrap addon, which the web lacks
    { SDLK_LEFTBRACKET, KEY_CTRL | KEY_SHIFT, COMMAND_NONE }, // fold: the web editor loads no fold addon
    { SDLK_RIGHTBRACKET, KEY_CTRL | KEY_SHIFT, COMMAND_NONE },

    // two-key commands of the sublime keymap: Ctrl+K, then the second key
    { SDLK_K, KEY_CTRL, COMMAND_CHORD },
    { SDLK_U, KEY_CTRL, COMMAND_UPPER_CASE, .prefix = SDLK_K },
    { SDLK_L, KEY_CTRL, COMMAND_LOWER_CASE, .prefix = SDLK_K },
    { SDLK_K, KEY_CTRL, COMMAND_DELETE_LINE_RIGHT, .prefix = SDLK_K },
    { SDLK_BACKSPACE, KEY_CTRL, COMMAND_DELETE_LINE_LEFT, .prefix = SDLK_K },
    { SDLK_C, KEY_CTRL, COMMAND_SHOW_IN_CENTER, .prefix = SDLK_K },
    { SDLK_G, KEY_CTRL, COMMAND_CLEAR_BOOKMARKS, .prefix = SDLK_K },
    { SDLK_SPACE, KEY_CTRL, COMMAND_SET_MARK, .prefix = SDLK_K },
    { SDLK_A, KEY_CTRL, COMMAND_SELECT_TO_MARK, .prefix = SDLK_K },
    { SDLK_W, KEY_CTRL, COMMAND_DELETE_TO_MARK, .prefix = SDLK_K },
    { SDLK_X, KEY_CTRL, COMMAND_SWAP_WITH_MARK, .prefix = SDLK_K },
    { SDLK_Y, KEY_CTRL, COMMAND_YANK, .prefix = SDLK_K },
    { SDLK_D, KEY_CTRL, COMMAND_SKIP_AND_SELECT_NEXT, .prefix = SDLK_K },
    { SDLK_0, KEY_CTRL, COMMAND_NONE, .prefix = SDLK_K }, // unfold all, fold all: no fold addon in the web editor
    { SDLK_1, KEY_CTRL, COMMAND_NONE, .prefix = SDLK_K },
    { SDLK_J, KEY_CTRL, COMMAND_NONE, .prefix = SDLK_K },
    { SDLK_DELETE, KEY_CTRL, COMMAND_DELETE_WORD_RIGHT },
    { SDLK_RIGHTBRACKET, KEY_CTRL, COMMAND_INDENT_MORE },
    { SDLK_LEFTBRACKET, KEY_CTRL, COMMAND_INDENT_LESS },
    { SDLK_M, KEY_CTRL, COMMAND_GO_TO_BRACKET },
    { SDLK_M, KEY_CTRL | KEY_SHIFT, COMMAND_SELECT_BETWEEN_BRACKETS },
    { SDLK_UP, KEY_CTRL | KEY_SHIFT, COMMAND_ADD_CURSOR_ABOVE }, // as in Pulsar
    { SDLK_DOWN, KEY_CTRL | KEY_SHIFT, COMMAND_ADD_CURSOR_BELOW },
    { SDLK_UP, KEY_CTRL | KEY_ALT, COMMAND_ADD_CURSOR_ABOVE }, // the sublime keymap of the web editor
    { SDLK_DOWN, KEY_CTRL | KEY_ALT, COMMAND_ADD_CURSOR_BELOW },
    { SDLK_I, KEY_ALT | KEY_SHIFT, COMMAND_SPLIT_SELECTION }, // VS Code; Ctrl+Shift+L saves to URL as in the web
    { SDLK_EQUALS, KEY_CTRL, COMMAND_FONT_BIGGER },
    { SDLK_EQUALS, KEY_CTRL | KEY_SHIFT, COMMAND_FONT_BIGGER }, // Ctrl+plus on the us layout
    { SDLK_1, KEY_CTRL | KEY_SHIFT, COMMAND_FONT_BIGGER },      // Ctrl+plus on the Swiss (ch) layout
    { SDLK_PLUS, KEY_CTRL, COMMAND_FONT_BIGGER },
    { SDLK_KP_PLUS, KEY_CTRL, COMMAND_FONT_BIGGER },
    { SDLK_MINUS, KEY_CTRL, COMMAND_FONT_SMALLER },
    { SDLK_KP_MINUS, KEY_CTRL, COMMAND_FONT_SMALLER },
    { SDLK_UP, KEY_CTRL, COMMAND_NONE },
    { SDLK_DOWN, KEY_CTRL, COMMAND_NONE },
    { SDLK_RETURN, 0, COMMAND_NEWLINE },
    { SDLK_KP_ENTER, 0, COMMAND_NEWLINE },
    { SDLK_BACKSPACE, KEY_CTRL, COMMAND_DELETE_WORD_LEFT },
    { SDLK_BACKSPACE, 0, COMMAND_BACKSPACE },
    { SDLK_BACKSPACE, KEY_SHIFT, COMMAND_BACKSPACE },
    { SDLK_DELETE, 0, COMMAND_DELETE },
    { SDLK_TAB, 0, COMMAND_INDENT },
    { SDLK_TAB, KEY_SHIFT, COMMAND_OUTDENT },

    // cursor motion; Shift extends the selection
    { SDLK_LEFT, KEY_CTRL, COMMAND_WORD_LEFT, .shift_extends = true },
    { SDLK_RIGHT, KEY_CTRL, COMMAND_WORD_RIGHT, .shift_extends = true },
    { SDLK_LEFT, KEY_ALT, COMMAND_SUBWORD_LEFT, .shift_extends = true }, // the sublime keymap of the web editor
    { SDLK_RIGHT, KEY_ALT, COMMAND_SUBWORD_RIGHT, .shift_extends = true },
    { SDLK_HOME, KEY_CTRL, COMMAND_DOCUMENT_START, .shift_extends = true },
    { SDLK_END, KEY_CTRL, COMMAND_DOCUMENT_END, .shift_extends = true },
    { SDLK_LEFT, 0, COMMAND_LEFT, .shift_extends = true },
    { SDLK_RIGHT, 0, COMMAND_RIGHT, .shift_extends = true },
    { SDLK_UP, 0, COMMAND_UP, .shift_extends = true },
    { SDLK_DOWN, 0, COMMAND_DOWN, .shift_extends = true },
    { SDLK_HOME, 0, COMMAND_LINE_START, .shift_extends = true },
    { SDLK_END, 0, COMMAND_LINE_END, .shift_extends = true },
    { SDLK_PAGEUP, 0, COMMAND_PAGE_UP, .shift_extends = true },
    { SDLK_PAGEDOWN, 0, COMMAND_PAGE_DOWN, .shift_extends = true },
};

static unsigned binding_modifiers(SDL_Keymod modifiers) {
    return (modifiers & SDL_KMOD_CTRL ? KEY_CTRL : 0) | (modifiers & SDL_KMOD_SHIFT ? KEY_SHIFT : 0) |
           (modifiers & SDL_KMOD_LALT ? KEY_ALT : 0);
}

// The table in use: the default table with the "keybindings" of the settings file on top.
static struct key_binding *keymap;
static size_t keymap_count;
static bool rebound[COMMAND_COUNT];

static void ensure_keymap(void) {
    if (keymap) return;
    keymap_count = sizeof default_keymap / sizeof default_keymap[0];
    keymap = malloc(sizeof default_keymap);
    memcpy(keymap, default_keymap, sizeof default_keymap);
}

const char *command_name(enum command command) {
    static char names[COMMAND_COUNT][48];
    static const char *const upper[] = {
#define COMMAND_NAME(name, description) #name,
        COMMANDS(COMMAND_NAME)
#undef COMMAND_NAME
    };
    if (command >= COMMAND_COUNT) return "";
    if (!names[command][0])
        for (size_t i = 0; upper[command][i] && i + 1 < sizeof names[0]; i++)
            names[command][i] = (char)tolower((unsigned char)upper[command][i]);
    return names[command];
}

// One key of a binding, such as "Ctrl+Shift+Enter" or "F3"; false for an unknown name.
static bool parse_key(const char *text, unsigned *modifiers, SDL_Keycode *key) {
    char part[64];
    *modifiers = 0;
    *key = SDLK_UNKNOWN;
    for (const char *start = text; *start;) {
        size_t length = strcspn(start, "+");
        if (length == 0 && start[0] == '+') length = 1; // "Ctrl++" is Ctrl and the plus key
        snprintf(part, sizeof part, "%.*s", (int)length, start);
        start += length;
        if (*start == '+' && start[1]) start++;
        if (!strcasecmp(part, "ctrl") || !strcasecmp(part, "control")) *modifiers |= KEY_CTRL;
        else if (!strcasecmp(part, "shift")) *modifiers |= KEY_SHIFT;
        else if (!strcasecmp(part, "alt")) *modifiers |= KEY_ALT;
        else if (!strcasecmp(part, "enter")) *key = SDLK_RETURN;
        else if (!strcasecmp(part, "esc")) *key = SDLK_ESCAPE;
        else if (strlen(part) == 1) *key = (SDL_Keycode)tolower((unsigned char)part[0]);
        else if ((*key = SDL_GetKeyFromName(part)) == SDLK_UNKNOWN) return false;
    }
    return *key != SDLK_UNKNOWN;
}

bool keymap_bind(const char *name, const char *keys, char *error, size_t size) {
    ensure_keymap();
    enum command command = COMMAND_COUNT;
    for (int i = 0; i < COMMAND_COUNT; i++)
        if (!strcmp(command_name((enum command)i), name)) command = (enum command)i;
    if (command == COMMAND_COUNT) {
        snprintf(error, size, "keybindings: no command %s (F1 lists the commands)", name);
        return false;
    }
    // The first binding of a command replaces its default keys; the context of the defaults stays.
    struct key_binding template = { .command = command };
    bool have_template = false;
    for (size_t i = 0; i < keymap_count;) {
        if (keymap[i].command != command) {
            i++;
            continue;
        }
        if (!have_template) template = keymap[i];
        have_template = true;
        if (rebound[command]) {
            i++;
            continue;
        }
        memmove(&keymap[i], &keymap[i + 1], (keymap_count - i - 1) * sizeof *keymap);
        keymap_count--;
    }
    rebound[command] = true;
    if (!keys[0]) return true; // "" unbinds
    // "Ctrl+K Ctrl+U": a two-key command with Ctrl and a key first.
    char first[64] = "", second[64];
    const char *space = strchr(keys, ' ');
    if (space) snprintf(first, sizeof first, "%.*s", (int)(space - keys), keys);
    snprintf(second, sizeof second, "%s", space ? space + 1 : keys);
    unsigned prefix_modifiers = 0;
    SDL_Keycode prefix = 0;
    struct key_binding binding = template;
    binding.prefix = 0;
    binding.shift_extends = false;
    if ((first[0] && (!parse_key(first, &prefix_modifiers, &prefix) || prefix_modifiers != KEY_CTRL)) ||
        !parse_key(second, &binding.modifiers, &binding.key)) {
        snprintf(error, size, "keybindings: cannot read the key \"%s\" of %s", keys, name);
        return false;
    }
    binding.prefix = prefix;
    keymap = realloc(keymap, (keymap_count + 1) * sizeof *keymap);
    memmove(keymap + 1, keymap, keymap_count++ * sizeof *keymap); // first, so that it wins over other defaults
    keymap[0] = binding;
    return true;
}

const struct key_binding *keymap_lookup(SDL_Keycode key, SDL_Keymod modifiers, enum key_context context,
                                        SDL_Keycode prefix, bool *extend) {
    ensure_keymap();
    unsigned pressed = binding_modifiers(modifiers);
    for (size_t i = 0; i < keymap_count; i++) {
        const struct key_binding *binding = &keymap[i];
        if (binding->context != CONTEXT_ANY && binding->context != context) continue;
        if (binding->prefix != prefix) continue;
        unsigned compared = binding->shift_extends ? pressed & ~KEY_SHIFT : pressed;
        if (binding->key == key && binding->modifiers == compared) {
            *extend = binding->shift_extends && (pressed & KEY_SHIFT);
            return binding;
        }
    }
    return NULL;
}

static const char *key_name(SDL_Keycode key) {
    if (key == SDLK_RETURN) return "Enter";
    if (key == SDLK_KP_ENTER) return "Keypad Enter";
    return SDL_GetKeyName(key);
}

const char *command_description(enum command command) {
    static const char *const descriptions[] = {
#define COMMAND_DESCRIPTION(name, description) description,
        COMMANDS(COMMAND_DESCRIPTION)
#undef COMMAND_DESCRIPTION
    };
    return command < COMMAND_COUNT ? descriptions[command] : "";
}

void command_keys(enum command command, char *text, size_t size) {
    size_t used = 0;
    text[0] = '\0';
    ensure_keymap();
    for (size_t i = 0; i < keymap_count && used + 1 < size; i++) {
        const struct key_binding *binding = &keymap[i];
        if (binding->command != command) continue;
        char chord[32] = "";
        if (binding->prefix) snprintf(chord, sizeof chord, "Ctrl+%s ", key_name(binding->prefix));
        int written = snprintf(text + used, size - used, "%s%s%s%s%s%s%s", used ? ", " : "", chord,
                               binding->modifiers & KEY_CTRL ? "Ctrl+" : "",
                               binding->modifiers & KEY_SHIFT ? "Shift+" : "",
                               binding->modifiers & KEY_ALT ? "Alt+" : "", key_name(binding->key),
                               binding->shift_extends ? " (+Shift)" : "");
        if (written < 0) break;
        used += (size_t)written < size - used ? (size_t)written : size - used - 1;
    }
}
