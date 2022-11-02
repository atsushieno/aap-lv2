
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
#include <aap/ext/aap-midi2.h>
#include <aap/ext/presets.h>
#include <aap/ext/state.h>

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
#include <lv2/worker/worker.h>
#include <lv2/log/log.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/presets/presets.h>
#include <lv2/resize-port/resize-port.h>

#include "cmidi2.h"

#define AAP_LV2_LOG_PERF 0
#define AAP_TO_ATOM_CONVERSION_BUFFER_SIZE 0x80000 // 524288

#define AAP_LV2_TAG "aap-lv2"

enum AAPLV2InstanceState {
    AAP_LV2_INSTANCE_STATE_INITIAL = 1,
    AAP_LV2_INSTANCE_STATE_PREPARED,
    AAP_LV2_INSTANCE_STATE_ACTIVE,
    AAP_LV2_INSTANCE_STATE_TERMINATING,
    AAP_LV2_INSTANCE_STATE_ERROR
};

namespace aaplv2bridge {

int log_vprintf(LV2_Log_Handle, LV2_URID type, const char *fmt, va_list ap) {
    return aap::avprintf(fmt, ap);
}

int log_printf(LV2_Log_Handle handle, LV2_URID type, const char *fmt, ...) {
    va_list ap;
    va_start (ap, fmt);
    int ret = log_vprintf(handle, type, fmt, ap);
    va_end (ap);
    return ret;
}

class AAPLV2PluginContextStatics {
public:
    explicit AAPLV2PluginContextStatics(LilvWorld* world) {
        audio_port_uri_node = lilv_new_uri(world, LV2_CORE__AudioPort);
        control_port_uri_node = lilv_new_uri(world, LV2_CORE__ControlPort);
        input_port_uri_node = lilv_new_uri(world, LV2_CORE__InputPort);
        output_port_uri_node = lilv_new_uri(world, LV2_CORE__OutputPort);
        atom_port_uri_node = lilv_new_uri(world, LV2_ATOM__AtomPort);
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
        lilv_node_free(work_interface_uri_node);
        lilv_node_free(resize_port_minimum_size_node);
        lilv_node_free(presets_preset_node);
        lilv_node_free(rdfs_label_node);
    }

    LilvNode *audio_port_uri_node, *control_port_uri_node, *atom_port_uri_node,
             *input_port_uri_node, *output_port_uri_node, *work_interface_uri_node,
             *resize_port_minimum_size_node, *presets_preset_node,
             *rdfs_label_node;
};

class AAPLV2PluginContext;
typedef AAPLV2PluginContext Jalv;


// imported from jalv
typedef struct {
    Jalv*                       ctx;       ///< Pointer back to AAPLV2PluginContext
    ZixRing*                    requests{nullptr};   ///< Requests to the worker
    ZixRing*                    responses{nullptr};  ///< Responses from the worker
    void*                       response;   ///< Worker response buffer
    ZixSem                      sem;        ///< Worker semaphore
    ZixThread                   thread;     ///< Worker thread
    const LV2_Worker_Interface* iface{nullptr};      ///< Plugin worker interface
    bool                        threaded;   ///< Run work in another thread
} JalvWorker;


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

void* aap_lv2_plugin_get_extension(AndroidAudioPlugin *plugin, const char *uri);

struct AAPLV2URIDs {
    LV2_URID urid_atom_sequence_type{0},
            urid_midi_event_type{0},
            urid_time_frame{0},
            urid_atom_float_type{0};
};

class AAPLV2PluginContext {
public:
    AAPLV2PluginContext(AAPLV2PluginContextStatics *statics, LilvWorld *world,
                        const LilvPlugin *plugin, const char* pluginUniqueId, int32_t sampleRate)
            : statics(statics), world(world), plugin(plugin), aap_plugin_id(pluginUniqueId), sample_rate(sampleRate) {
        symap = symap_new();
        // They don't have default assignment...
        worker.threaded = false;
        state_worker.threaded = false;
    }

    ~AAPLV2PluginContext() {
        for (auto &p : presets)
            if (p->data)
                free(p->data);
        for (auto p : midi_atom_inputs)
            free(p.second);
        symap_free(symap);
    }

