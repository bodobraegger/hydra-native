#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "native.h"
#include "process.h"

#define MAX_VIDEO_DIMENSION 16384 // a sketch gives the size; larger frames are no video and overflow no size
#define CAMERA_FPS 30
#define RGBA_BYTES 4
#define FRAME_BUFFERS 3 // a triple buffer: see struct video_source
#define PLANE_COUNT 3   // Y, U and V of I420

// A source reads frames from a stream in a thread of its own (the output of a process, or a stream of the patch
// bay), or takes them from an SDL camera when the render thread asks for a frame.
//
// The frames go straight into pixel buffer objects, which stay mapped, so that the GPU copies them into the texture
// and the main thread copies no pixels. The three buffers are a triple buffer: the reader fills `writing`, then
// swaps it with `ready` under the lock; the main thread swaps `ready` with `shown` when it is fresh, unmaps `shown`,
// starts the upload from it and maps it again. So the reader never waits for the main thread, and a slow main
// thread drops frames instead of holding the stream.
struct video_source {
    struct process *process; // NULL for a stream of the patch bay and for a camera
    bool kill_at_once;       // a media command, see video_open_media
    bool on_demand;          // no reader thread: video_upload reads the frames, see video_open_media
    int stream;              // -1 for a camera
    SDL_Camera *camera;
    int width;
    int height;
    enum video_format format;
    size_t frame_size;
    GLuint buffers[FRAME_BUFFERS];
    uint8_t *mapped[FRAME_BUFFERS]; // NULL where the driver could not map the buffer
    int writing, ready, shown;      // indexes into `buffers`
    bool fresh;                     // `ready` holds a frame that the main thread did not take yet
    GLuint planes[PLANE_COUNT];     // I420: the planes, which the conversion draws into the texture of the sketch
    float color_matrix[9];          // I420: YUV to RGB, in columns
    float color_offset[3];
    SDL_AtomicInt frames; // received from the stream
    SDL_AtomicInt running;
    SDL_Thread *thread;
    SDL_Mutex *lock;
};

static struct video_source *sources[MAX_VIDEO_SOURCES];

// The draw that converts the planes of an I420 frame into the RGBA texture of the sketch. It draws the triangle of
// hydra at attribute 0 (src/main.c); the coordinates of the texture are those of the frame, row 0 at the top.
static const char *const CONVERSION_VERTEX = "#version 300 es\n"
                                             "in vec2 position;\n"
                                             "out vec2 uv;\n"
                                             "void main() {\n"
                                             "  uv = position * 0.5 + 0.5;\n"
                                             "  gl_Position = vec4(position, 0.0, 1.0);\n"
                                             "}\n";
static const char *const CONVERSION_FRAGMENT = "#version 300 es\n"
                                               "precision mediump float;\n"
                                               "uniform sampler2D luma, blue, red;\n"
                                               "uniform mat3 matrix;\n"
                                               "uniform vec3 offset;\n"
                                               "in vec2 uv;\n"
                                               "out vec4 color;\n"
                                               "void main() {\n"
                                               "  vec3 yuv = vec3(texture(luma, uv).r, texture(blue, uv).r,\n"
                                               "                  texture(red, uv).r);\n"
                                               "  color = vec4(clamp(matrix * (yuv - offset), 0.0, 1.0), 1.0);\n"
                                               "}\n";

static struct {
    GLuint program, framebuffer;
    GLint matrix, offset;
    bool failed; // the driver did not compile it; I420 sources stay black
} conversion;

static size_t frame_size(enum video_format format, int width, int height) {
    size_t pixels = (size_t)width * (size_t)height;
    if (format == VIDEO_FORMAT_RGBA) return pixels * RGBA_BYTES;
    size_t chroma = (size_t)((width + 1) / 2) * (size_t)((height + 1) / 2);
    return pixels + 2 * chroma;
}

// The frame in `writing` becomes the newest one.
static void publish_frame(struct video_source *source) {
    SDL_LockMutex(source->lock);
    int swap = source->ready;
    source->ready = source->writing;
    source->writing = swap;
    source->fresh = true;
    SDL_UnlockMutex(source->lock);
    SDL_AddAtomicInt(&source->frames, 1);
}

// The next frame of the stream; at the end of the stream, the source stops running. Only the reader touches
// `writing`, so the read needs no lock.
static bool read_frame(struct video_source *source) {
    if (!stream_read_all(source->stream, source->mapped[source->writing], source->frame_size)) {
        SDL_SetAtomicInt(&source->running, 0);
        return false;
    }
    publish_frame(source);
    return true;
}

