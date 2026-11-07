// An LV2 plugin that reports what it observes through its audio output, one value per sample:
// [0] free-wheeling port, [1] gain port, [2] the last worker response (a block number),
// [3] whether that work ran on the thread that calls run().
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lv2/core/lv2.h"
#include "lv2/worker/worker.h"

typedef struct {
    const float* gain;
    const float* free_wheel;
    float* out;
    LV2_Worker_Schedule* schedule;
    pthread_t run_thread;
    int32_t block;
    int32_t last_response;
    int32_t last_inline;
} Probe;

typedef struct {
    int32_t block;
    int32_t ran_inline;
} Response;

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* path, const LV2_Feature* const* features) {
    Probe* self = (Probe*) calloc(1, sizeof(Probe));
    for (int i = 0; features[i]; i++)
        if (!strcmp(features[i]->URI, LV2_WORKER__schedule))
            self->schedule = (LV2_Worker_Schedule*) features[i]->data;
    return self;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
    Probe* self = (Probe*) h;
    if (port == 0) self->gain = (const float*) data;
    if (port == 1) self->free_wheel = (const float*) data;
    if (port == 2) self->out = (float*) data;
}

static void run(LV2_Handle h, uint32_t frames) {
    Probe* self = (Probe*) h;
    self->run_thread = pthread_self();
    self->out[0] = *self->free_wheel;
    self->out[1] = *self->gain;
    self->out[2] = (float) self->last_response;
    self->out[3] = (float) self->last_inline;
    self->block++;
    self->schedule->schedule_work(self->schedule->handle, sizeof(self->block), &self->block);
}

// Slower than the test's blocks, so an asynchronous result always arrives late.
static LV2_Worker_Status work(LV2_Handle h, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle handle, uint32_t size, const void* data) {
    Probe* self = (Probe*) h;
    usleep(20000);
    Response response = {*(const int32_t*) data, pthread_equal(self->run_thread, pthread_self()) ? 1 : 0};
    return respond(handle, sizeof(response), &response);
}

static LV2_Worker_Status work_response(LV2_Handle h, uint32_t size, const void* data) {
    Probe* self = (Probe*) h;
    const Response* response = (const Response*) data;
    self->last_response = response->block;
    self->last_inline = response->ran_inline;
    return LV2_WORKER_SUCCESS;
}

static const void* extension_data(const char* uri) {
    static const LV2_Worker_Interface iface = {work, work_response, NULL};
    return strcmp(uri, LV2_WORKER__interface) ? NULL : &iface;
}

static void cleanup(LV2_Handle h) { free(h); }

static const LV2_Descriptor descriptor = {"urn:aap-test:offline-probe", instantiate, connect_port, NULL, run, NULL, cleanup, extension_data};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) { return index == 0 ? &descriptor : NULL; }
