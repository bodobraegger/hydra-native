// Checks src/process.h on the system of the build: pipes in both directions, exit status, timed wait, the shell,
// PATH search, and that process_kill ends a whole shell pipeline. The test starts copies of itself as children
// (--copy, --exit, --sleep), so it needs no other program than the shell of the system.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#define SHELL_PROGRAM "cmd"
#else
#include <unistd.h>
#define SHELL_PROGRAM "sh"
#endif

#include "../src/process.h"

#define CHILD_SLEEP_MS 30000
#define EXPECTED_EXIT_STATUS 3
#define KILL_DEADLINE_SECONDS 5
#define RUNNING_CHECK_MS 50

static int failures;

static void check(int condition, const char *what) {
    printf("%s: %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) failures++;
}

static void sleep_ms(int milliseconds) {
#ifdef _WIN32
    Sleep((DWORD)milliseconds);
#else
    struct timespec duration = { milliseconds / 1000, (long)(milliseconds % 1000) * 1000000L };
    nanosleep(&duration, NULL);
#endif
}

static int run_child(const char *mode, const char *argument) {
    if (!strcmp(mode, "--exit")) return atoi(argument);
    if (!strcmp(mode, "--sleep")) {
        sleep_ms(CHILD_SLEEP_MS);
        return 0;
    }
    // --copy: standard input to standard output, until the end of the input
    char buffer[256];
    size_t count;
    while ((count = fread(buffer, 1, sizeof buffer, stdin)) > 0) fwrite(buffer, 1, count, stdout);
    return 0;
}

// Reads all output of a child into `text` (NUL-terminated).
static size_t read_output(int stream, char *text, size_t size) {
    size_t used = 0;
    long count;
    while (used + 1 < size) {
        count = stream_read_ready(stream, text + used, size - 1 - used);
        if (count < 0) break;
        if (count == 0) sleep_ms(1);
        used += (size_t)(count > 0 ? count : 0);
    }
    text[used] = '\0';
    return used;
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] == '-') return run_child(argv[1], argc > 2 ? argv[2] : "0");
    const char *self = argv[0];
    char text[256];

    // A round trip through both pipes of a child.
    int input = -1, output = -1;
    const char *copy[] = { self, "--copy", NULL };
    struct process *process = process_start(copy, &input, &output);
    check(process != NULL, "start a child with two pipes");
    if (process) {
        const char message[] = "frames and lines";
        check(stream_write_all(input, message, sizeof message - 1), "write to the input pipe");
        stream_close(input);
        read_output(output, text, sizeof text);
        stream_close(output);
        printf("  output: \"%s\"\n", text);
        check(!strcmp(text, message), "the child copies its input to its output");
        check(process_wait(process, PROCESS_WAIT_FOREVER) == 0, "exit status 0");
        process_free(process);
    }

    // Exit status and a timed wait.
    char status_text[16];
    snprintf(status_text, sizeof status_text, "%d", EXPECTED_EXIT_STATUS);
    const char *exit_argv[] = { self, "--exit", status_text, NULL };
    process = process_start(exit_argv, NULL, NULL);
    int status = process ? process_wait(process, PROCESS_WAIT_FOREVER) : -2;
    printf("  status: %d\n", status);
    check(status == EXPECTED_EXIT_STATUS, "process_wait gives the exit status");
    process_free(process);

    const char *sleep_argv[] = { self, "--sleep", NULL };
    process = process_start(sleep_argv, NULL, NULL);
    check(process && process_wait(process, RUNNING_CHECK_MS) == PROCESS_RUNNING, "a timed wait returns PROCESS_RUNNING");
    process_free(process); // kills the sleeping child

    // A child that does not read its input: stream_write_ready fills the pipe and then returns 0 instead of waiting.
    input = -1;
    process = process_start(sleep_argv, &input, NULL);
    check(process != NULL, "start a child that does not read");
    if (process) {
        char block[4096] = { 0 };
        long count, written = 0;
        while ((count = stream_write_ready(input, block, sizeof block)) > 0 && written < (64L << 20)) written += count;
        printf("  the pipe took %ld bytes\n", written);
        check(count == 0 && written > 0, "stream_write_ready returns 0 for a full pipe");
        stream_close(input);
        process_free(process);
    }

    // The shell of the system: /bin/sh -c or cmd.exe /d /s /c.
    output = -1;
    process = process_start_shell("echo shell works", NULL, &output);
    check(process != NULL, "start the shell");
    if (process) {
        read_output(output, text, sizeof text);
        stream_close(output);
        printf("  shell output: \"%s\"\n", text);
        check(!strncmp(text, "shell works", strlen("shell works")), "the shell runs the command");
        process_free(process);
    }

    // process_kill ends both commands of a pipeline: the output pipe reaches its end only when no process of the
    // group holds it any more.
    char pipeline[2048];
    snprintf(pipeline, sizeof pipeline, "\"%s\" --sleep | \"%s\" --sleep", self, self);
    output = -1;
    process = process_start_shell(pipeline, NULL, &output);
    check(process != NULL, "start a pipeline");
    if (process) {
        sleep_ms(RUNNING_CHECK_MS * 4);
        process_kill(process);
        time_t deadline = time(NULL) + KILL_DEADLINE_SECONDS;
        long count = 0;
        while ((count = stream_read_ready(output, text, sizeof text)) >= 0 && time(NULL) < deadline) sleep_ms(10);
        check(count < 0, "process_kill ends every process of the pipeline");
        stream_close(output);
        process_free(process);
    }

    check(process_find_program(SHELL_PROGRAM), "process_find_program finds the shell in PATH");
    check(!process_find_program("hydra-native-no-such-program"), "process_find_program misses an unknown name");

    printf("process_test: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
