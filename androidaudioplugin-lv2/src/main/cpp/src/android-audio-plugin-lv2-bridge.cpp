
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
#include <aap/unstable/aap-midi2.h>
#include <aap/unstable/presets.h>

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

#include "cmidi2.h"
#include "lv2-midi2.h"

#define JUCEAAP_LOG_PERF 0

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

typedef struct {
    LilvNode *audio_port_uri_node, *control_port_uri_node, *atom_port_uri_node,
             *input_port_uri_node, *output_port_uri_node, *work_interface_uri_node,
             *atom_supports_uri_node, *presets_preset_node,
             *rdfs_label_node,
             *midi2_established_protocol_uri_node, *midi2_ump_uri_node;
} AAPLV2PluginContextStatics;

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
    LV2_Feature midi2UmpFeature{LV2_MIDI2__ump, nullptr};
    LV2_Feature midi2Midi1ProtocolFeature{LV2_MIDI2__midi1Protocol, nullptr};
    LV2_Feature midi2Midi2ProtocolFeature{LV2_MIDI2__midi2Protocol, nullptr};
};

int32_t aap_lv2_get_preset_count(aap_presets_context_t* context);
int32_t aap_lv2_get_preset_data_size(aap_presets_context_t* context, int32_t index);
void aap_lv2_get_preset(aap_presets_context_t* context, int32_t index, bool skipBinary, aap_preset_t* destination);
int32_t aap_lv2_get_preset_index(aap_presets_context_t* context);
void aap_lv2_set_preset_index(aap_presets_context_t* context, int32_t index);

struct AAPLV2URIDs {
    LV2_URID urid_atom_sequence_type{0},
        urid_midi_event_type{0},
        urid_time_frame{0},
        urid_time_beats{0},
        urid_atom_float_type{0},
        urid_worker_interface{0},
        urid_midi2_midi1_protocol{0},
        urid_midi2_midi2_protocol{0};
};

aap_presets_extension_t presets_ext{nullptr,
                                aap_lv2_get_preset_count,
                                aap_lv2_get_preset_data_size,
                                aap_lv2_get_preset,
                                aap_lv2_get_preset_index,
                                aap_lv2_set_preset_index};

class AAPLV2PluginContext {
public:
    AAPLV2PluginContext(AAPLV2PluginContextStatics *statics, LilvWorld *world,
                        const LilvPlugin *plugin, const char* pluginUniqueId, int32_t sampleRate)
            : statics(statics), world(world), plugin(plugin), plugin_id(pluginUniqueId), sample_rate(sampleRate) {
        midi_atom_forge = (LV2_Atom_Forge *) calloc(1024, 1);
        symap = symap_new();
        // They don't have default assignment...
        worker.threaded = false;
        state_worker.threaded = false;
    }

    ~AAPLV2PluginContext() {
        for (auto p : midi_atom_buffers)
            free(p.second);
        free(midi_atom_forge);
        symap_free(symap);
    }

    AAPLV2PluginContextStatics *statics;
    AAPLv2PluginFeatures features;
    AAPLV2URIDs urids;
    LilvWorld *world;
    const LilvPlugin *plugin;
    LilvInstance *instance{nullptr};
    const char *plugin_id{nullptr};
    int32_t sample_rate;
    AndroidAudioPluginBuffer *cached_buffer{nullptr};
    void *dummy_raw_buffer{nullptr};
    int32_t midi_buffer_size = 1024;
    std::map<int32_t, LV2_Atom_Sequence *> midi_atom_buffers{};
    LV2_Atom_Forge *midi_atom_forge;
    LV2_URID ump_established_protocol{0}; // between this LV2 bridge and the actual LV2 plugin
    bool ipc_midi2_enabled{false}; // between host app and service (this bridge)
    aap_midi2_extension_t aap_midi2;
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

