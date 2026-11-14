#pragma once

#include <stdbool.h>

#include <SDL3/SDL.h>

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
    X(FORMAT, "format code: indent by brackets and chains") \
    X(SCREENCAP, "save a screenshot and the code") \
    X(RENDER_TO_FILE, "render to file") \
    X(SAVE_TO_URL, "copy the hydra.ojack.xyz URL of the code") \
    X(GALLERY_SHARE, "share the sketch with the gallery") \
    X(CLEAR_ALL, "clear all: hush, speed 1, empty code; with an open file, reset to the saved file") \
    X(CLEAR_AND_CLOSE, "clear all and close the file, so that Ctrl+S cannot overwrite it") \
    X(SHOW_EXAMPLE, "show a random example") \
    X(MUTATE, "randomize a number (mutator)") \
    X(MUTATE_TRANSFORM, "randomize a function (mutator)") \
    X(MUTATE_UNDO, "undo the last randomization") \
    X(TOGGLE_HELP, "help: keys and commands") \
    X(TOGGLE_INFO, "info panel and language") \
    X(TOGGLE_EXTENSIONS, "extensions and libraries") \
    X(TOGGLE_SETTINGS, "settings: font, theme and autocomplete") \
    X(COPY_PATCH_BAY_ID, "copy the own stream id of the patch bay, for s0.initStream") \
    X(SET_SERVER, "set the gallery and patch bay server") \
    X(SELECT_NEXT_OCCURRENCE, "select the word, then add the next occurrence") \
    X(FIND, "find") \
    X(OPEN_URL, "open the hydra.ojack.xyz URL in the clipboard, like pasting it into the address bar") \
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
    X(WORD_LEFT, "group left (word, punctuation or line end)") \
    X(WORD_RIGHT, "group right (word, punctuation or line end)") \
    X(LINE_START, "line start") \
    X(LINE_END, "line end") \
    X(PAGE_UP, "page up") \
    X(PAGE_DOWN, "page down") \
    X(DOCUMENT_START, "start of the file") \
    X(DOCUMENT_END, "end of the file") \
    X(SELECT_LINE, "select the line, again for the next") \
    X(DELETE_LINE, "delete the lines") \
    X(SORT_LINES, "sort the lines") \
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
    X(GO_TO_LINE, "go to line") \
    X(COMMAND_PALETTE, "command palette") \
    X(TOGGLE_WRAP, "toggle soft wrap") \
    X(TRIGGER_COMPLETION, "show completions") \
    X(COMPLETION_NEXT, "next completion") \
    X(COMPLETION_PREVIOUS, "previous completion") \
    X(COMPLETION_ACCEPT, "insert the completion") \
    X(SIGNATURE_HELP, "show the parameters of the function around the cursor") \
    X(SNIPPET_NEXT, "next argument of the inserted call") \
    X(SNIPPET_PREVIOUS, "previous argument of the inserted call") \
    X(SNIPPET_END, "leave the arguments of the inserted call") \
    X(NUMBER_ACCEPT, "apply the typed number of the settings menu") \
    X(NUMBER_CANCEL, "leave the number field without a change") \
    X(NUMBER_UP, "number field: one step up") \
    X(NUMBER_DOWN, "number field: one step down") \
    X(NUMBER_UP_LARGE, "number field: ten steps up") \
    X(NUMBER_DOWN_LARGE, "number field: ten steps down") \
    X(NUMBER_BACKSPACE, "number field: delete left") \
    X(PROMPT_ACCEPT, "find next, go, or run the command") \
    X(PROMPT_ACCEPT_BACKWARD, "find the previous match") \
    X(PROMPT_UP, "next command up in the palette") \
    X(PROMPT_DOWN, "next command down in the palette") \
    X(PROMPT_BACKSPACE, "delete in the prompt") \
    X(MENU_NEXT, "next entry of the drop-down list") \
    X(MENU_PREVIOUS, "previous entry of the drop-down list") \
    X(MENU_ACCEPT, "run the entry of the drop-down list") \
    X(CHORD, "first key of a two-key command") \
    X(SUBWORD_LEFT, "subword left (camelCase)") \
    X(SUBWORD_RIGHT, "subword right (camelCase)") \
    X(TOGGLE_BOOKMARK, "toggle a bookmark") \
    X(NEXT_BOOKMARK, "next bookmark") \
    X(PREVIOUS_BOOKMARK, "previous bookmark") \
    X(CLEAR_BOOKMARKS, "clear the bookmarks") \
    X(SELECT_BOOKMARKS, "select the bookmarked lines") \
    X(DELETE_LINE_RIGHT, "delete to the line end") \
    X(DELETE_LINE_LEFT, "delete to the line start") \
    X(SHOW_IN_CENTER, "scroll the cursor to the center") \
    X(SKIP_AND_SELECT_NEXT, "skip this occurrence, select the next") \
    X(SORT_LINES_INSENSITIVE, "sort the lines, ignore case") \
    X(SORT_LINES_REVERSE, "sort the lines backwards") \
    X(SORT_LINES_REVERSE_INSENSITIVE, "sort the lines backwards, ignore case") \

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
    CONTEXT_GHOST, // a ghost-text suggestion shows after the cursor
    CONTEXT_PROMPT, // find, go to line or the command palette is open
    CONTEXT_SNIPPET, // an inserted call has tab stops on its arguments
    CONTEXT_NUMBER, // a number of the settings menu is a text field
    CONTEXT_MENU, // the drop-down list of the toolbar button "reset" is open
};

struct key_binding {
    SDL_Keycode key;
    unsigned modifiers;
    enum command command;
    bool global; // also works while the code is hidden
    bool shift_extends; // matches with and without Shift; Shift extends the selection
    enum key_context context;
    SDL_Keycode prefix; // a two-key command: Ctrl+prefix first, like Ctrl+K Ctrl+U
};

// Returns the binding for a key press, or NULL. `extend` reports Shift for motion bindings.
// `prefix` is the first key of a two-key command, or 0.
const struct key_binding *keymap_lookup(SDL_Keycode key, SDL_Keymod modifiers, enum key_context context,
                                        SDL_Keycode prefix, bool *extend);
const char *command_description(enum command command);
const char *command_name(enum command command); // lowercase, as in the "keybindings" of the settings file
// Binds a command to a key such as "Ctrl+Shift+Enter" or "Ctrl+K Ctrl+U" ("" unbinds); the first call for a
// command replaces its default keys. False with a message in `error` for an unknown command or key.
bool keymap_bind(const char *command, const char *keys, char *error, size_t size);
// Writes the keys of a command, such as "Ctrl+Enter, Shift+Enter", or "" when it has none.
void command_keys(enum command command, char *text, size_t size);
