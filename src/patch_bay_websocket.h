// The WebSocket of the signalling of hydra-native-patch-bay (src/patch_bay_main.c), with the C++ API of
// libdatachannel (src/patch_bay_websocket.cpp): its C API cannot give mbedTLS the CA certificates for the check of
// a wss:// server. There is one WebSocket at a time.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum websocket_event { WEBSOCKET_OPEN, WEBSOCKET_MESSAGE, WEBSOCKET_CLOSED, WEBSOCKET_ERROR };

// Called from a thread of libdatachannel: `text` is the message or the error, NULL for the other events.
typedef void (*websocket_callback)(enum websocket_event event, const char *text, size_t size);

// Opens `url` (ws:// or wss://); `certificates` is a PEM file of the trusted CA certificates, needed for wss://.
// False when the URL is invalid.
bool websocket_open(const char *url, const char *certificates, websocket_callback callback);
// False when the WebSocket is not open or the send failed.
bool websocket_send(const char *text);
void websocket_close(void);

#ifdef __cplusplus
}
#endif
