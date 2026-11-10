// All keys of hydra-native in one table. Change a key here and run `make`.
// The order matters: the first matching binding wins.
#include "keymap.h"

static const struct key_binding keymap[] = {
    // overlay, evaluation and files (the keys of the user's Pulsar keymap and of hydra.ojack.xyz)
    { SDLK_h, KEY_CTRL | KEY_SHIFT, COMMAND_TOGGLE_CODE, .global = true },
    { SDLK_RETURN, KEY_CTRL | KEY_ALT, COMMAND_EVAL_ALL, .global = true },
    { SDLK_RETURN, KEY_CTRL | KEY_SHIFT, COMMAND_EVAL_ALL, .global = true },
    { SDLK_RETURN, KEY_CTRL, COMMAND_EVAL_LINE, .global = true },
    { SDLK_RETURN, KEY_SHIFT, COMMAND_EVAL_LINE, .global = true },
    { SDLK_RETURN, KEY_ALT, COMMAND_EVAL_BLOCK, .global = true },
    { SDLK_s, KEY_CTRL, COMMAND_SAVE, .global = true },
    { SDLK_F11, 0, COMMAND_FULLSCREEN, .global = true },
    { SDLK_q, KEY_CTRL, COMMAND_QUIT, .global = true },
    { SDLK_ESCAPE, 0, COMMAND_ESCAPE, .global = true },

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
    { SDLK_f, KEY_ALT | KEY_SHIFT, COMMAND_FORMAT },
    { SDLK_d, KEY_CTRL, COMMAND_SELECT_NEXT_OCCURRENCE },
    { SDLK_f, KEY_CTRL, COMMAND_FIND },
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

const struct key_binding *keymap_lookup(SDL_Keycode key, SDL_Keymod modifiers, bool *extend) {
    unsigned pressed = binding_modifiers(modifiers);
    for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++) {
        const struct key_binding *binding = &keymap[i];
        unsigned compared = binding->shift_extends ? pressed & ~KEY_SHIFT : pressed;
        if (binding->key == key && binding->modifiers == compared) {
            *extend = binding->shift_extends && (pressed & KEY_SHIFT);
            return binding;
        }
    }
    return NULL;
}
