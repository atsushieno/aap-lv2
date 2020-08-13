#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/urid/urid.h>
#include <lv2/midi/midi.h>
#include <lv2/state/state.h>
#include "fluidsynth.h"
#include "fluid_androidasset.h"
#include <jni.h>
#include "aap/android-context.h"

#define FLUIDSYNTH_LV2_URI "https://github.com/atsushieno/aap-fluidsynth"
#define FLUIDSYNTH_LV2_ATOM_INPUT_PORT 0
#define FLUIDSYNTH_LV2_AUDIO_OUT_LEFT 1
#define FLUIDSYNTH_LV2_AUDIO_OUT_RIGHT 2

typedef struct {
	LV2_URID_Map *urid_map;
	LV2_URID midi_event_uri;
	fluid_settings_t *settings;
	fluid_synth_t *synth;
	fluid_sfloader_t *asset_sfloader;
    JNIEnv *jnienv;
	double sample_rate;
	const char * bundle_path;
	float* ports[3];
} FluidsynthLV2Handle;

LV2_Handle fluidsynth_lv2_instantiate(
		const LV2_Descriptor * descriptor,
		double sample_rate,
		const char * bundle_path,
		const LV2_Feature *const * features) {

	FluidsynthLV2Handle* handle = (FluidsynthLV2Handle*) calloc(sizeof(FluidsynthLV2Handle), 1);
	handle->sample_rate = sample_rate;
	handle->bundle_path = strdup(bundle_path);

	handle->urid_map = NULL;
	for (int i = 0; features[i]; i++) {
		const LV2_Feature* f = features[i];
		if (!strcmp(f->URI, LV2_URID__map))
			handle->urid_map = (LV2_URID_Map*) f->data;
	}
	assert(handle->urid_map);
	handle->midi_event_uri = handle->urid_map->map(handle->urid_map->handle, LV2_MIDI__MidiEvent);

    auto settings = new_fluid_settings();
    handle->settings = settings;
    fluid_settings_setnum(settings, "synth.gain", 1.0);
    fluid_settings_setnum(settings, "synth.sample-rate", sample_rate);
    fluid_settings_setint(settings, "synth.threadsafe-api", 0);
    auto synth = new_fluid_synth(settings);
    handle->synth = synth;

    JNIEnv* env;
    JavaVM* vm = aap::get_android_jvm();
    vm->AttachCurrentThread(&env, NULL);
    handle->jnienv = env;
    auto assetManager = aap::get_android_asset_manager(env);
    handle->asset_sfloader = new_fluid_android_asset_sfloader(settings, assetManager);
    fluid_synth_add_sfloader(synth, handle->asset_sfloader);
    //Java_fluidsynth_androidextensions_NativeHandler_setAssetManagerContext()

    // TODO: load soundfonts here.

	return handle;
}

void fluidsynth_lv2_connect_port(
		LV2_Handle instance,
		uint32_t port,
		void * data_location) {
	FluidsynthLV2Handle* a = (FluidsynthLV2Handle*) instance;
	a->ports[port] = (float*) data_location;
}

void fluidsynth_lv2_activate(LV2_Handle instance) {
}

void fluidsynth_lv2_process_midi_event(FluidsynthLV2Handle *a, const LV2_Atom_Event *ev) {

    // TODO: implement
}

void fluidsynth_lv2_run(LV2_Handle instance, uint32_t sample_count) {
	FluidsynthLV2Handle* a = (FluidsynthLV2Handle*) instance;

	LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*) a->ports[FLUIDSYNTH_LV2_ATOM_INPUT_PORT];

	LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
		if (ev->body.type == a->midi_event_uri) {
			puts("MIDI EVENT");
			fluidsynth_lv2_process_midi_event(a, ev);
		}
	}

    float* out[2] = {a->ports[FLUIDSYNTH_LV2_AUDIO_OUT_LEFT], a->ports[FLUIDSYNTH_LV2_AUDIO_OUT_RIGHT]};
    fluid_synth_process(a->synth, sample_count, 0, NULL, sample_count, out);
}

void fluidsynth_lv2_deactivate(LV2_Handle instance) {
}

void fluidsynth_lv2_cleanup(LV2_Handle instance) {
	FluidsynthLV2Handle* a = (FluidsynthLV2Handle*) instance;
	delete_fluid_sfloader(a->asset_sfloader);
    JavaVM* vm = aap::get_android_jvm();
    vm->DetachCurrentThread();
    delete_fluid_synth(a->synth);
    delete_fluid_settings(a->settings);
	free((void*) a->bundle_path);
	free(a);
}

const void * fluidsynth_lv2_extension_data(const char * uri) {
	return NULL;
}

const LV2_Descriptor fluidsynth_lv2 = {
	FLUIDSYNTH_LV2_URI,
	fluidsynth_lv2_instantiate,
	fluidsynth_lv2_connect_port,
	fluidsynth_lv2_activate,
	fluidsynth_lv2_run,
	fluidsynth_lv2_deactivate,
	fluidsynth_lv2_cleanup,
	fluidsynth_lv2_extension_data
};

const LV2_Descriptor * lv2_descriptor(uint32_t index)
{
	return &fluidsynth_lv2;
}

