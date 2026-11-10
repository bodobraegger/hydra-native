// Child processes whose output the main loop reads without blocking: downloads with curl (loadScript, languages,
// extensions, thumbnails) and helpers such as tools/gallery_share.py.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "native.h"

#define MAX_FETCHES 16
#define FETCH_TIMEOUT_SECONDS "30"

struct fetch {
    pid_t pid;
    int fd;
    char *data;
    size_t length;
    size_t capacity;
};

static struct fetch *fetches[MAX_FETCHES];

int process_start(char *const argv[]) {
    int id = 0;
    while (id < MAX_FETCHES && fetches[id]) id++;
    int pipe_fds[2];
    if (id == MAX_FETCHES || pipe2(pipe_fds, O_CLOEXEC) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipe_fds[1]);
    if (pid < 0) {
        close(pipe_fds[0]);
        return -1;
    }
    fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
    struct fetch *fetch = calloc(1, sizeof *fetch);
    fetch->pid = pid;
    fetch->fd = pipe_fds[0];
    fetches[id] = fetch;
    return id;
}

int fetch_start(const char *url) {
    char *argv[] = { "curl", "-fsSL", "--max-time", FETCH_TIMEOUT_SECONDS, "--", (char *)url, NULL };
    return process_start(argv);
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
            ssize_t count = read(fetch->fd, fetch->data + fetch->length, fetch->capacity - fetch->length - 1);
            if (count > 0) {
                fetch->length += (size_t)count;
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EINTR)) break;
            int status = 0;
            waitpid(fetch->pid, &status, 0);
            close(fetch->fd);
            fetch->data[fetch->length] = '\0';
            done(id, WIFEXITED(status) && WEXITSTATUS(status) == 0, fetch->data, fetch->length);
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
        kill(fetch->pid, SIGTERM);
        waitpid(fetch->pid, NULL, 0);
        close(fetch->fd);
        free(fetch->data);
        free(fetch);
        fetches[id] = NULL;
    }
}
