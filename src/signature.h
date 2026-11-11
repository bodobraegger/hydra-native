#pragma once

// The definitions of the hydra functions (from hydra-synth at run time, through __functionSignatures in
// js/main.js) and the call around the cursor, for the signature help and the typed completions of src/ui.c.
#include <stdbool.h>
#include <stddef.h>

#define MAX_SIGNATURE_INPUTS 16
#define SIGNATURE_NAME 64

struct signature_input {
    char name[SIGNATURE_NAME];
    char type[SIGNATURE_NAME]; // float, vec4 (a texture) or sampler2D
    char default_value[SIGNATURE_NAME]; // "" when the definition has none
};

struct signature {
    char name[SIGNATURE_NAME];
    char kind[SIGNATURE_NAME]; // src, coord, color, combine or combineCoord; else a kind of js/api-signatures.js
    struct signature_input inputs[MAX_SIGNATURE_INPUTS];
    size_t input_count;
};

// The innermost unclosed '(' before `offset` with a name before it, outside strings and comments.
struct call_context {
    size_t name_start;
    size_t argument; // the index of the argument that holds `offset`
    bool direct; // `offset` is in the argument itself, not in a bracket inside it
};

// Parses lines "name<TAB>kind<TAB>input:type:default..."; returns the count.
size_t signatures_parse(const char *text, struct signature *list, size_t capacity);
const struct signature *signature_find(const struct signature *list, size_t count, const char *name, size_t length);
// The innermost call around `offset` of a function in `list`.
bool signature_call_at(const char *text, size_t offset, const struct signature *list, size_t count,
                       struct call_context *call, const struct signature **found);
bool signature_input_is_texture(const struct signature_input *input);
bool signature_is_chained(const struct signature *signature); // a glsl transform of a chain
bool signature_is_source(const struct signature *signature); // a glsl source function such as osc
// "modulate(texture, amount = 0.1)", with the range of input `active` in [*active_start, *active_end).
void signature_label(const struct signature *signature, size_t active, char *label, size_t size, size_t *active_start,
                     size_t *active_end);
// "modulate(${o0}, ${0.1})": the call with the defaults as tab stops; o0 for a texture input.
void signature_snippet(const struct signature *signature, char *snippet, size_t size);
