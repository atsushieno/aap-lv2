
#include "aap-lv2-internal.h"
#if ANDROID
#include <android/trace.h>
#endif

namespace aaplv2bridge {

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


void resetMidiAtomBuffers(AAPLV2PluginContext* ctx, aap_buffer_t* buffer, std::map<int32_t, LV2_Atom_Sequence*> &map, bool isInput) {
    for (auto p : map) {
        lv2_atom_sequence_clear(p.second);
        // it seems we have to keep those sequences clean and assign appropriate sizes.
        auto bufferSize =
                ctx->explicit_port_buffer_sizes.find(p.first) == ctx->explicit_port_buffer_sizes.end() ?
                buffer->num_frames(*buffer) * sizeof(float) :
                ctx->explicit_port_buffer_sizes[p.first];
        auto *forge = isInput ? &ctx->midi_forges_in[p.first] : &ctx->midi_forges_out[p.first];
        lv2_atom_forge_set_buffer(forge, (uint8_t *) p.second, bufferSize);
        // FIXME: do we need this? Feels unnecessary
        //p.second->atom.size = bufferSize - sizeof(LV2_Atom);

        LV2_Atom_Forge_Frame frame;
        auto seqRef = lv2_atom_forge_sequence_head(forge, &frame, ctx->urids.urid_time_frame);
        auto seq = (LV2_Atom_Sequence *) lv2_atom_forge_deref(forge, seqRef);
        lv2_atom_sequence_clear(seq);
        lv2_atom_forge_pop(forge, &frame);
    }
}

void resetPatchAtomBuffer(AAPLV2PluginContext* ctx, aap_buffer_t* buffer, int32_t port) {
    if (port < 0)
        return;
    auto buf = ctx->explicitly_allocated_port_buffers[port];
    if (!buf)
        buf = buffer->get_buffer(*buffer, ctx->mappings.lv2_to_aap_portmap[port]);
    auto seq = static_cast<LV2_Atom_Sequence*>(buf);
    lv2_atom_sequence_clear(seq);
    // it seems we have to keep those sequences clean and assign appropriate sizes.
    auto bufferSize =
            ctx->explicit_port_buffer_sizes.find(port) == ctx->explicit_port_buffer_sizes.end() ?
            buffer->num_frames(*buffer) * sizeof(float) :
            ctx->explicit_port_buffer_sizes[port];
    seq->atom.size = bufferSize - sizeof(LV2_Atom);
}

void allocatePortBuffers(AndroidAudioPlugin *plugin, aap_buffer_t *buffer) {
    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;
    auto lilvPlugin = ctx->plugin;
    auto instance = ctx->instance;

    assert(buffer != nullptr);

    uint32_t numLV2Ports = lilv_plugin_get_num_ports(lilvPlugin);

    auto aapPluginExt = (aap_host_plugin_info_extension_t *) ctx->aap_host->get_extension(
            ctx->aap_host, AAP_PLUGIN_INFO_EXTENSION_URI);
    assert(aapPluginExt);
    auto aapPluginInfo = aapPluginExt->get(aapPluginExt, ctx->aap_host, ctx->aap_plugin_id.c_str());
    for (int i = 0; i < aapPluginInfo.get_port_count(&aapPluginInfo); i++) {
        auto portInfo = aapPluginInfo.get_port(&aapPluginInfo, i);
        if (portInfo.content_type(&portInfo) == AAP_CONTENT_TYPE_MIDI2) {
            if (portInfo.direction(&portInfo) == AAP_PORT_DIRECTION_INPUT)
                ctx->mappings.aap_midi_in_port = i;
            else
                ctx->mappings.aap_midi_out_port = i;
        }
    }

    if (!ctx->dummy_raw_buffer)
        ctx->dummy_raw_buffer = calloc(buffer->num_frames(*buffer) * sizeof(float), 1);

    // (1) For ports that has rsz:minimumSize, we also allocate local buffer.
    //     And if it is not an Atom port, it always memcpy.
    // (2) We allocate memory locally for every LV2 Atom port, for both inputs and outputs.
    // (3) For control ports, they point to an element in `control_buffer_pointers`.
    //     They may receive parameter changes via AAP MIDI2 Assignable Controllers.
    //     They also have to be assigned default control values.
    // (4) For other ports, we assign audio pointer from `buffer` as they do not likely move at `process()`,
    //     and IF they indeed moved (we store `cached_buffer`), then we can call `connect_port()` at any time.

    // (3) ^
    if (ctx->control_buffer_pointers)
        free(ctx->control_buffer_pointers);
    ctx->control_buffer_pointers = static_cast<float *>(calloc(numLV2Ports, sizeof(float)));

    int32_t numLV2MidiInPorts = 0;
    int32_t numLV2MidiOutPorts = 0;
    int32_t currentAAPPortIndex = 0;
    for (int i = 0; i < numLV2Ports; i++) {
        ctx->mappings.lv2_to_aap_portmap[i] = -1;
        const LilvPort *lilvPort = lilv_plugin_get_port_by_index(lilvPlugin, i);

        if (IS_CONTROL_IN(ctx, lilvPlugin, lilvPort)) {
            auto defaultNode = lilv_port_get(lilvPlugin, lilvPort, ctx->statics->default_uri_node);
            if (defaultNode)
                ctx->control_buffer_pointers[i] = lilv_node_as_float(defaultNode);
        }

        // Try to get rsz:minimumSize. If it exists, we have to allocate sufficient buffer.
        LilvNode *minimumSizeNode = lilv_port_get(lilvPlugin, lilvPort,
                                                  ctx->statics->resize_port_minimum_size_node);
        auto rszMinimumSize = minimumSizeNode ? (size_t) lilv_node_as_int(minimumSizeNode) : 0;
        if (minimumSizeNode)
            ctx->explicit_port_buffer_sizes[i] = rszMinimumSize;

        if (IS_ATOM_PORT(ctx, lilvPlugin, lilvPort)) {
            auto bufferSize = minimumSizeNode ? ctx->explicit_port_buffer_sizes[i]
                                              : ctx->atom_buffer_size;

            // (2) ^
            if (IS_INPUT_PORT(ctx, lilvPlugin, lilvPort)) {
                if (lilv_port_supports_event(lilvPlugin, lilvPort,
                                             ctx->statics->midi_event_uri_node)) {
                    ctx->mappings.ump_group_to_atom_in_port[numLV2MidiInPorts++] = i;
                    ctx->midi_atom_inputs[i] = static_cast<LV2_Atom_Sequence *>(calloc(
                            bufferSize, 1));
                } else {
                    // it may be unused in AAP, but we have to allocate a buffer for such an Atom port anyways.
                    ctx->explicitly_allocated_port_buffers[i] = calloc(bufferSize, 1);
                    if (lilv_port_supports_event(lilvPlugin, lilvPort,
                                                 ctx->statics->patch_message_uri_node))
                        ctx->mappings.lv2_patch_in_port = i;
                }
            } else {
                if (lilv_port_supports_event(lilvPlugin, lilvPort,
                                             ctx->statics->midi_event_uri_node)) {
                    ctx->mappings.atom_out_port_to_ump_group[i] = numLV2MidiOutPorts++;
                    ctx->midi_atom_outputs[i] = static_cast<LV2_Atom_Sequence *>(calloc(
                            bufferSize, 1));
                } else {
                    // it may be unused in AAP, but we have to allocate a buffer for such an Atom port anyways.
                    ctx->explicitly_allocated_port_buffers[i] = calloc(bufferSize, 1);
                    if (lilv_port_supports_event(lilvPlugin, lilvPort,
                                                 ctx->statics->patch_message_uri_node))
                        ctx->mappings.lv2_patch_out_port = i;
                }
            }
        }
        // (1) ^
        else if (rszMinimumSize > buffer->num_frames(*buffer) * sizeof(float)) {
            if (ctx->explicitly_allocated_port_buffers[i])
                free(ctx->explicitly_allocated_port_buffers[i]);
            ctx->explicitly_allocated_port_buffers[i] = calloc(rszMinimumSize, 1);
        } else if (IS_CONTROL_PORT(ctx, lilvPlugin, lilvPort)) {
            // (3) ^ (we don't allocate for each ControlPort)
            ctx->mappings.lv2_index_to_port[lilv_port_get_index(lilvPlugin, lilvPort)] = i;
        } else {
            // (4) ^
            while (currentAAPPortIndex < buffer->num_ports(*buffer)) {
                auto portInfo = aapPluginInfo.get_port(&aapPluginInfo, currentAAPPortIndex);
                if (portInfo.content_type(&portInfo) == AAP_CONTENT_TYPE_AUDIO)
                    break;
                currentAAPPortIndex++;
            }
            ctx->mappings.aap_to_lv2_portmap[currentAAPPortIndex] = i;
            ctx->mappings.lv2_to_aap_portmap[i] = currentAAPPortIndex;
            currentAAPPortIndex++;
        }
    }
}

void clearBufferForRun(AAPLV2PluginContext* ctx, aap_buffer_t *buffer) {
    auto lilvPlugin = ctx->plugin;
    auto instance = ctx->instance;
    uint32_t numLV2Ports = lilv_plugin_get_num_ports(lilvPlugin);

    auto uridMap = &ctx->features.urid_map_feature_data;
    for (auto p : ctx->midi_atom_inputs)
        lv2_atom_forge_init(&ctx->midi_forges_in[p.first], uridMap);
    for (auto p : ctx->midi_atom_outputs)
        lv2_atom_forge_init(&ctx->midi_forges_out[p.first], uridMap);
    lv2_atom_forge_init(&ctx->patch_forge_in, uridMap);
    lv2_atom_forge_init(&ctx->patch_forge_out, uridMap);

    if (buffer != ctx->cached_buffer) {
        for (int p = 0; p < numLV2Ports; p++) {
            auto epbIter = ctx->explicitly_allocated_port_buffers.find(p);
            if (epbIter != ctx->explicitly_allocated_port_buffers.end()) {
                lilv_instance_connect_port(instance, p, epbIter->second);
                continue;
            }
            auto midiInIter = ctx->midi_atom_inputs.find(p);
            if (midiInIter != ctx->midi_atom_inputs.end()) {
                lilv_instance_connect_port(instance, p, midiInIter->second);
                continue;
            }
            auto midiOutIter = ctx->midi_atom_outputs.find(p);
            if (midiOutIter != ctx->midi_atom_outputs.end()) {
                lilv_instance_connect_port(instance, p, midiOutIter->second);
                continue;
            }

            const LilvPort *lilvPort = lilv_plugin_get_port_by_index(lilvPlugin, p);
            if (IS_CONTROL_PORT(ctx, lilvPlugin, lilvPort)) {
                lilv_instance_connect_port(instance, p, ctx->control_buffer_pointers + p);
                continue;
            }

            // otherwise, it is either an audio port or CV port or whatever.
            int32_t aapPortIndex = ctx->mappings.lv2_to_aap_portmap[p];
            if (aapPortIndex >= 0)
                lilv_instance_connect_port(instance, p, buffer->get_buffer(*buffer, aapPortIndex));
        }
        ctx->cached_buffer = buffer;
    }

    // Clean up Atom output sequences.
    resetMidiAtomBuffers(ctx, buffer, ctx->midi_atom_inputs, true);
    resetMidiAtomBuffers(ctx, buffer, ctx->midi_atom_outputs, false);
    resetPatchAtomBuffer(ctx, buffer, ctx->mappings.lv2_patch_in_port);
    resetPatchAtomBuffer(ctx, buffer, ctx->mappings.lv2_patch_out_port);
}

void aap_lv2_plugin_prepare(AndroidAudioPlugin *plugin, aap_buffer_t *buffer) {
    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_ERROR)
        return;
    if (ctx->instance_state != AAP_LV2_INSTANCE_STATE_INITIAL) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "LV2 plugin %s not at unprepared state.", ctx->aap_plugin_id.c_str());
        ctx->instance_state = AAP_LV2_INSTANCE_STATE_ERROR;
        return;
    }

    allocatePortBuffers(plugin, buffer);
    clearBufferForRun(ctx, buffer);

    ctx->instance_state = AAP_LV2_INSTANCE_STATE_PREPARED;

    aap::a_log_f(AAP_LOG_LEVEL_INFO, AAP_LV2_TAG, "LV2 plugin %s is ready, prepared.", ctx->aap_plugin_id.c_str());
}