static int read_frames(void *argument) {
    struct video_source *source = argument;
    while (SDL_GetAtomicInt(&source->running) && read_frame(source)) {}
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

// Mapped for writing, with the old content gone: the driver gives new memory while the GPU still copies from the
// old one, so the map does not wait for the GPU.
static uint8_t *map_buffer(GLuint buffer, size_t size) {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
    uint8_t *mapped =
        glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr)size, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    return mapped;
}

static void unmap_buffer(GLuint buffer) {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

// On the main thread, after the reader thread ended.
static void free_source(struct video_source *source) {
    if (source->camera) SDL_CloseCamera(source->camera);
    if (source->stream >= 0) stream_close(source->stream);
    process_free(source->process);
    SDL_DestroyMutex(source->lock);
    for (int i = 0; i < FRAME_BUFFERS; i++)
        if (source->mapped[i]) unmap_buffer(source->buffers[i]);
    glDeleteBuffers(FRAME_BUFFERS, source->buffers);
    glDeleteTextures(PLANE_COUNT, source->planes);
    free(source);
}

static GLuint create_plane(int width, int height, GLint filter) {
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

// The buffers, mapped, and the planes of I420; false when the driver has no memory for them.
static bool create_frames(struct video_source *source) {
    while (glGetError() != GL_NO_ERROR) {} // the errors of earlier calls
    glGenBuffers(FRAME_BUFFERS, source->buffers);
    for (int i = 0; i < FRAME_BUFFERS; i++) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, source->buffers[i]);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)source->frame_size, NULL, GL_STREAM_DRAW);
        if (!(source->mapped[i] = map_buffer(source->buffers[i], source->frame_size))) return false;
    }
    source->writing = 0;
    source->ready = 1;
    source->shown = 2;
    if (source->format == VIDEO_FORMAT_I420) {
        int chroma_width = (source->width + 1) / 2, chroma_height = (source->height + 1) / 2;
        source->planes[0] = create_plane(source->width, source->height, GL_NEAREST);
        source->planes[1] = create_plane(chroma_width, chroma_height, GL_LINEAR);
        source->planes[2] = create_plane(chroma_width, chroma_height, GL_LINEAR);
    }
    return glGetError() == GL_NO_ERROR;
}

static void set_color(struct video_source *source, enum video_matrix matrix, bool full_range) {
    // The weights of red and blue in the luma of each matrix (ITU-R BT.601, BT.709, BT.2020).
    static const float weights[][2] = {
        [VIDEO_MATRIX_BT601] = { 0.299f, 0.114f },
        [VIDEO_MATRIX_BT709] = { 0.2126f, 0.0722f },
        [VIDEO_MATRIX_BT2020] = { 0.2627f, 0.0593f },
    };
    float red = weights[matrix][0], blue = weights[matrix][1], green = 1.0f - red - blue;
    // Limited range: luma from 16 to 235 and chroma from 16 to 240 of 255.
    float luma_scale = full_range ? 1.0f : 255.0f / 219.0f, chroma_scale = full_range ? 1.0f : 255.0f / 224.0f;
    const float columns[9] = {
        luma_scale,
        luma_scale,
        luma_scale,
        0.0f,
        -chroma_scale * 2.0f * blue * (1.0f - blue) / green,
        chroma_scale * 2.0f * (1.0f - blue),
        chroma_scale * 2.0f * (1.0f - red),
        -chroma_scale * 2.0f * red * (1.0f - red) / green,
        0.0f,
    };
    memcpy(source->color_matrix, columns, sizeof columns);
    source->color_offset[0] = full_range ? 0.0f : 16.0f / 255.0f;
    source->color_offset[1] = source->color_offset[2] = 128.0f / 255.0f;
}

// A source for frames of width x height from `stream` or `camera`; the source owns the stream, the process and
// the camera, also when it cannot start (-1). The texture keeps its content until the first frame arrives.
static int add_source(int handle, struct process *process, int stream, SDL_Camera *camera, int width, int height,
                      enum video_format format, bool on_demand) {
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
    source->on_demand = on_demand;
    source->width = width;
    source->height = height;
    source->format = format;
    source->frame_size = frame_size(format, width, height);
    set_color(source, VIDEO_MATRIX_BT601, false);
    source->lock = SDL_CreateMutex();
    SDL_SetAtomicInt(&source->running, 1);
    bool ready = source->lock && create_frames(source);
    bool threaded = !camera && !on_demand;
    if (ready && threaded) ready = (source->thread = SDL_CreateThread(read_frames, "video", source)) != NULL;
    if (!ready) {
        fprintf(stderr, "video: no memory for frames of %dx%d\n", width, height);
        free_source(source);
        return -1;
    }
    sources[handle] = source;
    return handle;
}

