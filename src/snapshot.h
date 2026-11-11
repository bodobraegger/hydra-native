// Pictures of the window: the --screenshot file, screencap() with the code next to it, and the share with the
// gallery, whose picture is the next frame.
#pragma once

#include <stdbool.h>
#include <stddef.h>

bool snapshot_write_png(const char *path, int width, int height); // the window of width x height pixels
// The base name of downloads, like in the web editor: hydra-Y-M-D-H.M.S in the downloads folder of the
// user (XDG user dirs on Linux), or in the current directory.
void snapshot_download_base(char *base, size_t size);
void snapshot_request_screencap(void);
// After the hydra frame and before the code overlay: saves a requested screencap and sends a waiting share.
void snapshot_frame(int width, int height);

void share_set_server(const char *url); // the gallery server; "" for none
const char *share_server(void);
bool share_start(const char *code, const char *name); // false without a server, and while a share runs
void share_done(bool ok, const char *text); // the answer of __shareSketch: the sketch URL or the error
