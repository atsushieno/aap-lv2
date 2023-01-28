#ifndef AAP_LV2_INTERNAL_INCLUDED
#define AAP_LV2_INTERNAL_INCLUDED 1

#include <unistd.h>
#include <dlfcn.h>
#include <cmath>
#include <ctime>
#include <cstring>
#include <cassert>
#include <memory>
#include <vector>
#include <map>
#include <string>

#include <aap/unstable/logging.h>
#include <aap/android-audio-plugin.h>
#include <aap/ext/midi.h>
#include <aap/ext/parameters.h>
#include <aap/ext/presets.h>
#include <aap/ext/state.h>
#include <aap/ext/plugin-info.h>

#include "symap.h"
#include "zix/sem.h"
#include "zix/ring.h"
#include "zix/thread.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/atom/forge.h>
#include <lv2/urid/urid.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/worker/worker.h>
#include <lv2/log/log.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/presets/presets.h>
#include <lv2/resize-port/resize-port.h>
#include <lv2/patch/patch.h>

#include "cmidi2.h"


#define AAP_LV2_LOG_PERF 0

#define AAP_LV2_TAG "aap-lv2"

enum AAPLV2InstanceState {
    AAP_LV2_INSTANCE_STATE_INITIAL = 1,
    AAP_LV2_INSTANCE_STATE_PREPARED,
    AAP_LV2_INSTANCE_STATE_ACTIVE,
    AAP_LV2_INSTANCE_STATE_TERMINATING,
    AAP_LV2_INSTANCE_STATE_ERROR
};

namespace aaplv2bridge {

inline int log_vprintf(LV2_Log_Handle, LV2_URID type, const char *fmt, va_list ap) {
    return aap::avprintf(fmt, ap);
}

inline int log_printf(LV2_Log_Handle handle, LV2_URID type, const char *fmt, ...) {
    va_list ap;
    va_start (ap, fmt);
    int ret = log_vprintf(handle, type, fmt, ap);
    va_end (ap);
    return ret;
}

class AAPLV2PluginContextStatics {
public:
    explicit AAPLV2PluginContextStatics(LilvWorld *world) {
        audio_port_uri_node = lilv_new_uri(world, LV2_CORE__AudioPort);
        control_port_uri_node = lilv_new_uri(world, LV2_CORE__ControlPort);
        input_port_uri_node = lilv_new_uri(world, LV2_CORE__InputPort);
        output_port_uri_node = lilv_new_uri(world, LV2_CORE__OutputPort);
        default_uri_node = lilv_new_uri(world, LV2_CORE__default);
        atom_port_uri_node = lilv_new_uri(world, LV2_ATOM__AtomPort);
        midi_event_uri_node = lilv_new_uri(world, LV2_MIDI__MidiEvent);
        patch_message_uri_node = lilv_new_uri(world, LV2_PATCH__Message);
        work_interface_uri_node = lilv_new_uri(world, LV2_WORKER__interface);
        resize_port_minimum_size_node = lilv_new_uri(world, LV2_RESIZE_PORT__minimumSize);
        presets_preset_node = lilv_new_uri(world, LV2_PRESETS__Preset);
        rdfs_label_node = lilv_new_uri(world, LILV_NS_RDFS "label");
    }

    ~AAPLV2PluginContextStatics() {
        lilv_node_free(audio_port_uri_node);
        lilv_node_free(control_port_uri_node);
        lilv_node_free(atom_port_uri_node);
        lilv_node_free(input_port_uri_node);
        lilv_node_free(output_port_uri_node);
        lilv_node_free(midi_event_uri_node);
        lilv_node_free(patch_message_uri_node);
        lilv_node_free(work_interface_uri_node);
        lilv_node_free(resize_port_minimum_size_node);
        lilv_node_free(presets_preset_node);
        lilv_node_free(rdfs_label_node);
    }

