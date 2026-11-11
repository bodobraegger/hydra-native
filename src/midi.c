// MIDI input and output for navigator.requestMIDIAccess() (js/midi.js) through the ALSA sequencer.
// hydra-native connects its input port to every readable port, also to ports that appear later.
// Output goes directly to the port of a MIDIOutput.
// A queue stamps each input event with its arrival time in the kernel, so that the time stamp of a
// MIDIMessageEvent does not depend on when the frame reads it.
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <string.h>

#include "midi.h"

#define MAX_EVENT_BYTES 64 // of a channel message, and of each part of a system exclusive message that goes out
#define SYSEX_START 0xF0
#define SYSEX_END 0xF7
#define SYSTEM_CLIENT 0

static snd_seq_t *sequencer;
static int own_client, own_port, own_output_port, queue;
static snd_midi_event_t *decoder;
static snd_midi_event_t *encoder;
static bool inputs_changed;
// The sequencer gives a system exclusive message in parts; they collect here until the part that ends with F7.
static unsigned char sysex[MIDI_MAX_MESSAGE_BYTES];
static size_t sysex_length;
static bool sysex_too_long;

static bool has_capabilities(snd_seq_port_info_t *port, unsigned wanted) {
    unsigned capability = snd_seq_port_info_get_capability(port);
    int client = snd_seq_port_info_get_client(port);
    return client != own_client && client != SYSTEM_CLIENT && (capability & wanted) == wanted &&
           !(capability & SND_SEQ_PORT_CAP_NO_EXPORT);
}

static bool is_readable_source(snd_seq_port_info_t *port) {
    return has_capabilities(port, SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ);
}

static bool is_writable_destination(snd_seq_port_info_t *port) {
    return has_capabilities(port, SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);
}

static void connect_all(void) {
    snd_seq_client_info_t *client;
    snd_seq_port_info_t *port;
    snd_seq_client_info_alloca(&client);
    snd_seq_port_info_alloca(&port);
    snd_seq_client_info_set_client(client, -1);
    while (snd_seq_query_next_client(sequencer, client) >= 0) {
        snd_seq_port_info_set_client(port, snd_seq_client_info_get_client(client));
        snd_seq_port_info_set_port(port, -1);
        while (snd_seq_query_next_port(sequencer, port) >= 0)
            if (is_readable_source(port))
                snd_seq_connect_from(sequencer, own_port, snd_seq_port_info_get_client(port),
                                     snd_seq_port_info_get_port(port));
    }
    inputs_changed = true;
}

