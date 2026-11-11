// The code socket: an editor sends JavaScript, closes its write side and gets "ok" or the error.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define CODE_SOCKET_FILE_NAME "hydra-native.sock"

// The path of the socket: `setting`, or the file in $XDG_RUNTIME_DIR for "". False when it is too long.
bool code_socket_path(char *path, size_t size, const char *setting);
int code_socket_listen(const char *path); // the listener, or -1 when another hydra-native has the socket
void code_socket_poll(int listener); // evaluates the code of a waiting client
int code_socket_send(const char *path); // --send: stdin to a running hydra-native; the exit status