    LilvNode *audio_port_uri_node, *control_port_uri_node, *atom_port_uri_node,
            *input_port_uri_node, *output_port_uri_node,
            *default_uri_node,
            *midi_event_uri_node, *patch_message_uri_node,
            *resize_port_minimum_size_node, *presets_preset_node,
            *work_interface_uri_node, *rdfs_label_node;
};

class AAPLV2PluginContext;

typedef AAPLV2PluginContext Jalv;


// imported from jalv
typedef struct {
    Jalv *ctx;       ///< Pointer back to AAPLV2PluginContext
    ZixRing *requests{nullptr};   ///< Requests to the worker
    ZixRing *responses{nullptr};  ///< Responses from the worker
    void *response;   ///< Worker response buffer
    ZixSem sem;        ///< Worker semaphore
    ZixThread thread;     ///< Worker thread
    const LV2_Worker_Interface *iface{nullptr};      ///< Plugin worker interface
    bool threaded;   ///< Run work in another thread
} JalvWorker;

void
jalv_worker_emit_responses(JalvWorker *worker, LilvInstance *instance);


class AAPLv2PluginFeatures {
public:
    LV2_URID_Map urid_map_feature_data;
    LV2_URID_Unmap urid_unmap_feature_data;
    LV2_Worker_Schedule worker_schedule_data{};
    LV2_Worker_Schedule state_worker_schedule_data{};
    LV2_Log_Log logData{nullptr, log_printf, log_vprintf};

    const int minBlockLengthValue = 128;
    // FIXME: this should not be a magic number, but lowering the value causes aap-sfizz crash.
    //  Needs some investigation.
    const int maxBlockLengthValue = 8192;
    LV2_Options_Option minBlockLengthOption;
    LV2_Options_Option maxBlockLengthOption;

    LV2_Feature mapFeature{LV2_URID__map, &urid_map_feature_data};
    LV2_Feature unmapFeature{LV2_URID__unmap, &urid_unmap_feature_data};
    LV2_Feature logFeature{LV2_LOG__log, &logData};
    LV2_Feature bufSizeFeature{LV2_BUF_SIZE__boundedBlockLength, nullptr};
    LV2_Feature optionsFeature{LV2_OPTIONS__options, nullptr};
    LV2_Feature workerFeature{LV2_WORKER__schedule, &worker_schedule_data};
    LV2_Feature stateWorkerFeature{LV2_WORKER__schedule, &state_worker_schedule_data};
    LV2_Feature threadSafeRestoreFeature{LV2_STATE__threadSafeRestore, nullptr};
};

void *aap_lv2_plugin_get_extension(AndroidAudioPlugin *plugin, const char *uri);

struct AAPLV2URIDs {
    LV2_URID urid_atom_sequence_type{0},
            urid_midi_event_type{0},
            urid_time_frame{0},
            urid_atom_float_type{0};
};

class AAPLV2PortMappings {
public:
    int32_t aap_midi_in_port{-1};
    int32_t aap_midi_out_port{-1};
    std::map<int32_t, int32_t> aap_to_lv2_portmap{};
    std::map<int32_t, int32_t> lv2_to_aap_portmap{};
    std::map<uint32_t, int32_t> lv2_index_to_port{};
    std::map<int32_t, int32_t> ump_group_to_atom_in_port{};
    std::map<int32_t, int32_t> atom_out_port_to_ump_group{};
    int32_t lv2_patch_in_port{-1};
    int32_t lv2_patch_out_port{-1};
};

class AAPLV2PluginContext {
public:
    AAPLV2PluginContext(AndroidAudioPluginHost *host, AAPLV2PluginContextStatics *statics,
                        LilvWorld *world,
                        const LilvPlugin *plugin, const char *pluginUniqueId, int32_t sampleRate)
            : aap_host(host), statics(statics), world(world), plugin(plugin),
              aap_plugin_id(pluginUniqueId), sample_rate(sampleRate) {
        symap = symap_new();
        // They don't have default assignment...
        worker.threaded = false;
        state_worker.threaded = false;
    }

    ~AAPLV2PluginContext() {
        for (auto &p: presets)
            if (p->data)
                free(p->data);
        for (auto p: midi_atom_inputs)
            free(p.second);
        for (auto p: explicitly_allocated_port_buffers)
            free(p.second);
        if (control_buffer_pointers)
            free(control_buffer_pointers);
        symap_free(symap);
    }

    int32_t instance_state{AAP_LV2_INSTANCE_STATE_INITIAL};
    AndroidAudioPluginHost *aap_host;
    AAPLV2PluginContextStatics *statics;
    AAPLv2PluginFeatures features;
    AAPLV2URIDs urids;
    AAPLV2PortMappings mappings;
    LilvWorld *world;
    const LilvPlugin *plugin;
    LilvInstance *instance{nullptr};
    std::string aap_plugin_id{};
    int32_t sample_rate;

    AndroidAudioPluginBuffer *cached_buffer{nullptr};

    void *dummy_raw_buffer{nullptr};

