
#include <unistd.h>
#include <memory>
#include <cstring>
#include <cassert>
#include <vector>
#include <map>
#include <string>

#include <lilv/lilv.h>

#include <aap/unstable/logging.h>
#include <aap/android-audio-plugin.h>
#include <aap/ext/presets.h>
#include <aap/ext/state.h>

#include "aap-lv2-internal.h"

namespace aaplv2bridge {

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

// State extension

// Lilv state port values are scalar controls, never audio/CV/Atom buffers.
// An unprepared snapshot omits these values; plugin-owned state can still save.
static const LilvPort* aap_lv2_state_control_port(AAPLV2PluginContext* context, const char* symbol) {
    if (!context || !context->world || !context->plugin || !context->statics ||
        !context->control_buffer_pointers || !symbol) return nullptr;
    auto node = lilv_new_string(context->world, symbol);
    if (!node) return nullptr;
    auto port = lilv_plugin_get_port_by_symbol(context->plugin, node);
    lilv_node_free(node);
    if (!port || lilv_port_get_index(context->plugin, port) >= lilv_plugin_get_num_ports(context->plugin) ||
        !context->IS_CONTROL_PORT(context->plugin, port)) return nullptr;
    return port;
}

const void* aap_lv2_get_port_value(const char* symbol, void* userData, uint32_t* size, uint32_t* type) {
    if (size) *size = 0;
    if (type) *type = 0;
    if (!size || !type) return nullptr;
    auto context = static_cast<AAPLV2PluginContext*>(userData);
    auto port = aap_lv2_state_control_port(context, symbol);
    if (!port) return nullptr;
    *size = sizeof(float);
    *type = context->urids.urid_atom_float_type;
    return context->control_buffer_pointers + lilv_port_get_index(context->plugin, port);
}

void aap_lv2_set_port_value(const char* symbol, void* userData, const void* value, uint32_t size, uint32_t type) {
    auto context = static_cast<AAPLV2PluginContext*>(userData);
    auto port = aap_lv2_state_control_port(context, symbol);
    if (!port || !value || !type) return;
    float control;
    // Turtle presets may use integer or double literals. Convert numeric atoms
    // to the ControlPort's float instead of copying their representation.
    if (type == context->urids.urid_atom_float_type && size == sizeof(float)) {
        memcpy(&control, value, sizeof(control));
    } else if (type == context->urids.urid_atom_int_type && size == sizeof(int32_t)) {
        int32_t number; memcpy(&number, value, sizeof(number)); control = static_cast<float>(number);
    } else if (type == context->urids.urid_atom_long_type && size == sizeof(int64_t)) {
        int64_t number; memcpy(&number, value, sizeof(number)); control = static_cast<float>(number);
    } else if (type == context->urids.urid_atom_double_type && size == sizeof(double)) {
        double number; memcpy(&number, value, sizeof(number)); control = static_cast<float>(number);
    } else {
        return;
    }
    context->control_buffer_pointers[lilv_port_get_index(context->plugin, port)] = control;
}

// Returns a heap-allocated Turtle string (free() by the caller), or nullptr on failure.
static char* aap_lv2_build_state_string(AAPLV2PluginContext* l) {
    auto features = l->stateFeaturesList();
    LilvState *state = lilv_state_new_from_instance(l->plugin, l->instance, &l->features.urid_map_feature_data,
                                                    nullptr, nullptr, nullptr, nullptr, aap_lv2_get_port_value, l, 0, features.data());
    if (!state)
        return nullptr;
    // Display names may contain spaces or other characters forbidden in IRIs.
    std::string stateUri = "urn:aap_state:";
    stateUri += lilv_node_as_uri(lilv_plugin_get_uri(l->plugin));
    auto stateString = lilv_state_to_string(l->world, &l->features.urid_map_feature_data, &l->features.urid_unmap_feature_data,
                                            state, stateUri.c_str(), nullptr);
    lilv_state_free(state);
    return stateString;
}

size_t aap_lv2_get_state_size(aap_state_extension_t* ext, AndroidAudioPlugin* plugin) {
    auto l = (AAPLV2PluginContext *) plugin->plugin_specific;
    char* stateString = aap_lv2_build_state_string(l);
    size_t ret = stateString ? strlen(stateString) : 0;
    free(stateString);
    return ret;
}

void aap_lv2_get_state(aap_state_extension_t* ext, AndroidAudioPlugin* plugin, aap_state_t *result) {
    auto l = (AAPLV2PluginContext *) plugin->plugin_specific;
    char* stateString = aap_lv2_build_state_string(l);
    if (!stateString) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG,
                     "aap_lv2_get_state: failed to serialize state for %s", l->aap_plugin_id.c_str());
        result->data = strdup("");
        result->data_size = 0;
        return;
    }
    result->data = stateString;
    result->data_size = strlen(stateString);
}

