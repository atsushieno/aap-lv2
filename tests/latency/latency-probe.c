// An LV2 plugin that reports its "delay" control input as its latency, as LV2 plugins do in run().
#include <stdlib.h>
#include <string.h>
#include "lv2/core/lv2.h"

typedef struct {
    const float* delay;
    float* latency;
    float* out;
} Probe;

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* path, const LV2_Feature* const* features) {
    return calloc(1, sizeof(Probe));
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
    Probe* self = (Probe*) h;
    if (port == 0) self->delay = (const float*) data;
    if (port == 1) self->latency = (float*) data;
    if (port == 2) self->out = (float*) data;
}

static void run(LV2_Handle h, uint32_t frames) {
    Probe* self = (Probe*) h;
    *self->latency = *self->delay;
    memset(self->out, 0, frames * sizeof(float));
}

static void cleanup(LV2_Handle h) { free(h); }

static const LV2_Descriptor descriptor = {"urn:aap-test:latency-probe", instantiate, connect_port, NULL, run, NULL, cleanup, NULL};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) { return index == 0 ? &descriptor : NULL; }
