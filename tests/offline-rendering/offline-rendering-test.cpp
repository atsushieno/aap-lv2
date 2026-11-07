// Runs the actual LV2 bridge and lilv against offline-probe.lv2, through the AAP plugin API.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>
#include <aap/android-audio-plugin.h>
#include <aap/ext/midi.h>
#include <aap/ext/offline-rendering.h>
#include <aap/ext/parameters.h>
#include <aap/ext/plugin-info.h>
#include <aap/ext/state.h>

extern "C" AndroidAudioPluginFactory* GetAndroidAudioPluginFactoryLV2Bridge();

namespace {
constexpr int32_t FRAMES = 64, MIDI_SIZE = 4096;
constexpr int32_t GAIN_PORT = 0, FREE_WHEEL_PORT = 1;
constexpr int32_t AUDIO_OUT = 0, MIDI_IN = 1, MIDI_OUT = 2, PORTS = 3;
constexpr auto REALTIME = AAP_OFFLINE_RENDERING_MODE_REALTIME;
constexpr auto OFFLINE = AAP_OFFLINE_RENDERING_MODE_OFFLINE;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<float> audio(FRAMES);
std::vector<uint8_t> midiIn(MIDI_SIZE), midiOut(MIDI_SIZE);

aap_port_direction portDirection(aap_plugin_info_port_t* p) {
    return (intptr_t) p->context == MIDI_IN ? AAP_PORT_DIRECTION_INPUT : AAP_PORT_DIRECTION_OUTPUT;
}
aap_content_type portContent(aap_plugin_info_port_t* p) {
    return (intptr_t) p->context == AUDIO_OUT ? AAP_CONTENT_TYPE_AUDIO : AAP_CONTENT_TYPE_MIDI2;
}
aap_plugin_info_t getPluginInfo(aap_host_plugin_info_extension_t*, AndroidAudioPluginHost*, const char*) {
    aap_plugin_info_t info{};
    info.get_port_count = [](aap_plugin_info_t*) -> uint32_t { return PORTS; };
    info.get_port = [](aap_plugin_info_t*, uint32_t index) {
        aap_plugin_info_port_t port{};
        port.context = (void*) (intptr_t) index;
        port.content_type = portContent;
        port.direction = portDirection;
        return port;
    };
    return info;
}
aap_host_plugin_info_extension_t pluginInfoExtension{getPluginInfo};
AndroidAudioPluginHost host{nullptr, [](AndroidAudioPluginHost*, const char* uri) -> void* {
    return strcmp(uri, AAP_PLUGIN_INFO_EXTENSION_URI) ? nullptr : &pluginInfoExtension;
}, [](AndroidAudioPluginHost*) {}};

aap_buffer_t makeBuffer() {
    aap_buffer_t buffer{};
    buffer.num_frames = [](aap_buffer_t*) -> int32_t { return FRAMES; };
    buffer.num_ports = [](aap_buffer_t*) -> int32_t { return PORTS; };
    buffer.get_buffer = [](aap_buffer_t*, int32_t index) -> void* {
        return index == AUDIO_OUT ? (void*) audio.data() : index == MIDI_IN ? (void*) midiIn.data() : (void*) midiOut.data();
    };
    buffer.get_buffer_size = [](aap_buffer_t*, int32_t index) -> int32_t {
        return index == AUDIO_OUT ? FRAMES * (int32_t) sizeof(float) : MIDI_SIZE;
    };
    return buffer;
}

struct Observed { float freeWheel, gain; int32_t response; bool ranInline; };

struct Fixture {
    AndroidAudioPlugin* plugin;
    aap_buffer_t buffer{makeBuffer()};
    aap_offline_rendering_extension_t* ext;

