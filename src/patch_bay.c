// The process of tools/patch-bay.mjs (Node.js with @roamhq/wrtc): JSON lines go to its stdin, its stdout lines go to
// js/patch-bay.js, and while a peer receives the output, the frames go into the FIFO <directory>/output.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "native.h"

#define MAX_LINE (1 << 20)
#define STOP_WAIT_STEPS 40 // 2 s for the helper to leave the room
#define STOP_WAIT_STEP_US 50000

static struct {
    pid_t pid;
    int input, output; // the stdin and stdout of the helper
    char directory[512];
    char *buffer;
    size_t used;
} helper = { .pid = -1, .input = -1, .output = -1 };

bool patch_bay_start(const char *script, const char *server, const char *room) {
    if (helper.pid > 0) return true;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    snprintf(helper.directory, sizeof helper.directory, "%s/hydra-native-patch-bay-%d", runtime ? runtime : "/tmp", (int)getpid());
    int to_helper[2], from_helper[2];
    if (pipe2(to_helper, O_CLOEXEC) != 0) return false;
    if (pipe2(from_helper, O_CLOEXEC) != 0) {
        close(to_helper[0]);
        close(to_helper[1]);
        return false;
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(to_helper[0], STDIN_FILENO);
        dup2(from_helper[1], STDOUT_FILENO);
        execlp("node", "node", script, server, room, helper.directory, (char *)NULL);
        _exit(127);
    }
    close(to_helper[0]);
    close(from_helper[1]);
    if (pid < 0) {
        close(to_helper[1]);
        close(from_helper[0]);
        return false;
    }
    fcntl(from_helper[0], F_SETFL, O_NONBLOCK);
    helper.pid = pid;
    helper.input = to_helper[1];
    helper.output = from_helper[0];
    helper.buffer = malloc(MAX_LINE);
    helper.used = 0;
    return true;
}

bool patch_bay_send(const char *line) {
    if (helper.input < 0) return false;
    size_t length = strlen(line);
    for (size_t done = 0; done < length;) {
        ssize_t count = write(helper.input, line + done, length - done);
        if (count <= 0) return false;
        done += (size_t)count;
    }
    return write(helper.input, "\n", 1) == 1;
}

void patch_bay_poll(void (*line)(const char *text)) {
    if (helper.output < 0) return;
    for (;;) {
        ssize_t count = read(helper.output, helper.buffer + helper.used, MAX_LINE - 1 - helper.used);
        if (count <= 0) {
            if (count == 0 || (errno != EAGAIN && errno != EINTR)) {
                fprintf(stderr, "patch bay: the helper ended\n");
                patch_bay_stop();
            }
            return;
        }
        helper.used += (size_t)count;
        char *start = helper.buffer, *end;
        while ((end = memchr(start, '\n', helper.used - (size_t)(start - helper.buffer)))) {
            *end = '\0';
            line(start);
            start = end + 1;
        }
        helper.used -= (size_t)(start - helper.buffer);
        memmove(helper.buffer, start, helper.used);
        if (helper.used == MAX_LINE - 1) helper.used = 0; // a line too long for the buffer is dropped
    }
}

bool patch_bay_publish(bool on, int width, int height) {
    publisher_stop();
    if (!on || helper.pid <= 0) return true;
    char path[600], size[128];
    snprintf(path, sizeof path, "%s/output", helper.directory);
    // The helper keeps the FIFO open for reading, so this open does not wait; then the writer thread blocks.
    int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    fcntl(fd, F_SETFL, 0);
    snprintf(size, sizeof size, "{\"type\":\"size\",\"width\":%d,\"height\":%d}", width, height);
    if (!patch_bay_send(size) || !publisher_start(fd, width, height)) {
        close(fd);
        return false;
    }
    return true;
}

void patch_bay_stop(void) {
    publisher_stop();
    if (helper.pid <= 0) return;
    close(helper.input); // the helper leaves the room and removes its directory at the end of its input
    close(helper.output);
    for (int wait = 0; wait < STOP_WAIT_STEPS && waitpid(helper.pid, NULL, WNOHANG) == 0; wait++) usleep(STOP_WAIT_STEP_US);
    if (waitpid(helper.pid, NULL, WNOHANG) == 0) {
        kill(helper.pid, SIGKILL);
        waitpid(helper.pid, NULL, 0);
    }
    free(helper.buffer);
    helper.pid = -1;
    helper.input = helper.output = -1;
}
