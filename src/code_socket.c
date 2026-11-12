// A Unix socket on all systems: Windows 10 1803 and newer have them in Winsock (afunix.h). A named pipe has no
// half-close, which tells the end of the code.
#include "code_socket.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <afunix.h>
#define close_socket closesocket
#define SHUT_WR SD_SEND
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#define close_socket close
#endif

#include "ui.h"

#define CLIENT_TIMEOUT_SECONDS 2
#define MAX_REPLY_SIZE 4096

bool code_socket_path(char *path, size_t size, const char *setting) {
#if defined(_WIN32)
    const char *directory = getenv("TEMP");
#else
    const char *directory = getenv("XDG_RUNTIME_DIR");
#endif
    if (setting[0]) snprintf(path, size, "%s", setting);
    else snprintf(path, size, "%s/%s", directory ? directory : "/tmp", CODE_SOCKET_FILE_NAME);
    if (strlen(path) < sizeof(((struct sockaddr_un *)0)->sun_path)) return true;
    fprintf(stderr, "socket path too long: %s\n", path);
    return false;
}

// SOCK_CLOEXEC, SOCK_NONBLOCK and accept4 are not on macOS, so fcntl sets the flags. An accepted socket of macOS
// and Windows inherits the mode of the listener. Child processes of Windows inherit no socket.
static int with_flags(int fd, bool nonblocking) {
    if (fd < 0) return fd;
#if defined(_WIN32)
    u_long mode = nonblocking;
    ioctlsocket((SOCKET)fd, FIONBIO, &mode);
#else
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, nonblocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
#endif
    return fd;
}

// Windows keeps the significant bits of a handle, also of a SOCKET, in its lower 32 bits, so it fits into an int.
static int connect_socket(const char *path) {
#if defined(_WIN32)
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return -1;
#endif
    int fd = with_flags((int)socket(AF_UNIX, SOCK_STREAM, 0), false);
    if (fd < 0) return -1;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof address.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        close_socket(fd);
        return -1;
    }
    return fd;
}

int code_socket_listen(const char *path) {
    int existing = connect_socket(path);
    if (existing >= 0) {
        close_socket(existing);
        fprintf(stderr, "another hydra-native listens on %s\n", path);
        return -1;
    }
    unlink(path);
    int fd = with_flags((int)socket(AF_UNIX, SOCK_STREAM, 0), true);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof address.sun_path, "%s", path);
    if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0 || listen(fd, 4) != 0) {
        fprintf(stderr, "cannot listen on %s: %s\n", path, strerror(errno));
        close_socket(fd);
        return -1;
    }
    return fd;
}

// Reads a socket, or the standard input when `socket` is false.
static char *read_all(int fd, bool socket, size_t *length) {
    size_t capacity = 65536, used = 0;
    char *buffer = malloc(capacity);
    for (;;) {
        size_t free_size = capacity - used - 1;
        long count = socket ? recv(fd, buffer + used, free_size, 0) : read(fd, buffer + used, free_size);
        if (count <= 0) break;
        used += (size_t)count;
        if (used + 1 == capacity) buffer = realloc(buffer, capacity *= 2);
    }
    buffer[used] = '\0';
    *length = used;
    return buffer;
}

// ponytail: one client at a time, read synchronously with a timeout; editors send small blocks and close.
static void serve_client(int listener) {
    int client = with_flags((int)accept(listener, NULL, NULL), false);
    if (client < 0) return;
#if defined(_WIN32)
    DWORD timeout = CLIENT_TIMEOUT_SECONDS * 1000;
#else
    struct timeval timeout = { .tv_sec = CLIENT_TIMEOUT_SECONDS };
#endif
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const void *)&timeout, sizeof timeout);
    size_t length;
    char *code = read_all(client, true, &length);
    char *error = ui_evaluate_socket(code, length);
    char reply[MAX_REPLY_SIZE];
    snprintf(reply, sizeof reply, "%s\n", error ? error : "ok");
    if (error) fprintf(stderr, "%s", reply);
    if (send(client, reply, strlen(reply), 0) < 0) fprintf(stderr, "socket reply failed: %s\n", strerror(errno));
    free(error);
    free(code);
    close_socket(client);
}

// The listener does not block, so accept returns at once when no client waits.
void code_socket_poll(int listener) {
    if (listener >= 0) serve_client(listener);
}

int code_socket_send(const char *path) {
    int fd = connect_socket(path);
    if (fd < 0) {
        fprintf(stderr, "no hydra-native listens on %s\n", path);
        return 2;
    }
    size_t length;
    char *code = read_all(STDIN_FILENO, false, &length);
    bool written = send(fd, code, length, 0) == (long)length;
    free(code);
    shutdown(fd, SHUT_WR);
    char *reply = read_all(fd, true, &length);
    close_socket(fd);
    fputs(reply, written && strncmp(reply, "ok\n", 3) == 0 ? stdout : stderr);
    int status = written && strncmp(reply, "ok\n", 3) == 0 ? 0 : 1;
    free(reply);
    return status;
}
