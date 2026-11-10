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
    X(SAVE_TO_URL, "copy the hydra.ojack.xyz URL of the code") \
    X(CLEAR_ALL, "clear all: hush, speed 1, empty code") \
    X(SHOW_EXAMPLE, "show a random example") \
    X(MUTATE, "randomize a number (mutator)") \
    X(MUTATE_TRANSFORM, "randomize a function (mutator)") \
    X(MUTATE_UNDO, "undo the last randomization") \
    X(TOGGLE_HELP, "help: keys and commands") \
    X(SELECT_NEXT_OCCURRENCE, "select the word, then add the next occurrence") \
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
    X(SELECT_LINE, "select the line, again for the next") \
    X(DELETE_LINE, "delete the lines") \
    X(JOIN_LINES, "join lines") \
    X(SORT_LINES, "sort the lines") \
    X(TRANSPOSE, "swap the characters around the cursor") \
    X(DELETE_WORD_RIGHT, "delete the word right") \
    X(INDENT_MORE, "indent the lines") \
    X(INDENT_LESS, "outdent the lines") \
    X(GO_TO_BRACKET, "go to the matching bracket") \
    X(SELECT_BETWEEN_BRACKETS, "select inside the brackets, again with them") \
    X(ADD_CURSOR_ABOVE, "add a cursor above") \
    X(ADD_CURSOR_BELOW, "add a cursor below") \
    X(SPLIT_SELECTION, "one cursor per selected line") \
    X(FIND_NEXT, "find the next match") \
    X(FIND_PREVIOUS, "find the previous match") \
    X(REPLACE, "find and replace") \
    X(GO_TO_LINE, "go to line") \
    X(COMMAND_PALETTE, "command palette") \
    X(TOGGLE_WRAP, "toggle soft wrap") \
    X(TRIGGER_COMPLETION, "show completions") \
    X(COMPLETION_NEXT, "next completion") \
    X(COMPLETION_PREVIOUS, "previous completion") \
    X(COMPLETION_ACCEPT, "insert the completion") \
    X(PROMPT_ACCEPT, "find next, replace, go, or run the command") \
    X(PROMPT_ACCEPT_BACKWARD, "find the previous match") \
    X(PROMPT_NEXT_FIELD, "switch between find and replace") \
    X(PROMPT_UP, "next command up in the palette") \
    X(PROMPT_DOWN, "next command down in the palette") \
    X(PROMPT_BACKSPACE, "delete in the prompt") \
    X(REPLACE_ALL, "replace all") \

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

// A binding with a context works only while that popup is open; the others work everywhere.
enum key_context {
    CONTEXT_ANY,
    CONTEXT_COMPLETION, // the autocomplete list is open
    CONTEXT_PROMPT, // find, replace, go to line or the command palette is open
};

struct key_binding {
    SDL_Keycode key;
    unsigned modifiers;
    enum command command;
    bool global; // also works while the code is hidden
    bool shift_extends; // matches with and without Shift; Shift extends the selection
    enum key_context context;
};

// Returns the binding for a key press, or NULL. `extend` reports Shift for motion bindings.
const struct key_binding *keymap_lookup(SDL_Keycode key, SDL_Keymod modifiers, enum key_context context, bool *extend);
const char *command_description(enum command command);
// Writes the keys of a command, such as "Ctrl+Enter, Shift+Enter", or "" when it has none.
void command_keys(enum command command, char *text, size_t size);
