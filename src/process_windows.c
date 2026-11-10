// The Windows backend of src/process.h: CreateProcessW in a job object, anonymous pipes, and named pipes as local
// sockets. Pipe handles become C runtime descriptors (_open_osfhandle), and the stream functions use ReadFile
// and WriteFile on them, so no text mode of the C runtime changes the bytes.
#define WIN32_LEAN_AND_MEAN
#include "process.h"

#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define PIPE_CONNECT_ATTEMPTS 10
#define PIPE_BUSY_WAIT_MS 200
#define KILLED_EXIT_CODE 1
#define MAX_IO_BYTES (1u << 30)

struct process {
    HANDLE process;
    HANDLE job;
    int status; // PROCESS_RUNNING until the process ended
};

// A UTF-16 copy of a UTF-8 text, or NULL.
static wchar_t *wide(const char *text) {
    int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (length <= 0) return NULL;
    wchar_t *result = malloc(sizeof *result * (size_t)length);
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result, length);
    return result;
}

// Appends `argument` to `line` with the quoting rules of CommandLineToArgvW and the C runtime: quotes around
// arguments with spaces, tabs or quotes, a backslash before each quote, and doubled backslashes before a quote.
static void append_argument(char *line, size_t *used, const char *argument) {
    if (*used) line[(*used)++] = ' ';
    bool needs_quotes = !*argument || strpbrk(argument, " \t\n\v\"");
    if (!needs_quotes) {
        size_t length = strlen(argument);
        memcpy(line + *used, argument, length);
        *used += length;
        return;
    }
    line[(*used)++] = '"';
    for (const char *c = argument;; c++) {
        size_t backslashes = 0;
        while (*c == '\\') {
            backslashes++;
            c++;
        }
        if (!*c) { // before the closing quote, the backslashes are doubled
            for (size_t i = 0; i < backslashes * 2; i++) line[(*used)++] = '\\';
            break;
        }
        if (*c == '"') {
            for (size_t i = 0; i < backslashes * 2 + 1; i++) line[(*used)++] = '\\';
        } else {
            for (size_t i = 0; i < backslashes; i++) line[(*used)++] = '\\';
        }
        line[(*used)++] = *c;
    }
    line[(*used)++] = '"';
}

// An inheritable duplicate of a standard handle of this process for the child, or NULL when there is none (a GUI
// program has no console streams).
static HANDLE inheritable_standard_handle(DWORD which) {
    HANDLE handle = GetStdHandle(which), duplicate = NULL;
    if (handle == NULL || handle == INVALID_HANDLE_VALUE) return NULL;
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return NULL;
    return duplicate;
}

// A pipe whose end for the child (`child_end`) is inheritable; the end of this process becomes a descriptor.
static bool make_pipe(bool child_reads, HANDLE *child_end, int *own_end) {
    SECURITY_ATTRIBUTES security = { sizeof security, NULL, TRUE };
    HANDLE read_end, write_end;
    if (!CreatePipe(&read_end, &write_end, &security, 0)) return false;
    *child_end = child_reads ? read_end : write_end;
    HANDLE own = child_reads ? write_end : read_end;
    SetHandleInformation(own, HANDLE_FLAG_INHERIT, 0);
    *own_end = _open_osfhandle((intptr_t)own, child_reads ? _O_WRONLY | _O_BINARY : _O_RDONLY | _O_BINARY);
    if (*own_end < 0) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        return false;
    }
    return true;
}

// Starts `command_line` (UTF-8) suspended, puts it into a new job object that kills its processes when the last
// handle to the job closes, and resumes it. Only the three standard handles are inherited
// (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), so a pipe of one child never leaks into another child.
static struct process *spawn(const char *command_line, int *input, int *output) {
    HANDLE standard[3] = { NULL, NULL, NULL };
    int own_input = -1, own_output = -1;
    bool ok = true;
    if (input) ok = make_pipe(true, &standard[0], &own_input);
    else standard[0] = inheritable_standard_handle(STD_INPUT_HANDLE);
    if (ok && output) ok = make_pipe(false, &standard[1], &own_output);
    else if (ok) standard[1] = inheritable_standard_handle(STD_OUTPUT_HANDLE);
    standard[2] = inheritable_standard_handle(STD_ERROR_HANDLE);