    void* getExtension(const char *uri) {
        if (strcmp(uri, AAP_PRESETS_EXTENSION_URI) == 0) {
            presets_ext.context = this;
            return &presets_ext;
        }
        return nullptr;
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


// imported from jalv_internal.h
typedef int (*PresetSink)(Jalv*           jalv,
                          const LilvNode* node,
                          const LilvNode* title,
                          void*           data);

// imported from jalv/src/state.c with some changes to match AAPLV2Context
int
jalv_load_presets(Jalv* jalv, PresetSink sink, void* data)
{
    LilvNodes* presets = lilv_plugin_get_related(jalv->plugin,
                                                 jalv->statics->presets_preset_node);
    LILV_FOREACH(nodes, i, presets) {
        const LilvNode* preset = lilv_nodes_get(presets, i);
        lilv_world_load_resource(jalv->world, preset);
        if (!sink) {
            continue;
        }

        LilvNodes* labels = lilv_world_find_nodes(
                jalv->world, preset, jalv->statics->rdfs_label_node, nullptr);
        if (labels) {
            const LilvNode* label = lilv_nodes_get_first(labels);
            sink(jalv, preset, label, data);
            lilv_nodes_free(labels);
        } else {
            fprintf(stderr, "Preset <%s> has no rdfs:label\n",
                    lilv_node_as_string(lilv_nodes_get(presets, i)));
        }
    }
    lilv_nodes_free(presets);

    return 0;
}

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

// The code below (jalv_worker_xxx) is copied from jalv worker.c and then made minimum required changes.

static LV2_Worker_Status
jalv_worker_respond(LV2_Worker_Respond_Handle handle,
                    uint32_t                  size,
                    const void*               data)
{
    JalvWorker* worker = (JalvWorker*)handle;
    zix_ring_write(worker->responses, (const char*)&size, sizeof(size));
    zix_ring_write(worker->responses, (const char*)data, size);
    return LV2_WORKER_SUCCESS;
}

static void*
worker_func(void* data)
{
    JalvWorker* worker = (JalvWorker*)data;
    Jalv*       jalv   = worker->ctx;
    void*       buf    = NULL;
    while (true) {
        zix_sem_wait(&worker->sem);
        if (jalv->exit) {
            break;
        }

        uint32_t size = 0;
        zix_ring_read(worker->requests, (char*)&size, sizeof(size));

        if (!(buf = realloc(buf, size))) {
            fprintf(stderr, "error: realloc() failed\n");
            free(buf);
            return NULL;
        }

        zix_ring_read(worker->requests, (char*)buf, size);

        zix_sem_wait(&jalv->work_lock);
        worker->iface->work(
                jalv->instance->lv2_handle, jalv_worker_respond, worker, size, buf);
        zix_sem_post(&jalv->work_lock);
    }

    free(buf);
    return NULL;
}

void
jalv_worker_init(Jalv*                       ZIX_UNUSED(jalv),
                 JalvWorker*                 worker,
                 const LV2_Worker_Interface* iface,
                 bool                        threaded)
{
    worker->iface = iface;
    worker->threaded = threaded;
    if (threaded) {
        zix_thread_create(&worker->thread, 4096, worker_func, worker);
        worker->requests = zix_ring_new(4096);
        zix_ring_mlock(worker->requests);
    }
    worker->responses = zix_ring_new(4096);
    worker->response  = malloc(4096);
    zix_ring_mlock(worker->responses);
}

void
jalv_worker_finish(JalvWorker* worker)
{
    if (worker->threaded) {
        zix_sem_post(&worker->sem);
        zix_thread_join(worker->thread, NULL);
    }
}

void
jalv_worker_destroy(JalvWorker* worker)
{
    if (worker->requests) {
        if (worker->threaded) {
            zix_ring_free(worker->requests);
        }
        zix_ring_free(worker->responses);
        free(worker->response);
    }
}

LV2_Worker_Status
jalv_worker_schedule(LV2_Worker_Schedule_Handle handle,
                     uint32_t                   size,
                     const void*                data)
{
    JalvWorker* worker = (JalvWorker*)handle;
    Jalv*       jalv   = worker->ctx;
    if (worker->threaded) {
        // Schedule a request to be executed by the worker thread
        zix_ring_write(worker->requests, (const char*)&size, sizeof(size));
        zix_ring_write(worker->requests, (const char*)data, size);
        zix_sem_post(&worker->sem);
    } else {
        // Execute work immediately in this thread
        zix_sem_wait(&jalv->work_lock);
        worker->iface->work(
                jalv->instance->lv2_handle, jalv_worker_respond, worker, size, data);
        zix_sem_post(&jalv->work_lock);
    }
    return LV2_WORKER_SUCCESS;
}

void
jalv_worker_emit_responses(JalvWorker* worker, LilvInstance* instance)
{
    if (worker->responses) {
        uint32_t read_space = zix_ring_read_space(worker->responses);
        while (read_space) {
            uint32_t size = 0;
            zix_ring_read(worker->responses, (char*)&size, sizeof(size));

            zix_ring_read(worker->responses, (char*)worker->response, size);

            worker->iface->work_response(
                    instance->lv2_handle, size, worker->response);

            read_space -= sizeof(size) + size;
        }
    }
}

// end of jalv worker code.

void aap_lv2_plugin_delete(
        AndroidAudioPluginFactory *,
        AndroidAudioPlugin *plugin) {
    auto l = (AAPLV2PluginContext *) plugin->plugin_specific;

    l->exit = true;

    // Terminate the worker
    jalv_worker_finish(&l->worker);

    // Destroy the worker
    jalv_worker_destroy(&l->worker);

    free(l->dummy_raw_buffer);
    lilv_instance_free(l->instance);
    lilv_node_free(l->statics->audio_port_uri_node);
    lilv_node_free(l->statics->control_port_uri_node);
    lilv_node_free(l->statics->atom_port_uri_node);
    lilv_node_free(l->statics->input_port_uri_node);
    lilv_node_free(l->statics->output_port_uri_node);
    lilv_node_free(l->statics->work_interface_uri_node);
    lilv_node_free(l->statics->presets_preset_node);
    lilv_node_free(l->statics->rdfs_label_node);
    lilv_node_free(l->statics->midi2_established_protocol_uri_node);
    lilv_node_free(l->statics->midi2_ump_uri_node);
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

    if (allocationPermitted) {
        if (!ctx->dummy_raw_buffer)
            ctx->dummy_raw_buffer = calloc(buffer->num_frames * sizeof(float), 1);

        //  >>> The buffer size could be specified using Buf Size extension, for Atom-specific ports.
        if (ctx->midi_buffer_size < buffer->num_frames) {
            for (auto p : ctx->midi_atom_buffers) {
                free(p.second);
                ctx->midi_atom_buffers[p.first] = (LV2_Atom_Sequence *) calloc(buffer->num_frames,
                                                                               1);
            }
        }
    }

    int numPorts = lilv_plugin_get_num_ports(lilvPlugin);
    for (int p = 0; p < numPorts; p++) {
        auto iter = ctx->midi_atom_buffers.find(p);
        auto bp = iter != ctx->midi_atom_buffers.end() ? iter->second : buffer->buffers[p];
        if (bp == nullptr)
            lilv_instance_connect_port(instance, p, ctx->dummy_raw_buffer);
        else
            lilv_instance_connect_port(instance, p, bp);
    }
}

void aap_lv2_plugin_prepare(AndroidAudioPlugin *plugin, AndroidAudioPluginBuffer *buffer) {
    auto l = (AAPLV2PluginContext *) plugin->plugin_specific;
    resetPorts(plugin, buffer, true);
    aap::a_log_f(AAP_LOG_LEVEL_INFO, "aap-lv2", "LV2 plugin %s is ready, prepared.", l->plugin_id);
}

void aap_lv2_plugin_activate(AndroidAudioPlugin *plugin) {
    auto l = (AAPLV2PluginContext *) plugin->plugin_specific;
    lilv_instance_activate(l->instance);
}

void
write_midi_events_to_lv2_forge(AAPLV2PluginContext* ctx, LV2_Atom_Forge *forge, LV2_Atom_Sequence *, int32_t numFrames,
                                   int32_t timeDivision, void *src) {
    assert(src != nullptr);
    assert(forge != nullptr);

    int32_t srcN = 8;

    auto csrc = (uint8_t *) src;
    int32_t srcEnd = *((int32_t *) src + 1) + 8; // offset

    uint8_t running_status = 0;

    uint64_t deltaTime = 0;

    while (srcN < srcEnd) {
        // MIDI Event message
        // Atom Event header
        uint64_t timecode = 0;
        uint digits = 0;
        while (csrc[srcN] >= 0x80 && srcN < srcEnd) // variable length
            timecode += ((uint32_t) (csrc[srcN++] - 0x80)) << (7 * digits++);
        if (srcN == srcEnd)
            break; // invalid data
        timecode += (csrc[srcN++] << (7 * digits));

        uint8_t statusByte = csrc[srcN] >= 0x80 ? csrc[srcN] : running_status;
        running_status = statusByte;
        uint8_t eventType = statusByte & 0xF0u;
        uint32_t midiEventSize = 3;
        int sysexPos = srcN;
        switch (eventType) {
        case 0xF0:
            midiEventSize = 2; // F0 + F7
            while (csrc[sysexPos++] != 0xF7 && sysexPos < srcEnd)
                midiEventSize++;
            break;
        case 0xC0:
        case 0xD0:
        case 0xF1:
        case 0xF3:
        case 0xF9:
            midiEventSize = 2;
            break;
        case 0xF6:
        case 0xF7:
            midiEventSize = 1;
            break;
        default:
            if (eventType > 0xF8)
                midiEventSize = 1;
            break;
        }

        if (timeDivision < 0) {
            // deltaTime is a frame count
            int32_t framesPerSecond = ctx->sample_rate;
            deltaTime += ((((timecode & 0xFF000000) >> 24) * 60 + ((timecode & 0xFF0000) >> 16)) * 60 + ((timecode & 0xFF00) >> 8) * timeDivision + (timecode & 0xFF));
            double timestamp = 1.0 * deltaTime / -timeDivision * framesPerSecond;
            lv2_atom_forge_frame_time(forge, timestamp);
        } else {
            // deltaTime is a beat based time
            deltaTime += timecode;
            lv2_atom_forge_beat_time(forge, (double) deltaTime / timeDivision * 120 / 60);
        }
        lv2_atom_forge_raw(forge, &midiEventSize, sizeof(int));
        lv2_atom_forge_raw(forge, &ctx->urids.urid_midi_event_type, sizeof(int));
        lv2_atom_forge_raw(forge, csrc + srcN, midiEventSize);
        lv2_atom_forge_pad(forge, midiEventSize);
        srcN += midiEventSize;
    }
}

void
write_midi2_events_as_midi1_to_lv2_forge(AAPLV2PluginContext* ctx, LV2_Atom_Forge *forge, int32_t timeDivision, void *src) {
    assert(src != nullptr);
    assert(forge != nullptr);

    // The header bytes are structured as:
    // - 0..3: int32_t size
    // - 4..7: reserved
    // - 8..11: MIDI-CI protocol (MIDI 1.0 or MIDI 2.0)
    // - 12..15: reserved
    int32_t srcN = 32;

    int32_t srcEnd = ((int32_t *) src)[0] + 32; // offset

    uint64_t currentJRTimestamp = 0; // unit of 1/31250 sec. (JR_TIMESTAMP_TICKS_PER_SECOND)

    uint8_t midi1Bytes[16];

    CMIDI2_UMP_SEQUENCE_FOREACH(src + srcN, srcEnd - srcN, iter) {
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

#if AAP_LV2_LOG_PERF
    struct timespec timeSpecBegin, timeSpecEnd;
    clock_gettime(CLOCK_REALTIME, &timeSpecBegin);
#endif

    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;

    if (buffer != ctx->cached_buffer)
        resetPorts(plugin, buffer, false);

    /* Process any worker replies. */
    jalv_worker_emit_responses(&ctx->state_worker, ctx->instance);
    jalv_worker_emit_responses(&ctx->worker, ctx->instance);

    /* Notify the plugin the run() cycle is finished */
    if (ctx->worker.iface && ctx->worker.iface->end_run)
        ctx->worker.iface->end_run(ctx->instance->lv2_handle);

    // convert AAP MIDI/MIDI2 messages into Atom Sequence of MidiEvent.
    for (auto p : ctx->midi_atom_buffers) {
        if (IS_OUTPUT_PORT(ctx, ctx->plugin, lilv_plugin_get_port_by_index(ctx->plugin, p.first))) {
            // For output ports. it must indicate the size of atom buffer.
            p.second->atom.size = buffer->num_frames * sizeof(float) - sizeof(LV2_Atom);
            continue;
        }
        void *src = buffer->buffers[p.first];
        auto uridMap = &ctx->features.urid_map_feature_data;
        auto forge = ctx->midi_atom_forge;
        lv2_atom_forge_init(forge, uridMap);
        lv2_atom_forge_set_buffer(forge, (uint8_t *) p.second, buffer->num_frames * sizeof(float));
        LV2_Atom_Forge_Frame frame;
        lv2_atom_sequence_clear(p.second);

        /*
         * There are 3 kinds of MIDI message processing involved:
         *
         * a) If the incoming input is MIDI2 (UMP),
         *   1) If the plugin supports UMP, then send it as MIDI2 Atom (for either of the protocols).
         *   2) If the plugin does not support UMP, then convert it to MIDI 1.0 Atom and send it.
         * b) If the incoming input is MIDI1, then send it as MIDI Atom.
         *
         * There may be b1) and b2) in the future: if the plugin only supports UMP, then convert it to UMP and send it.
         */

        if (ctx->ump_established_protocol == 0 && ctx->ipc_midi2_enabled) {
            auto seqRef = lv2_atom_forge_sequence_head(forge, &frame, ctx->urids.urid_time_frame);
            auto seq = (LV2_Atom_Sequence *) lv2_atom_forge_deref(forge, seqRef);
            lv2_atom_forge_pop(forge, &frame);
            write_midi2_events_as_midi1_to_lv2_forge(ctx, forge, buffer->num_frames, src);
            seq->atom.size = forge->offset - sizeof(LV2_Atom);
        } else {
            int32_t timeDivision = *((int *) src);
            auto seqRef = lv2_atom_forge_sequence_head(forge, &frame,
                                                       timeDivision > 0x7FFF
                                                       ? ctx->urids.urid_time_frame
                                                       : ctx->urids.urid_time_beats);
            auto seq = (LV2_Atom_Sequence *) lv2_atom_forge_deref(forge, seqRef);
            lv2_atom_forge_pop(forge, &frame);
            write_midi_events_to_lv2_forge(ctx, forge, seq, buffer->num_frames, timeDivision, src);
            seq->atom.size = forge->offset - sizeof(LV2_Atom);
        }
    }

    lilv_instance_run(ctx->instance, buffer->num_frames);

    // FIXME: there should be another set of converters that transforms LV2 MIDI outputs into AAP MIDI stream (either 1.0 or 2.0)

#if AAP_LV2_LOG_PERF
    clock_gettime(CLOCK_REALTIME, &timeSpecEnd);
    long timeDiff = (timeSpecEnd.tv_sec - timeSpecBegin.tv_sec) * 1000000000 + timeSpecEnd.tv_nsec - timeSpecBegin.tv_nsec;
    aap::a_log_f(AAP_LOG_LEVEL_DEBUG, "aap-lv2", AAP LV2Bridge Perf: time diff %ld / %ld", timeDiff, timeoutInNanoseconds);
#endif
}

void aap_lv2_plugin_deactivate(AndroidAudioPlugin *plugin) {
    auto l = (AAPLV2PluginContext *) plugin->plugin_specific;
    lilv_instance_deactivate(l->instance);
}

const void* aap_lv2_get_port_value(
        const char* port_symbol, void* user_data, uint32_t* size, uint32_t* type)
{
    auto l = (AAPLV2PluginContext *) user_data;
    auto uri = lilv_new_string(l->world, port_symbol);
    auto port = lilv_plugin_get_port_by_symbol(l->plugin, uri);
    lilv_node_free(uri);
    int index = lilv_port_get_index(l->plugin, port);

    // FIXME: preserve buffer in context, and retrieve from there.
    auto data = l->cached_buffer->buffers[index];

    // FIXME: implement correctly
    *size = sizeof(float);
    *type = l->urids.urid_atom_float_type;
    return data;
}

void aap_lv2_set_port_value(
        const char* port_symbol, void* user_data, const void* value, uint32_t size, uint32_t type)
{
    auto l = (AAPLV2PluginContext *) user_data;
    auto uri = lilv_new_string(l->world, port_symbol);
    auto port = lilv_plugin_get_port_by_symbol(l->plugin, uri);
    lilv_node_free(uri);
    int index = lilv_port_get_index(l->plugin, port);

    // FIXME: preserve buffer in context, and retrieve from there.
    auto data = l->cached_buffer->buffers[index];

    // should there be any type check?
    memcpy(data, value, size);
}

void aap_lv2_plugin_get_state(AndroidAudioPlugin *plugin, AndroidAudioPluginState *result) {
    auto l = (AAPLV2PluginContext *) plugin->plugin_specific;
    auto features = l->stateFeaturesList();
    LilvState *state = lilv_state_new_from_instance(l->plugin, l->instance, &l->features.urid_map_feature_data,
            nullptr, nullptr, nullptr, nullptr, aap_lv2_get_port_value, l, 0, features.get());
    auto nameNode = lilv_plugin_get_name(l->plugin);
    std::string stateUriBase{"urn:aap_state:"};
    auto nameChars = lilv_node_as_string(nameNode);
    std::string nameString{nameChars};
    std::string stateUri = stateUriBase + nameString;
    lilv_node_free(nameNode);
    auto stateString = lilv_state_to_string(l->world, &l->features.urid_map_feature_data, &l->features.urid_unmap_feature_data,
            state, stateUri.c_str(), nullptr);
    result->raw_data = strdup(stateString);
    result->data_size = strlen(stateString);
    lilv_state_delete(l->world, state);
}

void aap_lv2_plugin_set_state(AndroidAudioPlugin *plugin, AndroidAudioPluginState *input) {
    auto l = (AAPLV2PluginContext *) plugin->plugin_specific;
    LilvState *state = lilv_state_new_from_string(l->world, &l->features.urid_map_feature_data, (const char*) input->raw_data);
    auto features = l->stateFeaturesList();
    lilv_state_restore(state, l->instance, aap_lv2_set_port_value, l, 0, features.get());
    lilv_state_delete(l->world, state);
}

LV2_Worker_Status
aap_lv2_schedule_work(LV2_Worker_Schedule_Handle handle, uint32_t size, const void *data) {
    return jalv_worker_schedule(handle, size, data);
}

void* aap_lv2_plugin_get_extension(AndroidAudioPlugin *plugin, const char *uri) {
    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;
    return ctx->getExtension(uri);
}

AndroidAudioPlugin *aap_lv2_plugin_new(
        AndroidAudioPluginFactory *pluginFactory,
        const char *pluginUniqueID,
        int sampleRate,
        AndroidAudioPluginHost *host) {
    aap::a_log_f(AAP_LOG_LEVEL_INFO, "aap-lv2", "Instantiating aap-lv2 plugin %s", pluginUniqueID);

    auto world = lilv_world_new();
    // Here we expect that LV2_PATH is already set using setenv() etc.
    lilv_world_load_all(world);

    auto statics = new AAPLV2PluginContextStatics();
    statics->audio_port_uri_node = lilv_new_uri(world, LV2_CORE__AudioPort);
    statics->control_port_uri_node = lilv_new_uri(world, LV2_CORE__ControlPort);
    statics->input_port_uri_node = lilv_new_uri(world, LV2_CORE__InputPort);
    statics->output_port_uri_node = lilv_new_uri(world, LV2_CORE__OutputPort);
    statics->atom_port_uri_node = lilv_new_uri(world, LV2_ATOM__AtomPort);
    statics->work_interface_uri_node = lilv_new_uri(world, LV2_WORKER__interface);
    statics->midi2_established_protocol_uri_node = lilv_new_uri(world, LV2_MIDI2__establishedProtocol);
    statics->atom_supports_uri_node = lilv_new_uri(world, LV2_ATOM__supports);
    statics->presets_preset_node = lilv_new_uri(world, LV2_PRESETS__Preset);
    statics->rdfs_label_node = lilv_new_uri(world, LILV_NS_RDFS "label");
    statics->midi2_ump_uri_node = lilv_new_uri(world, LV2_MIDI2__UMP);

    auto allPlugins = lilv_world_get_all_plugins(world);
    if (lilv_plugins_size(allPlugins) <= 0) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, "aap-lv2", "No LV2 plugins were found.");
        return nullptr;
    }

    // LV2 Plugin URI is just LV2 URI prefixed by "lv2".
    if (strncmp(pluginUniqueID, "lv2:", strlen("lv2:"))) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, "aap-lv2", "Unexpected AAP LV2 pluginId: %s", pluginUniqueID);
        return nullptr;
    }
    auto pluginUriNode = lilv_new_uri(world, pluginUniqueID + strlen("lv2:"));
    const LilvPlugin *plugin = lilv_plugins_get_by_uri(allPlugins, pluginUriNode);
    lilv_node_free(pluginUriNode);
    if (!plugin) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, "aap-lv2", "LV2 plugin could not be instantiated: %s",
                     lilv_node_as_uri(pluginUriNode));
        return nullptr;
    }
    if (!lilv_plugin_verify(plugin)) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, "aap-lv2", "LV2 plugin is invalid: %s",
                     lilv_node_as_uri(pluginUriNode));
        return nullptr;
    }

    aap::a_log_f(AAP_LOG_LEVEL_INFO, "aap-lv2", "Plugin %s is valid, ready to instantiate.", pluginUniqueID);

    auto ctx = new AAPLV2PluginContext(statics, world, plugin, pluginUniqueID, sampleRate);

    auto midi2ext = (aap_midi2_extension_t*) host->get_extension_data(host, AAP_MIDI2_EXTENSION_URI);
    if (midi2ext != nullptr) {
        ctx->aap_midi2 = *midi2ext;
        if (midi2ext->protocol == AAP_PROTOCOL_MIDI2_0)
            // Right now, we switch to possible MIDI 2.0 protocol whenever client (host)
            // specifies so, regardless of whether the plugin supports it or not.
            // At this state we convert the incoming UMPs to MIDI 1.0 messages anyways,
            // and in the future the conversion will be applied optionally if the plugin
            // does not support MIDI 2.0.
            ctx->ipc_midi2_enabled = true;
    }

    ctx->features.urid_map_feature_data.handle = ctx;
    ctx->features.urid_map_feature_data.map = map_uri;
    ctx->features.urid_unmap_feature_data.handle = ctx;
    ctx->features.urid_unmap_feature_data.unmap = unmap_uri;

    if (zix_sem_init(&ctx->symap_lock, 1)) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, "aap-lv2", "Failed to initialize semaphore (symap). plugin: %s",
                     lilv_node_as_uri(pluginUriNode));
        return nullptr;
    }
    if (zix_sem_init(&ctx->work_lock, 1)) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, "aap-lv2", "Failed to initialize semaphore (work_lock). plugin: %s",
                     lilv_node_as_uri(pluginUriNode));
        return nullptr;
    }
    ctx->worker.ctx = ctx;
    ctx->state_worker.ctx = ctx;

    ctx->features.worker_schedule_data.handle = &ctx->worker;
    ctx->features.worker_schedule_data.schedule_work = aap_lv2_schedule_work;
    ctx->features.state_worker_schedule_data.handle = &ctx->state_worker;
    ctx->features.state_worker_schedule_data.schedule_work = aap_lv2_schedule_work;

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
            &ctx->features.midi2UmpFeature,
            &ctx->features.midi2Midi1ProtocolFeature,
            &ctx->features.midi2Midi2ProtocolFeature,
            nullptr
    };

    // for jalv worker
    if (zix_sem_init(&ctx->worker.sem, 0)) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, "aap-lv2", "Failed to initialize semaphore on worker. plugin: %s",
                     lilv_node_as_uri(pluginUriNode));
        return nullptr;
    }

    LilvInstance *instance = lilv_plugin_instantiate(plugin, sampleRate, features);
    if (!instance) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, "aap-lv2", "Failed to instantiate plugin: %s",
                     lilv_node_as_uri(pluginUriNode));
        return nullptr;
    }
    ctx->instance = instance;

    auto map = &ctx->features.urid_map_feature_data;
    if (!ctx->urids.urid_atom_sequence_type) {
        ctx->urids.urid_atom_sequence_type = map->map(map->handle, LV2_ATOM__Sequence);
        ctx->urids.urid_midi_event_type = map->map(map->handle, LV2_MIDI__MidiEvent);
        ctx->urids.urid_time_beats = map->map(map->handle, LV2_ATOM__beatTime);
        ctx->urids.urid_time_frame = map->map(map->handle, LV2_ATOM__frameTime);
        ctx->urids.urid_atom_float_type = map->map(map->handle, LV2_ATOM__Float);
        ctx->urids.urid_worker_interface = map->map(map->handle, LV2_WORKER__interface);
        ctx->urids.urid_midi2_midi1_protocol = map->map(map->handle, LV2_MIDI2__midi1Protocol);
        ctx->urids.urid_midi2_midi2_protocol = map->map(map->handle, LV2_MIDI2__midi2Protocol);
    }

    int nPorts = lilv_plugin_get_num_ports(plugin);
    bool hasUMPPort = false;
    for (int i = 0; i < nPorts; i++) {
        const LilvPort* portNode = lilv_plugin_get_port_by_index(plugin, i);
        if (IS_ATOM_PORT(ctx, plugin, portNode)) {
            LilvNodes* supports = lilv_port_get_value(plugin, portNode, ctx->statics->atom_supports_uri_node);
            if (!lilv_nodes_contains(supports, ctx->statics->midi2_ump_uri_node))
                hasUMPPort = true;
            ctx->midi_atom_buffers[i] = (LV2_Atom_Sequence *) calloc(ctx->midi_buffer_size, 1);
        }
    }

    /* Check for thread-safe state restore() method. */
    LilvNode* state_threadSafeRestore = lilv_new_uri(
            ctx->world, LV2_STATE__threadSafeRestore);
    if (lilv_plugin_has_feature(ctx->plugin, state_threadSafeRestore)) {
        ctx->safe_restore = true;
    }
    lilv_node_free(state_threadSafeRestore);

    if (lilv_plugin_has_extension_data(ctx->plugin, ctx->statics->work_interface_uri_node)) {
        const LV2_Worker_Interface* iface = (const LV2_Worker_Interface*)
                lilv_instance_get_extension_data(ctx->instance, LV2_WORKER__interface);

        jalv_worker_init(ctx, &ctx->worker, iface, true);
        if (ctx->safe_restore) {
            jalv_worker_init(ctx, &ctx->state_worker, iface, false);
        }
    }

    if (hasUMPPort && lilv_plugin_has_extension_data(ctx->plugin, ctx->statics->midi2_established_protocol_uri_node)) {
        ctx->ump_established_protocol = (LV2_URID) (int64_t) lilv_instance_get_extension_data(ctx->instance, LV2_MIDI2__establishedProtocol);
    }

    auto ret = new AndroidAudioPlugin{
            ctx,
            aap_lv2_plugin_prepare,
            aap_lv2_plugin_activate,
            aap_lv2_plugin_process,
            aap_lv2_plugin_deactivate,
            aap_lv2_plugin_get_state,
            aap_lv2_plugin_set_state,
            aap_lv2_plugin_get_extension
    };
    aap::a_log_f(AAP_LOG_LEVEL_INFO, "aap-lv2", "Instantiated aap-lv2 plugin %s", pluginUniqueID);
    return ret;
}