    int32_t instance_state{AAP_LV2_INSTANCE_STATE_INITIAL};
    AAPLV2PluginContextStatics *statics;
    AAPLv2PluginFeatures features;
    AAPLV2URIDs urids;
    LilvWorld *world;
    const LilvPlugin *plugin;
    LilvInstance *instance{nullptr};
    std::string aap_plugin_id{};
    int32_t sample_rate;
    AndroidAudioPluginBuffer *cached_buffer{nullptr};
    void *dummy_raw_buffer{nullptr};
    // We store ResizePort::minimumSize here, to specify sufficient Atom buffer size
    // (not to allocate local memory; it is passed as a shared memory by the local service host).
    std::map<int32_t, size_t> explicit_port_buffer_sizes{};
    int32_t midi_buffer_size = 1024;
    // They will all receive transformed AAP MIDI inputs into Atom.
    std::map<int32_t, LV2_Atom_Sequence *> midi_atom_inputs{};
    // Their Atom_Sequence strcts have to be re-assigned appropriate Atom sequence size every time
    //  (FIXME: is it appropriate? sfizz clearly expects this, but I cannot find appropriate LV2 spec description.)
    std::map<int32_t, LV2_Atom_Sequence *> atom_outputs{};
    LV2_Atom_Forge midi_in_atom_forge{};
    int32_t selected_preset_index{-1};
    std::vector<std::unique_ptr<aap_preset_t>> presets{};

    std::unique_ptr<LV2_Feature*> stateFeaturesList()
    {
        LV2_Feature* list [] {
                &features.mapFeature,
                &features.unmapFeature,
                &features.logFeature,
                &features.optionsFeature,
                &features.threadSafeRestoreFeature,
                &features.stateWorkerFeature,
                nullptr
        };
        auto ptr = (LV2_Feature**) calloc(sizeof(LV2_Feature*), sizeof(list));
        memcpy(ptr, list, sizeof(LV2_Feature*) * sizeof(list));
        std::unique_ptr<LV2_Feature*> ret{ptr};
        return ret;
    }