static int open_command(const char *command, int width, int height, enum video_format format, bool on_demand) {
    int handle = free_handle();
    if (handle < 0 || !valid_size(width, height)) return -1;
    // The shell runs in a group of its own, so video_close stops the whole pipeline (helper plus gst or ffmpeg).
    int output;
    struct process *process = process_start_shell(command, NULL, &output);
    if (!process) return -1;
    return add_source(handle, process, output, NULL, width, height, format, on_demand);
}

int video_open(const char *command, int width, int height) {
    return open_command(command, width, height, VIDEO_FORMAT_RGBA, false);
}

// On SIGTERM, ffmpeg still writes the frames that it decoded already, at the pace of its input or filter, so
// video_close kills it at once; its output has nothing to finish. A source `on_demand` has no reader thread:
// video_upload reads the frames on the calling thread, and ffmpeg waits at the full pipe meanwhile.
int video_open_media(const char *command, int width, int height, enum video_format format, bool on_demand) {
    int handle = open_command(command, width, height, format, on_demand);
    if (handle >= 0) sources[handle]->kill_at_once = true;
    return handle;
}

int video_open_stream(int stream, int width, int height) {
    int handle = free_handle();
    if (handle < 0 || !valid_size(width, height)) {
        stream_close(stream);
        return -1;
    }
    return add_source(handle, NULL, stream, NULL, width, height, VIDEO_FORMAT_I420, false);
}

#if defined(__linux__)
#define MAX_VIDEO_DEVICES 64
#define CAMERA_NAME_SIZE 24
// GStreamer pads the rows of an I420 plane to 4 bytes, so I420 without padding needs a width of a multiple of 8.
#define I420_WIDTH_STEP 8

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

// I420 in BT.601 where the size permits it, else RGBA.
int video_open_camera(int index, int width, int height) {
    int numbers[MAX_VIDEO_DEVICES];
    int count = linux_cameras(numbers, MAX_VIDEO_DEVICES);
    if (index < 0 || index >= count) return -1;
    bool planar = width % I420_WIDTH_STEP == 0 && height % 2 == 0;
    char command[512];
    snprintf(command, sizeof command,
             "gst-launch-1.0 -q v4l2src device=/dev/video%d ! decodebin ! videoconvert ! videoscale ! "
             "video/x-raw,format=%s,width=%d,height=%d ! fdsink fd=1 sync=false",
             numbers[index], planar ? "I420,colorimetry=bt601" : "RGBA", width, height);
    return open_command(command, width, height, planar ? VIDEO_FORMAT_I420 : VIDEO_FORMAT_RGBA, false);
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
    return add_source(handle, NULL, -1, camera, width, height, VIDEO_FORMAT_RGBA, false);
}
#endif

// The newest frame of the SDL camera into `writing`, on the main thread, which is the reader of a camera.
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
    size_t row = (size_t)source->width * RGBA_BYTES;
    int rows = newest->h < source->height ? newest->h : source->height;
    if (newest->format == SDL_PIXELFORMAT_RGBA32 && newest->w == source->width) {
        uint8_t *pixels = source->mapped[source->writing];
        for (int y = 0; y < rows; y++)
            memcpy(pixels + row * (size_t)y, (const uint8_t *)newest->pixels + (size_t)newest->pitch * (size_t)y, row);
        publish_frame(source);
    }
    SDL_ReleaseCameraFrame(source->camera, newest);
}

static struct video_source *find_source(int handle) {
    if (handle < 0 || handle >= MAX_VIDEO_SOURCES) return NULL;
    return sources[handle];
}

void video_set_color(int handle, enum video_matrix matrix, bool full_range) {
    struct video_source *source = find_source(handle);
    if (source) set_color(source, matrix, full_range);
}