// Presets extension

int32_t aap_lv2_on_preset_loaded(Jalv* jalv, const LilvNode* node, const LilvNode* title, void* data) {
    auto presets_context = (aap_presets_context_t*) data;
    auto ctx = (AAPLV2PluginContext*) presets_context->context;
    // FIXME: implement (load and store them)
    return 0;
}

int32_t aap_lv2_get_preset_count(aap_presets_context_t* context) {
    auto ctx = ((AAPLV2PluginContext *) context->context);
    jalv_load_presets(ctx, aap_lv2_on_preset_loaded, context);
    return ctx->presets.size();
}
int32_t aap_lv2_get_preset_data_size(aap_presets_context_t* context, int32_t index) {
    auto ctx = ((AAPLV2PluginContext *) context->context);
}
void aap_lv2_get_preset(aap_presets_context_t* context, int32_t index, bool skipBinary, aap_preset_t* destination) {
    auto ctx = ((AAPLV2PluginContext *) context->context);
}
int32_t aap_lv2_get_preset_index(aap_presets_context_t* context) {
    auto ctx = ((AAPLV2PluginContext *) context->context);
}
void aap_lv2_set_preset_index(aap_presets_context_t* context, int32_t index) {
    auto ctx = ((AAPLV2PluginContext *) context->context);
}


} // namespace aaplv2bridge

AndroidAudioPluginFactory aap_lv2_factory{aaplv2bridge::aap_lv2_plugin_new,
                                          aaplv2bridge::aap_lv2_plugin_delete,
                                          nullptr};

extern "C" {

AndroidAudioPluginFactory *GetAndroidAudioPluginFactoryLV2Bridge() { return &aap_lv2_factory; }

} // extern "C"
