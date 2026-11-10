// Sends a MIDI clock at a known tempo from a sequencer client of its own and checks that the tempo computed
// from the time stamps of src/midi.c stays steady, while the test reads the messages only every 50 ms
// like a slow frame. The own client keeps the clocks of other programs out. Skips without the ALSA sequencer
// (for example in CI).
#include <alsa/asoundlib.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "../src/midi.h"

#define TEMPO_BPM 96.0
#define CLOCKS_PER_BEAT 24
#define BEATS 12
#define CLOCKS (BEATS * CLOCKS_PER_BEAT)
#define POLL_INTERVAL_MS 50
#define MAX_TEMPO_ERROR_BPM 0.2
#define MIDI_CLOCK 0xF8
#define NANOSECONDS_PER_SECOND 1000000000L

static snd_seq_t *sender;
static int sender_client, sender_port;
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

static void receive(int client, int port, double time, const unsigned char *bytes, int length) {
    if (client != sender_client || port != sender_port || length != 1 || bytes[0] != MIDI_CLOCK) return;
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

int main(void) {
    if (access("/dev/snd/seq", R_OK | W_OK) != 0) {
        printf("midi test: skipped, no ALSA sequencer\n");
        return 0;
    }
    // The port exists before midi_start, which connects to every readable port.
    if (snd_seq_open(&sender, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) return 1;
    snd_seq_set_client_name(sender, "hydra-native midi test");
    sender_client = snd_seq_client_id(sender);
    sender_port = snd_seq_create_simple_port(sender, "clock", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                             SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (sender_port < 0 || !midi_start()) return 1;
    send_clock();
    double deadline = monotonic_ms() + (BEATS + 2) * 60000.0 / TEMPO_BPM;
    while (received < CLOCKS && monotonic_ms() < deadline) {
        struct timespec wait = { 0, POLL_INTERVAL_MS * 1000000L };
        nanosleep(&wait, NULL);
        poll_time = monotonic_ms();
        midi_poll(poll_time, receive);
    }
    snd_seq_close(sender);
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
    if (error > MAX_TEMPO_ERROR_BPM || !ordered) {
        printf("midi test: FAIL\n");
        return 1;
    }
    printf("midi test: ok\n");
    return 0;
}