    // from jalv codebase
    Symap*             symap{nullptr};          ///< URI map
    ZixSem             symap_lock;     ///< Lock for URI map
    JalvWorker         worker;         ///< Worker thread implementation
    JalvWorker         state_worker;   ///< Synchronous worker for state restore
    ZixSem             work_lock;      ///< Lock for plugin work() method
    bool               safe_restore{false};   ///< Plugin restore() is thread-safe
    bool exit{false};
};


static LV2_URID
map_uri(LV2_URID_Map_Handle handle,
        const char*         uri)
{
    auto ctx = (AAPLV2PluginContext*)handle;
    zix_sem_wait(&ctx->symap_lock);
    const LV2_URID id = symap_map(ctx->symap, uri);
    zix_sem_post(&ctx->symap_lock);
    return id;
}

static const char*
unmap_uri(LV2_URID_Unmap_Handle handle,
          LV2_URID              urid)
{
    auto ctx = (AAPLV2PluginContext*)handle;
    zix_sem_wait(&ctx->symap_lock);
    const char* uri = symap_unmap(ctx->symap, urid);
    zix_sem_post(&ctx->symap_lock);
    return uri;
}

#define PORTCHECKER_SINGLE(_name_, _type_) inline bool _name_ (AAPLV2PluginContext *ctx, const LilvPlugin* plugin, const LilvPort* port) { return lilv_port_is_a (plugin, port, ctx->statics->_type_); }
#define PORTCHECKER_AND(_name_, _cond1_, _cond2_) inline bool _name_ (AAPLV2PluginContext *ctx, const LilvPlugin* plugin, const LilvPort* port) { return _cond1_ (ctx, plugin, port) && _cond2_ (ctx, plugin, port); }

PORTCHECKER_SINGLE (IS_AUDIO_PORT, audio_port_uri_node)

PORTCHECKER_SINGLE (IS_INPUT_PORT, input_port_uri_node)

PORTCHECKER_SINGLE (IS_OUTPUT_PORT, output_port_uri_node)

PORTCHECKER_SINGLE (IS_CONTROL_PORT, control_port_uri_node)

PORTCHECKER_SINGLE (IS_ATOM_PORT, atom_port_uri_node)

PORTCHECKER_AND (IS_AUDIO_IN, IS_AUDIO_PORT, IS_INPUT_PORT)

PORTCHECKER_AND (IS_AUDIO_OUT, IS_AUDIO_PORT, IS_OUTPUT_PORT)

PORTCHECKER_AND (IS_CONTROL_IN, IS_CONTROL_PORT, IS_INPUT_PORT)

PORTCHECKER_AND (IS_CONTROL_OUT, IS_CONTROL_PORT, IS_OUTPUT_PORT)

PORTCHECKER_AND (IS_ATOM_IN, IS_ATOM_PORT, IS_INPUT_PORT)

PORTCHECKER_AND (IS_ATOM_OUT, IS_ATOM_PORT, IS_OUTPUT_PORT)

void
jalv_worker_init(Jalv*                       ZIX_UNUSED(jalv),
                 JalvWorker*                 worker,
                 const LV2_Worker_Interface* iface,
                 bool                        threaded);
LV2_Worker_Status
jalv_worker_schedule(LV2_Worker_Schedule_Handle handle,
                     uint32_t                   size,
                     const void*                data);
void
jalv_worker_destroy(JalvWorker* worker);
void
jalv_worker_finish(JalvWorker* worker);
void
jalv_worker_emit_responses(JalvWorker* worker, LilvInstance* instance);

void aap_lv2_plugin_delete(
        AndroidAudioPluginFactory *,
        AndroidAudioPlugin *plugin) {
    auto l = (AAPLV2PluginContext *) plugin->plugin_specific;

    l->exit = true;
    l->instance_state = AAP_LV2_INSTANCE_STATE_TERMINATING;

    // Terminate the worker
    jalv_worker_finish(&l->worker);

    // Destroy the worker
    jalv_worker_destroy(&l->worker);

    free(l->dummy_raw_buffer);
    lilv_instance_free(l->instance);
    delete l->statics;
    lilv_world_free(l->world);
    delete l;
    delete plugin;
}

// It can be called by prepare() or process(), and for the latter case it must conform to
// realtime processing requirements. Thus, no allocation is permitted.
void resetPorts(AndroidAudioPlugin *plugin, AndroidAudioPluginBuffer *buffer, bool allocationPermitted) {
    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;
    auto lilvPlugin = ctx->plugin;
    auto instance = ctx->instance;

    ctx->cached_buffer = buffer;

    assert(buffer != nullptr);

    if (allocationPermitted) { // it can go into time-consuming allocation and port node lookup.
        int nPorts = lilv_plugin_get_num_ports(lilvPlugin);
        for (int i = 0; i < nPorts; i++) {
            const LilvPort* portNode = lilv_plugin_get_port_by_index(lilvPlugin, i);
            if (IS_ATOM_PORT(ctx, lilvPlugin, portNode)) {
                if (IS_INPUT_PORT(ctx, lilvPlugin, portNode)) {
                    ctx->midi_atom_inputs[i] = (LV2_Atom_Sequence *) calloc(ctx->midi_buffer_size, 1);
                } else {
                    ctx->atom_outputs[i] = (LV2_Atom_Sequence *) buffer->buffers[i];
                }

                const LilvPort *lilvPort = lilv_plugin_get_port_by_index(lilvPlugin, i);
                LilvNode* minimumSizeNode = lilv_port_get(lilvPlugin, lilvPort, ctx->statics->resize_port_minimum_size_node);
                if (minimumSizeNode) {
                    auto minSize = (size_t) lilv_node_as_int(minimumSizeNode);
                    if (minSize > buffer->num_frames * sizeof(float))
                        ctx->explicit_port_buffer_sizes[i] = minSize;
                }
            }
        }

        if (!ctx->dummy_raw_buffer)
            ctx->dummy_raw_buffer = calloc(buffer->num_frames * sizeof(float), 1);

        //  >>> The buffer size could be specified using Buf Size extension, for Atom-specific ports.
        if (ctx->midi_buffer_size < buffer->num_frames) {
            for (auto p : ctx->midi_atom_inputs) {
                free(p.second);
                ctx->midi_atom_inputs[p.first] = (LV2_Atom_Sequence *) calloc(AAP_TO_ATOM_CONVERSION_BUFFER_SIZE, 1);
            }
        }
    }

    int numPorts = lilv_plugin_get_num_ports(lilvPlugin);
    for (int p = 0; p < numPorts; p++) {
        auto iter = ctx->midi_atom_inputs.find(p);
        auto bp = iter != ctx->midi_atom_inputs.end() ? iter->second : buffer->buffers[p];
        if (bp == nullptr)
            lilv_instance_connect_port(instance, p, ctx->dummy_raw_buffer);
        else
            lilv_instance_connect_port(instance, p, bp);
    }
}

void aap_lv2_plugin_prepare(AndroidAudioPlugin *plugin, AndroidAudioPluginBuffer *buffer) {
    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_ERROR)
        return;
    if (ctx->instance_state != AAP_LV2_INSTANCE_STATE_INITIAL) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "LV2 plugin %s not at unprepared state.", ctx->aap_plugin_id.c_str());
        ctx->instance_state = AAP_LV2_INSTANCE_STATE_ERROR;
        return;
    }

    resetPorts(plugin, buffer, true);

    ctx->instance_state = AAP_LV2_INSTANCE_STATE_PREPARED;

    for (auto p : ctx->midi_atom_inputs) {
        auto uridMap = &ctx->features.urid_map_feature_data;
        auto forge = &ctx->midi_in_atom_forge;
        lv2_atom_forge_init(forge, uridMap);
    }

    aap::a_log_f(AAP_LOG_LEVEL_INFO, AAP_LV2_TAG, "LV2 plugin %s is ready, prepared.", ctx->aap_plugin_id.c_str());
}