    // a ControlPort points to single float value, which can be stored in an array.
    float *control_buffer_pointers{nullptr};
    // We store ResizePort::minimumSize here, to specify sufficient Atom buffer size
    // (not to allocate local memory; it is passed as a shared memory by the local service host).
    std::map<int32_t, size_t> explicit_port_buffer_sizes{};
    // FIXME: make it a simple array so that we don't have to iterate over in every `process()`.
    std::map<int32_t, void *> explicitly_allocated_port_buffers{};
    int32_t atom_buffer_size = 0x1000;
    // They receive the Atom events that were translated from AAP MIDI2 inputs.
    std::map<int32_t, LV2_Atom_Sequence *> midi_atom_inputs{};
    // Their outputs have to be translated to AAP MIDI2 outputs.
    std::map<int32_t, LV2_Atom_Sequence *> midi_atom_outputs{};

    std::map<int32_t, LV2_Atom_Forge> midi_forges_in{};
    std::map<int32_t, LV2_Atom_Forge> midi_forges_out{};
    LV2_Atom_Forge patch_forge_in{};
    LV2_Atom_Forge patch_forge_out{};

    int32_t selected_preset_index{-1};
    std::vector<std::unique_ptr<aap_preset_t>> presets{};

    std::unique_ptr<LV2_Feature *> stateFeaturesList() {
        LV2_Feature *list[]{
                &features.mapFeature,
                &features.unmapFeature,
                &features.logFeature,
                &features.optionsFeature,
                &features.threadSafeRestoreFeature,
                &features.stateWorkerFeature,
                nullptr
        };
        auto ptr = (LV2_Feature **) calloc(sizeof(LV2_Feature *), sizeof(list));
        memcpy(ptr, list, sizeof(LV2_Feature *) * sizeof(list));
        std::unique_ptr<LV2_Feature *> ret{ptr};
        return ret;
    }

    // from jalv codebase
    Symap *symap{nullptr};          ///< URI map
    ZixSem symap_lock;     ///< Lock for URI map
    JalvWorker worker;         ///< Worker thread implementation
    JalvWorker state_worker;   ///< Synchronous worker for state restore
    ZixSem work_lock;      ///< Lock for plugin work() method
    bool safe_restore{false};   ///< Plugin restore() is thread-safe
    bool exit{false};
};

static LV2_URID
map_uri(LV2_URID_Map_Handle handle,
        const char *uri) {
    auto ctx = (AAPLV2PluginContext *) handle;
    zix_sem_wait(&ctx->symap_lock);
    const LV2_URID id = symap_map(ctx->symap, uri);
    zix_sem_post(&ctx->symap_lock);
    return id;
}

static const char *
unmap_uri(LV2_URID_Unmap_Handle handle,
          LV2_URID urid) {
    auto ctx = (AAPLV2PluginContext *) handle;
    zix_sem_wait(&ctx->symap_lock);
    const char *uri = symap_unmap(ctx->symap, urid);
    zix_sem_post(&ctx->symap_lock);
    return uri;
}


// AAP extensions

size_t aap_lv2_get_state_size(AndroidAudioPluginExtensionTarget target);

void aap_lv2_get_state(AndroidAudioPluginExtensionTarget target, aap_state_t *input);

void aap_lv2_set_state(AndroidAudioPluginExtensionTarget target, aap_state_t *input);

int32_t aap_lv2_get_preset_count(AndroidAudioPluginExtensionTarget target);

int32_t aap_lv2_get_preset_data_size(AndroidAudioPluginExtensionTarget target, int32_t index);

void aap_lv2_get_preset(AndroidAudioPluginExtensionTarget target, int32_t index, bool skipBinary,
                        aap_preset_t *destination);

int32_t aap_lv2_get_preset_index(AndroidAudioPluginExtensionTarget target);

void aap_lv2_set_preset_index(AndroidAudioPluginExtensionTarget target, int32_t index);

void aap_lv2_plugin_prepare(AndroidAudioPlugin *plugin, AndroidAudioPluginBuffer *buffer);

void aap_lv2_plugin_activate(AndroidAudioPlugin *plugin);

void aap_lv2_plugin_process(AndroidAudioPlugin *plugin,
                            AndroidAudioPluginBuffer *buffer,
                            long timeoutInNanoseconds);

void aap_lv2_plugin_deactivate(AndroidAudioPlugin *plugin);

}

#endif // ifndef AAP_LV2_INTERNAL_INCLUDED