void aap_lv2_set_state(aap_state_extension_t* ext, AndroidAudioPlugin* plugin, aap_state_t *input) {
    auto l = (AAPLV2PluginContext *) plugin->plugin_specific;
    if (!input || !input->data || input->data_size == 0) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "aap_lv2_set_state: empty state payload");
        return;
    }
    LilvState *state = lilv_state_new_from_string(l->world, &l->features.urid_map_feature_data, (const char*) input->data);
    if (!state) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG,
                     "aap_lv2_set_state: could not parse LV2 state for %s", l->aap_plugin_id.c_str());
        return;
    }
    auto features = l->stateFeaturesList();
    lilv_state_restore(state, l->instance, aap_lv2_set_port_value, l, 0, features.data());
    lilv_state_free(state);
    l->markAllParameterValuesDirty();
}

// Presets extension

int32_t aap_lv2_on_preset_loaded(Jalv* jalv, const LilvNode* node, const LilvNode* title, void* data) {
    auto name = lilv_node_as_string(title);
    auto uridMap = &jalv->features.urid_map_feature_data;
    auto uridUnmap = &jalv->features.urid_unmap_feature_data;
    auto state = lilv_state_new_from_world(jalv->world, uridMap, node);
    auto stateData = lilv_state_to_string(jalv->world, uridMap, uridUnmap, state, lilv_node_as_string(node), nullptr);

    aap_preset_t preset;
    preset.id = (int32_t) jalv->presets.size();
    strncpy(preset.name, name, AAP_PRESETS_EXTENSION_MAX_NAME_LENGTH);
    jalv->presets.emplace_back(std::make_unique<AAPPresetAndLv2Binary>(preset, strdup(stateData)));
    aap::a_log_f(AAP_LOG_LEVEL_DEBUG, "AAP-LV2", "aap_lv2_on_preset_loaded. %s: %s", name, lilv_node_as_string(node));
    free(stateData);
    lilv_state_free(state);
    return 0;
}

void aap_lv2_ensure_preset_loaded(AAPLV2PluginContext *ctx) {
    if (ctx->presets.size() == 0)
        jalv_load_presets(ctx, aap_lv2_on_preset_loaded, nullptr);
}

int32_t aap_lv2_get_preset_count(aap_presets_extension_t* ext, AndroidAudioPlugin* plugin) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    aap_lv2_ensure_preset_loaded(ctx);
    return ctx->presets.size();
}
void aap_lv2_get_preset(aap_presets_extension_t* ext, AndroidAudioPlugin* plugin, int32_t index, aap_preset_t* destination, aapxs_completion_callback callback, void* callbackContext) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    aap_lv2_ensure_preset_loaded(ctx);
    auto preset = ctx->presets[index].get();
    strncpy(destination->name, preset->preset.name, AAP_PRESETS_EXTENSION_MAX_NAME_LENGTH);
    if (callback)
        callback(callbackContext, plugin);
}
void aap_lv2_set_preset_index(aap_presets_extension_t* ext, AndroidAudioPlugin* plugin, int32_t index) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    aap_lv2_ensure_preset_loaded(ctx);

    for (auto& p : ctx->presets) {
        if (p->preset.id == index) {
            auto state = lilv_state_new_from_string(ctx->world,
                                                    &ctx->features.urid_map_feature_data,
                                                    (const char *) p->data);
            lilv_state_restore(state, ctx->instance, aap_lv2_set_port_value, ctx, 0, ctx->stateFeaturesList().data());
            lilv_state_free(state);
            break;
        }
    }
    ctx->markAllParameterValuesDirty();
}

// parameters extension

int32_t aap_lv2_get_parameter_count(aap_parameters_extension_t* ext, AndroidAudioPlugin *plugin) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    return ctx->getAAPParameterCount();
}

aap_parameter_info_t aap_lv2_get_parameter(aap_parameters_extension_t* ext, AndroidAudioPlugin *plugin, int32_t index) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    return ctx->getAAPParameterInfo(index);
}

double aap_lv2_get_parameter_property(aap_parameters_extension_t* ext, AndroidAudioPlugin *plugin, int32_t parameterId, int32_t propertyId) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    return ctx->getAAPParameterProperty(parameterId, propertyId);
}

int32_t aap_lv2_get_enumeration_count(aap_parameters_extension_t* ext, AndroidAudioPlugin *plugin, int32_t parameterId) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    return ctx->getAAPEnumerationCount(parameterId);
}

aap_parameter_enum_t aap_lv2_get_enumeration(aap_parameters_extension_t* ext, AndroidAudioPlugin *plugin, int32_t parameterId, int32_t enumIndex) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    return ctx->getAAPEnumeration(parameterId, enumIndex);
}

aap_parameters_extension_t params_ext{nullptr,
                                      aap_lv2_get_parameter_count,
                                      aap_lv2_get_parameter,
                                      aap_lv2_get_parameter_property,
                                      aap_lv2_get_enumeration_count,
                                      aap_lv2_get_enumeration
                                      };

