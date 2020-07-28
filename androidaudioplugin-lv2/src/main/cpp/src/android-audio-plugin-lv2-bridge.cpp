
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

#include "aap/logging.h"
#include "aap/android-audio-plugin.h"

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

#define JUCEAAP_LOG_PERF 0

namespace aaplv2bridge {

typedef struct {
    bool operator()(std::string &p1, std::string &p2) const { return p1 == p2; }
} uricomp;

// WARNING: NEVER EVER use this function and URID feature variable for loading and saving state.
// State must be preserved in stable semantics, and this function and internal map are never
// stable. The value of the mapped integers change every time we make changes to this code.
LV2_URID urid_map_func(LV2_URID_Map_Handle handle, const char *uri) {
    auto map = static_cast<std::map<std::string, LV2_URID> *> (handle);
    std::string s{uri};
    auto it = map->find(s);
    if (it == map->end())
        map->emplace(s, map->size() + 1000);
    return map->find(s)->second;
}

int log_vprintf(LV2_Log_Handle, LV2_URID type, const char *fmt, va_list ap) {
    int ret = aap::aprintf("LV2 LOG (%d): ", type);
    ret += aap::aprintf(fmt, ap);
    return ret;
}

int log_printf(LV2_Log_Handle handle, LV2_URID type, const char *fmt, ...) {
    va_list ap;
    va_start (ap, fmt);
    return log_vprintf(handle, type, fmt, ap);
}

typedef struct {
    LilvNode *audio_port_uri_node, *control_port_uri_node, *atom_port_uri_node, *input_port_uri_node, *output_port_uri_node, *work_interface_uri_node;

} AAPLV2PluginContextStatics;

// imported from jalv
typedef struct {
    void*                       ctx;       ///< Pointer back to AAPLV2PluginContext
    ZixRing*                    requests{nullptr};   ///< Requests to the worker
    ZixRing*                    responses{nullptr};  ///< Responses from the worker
    void*                       response;   ///< Worker response buffer
    ZixSem                      sem;        ///< Worker semaphore
    ZixThread                   thread;     ///< Worker thread
    const LV2_Worker_Interface* iface;      ///< Plugin worker interface
    bool                        threaded;   ///< Run work in another thread
} JalvWorker;

class AAPLv2PluginFeatures {
public:
    inline void set(LV2_Feature& feature, const char* uri, void* data) {
        feature.URI = uri;
        feature.data = data;
    }

    std::map<std::string, LV2_URID, uricomp> urid_map{};
    LV2_URID_Map urid_map_feature_data{&urid_map, urid_map_func};
    LV2_Worker_Schedule worker_schedule_data{};
    LV2_Worker_Schedule state_worker_schedule_data{};
    LV2_Log_Log logData{nullptr, log_printf, log_vprintf};

    const int minBlockLengthValue = 128;
    const int maxBlockLengthValue = 8192;
    LV2_Options_Option minBlockLengthOption{LV2_OPTIONS_INSTANCE,
                0,
                urid_map_func(&urid_map, LV2_BUF_SIZE__minBlockLength),
                sizeof(int),
                urid_map_func(&urid_map, LV2_ATOM__Int),
                &minBlockLengthValue};
    LV2_Options_Option maxBlockLengthOption{LV2_OPTIONS_INSTANCE,
                0,
                urid_map_func(&urid_map, LV2_BUF_SIZE__maxBlockLength),
                sizeof(int),
                urid_map_func(&urid_map, LV2_ATOM__Int),
                &maxBlockLengthValue};

