#include "PresetStore.h"

namespace rig
{

juce::File PresetStore::getFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
              .getChildFile ("Application Support")
              .getChildFile ("LiveRig")
              .getChildFile ("presets.json");
}

static juce::var capture (const ParamStore& params)
{
    auto* obj = new juce::DynamicObject();

    for (int i = pInGain; i < NumParams; ++i)
        obj->setProperty (juce::Identifier (spec (i).id), params.get (i));

    return juce::var (obj);
}

void PresetStore::load()
{
    names.clear();
    values.clear();
    effectPresets.clear();

    const auto file = getFile();

    if (file.existsAsFile())
    {
        const auto parsed = juce::JSON::parse (file.loadFileAsString());

        if (auto* root = parsed.getDynamicObject())
        {
            const auto list = root->getProperty ("presets");

            if (auto* array = list.getArray())
            {
                for (const auto& entry : *array)
                {
                    if (auto* obj = entry.getDynamicObject())
                    {
                        const auto name = obj->getProperty ("name").toString();

                        if (name.isNotEmpty() && ! names.contains (name))
                        {
                            names.add (name);
                            values.add (obj->getProperty ("params"));
                        }
                    }
                }
            }

            const auto effects = root->getProperty ("effects");

            if (auto* array = effects.getArray())
            {
                for (const auto& entry : *array)
                {
                    if (auto* obj = entry.getDynamicObject())
                    {
                        EffectPreset preset;
                        preset.effect = obj->getProperty ("effect").toString();
                        preset.name = obj->getProperty ("name").toString();
                        preset.params = obj->getProperty ("params");

                        if (preset.effect.isNotEmpty() && preset.name.isNotEmpty()
                            && indexOfEffectPreset (preset.effect, preset.name) < 0)
                            effectPresets.add (preset);
                    }
                }
            }
        }
    }

    bool changed = false;

    if (names.isEmpty())
    {
        addFactoryPresets();
        changed = true;
    }

    if (effectPresets.isEmpty())
    {
        addFactoryEffectPresets();
        changed = true;
    }

    if (changed)
        save();
}

bool PresetStore::apply (const juce::String& name, ParamStore& params) const
{
    const int index = names.indexOf (name);

    if (index < 0)
        return false;

    auto* obj = values[index].getDynamicObject();

    if (obj == nullptr)
        return false;

    for (int i = pInGain; i < NumParams; ++i)
    {
        const auto& s = spec (i);
        const juce::Identifier key (s.id);

        if (obj->hasProperty (key))
            params.set (i, juce::jlimit (s.minValue, s.maxValue,
                                         (float) (double) obj->getProperty (key)));
    }

    return true;
}

void PresetStore::store (const juce::String& name, const ParamStore& params)
{
    if (name.isEmpty())
        return;

    const int index = names.indexOf (name);

    if (index >= 0)
    {
        values.set (index, capture (params));
    }
    else
    {
        names.add (name);
        values.add (capture (params));
    }

    save();
}

void PresetStore::remove (const juce::String& name)
{
    const int index = names.indexOf (name);

    if (index < 0)
        return;

    names.remove (index);
    values.remove (index);
    save();
}

void PresetStore::save() const
{
    juce::Array<juce::var> array;

    for (int i = 0; i < names.size(); ++i)
    {
        auto* entry = new juce::DynamicObject();
        entry->setProperty ("name", names[i]);
        entry->setProperty ("params", values[i]);
        array.add (juce::var (entry));
    }

    juce::Array<juce::var> effectArray;

    for (const auto& preset : effectPresets)
    {
        auto* entry = new juce::DynamicObject();
        entry->setProperty ("effect", preset.effect);
        entry->setProperty ("name", preset.name);
        entry->setProperty ("params", preset.params);
        effectArray.add (juce::var (entry));
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("presets", array);
    root->setProperty ("effects", effectArray);

    const auto file = getFile();
    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::JSON::toString (juce::var (root), false));
}

int PresetStore::indexOfEffectPreset (const juce::String& effect, const juce::String& name) const
{
    for (int i = 0; i < effectPresets.size(); ++i)
        if (effectPresets[i].effect == effect && effectPresets[i].name == name)
            return i;

    return -1;
}

juce::StringArray PresetStore::getEffectNames (const juce::String& effect) const
{
    juce::StringArray result;

    for (const auto& preset : effectPresets)
        if (preset.effect == effect)
            result.add (preset.name);

    return result;
}

bool PresetStore::effectContains (const juce::String& effect, const juce::String& name) const
{
    return indexOfEffectPreset (effect, name) >= 0;
}

