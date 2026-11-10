#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "native.h"

#define MAX_VIDEO_SOURCES 16

struct video_source {
    pid_t pid;
    int fd;
    int width;
    int height;
    size_t frame_size;
    // The reader thread fills `back`, then swaps it with `front` under the lock.
    uint8_t *front;
    uint8_t *back;
    bool fresh;
    bool running;
    pthread_t thread;
    pthread_mutex_t lock;
};

static struct video_source *sources[MAX_VIDEO_SOURCES];

static bool read_full(int fd, uint8_t *buffer, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t count = read(fd, buffer + done, size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        done += (size_t)count;
    }
    return true;
}

static void *read_frames(void *argument) {
    struct video_source *source = argument;
    while (read_full(source->fd, source->back, source->frame_size)) {
        pthread_mutex_lock(&source->lock);
        uint8_t *swap = source->front;
        source->front = source->back;
        source->back = swap;
        source->fresh = true;
        pthread_mutex_unlock(&source->lock);
    }
    source->running = false;
    return NULL;
}

int video_open(const char *command, int width, int height) {
    int handle = 0;
    while (handle < MAX_VIDEO_SOURCES && sources[handle]) handle++;
    if (handle == MAX_VIDEO_SOURCES || width <= 0 || height <= 0) return -1;

    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    if (pid == 0) {
        // Own process group, so video_close can stop the whole pipeline (helper plus gst/ffmpeg children).
        setpgid(0, 0);
        dup2(pipe_fds[1], STDOUT_FILENO);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[1]);

    struct video_source *source = calloc(1, sizeof *source);
    source->pid = pid;
    source->fd = pipe_fds[0];
    source->width = width;
    source->height = height;
    source->frame_size = (size_t)width * (size_t)height * 4;
    source->front = malloc(source->frame_size);
    source->back = malloc(source->frame_size);
    source->running = true;
    pthread_mutex_init(&source->lock, NULL);
    pthread_create(&source->thread, NULL, read_frames, source);
    sources[handle] = source;
    return handle;
}

static struct video_source *find_source(int handle) {
    if (handle < 0 || handle >= MAX_VIDEO_SOURCES) return NULL;
    return sources[handle];
}

bool video_upload(int handle, GLuint texture) {
    struct video_source *source = find_source(handle);
    if (!source) return false;
    pthread_mutex_lock(&source->lock);
    bool fresh = source->fresh;
    if (fresh) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, source->width, source->height, GL_RGBA, GL_UNSIGNED_BYTE, source->front);
        source->fresh = false;
    }
    pthread_mutex_unlock(&source->lock);
    return fresh;
}

bool video_is_running(int handle) {
    struct video_source *source = find_source(handle);
    return source && source->running;
}

void video_close(int handle) {
    struct video_source *source = find_source(handle);
    if (!source) return;
    kill(-source->pid, SIGTERM);
    // The reader thread ends when the pipe reaches end of file.
    pthread_join(source->thread, NULL);
    close(source->fd);
    waitpid(source->pid, NULL, 0);
    pthread_mutex_destroy(&source->lock);
    free(source->front);
    free(source->back);
    free(source);
    sources[handle] = NULL;
}

void video_close_all(void) {
    for (int handle = 0; handle < MAX_VIDEO_SOURCES; handle++) video_close(handle);
}