    LV2_Feature uridFeature{LV2_URID__map, &urid_map_feature_data};
    LV2_Feature logFeature{LV2_LOG__log, &logData};
    LV2_Feature bufSizeFeature{LV2_BUF_SIZE__boundedBlockLength, nullptr};
    LV2_Feature optionsFeature{LV2_OPTIONS__options, nullptr};
    LV2_Feature workerFeature{LV2_WORKER__schedule, &worker_schedule_data};
    LV2_Feature stateWorkerFeature{LV2_WORKER__schedule, &state_worker_schedule_data};
    LV2_Feature threadSafeRestoreFeature{LV2_STATE__threadSafeRestore, nullptr};
};

struct AAPLV2URIDs {
    LV2_URID urid_atom_sequence_type{0},
        urid_midi_event_type{0},
        urid_time_frame{0},
        urid_time_beats{0},
        urid_worker_interface{0};
};

class AAPLV2PluginContext {
public:
    AAPLV2PluginContext(AAPLV2PluginContextStatics *statics, LilvWorld *world,
                        const LilvPlugin *plugin)
            : statics(statics), world(world), plugin(plugin), instance(instance) {
        midi_atom_forge = (LV2_Atom_Forge *) calloc(1024, 1);
    }

    ~AAPLV2PluginContext() {
        for (auto p : midi_atom_buffers)
            free(p.second);
        free(midi_atom_forge);
    }

    AAPLV2PluginContextStatics *statics;
    AAPLv2PluginFeatures features;
    AAPLV2URIDs urids;
    LilvWorld *world;
    const LilvPlugin *plugin;
    LilvInstance *instance;
    AndroidAudioPluginBuffer *cached_buffer{nullptr};
    void *dummy_raw_buffer{nullptr};
    int32_t midi_buffer_size = 1024;
    std::map<int32_t, LV2_Atom_Sequence *> midi_atom_buffers{};
    LV2_Atom_Forge *midi_atom_forge;

