#pragma once

#include <juce_core/juce_core.h>

#include "Parameters.h"

namespace rig
{

// Named snapshots of the live chain — everything from input gain to live out,
// but not the stem faders, which belong to the song rather than the sound.
class PresetStore
{
public:
    static juce::File getFile();

    void load();

    juce::StringArray getNames() const     { return names; }
    bool contains (const juce::String& name) const { return names.contains (name); }

    bool apply (const juce::String& name, ParamStore& params) const;
    void store (const juce::String& name, const ParamStore& params);
    void remove (const juce::String& name);

private:
    void save() const;
    void addFactoryPresets();

    juce::StringArray names;
    juce::Array<juce::var> values;
};

} // namespace rig