bool PresetStore::applyEffect (const juce::String& effect, const juce::String& name,
                               ParamStore& params) const
{
    const int index = indexOfEffectPreset (effect, name);

    if (index < 0)
        return false;

    auto* obj = effectPresets[index].params.getDynamicObject();

    if (obj == nullptr)
        return false;

    for (const auto& property : obj->getProperties())
    {
        const int id = paramIndexForId (property.name.toString());

        if (id < 0)
            continue;

        const auto& s = spec (id);
        params.set (id, juce::jlimit (s.minValue, s.maxValue, (float) (double) property.value));
    }

    return true;
}

void PresetStore::storeEffect (const juce::String& effect, const juce::String& name,
                               const ParamStore& params, const std::vector<int>& paramIds)
{
    if (effect.isEmpty() || name.isEmpty())
        return;

    auto* obj = new juce::DynamicObject();

    for (int id : paramIds)
        obj->setProperty (juce::Identifier (spec (id).id), params.get (id));

    const int index = indexOfEffectPreset (effect, name);

    if (index >= 0)
    {
        effectPresets.getReference (index).params = juce::var (obj);
    }
    else
    {
        effectPresets.add ({ effect, name, juce::var (obj) });
    }

    save();
}

void PresetStore::removeEffect (const juce::String& effect, const juce::String& name)
{
    const int index = indexOfEffectPreset (effect, name);

    if (index < 0)
        return;

    effectPresets.remove (index);
    save();
}

void PresetStore::addFactoryEffectPresets()
{
    struct Setting { int id; float value; };

    struct Factory
    {
        const char* effect;
        const char* name;
        std::vector<Setting> settings;
    };

    const Factory factories[]
    {
        { "comp", "Gentle glue",  { { pCompThresh, -16.0f }, { pCompRatio, 2.0f }, { pCompAttack, 20.0f },
                                    { pCompRelease, 220.0f }, { pCompMakeup, 2.0f } } },
        { "comp", "Fast FET",     { { pCompThresh, -22.0f }, { pCompRatio, 4.0f }, { pCompAttack, 0.8f },
                                    { pCompRelease, 120.0f }, { pCompMakeup, 5.0f } } },
        { "comp", "Squash",       { { pCompThresh, -30.0f }, { pCompRatio, 12.0f }, { pCompAttack, 1.0f },
                                    { pCompRelease, 90.0f }, { pCompMakeup, 9.0f } } },

        { "eq", "Flat",           { { pEqLoFreq, 100.0f }, { pEqLoGain, 0.0f }, { pEqMidFreq, 700.0f },
                                    { pEqMidGain, 0.0f }, { pEqMidQ, 0.9f }, { pEqHiFreq, 3000.0f },
                                    { pEqHiGain, 0.0f } } },
        { "eq", "Fat lows",       { { pEqLoFreq, 80.0f }, { pEqLoGain, 4.5f }, { pEqMidFreq, 600.0f },
                                    { pEqMidGain, -2.0f }, { pEqMidQ, 0.8f }, { pEqHiFreq, 3000.0f },
                                    { pEqHiGain, 1.0f } } },
        { "eq", "Cut mud",        { { pEqLoFreq, 90.0f }, { pEqLoGain, 1.0f }, { pEqMidFreq, 320.0f },
                                    { pEqMidGain, -5.0f }, { pEqMidQ, 1.4f }, { pEqHiFreq, 2500.0f },
                                    { pEqHiGain, 2.0f } } },
        { "eq", "Scooped",        { { pEqLoFreq, 90.0f }, { pEqLoGain, 5.0f }, { pEqMidFreq, 800.0f },
                                    { pEqMidGain, -7.0f }, { pEqMidQ, 0.7f }, { pEqHiFreq, 3500.0f },
                                    { pEqHiGain, 4.0f } } },

        { "drive", "Edge",        { { pDriveAmount, 3.0f }, { pDriveTone, 5000.0f }, { pDriveLevel, -1.0f } } },
        { "drive", "Grit",        { { pDriveAmount, 12.0f }, { pDriveTone, 3200.0f }, { pDriveLevel, -4.0f } } },
        { "drive", "Wall",        { { pDriveAmount, 34.0f }, { pDriveTone, 2200.0f }, { pDriveLevel, -8.0f } } },

        { "octave", "Classic +12",{ { pOctInterval, 12.0f }, { pOctMix, 0.5f } } },
        { "octave", "Just a hint",{ { pOctInterval, 12.0f }, { pOctMix, 0.25f } } },
        { "octave", "Full up",    { { pOctInterval, 12.0f }, { pOctMix, 1.0f } } },
        { "octave", "Sub -12",    { { pOctInterval, -12.0f }, { pOctMix, 0.4f } } },
        { "octave", "Fifth up",   { { pOctInterval, 7.0f }, { pOctMix, 0.4f } } },

        { "phaser", "Slow sweep", { { pPhaserRate, 0.25f }, { pPhaserDepth, 0.7f }, { pPhaserFeedback, 0.35f } } },
        { "phaser", "Funk",       { { pPhaserRate, 1.6f }, { pPhaserDepth, 0.85f }, { pPhaserFeedback, 0.55f } } },
        { "phaser", "Deep",       { { pPhaserRate, 0.6f }, { pPhaserDepth, 1.0f }, { pPhaserFeedback, 0.8f } } },

        { "delay", "Slapback",    { { pDelayTime, 110.0f }, { pDelayFeedback, 0.15f }, { pDelayMix, 0.25f },
                                    { pDelayHighcut, 4500.0f } } },
        { "delay", "Quarter-ish", { { pDelayTime, 420.0f }, { pDelayFeedback, 0.35f }, { pDelayMix, 0.3f },
                                    { pDelayHighcut, 3500.0f } } },
        { "delay", "Long wash",   { { pDelayTime, 780.0f }, { pDelayFeedback, 0.6f }, { pDelayMix, 0.4f },
                                    { pDelayHighcut, 2200.0f } } },

        { "reverb", "Room",       { { pRevSize, 0.32f }, { pRevDamp, 0.55f }, { pRevMix, 0.18f } } },
        { "reverb", "Hall",       { { pRevSize, 0.68f }, { pRevDamp, 0.4f }, { pRevMix, 0.3f } } },
        { "reverb", "Wash",       { { pRevSize, 0.92f }, { pRevDamp, 0.25f }, { pRevMix, 0.45f } } },
    };

    for (const auto& factory : factories)
    {
        auto* obj = new juce::DynamicObject();

        for (const auto& setting : factory.settings)
            obj->setProperty (juce::Identifier (spec (setting.id).id), setting.value);

        effectPresets.add ({ factory.effect, factory.name, juce::var (obj) });
    }

}

