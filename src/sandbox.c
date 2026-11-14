#include "sandbox.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_DIRECTORY "/.cache/hydra-native/"

static bool sandboxed;

void sandbox_enter(void) {
    if (!sandboxed) fprintf(stderr, "sandbox: shell commands and local files are off from now on\n");
    sandboxed = true;
}

bool sandbox_active(void) { return sandboxed; }

static bool in_directory(const char *path, const char *directory) {
    size_t length = strlen(directory);
    return directory[0] && !strncmp(path, directory, length) && (directory[length - 1] == '/' || path[length] == '/');
}

bool sandbox_allows_path(const char *path) {
    if (!sandboxed) return true;
    if (strstr(path, "..")) return false;
    char cache[PATH_MAX];
    const char *home = getenv("HOME");
    snprintf(cache, sizeof cache, "%s" CACHE_DIRECTORY, home ? home : "/tmp");
    return in_directory(path, cache);
}

// `text` as one word of the shell of process_start_shell (malloc'd): in single quotes for /bin/sh, in double quotes
// for cmd.exe, where a word cannot hold a double quote. NULL for such a word and for control characters.
static char *shell_word(const char *text) {
    size_t length = 3;
    for (const char *c = text; *c; c++) {
        if ((unsigned char)*c < 0x20 || *c == 0x7f) return NULL;
        length += *c == '\'' ? 4 : 1;
    }
#if defined(_WIN32)
    if (strchr(text, '"')) return NULL;
    char *word = malloc(length);
    snprintf(word, length, "\"%s\"", text);
#else
    char *word = malloc(length), *end = word;
    *end++ = '\'';
    for (const char *c = text; *c; c++) {
        if (*c == '\'') {
            memcpy(end, "'\\''", 4);
            end += 4;
        } else *end++ = *c;
    }
    *end++ = '\'';
    *end = '\0';
#endif
    return word;
}

const char *sandbox_media_input(const char *url) {
    bool remote = !strncmp(url, "https://", 8) || !strncmp(url, "http://", 7);
    if (sandboxed && !remote) return NULL;
    return !remote && !strncmp(url, "file://", 7) ? url + 7 : url;
}

enum command_status sandbox_media_command(char *command, size_t size, const char *url, const char *kind, int width,
                                          int height, const struct media_playback *playback) {
    const char *input = sandbox_media_input(url);
    if (!input) return COMMAND_SANDBOXED;
    char *word = shell_word(input);
    if (!word) return COMMAND_INVALID;
    char format[64];
    snprintf(format, sizeof format, "best[height<=%d]/best", height);
    char *format_word = shell_word(format);
    // A video decodes at full speed and the realtime filter paces its frames, so that the frames from the keyframe
    // before the position of -ss to the position come at once.
    char pace[48] = "";
    if (!strcmp(kind, "video")) snprintf(pace, sizeof pace, "realtime=speed=%.3f,", playback->rate);
    char output[144];
    snprintf(output, sizeof output, "-vf %sscale=%d:%d -pix_fmt rgba -f rawvideo -", pace, width, height);
    const char *protocols = sandboxed ? "-protocol_whitelist " SANDBOX_PROTOCOLS " " : "";
    if (!strcmp(kind, "youtube"))
        snprintf(command, size, "yt-dlp -q -f %s -o - %s | ffmpeg -hide_banner -loglevel error -re -i - %s",
                 format_word, word, output);
    else if (!strcmp(kind, "image"))
        snprintf(command, size, "ffmpeg -hide_banner -loglevel error %s-i %s -frames:v 1 %s", protocols, word, output);
    else
        snprintf(command, size, "ffmpeg -hide_banner -loglevel error %s-ss %.3f %s-i %s %s", protocols,
                 playback->start, playback->loop ? "-stream_loop -1 " : "", word, output);
    free(word);
    free(format_word);
    return COMMAND_OK;
}

enum command_status sandbox_capture_command(char *command, size_t size, const char *program_directory,
                                            const char *mode, const int *numbers, int count) {
    static const char *const modes[] = { "picker", "monitor", "area", "window" };
    bool known = false;
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) known = known || !strcmp(mode, modes[i]);
    if (!known) return COMMAND_INVALID;
    // Only the picker asks the user; the other modes would record the screen for shared code without a dialog.
    if (sandboxed && strcmp(mode, "picker") != 0) return COMMAND_SANDBOXED;
    char program[PATH_MAX + 32];
#if defined(_WIN32)
    snprintf(program, sizeof program, "%s\\hydra-native-capture.exe", program_directory);
#else
    snprintf(program, sizeof program, "%s/hydra-native-capture", program_directory);
#endif
    char *program_word = shell_word(program);
    if (!program_word) return COMMAND_FAILED;
    int length = snprintf(command, size, "%s %s", program_word, mode);
    for (int i = 0; i < count && length < (int)size; i++)
        length += snprintf(command + length, size - (size_t)length, " %d", numbers[i]);
    free(program_word);
    return COMMAND_OK;
}
