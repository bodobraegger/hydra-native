// Screen capture for initScreen, initMonitor, initArea and initWindow behind one interface: a backend negotiates
// with the system and writes RGBA frames of width x height to a file descriptor until the capture ends, which
// src/video.c reads like any other video source. The Linux backend (src/capture_linux.c) uses xdg-desktop-portal
// and the ScreenCast API of GNOME Mutter over D-Bus, and PipeWire through GStreamer. Backends for macOS
// (ScreenCaptureKit) and Windows (Windows.Graphics.Capture) can implement capture_run later; see PORTING.md.
#pragma once

#include <stdbool.h>

enum capture_source {
    CAPTURE_PICKER,  // the system dialog picks a window or a monitor (getDisplayMedia, initScreen)
    CAPTURE_MONITOR, // a monitor by index, without a dialog
    CAPTURE_AREA,    // a rectangle of the desktop
    CAPTURE_WINDOW,  // the window that has the focus after a delay
};

struct capture_request {
    enum capture_source source;
    int monitor;
    int x, y, area_width, area_height;
    double delay_seconds;
    int width, height; // of the frames
    bool dry_run; // print the system calls instead of making them (tests)
};

// Captures until the source ends or the process gets SIGINT or SIGTERM; returns 0, or 1 with a message on stderr.
int capture_run(const struct capture_request *request, int output);