    Observed process() {
        plugin->process(plugin, &buffer, FRAMES, 0);
        return {audio[0], audio[1], (int32_t) audio[2], audio[3] != 0};
    }
    void setMode(aap_offline_rendering_mode_t mode) {
        check(ext->set_offline_mode(ext, plugin, mode) == AAP_OFFLINE_RENDERING_RESULT_OK, "set_offline_mode succeeds");
    }
    void sendParameter(int32_t id, double plain) {
        auto header = (AAPMidiBufferHeader*) midiIn.data();
        auto ump = (uint32_t*) (header + 1);
        aapMidi2ParameterSysex8(ump, ump + 1, ump + 2, ump + 3, 0, 0, 0, 0, (uint16_t) id,
                                aapParameterPlainToTransportUint32(0.0, 1.0, plain));
        header->length = 16;
    }
    void clearParameters() { ((AAPMidiBufferHeader*) midiIn.data())->length = 0; }
    // Whether the plugin output carried a parameter change for the ID.
    bool emittedParameter(int32_t id) {
        auto header = (AAPMidiBufferHeader*) midiOut.data();
        auto words = (uint32_t*) (header + 1);
        for (uint32_t offset = 0; offset + 16 <= header->length; offset += 16)
            if ((words[offset / 4 + 2] & 0xFFFF) == (uint32_t) id) return true;
        return false;
    }
};

void parameterList(Fixture& f) {
    auto parameters = (aap_parameters_extension_t*) f.plugin->get_extension(f.plugin, AAP_PARAMETERS_EXTENSION_URI);
    check(parameters->get_parameter_count(parameters, f.plugin) == 1, "free-wheeling port is not a parameter");
    check(parameters->get_parameter(parameters, f.plugin, 0).stable_id == GAIN_PORT, "the other control port still is");
}

void modeBeforePrepare(Fixture& f) {
    f.setMode(OFFLINE);
    f.plugin->prepare(f.plugin, 48000, &f.buffer);
    f.plugin->activate(f.plugin);
    check(f.process().freeWheel == 1.0f, "a mode set before prepare() reaches the port");
    f.plugin->deactivate(f.plugin);

    f.setMode(REALTIME);
    f.plugin->prepare(f.plugin, 48000, &f.buffer);
    f.plugin->activate(f.plugin);
    check(f.process().freeWheel == 0.0f, "preparing again keeps the mode");
    f.plugin->deactivate(f.plugin);
}

// Returns the number of blocks that the plugin has run so far.
int32_t realtimeWorkerIsAsynchronous(Fixture& f, int32_t blocksSoFar) {
    f.plugin->activate(f.plugin);
    Observed last{};
    for (int i = 0; i < 5; i++) {
        last = f.process();
        blocksSoFar++;
        check(last.freeWheel == 0.0f, "realtime blocks see 0");
    }
    check(last.response < blocksSoFar - 1, "realtime results arrive late, which is why offline must not do this");

    // Let the worker thread finish, to see where that work ran.
    usleep(300000);
    last = f.process();
    blocksSoFar++;
    check(!last.ranInline, "realtime work runs on the worker thread");

    // Leave requests in flight, for the switch to offline mode.
    for (int i = 0; i < 3; i++) {
        f.process();
        blocksSoFar++;
    }
    f.plugin->deactivate(f.plugin);
    return blocksSoFar;
}

int32_t offlineWorkerIsRepeatable(Fixture& f, int32_t blocksSoFar) {
    f.setMode(OFFLINE);
    f.plugin->activate(f.plugin);
    auto first = f.process();
    check(first.freeWheel == 1.0f, "a mode set after prepare() reaches the port");
    check(first.response == blocksSoFar, "the first offline block waits for realtime requests");
    blocksSoFar++;
    for (int i = 0; i < 10; i++) {
        auto observed = f.process();
        check(observed.response == blocksSoFar, "each offline block has the result of the previous one, block " + std::to_string(blocksSoFar));
        check(observed.ranInline, "offline work runs inside run()");
        blocksSoFar++;
    }
    return blocksSoFar;
}

int32_t parametersCannotChangeTheMode(Fixture& f, int32_t blocksSoFar) {
    f.sendParameter(FREE_WHEEL_PORT, 0.0);
    auto observed = f.process();
    blocksSoFar++;
    check(observed.freeWheel == 1.0f, "a parameter change does not leave offline mode");
    f.sendParameter(GAIN_PORT, 0.25);
    observed = f.process();
    blocksSoFar++;
    f.clearParameters();
    check(observed.gain == 0.25f, "the parameter path itself works");
    check(!f.emittedParameter(FREE_WHEEL_PORT), "the port is not reported as a parameter change");
    f.plugin->deactivate(f.plugin);
    return blocksSoFar;
}

int32_t stateCannotChangeTheMode(Fixture& f, int32_t blocksSoFar) {
    auto state = (aap_state_extension_t*) f.plugin->get_extension(f.plugin, AAP_STATE_EXTENSION_URI);
    aap_state_t output{};
    state->get_state(state, f.plugin, &output);
    std::string turtle((const char*) output.data, output.data_size);
    free(output.data);
    check(turtle.find("\"gain\"") != std::string::npos, "state has the other control port: " + turtle);
    check(turtle.find("free_wheel") == std::string::npos, "state does not capture the mode: " + turtle);

    // A state from elsewhere can have the port (e.g. saved by another LV2 host), and a new gain.
    auto gain = turtle.find("\"gain\"");
    auto value = turtle.find("0.25", gain);
    check(value != std::string::npos, "state has the current gain: " + turtle);
    turtle.replace(value, 4, "0.75");
    auto position = turtle.find("lv2:port [");
    check(position != std::string::npos, "state lists ports: " + turtle);
    turtle.replace(position, strlen("lv2:port ["), "lv2:port [ lv2:symbol \"free_wheel\" ; <http://lv2plug.in/ns/ext/presets#value> 0.0 ] , [");
    aap_state_t input{turtle.data(), turtle.size()};
    state->set_state(state, f.plugin, &input);

    f.plugin->activate(f.plugin);
    auto observed = f.process();
    blocksSoFar++;
    check(observed.gain == 0.75f, "the state was applied");
    check(observed.freeWheel == 1.0f, "a state does not leave offline mode");
    f.plugin->deactivate(f.plugin);
    return blocksSoFar;
}

void backToRealtime(Fixture& f) {
    f.setMode(REALTIME);
    f.plugin->activate(f.plugin);
    Observed observed{};
    for (int i = 0; i < 3; i++) observed = f.process();
    check(observed.freeWheel == 0.0f, "back to realtime");
    f.plugin->deactivate(f.plugin);
}
}

int main(int argc, char** argv) {
    setenv("LV2_PATH", argv[1], 1);
    auto factory = GetAndroidAudioPluginFactoryLV2Bridge();
    auto plugin = factory->instantiate(factory, "lv2:urn:aap-test:offline-probe", &host);
    try {
        check(plugin != nullptr, "the probe is instantiated");
        auto ext = (aap_offline_rendering_extension_t*) plugin->get_extension(plugin, AAP_OFFLINE_RENDERING_EXTENSION_URI);
        check(ext && ext->set_offline_mode, "the bridge provides the extension");
        Fixture f{plugin, makeBuffer(), ext};
        parameterList(f);
        modeBeforePrepare(f);
        int32_t blocks = 2;
        blocks = realtimeWorkerIsAsynchronous(f, blocks);
        blocks = offlineWorkerIsRepeatable(f, blocks);
        blocks = parametersCannotChangeTheMode(f, blocks);
        blocks = stateCannotChangeTheMode(f, blocks);
        backToRealtime(f);
        factory->release(factory, plugin);
    } catch (const std::exception& e) {
        fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
    puts("PASS: mode reaches the port, worker is repeatable offline, parameters and state cannot change the mode");
    return 0;
}
