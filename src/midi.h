// MIDI input and output for navigator.requestMIDIAccess() (js/midi.js): the ALSA sequencer on Linux (src/midi.c),
// libremidi on macOS and Windows (src/midi_libremidi.cc).
#pragma once

#include <stdbool.h>

// The longest message that midi_send() takes and midi_poll() gives: a system exclusive message with its F0 and F7.
// A longer one that arrives is dropped, with one line on stderr.
#define MIDI_MAX_MESSAGE_BYTES 65536

// `client` and `port` identify a port for midi_send() and midi_poll().
struct midi_port {
    int client;
    int port;
    char name[128];
    char manufacturer[128];
};

bool midi_start(void);
int midi_inputs(struct midi_port *inputs, int capacity);
int midi_outputs(struct midi_port *outputs, int capacity);
bool midi_send(int client, int port, const unsigned char *bytes, int length);
bool midi_inputs_changed(void); // ports appeared or went away since the last midi_inputs() call
// Calls `message` for each MIDI message that arrived since the last call, with its arrival time in the clock of
// `now` (milliseconds, the time of performance.now() in the program); returns their count.
int midi_poll(double now, void (*message)(int client, int port, double time, const unsigned char *bytes, int length));
