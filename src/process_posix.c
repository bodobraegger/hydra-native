// The POSIX backend of src/process.h (Linux and macOS): posix_spawn into a new process group, pipes and Unix
// sockets.
#define _GNU_SOURCE
#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SIGNAL_STATUS_BASE 128
#define WAIT_STEP_NS 10000000L // 10 ms between two checks of process_wait with a timeout
#define STOP_GRACE_MS 300

extern char **environ;

struct process {
    pid_t pid;
    int status; // PROCESS_RUNNING until waitpid returned
};

// macOS starts programs from the Finder and the Dock with a PATH of only the system folders, so the folders of
// Homebrew (ffmpeg, node, yt-dlp) are added once.
static void add_package_folders_to_path(void) {
#ifdef __APPLE__
    static bool done;
    if (done) return;
    done = true;
    const char *path = getenv("PATH");
    if (path && strstr(path, "/opt/homebrew/bin")) return;
    char extended[8192];
    snprintf(extended, sizeof extended, "%s%s/opt/homebrew/bin:/usr/local/bin", path ? path : "/usr/bin:/bin",
             path && *path ? ":" : "");
    setenv("PATH", extended, 1);
#endif
}

static bool make_pipe(int fds[2]) {
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
}

static void close_pipe(int fds[2]) {
    if (fds[0] >= 0) close(fds[0]);
    if (fds[1] >= 0) close(fds[1]);
}

