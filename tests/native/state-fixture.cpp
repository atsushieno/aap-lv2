#include <lv2/core/lv2.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/atom/atom.h>
#include <lv2/options/options.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static void check(bool condition) { if (!condition) { fputs("invalid fixture state feature\n", stderr); std::abort(); } }
struct Instance { LV2_URID key, type; uint32_t counter{0}; float* audio{nullptr}; };
static LV2_Handle instantiate(const LV2_Descriptor*, double, const char*, const LV2_Feature* const* features) {
    auto result = new Instance{};
    for (auto f = features; f && *f; ++f) {
        if (!strcmp((*f)->URI, LV2_URID__map)) {
            auto map = static_cast<LV2_URID_Map*>((*f)->data);
            result->key = map->map(map->handle, "urn:aap:test:counter");
            result->type = map->map(map->handle, LV2_ATOM__Int);
        }
    }
    check(result->key && result->type);
    return result;
}
static LV2_State_Status save(LV2_Handle handle, LV2_State_Store_Function store, LV2_State_Handle state,
                            uint32_t, const LV2_Feature* const* features) {
    // Consume the whole production feature list, including its terminator.
    unsigned count = 0;
    for (auto f = features; f && *f; ++f) {
        check((*f)->URI); check(++count < 16);
        if (!strcmp((*f)->URI, LV2_OPTIONS__options) && (*f)->data) {
            auto options = static_cast<const LV2_Options_Option*>((*f)->data);
            check(options[0].size == sizeof(int) && options[0].value);
            check(options[1].size == sizeof(int) && options[1].value);
            check(!options[2].key);
        }
    }
    check(count >= 6);
    auto instance = static_cast<Instance*>(handle);
    return store(state, instance->key, &instance->counter, sizeof(instance->counter), instance->type,
                 LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}
static LV2_State_Status restore(LV2_Handle handle, LV2_State_Retrieve_Function retrieve,
                               LV2_State_Handle state, uint32_t, const LV2_Feature* const*) {
    auto instance = static_cast<Instance*>(handle);
    size_t size = 0; uint32_t type = 0, flags = 0;
    auto value = retrieve(state, instance->key, &size, &type, &flags);
    if (!value || size != sizeof(instance->counter) || type != instance->type) return LV2_STATE_ERR_BAD_TYPE;
    memcpy(&instance->counter, value, size);
    return LV2_STATE_SUCCESS;
}
static LV2_State_Interface stateInterface{save, restore};
static LV2_Descriptor descriptor{
    "urn:aap:test:state", instantiate,
    [](LV2_Handle handle, uint32_t port, void* data) { if (port == 1) static_cast<Instance*>(handle)->audio = static_cast<float*>(data); },
    nullptr,
    [](LV2_Handle handle, uint32_t frames) {
        auto instance = static_cast<Instance*>(handle); ++instance->counter;
        if (frames && instance->audio) instance->audio[0] = instance->counter;
    }, nullptr,
    [](LV2_Handle handle) { delete static_cast<Instance*>(handle); },
    [](const char* uri) -> const void* { return !strcmp(uri, LV2_STATE__interface) ? &stateInterface : nullptr; }
};
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) { return index == 0 ? &descriptor : nullptr; }
