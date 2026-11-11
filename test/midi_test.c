// Sends a MIDI clock at a known tempo from a sequencer client of its own and checks that the tempo computed
// from the time stamps of src/midi.h stays steady, while the test reads the messages only every 50 ms
// like a slow frame. The own client keeps the clocks of other programs out. The test finds its port in the
// lists of src/midi.h, so it also runs with src/midi_libremidi.cc, and it checks that midi_send() sends two
// messages of one call to that port. Skips without the ALSA sequencer (for example in CI).
#include <alsa/asoundlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/midi.h"

#define TEMPO_BPM 96.0
#define CLOCKS_PER_BEAT 24
#define BEATS 12
#define CLOCKS (BEATS * CLOCKS_PER_BEAT)
#define POLL_INTERVAL_MS 50
#define MAX_TEMPO_ERROR_BPM 0.2
#define MIDI_CLOCK 0xF8
#define NANOSECONDS_PER_SECOND 1000000000L
#define MAX_PORTS 64
#define OUTPUT_TIMEOUT_MS 1000
#define SYSEX_BYTES 300 // more than one event of the sequencer holds

static snd_seq_t *sender;
static int sender_port;
static struct midi_port input, output; // the port of the sender as src/midi.h lists it
static double clock_times[CLOCKS];
static double poll_times[CLOCKS];
static int received;
static double poll_time;

// The queue of the sender schedules every clock in the kernel, so that the load of the machine does not move
// the clocks; the test then measures only the time stamps of src/midi.c.
static void send_clock(void) {
    int queue = snd_seq_alloc_queue(sender);
    snd_seq_start_queue(sender, queue, NULL);
    snd_seq_drain_output(sender);
    long interval = (long)(60.0 / TEMPO_BPM / CLOCKS_PER_BEAT * NANOSECONDS_PER_SECOND);
    for (int i = 0; i < CLOCKS; i++) {
        long nanoseconds = (long)(i + 1) * interval;
        snd_seq_real_time_t time = { (unsigned)(nanoseconds / NANOSECONDS_PER_SECOND),
                                     (unsigned)(nanoseconds % NANOSECONDS_PER_SECOND) };
        snd_seq_event_t event;
        snd_seq_ev_clear(&event);
        event.type = SND_SEQ_EVENT_CLOCK;
        snd_seq_ev_set_source(&event, sender_port);
        snd_seq_ev_set_subs(&event);
        snd_seq_ev_schedule_real(&event, queue, 0, &time);
        snd_seq_event_output(sender, &event);
    }
    snd_seq_drain_output(sender);
}

static bool find_port(int (*list)(struct midi_port *ports, int capacity), const char *name, struct midi_port *found) {
    struct midi_port ports[MAX_PORTS];
    int count = list(ports, MAX_PORTS);
    for (int i = 0; i < count; i++) {
        if (strcmp(ports[i].name, name) != 0) continue;
        *found = ports[i];
        return true;
    }
    return false;
}

static int received_sysex_bytes;

static void receive(int client, int port, double time, const unsigned char *bytes, int length) {
    if (client == input.client && port == input.port && length == SYSEX_BYTES && bytes[0] == 0xF0 &&
        bytes[length - 1] == 0xF7)
        received_sysex_bytes = length;
    if (client != input.client || port != input.port || length != 1 || bytes[0] != MIDI_CLOCK) return;
    if (received == CLOCKS) return;
    poll_times[received] = poll_time;
    clock_times[received++] = time;
}

static double monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1e3 + now.tv_nsec / 1e6;
}

// The largest difference from TEMPO_BPM of the tempo of each beat.
static double tempo_error(const double *times, const char *label) {
    double largest = 0;
    printf("%s:", label);
    for (int beat = 0; beat + 1 < BEATS; beat++) {
        double beat_ms = times[(beat + 1) * CLOCKS_PER_BEAT] - times[beat * CLOCKS_PER_BEAT];
        double tempo = 60000.0 / beat_ms;
        printf(" %.2f", tempo);
        largest = fmax(largest, fabs(tempo - TEMPO_BPM));
    }
    printf(" BPM\n");
    return largest;
}

// midi_send() with a note on and a note off in one call; the client of the test receives both.
static bool check_output(void) {
    const unsigned char notes[] = { 0x90, 60, 100, 0x80, 60, 0 };
    if (!midi_send(output.client, output.port, notes, sizeof notes)) return false;
    snd_seq_nonblock(sender, 1);
    int received_notes = 0;
    double deadline = monotonic_ms() + OUTPUT_TIMEOUT_MS;
    while (received_notes < 2 && monotonic_ms() < deadline) {
        snd_seq_event_t *event;
        if (snd_seq_event_input(sender, &event) < 0 || !event) {
            struct timespec wait = { 0, 1000000L };
            nanosleep(&wait, NULL);
            continue;
        }
        int expected = received_notes == 0 ? SND_SEQ_EVENT_NOTEON : SND_SEQ_EVENT_NOTEOFF;
        if (event->type == expected && event->data.note.note == 60) received_notes++;
    }
    printf("midi test: midi_send gave %d of 2 notes\n", received_notes);
    return received_notes == 2;
}

