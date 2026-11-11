#include "code_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ui.h"

#define CLIENT_TIMEOUT_SECONDS 2
#define MAX_REPLY_SIZE 4096

bool code_socket_path(char *path, size_t size, const char *setting) {
    const char *directory = getenv("XDG_RUNTIME_DIR");
    if (setting[0]) snprintf(path, size, "%s", setting);
    else snprintf(path, size, "%s/%s", directory ? directory : "/tmp", CODE_SOCKET_FILE_NAME);
    if (strlen(path) < sizeof(((struct sockaddr_un *)0)->sun_path)) return true;
    fprintf(stderr, "socket path too long: %s\n", path);
    return false;
}

// SOCK_CLOEXEC, SOCK_NONBLOCK and accept4 are not on macOS, so fcntl sets the flags.
static int with_flags(int fd, bool nonblocking) {
    if (fd < 0) return fd;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (nonblocking) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

static int connect_socket(const char *path) {
    int fd = with_flags(socket(AF_UNIX, SOCK_STREAM, 0), false);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof address.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int code_socket_listen(const char *path) {
    int existing = connect_socket(path);
    if (existing >= 0) {
        close(existing);
        fprintf(stderr, "another hydra-native listens on %s\n", path);
        return -1;
    }
    unlink(path);
    int fd = with_flags(socket(AF_UNIX, SOCK_STREAM, 0), true);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof address.sun_path, "%s", path);
    if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0 || listen(fd, 4) != 0) {
        fprintf(stderr, "cannot listen on %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static char *read_all(int fd, size_t *length) {
    size_t capacity = 65536, used = 0;
    char *buffer = malloc(capacity);
    ssize_t count;
    while ((count = read(fd, buffer + used, capacity - used - 1)) > 0) {
        used += (size_t)count;
        if (used + 1 == capacity) buffer = realloc(buffer, capacity *= 2);
    }
    buffer[used] = '\0';
    *length = used;
    return buffer;
}

// ponytail: one client at a time, read synchronously with a timeout; editors send small blocks and close.
static void serve_client(int listener) {
    int client = with_flags(accept(listener, NULL, NULL), false);
    if (client < 0) return;
    struct timeval timeout = { .tv_sec = CLIENT_TIMEOUT_SECONDS };
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    size_t length;
    char *code = read_all(client, &length);
    char *error = ui_evaluate_socket(code, length);
    char reply[MAX_REPLY_SIZE];
    snprintf(reply, sizeof reply, "%s\n", error ? error : "ok");
    if (error) fprintf(stderr, "%s", reply);
    if (write(client, reply, strlen(reply)) < 0) fprintf(stderr, "socket reply failed: %s\n", strerror(errno));
    free(error);
    free(code);
    close(client);
}

void code_socket_poll(int listener) {
    struct pollfd poll_listener = { .fd = listener, .events = POLLIN };
    if (listener >= 0 && poll(&poll_listener, 1, 0) > 0) serve_client(listener);
}

int code_socket_send(const char *path) {
    int fd = connect_socket(path);
    if (fd < 0) {
        fprintf(stderr, "no hydra-native listens on %s\n", path);
        return 2;
    }
    size_t length;
    char *code = read_all(STDIN_FILENO, &length);
    bool written = write(fd, code, length) == (ssize_t)length;
    free(code);
    shutdown(fd, SHUT_WR);
    char *reply = read_all(fd, &length);
    close(fd);
    fputs(reply, written && strncmp(reply, "ok\n", 3) == 0 ? stdout : stderr);
    int status = written && strncmp(reply, "ok\n", 3) == 0 ? 0 : 1;
    free(reply);
    return status;
}
