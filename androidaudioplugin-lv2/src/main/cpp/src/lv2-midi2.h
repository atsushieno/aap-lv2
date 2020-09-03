#ifndef LV2_MIDI2_H
#define LV2_MIDI2_H

#ifdef __cplusplus
extern "C" {
#endif

#define LV2_MIDI2_URI    "http://atsushieno.dev/ns/lv2/midi2"
#define LV2_MIDI2_PREFIX LV2_MIDI2_URI "#"

/* URIs for features and extension data */
#define LV2_MIDI2__ump             LV2_MIDI2_PREFIX "ump"
#define LV2_MIDI2__midi1Protocol   LV2_MIDI2_PREFIX "midi1Protocol"
#define LV2_MIDI2__midi2Protocol   LV2_MIDI2_PREFIX "midi2Protocol"
#define LV2_MIDI2__establishedProtocol   LV2_MIDI2_PREFIX "establishedProtocol"

/* Atom types */
#define LV2_MIDI2__UMP             LV2_MIDI2_PREFIX "UMP"
#define LV2_MIDI2__SystemMessage   LV2_MIDI2_PREFIX "SystemMessage"
#define LV2_MIDI2__Midi1ChannelMessage   LV2_MIDI2_PREFIX "Midi1ChannelMessage"
#define LV2_MIDI2__Sysex7Message   LV2_MIDI2_PREFIX "Sysex7Message"
#define LV2_MIDI2__Midi2ChannelMessage   LV2_MIDI2_PREFIX "Midi2ChanneleMessage"
#define LV2_MIDI2__Sysex8Message   LV2_MIDI2_PREFIX "Sysex8Message"
#define LV2_MIDI2__MixedDataSet    LV2_MIDI2_PREFIX "MixedDataSet"
#define LV2_MIDI2__SystemCommon    LV2_MIDI2_PREFIX "SystemCommon"
#define LV2_MIDI2__SystemRealtime  LV2_MIDI2_PREFIX "SystemRealtime"
#define LV2_MIDI2__TimeCode   LV2_MIDI2_PREFIX "TimeCode"

#define LV2_MIDI2__SongPosition   LV2_MIDI2_PREFIX "SongPosition"
#define LV2_MIDI2__SongSelect   LV2_MIDI2_PREFIX "SongSelect"
#define LV2_MIDI2__TuneRequest   LV2_MIDI2_PREFIX "TuneRequest"
#define LV2_MIDI2__TimingClock   LV2_MIDI2_PREFIX "TimingClock"
#define LV2_MIDI2__TimingStart   LV2_MIDI2_PREFIX "TimingStart"
#define LV2_MIDI2__Start   LV2_MIDI2_PREFIX "Start"
#define LV2_MIDI2__Continue   LV2_MIDI2_PREFIX "Continue"
#define LV2_MIDI2__Stop   LV2_MIDI2_PREFIX "Stop"
#define LV2_MIDI2__ActiveSensing   LV2_MIDI2_PREFIX "ActiveSensing"
#define LV2_MIDI2__Reset   LV2_MIDI2_PREFIX "Reset"
#define LV2_MIDI2__NoteOff1   LV2_MIDI2_PREFIX "NoteOff1"
#define LV2_MIDI2__NoteOn1   LV2_MIDI2_PREFIX "NoteOn1"
#define LV2_MIDI2__PolyphonicPressure1   LV2_MIDI2_PREFIX "PolyphonicPressure1"
#define LV2_MIDI2__ControlChange1   LV2_MIDI2_PREFIX "ControlChange1"
#define LV2_MIDI2__ProgramChange1   LV2_MIDI2_PREFIX "ProgramChange1"
#define LV2_MIDI2__ChannelPressure1   LV2_MIDI2_PREFIX "ChannelPressure1"
#define LV2_MIDI2__PitchBend1   LV2_MIDI2_PREFIX "PitchBend1"
#define LV2_MIDI2__NoteOff2   LV2_MIDI2_PREFIX "NoteOff2"
#define LV2_MIDI2__NoteOn2   LV2_MIDI2_PREFIX "NoteOn2"
#define LV2_MIDI2__ChannelPressure2   LV2_MIDI2_PREFIX "ChannelPressure2"
#define LV2_MIDI2__ControlChange2   LV2_MIDI2_PREFIX "ControlChange2"
#define LV2_MIDI2__RegisteredPerNoteController   LV2_MIDI2_PREFIX "RegisteredPerNoteController"
#define LV2_MIDI2__AssignablePerNoteController   LV2_MIDI2_PREFIX "AssignablePerNoteController"
#define LV2_MIDI2__RegisterdController   LV2_MIDI2_PREFIX "RegisterdController"
#define LV2_MIDI2__AssignableController   LV2_MIDI2_PREFIX "AssignableController"
#define LV2_MIDI2__RelativeRegisterdController   LV2_MIDI2_PREFIX "RelativeRegisterdController"
#define LV2_MIDI2__RelativeAssignableController   LV2_MIDI2_PREFIX "RelativeAssignableController"
#define LV2_MIDI2__ProgramChange2   LV2_MIDI2_PREFIX "ProgramChange2"
#define LV2_MIDI2__ChannelPressure2   LV2_MIDI2_PREFIX "ChannelPressure2"
#define LV2_MIDI2__Pitchbend2   LV2_MIDI2_PREFIX "Pitchbend2"
#define LV2_MIDI2__PerNotePitchbend   LV2_MIDI2_PREFIX "PerNotePitchbend"
#define LV2_MIDI2__PerNoteManagement   LV2_MIDI2_PREFIX "PerNoteManagement"

/* data members */
#define LV2_MIDI2__status   LV2_MIDI2_PREFIX "status"
#define LV2_MIDI2__statusMask   LV2_MIDI2_PREFIX "statusMask"
#define LV2_MIDI2__messageTypeMask   LV2_MIDI2_PREFIX "messageTypeMask"
#define LV2_MIDI2__data8    LV2_MIDI2_PREFIX "data8"
#define LV2_MIDI2__data16   LV2_MIDI2_PREFIX "data16"
#define LV2_MIDI2__data32   LV2_MIDI2_PREFIX "data32"

#ifdef __cplusplus
}
#endif

#endif // LV2_MIDI2_H