    HANDLE inherited[3];
    DWORD inherited_count = 0;
    for (int i = 0; i < 3; i++)
        if (standard[i]) inherited[inherited_count++] = standard[i];
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = malloc(attribute_size);
    ok = ok && InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size);
    bool attributes_ready = ok;
    if (ok && inherited_count)
        ok = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                       inherited_count * sizeof *inherited, NULL, NULL);

    STARTUPINFOEXW startup = { 0 };
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = standard[0];
    startup.StartupInfo.hStdOutput = standard[1];
    startup.StartupInfo.hStdError = standard[2];
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION information = { 0 };
    HANDLE job = NULL;
    wchar_t *line = ok ? wide(command_line) : NULL;
    if (line) {
        // CREATE_NO_WINDOW: a console program (cmd.exe, ffmpeg, node) opens no console window.
        DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
        ok = CreateProcessW(NULL, line, NULL, NULL, inherited_count > 0, flags, NULL, NULL, &startup.StartupInfo,
                            &information);
        if (!ok) fprintf(stderr, "cannot start %s: error %lu\n", command_line, GetLastError());
    } else {
        ok = false;
    }
    if (ok) {
        job = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = { 0 };
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) ||
            !AssignProcessToJobObject(job, information.hProcess)) {
            fprintf(stderr, "cannot start %s: no job object, error %lu\n", command_line, GetLastError());
            TerminateProcess(information.hProcess, KILLED_EXIT_CODE);
            CloseHandle(information.hProcess);
            if (job) CloseHandle(job);
            ok = false;
        } else {
            ResumeThread(information.hThread);
        }
        CloseHandle(information.hThread);
    }
    free(line);
    if (attributes_ready) DeleteProcThreadAttributeList(attributes);
    free(attributes);
    for (int i = 0; i < 3; i++)
        if (standard[i]) CloseHandle(standard[i]);
    if (!ok) {
        if (own_input >= 0) _close(own_input);
        if (own_output >= 0) _close(own_output);
        return NULL;
    }
    if (input) *input = own_input;
    if (output) *output = own_output;
    struct process *process = malloc(sizeof *process);
    process->process = information.hProcess;
    process->job = job;
    process->status = PROCESS_RUNNING;
    return process;
}

struct process *process_start(const char *const argv[], int *input, int *output) {
    size_t size = 1;
    for (const char *const *argument = argv; *argument; argument++) size += strlen(*argument) * 2 + 3;
    char *line = malloc(size);
    size_t used = 0;
    for (const char *const *argument = argv; *argument; argument++) append_argument(line, &used, *argument);
    line[used] = '\0';
    struct process *process = spawn(line, input, output);
    free(line);
    return process;
}

// cmd.exe /s /c "COMMAND" removes the outer quotes and runs COMMAND unchanged, quotes inside included (the way of
// Node.js). /d skips the AutoRun commands of the registry.
struct process *process_start_shell(const char *command, int *input, int *output) {
    const char *shell = getenv("ComSpec");
    if (!shell || !*shell) shell = "cmd.exe";
    size_t size = strlen(shell) + strlen(command) + 32;
    char *line = malloc(size);
    snprintf(line, size, "\"%s\" /d /s /c \"%s\"", shell, command);
    struct process *process = spawn(line, input, output);
    free(line);
    return process;
}

void process_kill(struct process *process) {
    if (process->status == PROCESS_RUNNING) TerminateJobObject(process->job, KILLED_EXIT_CODE);
}

