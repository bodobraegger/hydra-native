#pragma once

#include <stdbool.h>
#include <stddef.h>

unsigned thumbnail_texture(const char *url); // starts the download on the first call; 0 until it is ready
bool thumbnails_fetch_done(int fetch, bool ok, const char *data, size_t length); // true when the download was a thumbnail
