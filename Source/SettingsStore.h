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
    juce::String ampModel;      // path of the loaded NAM capture, empty for none
};

} // namespace rig