static bool conversion_ready(void) {
    if (conversion.program || conversion.failed) return !conversion.failed;
    GLuint program = program_create(CONVERSION_VERTEX, CONVERSION_FRAGMENT);
    char *error = NULL;
    if (!program || program_status(program, true, &error) != PROGRAM_LINKED) {
        fprintf(stderr, "video: the YUV conversion does not compile: %s\n", error ? error : "no program");
        free(error);
        conversion.failed = true;
        return false;
    }
    conversion.program = program;
    glUseProgram(program);
    static const char *const samplers[PLANE_COUNT] = { "luma", "blue", "red" };
    for (int i = 0; i < PLANE_COUNT; i++) glUniform1i(glGetUniformLocation(program, samplers[i]), i);
    conversion.matrix = glGetUniformLocation(program, "matrix");
    conversion.offset = glGetUniformLocation(program, "offset");
    glGenFramebuffers(1, &conversion.framebuffer);
    return true;
}

// Draws the planes into `texture`. The state of hydra (framebuffer, viewport, program, textures) stays as it was.
static void convert(const struct video_source *source, GLuint texture) {
    if (!conversion_ready()) return;
    GLint framebuffer, program, active_texture, viewport[4], bound[PLANE_COUNT];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glBindFramebuffer(GL_FRAMEBUFFER, conversion.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    glViewport(0, 0, source->width, source->height);
    glUseProgram(conversion.program);
    glUniformMatrix3fv(conversion.matrix, 1, GL_FALSE, source->color_matrix);
    glUniform3fv(conversion.offset, 1, source->color_offset);
    for (int i = 0; i < PLANE_COUNT; i++) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound[i]);
        glBindTexture(GL_TEXTURE_2D, source->planes[i]);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    for (int i = 0; i < PLANE_COUNT; i++) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        glBindTexture(GL_TEXTURE_2D, (GLuint)bound[i]);
    }
    glActiveTexture((GLenum)active_texture);
    glUseProgram((GLuint)program);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)framebuffer);
}

// Uploads the frame of `shown` from its buffer, then maps the buffer again for the reader.
static void upload_shown(struct video_source *source, GLuint texture) {
    GLuint buffer = source->buffers[source->shown];
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    if (source->format == VIDEO_FORMAT_RGBA) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, source->width, source->height, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    } else {
        int widths[PLANE_COUNT] = { source->width, (source->width + 1) / 2, (source->width + 1) / 2 };
        int heights[PLANE_COUNT] = { source->height, (source->height + 1) / 2, (source->height + 1) / 2 };
        size_t offset = 0;
        for (int i = 0; i < PLANE_COUNT; i++) {
            glBindTexture(GL_TEXTURE_2D, source->planes[i]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, widths[i], heights[i], GL_RED, GL_UNSIGNED_BYTE,
                            (const void *)(uintptr_t)offset);
            offset += (size_t)widths[i] * (size_t)heights[i];
        }
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (source->format == VIDEO_FORMAT_I420) convert(source, texture);
    source->mapped[source->shown] = map_buffer(buffer, source->frame_size);
    if (!source->mapped[source->shown]) fprintf(stderr, "video: the driver cannot map a frame buffer\n");
}

// A source on demand first reads and drops frames until `frames` came; it blocks while ffmpeg decodes them.
bool video_upload(int handle, GLuint texture, int frames) {
    struct video_source *source = find_source(handle);
    if (!source) return false;
    while (source->on_demand && SDL_GetAtomicInt(&source->frames) < frames && read_frame(source)) {}
    if (source->camera) take_camera_frame(source);
    SDL_LockMutex(source->lock);
    // A buffer that the driver could not map again stays `shown`, so that the reader never gets it.
    bool fresh = source->fresh && source->mapped[source->shown];
    if (fresh) {
        int swap = source->shown;
        source->shown = source->ready;
        source->ready = swap;
        source->fresh = false;
    }
    SDL_UnlockMutex(source->lock);
    if (fresh) upload_shown(source, texture);
    return fresh;
}

int video_frames(int handle) {
    struct video_source *source = find_source(handle);
    return source ? SDL_GetAtomicInt(&source->frames) : 0;
}

int video_count(void) {
    int count = 0;
    for (int handle = 0; handle < MAX_VIDEO_SOURCES; handle++) count += sources[handle] != NULL;
    return count;
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
        if (source->kill_at_once) process_kill_at_once(source->process);
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
    if (conversion.program) program_delete(conversion.program);
    glDeleteFramebuffers(1, &conversion.framebuffer);
    conversion.program = conversion.framebuffer = 0;
}
