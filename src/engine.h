// The QuickJS engine that runs the bundle and the sketches, on the main thread.
#pragma once

#include <stddef.h>

#include "quickjs.h"

extern JSContext *js;

void engine_start(int argc, char **argv);
double now_ms(void); // the clock of performance.now() and of the timers
char *read_file(const char *path, size_t *length); // malloc'd with a final NUL, or NULL with errno
// Evaluates code in the global scope with top-level await, like the web editor, but `var` and functions
// stay global as in the browser console. Returns NULL on success, else a malloc'd message with the stack.
char *engine_evaluate(const char *code, size_t length, const char *name);
char *engine_evaluate_file(const char *path);
void engine_check_pending_evaluations(void); // shows the errors of evaluations that waited for a promise
void engine_call(const char *name, int argc, JSValue *argv); // a global function; frees the arguments
char *engine_call_text(const char *function, const char *argument); // the malloc'd text it returns, or NULL
void engine_run_pending_jobs(void);
double engine_run_timers(void); // the time of the next timer in ms of now_ms(), or -1
