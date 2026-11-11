#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "native.h"
#include "process.h"

#define MAX_VIDEO_SOURCES 16
#define MAX_VIDEO_DIMENSION 16384 // a sketch gives the size; larger frames are no video and overflow no size
#define CAMERA_FPS 30

// A source reads frames from a stream in a thread of its own (the output of a process, or a stream of the patch
// bay), or takes them from an SDL camera when the render thread asks for a frame.
struct video_source {
    struct process *process; // NULL for a stream of the patch bay and for a camera
    int stream;              // -1 for a camera
    SDL_Camera *camera;
    int width;
    int height;
    size_t frame_size;
    // The reader thread fills `back`, then swaps it with `front` under the lock.
    uint8_t *front;
    uint8_t *back;
    bool fresh;
    SDL_AtomicInt running;
    SDL_Thread *thread;
    SDL_Mutex *lock;
};

static struct video_source *sources[MAX_VIDEO_SOURCES];

static int read_frames(void *argument) {
    struct video_source *source = argument;
    while (SDL_GetAtomicInt(&source->running) && stream_read_all(source->stream, source->back, source->frame_size)) {
        SDL_LockMutex(source->lock);
        uint8_t *swap = source->front;
        source->front = source->back;
        source->back = swap;
        source->fresh = true;
        SDL_UnlockMutex(source->lock);
    }
    SDL_SetAtomicInt(&source->running, 0);
    return 0;
}

static int free_handle(void) {
    int handle = 0;
    while (handle < MAX_VIDEO_SOURCES && sources[handle]) handle++;
    return handle == MAX_VIDEO_SOURCES ? -1 : handle;
}

static bool valid_size(int width, int height) {
    return width > 0 && height > 0 && width <= MAX_VIDEO_DIMENSION && height <= MAX_VIDEO_DIMENSION;
}

static void free_source(struct video_source *source) {
    if (source->camera) SDL_CloseCamera(source->camera);
    if (source->stream >= 0) stream_close(source->stream);
    process_free(source->process);
    SDL_DestroyMutex(source->lock);
    free(source->front);
    free(source->back);
    free(source);
}

// A source for frames of width x height from `stream` or `camera`; the source owns the stream, the process and
// the camera, also when it cannot start (-1). The frame is black until the first one arrives.
static int add_source(int handle, struct process *process, int stream, SDL_Camera *camera, int width, int height) {
    struct video_source *source = calloc(1, sizeof *source);
    if (!source) {
        if (camera) SDL_CloseCamera(camera);
        if (stream >= 0) stream_close(stream);
        process_free(process);
        return -1;
    }
    source->process = process;
    source->stream = stream;
    source->camera = camera;
    source->width = width;
    source->height = height;
    source->frame_size = (size_t)width * (size_t)height * 4;
    source->front = calloc(1, source->frame_size);
    source->back = calloc(1, source->frame_size);
    source->lock = SDL_CreateMutex();
    SDL_SetAtomicInt(&source->running, 1);
    bool ready = source->front && source->back && source->lock;
    if (ready && !camera) ready = (source->thread = SDL_CreateThread(read_frames, "video", source)) != NULL;
    if (!ready) {
        free_source(source);
        return -1;
    }
    sources[handle] = source;
    return handle;
}

int video_open(const char *command, int width, int height) {
    int handle = free_handle();
    if (handle < 0 || !valid_size(width, height)) return -1;
    // The shell runs in a group of its own, so video_close stops the whole pipeline (helper plus gst or ffmpeg).
    int output;
    struct process *process = process_start_shell(command, NULL, &output);
    if (!process) return -1;
    return add_source(handle, process, output, NULL, width, height);
}

int video_open_stream(int stream, int width, int height) {
    int handle = free_handle();
    if (handle < 0 || !valid_size(width, height)) {
        stream_close(stream);
        return -1;
    }
    return add_source(handle, NULL, stream, NULL, width, height);
}

#if defined(__linux__)
#define MAX_VIDEO_DEVICES 64
#define CAMERA_NAME_SIZE 24

// The capture nodes of /dev/video* in the order of their number; a camera also has a metadata node, whose index
// is 1.
static int linux_cameras(int *numbers, int capacity) {
    int count = 0;
    for (int number = 0; number < MAX_VIDEO_DEVICES && count < capacity; number++) {
        char path[64], index[8] = "";
        snprintf(path, sizeof path, "/sys/class/video4linux/video%d/index", number);
        FILE *file = fopen(path, "r");
        if (!file) continue;
        bool capture = fgets(index, sizeof index, file) && atoi(index) == 0;
        fclose(file);
        if (capture) numbers[count++] = number;
    }
    return count;
}

int video_camera_names(const char **names, int capacity) {
    static char storage[MAX_VIDEO_DEVICES][CAMERA_NAME_SIZE];
    int numbers[MAX_VIDEO_DEVICES];
    int count = linux_cameras(numbers, capacity < MAX_VIDEO_DEVICES ? capacity : MAX_VIDEO_DEVICES);
    for (int index = 0; index < count; index++) {
        snprintf(storage[index], CAMERA_NAME_SIZE, "/dev/video%d", numbers[index]);
        names[index] = storage[index];
    }
    return count;
}

