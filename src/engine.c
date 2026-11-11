#define _GNU_SOURCE
#include "engine.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "quickjs-libc.h"
#include "ui.h"

static JSRuntime *runtime;
JSContext *js;

void engine_start(int argc, char **argv) {
    runtime = JS_NewRuntime();
    js = JS_NewContext(runtime);
    js_std_add_helpers(js, argc, argv);
}

double now_ms(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec * 1000.0 + time.tv_nsec / 1e6;
}

char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1);
    size_t got = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[got] = '\0';
    if (length) *length = got;
    return text;
}

void engine_run_pending_jobs(void) {
    JSContext *job_context;
    int status;
    while ((status = JS_ExecutePendingJob(runtime, &job_context)) != 0)
        if (status < 0) js_std_dump_error(job_context);
}

static char *error_message(JSValue error) {
    const char *text = JS_ToCString(js, error);
    const char *stack = NULL;
    JSValue stack_value = JS_UNDEFINED;
    if (JS_IsError(js, error)) {
        stack_value = JS_GetPropertyStr(js, error, "stack");
        if (!JS_IsUndefined(stack_value)) stack = JS_ToCString(js, stack_value);
    }
    char *message;
    if (asprintf(&message, "%s\n%s", text ? text : "error", stack ? stack : "") < 0) message = strdup("error");
    JS_FreeCString(js, text);
    if (stack) JS_FreeCString(js, stack);
    JS_FreeValue(js, stack_value);
    return message;
}

// Evaluations that wait for a promise (top-level await); their errors arrive later.
#define MAX_PENDING_EVALUATIONS 32
static JSValue pending_evaluations[MAX_PENDING_EVALUATIONS];
static int pending_evaluation_count;

char *engine_evaluate(const char *code, size_t length, const char *name) {
    JSValue result = JS_Eval(js, code, length, name, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_ASYNC);
    char *message = NULL;
    if (JS_IsException(result)) {
        JSValue exception = JS_GetException(js);
        message = error_message(exception);
        JS_FreeValue(js, exception);
        JS_FreeValue(js, result);
        engine_run_pending_jobs();
        return message;
    }
    engine_run_pending_jobs();
    switch (JS_PromiseState(js, result)) {
    case JS_PROMISE_REJECTED: {
        JSValue reason = JS_PromiseResult(js, result);
        message = error_message(reason);
        JS_FreeValue(js, reason);
        break;
    }
    case JS_PROMISE_PENDING:
        if (pending_evaluation_count < MAX_PENDING_EVALUATIONS) {
            pending_evaluations[pending_evaluation_count++] = result;
            return NULL;
        }
        break;
    default: break;
    }
    JS_FreeValue(js, result);
    return message;
}

void engine_check_pending_evaluations(void) {
    for (int i = 0; i < pending_evaluation_count;) {
        JSValue promise = pending_evaluations[i];
        JSPromiseStateEnum state = JS_PromiseState(js, promise);
        if (state == JS_PROMISE_PENDING) {
            i++;
            continue;
        }
        if (state == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(js, promise);
            char *message = error_message(reason);
            fprintf(stderr, "%s\n", message);
            ui_show_message(message, true);
            free(message);
            JS_FreeValue(js, reason);
        }
        JS_FreeValue(js, promise);
        pending_evaluations[i] = pending_evaluations[--pending_evaluation_count];
    }
}

char *engine_evaluate_file(const char *path) {
    size_t length;
    char *code = read_file(path, &length);
    if (!code) {
        char *message;
        if (asprintf(&message, "cannot read %s: %s", path, strerror(errno)) < 0) return strdup("cannot read file");
        return message;
    }
    char *message = engine_evaluate(code, length, path);
    free(code);
    return message;
}

void engine_call(const char *name, int argc, JSValue *argv) {
    JSValue global = JS_GetGlobalObject(js);
    JSValue function = JS_GetPropertyStr(js, global, name);
    if (JS_IsFunction(js, function)) {
        JSValue result = JS_Call(js, function, global, argc, argv);
        if (JS_IsException(result)) js_std_dump_error(js);
        JS_FreeValue(js, result);
    }
    for (int i = 0; i < argc; i++) JS_FreeValue(js, argv[i]);
    JS_FreeValue(js, function);
    JS_FreeValue(js, global);
}

char *engine_call_text(const char *function, const char *argument) {
    JSValue global = JS_GetGlobalObject(js);
    JSValue callee = JS_GetPropertyStr(js, global, function);
    JSValue value = JS_NewString(js, argument);
    JSValue result = JS_Call(js, callee, global, 1, &value);
    char *text = NULL;
    if (JS_IsException(result)) {
        js_std_dump_error(js);
    } else {
        const char *string = JS_ToCString(js, result);
        if (string) text = strdup(string);
        JS_FreeCString(js, string);
    }
    JS_FreeValue(js, result);
    JS_FreeValue(js, value);
    JS_FreeValue(js, callee);
    JS_FreeValue(js, global);
    engine_run_pending_jobs();
    return text;
}

double engine_run_timers(void) {
    JSValue global = JS_GetGlobalObject(js);
    JSValue function = JS_GetPropertyStr(js, global, "__runTimers");
    JSValue now = JS_NewFloat64(js, now_ms());
    JSValue result = JS_Call(js, function, global, 1, &now);
    double next = -1;
    if (JS_IsException(result)) js_std_dump_error(js);
    else JS_ToFloat64(js, &next, result);
    JS_FreeValue(js, result);
    JS_FreeValue(js, function);
    JS_FreeValue(js, global);
    engine_run_pending_jobs();
    return next;
}