void aap_lv2_plugin_activate(AndroidAudioPlugin *plugin) {
    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_ERROR)
        return;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_TERMINATING)
        return;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_PREPARED) {
        lilv_instance_activate(ctx->instance);
        ctx->instance_state = AAP_LV2_INSTANCE_STATE_ACTIVE;
    } else {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "LV2 plugin %s is not at prepared state.", ctx->aap_plugin_id.c_str());
        ctx->instance_state = AAP_LV2_INSTANCE_STATE_ERROR;
    }
}

bool readMidi2Parameter(uint8_t *group, uint8_t* channel, uint8_t* key, uint8_t* extra,
                        uint16_t *index, float *value, cmidi2_ump* ump) {
    auto raw = (uint32_t*) ump;
    return aapReadMidi2ParameterSysex8(group, channel, key, extra, index, value,
                                       *raw, *(raw + 1), *(raw + 2), *(raw + 3));
}

bool
write_midi2_events_as_midi1_to_lv2_forge(AAPLV2PluginContext* ctx, aap_buffer_t *buffer) {

    int32_t aapInPort = ctx->mappings.aap_midi_in_port;
    void *src = buffer->get_buffer(*buffer, aapInPort);

    if (src == nullptr) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "AAP input port %d is not assigned a valid buffer.", aapInPort);
        return false;
    }

    volatile auto aapmb = (AAPMidiBufferHeader*) src;
    uint64_t currentJRTimestamp = 0; // unit of 1/31250 sec. (JR_TIMESTAMP_TICKS_PER_SECOND)

    uint8_t midi1Bytes[16];

    // We deal with both MIDI and Patch (parameter changes) from the unified UMP sequence, while
    // this function has to deal with multiple use-cases, in particular:
    // - The Patch Atom sequence and the MIDI Atom sequence might be the only one, then
    //   they have to be unified, ordered by the event timestamp.
    // - The Patch Atom sequence and the MIDI Atom sequence might be different, then
    //   we have to store them separately.
    // - There may not be a Patch Atom sequence, then it may be ControlPort.
    //   In that case, there may be no Atom output.
    // - There may be no Atom sequence at all, when it is an effect plugin and has only ControlPorts.
    // - There may be more than one MIDI Atom sequences. We differentiate the destination
    //   (input to LV2) by UMP "group".

    int32_t prevGroup{-1};
    int32_t atomMidiIn{-1};
    // I want to initialize it with nullptr, but clang analyzer in Android Studio is somehow stupid
    // and treats as if it were always nullptr (flow analysis wise, it is always initialized to some value).
    LV2_Atom_Forge *midiForge;
    LV2_Atom_Sequence *midiSeq{nullptr};
    auto &portmap = ctx->mappings.ump_group_to_atom_in_port;

    CMIDI2_UMP_SEQUENCE_FOREACH((uint8_t*) src + sizeof(AAPMidiBufferHeader), aapmb->length, iter) {
        auto ump = (cmidi2_ump*) iter;

        auto targetUmpGroup = cmidi2_ump_get_group(ump);
        if (prevGroup != targetUmpGroup) {
            prevGroup = targetUmpGroup;
            atomMidiIn = portmap.find(targetUmpGroup) == portmap.end() ?
                    -1 : ctx->mappings.ump_group_to_atom_in_port[targetUmpGroup];
            midiForge = atomMidiIn < 0 ? nullptr : &ctx->midi_forges_in[atomMidiIn];
            midiSeq = atomMidiIn < 0 ? nullptr : ctx->midi_atom_inputs[atomMidiIn];
        }

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

        uint8_t paramGroup, paramChannel, paramKey{0}, paramExtra{0};
        uint16_t paramId;
        float paramValue;

        if (readMidi2Parameter(&paramGroup, &paramChannel, &paramKey, &paramExtra, &paramId, &paramValue, ump)) {
            // Parameter changes.
            // They are used either for Atom Sequence or ControlPort.
            auto paramValueU32 = *(uint32_t*) &paramValue;
            float paramValueF32 = *(float *) (uint32_t *) &paramValueU32;
            if (ctx->mappings.lv2_patch_in_port >= 0) {
                // write Patch to the Atom port
                auto patchForge = &ctx->patch_forge_in;

                // FIXME: implement patch object output
                //assert(false);

                void* ptr = ctx->explicitly_allocated_port_buffers[ctx->mappings.lv2_patch_in_port];
                ((LV2_Atom_Sequence*) ptr)->atom.size = patchForge->offset - sizeof(LV2_Atom);
            } else {
                // set ControlPort value.
                uint32_t portIndex = ctx->mappings.lv2_index_to_port[paramId];
                ctx->control_buffer_pointers[portIndex] = paramValueF32;
            }

            continue;
        }

        // Otherwise - MIDI message. Downconvert from UMP to bytestream.

        uint32_t midiEventSize = 3;
        uint64_t sysex7;

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
            switch (statusCode) {
                case CMIDI2_STATUS_RPN:
                case CMIDI2_STATUS_NRPN:
                    // FIXME: implement
                    continue;
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
            break;
        }

        if (!midiForge)
            // No MIDI Atom port, nowhere to write MIDI messages.
            return true;

        lv2_atom_forge_frame_time(midiForge, (double) currentJRTimestamp / JR_TIMESTAMP_TICKS_PER_SECOND * ctx->sample_rate);
        lv2_atom_forge_atom(midiForge, midiEventSize, ctx->urids.urid_midi_event_type);
        lv2_atom_forge_write(midiForge, midi1Bytes, midiEventSize);

        midiSeq->atom.size = midiForge->offset - sizeof(LV2_Atom);
    }

    return true;
}

