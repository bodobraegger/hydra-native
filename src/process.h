// Child processes and byte streams on POSIX (Linux, macOS: src/process_posix.c) and Windows
// (src/process_windows.c). Each child starts in a group of its own, a process group on POSIX and a job object on
// Windows, so that process_kill stops the child and all its descendants, for example every command of a shell
// pipeline. Streams are file descriptors on all systems (C runtime descriptors on Windows): pipes to children and
// local sockets, which the stream functions read and write.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define PROCESS_RUNNING (-1)
#define PROCESS_WAIT_FOREVER (-1)

struct process;

// Starts argv[0], searched in PATH, with the arguments argv (NULL-terminated). When `input` (`output`) is not
// NULL, the standard input (output) of the child is a new pipe and *input (*output) gets the end of this process;
// otherwise the child inherits the stream of this process. The child also inherits the standard error. Returns
// NULL, with a message on stderr, when the program cannot start.
struct process *process_start(const char *const argv[], int *input, int *output);
// Runs `command` with /bin/sh -c on POSIX and with cmd.exe /d /s /c on Windows, so the shell syntax and the
// quoting differ between the systems.
struct process *process_start_shell(const char *command, int *input, int *output);
// Asks the whole group to end: SIGTERM on POSIX, TerminateJobObject on Windows.
void process_kill(struct process *process);
// Waits up to `timeout_ms` milliseconds (PROCESS_WAIT_FOREVER: no limit) for the end of the child. Returns its
// exit status, 128 + the signal number after a signal on POSIX, or PROCESS_RUNNING.
int process_wait(struct process *process, int timeout_ms);
// Ends the group if the child still runs and waits for the child: SIGTERM, then SIGKILL after a short grace time on
// POSIX, so that a child that ignores SIGTERM cannot block the caller; TerminateJobObject on Windows. Returns the
// exit status.
int process_stop(struct process *process);
// process_stop, then frees `process`.
void process_free(struct process *process);
// Whether `name` is a program in PATH (on Windows also with the extension .exe).
bool process_find_program(const char *name);

// Read or write all `size` bytes; false at the end of the stream or on an error.
bool stream_read_all(int stream, void *buffer, size_t size);
bool stream_write_all(int stream, const void *buffer, size_t size);
// Reads the bytes that are there without waiting: their count, 0 when there are none, or -1 at the end of the
// stream or on an error.
long stream_read_ready(int stream, void *buffer, size_t size);
// Makes a blocking read of another thread on `stream` return: shutdown on POSIX sockets, CancelIoEx on Windows.
void stream_shutdown(int stream);
void stream_close(int stream);
// The address of a local socket of this process: a Unix socket path (in XDG_RUNTIME_DIR, TMPDIR or /tmp) on
// POSIX, a named pipe (\\.\pipe\...) on Windows. Node.js listens on both kinds with net.Server.listen(address).
void stream_local_address(char *address, size_t size, const char *name);
// Connects to the local socket at `address`; the stream, or -1.
int stream_connect(const char *address);
