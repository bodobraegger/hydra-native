// hydra-native-capture: the screen capture of src/capture.h as a video source command for src/video.c (js/sources.js
// starts it). It writes RGBA frames to stdout.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capture.h"

static const char *const usage =
    "usage: hydra-native-capture picker WIDTH HEIGHT [--dry-run]   (the dialog of the system, initScreen)\n"
    "       hydra-native-capture monitor INDEX WIDTH HEIGHT\n"
    "       hydra-native-capture area X Y AREA_WIDTH AREA_HEIGHT WIDTH HEIGHT\n"
    "       hydra-native-capture window DELAY_SECONDS WIDTH HEIGHT   (the window that has the focus after the delay)\n";

int main(int argc, char **argv) {
    struct capture_request request = { 0 };
    int count = argc;
    if (count > 1 && !strcmp(argv[count - 1], "--dry-run")) {
        request.dry_run = true;
        count--;
    }
    const char *mode = count > 1 ? argv[1] : "";
    int needed = !strcmp(mode, "picker") ? 4 : !strcmp(mode, "monitor") || !strcmp(mode, "window") ? 5 : !strcmp(mode, "area") ? 8 : 0;
    if (!needed || count != needed) {
        fputs(usage, stderr);
        return 2;
    }
    request.width = atoi(argv[count - 2]);
    request.height = atoi(argv[count - 1]);
    if (!strcmp(mode, "picker")) {
        request.source = CAPTURE_PICKER;
    } else if (!strcmp(mode, "monitor")) {
        request.source = CAPTURE_MONITOR;
        request.monitor = atoi(argv[2]);
    } else if (!strcmp(mode, "window")) {
        request.source = CAPTURE_WINDOW;
        request.delay_seconds = atof(argv[2]);
    } else {
        request.source = CAPTURE_AREA;
        request.x = atoi(argv[2]);
        request.y = atoi(argv[3]);
        request.area_width = atoi(argv[4]);
        request.area_height = atoi(argv[5]);
    }
    if (request.width <= 0 || request.height <= 0) {
        fputs(usage, stderr);
        return 2;
    }
    return capture_run(&request, fileno(stdout));
}