    // from jalv codebase
    JalvWorker         worker;         ///< Worker thread implementation
    JalvWorker         state_worker;   ///< Synchronous worker for state restore
    ZixSem             work_lock;      ///< Lock for plugin work() method
    bool               safe_restore;   ///< Plugin restore() is thread-safe
    bool terminate{false};
};

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

// The code below (jalv_xxx) is imported from jalv and then modified.


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
    auto       ctx   = (AAPLV2PluginContext*) worker->ctx;
    void*       buf    = NULL;
    while (true) {
        zix_sem_wait(&worker->sem);
        if (ctx->terminate) {
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

        zix_sem_wait(&ctx->work_lock);
        worker->iface->work(
                ctx->instance->lv2_handle, jalv_worker_respond, worker, size, buf);
        zix_sem_post(&ctx->work_lock);
    }

    free(buf);
    return NULL;
}

void
jalv_worker_init(AAPLV2PluginContext*,
JalvWorker*                 worker,
const LV2_Worker_Interface* iface,
bool                        threaded) {
    worker->iface = iface;
    worker->threaded = threaded;
    if (threaded) {
        zix_thread_create(&worker->thread, 4096, worker_func, worker);
        worker->requests = zix_ring_new(4096);
        zix_ring_mlock(worker->requests);
    }
    worker->responses = zix_ring_new(4096);
    worker->response = malloc(4096);
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
    auto       ctx   = (AAPLV2PluginContext*) worker->ctx;
    if (worker->threaded) {
        // Schedule a request to be executed by the worker thread
        zix_ring_write(worker->requests, (const char*)&size, sizeof(size));
        zix_ring_write(worker->requests, (const char*)data, size);
        zix_sem_post(&worker->sem);
    } else {
        // Execute work immediately in this thread
        zix_sem_wait(&ctx->work_lock);
        worker->iface->work(
                ctx->instance->lv2_handle, jalv_worker_respond, worker, size, data);
        zix_sem_post(&ctx->work_lock);
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
    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;

    ctx->terminate = true;

    // Terminate the worker
    jalv_worker_finish(&ctx->worker);

    // Destroy the worker
    jalv_worker_destroy(&ctx->worker);

    auto l = (AAPLV2PluginContext *) plugin->plugin_specific;
    free(l->dummy_raw_buffer);
    lilv_instance_free(l->instance);
    lilv_node_free(l->statics->audio_port_uri_node);
    lilv_node_free(l->statics->control_port_uri_node);
    lilv_node_free(l->statics->atom_port_uri_node);
    lilv_node_free(l->statics->input_port_uri_node);
    lilv_node_free(l->statics->output_port_uri_node);
    lilv_node_free(l->statics->work_interface_uri_node);
    delete l->statics;
    lilv_world_free(l->world);
    delete l;
    delete plugin;
}

void resetPorts(AndroidAudioPlugin *plugin, AndroidAudioPluginBuffer *buffer) {
    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;
    auto lilvPlugin = ctx->plugin;
    auto instance = ctx->instance;

    auto dummyBuffer = calloc(buffer->num_frames * sizeof(float), 1);
    ctx->dummy_raw_buffer = dummyBuffer;

    ctx->cached_buffer = buffer;

    assert(buffer != nullptr);

    // FIXME: <del>it is quite awkward to reset buffer size to whatever value for audio I/O but
    //  there is no any reasonable alternative value to reuse. Maybe something like 0x1000 is enough
    //  (but what happens if there are MPE-like massive messages?)</del>
    //  >>> The buffer size could be specified using Buf Size extension, for Atom-specific ports.
    if (ctx->midi_buffer_size < buffer->num_frames) {
        for (auto p : ctx->midi_atom_buffers) {
            free(p.second);
            ctx->midi_atom_buffers[p.first] = (LV2_Atom_Sequence *) calloc(buffer->num_frames, 1);
        }
    }

    int numPorts = lilv_plugin_get_num_ports(lilvPlugin);
    for (int p = 0; p < numPorts; p++) {
        auto iter = ctx->midi_atom_buffers.find(p);
        auto bp = iter != ctx->midi_atom_buffers.end() ? iter->second : buffer->buffers[p];
        if (bp == nullptr)
            lilv_instance_connect_port(instance, p, dummyBuffer);
        else
            lilv_instance_connect_port(instance, p, bp);
    }
}

void aap_lv2_plugin_prepare(AndroidAudioPlugin *plugin, AndroidAudioPluginBuffer *buffer) {
    resetPorts(plugin, buffer);
}

void aap_lv2_plugin_activate(AndroidAudioPlugin *plugin) {
    auto l = (AAPLV2PluginContext *) plugin->plugin_specific;
    lilv_instance_activate(l->instance);
}

// returns true if there was at least one MIDI message in src.
void
normalize_midi_event_for_lv2_forge(AAPLV2PluginContext* ctx, LV2_Atom_Forge *forge, LV2_Atom_Sequence *, int32_t numFrames,
                                   int32_t timeDivision, void *src) {
    assert(src != nullptr);
    assert(forge != nullptr);

    int srcN = 8;

    auto csrc = (uint8_t *) src;
    int32_t srcEnd = *((int *) src + 1) + 8; // offset

    unsigned char running_status = 0;

    uint64_t ticks = 0;

    // This is far from precise. No support for sysex and meta, no run length.
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
            uint8_t ticksPerFrame = -timeDivision;
            ticks += ((((timecode & 0xFF000000u) >> 24u) * 60 + ((timecode & 0xFF0000u) >> 16u)) *
                      60 +
                      ((timecode & 0xFF00u) >> 8u) * ticksPerFrame + (timecode & 0xFFu));
            lv2_atom_forge_frame_time(forge, ticks * numFrames / ticksPerFrame);
        } else {
            ticks += timecode;
            lv2_atom_forge_beat_time(forge, (double) ticks / timeDivision * 120 / 60);
        }
        lv2_atom_forge_raw(forge, &midiEventSize, sizeof(int));
        lv2_atom_forge_raw(forge, &ctx->urids.urid_midi_event_type, sizeof(int));
        lv2_atom_forge_raw(forge, csrc + srcN, midiEventSize);
        lv2_atom_forge_pad(forge, midiEventSize);
        srcN += midiEventSize;
    }
}

