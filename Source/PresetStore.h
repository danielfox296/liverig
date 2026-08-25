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

    // Per-effect presets. The effect key is a stable short string ("eq",
    // "octave"); the parameter list is what gets captured. Bypass is left out
    // on purpose: recalling a sound should not decide whether it is engaged.
    juce::StringArray getEffectNames (const juce::String& effect) const;
    bool effectContains (const juce::String& effect, const juce::String& name) const;

    bool applyEffect (const juce::String& effect, const juce::String& name,
                      ParamStore& params) const;
    void storeEffect (const juce::String& effect, const juce::String& name,
                      const ParamStore& params, const std::vector<int>& paramIds);
    void removeEffect (const juce::String& effect, const juce::String& name);

private:
    struct EffectPreset
    {
        juce::String effect, name;
        juce::var params;
    };

    int indexOfEffectPreset (const juce::String& effect, const juce::String& name) const;

    void save() const;
    void addFactoryPresets();
    void addFactoryEffectPresets();

    juce::StringArray names;
    juce::Array<juce::var> values;
    juce::Array<EffectPreset> effectPresets;
};

} // namespace rig