int process_wait(struct process *process, int timeout_ms) {
    if (process->status != PROCESS_RUNNING) return process->status;
    DWORD timeout = timeout_ms == PROCESS_WAIT_FOREVER ? INFINITE : (DWORD)timeout_ms;
    if (WaitForSingleObject(process->process, timeout) != WAIT_OBJECT_0) return PROCESS_RUNNING;
    DWORD code = KILLED_EXIT_CODE;
    GetExitCodeProcess(process->process, &code);
    process->status = (int)(code & 0x7fffffff);
    return process->status;
}

int process_stop(struct process *process) {
    if (process_wait(process, 0) != PROCESS_RUNNING) return process->status;
    process_kill(process); // TerminateJobObject ends the group at once
    return process_wait(process, PROCESS_WAIT_FOREVER);
}

void process_free(struct process *process) {
    if (!process) return;
    process_stop(process);
    CloseHandle(process->process);
    CloseHandle(process->job); // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE ends the descendants that still run
    free(process);
}

bool process_find_program(const char *name) {
    wchar_t *wide_name = wide(name);
    if (!wide_name) return false;
    wchar_t found[MAX_PATH];
    DWORD length = SearchPathW(NULL, wide_name, L".exe", MAX_PATH, found, NULL);
    free(wide_name);
    return length > 0 && length < MAX_PATH;
}

// ---------------------------------------------------------------- streams

static HANDLE stream_handle(int stream) { return (HANDLE)_get_osfhandle(stream); }

bool stream_read_all(int stream, void *buffer, size_t size) {
    HANDLE handle = stream_handle(stream);
    for (size_t done = 0; done < size;) {
        DWORD count = 0, wanted = (DWORD)(size - done < MAX_IO_BYTES ? size - done : MAX_IO_BYTES);
        if (!ReadFile(handle, (char *)buffer + done, wanted, &count, NULL) || count == 0) return false;
        done += count;
    }
    return true;
}

bool stream_write_all(int stream, const void *buffer, size_t size) {
    HANDLE handle = stream_handle(stream);
    for (size_t done = 0; done < size;) {
        DWORD count = 0, wanted = (DWORD)(size - done < MAX_IO_BYTES ? size - done : MAX_IO_BYTES);
        if (!WriteFile(handle, (const char *)buffer + done, wanted, &count, NULL) || count == 0) return false;
        done += count;
    }
    return true;
}

long stream_read_ready(int stream, void *buffer, size_t size) {
    HANDLE handle = stream_handle(stream);
    DWORD available = 0, count = 0;
    if (!PeekNamedPipe(handle, NULL, 0, NULL, &available, NULL)) return -1;
    if (available == 0) return 0;
    DWORD wanted = (DWORD)(size < available ? size : available);
    if (!ReadFile(handle, buffer, wanted, &count, NULL)) return -1;
    return (long)count;
}

// CancelIoEx also cancels a synchronous ReadFile of another thread of this process. A read that starts after
// the call still waits for data, so the reader must check its own stop condition between reads.
void stream_shutdown(int stream) { CancelIoEx(stream_handle(stream), NULL); }

void stream_close(int stream) { _close(stream); }

void stream_local_address(char *address, size_t size, const char *name) {
    snprintf(address, size, "\\\\.\\pipe\\%s-%lu", name, GetCurrentProcessId());
}

int stream_connect(const char *address) {
    wchar_t *name = wide(address);
    if (!name) return -1;
    HANDLE handle = INVALID_HANDLE_VALUE;
    // All instances of the pipe can be busy with other clients for a moment.
    for (int attempt = 0; attempt < PIPE_CONNECT_ATTEMPTS; attempt++) {
        handle = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (handle != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) break;
        WaitNamedPipeW(name, PIPE_BUSY_WAIT_MS);
    }
    free(name);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    int stream = _open_osfhandle((intptr_t)handle, _O_BINARY);
    if (stream < 0) CloseHandle(handle);
    return stream;
}
