#include "signature.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MAX_DEPTH 256

static bool is_name_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '$'; }

// The end of a string or comment that starts at `i`, or `i` when none starts there. `open` tells that it is not
// closed before `length`.
static size_t skip_literal(const char *text, size_t length, size_t i, bool *open) {
    char c = text[i], next = i + 1 < length ? text[i + 1] : '\0';
    *open = false;
    if (c == '/' && next == '/') {
        while (i < length && text[i] != '\n') i++;
        *open = i == length;
        return i;
    }
    if (c == '/' && next == '*') {
        for (i += 2; i + 1 < length; i++)
            if (text[i] == '*' && text[i + 1] == '/') return i + 2;
        *open = true;
        return length;
    }
    if (c != '"' && c != '\'' && c != '`') return i;
    for (i++; i < length; i++) {
        if (text[i] == '\\') i++;
        else if (text[i] == c) return i + 1;
        else if (text[i] == '\n' && c != '`') return i;
    }
    *open = true;
    return length;
}

size_t signatures_parse(const char *text, struct signature *list, size_t capacity) {
    size_t count = 0;
    for (const char *line = text; line && *line && count < capacity;) {
        const char *end = strchr(line, '\n');
        size_t line_length = end ? (size_t)(end - line) : strlen(line);
        struct signature *signature = &list[count];
        memset(signature, 0, sizeof *signature);
        size_t field = 0;
        for (const char *part = line; part < line + line_length; field++) {
            const char *tab = memchr(part, '\t', (size_t)(line + line_length - part));
            int part_length = (int)((tab ? tab : line + line_length) - part);
            if (field == 0) snprintf(signature->name, sizeof signature->name, "%.*s", part_length, part);
            else if (field == 1) snprintf(signature->kind, sizeof signature->kind, "%.*s", part_length, part);
            else if (signature->input_count < MAX_SIGNATURE_INPUTS) {
                struct signature_input *input = &signature->inputs[signature->input_count++];
                const char *colon = memchr(part, ':', (size_t)part_length);
                const char *second = colon ? memchr(colon + 1, ':', (size_t)(part + part_length - colon - 1)) : NULL;
                if (!second) {
                    signature->input_count--;
                } else {
                    snprintf(input->name, sizeof input->name, "%.*s", (int)(colon - part), part);
                    snprintf(input->type, sizeof input->type, "%.*s", (int)(second - colon - 1), colon + 1);
                    snprintf(input->default_value, sizeof input->default_value, "%.*s",
                             (int)(part + part_length - second - 1), second + 1);
                }
            }
            part = tab ? tab + 1 : line + line_length;
        }
        if (signature->name[0]) count++;
        line = end ? end + 1 : NULL;
    }
    return count;
}

const struct signature *signature_find(const struct signature *list, size_t count, const char *name, size_t length) {
    for (size_t i = 0; i < count; i++)
        if (strlen(list[i].name) == length && !strncmp(list[i].name, name, length)) return &list[i];
    return NULL;
}

bool signature_call_at(const char *text, size_t offset, const struct signature *list, size_t count,
                       struct call_context *call, const struct signature **found) {
    struct frame {
        char bracket;
        size_t open, commas;
    } frames[MAX_DEPTH];
    size_t depth = 0;
    bool in_literal = false;
    for (size_t i = 0; i < offset;) {
        size_t after = skip_literal(text, offset, i, &in_literal);
        if (after != i) {
            i = after;
            continue;
        }
        char c = text[i];
        if ((c == '(' || c == '[' || c == '{') && depth < MAX_DEPTH) frames[depth++] = (struct frame){ c, i, 0 };
        else if ((c == ')' || c == ']' || c == '}') && depth > 0) depth--;
        else if (c == ',' && depth > 0) frames[depth - 1].commas++;
        i++;
    }
    for (size_t level = depth; level-- > 0;) {
        if (frames[level].bracket != '(') continue;
        size_t end = frames[level].open;
        while (end > 0 && isspace((unsigned char)text[end - 1])) end--;
        size_t start = end;
        while (start > 0 && is_name_char(text[start - 1])) start--;
        const struct signature *signature = signature_find(list, count, text + start, end - start);
        if (!signature) continue;
        size_t before = start;
        while (before > 0 && isspace((unsigned char)text[before - 1])) before--;
        *call = (struct call_context){
            .open = frames[level].open,
            .name_start = start,
            .name_length = end - start,
            .argument = frames[level].commas,
            .chained = before > 0 && text[before - 1] == '.',
            .direct = level == depth - 1 && !in_literal,
        };
        *found = signature;
        return true;
    }
    return false;
}

size_t signature_arguments(const char *text, size_t length, size_t open, size_t *starts, size_t *ends, size_t capacity) {
    size_t count = 0, depth = 0, start = open + 1;
    for (size_t i = open + 1; i < length;) {
        bool open_literal;
        size_t after = skip_literal(text, length, i, &open_literal);
        if (after != i) {
            i = after;
            continue;
        }
        char c = text[i];
        bool last = c == ')' && depth == 0;
        if ((c == ',' && depth == 0) || last) {
            size_t first = start, end = i;
            while (first < end && isspace((unsigned char)text[first])) first++;
            while (end > first && isspace((unsigned char)text[end - 1])) end--;
            if (count < capacity && (!last || count > 0 || end > first)) {
                starts[count] = first;
                ends[count++] = end;
            }
            if (last) break;
            start = i + 1;
        } else if (c == '(' || c == '[' || c == '{') {
            depth++;
        } else if ((c == ')' || c == ']' || c == '}') && depth > 0) {
            depth--;
        }
        i++;
    }
    return count;
}

bool signature_input_is_texture(const struct signature_input *input) {
    return !strcmp(input->type, "vec4") || !strcmp(input->type, "sampler2D");
}

bool signature_is_chained(const struct signature *signature) {
    static const char *const kinds[] = { "color", "coord", "combine", "combineCoord" };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
        if (!strcmp(signature->kind, kinds[i])) return true;
    return false;
}

bool signature_is_source(const struct signature *signature) { return !strcmp(signature->kind, "src"); }

void signature_label(const struct signature *signature, size_t active, char *label, size_t size, size_t *active_start,
                     size_t *active_end) {
    size_t used = (size_t)snprintf(label, size, "%s(", signature->name);
    *active_start = *active_end = 0;
    for (size_t i = 0; i < signature->input_count && used < size; i++) {
        const struct signature_input *input = &signature->inputs[i];
        if (i > 0) used += (size_t)snprintf(label + used, size - used, ", ");
        if (used >= size) break;
        if (i == active) *active_start = used;
        if (input->default_value[0]) used += (size_t)snprintf(label + used, size - used, "%s = %s", input->name, input->default_value);
        else used += (size_t)snprintf(label + used, size - used, "%s", input->name);
        if (i == active) *active_end = used < size ? used : size - 1;
    }
    if (used < size) snprintf(label + used, size - used, ")");
}

void signature_snippet(const struct signature *signature, char *snippet, size_t size) {
    size_t used = (size_t)snprintf(snippet, size, "%s(", signature->name);
    for (size_t i = 0; i < signature->input_count && used < size; i++) {
        const struct signature_input *input = &signature->inputs[i];
        const char *value = signature_input_is_texture(input) ? "o0" : input->default_value[0] ? input->default_value : "0";
        used += (size_t)snprintf(snippet + used, size - used, "%s${%s}", i > 0 ? ", " : "", value);
    }
    if (used < size) snprintf(snippet + used, size - used, ")");
}