bool
read_forge_events_as_midi2_events(AAPLV2PluginContext* ctx, aap_buffer_t * buffer) {
    int32_t aapOutPort = ctx->mappings.aap_midi_out_port;
    void *dst = buffer->get_buffer(*buffer, aapOutPort);

    // FIXME: implement

    return true;
}

const char *AAP_LV2_TRACE_SECTION_NAME = "aap::lv2::process";
const char *AAP_LV2_TRACE_SECTION_RUN_NAME = "aap::lv2::lilv_run";

void aap_lv2_plugin_process(AndroidAudioPlugin *plugin,
                            aap_buffer_t *buffer,
                            int32_t frameCount,
                            int64_t timeoutInNanoseconds) {
    // FIXME: use timeoutInNanoseconds?

    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_ERROR)
        return;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_TERMINATING)
        return;
    if (ctx->instance_state != AAP_LV2_INSTANCE_STATE_ACTIVE) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "LV2 plugin %s is not at prepared state.", ctx->aap_plugin_id.c_str());
        ctx->instance_state = AAP_LV2_INSTANCE_STATE_ERROR;
        return;
    }

    struct timespec tsBegin{0, 0}, tsEnd{0, 0};
    struct timespec tsRunBegin{0, 0}, tsRunEnd{0, 0};