void aap_lv2_plugin_activate(AndroidAudioPlugin *plugin) {
    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_ERROR)
        return;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_TERMINATING)
        return;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_PREPARED) {
        lilv_instance_activate(ctx->instance);
        ctx->instance_state = AAP_LV2_INSTANCE_STATE_ACTIVE;
    } else {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "LV2 plugin %s is not at prepared state.", ctx->aap_plugin_id.c_str());
        ctx->instance_state = AAP_LV2_INSTANCE_STATE_ERROR;
    }
}

void
write_midi2_events_as_midi1_to_lv2_forge(AAPLV2PluginContext* ctx, LV2_Atom_Forge *forge, int32_t timeDivision, void *src) {
    assert(src != nullptr);
    assert(forge != nullptr);

    volatile auto aapmb = (AAPMidiBufferHeader*) src;
    uint64_t currentJRTimestamp = 0; // unit of 1/31250 sec. (JR_TIMESTAMP_TICKS_PER_SECOND)

    uint8_t midi1Bytes[16];

    CMIDI2_UMP_SEQUENCE_FOREACH((uint8_t*) src + sizeof(AAPMidiBufferHeader), aapmb->length, iter) {
        auto ump = (cmidi2_ump*) iter;
        uint32_t midiEventSize = 3;
        uint64_t sysex7, sysex8_1, sysex8_2;

        auto messageType = cmidi2_ump_get_message_type(ump);
        auto statusCode = cmidi2_ump_get_status_code(ump); // may not apply, but won't break.

        // update time info if it is a utility message, and skip Atom event emission.
        if (messageType == CMIDI2_MESSAGE_TYPE_UTILITY) {
            switch (statusCode) {
            case CMIDI2_JR_CLOCK:
                // FIXME: take JR_CLOCK into consideration
                break;
            case CMIDI2_JR_TIMESTAMP:
                currentJRTimestamp += cmidi2_ump_get_jr_timestamp_timestamp(ump);
                break;
            }
            continue;
        }

        lv2_atom_forge_frame_time(forge, (double) currentJRTimestamp / JR_TIMESTAMP_TICKS_PER_SECOND * ctx->sample_rate);

        midi1Bytes[0] = statusCode | cmidi2_ump_get_channel(ump);

        switch (messageType) {
        case CMIDI2_MESSAGE_TYPE_SYSTEM:
            midiEventSize = 1;
            switch (statusCode) {
            case 0xF1:
            case 0xF3:
            case 0xF9:
                midi1Bytes[1] = cmidi2_ump_get_system_message_byte2(ump);
                midiEventSize = 2;
                break;
            }
            break;
        case CMIDI2_MESSAGE_TYPE_MIDI_1_CHANNEL:
            midi1Bytes[1] = cmidi2_ump_get_midi1_byte2(ump);
            switch (statusCode) {
            case 0xC0:
            case 0xD0:
                midiEventSize = 2;
                break;
            default:
                midi1Bytes[2] = cmidi2_ump_get_midi1_byte3(ump);
                break;
            }
            break;
        case CMIDI2_MESSAGE_TYPE_MIDI_2_CHANNEL:
            // FIXME: convert MIDI2 to MIDI1 as long as possible
            switch (statusCode) {
                case CMIDI2_STATUS_NOTE_OFF:
                case CMIDI2_STATUS_NOTE_ON:
                    midiEventSize = 3;
                    midi1Bytes[1] = cmidi2_ump_get_midi2_note_note(ump);
                    midi1Bytes[2] = cmidi2_ump_get_midi2_note_velocity(ump) / 0x200;
                    break;
                case CMIDI2_STATUS_PAF:
                    midiEventSize = 3;
                    midi1Bytes[1] = cmidi2_ump_get_midi2_paf_note(ump);
                    midi1Bytes[2] = cmidi2_ump_get_midi2_paf_data(ump) / 0x2000000;
                    break;
                case CMIDI2_STATUS_CC:
                    midiEventSize = 3;
                    midi1Bytes[1] = cmidi2_ump_get_midi2_cc_index(ump);
                    midi1Bytes[2] = cmidi2_ump_get_midi2_cc_data(ump) / 0x2000000;
                    break;
                case CMIDI2_STATUS_PROGRAM:
                    if (cmidi2_ump_get_midi2_program_options(ump) == 1) {
                        midiEventSize = 8;
                        midi1Bytes[6] = midi1Bytes[0]; // copy
                        midi1Bytes[7] = cmidi2_ump_get_midi2_program_program(ump);
                        midi1Bytes[0] = midi1Bytes[6] & 0xF + CMIDI2_STATUS_CC;
                        midi1Bytes[1] = 0; // Bank MSB
                        midi1Bytes[2] = cmidi2_ump_get_midi2_program_bank_msb(ump);
                        midi1Bytes[3] = midi1Bytes[6] & 0xF + CMIDI2_STATUS_CC;
                        midi1Bytes[4] = 32; // Bank LSB
                        midi1Bytes[5] = cmidi2_ump_get_midi2_program_bank_lsb(ump);
                    } else {
                        midiEventSize = 2;
                        midi1Bytes[1] = cmidi2_ump_get_midi2_program_program(ump);
                    }
                    break;
                case CMIDI2_STATUS_CAF:
                    midiEventSize = 2;
                    midi1Bytes[1] = cmidi2_ump_get_midi2_caf_data(ump);
                    break;
                case CMIDI2_STATUS_PITCH_BEND:
                    midiEventSize = 3;
                    auto pitchBendV1 = cmidi2_ump_get_midi2_pitch_bend_data(ump) / 0x40000;
                    midi1Bytes[1] = pitchBendV1 % 0x80;
                    midi1Bytes[2] = pitchBendV1 / 0x80;
                    break;
                // skip for other status bytes; we cannot support them.
            }
            break;
        case CMIDI2_MESSAGE_TYPE_SYSEX7:
            midiEventSize = 1 + cmidi2_ump_get_sysex7_num_bytes(ump);
            sysex7 = cmidi2_ump_read_uint64_bytes(ump);
            for (int i = 0; i < midiEventSize - 1; i++)
                midi1Bytes[i] = cmidi2_ump_get_byte_from_uint64(sysex7, 2 + i);
            break;
        case CMIDI2_MESSAGE_TYPE_SYSEX8_MDS:
            // Note that sysex8 num_bytes contains streamId, which is NOT part of MIDI 1.0 sysex7.
            midiEventSize = 1 + cmidi2_ump_get_sysex8_num_bytes(ump) - 1;
            sysex8_1 = cmidi2_ump_read_uint64_bytes(ump);
            sysex8_2 = cmidi2_ump_read_uint64_bytes(ump);
            for (int i = 0; i < 5 && i < midiEventSize - 1; i++)
                midi1Bytes[i] = cmidi2_ump_get_byte_from_uint64(sysex8_1, 3 + i);
            for (int i = 6; i < midiEventSize - 1; i++)
                midi1Bytes[i] = cmidi2_ump_get_byte_from_uint64(sysex8_2, i);
            // verify 7bit compatibility and then SYSEX8 to SYSEX7
            for (int i = 1; i < midiEventSize; i++) {
                if (midi1Bytes[i] > 0x80) {
                    midiEventSize = 0;
                    break;
                }
            }
            break;
        }

        lv2_atom_forge_raw(forge, &midiEventSize, sizeof(int));
        lv2_atom_forge_raw(forge, &ctx->urids.urid_midi_event_type, sizeof(int));
        lv2_atom_forge_raw(forge, midi1Bytes, midiEventSize);
        lv2_atom_forge_pad(forge, midiEventSize);
    }
}

