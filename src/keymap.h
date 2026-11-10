#pragma once

#include <stdbool.h>

#include <SDL.h>

// All commands with the name that the help shows. COMMAND_NONE is a key that does nothing on purpose.
#define COMMANDS(X) \
    X(NONE, "nothing") \
    X(TOGGLE_CODE, "hide or show the code") \
    X(EVAL_LINE, "evaluate the line or the selection") \
    X(EVAL_BLOCK, "evaluate the block between blank lines") \
    X(EVAL_ALL, "evaluate all") \
    X(SAVE, "save the file") \
    X(UNDO, "undo") \
    X(REDO, "redo") \
    X(COPY, "copy") \
    X(CUT, "cut") \
    X(PASTE, "paste") \
    X(SELECT_ALL, "select all") \
    X(TOGGLE_COMMENT, "toggle comment") \
    X(MOVE_LINE_UP, "move lines up") \
    X(MOVE_LINE_DOWN, "move lines down") \
    X(DUPLICATE_LINE, "duplicate lines") \
    X(FORMAT, "format code (js-beautify)") \
    X(SCREENCAP, "save a screenshot and the code") \
    X(CLEAR_ALL, "clear all: hush, speed 1, empty code") \
    X(SHOW_EXAMPLE, "show a random example") \
    X(MUTATE, "randomize a number (mutator)") \
    X(MUTATE_TRANSFORM, "randomize a function (mutator)") \
    X(MUTATE_UNDO, "undo the last randomization") \
    X(TOGGLE_HELP, "help: keys and commands") \
    X(SELECT_NEXT_OCCURRENCE, "select the next occurrence") \
    X(FIND, "find") \
    X(FONT_BIGGER, "bigger font") \
    X(FONT_SMALLER, "smaller font") \
    X(FULLSCREEN, "fullscreen") \
    X(ESCAPE, "close, clear the selection, leave fullscreen") \
    X(QUIT, "quit") \
    X(NEWLINE, "new line") \
    X(BACKSPACE, "delete left") \
    X(DELETE, "delete right") \
    X(DELETE_WORD_LEFT, "delete the word left") \
    X(INDENT, "indent") \
    X(OUTDENT, "outdent") \
    X(LEFT, "left") \
    X(RIGHT, "right") \
    X(UP, "up") \
    X(DOWN, "down") \
    X(WORD_LEFT, "word left") \
    X(WORD_RIGHT, "word right") \
    X(LINE_START, "line start") \
    X(LINE_END, "line end") \
    X(PAGE_UP, "page up") \
    X(PAGE_DOWN, "page down") \
    X(DOCUMENT_START, "start of the file") \
    X(DOCUMENT_END, "end of the file") \

enum command {
#define COMMAND_ENUM(name, description) COMMAND_##name,
    COMMANDS(COMMAND_ENUM)
#undef COMMAND_ENUM
    COMMAND_COUNT
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
const char *command_description(enum command command);
// Writes the keys of a command, such as "Ctrl+Enter, Shift+Enter", or "" when it has none.
void command_keys(enum command command, char *text, size_t size);