int video_open_camera(int index, int width, int height) {
    int numbers[MAX_VIDEO_DEVICES];
    int count = linux_cameras(numbers, MAX_VIDEO_DEVICES);
    if (index < 0 || index >= count) return -1;
    char command[512];
    snprintf(command, sizeof command,
             "gst-launch-1.0 -q v4l2src device=/dev/video%d ! decodebin ! videoconvert ! videoscale ! "
             "video/x-raw,format=RGBA,width=%d,height=%d ! fdsink fd=1 sync=false",
             numbers[index], width, height);
    return video_open(command, width, height);
}
#else
static bool start_cameras(void) {
    if (SDL_WasInit(SDL_INIT_CAMERA) || SDL_InitSubSystem(SDL_INIT_CAMERA)) return true;
    fprintf(stderr, "camera: %s\n", SDL_GetError());
    return false;
}

int video_camera_names(const char **names, int capacity) {
    if (!start_cameras()) return 0;
    int count = 0;
    SDL_CameraID *cameras = SDL_GetCameras(&count);
    for (int index = 0; index < count && index < capacity; index++) {
        const char *name = SDL_GetCameraName(cameras[index]);
        names[index] = name ? name : "camera";
    }
    SDL_free(cameras);
    return count < capacity ? count : capacity;
}

// SDL converts and scales the frames of the camera to RGBA of width x height in its own camera thread.
int video_open_camera(int index, int width, int height) {
    int handle = free_handle();
    if (handle < 0 || !valid_size(width, height) || !start_cameras()) return -1;
    int count = 0;
    SDL_CameraID *cameras = SDL_GetCameras(&count);
    SDL_CameraID camera_id = cameras && index >= 0 && index < count ? cameras[index] : 0;
    SDL_free(cameras);
    if (!camera_id) return -1;
    SDL_CameraSpec spec = { .format = SDL_PIXELFORMAT_RGBA32, .colorspace = SDL_COLORSPACE_SRGB, .width = width,
                            .height = height, .framerate_numerator = CAMERA_FPS, .framerate_denominator = 1 };
    SDL_Camera *camera = SDL_OpenCamera(camera_id, &spec);
    if (!camera) {
        fprintf(stderr, "camera %d: %s\n", index, SDL_GetError());
        return -1;
    }
    return add_source(handle, NULL, -1, camera, width, height);
}
#endif

// Under the lock of the source.
static void take_camera_frame(struct video_source *source) {
    if (!SDL_GetAtomicInt(&source->running)) return;
    if (SDL_GetCameraPermissionState(source->camera) == SDL_CAMERA_PERMISSION_STATE_DENIED) {
        fprintf(stderr, "camera: access denied; allow the camera for hydra-native in the system settings\n");
        SDL_SetAtomicInt(&source->running, 0);
        return;
    }
    SDL_Surface *frame, *newest = NULL;
    while ((frame = SDL_AcquireCameraFrame(source->camera, NULL))) {
        if (newest) SDL_ReleaseCameraFrame(source->camera, newest);
        newest = frame;
    }
    if (!newest) return;
    size_t row = (size_t)source->width * 4;
    int rows = newest->h < source->height ? newest->h : source->height;
    if (newest->format == SDL_PIXELFORMAT_RGBA32 && newest->w == source->width) {
        for (int y = 0; y < rows; y++)
            memcpy(source->front + row * (size_t)y, (const uint8_t *)newest->pixels + (size_t)newest->pitch * (size_t)y,
                   row);
        source->fresh = true;
    }
    SDL_ReleaseCameraFrame(source->camera, newest);
}

static struct video_source *find_source(int handle) {
    if (handle < 0 || handle >= MAX_VIDEO_SOURCES) return NULL;
    return sources[handle];
}

bool video_upload(int handle, GLuint texture) {
    struct video_source *source = find_source(handle);
    if (!source) return false;
    SDL_LockMutex(source->lock);
    if (source->camera) take_camera_frame(source);
    bool fresh = source->fresh;
    if (fresh) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, source->width, source->height, GL_RGBA, GL_UNSIGNED_BYTE,
                        source->front);
        source->fresh = false;
    }
    SDL_UnlockMutex(source->lock);
    return fresh;
}

// `pixels` holds width * height * 4 bytes of video_size().
bool video_copy_frame(int handle, unsigned char *pixels) {
    struct video_source *source = find_source(handle);
    if (!source) return false;
    SDL_LockMutex(source->lock);
    if (source->camera) take_camera_frame(source);
    memcpy(pixels, source->front, source->frame_size);
    SDL_UnlockMutex(source->lock);
    return true;
}

bool video_size(int handle, int *width, int *height) {
    struct video_source *source = find_source(handle);
    if (!source) return false;
    *width = source->width;
    *height = source->height;
    return true;
}

bool video_is_running(int handle) {
    struct video_source *source = find_source(handle);
    return source && SDL_GetAtomicInt(&source->running);
}

void video_close(int handle) {
    struct video_source *source = find_source(handle);
    if (!source) return;
    // The reader thread ends at the end of the stream: the processes of the group end, or the socket shuts down.
    if (source->camera) {
        SDL_CloseCamera(source->camera);
        source->camera = NULL;
    } else if (source->process) {
        // Not only SIGTERM: this runs on the main thread, which must not wait for a child that does not end.
        process_stop(source->process);
    } else {
        SDL_SetAtomicInt(&source->running, 0);
        stream_shutdown(source->stream);
    }
    if (source->thread) SDL_WaitThread(source->thread, NULL);
    free_source(source);
    sources[handle] = NULL;
}

void video_close_all(void) {
    for (int handle = 0; handle < MAX_VIDEO_SOURCES; handle++) video_close(handle);
}