// A system exclusive message of SYSEX_BYTES, in both directions: the sequencer carries it in parts, src/midi.h
// gives and takes it as one message.
static bool check_sysex(void) {
    unsigned char message[SYSEX_BYTES];
    memset(message, 0x11, sizeof message);
    message[0] = 0xF0;
    message[SYSEX_BYTES - 1] = 0xF7;
    snd_seq_nonblock(sender, 1);
    if (!midi_send(output.client, output.port, message, sizeof message)) return false;
    int sent_bytes = 0;
    double deadline = monotonic_ms() + OUTPUT_TIMEOUT_MS;
    snd_seq_event_t *event;
    while (sent_bytes < SYSEX_BYTES && monotonic_ms() < deadline)
        if (snd_seq_event_input(sender, &event) >= 0 && event && event->type == SND_SEQ_EVENT_SYSEX)
            sent_bytes += (int)event->data.ext.len;

    snd_seq_event_t outgoing;
    snd_seq_ev_clear(&outgoing);
    snd_seq_ev_set_source(&outgoing, sender_port);
    snd_seq_ev_set_subs(&outgoing);
    snd_seq_ev_set_direct(&outgoing);
    snd_seq_ev_set_sysex(&outgoing, sizeof message, message);
    snd_seq_event_output_direct(sender, &outgoing);
    while (!received_sysex_bytes && monotonic_ms() < deadline + OUTPUT_TIMEOUT_MS) {
        struct timespec wait = { 0, 1000000L };
        nanosleep(&wait, NULL);
        midi_poll(monotonic_ms(), receive);
    }
    printf("midi test: system exclusive, %d of %d bytes sent, %d received\n", sent_bytes, SYSEX_BYTES,
           received_sysex_bytes);
    return sent_bytes == SYSEX_BYTES && received_sysex_bytes == SYSEX_BYTES;
}

int main(void) {
    if (access("/dev/snd/seq", R_OK | W_OK) != 0) {
        printf("midi test: skipped, no ALSA sequencer\n");
        return 0;
    }
    // The port exists before midi_start, which connects to every readable port. It is also writable, for
    // check_output(), and its name is unique, so that parallel runs find their own port.
    if (snd_seq_open(&sender, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) return 1;
    snd_seq_set_client_name(sender, "hydra-native midi test");
    char name[64];
    snprintf(name, sizeof name, "clock %d", (int)getpid());
    sender_port = snd_seq_create_simple_port(sender, name,
                                             SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
                                                 SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                             SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (sender_port < 0 || !midi_start()) return 1;
    if (!find_port(midi_inputs, name, &input) || !find_port(midi_outputs, name, &output)) {
        printf("midi test: FAIL, the port \"%s\" is not in the inputs and outputs\n", name);
        return 1;
    }
    send_clock();
    double deadline = monotonic_ms() + (BEATS + 2) * 60000.0 / TEMPO_BPM;
    while (received < CLOCKS && monotonic_ms() < deadline) {
        struct timespec wait = { 0, POLL_INTERVAL_MS * 1000000L };
        nanosleep(&wait, NULL);
        poll_time = monotonic_ms();
        midi_poll(poll_time, receive);
    }
    if (received < CLOCKS) {
        printf("midi test: FAIL, received %d of %d clocks\n", received, CLOCKS);
        return 1;
    }
    double latest_lag = 0;
    for (int i = 0; i < CLOCKS; i++) latest_lag = fmax(latest_lag, poll_times[i] - clock_times[i]);
    tempo_error(poll_times, "tempo from the poll time ");
    double error = tempo_error(clock_times, "tempo from the time stamp");
    bool ordered = latest_lag >= 0 && latest_lag <= POLL_INTERVAL_MS * 2;
    printf("midi test: largest error %.3f BPM (limit %.1f), largest lag of a poll %.1f ms\n", error,
           MAX_TEMPO_ERROR_BPM, latest_lag);
    bool sent = check_output() && check_sysex();
    snd_seq_close(sender);
    if (error > MAX_TEMPO_ERROR_BPM || !ordered || !sent) {
        printf("midi test: FAIL\n");
        return 1;
    }
    printf("midi test: ok\n");
    return 0;
}