#if ANDROID
    if (ATrace_isEnabled()) {
        ATrace_beginSection(AAP_LV2_TRACE_SECTION_NAME);
        clock_gettime(CLOCK_REALTIME, &tsBegin);
    }
#endif

    // pre-process

    clearBufferForRun(ctx, buffer);

    /* Process any worker replies. */
    jalv_worker_emit_responses(&ctx->state_worker, ctx->instance);
    jalv_worker_emit_responses(&ctx->worker, ctx->instance);

    /* Notify the plugin the run() cycle is finished */
    if (ctx->worker.iface && ctx->worker.iface->end_run)
        ctx->worker.iface->end_run(ctx->instance->lv2_handle);

    // Convert AAP MIDI/MIDI2 messages into Atom Sequence of MidiEvent.
    // Here, we iterate over UMPs multiple times, which would look inefficient, but in practice
    // there would be only one MIDI port, and there would not be many MIDI in messages.
    if (!write_midi2_events_as_midi1_to_lv2_forge(ctx, buffer))
        return;

    auto numFramesInBuffer = buffer->num_frames(*buffer);
    if (numFramesInBuffer < frameCount) {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "frameCount passed to process() function is larger than num_frames() in aap_buffer_t.");
        frameCount = numFramesInBuffer;
    }

    // process
