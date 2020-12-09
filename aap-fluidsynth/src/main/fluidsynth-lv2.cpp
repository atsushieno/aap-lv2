#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/urid/urid.h>
#include <lv2/log/log.h>
#include <lv2/midi/midi.h>
#include <lv2/state/state.h>
#include "fluidsynth.h"
#include "fluid_androidasset.h"
#include <jni.h>
#include <string>
#include "aap/android-context.h"

#define FLUIDSYNTH_LV2_URI "https://github.com/atsushieno/aap-fluidsynth"
#define FLUIDSYNTH_LV2_ATOM_INPUT_PORT 0
#define FLUIDSYNTH_LV2_AUDIO_OUT_LEFT 1
#define FLUIDSYNTH_LV2_AUDIO_OUT_RIGHT 2
#define DEFAULT_SOUNDFONTS {"/FluidR3Mono_GM.sf3"}

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
	LV2_Log_Log *log = NULL;
	for (int i = 0; features[i]; i++) {
		const LV2_Feature* f = features[i];
		if (!strcmp(f->URI, LV2_URID__map))
			handle->urid_map = (LV2_URID_Map*) f->data;
		if (!strcmp(f->URI, LV2_LOG__log))
		    log = (LV2_Log_Log*) f->data;
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

    const char* defaultSoundfonts[] DEFAULT_SOUNDFONTS;
    int sfid;
    for (auto sf : defaultSoundfonts) {
        sfid = fluid_synth_sfload(synth, sf, 0);
        if (sfid < 0 && log) {
            std::string msg{"unable to open soundfont: "};
            log->printf(log->handle, 0, "%s", (msg + sf).c_str());
        }
    }

    auto status = fluid_synth_program_select(synth, 0, sfid, 0, 0); // initialize prog/bank to 0.
    assert(status == FLUID_OK);

    // it is what FluidPlug does - boostrap synth engine
    float l[1024];
    float r[1024];
    fluid_synth_write_float(synth, 1024, l, 0, 1, r, 0, 1);

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
	FluidsynthLV2Handle* a = (FluidsynthLV2Handle*) instance;
}

void fluidsynth_lv2_process_midi_event(FluidsynthLV2Handle *a, const LV2_Atom_Event *ev) {
    int noise, tone_switch, noise_switch, env_switch;
    const uint8_t *const msg = (const uint8_t *)(ev + 1);
    int channel = msg[0] & 0xF;
    int mixer;
    fluid_synth_t* synth = a->synth;
    switch (lv2_midi_message_type(msg)) {
    case LV2_MIDI_MSG_NOTE_OFF:
        fluid_synth_noteoff(synth, channel, msg[1]);
        break;
    case LV2_MIDI_MSG_NOTE_ON:
        fluid_synth_noteon(synth, channel, msg[1], msg[2]);
        break;
    case LV2_MIDI_MSG_CHANNEL_PRESSURE:
        fluid_synth_channel_pressure(synth, channel, msg[1]);
        break;
    case LV2_MIDI_MSG_CONTROLLER:
        fluid_synth_cc(synth, channel, msg[1], msg[2]);
        break;
    case LV2_MIDI_MSG_PGM_CHANGE:
        fluid_synth_program_change(synth, channel, msg[1]);
        break;
    case LV2_MIDI_MSG_NOTE_PRESSURE:
        fluid_synth_key_pressure(synth, channel, msg[1], msg[2]);
        break;
    case LV2_MIDI_MSG_BENDER:
        fluid_synth_pitch_bend(synth, channel, msg[1] + msg[2] * 0x80);
        break;
    case LV2_MIDI_MSG_RESET:
        fluid_synth_system_reset(synth);
        break;
    case LV2_MIDI_MSG_SYSTEM_EXCLUSIVE:
        fluid_synth_sysex(synth, (const char*) const_cast<const uint8_t *>(msg), ev->body.size, nullptr, nullptr, nullptr, 0);
        break;
    }
}

void fluidsynth_lv2_run(LV2_Handle instance, uint32_t sample_count) {
	FluidsynthLV2Handle* a = (FluidsynthLV2Handle*) instance;

	LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*) a->ports[FLUIDSYNTH_LV2_ATOM_INPUT_PORT];

	int curFrame = 0;

	LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
		if (ev->body.type == a->midi_event_uri) {
		    // FIXME: process beats
		    if (ev->time.frames > curFrame) {
                fluid_synth_write_float(a->synth, ev->time.frames - curFrame,
                                        a->ports[FLUIDSYNTH_LV2_AUDIO_OUT_LEFT], curFrame, 1,
                                        a->ports[FLUIDSYNTH_LV2_AUDIO_OUT_RIGHT], curFrame, 1);
                curFrame = ev->time.frames;
            }
			fluidsynth_lv2_process_midi_event(a, ev);
		}
	}

	if (curFrame < sample_count)
        fluid_synth_write_float(a->synth, sample_count - curFrame,
            a->ports[FLUIDSYNTH_LV2_AUDIO_OUT_LEFT], curFrame,1,
            a->ports[FLUIDSYNTH_LV2_AUDIO_OUT_RIGHT], curFrame, 1);
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