void PresetStore::addFactoryPresets()
{
    struct Setting { int id; float value; };

    struct Factory
    {
        const char* name;
        std::vector<Setting> settings;
    };

    const Factory factories[]
    {
        { "Clean DI", {} },

        { "Fingerstyle comp", { { pCompOn, 1.0f }, { pCompThresh, -20.0f }, { pCompRatio, 3.0f },
                                { pCompAttack, 8.0f }, { pCompRelease, 140.0f }, { pCompMakeup, 4.0f },
                                { pEqOn, 1.0f }, { pEqLoGain, 2.0f }, { pEqMidFreq, 500.0f },
                                { pEqMidGain, -2.0f }, { pEqHiGain, 1.5f } } },

        { "Octave lead", { { pCompOn, 1.0f }, { pCompThresh, -22.0f }, { pCompRatio, 4.0f },
                           { pCompAttack, 1.0f }, { pCompMakeup, 5.0f },
                           { pOctOn, 1.0f }, { pOctInterval, 12.0f }, { pOctMix, 0.55f },
                           { pEqOn, 1.0f }, { pEqHiGain, 2.5f } } },

        { "Dirty octave", { { pCompOn, 1.0f }, { pCompThresh, -20.0f }, { pCompRatio, 4.0f },
                            { pCompAttack, 2.0f }, { pCompMakeup, 3.0f },
                            { pDriveOn, 1.0f }, { pDriveAmount, 12.0f }, { pDriveTone, 3000.0f },
                            { pDriveLevel, -3.0f },
                            { pOctOn, 1.0f }, { pOctInterval, 12.0f }, { pOctMix, 0.5f } } },

        { "Phaser funk", { { pCompOn, 1.0f }, { pCompThresh, -18.0f }, { pCompRatio, 6.0f },
                           { pCompAttack, 1.0f }, { pCompMakeup, 4.0f },
                           { pPhaserOn, 1.0f }, { pPhaserRate, 1.2f }, { pPhaserDepth, 0.8f },
                           { pPhaserFeedback, 0.5f } } },

        { "Ambient swell", { { pOctOn, 1.0f }, { pOctInterval, 12.0f }, { pOctMix, 0.35f },
                             { pDelayOn, 1.0f }, { pDelayTime, 420.0f }, { pDelayFeedback, 0.45f },
                             { pDelayMix, 0.35f }, { pDelayHighcut, 3000.0f },
                             { pRevOn, 1.0f }, { pRevSize, 0.7f }, { pRevDamp, 0.4f },
                             { pRevMix, 0.35f } } },
    };

    for (const auto& factory : factories)
    {
        ParamStore params;

        for (const auto& setting : factory.settings)
            params.set (setting.id, setting.value);

        names.add (factory.name);
        values.add (capture (params));
    }
}

} // namespace rig
