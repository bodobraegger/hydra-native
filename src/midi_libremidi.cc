// MIDI input and output of macOS and Windows (src/midi.h) through libremidi: CoreMIDI and WinMM. Like src/midi.c,
// it opens every input, also inputs that appear later. The input callbacks run on threads of the system and store
// each message with its arrival time on std::chrono::steady_clock; midi_poll() gives the messages to the program in
// the clock of `now`. The port lists change only in midi_poll(), on the thread of the program.
// The ids are indexes into the lists below, with client 0 for inputs and 1 for outputs. A port that goes away keeps
// its index, and a port that comes back gets a new one.
#include <libremidi/libremidi.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include "midi.h"
}

namespace {

constexpr int INPUT_CLIENT = 0;
constexpr int OUTPUT_CLIENT = 1;
constexpr const char *PORT_NAME = "hydra-native";
// A time stamp of the system that is older than this, or in the future, counts as wrong (CoreMIDI sends 0 for
// "now"); the message then gets the time of its callback.
constexpr int64_t MAX_LAG_NS = 1'000'000'000;
constexpr unsigned char SYSEX_START = 0xF0, SYSEX_END = 0xF7;

int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct received {
    int port;
    int64_t arrival_ns; // on steady_clock
    libremidi::midi_bytes bytes;
};

struct input {
    libremidi::input_port info;
    // Lives until the program ends, also after the port went away, because CoreMIDI and WinMM do not promise that
    // no callback runs after the port is closed.
    std::unique_ptr<libremidi::midi_in> in;
    bool present = true;
};

struct output {
    libremidi::output_port info;
    std::unique_ptr<libremidi::midi_out> out; // opened on the first message
    bool present = true;
};

std::unique_ptr<libremidi::observer> observer;
std::vector<std::unique_ptr<input>> inputs; // the callbacks keep a pointer to their input
std::vector<output> outputs;
std::atomic<bool> ports_changed;
bool inputs_changed;
std::mutex lock;
std::vector<received> messages; // under `lock`

bool same_port(const libremidi::port_information &a, const libremidi::port_information &b) {
    return a.port == b.port && a.port_name == b.port_name;
}

void open_input(const libremidi::input_port &info) {
    int index = (int)inputs.size();
    input *slot = inputs.emplace_back(std::make_unique<input>()).get();
    slot->info = info;
    libremidi::input_configuration config;
    // As src/midi.c: clock and active sensing pass. System exclusive messages do not, as in Web MIDI without
    // the sysex permission.
    config.ignore_timing = false;
    config.ignore_sensing = false;
    config.timestamps = libremidi::timestamp_mode::Absolute;
    config.on_message = [slot, index](libremidi::message &&message) {
        int64_t now = steady_ns();
        int64_t lag = slot->in->absolute_timestamp() - message.timestamp;
        if (message.timestamp <= 0 || lag < 0 || lag > MAX_LAG_NS) lag = 0;
        std::lock_guard guard(lock);
        messages.push_back({ index, now - lag, std::move(message.bytes) });
    };
    try {
        slot->in = std::make_unique<libremidi::midi_in>(config, libremidi::midi_in_configuration_for(*observer));
        if (!slot->in->open_port(info, PORT_NAME).is_set()) return;
    } catch (const std::exception &) {
        slot->in.reset();
    }
    fprintf(stderr, "midi: cannot open the input %s\n", info.display_name.c_str());
}

// Opens the new inputs and marks the ports that went away.
void update_ports() {
    auto current_inputs = observer->get_input_ports();
    for (auto &slot : inputs) {
        if (!slot->present) continue;
        slot->present = std::ranges::any_of(current_inputs, [&](auto &port) { return same_port(port, slot->info); });
        if (!slot->present && slot->in) slot->in->close_port();
    }
    for (auto &port : current_inputs)
        if (std::ranges::none_of(inputs, [&](auto &slot) { return slot->present && same_port(slot->info, port); }))
            open_input(port);
    auto current_outputs = observer->get_output_ports();
    for (auto &slot : outputs) {
        if (!slot.present) continue;
        slot.present = std::ranges::any_of(current_outputs, [&](auto &port) { return same_port(port, slot.info); });
        if (!slot.present) slot.out.reset();
    }
    for (auto &port : current_outputs)
        if (std::ranges::none_of(outputs, [&](auto &slot) { return slot.present && same_port(slot.info, port); }))
            outputs.push_back({ port, nullptr, true });
    inputs_changed = true;
}

void fill(midi_port &entry, int client, int port, const libremidi::port_information &info) {
    entry.client = client;
    entry.port = port;
    snprintf(entry.name, sizeof entry.name, "%s", (info.display_name.empty() ? info.port_name : info.display_name).c_str());
    snprintf(entry.manufacturer, sizeof entry.manufacturer, "%s", info.manufacturer.c_str());
}

// The length of the MIDI message that starts with `status`; 0 for a system exclusive message, which ends with
// SYSEX_END.
int message_length(unsigned char status) {
    if (status < 0xF0) return (status & 0xE0) == 0xC0 ? 2 : 3; // program change and channel pressure have 2
    switch (status) {
    case SYSEX_START: return 0;
    case 0xF1: case 0xF3: return 2; // time code quarter frame, song select
    case 0xF2: return 3;            // song position
    default: return 1;
    }
}

} // namespace