#if ANDROID
    if (ATrace_isEnabled()) {
        ATrace_beginSection(AAP_LV2_TRACE_SECTION_RUN_NAME);
        clock_gettime(CLOCK_REALTIME, &tsRunBegin);
    }
#endif

    lilv_instance_run(ctx->instance, frameCount);

#if ANDROID
    if (ATrace_isEnabled()) {
        clock_gettime(CLOCK_REALTIME, &tsRunEnd);
        ATrace_setCounter(AAP_LV2_TRACE_SECTION_RUN_NAME,
                          (tsRunEnd.tv_sec - tsRunBegin.tv_sec) * 1000000000 + tsRunEnd.tv_nsec - tsRunBegin.tv_nsec);
        ATrace_endSection();
    }
#endif

    // post-process

    read_forge_events_as_midi2_events(ctx, buffer);

#if ANDROID
    if (ATrace_isEnabled()) {
        clock_gettime(CLOCK_REALTIME, &tsEnd);
        ATrace_setCounter(AAP_LV2_TRACE_SECTION_NAME,
                          (tsEnd.tv_sec - tsBegin.tv_sec) * 1000000000 + tsEnd.tv_nsec - tsBegin.tv_nsec);
        ATrace_endSection();
    }
#endif
}

void aap_lv2_plugin_deactivate(AndroidAudioPlugin *plugin) {
    auto ctx = (AAPLV2PluginContext *) plugin->plugin_specific;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_ERROR)
        return;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_TERMINATING)
        return;
    if (ctx->instance_state == AAP_LV2_INSTANCE_STATE_ACTIVE) {
        lilv_instance_deactivate(ctx->instance);
        ctx->cached_buffer = nullptr;
        ctx->instance_state = AAP_LV2_INSTANCE_STATE_PREPARED;
    } else {
        aap::a_log_f(AAP_LOG_LEVEL_ERROR, AAP_LV2_TAG, "LV2 plugin %s is not at prepared state.",
                     ctx->aap_plugin_id.c_str());
        ctx->instance_state = AAP_LV2_INSTANCE_STATE_ERROR;
    }
}

AndroidAudioPlugin *aap_lv2_plugin_new(
        AndroidAudioPluginFactory *pluginFactory,
        const char *pluginUniqueID,
        int sampleRate,
        AndroidAudioPluginHost *host);

void aap_lv2_plugin_delete(
        AndroidAudioPluginFactory *,
        AndroidAudioPlugin *plugin);

// FIXME: restructure AAPLV2Context and make it ordinal source code structure.
#include "aap-lv2-extensions.cpp"

AndroidAudioPluginFactory aap_lv2_factory{aap_lv2_plugin_new,
                                          aap_lv2_plugin_delete,
                                          nullptr};

} // namespace aaplv2bridge

extern "C" {

AndroidAudioPluginFactory *GetAndroidAudioPluginFactoryLV2Bridge() { return &aaplv2bridge::aap_lv2_factory; }

} // extern "C"
