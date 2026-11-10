#pragma once

#include <stdbool.h>

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
bool midi_inputs_changed(void); // ports appeared or went away // since the last midi_inputs() call
// Calls `message` for each MIDI message that arrived since the last call; returns their count.
int midi_poll(void (*message)(int client, int port, const unsigned char *bytes, int length));