void aap_lv2_plugin_process(AndroidAudioPlugin *plugin,
                            AndroidAudioPluginBuffer *buffer,
                            long timeoutInNanoseconds) {
    // FIXME: use timeoutInNanoseconds?

#if JUCEAAP_LOG_PERF
    struct timespec timeSpecBegin, timeSpecEnd;
    clock_gettime(CLOCK_REALTIME, &timeSpecBegin);
#endif

    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;

    if (buffer != ctx->cached_buffer)
        resetPorts(plugin, buffer);

    /* Process any worker replies. */
    jalv_worker_emit_responses(&ctx->state_worker, ctx->instance);
    jalv_worker_emit_responses(&ctx->worker, ctx->instance);

    /* Notify the plugin the run() cycle is finished */
    if (ctx->worker.iface && ctx->worker.iface->end_run)
        ctx->worker.iface->end_run(ctx->instance->lv2_handle);

    // convert AAP MIDI messages into Atom Sequence of MidiEvent.
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
        int32_t timeDivision = *((int *) src);
        auto seqRef = lv2_atom_forge_sequence_head(forge, &frame,
                                                   timeDivision > 0x7FFF ? ctx->urids.urid_time_frame
                                                                         : ctx->urids.urid_time_beats);
        auto seq = (LV2_Atom_Sequence *) lv2_atom_forge_deref(forge, seqRef);
        lv2_atom_forge_pop(forge, &frame);
        normalize_midi_event_for_lv2_forge(ctx, forge, seq, buffer->num_frames, timeDivision, src);
        seq->atom.size = forge->offset - sizeof(LV2_Atom);
    }

    lilv_instance_run(ctx->instance, buffer->num_frames);

#if JUCEAAP_LOG_PERF
    clock_gettime(CLOCK_REALTIME, &timeSpecEnd);
    long timeDiff = (timeSpecEnd.tv_sec - timeSpecBegin.tv_sec) * 1000000000 + timeSpecEnd.tv_nsec - timeSpecBegin.tv_nsec;
    aap::aprintf("AAP LV2Bridge Perf: time diff %ld / %ld", timeDiff, timeoutInNanoseconds);
#endif
}

void aap_lv2_plugin_deactivate(AndroidAudioPlugin *plugin) {
    auto l = (AAPLV2PluginContext *) plugin->plugin_specific;
    lilv_instance_deactivate(l->instance);
}

void aap_lv2_plugin_get_state(AndroidAudioPlugin *plugin, AndroidAudioPluginState *result) {
    assert(false); // FIXME: implement
}

void aap_lv2_plugin_set_state(AndroidAudioPlugin *plugin, AndroidAudioPluginState *input) {
    assert(false); // FIXME: implement
}

LV2_Worker_Status
aap_lv2_schedule_work(LV2_Worker_Schedule_Handle handle, uint32_t size, const void *data) {
    return jalv_worker_schedule(handle, size, data);
}