void aap_lv2_plugin_process(AndroidAudioPlugin *plugin,
                            AndroidAudioPluginBuffer *buffer,
                            long timeoutInNanoseconds) {
    // FIXME: use timeoutInNanoseconds?

    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_ERROR)
        return;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_TERMINATING)
        return;
    if (!ctx->instance_state == AAP_LV2_INSTANCE_STATE_ACTIVE) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "LV2 plugin %s is not at prepared state.", ctx->aap_plugin_id.c_str());
        ctx->instance_state = AAP_LV2_INSTANCE_STATE_ERROR;
        return;
    }

#if AAP_LV2_LOG_PERF
    struct timespec timeSpecBegin, timeSpecEnd;
    clock_gettime(CLOCK_REALTIME, &timeSpecBegin);
    aap::a_log_f(AAP_LOG_LEVEL_DEBUG, "aap-lv2.perf", "timeout: %ld nsec.", timeoutInNanoseconds);
#endif

    if (buffer != ctx->cached_buffer)
        resetPorts(plugin, buffer, false);

    /* Process any worker replies. */
    jalv_worker_emit_responses(&ctx->state_worker, ctx->instance);
    jalv_worker_emit_responses(&ctx->worker, ctx->instance);

    /* Notify the plugin the run() cycle is finished */
    if (ctx->worker.iface && ctx->worker.iface->end_run)
        ctx->worker.iface->end_run(ctx->instance->lv2_handle);

    for (auto p : ctx->atom_outputs) {
        // Clean up Atom output sequence ports.
        // non-MIDI Atom ports are unsupported, but regardless it "seems" we have to keep it clean and assign appropriate sizes.
        lv2_atom_sequence_clear(p.second);
        auto bufferSize = ctx->explicit_port_buffer_sizes.find(p.first) == ctx->explicit_port_buffer_sizes.end() ? buffer->num_frames * sizeof(float) : ctx->explicit_port_buffer_sizes[p.first];
        p.second->atom.size = bufferSize - sizeof(LV2_Atom);
    }

    // convert AAP MIDI/MIDI2 messages into Atom Sequence of MidiEvent.
    for (auto p : ctx->midi_atom_inputs) {
        void *src = buffer->buffers[p.first];
        auto forge = &ctx->midi_in_atom_forge;
        // Port buffer size may be specified by ResizePort::minimumSize (minimum is not necessarily the size, but one thing that needs to check).
        size_t bufSize = ctx->explicit_port_buffer_sizes.find(p.first) == ctx->explicit_port_buffer_sizes.end() ?
                         buffer->num_frames * sizeof(float) :
                         ctx->explicit_port_buffer_sizes[p.first];
        lv2_atom_forge_set_buffer(forge, (uint8_t *) p.second, bufSize);
        LV2_Atom_Forge_Frame frame;
        lv2_atom_sequence_clear(p.second);

        auto seqRef = lv2_atom_forge_sequence_head(forge, &frame, ctx->urids.urid_time_frame);
        auto seq = (LV2_Atom_Sequence *) lv2_atom_forge_deref(forge, seqRef);
        lv2_atom_forge_pop(forge, &frame);
        write_midi2_events_as_midi1_to_lv2_forge(ctx, forge, buffer->num_frames, src);
        seq->atom.size = forge->offset - sizeof(LV2_Atom);
    }

