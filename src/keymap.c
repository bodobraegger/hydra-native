// All keys of hydra-native in one table. Change a key here and run `make`.
// The order matters: the first matching binding wins.
#include "keymap.h"

#include <stdio.h>

static const struct key_binding keymap[] = {
    // while the autocomplete list is open
    { SDLK_DOWN, 0, COMMAND_COMPLETION_NEXT, .context = CONTEXT_COMPLETION },
    { SDLK_UP, 0, COMMAND_COMPLETION_PREVIOUS, .context = CONTEXT_COMPLETION },
    { SDLK_RETURN, 0, COMMAND_COMPLETION_ACCEPT, .context = CONTEXT_COMPLETION },
    { SDLK_TAB, 0, COMMAND_COMPLETION_ACCEPT, .context = CONTEXT_COMPLETION },

    // while find, replace, go to line or the command palette is open
    { SDLK_RETURN, KEY_CTRL | KEY_ALT, COMMAND_REPLACE_ALL, .context = CONTEXT_PROMPT },
    { SDLK_RETURN, 0, COMMAND_PROMPT_ACCEPT, .context = CONTEXT_PROMPT },
    { SDLK_KP_ENTER, 0, COMMAND_PROMPT_ACCEPT, .context = CONTEXT_PROMPT },
    { SDLK_RETURN, KEY_SHIFT, COMMAND_PROMPT_ACCEPT_BACKWARD, .context = CONTEXT_PROMPT },
    { SDLK_TAB, 0, COMMAND_PROMPT_NEXT_FIELD, .context = CONTEXT_PROMPT },
    { SDLK_UP, 0, COMMAND_PROMPT_UP, .context = CONTEXT_PROMPT },
    { SDLK_DOWN, 0, COMMAND_PROMPT_DOWN, .context = CONTEXT_PROMPT },
    { SDLK_BACKSPACE, 0, COMMAND_PROMPT_BACKSPACE, .context = CONTEXT_PROMPT },

    // overlay, evaluation and files (the keys of the user's Pulsar keymap and of hydra.ojack.xyz)
    { SDLK_h, KEY_CTRL | KEY_SHIFT, COMMAND_TOGGLE_CODE, .global = true },
    { SDLK_RETURN, KEY_CTRL | KEY_ALT, COMMAND_EVAL_ALL, .global = true },
    { SDLK_RETURN, KEY_CTRL | KEY_SHIFT, COMMAND_EVAL_ALL, .global = true },
    { SDLK_RETURN, KEY_CTRL, COMMAND_EVAL_LINE, .global = true },
    { SDLK_RETURN, KEY_SHIFT, COMMAND_EVAL_LINE, .global = true },
    { SDLK_RETURN, KEY_ALT, COMMAND_EVAL_BLOCK, .global = true },
    { SDLK_s, KEY_CTRL, COMMAND_SAVE, .global = true },
    { SDLK_s, KEY_CTRL | KEY_SHIFT, COMMAND_SCREENCAP, .global = true },
    { SDLK_l, KEY_CTRL | KEY_SHIFT, COMMAND_SAVE_TO_URL, .global = true },
    { SDLK_o, KEY_CTRL | KEY_SHIFT, COMMAND_OPEN_URL, .global = true },
    { SDLK_g, KEY_CTRL | KEY_SHIFT, COMMAND_GALLERY_SHARE, .global = true },
    { SDLK_F11, 0, COMMAND_FULLSCREEN, .global = true },
    { SDLK_q, KEY_CTRL, COMMAND_QUIT, .global = true },
    { SDLK_ESCAPE, 0, COMMAND_ESCAPE, .global = true },
    { SDLK_F1, 0, COMMAND_TOGGLE_HELP, .global = true },
    { SDLK_COMMA, KEY_CTRL, COMMAND_TOGGLE_SETTINGS, .global = true }, // the settings key of VS Code

    // toolbar actions of hydra.ojack.xyz (the web editor has no keys for them)
    { SDLK_DELETE, KEY_CTRL | KEY_SHIFT, COMMAND_CLEAR_ALL, .global = true },
    { SDLK_F7, 0, COMMAND_SHOW_EXAMPLE, .global = true },
    { SDLK_F8, 0, COMMAND_MUTATE, .global = true },
    { SDLK_F8, KEY_CTRL, COMMAND_MUTATE_TRANSFORM, .global = true },
    { SDLK_F8, KEY_SHIFT, COMMAND_MUTATE_UNDO, .global = true },

    // editing
    { SDLK_z, KEY_CTRL, COMMAND_UNDO },
    { SDLK_z, KEY_CTRL | KEY_SHIFT, COMMAND_REDO },
    { SDLK_y, KEY_CTRL, COMMAND_REDO },
    { SDLK_c, KEY_CTRL, COMMAND_COPY },
    { SDLK_x, KEY_CTRL, COMMAND_CUT },
    { SDLK_v, KEY_CTRL, COMMAND_PASTE },
    { SDLK_a, KEY_CTRL, COMMAND_SELECT_ALL },
    { SDLK_SLASH, KEY_CTRL, COMMAND_TOGGLE_COMMENT },
    { SDLK_7, KEY_CTRL | KEY_SHIFT, COMMAND_TOGGLE_COMMENT }, // Ctrl+/ on the Swiss (ch) layout
    { SDLK_UP, KEY_ALT, COMMAND_MOVE_LINE_UP },
    { SDLK_DOWN, KEY_ALT, COMMAND_MOVE_LINE_DOWN },
    { SDLK_DOWN, KEY_ALT | KEY_SHIFT, COMMAND_DUPLICATE_LINE },
    { SDLK_d, KEY_CTRL | KEY_SHIFT, COMMAND_DUPLICATE_LINE }, // the sublime keymap of the web editor
    { SDLK_f, KEY_ALT | KEY_SHIFT, COMMAND_FORMAT },
    { SDLK_f, KEY_CTRL | KEY_SHIFT, COMMAND_FORMAT }, // the key of hydra.ojack.xyz
    { SDLK_d, KEY_CTRL, COMMAND_SELECT_NEXT_OCCURRENCE },
    { SDLK_f, KEY_CTRL, COMMAND_FIND },
    { SDLK_F3, 0, COMMAND_FIND_NEXT },
    { SDLK_F3, KEY_SHIFT, COMMAND_FIND_PREVIOUS },
    { SDLK_h, KEY_CTRL, COMMAND_REPLACE },
    { SDLK_g, KEY_CTRL, COMMAND_GO_TO_LINE },
    { SDLK_p, KEY_CTRL | KEY_SHIFT, COMMAND_COMMAND_PALETTE, .global = true },
    { SDLK_z, KEY_ALT, COMMAND_TOGGLE_WRAP },
    { SDLK_SPACE, KEY_CTRL, COMMAND_TRIGGER_COMPLETION },
    { SDLK_l, KEY_CTRL, COMMAND_SELECT_LINE },
    { SDLK_k, KEY_CTRL | KEY_SHIFT, COMMAND_DELETE_LINE },
    { SDLK_j, KEY_CTRL, COMMAND_JOIN_LINES },
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
    { SDLK_SPACE, KEY_CTRL | KEY_SHIFT, COMMAND_SELECT_SCOPE },
    { SDLK_q, KEY_ALT, COMMAND_NONE }, // wrapLines of the sublime keymap needs the hardwrap addon, which the web lacks
    { SDLK_LEFTBRACKET, KEY_CTRL | KEY_SHIFT, COMMAND_NONE }, // fold: the web editor loads no fold addon
    { SDLK_RIGHTBRACKET, KEY_CTRL | KEY_SHIFT, COMMAND_NONE },

    // two-key commands of the sublime keymap: Ctrl+K, then the second key
    { SDLK_k, KEY_CTRL, COMMAND_CHORD },
    { SDLK_u, KEY_CTRL, COMMAND_UPPER_CASE, .prefix = SDLK_k },
    { SDLK_l, KEY_CTRL, COMMAND_LOWER_CASE, .prefix = SDLK_k },
    { SDLK_k, KEY_CTRL, COMMAND_DELETE_LINE_RIGHT, .prefix = SDLK_k },
    { SDLK_BACKSPACE, KEY_CTRL, COMMAND_DELETE_LINE_LEFT, .prefix = SDLK_k },
    { SDLK_c, KEY_CTRL, COMMAND_SHOW_IN_CENTER, .prefix = SDLK_k },
    { SDLK_g, KEY_CTRL, COMMAND_CLEAR_BOOKMARKS, .prefix = SDLK_k },
    { SDLK_SPACE, KEY_CTRL, COMMAND_SET_MARK, .prefix = SDLK_k },
    { SDLK_a, KEY_CTRL, COMMAND_SELECT_TO_MARK, .prefix = SDLK_k },
    { SDLK_w, KEY_CTRL, COMMAND_DELETE_TO_MARK, .prefix = SDLK_k },
    { SDLK_x, KEY_CTRL, COMMAND_SWAP_WITH_MARK, .prefix = SDLK_k },
    { SDLK_y, KEY_CTRL, COMMAND_YANK, .prefix = SDLK_k },
    { SDLK_d, KEY_CTRL, COMMAND_SKIP_AND_SELECT_NEXT, .prefix = SDLK_k },
    { SDLK_0, KEY_CTRL, COMMAND_NONE, .prefix = SDLK_k }, // unfold all, fold all: no fold addon in the web editor
    { SDLK_1, KEY_CTRL, COMMAND_NONE, .prefix = SDLK_k },
    { SDLK_j, KEY_CTRL, COMMAND_NONE, .prefix = SDLK_k },
    { SDLK_t, KEY_CTRL, COMMAND_TRANSPOSE },
    { SDLK_DELETE, KEY_CTRL, COMMAND_DELETE_WORD_RIGHT },
    { SDLK_RIGHTBRACKET, KEY_CTRL, COMMAND_INDENT_MORE },
    { SDLK_LEFTBRACKET, KEY_CTRL, COMMAND_INDENT_LESS },
    { SDLK_m, KEY_CTRL, COMMAND_GO_TO_BRACKET },
    { SDLK_m, KEY_CTRL | KEY_SHIFT, COMMAND_SELECT_BETWEEN_BRACKETS },
    { SDLK_UP, KEY_CTRL | KEY_SHIFT, COMMAND_ADD_CURSOR_ABOVE }, // the user's Pulsar keymap
    { SDLK_DOWN, KEY_CTRL | KEY_SHIFT, COMMAND_ADD_CURSOR_BELOW },
    { SDLK_UP, KEY_CTRL | KEY_ALT, COMMAND_ADD_CURSOR_ABOVE }, // the sublime keymap of the web editor
    { SDLK_DOWN, KEY_CTRL | KEY_ALT, COMMAND_ADD_CURSOR_BELOW },
    { SDLK_i, KEY_ALT | KEY_SHIFT, COMMAND_SPLIT_SELECTION }, // VS Code; Ctrl+Shift+L saves to URL as in the web
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
    return (modifiers & KMOD_CTRL ? KEY_CTRL : 0) | (modifiers & KMOD_SHIFT ? KEY_SHIFT : 0) |
           (modifiers & KMOD_LALT ? KEY_ALT : 0);
}

const struct key_binding *keymap_lookup(SDL_Keycode key, SDL_Keymod modifiers, enum key_context context,
                                        SDL_Keycode prefix, bool *extend) {
    unsigned pressed = binding_modifiers(modifiers);
    for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++) {
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
    for (size_t i = 0; i < sizeof keymap / sizeof keymap[0] && used + 1 < size; i++) {
        const struct key_binding *binding = &keymap[i];
        if (binding->command != command) continue;
        char chord[32] = "";
        if (binding->prefix) snprintf(chord, sizeof chord, "Ctrl+%s ", key_name(binding->prefix));
        int written = snprintf(text + used, size - used, "%s%s%s%s%s%s%s", used ? ", " : "", chord,
                               binding->modifiers & KEY_CTRL ? "Ctrl+" : "", binding->modifiers & KEY_SHIFT ? "Shift+" : "",
                               binding->modifiers & KEY_ALT ? "Alt+" : "", key_name(binding->key),
                               binding->shift_extends ? " (+Shift)" : "");
        if (written < 0) break;
        used += (size_t)written < size - used ? (size_t)written : size - used - 1;
    }
}
