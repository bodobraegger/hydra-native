#pragma once

#include <stdbool.h>

#include <SDL.h>

enum command {
    COMMAND_NONE, // a key that does nothing on purpose
    COMMAND_TOGGLE_CODE,
    COMMAND_EVAL_LINE,
    COMMAND_EVAL_BLOCK,
    COMMAND_EVAL_ALL,
    COMMAND_SAVE,
    COMMAND_UNDO,
    COMMAND_REDO,
    COMMAND_COPY,
    COMMAND_CUT,
    COMMAND_PASTE,
    COMMAND_SELECT_ALL,
    COMMAND_TOGGLE_COMMENT,
    COMMAND_MOVE_LINE_UP,
    COMMAND_MOVE_LINE_DOWN,
    COMMAND_DUPLICATE_LINE,
    COMMAND_FORMAT,
    COMMAND_SCREENCAP,
    COMMAND_SELECT_NEXT_OCCURRENCE,
    COMMAND_FIND,
    COMMAND_FONT_BIGGER,
    COMMAND_FONT_SMALLER,
    COMMAND_FULLSCREEN,
    COMMAND_ESCAPE,
    COMMAND_QUIT,
    COMMAND_NEWLINE,
    COMMAND_BACKSPACE,
    COMMAND_DELETE,
    COMMAND_DELETE_WORD_LEFT,
    COMMAND_INDENT,
    COMMAND_OUTDENT,
    COMMAND_LEFT,
    COMMAND_RIGHT,
    COMMAND_UP,
    COMMAND_DOWN,
    COMMAND_WORD_LEFT,
    COMMAND_WORD_RIGHT,
    COMMAND_LINE_START,
    COMMAND_LINE_END,
    COMMAND_PAGE_UP,
    COMMAND_PAGE_DOWN,
    COMMAND_DOCUMENT_START,
    COMMAND_DOCUMENT_END,
};

// Modifier bits of a binding. Only the left Alt key counts as Alt, so AltGr keeps typing characters.
#define KEY_CTRL 1
#define KEY_SHIFT 2
#define KEY_ALT 4

struct key_binding {
    SDL_Keycode key;
    unsigned modifiers;
    enum command command;
    bool global; // also works while the code is hidden
    bool shift_extends; // matches with and without Shift; Shift extends the selection
};

// Returns the binding for a key press, or NULL. `extend` reports Shift for motion bindings.
const struct key_binding *keymap_lookup(SDL_Keycode key, SDL_Keymod modifiers, bool *extend);
