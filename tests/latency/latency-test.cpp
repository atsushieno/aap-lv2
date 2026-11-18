// Runs the actual LV2 bridge and lilv against latency-probe.lv2, through the AAP plugin API.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <aap/android-audio-plugin.h>
#include <aap/ext/latency.h>
#include <aap/ext/midi.h>
#include <aap/ext/parameters.h>
#include <aap/ext/plugin-info.h>

extern "C" AndroidAudioPluginFactory* GetAndroidAudioPluginFactoryLV2Bridge();

namespace {
constexpr int32_t FRAMES = 64, MIDI_SIZE = 4096;
constexpr int32_t DELAY_PORT = 0;
constexpr int32_t AUDIO_OUT = 0, MIDI_IN = 1, MIDI_OUT = 2, PORTS = 3;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<float> audio(FRAMES);
std::vector<uint8_t> midiIn(MIDI_SIZE), midiOut(MIDI_SIZE);
std::atomic<int> notifications{0};
std::atomic<bool> notifiedOnProcessThread{false};
std::thread::id processThread;

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
aap_latency_host_extension_t latencyHostExtension{nullptr, [](aap_latency_host_extension_t*, AndroidAudioPluginHost*) {
    if (std::this_thread::get_id() == processThread) notifiedOnProcessThread = true;
    notifications++;
}};
AndroidAudioPluginHost host{nullptr, [](AndroidAudioPluginHost*, const char* uri) -> void* {
    if (!strcmp(uri, AAP_PLUGIN_INFO_EXTENSION_URI)) return &pluginInfoExtension;
    if (!strcmp(uri, AAP_LATENCY_EXTENSION_URI)) return &latencyHostExtension;
    return nullptr;
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

// Waits for the notifier thread, up to a second.
bool waitForNotifications(int expected) {
    auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (notifications < expected && std::chrono::steady_clock::now() < limit)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return notifications == expected;
}
// Gives an unexpected notification the time to arrive.
bool noMoreNotifications(int expected) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return notifications == expected;
}

struct Fixture {
    AndroidAudioPlugin* plugin;
    aap_buffer_t buffer{makeBuffer()};
    aap_latency_extension_t* ext;

    int32_t latency() { return ext->latency_in_samples(ext, plugin); }
    void process() { plugin->process(plugin, &buffer, FRAMES, 0); }
    void sendDelay(double samples) {
        auto header = (AAPMidiBufferHeader*) midiIn.data();
        auto ump = (uint32_t*) (header + 1);
        aapMidi2ParameterSysex8(ump, ump + 1, ump + 2, ump + 3, 0, 0, 0, 0, (uint16_t) DELAY_PORT,
                                aapParameterPlainToTransportUint32(0.0, 4096.0, samples));
        header->length = 16;
    }
    void clearParameters() { ((AAPMidiBufferHeader*) midiIn.data())->length = 0; }
};

void reportsLatency(Fixture& f) {
    check(f.ext && f.ext->latency_in_samples, "the bridge provides the extension");
    check(f.latency() == 0, "no latency before prepare()");
    f.plugin->prepare(f.plugin, 48000, &f.buffer);
    f.plugin->activate(f.plugin);
    check(f.latency() == 0 && notifications == 0, "LV2 reports latency only in run()");

    f.process();
    check(f.latency() == 256, "the latency port value after run()");
    check(waitForNotifications(1), "the first reported latency is notified");
    check(!notifiedOnProcessThread, "not on the thread that runs the plugin");
    f.process();
    check(noMoreNotifications(1), "the same latency is not notified again");

    f.sendDelay(1024);
    f.process();
    f.clearParameters();
    check(f.latency() == 1024 && waitForNotifications(2), "a changed latency is notified");

    // Changes faster than the notifier end with the last value, and at least one notification.
    for (auto delay : {100.0, 200.0, 300.0}) {
        f.sendDelay(delay);
        f.process();
    }
    f.clearParameters();
    check(f.latency() == 300, "the last reported latency");
    auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (notifications < 3 && std::chrono::steady_clock::now() < limit)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // Let the notifier finish what the burst left pending.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(notifications >= 3 && notifications <= 5, "a burst of changes is notified");
    auto afterBurst = notifications.load();

    // Preparing again keeps the last latency for queries until the next run().
    f.plugin->deactivate(f.plugin);
    f.plugin->prepare(f.plugin, 48000, &f.buffer);
    check(f.latency() == 300, "prepare() keeps the reported latency");
    f.plugin->activate(f.plugin);
    f.sendDelay(300);
    f.process();
    f.clearParameters();
    check(noMoreNotifications(afterBurst), "the same latency after prepare() is not notified");
    f.plugin->deactivate(f.plugin);
}

void noLatencyPort(AndroidAudioPluginFactory* factory) {
    auto plugin = factory->instantiate(factory, "lv2:urn:aap-test:offline-probe", &host);
    check(plugin != nullptr, "the probe without a latency port is instantiated");
    Fixture f{plugin};
    f.ext = (aap_latency_extension_t*) plugin->get_extension(plugin, AAP_LATENCY_EXTENSION_URI);
    auto before = notifications.load();
    plugin->prepare(plugin, 48000, &f.buffer);
    plugin->activate(plugin);
    f.process();
    check(f.latency() == 0 && noMoreNotifications(before), "a plugin without a latency port has none");
    plugin->deactivate(plugin);
    factory->release(factory, plugin);
}
}

int main(int argc, char** argv) {
    setenv("LV2_PATH", argv[1], 1);
    processThread = std::this_thread::get_id();
    auto factory = GetAndroidAudioPluginFactoryLV2Bridge();
    try {
        auto plugin = factory->instantiate(factory, "lv2:urn:aap-test:latency-probe", &host);
        check(plugin != nullptr, "the probe is instantiated");
        Fixture f{plugin};
        f.ext = (aap_latency_extension_t*) plugin->get_extension(plugin, AAP_LATENCY_EXTENSION_URI);
        reportsLatency(f);
        // Releasing joins the notifier thread.
        factory->release(factory, plugin);
        noLatencyPort(factory);
    } catch (const std::exception& e) {
        fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
    puts("PASS: the LV2 latency port reaches the extension, and changes in run() are notified off the audio thread");
    return 0;
}
