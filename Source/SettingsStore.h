#pragma once

#include <juce_core/juce_core.h>

#include "Parameters.h"

namespace rig
{

// One JSON file in the app support dir: every parameter, the last song folder
// and the audio device state.
class SettingsStore
{
public:
    static juce::File getFile();

    void load (ParamStore& params);
    void save (const ParamStore& params) const;

    juce::String songFolder;
    juce::String deviceState;   // AudioDeviceManager state, as XML text
    juce::String expandedPanels;
    juce::String instrument, instrumentState;
    juce::String chainOrder;    // movable stage keys, comma-separated, top to bottom
    juce::String tuner;         // "bass" or "guitar"
    juce::String leftPane;      // "loop" when the looper has the left pane; anything else is the backing track
    juce::String ampModel;      // path of the loaded NAM capture, empty for none

    // Which device input channel plays which role, 1-based; 0 means never
    // assigned (first active channel = bass, second = mic, as it always was).
    int bassInput = 0;
    int micInput = 0;

    // What the player calls each live channel; empty keeps the built-in name.
    juce::String bassName, micName, keysName;

    // The channels on the rail, comma-separated keys in order. A fresh rig
    // starts with just the bass; a settings file from before channels
    // migrates to the strips its values imply.
    juce::String channelList = "bass";

    // What each channel has loaded on its board, comma-separated stage keys
    // whose sequence is the wiring. A fresh board is empty — pedals arrive
    // on purpose. A file from before loadouts migrates with everything it
    // showed still showing.
    juce::String bassLoadout, micLoadout, keysLoadout;

    // The generic channels' names, boards and jacks,
    // "ch4=Name;comp,eq;3|ch5=;;0" — name, loadout, 1-based input channel.
    juce::String extraChannels;

    // Which hardware MIDI device plays the keys, by identifier; empty means
    // every connected device.
    juce::String midiSource;

    // The hosted-plugin roles: plugin name and its state blob, recalled on
    // launch the way the instrument is. The autotune's key and style live in
    // tunePluginState; the vocoder's voicing in vocoderPluginState.
    juce::String tunePlugin, tunePluginState;
    juce::String vocoderPlugin, vocoderPluginState;
};

} // namespace rig
