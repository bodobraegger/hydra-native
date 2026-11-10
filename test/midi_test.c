// Sends a MIDI clock at a known tempo through the "Midi Through" port and checks that the tempo computed
// from the time stamps of src/midi.c stays steady, while the test reads the messages only every 50 ms
// like a slow frame. Skips when the ALSA sequencer or the Midi Through port is not there (for example in CI).
#include <alsa/asoundlib.h>
#include <math.h>
#include <pthread.h>
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

static int through_client = -1, through_port;
static double clock_times[CLOCKS];
static double poll_times[CLOCKS];
static int received;
static double poll_time;

static bool find_through_port(snd_seq_t *sequencer) {
    snd_seq_client_info_t *client;
    snd_seq_port_info_t *port;
    snd_seq_client_info_alloca(&client);
    snd_seq_port_info_alloca(&port);
    snd_seq_client_info_set_client(client, -1);
    while (snd_seq_query_next_client(sequencer, client) >= 0) {
        if (strcmp(snd_seq_client_info_get_name(client), "Midi Through") != 0) continue;
        through_client = snd_seq_client_info_get_client(client);
        snd_seq_port_info_set_client(port, through_client);
        snd_seq_port_info_set_port(port, -1);
        if (snd_seq_query_next_port(sequencer, port) < 0) return false;
        through_port = snd_seq_port_info_get_port(port);
        return true;
    }
    return false;
}

static void *send_clock(void *argument) {
    long interval = (long)(60.0 / TEMPO_BPM / CLOCKS_PER_BEAT * NANOSECONDS_PER_SECOND);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    unsigned char clock = MIDI_CLOCK;
    for (int i = 0; i < CLOCKS; i++) {
        next.tv_nsec += interval;
        if (next.tv_nsec >= NANOSECONDS_PER_SECOND) {
            next.tv_nsec -= NANOSECONDS_PER_SECOND;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        midi_send(through_client, through_port, &clock, 1);
    }
    return NULL;
}

static void receive(int client, int port, double time, const unsigned char *bytes, int length) {
    if (client != through_client || port != through_port || length != 1 || bytes[0] != MIDI_CLOCK) return;
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
    if (!midi_start()) return 1;
    snd_seq_t *query;
    if (snd_seq_open(&query, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) return 1;
    bool found = find_through_port(query);
    snd_seq_close(query);
    if (!found) {
        printf("midi test: skipped, no Midi Through port (modprobe snd-seq-dummy)\n");
        return 0;
    }
    pthread_t sender;
    pthread_create(&sender, NULL, send_clock, NULL);
    double deadline = monotonic_ms() + (BEATS + 2) * 60000.0 / TEMPO_BPM;
    while (received < CLOCKS && monotonic_ms() < deadline) {
        struct timespec wait = { 0, POLL_INTERVAL_MS * 1000000L };
        nanosleep(&wait, NULL);
        poll_time = monotonic_ms();
        midi_poll(receive);
    }
    pthread_join(sender, NULL);
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
