// The process of tools/patch-bay.mjs (Node.js with @roamhq/wrtc): JSON lines go to its stdin, its stdout lines go to
// js/patch-bay.js. Frames go through a local socket of the helper (a Unix socket, or a named pipe on Windows): each
// connection starts with one line that names its channel, "output WIDTHxHEIGHT" for the output of hydra-native
// while a peer receives it, or the channel of a received stream, whose frames then come back.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native.h"
#include "process.h"

#define MAX_LINE (1 << 20)
#define STOP_WAIT_MS 2000 // for the helper to leave the room
#define SOCKET_NAME "hydra-native-patch-bay"

static struct {
    struct process *process;
    int input, output; // the stdin and stdout of the helper
    char address[512]; // of its local socket
    char *buffer;
    size_t used;
} helper = { .input = -1, .output = -1 };

bool patch_bay_start(const char *script, const char *server, const char *room) {
    if (helper.process) return true;
    stream_local_address(helper.address, sizeof helper.address, SOCKET_NAME);
    const char *argv[] = { "node", script, server, room, helper.address, NULL };
    helper.process = process_start(argv, &helper.input, &helper.output);
    if (!helper.process) return false;
    helper.buffer = malloc(MAX_LINE);
    helper.used = 0;
    return true;
}

bool patch_bay_send(const char *line) {
    if (helper.input < 0) return false;
    return stream_write_all(helper.input, line, strlen(line)) && stream_write_all(helper.input, "\n", 1);
}

void patch_bay_poll(void (*line)(const char *text)) {
    if (helper.output < 0) return;
    for (;;) {
        long count = stream_read_ready(helper.output, helper.buffer + helper.used, MAX_LINE - 1 - helper.used);
        if (count <= 0) {
            if (count < 0) {
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

int patch_bay_connect(const char *channel) {
    if (!helper.process) return -1;
    int stream = stream_connect(helper.address);
    if (stream < 0) {
        fprintf(stderr, "patch bay: cannot connect to %s\n", helper.address);
        return -1;
    }
    if (!stream_write_all(stream, channel, strlen(channel)) || !stream_write_all(stream, "\n", 1)) {
        stream_close(stream);
        return -1;
    }
    return stream;
}

bool patch_bay_publish(bool on, int width, int height) {
    publisher_stop();
    if (!on || !helper.process) return true;
    char channel[64];
    snprintf(channel, sizeof channel, "output %dx%d", width, height);
    int stream = patch_bay_connect(channel);
    if (stream < 0) return false;
    if (!publisher_start(stream, width, height)) {
        stream_close(stream);
        return false;
    }
    return true;
}

void patch_bay_stop(void) {
    publisher_stop();
    if (!helper.process) return;
    stream_close(helper.input); // the helper leaves the room and removes its socket at the end of its input
    stream_close(helper.output);
    process_wait(helper.process, STOP_WAIT_MS);
    process_free(helper.process); // kills the helper if it still runs
    free(helper.buffer);
    helper.process = NULL;
    helper.input = helper.output = -1;
}
