
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

// State extension

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

size_t aap_lv2_get_state_size(AndroidAudioPluginExtensionTarget target) {
    auto l = (AAPLV2PluginContext *) target.plugin->plugin_specific;
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
    auto ret = strlen(stateString);
    lilv_state_delete(l->world, state);
    return ret;
}

void aap_lv2_get_state(AndroidAudioPluginExtensionTarget target, aap_state_t *result) {
    auto l = (AAPLV2PluginContext *) target.plugin->plugin_specific;
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
    result->data = strdup(stateString);
    result->data_size = strlen(stateString);
    lilv_state_delete(l->world, state);
}

void aap_lv2_set_state(AndroidAudioPluginExtensionTarget target, aap_state_t *input) {
    auto l = (AAPLV2PluginContext *) target.plugin->plugin_specific;
    LilvState *state = lilv_state_new_from_string(l->world, &l->features.urid_map_feature_data, (const char*) input->data);
    auto features = l->stateFeaturesList();
    lilv_state_restore(state, l->instance, aap_lv2_set_port_value, l, 0, features.get());
    lilv_state_delete(l->world, state);
}

// Presets extension

int32_t aap_lv2_on_preset_loaded(Jalv* jalv, const LilvNode* node, const LilvNode* title, void* data) {
    auto name = lilv_node_as_string(title);
    auto uridMap = &jalv->features.urid_map_feature_data;
    auto uridUnmap = &jalv->features.urid_unmap_feature_data;
    auto state = lilv_state_new_from_world(jalv->world, uridMap, node);
    auto stateData = lilv_state_to_string(jalv->world, uridMap, uridUnmap, state, name, nullptr);

    aap_preset_t preset;
    preset.index = (int32_t) jalv->presets.size();
    strncpy(preset.name, name, AAP_PRESETS_EXTENSION_MAX_NAME_LENGTH);
    preset.data_size = (int32_t) strlen(const_cast<const char*>(stateData));
    preset.data = strdup(stateData);
    jalv->presets.emplace_back(std::make_unique<aap_preset_t>(preset));
    aap::a_log_f(AAP_LOG_LEVEL_DEBUG, "AAP-LV2", "aap_lv2_on_preset_loaded. %s: %s", name, lilv_node_as_string(node));
    free((void *) name);
    free(stateData);
    lilv_state_free(state);
    return 0;
}

void aap_lv2_ensure_preset_loaded(AAPLV2PluginContext *ctx) {
    if (ctx->presets.size() == 0)
        jalv_load_presets(ctx, aap_lv2_on_preset_loaded, nullptr);
}

int32_t aap_lv2_get_preset_count(AndroidAudioPluginExtensionTarget target) {
    auto ctx = ((AAPLV2PluginContext *) target.plugin->plugin_specific);
    aap_lv2_ensure_preset_loaded(ctx);
    return ctx->presets.size();
}
int32_t aap_lv2_get_preset_data_size(AndroidAudioPluginExtensionTarget target, int32_t index) {
    auto ctx = ((AAPLV2PluginContext *) target.plugin->plugin_specific);
    aap_lv2_ensure_preset_loaded(ctx);
    return ctx->presets[index]->data_size;
}
void aap_lv2_get_preset(AndroidAudioPluginExtensionTarget target, int32_t index, bool skipBinary, aap_preset_t* destination) {
    auto ctx = ((AAPLV2PluginContext *) target.plugin->plugin_specific);
    aap_lv2_ensure_preset_loaded(ctx);
    auto preset = ctx->presets[index].get();
    destination->data_size = preset->data_size;
    strncpy(destination->name, preset->name, AAP_PRESETS_EXTENSION_MAX_NAME_LENGTH);
    if (!skipBinary)
        memcpy(destination->data, preset->data, preset->data_size);
}
int32_t aap_lv2_get_preset_index(AndroidAudioPluginExtensionTarget target) {
    auto ctx = ((AAPLV2PluginContext *) target.plugin->plugin_specific);
    aap_lv2_ensure_preset_loaded(ctx);
    return ctx->selected_preset_index;
}
void aap_lv2_set_preset_index(AndroidAudioPluginExtensionTarget target, int32_t index) {
    auto ctx = ((AAPLV2PluginContext *) target.plugin->plugin_specific);
    aap_lv2_ensure_preset_loaded(ctx);
    ctx->selected_preset_index = index;
}