aap_state_extension_t state_ext{nullptr,
                                aap_lv2_get_state_size,
                                aap_lv2_get_state,
                                aap_lv2_set_state};

aap_presets_extension_t presets_ext{nullptr,
                                    aap_lv2_get_preset_count,
                                    aap_lv2_get_preset,
                                    aap_lv2_set_preset_index};

int32_t aap_lv2_get_bus_count(aap_buses_extension_t* ext, AndroidAudioPlugin* plugin,
                              aap_bus_kind kind, aap_port_direction direction) {
    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;
    return kind == AAP_BUS_KIND_AUDIO ? (int32_t) ctx->audio_buses[direction == AAP_PORT_DIRECTION_INPUT ? 0 : 1].size() : 0;
}

aap_bus_info_t aap_lv2_get_bus(aap_buses_extension_t* ext, AndroidAudioPlugin* plugin,
                               aap_bus_kind kind, aap_port_direction direction, int32_t index) {
    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;
    aap_bus_info_t info{};
    auto& buses = ctx->audio_buses[direction == AAP_PORT_DIRECTION_INPUT ? 0 : 1];
    if (kind != AAP_BUS_KIND_AUDIO || index < 0 || (size_t) index >= buses.size())
        return info;
    auto& bus = buses[(size_t) index];
    info.id = bus.id;
    info.kind = AAP_BUS_KIND_AUDIO;
    info.direction = direction;
    info.role = index == 0 ? AAP_BUS_ROLE_MAIN : AAP_BUS_ROLE_AUX;
    strncpy(info.name, bus.name.c_str(), AAP_MAX_BUS_NAME_CHARS - 1);
    info.channel_count = (int32_t) bus.lv2_ports.size();
    // the layout is left empty; the framework fills the default one for the channel count.
    info.enabled = true;
    return info;
}

aap_buses_extension_t buses_ext{nullptr,
                                aap_lv2_get_bus_count,
                                aap_lv2_get_bus,
                                nullptr};

void* aap_lv2_plugin_get_extension(AndroidAudioPlugin *plugin, const char *uri) {
    if (strcmp(uri, AAP_PARAMETERS_EXTENSION_URI) == 0) {
        return &params_ext;
    }
    if (strcmp(uri, AAP_STATE_EXTENSION_URI) == 0) {
        return &state_ext;
    }
    if (strcmp(uri, AAP_PRESETS_EXTENSION_URI) == 0) {
        return &presets_ext;
    }
    if (strcmp(uri, AAP_BUSES_EXTENSION_URI) == 0) {
        return &buses_ext;
    }
    return nullptr;
}

aap_plugin_info_t aap_lv2_plugin_get_plugin_info(AndroidAudioPlugin* plugin) {
    auto ctx = ((AAPLV2PluginContext *) plugin->plugin_specific);
    auto hostExt = (aap_host_plugin_info_extension_t*) ctx->aap_host->get_extension(ctx->aap_host, AAP_PLUGIN_INFO_EXTENSION_URI);
    return hostExt->get(hostExt, ctx->aap_host, ctx->aap_plugin_id.c_str());
}

// AAP factory members
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

AndroidAudioPlugin *aap_lv2_plugin_new(
        AndroidAudioPluginFactory *pluginFactory,
        const char *pluginUniqueID,
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

    auto ctx = new AAPLV2PluginContext(host, statics, world, plugin, pluginUniqueID);

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

    // FIXME: adjust those variables at prepare() step.
    ctx->features.options[0] = ctx->features.minBlockLengthOption;
    ctx->features.options[1] = ctx->features.maxBlockLengthOption;
    ctx->features.options[2] = LV2_Options_Option{LV2_OPTIONS_BLANK, 0, 0, 0, 0};
    ctx->features.optionsFeature.data = ctx->features.options.data();

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

    LilvInstance *instance = lilv_plugin_instantiate(plugin, ctx->sample_rate, features);
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
        ctx->urids.urid_atom_int_type = map->map(map->handle, LV2_ATOM__Int);
        ctx->urids.urid_atom_long_type = map->map(map->handle, LV2_ATOM__Long);
        ctx->urids.urid_atom_double_type = map->map(map->handle, LV2_ATOM__Double);
        ctx->urids.urid_patch_set = map->map(map->handle, LV2_PATCH__Set);
        ctx->urids.urid_patch_property = map->map(map->handle, LV2_PATCH__property);
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
            aap_lv2_plugin_get_extension,
            aap_lv2_plugin_get_plugin_info,
    };
    aap::a_log_f(AAP_LOG_LEVEL_INFO, AAP_LV2_TAG, "Instantiated aap-lv2 plugin %s", pluginUniqueID);
    return ret;
}

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

    lilv_instance_free(l->instance);
    delete l->statics;
    lilv_world_free(l->world);
    delete l;
    delete plugin;
}

}