#if AAP_LV2_LOG_PERF
    clock_gettime(CLOCK_REALTIME, &timeSpecEnd);
    long timeDiffRP = (timeSpecEnd.tv_sec - timeSpecBegin.tv_sec) * 1000000000 + timeSpecEnd.tv_nsec - timeSpecBegin.tv_nsec;
    aap::a_log_f(AAP_LOG_LEVEL_DEBUG, "aap-lv2.perf", "resetPorts perf. time diff %ld nsec.", timeDiffRP);
#endif

    lilv_instance_run(ctx->instance, buffer->num_frames);

    // FIXME: there should be another set of converters that transforms LV2 MIDI outputs into AAP MIDI stream (either 1.0 or 2.0)

#if AAP_LV2_LOG_PERF
    clock_gettime(CLOCK_REALTIME, &timeSpecEnd);
    long timeDiff = (timeSpecEnd.tv_sec - timeSpecBegin.tv_sec) * 1000000000 + timeSpecEnd.tv_nsec - timeSpecBegin.tv_nsec;
    aap::a_log_f(AAP_LOG_LEVEL_DEBUG, "aap-lv2.perf", "processing perf. time diff %ld nsec.", timeDiff);
#endif
}

void aap_lv2_plugin_deactivate(AndroidAudioPlugin *plugin) {
    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_ERROR)
        return;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_TERMINATING)
        return;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_ACTIVE) {
        lilv_instance_deactivate(ctx->instance);
        ctx->instance_state = AAP_LV2_INSTANCE_STATE_PREPARED;
    } else {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "LV2 plugin %s is not at prepared state.",
                     ctx->aap_plugin_id.c_str());
        ctx->instance_state = AAP_LV2_INSTANCE_STATE_ERROR;
    }
}

