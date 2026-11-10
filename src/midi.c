// MIDI input and output for navigator.requestMIDIAccess() (js/midi.js) through the ALSA sequencer.
// hydra-native connects its input port to every readable port, also to ports that appear later.
// Output goes directly to the port of a MIDIOutput.
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <string.h>

#include "midi.h"

#define MAX_MESSAGE_BYTES 64
#define MAX_PORT_NAME 128
#define SYSTEM_CLIENT 0

static snd_seq_t *sequencer;
static int own_client, own_port, own_output_port;
static snd_midi_event_t *decoder;
static snd_midi_event_t *encoder;
static bool inputs_changed;

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
    snd_midi_event_new(MAX_MESSAGE_BYTES, &encoder);
    // The announce port reports new ports, so that devices plugged in later connect too.
    snd_seq_connect_from(sequencer, own_port, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
    snd_midi_event_new(MAX_MESSAGE_BYTES, &decoder);
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

int midi_poll(void (*message)(int client, int port, const unsigned char *bytes, int length)) {
    if (!sequencer) return 0;
    int count = 0;
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
        unsigned char bytes[MAX_MESSAGE_BYTES];
        long length = snd_midi_event_decode(decoder, bytes, sizeof bytes, event);
        if (length > 0) {
            message(event->source.client, event->source.port, bytes, (int)length);
            count++;
        }
    }
    return count;
}