bool midi_start(void) {
    if (observer) return true;
    libremidi::observer_configuration config;
    config.track_hardware = config.track_virtual = config.track_any = true; // as src/midi.c, every port
    config.notify_in_constructor = false;
    config.input_added = config.input_removed = [](const libremidi::input_port &) { ports_changed = true; };
    config.output_added = config.output_removed = [](const libremidi::output_port &) { ports_changed = true; };
    try {
        observer = std::make_unique<libremidi::observer>(
            config, libremidi::observer_configuration_for(libremidi::midi1::default_api()));
    } catch (const std::exception &) {
        observer.reset();
    }
    if (!observer || observer->get_current_api() == libremidi::API::DUMMY) {
        fprintf(stderr, "midi: no MIDI system\n");
        observer.reset();
        return false;
    }
    update_ports();
    return true;
}

static int list_ports(struct midi_port *ports, int capacity, bool inputs_list) {
    int count = 0;
    if (inputs_list) {
        for (int i = 0; i < (int)inputs.size() && count < capacity; i++)
            if (inputs[i]->present) fill(ports[count++], INPUT_CLIENT, i, inputs[i]->info);
    } else {
        for (int i = 0; i < (int)outputs.size() && count < capacity; i++)
            if (outputs[i].present) fill(ports[count++], OUTPUT_CLIENT, i, outputs[i].info);
    }
    return count;
}

int midi_inputs(struct midi_port *ports, int capacity) {
    inputs_changed = false;
    return list_ports(ports, capacity, true);
}

int midi_outputs(struct midi_port *ports, int capacity) { return list_ports(ports, capacity, false); }

bool midi_send(int client, int port, const unsigned char *bytes, int length) {
    if (client != OUTPUT_CLIENT || port < 0 || port >= (int)outputs.size() || !outputs[port].present) return false;
    output &target = outputs[port];
    if (!target.out) {
        try {
            auto out = std::make_unique<libremidi::midi_out>(libremidi::output_configuration{},
                                                             libremidi::midi_out_configuration_for(*observer));
            if (out->open_port(target.info, PORT_NAME).is_set()) return false;
            target.out = std::move(out);
        } catch (const std::exception &) {
            return false;
        }
    }
    // libremidi sends one message per call (WinMM packs a short message into one word).
    for (int start = 0; start < length;) {
        int size = message_length(bytes[start]);
        if (size == 0) {
            const unsigned char *end = std::find(bytes + start, bytes + length, SYSEX_END);
            size = (int)(end - (bytes + start)) + (end < bytes + length ? 1 : 0);
        }
        size = std::min(size, length - start);
        if (target.out->send_message(bytes + start, (size_t)size).is_set()) return false;
        start += size;
    }
    return true;
}

bool midi_inputs_changed(void) { return inputs_changed; }

int midi_poll(double now, void (*message)(int client, int port, double time, const unsigned char *bytes, int length)) {
    if (!observer) return 0;
    if (ports_changed.exchange(false)) update_ports();
    std::vector<received> batch;
    {
        std::lock_guard guard(lock);
        batch.swap(messages);
    }
    int64_t steady_now = steady_ns();
    for (auto &item : batch)
        message(INPUT_CLIENT, item.port, now - (double)(steady_now - item.arrival_ns) / 1e6, item.bytes.data(),
                (int)item.bytes.size());
    return (int)batch.size();
}