bool midi_start(void) {
    if (sequencer) return true;
    if (snd_seq_open(&sequencer, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        fprintf(stderr, "midi: cannot open the ALSA sequencer\n");
        sequencer = NULL;
        return false;
    }
    snd_seq_set_client_name(sequencer, "hydra-native");
    own_client = snd_seq_client_id(sequencer);
    own_port = snd_seq_create_simple_port(sequencer, "input", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                          SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    own_output_port = snd_seq_create_simple_port(sequencer, "output", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                                 SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    queue = snd_seq_alloc_named_queue(sequencer, "hydra-native");
    snd_seq_port_info_t *port_info;
    snd_seq_port_info_alloca(&port_info);
    snd_seq_get_port_info(sequencer, own_port, port_info);
    snd_seq_port_info_set_timestamping(port_info, 1);
    snd_seq_port_info_set_timestamp_real(port_info, 1);
    snd_seq_port_info_set_timestamp_queue(port_info, queue);
    snd_seq_set_port_info(sequencer, own_port, port_info);
    snd_seq_start_queue(sequencer, queue, NULL);
    snd_seq_drain_output(sequencer);
    snd_midi_event_new(MAX_EVENT_BYTES, &encoder);
    // The announce port reports new ports, so that devices plugged in later connect too.
    snd_seq_connect_from(sequencer, own_port, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
    snd_midi_event_new(MAX_EVENT_BYTES, &decoder);
    snd_midi_event_no_status(decoder, 1);
    connect_all();
    return true;
}

static int list_ports(struct midi_port *ports, int capacity, bool (*accept)(snd_seq_port_info_t *port)) {
    if (!sequencer) return 0;
    snd_seq_client_info_t *client;
    snd_seq_port_info_t *port;
    snd_seq_client_info_alloca(&client);
    snd_seq_port_info_alloca(&port);
    snd_seq_client_info_set_client(client, -1);
    int count = 0;
    while (count < capacity && snd_seq_query_next_client(sequencer, client) >= 0) {
        snd_seq_port_info_set_client(port, snd_seq_client_info_get_client(client));
        snd_seq_port_info_set_port(port, -1);
        while (count < capacity && snd_seq_query_next_port(sequencer, port) >= 0) {
            if (!accept(port)) continue;
            ports[count].client = snd_seq_port_info_get_client(port);
            ports[count].port = snd_seq_port_info_get_port(port);
            snprintf(ports[count].name, sizeof ports[count].name, "%s", snd_seq_port_info_get_name(port));
            snprintf(ports[count].manufacturer, sizeof ports[count].manufacturer, "%s",
                     snd_seq_client_info_get_name(client));
            count++;
        }
    }
    return count;
}

int midi_inputs(struct midi_port *inputs, int capacity) {
    inputs_changed = false;
    return list_ports(inputs, capacity, is_readable_source);
}

int midi_outputs(struct midi_port *outputs, int capacity) {
    return list_ports(outputs, capacity, is_writable_destination);
}

bool midi_send(int client, int port, const unsigned char *bytes, int length) {
    if (!sequencer) return false;
    snd_midi_event_reset_encode(encoder);
    for (int i = 0; i < length; i++) {
        snd_seq_event_t event;
        snd_seq_ev_clear(&event);
        if (snd_midi_event_encode_byte(encoder, bytes[i], &event) != 1) continue; // the message is not complete yet
        snd_seq_ev_set_source(&event, own_output_port);
        snd_seq_ev_set_dest(&event, client, port);
        snd_seq_ev_set_direct(&event);
        if (snd_seq_event_output_direct(sequencer, &event) < 0) return false;
    }
    return true;
}

bool midi_inputs_changed(void) { return inputs_changed; }

static double real_time_ms(const snd_seq_real_time_t *time) { return time->tv_sec * 1e3 + time->tv_nsec / 1e6; }

// The difference between the clock of `now` and the queue time, read again for each poll so that a drift
// of the queue timer does not add up.
static double queue_offset_ms(double now) {
    snd_seq_queue_status_t *status;
    snd_seq_queue_status_alloca(&status);
    if (snd_seq_get_queue_status(sequencer, queue, status) < 0) return now;
    return now - real_time_ms(snd_seq_queue_status_get_real_time(status));
}

// Adds a part to `sysex`; true when the message is complete and fits.
static bool collect_sysex(const unsigned char *part, size_t length) {
    if (length && part[0] == SYSEX_START) {
        sysex_length = 0;
        sysex_too_long = false;
    }
    if (sysex_length + length > sizeof sysex) sysex_too_long = true;
    else {
        memcpy(sysex + sysex_length, part, length);
        sysex_length += length;
    }
    if (!length || part[length - 1] != SYSEX_END) return false;
    if (sysex_too_long) {
        static bool reported;
        if (!reported) fprintf(stderr, "midi: dropped a message of more than %d bytes\n", MIDI_MAX_MESSAGE_BYTES);
        reported = true;
    }
    return !sysex_too_long;
}

int midi_poll(double now, void (*message)(int client, int port, double time, const unsigned char *bytes, int length)) {
    if (!sequencer) return 0;
    int count = 0;
    double offset = queue_offset_ms(now);
    snd_seq_event_t *event;
    while (snd_seq_event_input(sequencer, &event) >= 0 && event) {
        if (event->type == SND_SEQ_EVENT_PORT_START) {
            connect_all();
            continue;
        }
        if (event->type == SND_SEQ_EVENT_PORT_EXIT) {
            inputs_changed = true;
            continue;
        }
        unsigned char channel_message[MAX_EVENT_BYTES];
        const unsigned char *bytes = channel_message;
        long length;
        if (event->type == SND_SEQ_EVENT_SYSEX) {
            if (!collect_sysex(event->data.ext.ptr, event->data.ext.len)) continue;
            bytes = sysex;
            length = (long)sysex_length;
        } else {
            length = snd_midi_event_decode(decoder, channel_message, sizeof channel_message, event);
        }
        if (length > 0) {
            bool stamped = (event->flags & SND_SEQ_TIME_STAMP_MASK) == SND_SEQ_TIME_STAMP_REAL;
            double time = stamped ? offset + real_time_ms(&event->time.time) : now;
            message(event->source.client, event->source.port, time, bytes, (int)length);
            count++;
        }
    }
    return count;
}
