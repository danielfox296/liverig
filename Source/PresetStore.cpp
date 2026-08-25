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
        }
    }

    if (names.isEmpty())
    {
        addFactoryPresets();
        save();
    }
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

    auto* root = new juce::DynamicObject();
    root->setProperty ("presets", array);

    const auto file = getFile();
    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::JSON::toString (juce::var (root), false));
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
