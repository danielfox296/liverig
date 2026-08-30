#pragma once

#include <juce_core/juce_core.h>

#include "Parameters.h"

namespace rig
{

// Named snapshots of the live chain — everything from input gain to live out,
// plus the order the stages are wired in, but not the stem faders, which
// belong to the song rather than the sound.
class PresetStore
{
public:
    static juce::File getFile();

    // Tests point the store at a scratch file; empty means the real one.
    juce::File fileOverride;

    void load();

    juce::StringArray getNames() const     { return names; }
    bool contains (const juce::String& name) const { return names.contains (name); }

    bool apply (const juce::String& name, ParamStore& params) const;

    // The channel-scoped forms: only the given parameter ids are captured or
    // recalled, so a Mic preset can never move a chain knob and vice versa.
    bool apply (const juce::String& name, ParamStore& params,
                const juce::Array<int>& onlyIds, int slot = 0) const;
    void store (const juce::String& name, const ParamStore& params,
                const juce::String& chainOrder, const juce::var& ampCapture,
                const juce::Array<int>& onlyIds, int slot = 0);

    void store (const juce::String& name, const ParamStore& params,
                const juce::String& chainOrder,
                const juce::var& ampCapture = juce::var());
    void remove (const juce::String& name);

    // The stage order the preset was saved with — empty for presets from
    // before order was captured, which leave the wiring alone.
    juce::String getChainOrder (const juce::String& name) const;

    // The NAM capture name the preset was saved with: a string (possibly
    // empty, meaning "saved with no capture"), or void for presets from
    // before captures existed — those leave the amp alone on recall.
    juce::var getAmpCapture (const juce::String& name) const;

    // Per-effect presets. The effect key is a stable short string ("eq",
    // "octave"); the parameter list is what gets captured. Bypass is left out
    // on purpose: recalling a sound should not decide whether it is engaged.
    juce::StringArray getEffectNames (const juce::String& effect) const;
    bool effectContains (const juce::String& effect, const juce::String& name) const;

    // captureOut, when given, receives the preset's stored capture name with
    // the same string-or-void semantics as getAmpCapture.
    bool applyEffect (const juce::String& effect, const juce::String& name,
                      ParamStore& params, juce::var* captureOut = nullptr,
                      int slot = 0) const;
    void storeEffect (const juce::String& effect, const juce::String& name,
                      const ParamStore& params, const std::vector<int>& paramIds,
                      const juce::var& capture = juce::var(), int slot = 0);
    void removeEffect (const juce::String& effect, const juce::String& name);

private:
    struct EffectPreset
    {
        juce::String effect, name;
        juce::var params;
        juce::var capture;
    };

    int indexOfEffectPreset (const juce::String& effect, const juce::String& name) const;

    juce::File storageFile() const { return fileOverride == juce::File() ? getFile() : fileOverride; }

    void save() const;
    void addFactoryPresets();
    void addFactoryEffectPresets();
    void mergeFactoryContent();

    juce::StringArray names;
    juce::Array<juce::var> values;
    juce::StringArray orders;         // index-aligned with names and values
    juce::Array<juce::var> ampCaptures;   // ditto; void where a preset predates captures
    juce::Array<EffectPreset> effectPresets;
};

} // namespace rig