AndroidAudioPlugin *aap_lv2_plugin_new(
        AndroidAudioPluginFactory *pluginFactory,
        const char *pluginUniqueID,
        int sampleRate,
        AndroidAudioPluginExtension **extensions) {
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

    auto allPlugins = lilv_world_get_all_plugins(world);
    assert(lilv_plugins_size(allPlugins) > 0);

    // LV2 Plugin URI is just LV2 URI prefixed by "lv2".
    assert (!strncmp(pluginUniqueID, "lv2:", strlen("lv2:")));
    auto pluginUriNode = lilv_new_uri(world, pluginUniqueID + strlen("lv2:"));
    const LilvPlugin *plugin = lilv_plugins_get_by_uri(allPlugins, pluginUriNode);
    lilv_node_free(pluginUriNode);
    assert (plugin);
    assert (lilv_plugin_verify(plugin));

    auto ctx = std::make_unique<AAPLV2PluginContext>(statics, world, plugin);

    assert(!zix_sem_init(&ctx->work_lock, 1));
    ctx->worker.ctx = ctx.get();
    ctx->state_worker.ctx = ctx.get();

    ctx->features.worker_schedule_data.handle = &ctx->worker;
    ctx->features.worker_schedule_data.schedule_work = aap_lv2_schedule_work;
    ctx->features.state_worker_schedule_data.handle = &ctx->state_worker;
    ctx->features.state_worker_schedule_data.schedule_work = aap_lv2_schedule_work;

    LV2_Options_Option options[3];

    // FIXME: adjust those variables at prepare() step.
    options[0] = ctx->features.minBlockLengthOption;
    options[1] = ctx->features.maxBlockLengthOption;
    options[2] = LV2_Options_Option{LV2_OPTIONS_BLANK, 0, 0, 0, 0};
    ctx->features.optionsFeature.data = &options;

    LV2_Feature* features[] = {
            &ctx->features.uridFeature,
            &ctx->features.logFeature,
            &ctx->features.bufSizeFeature,
            &ctx->features.optionsFeature,
            &ctx->features.threadSafeRestoreFeature,
            &ctx->features.workerFeature,
            // FIXME: enable this
            //&ctx->features.stateWorkerFeature,
            nullptr
    };

    // for jalv worker
    assert(!zix_sem_init(&ctx->worker.sem, 0));

    LilvInstance *instance = lilv_plugin_instantiate(plugin, sampleRate, features);
    assert (instance);
    ctx->instance = instance;

    auto map = &ctx->features.urid_map_feature_data;
    // Fixed value list of URID map. If it breaks then saved state will be lost!
    if (!ctx->urids.urid_atom_sequence_type) {
        ctx->urids.urid_atom_sequence_type = map->map(map->handle, LV2_ATOM__Sequence);
        ctx->urids.urid_midi_event_type = map->map(map->handle, LV2_MIDI__MidiEvent);
        ctx->urids.urid_time_beats = map->map(map->handle, LV2_ATOM__beatTime);
        ctx->urids.urid_time_frame = map->map(map->handle, LV2_ATOM__frameTime);
        ctx->urids.urid_worker_interface = map->map(map->handle, LV2_WORKER__interface);
    }

    int nPorts = lilv_plugin_get_num_ports(plugin);
    for (int i = 0; i < nPorts; i++) {
        if (IS_ATOM_PORT(ctx.get(), plugin, lilv_plugin_get_port_by_index(plugin, i))) {
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

    // for jalv worker
    // Create workers if necessary
    if (lilv_plugin_has_extension_data(ctx->plugin, ctx->statics->work_interface_uri_node)) {
        const LV2_Worker_Interface* iface = (const LV2_Worker_Interface*)
                lilv_instance_get_extension_data(ctx->instance, LV2_WORKER__interface);

        jalv_worker_init(ctx.get(), &ctx->worker, iface, true);
        if (ctx->safe_restore) {
            jalv_worker_init(ctx.get(), &ctx->state_worker, iface, false);
        }
    }

    return new AndroidAudioPlugin{
            ctx.release(),
            aap_lv2_plugin_prepare,
            aap_lv2_plugin_activate,
            aap_lv2_plugin_process,
            aap_lv2_plugin_deactivate,
            aap_lv2_plugin_get_state,
            aap_lv2_plugin_set_state
    };
}

} // namespace aaplv2bridge

AndroidAudioPluginFactory aap_lv2_factory{aaplv2bridge::aap_lv2_plugin_new,
                                          aaplv2bridge::aap_lv2_plugin_delete};

extern "C" {

AndroidAudioPluginFactory *GetAndroidAudioPluginFactoryLV2Bridge() { return &aap_lv2_factory; }

} // extern "C"
