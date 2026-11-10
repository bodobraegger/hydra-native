// Shader programs of the hydra passes, compiled without stopping the frames. programs_start() picks one method:
// - thread: a second EGL context, shared with the first one and without a surface, compiles and links on a thread
//   of its own. Mesa needs this: it offers GL_KHR_parallel_shader_compile, but its glLinkProgram links on the
//   calling thread anyway (about 100 ms for one large hydra pass on Intel iris). The thread uses EGL only: SDL
//   allows its window and GL functions only on the main thread, and a second SDL context would also reset the
//   swap interval of the window.
// - parallel: GL_KHR_parallel_shader_compile, the driver compiles on threads of its own. ANGLE does so; there a
//   second context would share one lock with the first.
// - blocking: neither one is available, so the first status query waits for the link.
#define _GNU_SOURCE
#include "native.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_egl.h>

#ifndef GL_COMPLETION_STATUS_KHR
#define GL_COMPLETION_STATUS_KHR 0x91B1
#endif
#define ALL_COMPILER_THREADS 0xFFFFFFFF // the driver picks the count
#define LOG_SIZE 4096

enum method { METHOD_BLOCKING, METHOD_PARALLEL, METHOD_THREAD };
static const char *const method_names[] = { "blocking", "parallel", "thread" };
static enum method method;

// A program for the thread. The main thread creates the GL names, so that program_create() returns at once.
struct job {
    GLuint program, vertex, fragment;
    char *vertex_source, *fragment_source; // freed by the thread
    bool started, done, linked;
    char *error; // malloc'd when the program failed
    struct job *next;
};

static struct {
    EGLDisplay display;
    EGLContext context;
    EGLBoolean (*make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*destroy_context)(EGLDisplay, EGLContext);
    pthread_t id;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    struct job *jobs; // newest first, until program_status() reports them
    bool stopping;
} thread = { .mutex = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER };

static void link_program(GLuint program, GLuint vertex, const char *vertex_source, GLuint fragment,
                         const char *fragment_source) {
    glShaderSource(vertex, 1, &vertex_source, NULL);
    glCompileShader(vertex);
    glShaderSource(fragment, 1, &fragment_source, NULL);
    glCompileShader(fragment);
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
}

// Reads the result of a link, frees the shaders, and deletes a program that failed; then `*error` is the
// malloc'd log in the form of the old blocking compile ("fragment shader: ...").
static bool finish_program(GLuint program, char **error) {
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    GLuint shaders[2];
    GLsizei count = 0;
    glGetAttachedShaders(program, 2, &count, shaders);
    char log[LOG_SIZE] = "";
    *error = NULL;
    for (GLsizei i = 0; i < count; i++) {
        GLint compiled, type;
        glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &compiled);
        glGetShaderiv(shaders[i], GL_SHADER_TYPE, &type);
        if (!compiled && !*error) {
            glGetShaderInfoLog(shaders[i], sizeof log, NULL, log);
            const char *name = type == GL_VERTEX_SHADER ? "vertex" : "fragment";
            if (asprintf(error, "%s shader: %s", name, log) < 0) *error = NULL;
        }
        glDetachShader(program, shaders[i]);
        glDeleteShader(shaders[i]);
    }
    if (linked) return true;
    if (!*error) {
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        if (asprintf(error, "link: %s", log) < 0) *error = NULL;
    }
    if (!*error) *error = strdup("link failed");
    glDeleteProgram(program);
    return false;
}

static struct job *next_job(void) {
    for (struct job *job = thread.jobs; job; job = job->next)
        if (!job->started) return job;
    return NULL;
}

// The newest waiting job first: after several evals in a row, the last one is the one that the outputs wait for.
static void *compile_jobs(void *unused) {
    thread.make_current(thread.display, EGL_NO_SURFACE, EGL_NO_SURFACE, thread.context);
    pthread_mutex_lock(&thread.mutex);
    while (!thread.stopping) {
        struct job *job = next_job();
        if (!job) {
            pthread_cond_wait(&thread.changed, &thread.mutex);
            continue;
        }
        job->started = true;
        pthread_mutex_unlock(&thread.mutex);
        link_program(job->program, job->vertex, job->vertex_source, job->fragment, job->fragment_source);
        bool linked = finish_program(job->program, &job->error);
        glFinish(); // the other context sees the program only after the link is complete
        free(job->vertex_source);
        free(job->fragment_source);
        pthread_mutex_lock(&thread.mutex);
        job->linked = linked;
        job->done = true;
        pthread_cond_broadcast(&thread.changed);
    }
    pthread_mutex_unlock(&thread.mutex);
    thread.make_current(thread.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return NULL;
}

// Needs EGL (not GLX on X11) and EGL_KHR_surfaceless_context.
static bool start_thread(void) {
    const char *(*query_string)(EGLDisplay, EGLint) = (void *)SDL_EGL_GetProcAddress("eglQueryString");
    EGLContext (*current_context)(void) = (void *)SDL_EGL_GetProcAddress("eglGetCurrentContext");
    EGLContext (*create_context)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) =
        (void *)SDL_EGL_GetProcAddress("eglCreateContext");
    thread.make_current = (void *)SDL_EGL_GetProcAddress("eglMakeCurrent");
    thread.destroy_context = (void *)SDL_EGL_GetProcAddress("eglDestroyContext");
    thread.display = SDL_EGL_GetCurrentDisplay();
    EGLConfig config = SDL_EGL_GetCurrentConfig();
    if (!thread.display || !config || !query_string || !current_context || !create_context || !thread.make_current ||
        !thread.destroy_context)
        return false;
    const char *extensions = query_string(thread.display, EGL_EXTENSIONS);
    if (!extensions || !strstr(extensions, "EGL_KHR_surfaceless_context")) return false;
    static const EGLint attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    thread.context = create_context(thread.display, config, current_context(), attributes);
    if (thread.context == EGL_NO_CONTEXT) return false;
    if (pthread_create(&thread.id, NULL, compile_jobs, NULL) == 0) return true;
    thread.destroy_context(thread.display, thread.context);
    return false;
}

