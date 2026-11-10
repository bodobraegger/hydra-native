// Child processes whose output the main loop reads without blocking: downloads with curl (loadScript, languages,
// extensions, thumbnails, fetch and XMLHttpRequest).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "native.h"

#define MAX_FETCHES 64
#define FETCH_TIMEOUT_SECONDS "30"
#define MAX_REQUEST_HEADERS 64

struct fetch {
    SDL_Process *process;
    SDL_IOStream *output; // non-blocking
    char *data;
    size_t length;
    size_t capacity;
};

static struct fetch *fetches[MAX_FETCHES];

// `input` (or nothing, if NULL) becomes the standard input of the process.
static int process_start(char *const argv[], SDL_IOStream *input) {
    int id = 0;
    while (id < MAX_FETCHES && fetches[id]) id++;
    if (id == MAX_FETCHES) return -1;
    SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)argv);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    if (input) {
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_REDIRECT);
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_POINTER, input);
    }
    SDL_Process *process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (!process) return -1;
    struct fetch *fetch = calloc(1, sizeof *fetch);
    fetch->process = process;
    fetch->output = SDL_GetProcessOutput(process);
    fetches[id] = fetch;
    return id;
}

int fetch_start(const char *url) {
    char *argv[] = { "curl", "-fsSL", CURL_PROTOCOL_OPTIONS, "--max-time", FETCH_TIMEOUT_SECONDS, "--", (char *)url,
                     NULL };
    return process_start(argv, NULL);
}

int http_start(const char *url, const char *method, const char *const *headers, size_t header_count, const void *body,
               size_t body_length) {
    // The body goes to the standard input of curl from a memory file, so no file on disk holds it, and the main
    // loop does not wait for curl to read it.
    SDL_IOStream *input = NULL;
    if (body) {
#if defined(__linux__)
        int body_fd = memfd_create("hydra-native-request", MFD_CLOEXEC);
#else // a temporary file, removed at once; the open file stays until the last close
        char name[] = "/tmp/hydra-native-request-XXXXXX";
        int body_fd = mkstemp(name);
        if (body_fd >= 0) unlink(name);
#endif
        if (body_fd < 0) return -1;
        for (size_t written = 0; written < body_length;) {
            ssize_t count = write(body_fd, (const char *)body + written, body_length - written);
            if (count <= 0) {
                close(body_fd);
                return -1;
            }
            written += (size_t)count;
        }
        // On Linux /dev/fd/N opens the file again at offset 0; on macOS it duplicates the descriptor and keeps
        // its offset, so the offset goes back first.
        lseek(body_fd, 0, SEEK_SET);
        char path[32];
        snprintf(path, sizeof path, "/dev/fd/%d", body_fd);
        input = SDL_IOFromFile(path, "rb");
        close(body_fd);
        if (!input) return -1;
    }
    // -D - writes the header blocks (one per redirect) before the body; without -f an error status is a response.
    const char *argv[20 + 2 * MAX_REQUEST_HEADERS] = { "curl", "-sSL", CURL_PROTOCOL_OPTIONS, "-D", "-", "--max-time",
                                                       FETCH_TIMEOUT_SECONDS };
    size_t count = 10;
    if (!strcmp(method, "HEAD")) argv[count++] = "-I";
    else if (strcmp(method, "GET") != 0) {
        argv[count++] = "-X";
        argv[count++] = method;
    }
    for (size_t i = 0; i < header_count && i < MAX_REQUEST_HEADERS; i++) {
        if (headers[i][0] == '@') continue; // curl reads "-H @file" from a file
        argv[count++] = "-H";
        argv[count++] = headers[i];
    }
    if (body) {
        argv[count++] = "--data-binary";
        argv[count++] = "@-";
    }
    argv[count++] = "--";
    argv[count++] = url;
    argv[count] = NULL;
    int id = process_start((char *const *)argv, input);
    if (input) SDL_CloseIO(input);
    return id;
}

void fetch_poll(void (*done)(int id, bool ok, const char *data, size_t length)) {
    for (int id = 0; id < MAX_FETCHES; id++) {
        struct fetch *fetch = fetches[id];
        if (!fetch) continue;
        for (;;) {
            if (fetch->length + 65536 > fetch->capacity) {
                fetch->capacity = fetch->capacity ? fetch->capacity * 2 : 65536 * 2;
                fetch->data = realloc(fetch->data, fetch->capacity);
            }
            size_t count = SDL_ReadIO(fetch->output, fetch->data + fetch->length, fetch->capacity - fetch->length - 1);
            if (count > 0) {
                fetch->length += count;
                continue;
            }
            if (SDL_GetIOStatus(fetch->output) == SDL_IO_STATUS_NOT_READY) break;
            int status = -1;
            SDL_WaitProcess(fetch->process, true, &status);
            SDL_DestroyProcess(fetch->process);
            fetch->data[fetch->length] = '\0';
            done(id, status == 0, fetch->data, fetch->length);
            free(fetch->data);
            free(fetch);
            fetches[id] = NULL;
            break;
        }
    }
}

void fetch_stop_all(void) {
    for (int id = 0; id < MAX_FETCHES; id++) {
        struct fetch *fetch = fetches[id];
        if (!fetch) continue;
        SDL_KillProcess(fetch->process, false);
        SDL_WaitProcess(fetch->process, true, NULL);
        SDL_DestroyProcess(fetch->process);
        free(fetch->data);
        free(fetch);
        fetches[id] = NULL;
    }
}
