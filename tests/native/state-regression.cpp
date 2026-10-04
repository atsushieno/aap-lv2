#include <stdexcept>
#include <filesystem>
// Exercise the callbacks and AAP state entry points in their production file.
#include "aap-lv2-extensions.cpp"
using namespace aaplv2bridge;
static void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main(int argc, char** argv) {
    check(argc == 2, "bundle argument");
    auto world = lilv_world_new();
    auto bundle = lilv_new_file_uri(world, nullptr, argv[1]);
    lilv_world_load_bundle(world, bundle); lilv_node_free(bundle);
    auto uri = lilv_new_uri(world, "urn:aap:test:state");
    auto plugin = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world), uri); lilv_node_free(uri);
    check(plugin, "real Lilv fixture loaded");
    {
        AAPLV2PluginContextStatics statics(world);
        AAPLV2PluginContext context(nullptr, &statics, world, plugin, "lv2:urn:aap:test:state");
        context.features.urid_map_feature_data = {context.symap,
            [](void* handle, const char* uri) { return symap_map(static_cast<Symap*>(handle), uri); }};
        context.features.urid_unmap_feature_data = {context.symap,
            [](void* handle, uint32_t id) { return symap_unmap(static_cast<Symap*>(handle), id); }};
        context.urids.urid_atom_float_type = symap_map(context.symap, LV2_ATOM__Float);
        context.urids.urid_atom_int_type = symap_map(context.symap, LV2_ATOM__Int);
        context.urids.urid_atom_long_type = symap_map(context.symap, LV2_ATOM__Long);
        context.urids.urid_atom_double_type = symap_map(context.symap, LV2_ATOM__Double);
        auto features = context.stateFeaturesList();
        context.instance = lilv_plugin_instantiate(plugin, 48000, features.data());
        check(context.instance, "real LV2 instance");
        AndroidAudioPlugin aap{}; aap.plugin_specific = &context;
        uint32_t size = 99, type = 99;
        check(!aap_lv2_get_port_value("gain", &context, &size, &type) && !size && !type, "unprepared port omitted");
        aap_state_t unprepared{}; aap_lv2_get_state(nullptr, &aap, &unprepared);
        check(unprepared.data_size && strstr(static_cast<char*>(unprepared.data), "counter"), "unprepared plugin-owned state captured");
        check(!strstr(static_cast<char*>(unprepared.data), "gain"), "unprepared controls absent from snapshot");
        free(unprepared.data);
        context.control_buffer_pointers = static_cast<float*>(calloc(2, sizeof(float)));
        context.control_buffer_pointers[0] = 1.25f; context.control_buffer_pointers[1] = 37;
        for (auto symbol : {"missing", "audio", static_cast<const char*>(nullptr)}) {
            size = type = 99;
            check(!aap_lv2_get_port_value(symbol, &context, &size, &type) && !size && !type, "invalid getter rejected");
            float input = 9;
            aap_lv2_set_port_value(symbol, &context, &input, sizeof(input), context.urids.urid_atom_float_type);
        }
        float input[2]{9, 10};
        for (auto bytes : {0u, 3u, 8u})
            aap_lv2_set_port_value("gain", &context, input, bytes, context.urids.urid_atom_float_type);
        aap_lv2_set_port_value("gain", &context, input, 4, context.urids.urid_atom_float_type + 100);
        aap_lv2_set_port_value("gain", &context, nullptr, 4, context.urids.urid_atom_float_type);
        check(context.control_buffer_pointers[0] == 1.25f && context.control_buffer_pointers[1] == 37, "malformed restores leave controls intact");
        check(!aap_lv2_get_port_value("gain", &context, nullptr, &type), "missing metadata pointer rejected");
        // Exercise Sratom's actual numeric literal types, including integer
        // and double preset values which must not be bit-copied as floats.
        for (const auto* literal : {"2", "\"2\"^^<http://www.w3.org/2001/XMLSchema#long>",
                                   "\"2.0\"^^<http://www.w3.org/2001/XMLSchema#double>"}) {
            std::string ttl = "@prefix lv2: <http://lv2plug.in/ns/lv2core#> . "
                "@prefix pset: <http://lv2plug.in/ns/ext/presets#> . "
                "<urn:aap:test:preset> a pset:Preset ; lv2:appliesTo <urn:aap:test:state> ; "
                "lv2:port [ lv2:symbol \"gain\" ; pset:value ";
            ttl += literal; ttl += " ] .";
            auto preset = lilv_state_new_from_string(world, &context.features.urid_map_feature_data, ttl.c_str());
            check(preset, "numeric preset parsed by real Lilv");
            lilv_state_emit_port_values(preset, aap_lv2_set_port_value, &context);
            check(context.control_buffer_pointers[0] == 2, "numeric preset converted to float");
            lilv_state_free(preset);
        }
        context.control_buffer_pointers[0] = 1.25f;
        float audio = 0;
        lilv_instance_connect_port(context.instance, 0, context.control_buffer_pointers);
        lilv_instance_connect_port(context.instance, 1, &audio);
        lilv_instance_activate(context.instance); lilv_instance_run(context.instance, 1); lilv_instance_deactivate(context.instance);
        check(audio == 1, "fixture processed");
        for (int i = 0; i < 20; ++i) {
            check(aap_lv2_get_state_size(nullptr, &aap) > 0, "size after processing and deactivation");
            aap_state_t snapshot{}; aap_lv2_get_state(nullptr, &aap, &snapshot);
            check(snapshot.data && snapshot.data_size, "state after processing and deactivation");
            context.control_buffer_pointers[0] = 0;
            aap_lv2_set_state(nullptr, &aap, &snapshot);
            check(context.control_buffer_pointers[0] == 1.25f, "control restored through real Lilv");
            free(snapshot.data);
        }
        lilv_instance_run(context.instance, 1); check(audio == 2, "plugin-owned counter restored");
        lilv_instance_free(context.instance);
    }
    lilv_world_free(world);
    // The complete production factory must keep its options alive after return.
    std::filesystem::path bundlePath(argv[1]);
    if (bundlePath.filename().empty()) bundlePath = bundlePath.parent_path();
    setenv("LV2_PATH", bundlePath.parent_path().c_str(), 1);
    AndroidAudioPluginHost host{};
    auto created = aap_lv2_plugin_new(nullptr, "lv2:urn:aap:test:state", &host);
    check(created, "production factory instantiated fixture");
    auto context = static_cast<AAPLV2PluginContext*>(created->plugin_specific);
    check(context->features.optionsFeature.data == context->features.options.data(), "options owned by instance");
    check(context->features.options[0].key && context->features.options[1].key && !context->features.options[2].key,
          "terminated options list survives factory return");
    aap_state_t snapshot{}; aap_lv2_get_state(nullptr, created, &snapshot);
    check(snapshot.data && snapshot.data_size, "factory snapshot before preparation");
    aap_lv2_set_state(nullptr, created, &snapshot); free(snapshot.data);
    aap_lv2_plugin_delete(nullptr, created);
    puts("PASS: real Lilv state before preparation and after run/deactivation, malformed port values, repeated save/restore");
}