AndroidAudioPlugin *aap_lv2_plugin_new(
        AndroidAudioPluginFactory *pluginFactory,
        const char *pluginUniqueID,
        int sampleRate,
        AndroidAudioPluginHost *host) {
    aap::a_log_f(AAP_LOG_LEVEL_INFO, AAP_LV2_TAG, "Instantiating aap-lv2 plugin %s", pluginUniqueID);

    auto world = lilv_world_new();
    // Here we expect that LV2_PATH is already set using setenv() etc.
    lilv_world_load_all(world);

    auto statics = new AAPLV2PluginContextStatics(world);

    auto allPlugins = lilv_world_get_all_plugins(world);
    if (lilv_plugins_size(allPlugins) <= 0) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "No LV2 plugins were found.");
        return nullptr;
    }

    // AAP-LV2 Plugin URI is just LV2 URI prefixed by "lv2:".
    if (strncmp(pluginUniqueID, "lv2:", strlen("lv2:"))) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "Unexpected AAP LV2 pluginId: %s", pluginUniqueID);
        return nullptr;
    }
    auto pluginUriNode = lilv_new_uri(world, pluginUniqueID + strlen("lv2:"));
    const LilvPlugin *plugin = lilv_plugins_get_by_uri(allPlugins, pluginUriNode);
    lilv_node_free(pluginUriNode);
    if (!plugin) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "LV2 plugin could not be instantiated: %s",
                     lilv_node_as_uri(pluginUriNode));
        return nullptr;
    }
    if (!lilv_plugin_verify(plugin)) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "LV2 plugin is invalid: %s",
                     lilv_node_as_uri(pluginUriNode));
        return nullptr;
    }

    aap::a_log_f(AAP_LOG_LEVEL_INFO, AAP_LV2_TAG, "Plugin %s is valid, ready to instantiate.", pluginUniqueID);

    auto ctx = new AAPLV2PluginContext(statics, world, plugin, pluginUniqueID, sampleRate);

    ctx->features.urid_map_feature_data.handle = ctx;
    ctx->features.urid_map_feature_data.map = map_uri;
    ctx->features.urid_unmap_feature_data.handle = ctx;
    ctx->features.urid_unmap_feature_data.unmap = unmap_uri;

    if (zix_sem_init(&ctx->symap_lock, 1)) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "Failed to initialize semaphore (symap). plugin: %s",
                     lilv_node_as_uri(pluginUriNode));
        return nullptr;
    }
    if (zix_sem_init(&ctx->work_lock, 1)) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "Failed to initialize semaphore (work_lock). plugin: %s",
                     lilv_node_as_uri(pluginUriNode));
        return nullptr;
    }
    ctx->worker.ctx = ctx;
    ctx->state_worker.ctx = ctx;

    ctx->features.worker_schedule_data.handle = &ctx->worker;
    ctx->features.worker_schedule_data.schedule_work = jalv_worker_schedule;
    ctx->features.state_worker_schedule_data.handle = &ctx->state_worker;
    ctx->features.state_worker_schedule_data.schedule_work = jalv_worker_schedule;

    ctx->features.minBlockLengthOption = {LV2_OPTIONS_INSTANCE,
        0,
        map_uri(ctx, LV2_BUF_SIZE__minBlockLength),
        sizeof(int),
        map_uri(ctx, LV2_ATOM__Int),
        &ctx->features.minBlockLengthValue};
    ctx->features.maxBlockLengthOption = {LV2_OPTIONS_INSTANCE,
                                          0,
                                          map_uri(ctx, LV2_BUF_SIZE__maxBlockLength),
                                          sizeof(int),
                                          map_uri(ctx, LV2_ATOM__Int),
                                          &ctx->features.maxBlockLengthValue};

    LV2_Options_Option options[3];

    // FIXME: adjust those variables at prepare() step.
    options[0] = ctx->features.minBlockLengthOption;
    options[1] = ctx->features.maxBlockLengthOption;
    options[2] = LV2_Options_Option{LV2_OPTIONS_BLANK, 0, 0, 0, 0};
    ctx->features.optionsFeature.data = &options;

    LV2_Feature* features [] {
            &ctx->features.mapFeature,
            &ctx->features.unmapFeature,
            &ctx->features.logFeature,
            &ctx->features.bufSizeFeature,
            &ctx->features.optionsFeature,
            &ctx->features.threadSafeRestoreFeature,
            &ctx->features.workerFeature,
            nullptr
    };

    // for jalv worker
    if (zix_sem_init(&ctx->worker.sem, 0)) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "Failed to initialize semaphore on worker. plugin: %s",
                     lilv_node_as_uri(pluginUriNode));
        return nullptr;
    }

    LilvInstance *instance = lilv_plugin_instantiate(plugin, sampleRate, features);
    if (!instance) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "Failed to instantiate plugin: %s",
                     lilv_node_as_uri(pluginUriNode));
        return nullptr;
    }
    ctx->instance = instance;

    auto map = &ctx->features.urid_map_feature_data;
    if (!ctx->urids.urid_atom_sequence_type) {
        ctx->urids.urid_atom_sequence_type = map->map(map->handle, LV2_ATOM__Sequence);
        ctx->urids.urid_midi_event_type = map->map(map->handle, LV2_MIDI__MidiEvent);
        ctx->urids.urid_time_frame = map->map(map->handle, LV2_ATOM__frameTime);
        ctx->urids.urid_atom_float_type = map->map(map->handle, LV2_ATOM__Float);
    }

    /* Check for thread-safe state restore() method. */
    LilvNode* state_threadSafeRestore = lilv_new_uri(
            ctx->world, LV2_STATE__threadSafeRestore);
    if (lilv_plugin_has_feature(ctx->plugin, state_threadSafeRestore)) {
        ctx->safe_restore = true;
    }
    lilv_node_free(state_threadSafeRestore);

    if (lilv_plugin_has_extension_data(ctx->plugin, ctx->statics->work_interface_uri_node)) {
        const auto* iface = (const LV2_Worker_Interface*)
                lilv_instance_get_extension_data(ctx->instance, LV2_WORKER__interface);

        jalv_worker_init(ctx, &ctx->worker, iface, true);
        if (ctx->safe_restore) {
            jalv_worker_init(ctx, &ctx->state_worker, iface, false);
        }
    }

    auto ret = new AndroidAudioPlugin{
            ctx,
            aap_lv2_plugin_prepare,
            aap_lv2_plugin_activate,
            aap_lv2_plugin_process,
            aap_lv2_plugin_deactivate,
            aap_lv2_plugin_get_extension
    };
    aap::a_log_f(AAP_LOG_LEVEL_INFO, AAP_LV2_TAG, "Instantiated aap-lv2 plugin %s", pluginUniqueID);
    return ret;
}

// FIXME: restructure AAPLV2Context and make it ordinal source code structure.
#include "aap-lv2-extensions.cpp"

AndroidAudioPluginFactory aap_lv2_factory{aap_lv2_plugin_new,
                                          aap_lv2_plugin_delete,
                                          nullptr};

} // namespace aaplv2bridge

extern "C" {

AndroidAudioPluginFactory *GetAndroidAudioPluginFactoryLV2Bridge() { return &aaplv2bridge::aap_lv2_factory; }

} // extern "C"