static struct process *spawn(const char *file, bool search, char *const argv[], int *input, int *output) {
    add_package_folders_to_path();
    int input_pipe[2] = { -1, -1 }, output_pipe[2] = { -1, -1 };
    if ((input && !make_pipe(input_pipe)) || (output && !make_pipe(output_pipe))) {
        close_pipe(input_pipe);
        close_pipe(output_pipe);
        return NULL;
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    if (input) posix_spawn_file_actions_adddup2(&actions, input_pipe[0], STDIN_FILENO);
    if (output) posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    pid_t pid;
    int error = search ? posix_spawnp(&pid, file, &actions, &attributes, argv, environ)
                       : posix_spawn(&pid, file, &actions, &attributes, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (input_pipe[0] >= 0) close(input_pipe[0]);
    if (output_pipe[1] >= 0) close(output_pipe[1]);
    if (error) {
        fprintf(stderr, "cannot start %s: %s\n", file, strerror(error));
        if (input_pipe[1] >= 0) close(input_pipe[1]);
        if (output_pipe[0] >= 0) close(output_pipe[0]);
        return NULL;
    }
    if (input) *input = input_pipe[1];
    if (output) *output = output_pipe[0];
    struct process *process = malloc(sizeof *process);
    process->pid = pid;
    process->status = PROCESS_RUNNING;
    return process;
}

struct process *process_start(const char *const argv[], int *input, int *output) {
    return spawn(argv[0], true, (char *const *)argv, input, output);
}

struct process *process_start_shell(const char *command, int *input, int *output) {
    const char *argv[] = { "sh", "-c", command, NULL };
    return spawn("/bin/sh", false, (char *const *)argv, input, output);
}

void process_kill(struct process *process) {
    if (process->status == PROCESS_RUNNING) kill(-process->pid, SIGTERM);
}

void process_kill_at_once(struct process *process) {
    if (process->status == PROCESS_RUNNING) kill(-process->pid, SIGKILL);
}

static bool reap(struct process *process, int options) {
    int status;
    pid_t result;
    while ((result = waitpid(process->pid, &status, options)) < 0 && errno == EINTR) {}
    if (result == 0) return false;
    if (result < 0) process->status = SIGNAL_STATUS_BASE; // not our child any more; should not happen
    else process->status = WIFEXITED(status) ? WEXITSTATUS(status) : SIGNAL_STATUS_BASE + WTERMSIG(status);
    return true;
}

int process_wait(struct process *process, int timeout_ms) {
    if (process->status != PROCESS_RUNNING) return process->status;
    if (timeout_ms == PROCESS_WAIT_FOREVER) {
        reap(process, 0);
        return process->status;
    }
    struct timespec step = { 0, WAIT_STEP_NS };
    for (long waited_ns = 0; !reap(process, WNOHANG); waited_ns += WAIT_STEP_NS) {
        if (waited_ns >= (long)timeout_ms * 1000000L) return PROCESS_RUNNING;
        nanosleep(&step, NULL);
    }
    return process->status;
}

int process_stop(struct process *process) {
    if (process_wait(process, 0) != PROCESS_RUNNING) return process->status;
    process_kill(process);
    if (process_wait(process, STOP_GRACE_MS) == PROCESS_RUNNING) {
        kill(-process->pid, SIGKILL);
        process_wait(process, PROCESS_WAIT_FOREVER);
    }
    return process->status;
}

void process_free(struct process *process) {
    if (!process) return;
    process_stop(process);
    free(process);
}

bool process_find_program(const char *name) {
    add_package_folders_to_path();
    if (strchr(name, '/')) return access(name, X_OK) == 0;
    const char *path = getenv("PATH");
    char candidate[4096];
    for (const char *start = path ? path : ""; *start;) {
        const char *end = strchr(start, ':');
        size_t length = end ? (size_t)(end - start) : strlen(start);
        snprintf(candidate, sizeof candidate, "%.*s/%s", (int)length, length ? start : ".", name);
        if (access(candidate, X_OK) == 0) return true;
        if (!end) break;
        start = end + 1;
    }
    return false;
}

// ---------------------------------------------------------------- streams

bool stream_read_all(int stream, void *buffer, size_t size) {
    for (size_t done = 0; done < size;) {
        ssize_t count = read(stream, (char *)buffer + done, size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        done += (size_t)count;
    }
    return true;
}

bool stream_write_all(int stream, const void *buffer, size_t size) {
    for (size_t done = 0; done < size;) {
        ssize_t count = write(stream, (const char *)buffer + done, size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        done += (size_t)count;
    }
    return true;
}

long stream_read_ready(int stream, void *buffer, size_t size) {
    struct pollfd ready = { .fd = stream, .events = POLLIN };
    int count = poll(&ready, 1, 0);
    if (count < 0) return errno == EINTR ? 0 : -1;
    if (count == 0) return 0;
    ssize_t bytes = read(stream, buffer, size);
    if (bytes < 0) return errno == EINTR || errno == EAGAIN ? 0 : -1;
    return bytes == 0 ? -1 : (long)bytes;
}

// A pipe that poll reports as writable takes PIPE_BUF bytes at once, so a write of at most that size does not wait.
long stream_write_ready(int stream, const void *buffer, size_t size) {
    struct pollfd ready = { .fd = stream, .events = POLLOUT };
    int count = poll(&ready, 1, 0);
    if (count < 0) return errno == EINTR ? 0 : -1;
    if (count == 0) return 0;
    if (ready.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    ssize_t bytes = write(stream, buffer, size < PIPE_BUF ? size : PIPE_BUF);
    if (bytes < 0) return errno == EINTR || errno == EAGAIN ? 0 : -1;
    return (long)bytes;
}

void stream_shutdown(int stream) { shutdown(stream, SHUT_RDWR); }

void stream_close(int stream) { close(stream); }

void stream_local_address(char *address, size_t size, const char *name) {
    const char *folder = getenv("XDG_RUNTIME_DIR");
    if (!folder) folder = getenv("TMPDIR");
    if (!folder) folder = "/tmp";
    size_t length = strlen(folder);
    const char *separator = length && folder[length - 1] == '/' ? "" : "/";
    snprintf(address, size, "%s%s%s-%d.sock", folder, separator, name, (int)getpid());
}

int stream_connect(const char *address) {
    struct sockaddr_un socket_address = { .sun_family = AF_UNIX };
    if (strlen(address) >= sizeof socket_address.sun_path) {
        fprintf(stderr, "the socket path %s is too long\n", address);
        return -1;
    }
    strcpy(socket_address.sun_path, address);
    int stream = socket(AF_UNIX, SOCK_STREAM, 0);
    if (stream < 0) return -1;
    fcntl(stream, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE // macOS: a write after the peer closed returns EPIPE instead of a signal
    int on = 1;
    setsockopt(stream, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    if (connect(stream, (struct sockaddr *)&socket_address, sizeof socket_address) != 0) {
        close(stream);
        return -1;
    }
    return stream;
}