void programs_start(void) {
    const char *forced = getenv("HYDRA_SHADER_COMPILE"); // one of method_names, to compare the methods
    bool parallel = SDL_GL_ExtensionSupported("GL_KHR_parallel_shader_compile");
    bool mesa = strstr((const char *)glGetString(GL_VERSION), "Mesa") != NULL;
    if (forced) {
        if (!strcmp(forced, "thread") && start_thread()) method = METHOD_THREAD;
        else method = !strcmp(forced, "parallel") && parallel ? METHOD_PARALLEL : METHOD_BLOCKING;
    } else if (mesa && start_thread()) method = METHOD_THREAD;
    else method = parallel ? METHOD_PARALLEL : METHOD_BLOCKING;
    void (*max_threads)(GLuint) = (void (*)(GLuint))SDL_GL_GetProcAddress("glMaxShaderCompilerThreadsKHR");
    if (method == METHOD_PARALLEL && max_threads) max_threads(ALL_COMPILER_THREADS);
}

void programs_stop(void) {
    if (method != METHOD_THREAD) return;
    pthread_mutex_lock(&thread.mutex);
    thread.stopping = true;
    pthread_cond_broadcast(&thread.changed);
    pthread_mutex_unlock(&thread.mutex);
    pthread_join(thread.id, NULL);
    thread.destroy_context(thread.display, thread.context);
    method = METHOD_BLOCKING;
}

const char *programs_method(void) { return method_names[method]; }

GLuint program_create(const char *vertex_source, const char *fragment_source) {
    GLuint program = glCreateProgram();
    GLuint vertex = glCreateShader(GL_VERTEX_SHADER), fragment = glCreateShader(GL_FRAGMENT_SHADER);
    if (method != METHOD_THREAD) {
        link_program(program, vertex, vertex_source, fragment, fragment_source);
        return program;
    }
    struct job *job = calloc(1, sizeof *job);
    *job = (struct job){ .program = program, .vertex = vertex, .fragment = fragment,
                         .vertex_source = strdup(vertex_source), .fragment_source = strdup(fragment_source) };
    glFlush(); // GLES 3.0, appendix D: another context may use an object only after its creation is flushed
    pthread_mutex_lock(&thread.mutex);
    job->next = thread.jobs;
    thread.jobs = job;
    pthread_cond_broadcast(&thread.changed);
    pthread_mutex_unlock(&thread.mutex);
    return program;
}

// Only the main thread adds and removes jobs, so the list stays the same while it waits.
static enum program_status thread_status(GLuint program, bool wait, char **error) {
    pthread_mutex_lock(&thread.mutex);
    struct job **link = &thread.jobs;
    while (*link && (*link)->program != program) link = &(*link)->next;
    struct job *job = *link;
    if (!job) {
        pthread_mutex_unlock(&thread.mutex);
        return PROGRAM_LINKED;
    }
    while (wait && !job->done) pthread_cond_wait(&thread.changed, &thread.mutex);
    if (!job->done) {
        pthread_mutex_unlock(&thread.mutex);
        return PROGRAM_COMPILING;
    }
    *link = job->next;
    pthread_mutex_unlock(&thread.mutex);
    enum program_status status = job->linked ? PROGRAM_LINKED : PROGRAM_FAILED;
    *error = job->error;
    free(job);
    return status;
}

enum program_status program_status(GLuint program, bool wait, char **error) {
    *error = NULL;
    if (method == METHOD_THREAD) return thread_status(program, wait, error);
    GLint done = GL_TRUE;
    if (method == METHOD_PARALLEL && !wait) glGetProgramiv(program, GL_COMPLETION_STATUS_KHR, &done);
    if (!done) return PROGRAM_COMPILING;
    return finish_program(program, error) ? PROGRAM_LINKED : PROGRAM_FAILED;
}
