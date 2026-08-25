#include "Parameters.h"

namespace rig
{

// id, label, min, max, default, skewCentre, step, suffix, decimals, isToggle
static const ParamSpec specs[] =
{
    { "stem0Gain", "Gain",     -60.0f,  12.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "stem1Gain", "Gain",     -60.0f,  12.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "stem2Gain", "Gain",     -60.0f,  12.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "stem3Gain", "Gain",     -60.0f,  12.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },

    { "stem0Mute", "Mute",       0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "stem1Mute", "Mute",       0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "stem2Mute", "Mute",       0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "stem3Mute", "Mute",       0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },

    { "inGain",    "In",       -24.0f,  24.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },

    { "compOn",    "Comp",       0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "compThr",   "Thresh",   -60.0f,   0.0f, -18.0f,     0.0f, 0.0f, " dB",  1, false },
    { "compRatio", "Ratio",      1.0f,  20.0f,   4.0f,     4.0f, 0.0f, ":1",   1, false },
    { "compAtk",   "Attack",     0.1f, 100.0f,   3.0f,     8.0f, 0.0f, " ms",  1, false },
    { "compRel",   "Release",   10.0f,1000.0f, 120.0f,   150.0f, 0.0f, " ms",  0, false },
    { "compMkup",  "Makeup",     0.0f,  24.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },

    { "eqOn",      "EQ",         0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "eqLoF",     "Low F",     40.0f, 400.0f, 100.0f,   120.0f, 0.0f, " Hz",  0, false },
    { "eqLoG",     "Low",      -15.0f,  15.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "eqMidF",    "Mid F",    100.0f,4000.0f, 700.0f,   700.0f, 0.0f, " Hz",  0, false },
    { "eqMidG",    "Mid",      -15.0f,  15.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "eqMidQ",    "Q",          0.2f,   8.0f,   0.9f,     1.2f, 0.0f, "",     2, false },
    { "eqHiF",     "High F",  1000.0f,12000.0f,3000.0f, 3500.0f, 0.0f, " Hz",  0, false },
    { "eqHiG",     "High",     -15.0f,  15.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },

    { "odOn",      "Drive",      0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "odDrive",   "Drive",      1.0f,  50.0f,   6.0f,    10.0f, 0.0f, "",     1, false },
    { "odTone",    "Tone",     300.0f,12000.0f,4000.0f, 2500.0f, 0.0f, " Hz",  0, false },
    { "odLevel",   "Level",    -24.0f,  12.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },

    { "octOn",     "Octave",     0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "octInt",    "Interval", -12.0f,  12.0f,  12.0f,     0.0f, 1.0f, " st",  0, false },
    { "octMix",    "Mix",        0.0f,   1.0f,   0.5f,     0.0f, 0.0f, "",     2, false },

    { "phOn",      "Phaser",     0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "phRate",    "Rate",       0.05f,  8.0f,   0.5f,     1.0f, 0.0f, " Hz",  2, false },
    { "phDepth",   "Depth",      0.0f,   1.0f,   0.6f,     0.0f, 0.0f, "",     2, false },
    { "phFb",      "Feedback",   0.0f,  0.95f,   0.4f,     0.0f, 0.0f, "",     2, false },

    { "dlOn",      "Delay",      0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "dlTime",    "Time",      10.0f,2000.0f, 380.0f,   400.0f, 0.0f, " ms",  0, false },
    { "dlFb",      "Feedback",   0.0f,  0.95f,  0.35f,     0.0f, 0.0f, "",     2, false },
    { "dlMix",     "Mix",        0.0f,   1.0f,   0.3f,     0.0f, 0.0f, "",     2, false },
    { "dlHicut",   "High cut", 500.0f,16000.0f,4000.0f, 3000.0f, 0.0f, " Hz",  0, false },

    { "rvOn",      "Reverb",     0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "rvSize",    "Size",       0.0f,   1.0f,   0.5f,     0.0f, 0.0f, "",     2, false },
    { "rvDamp",    "Damping",    0.0f,   1.0f,   0.5f,     0.0f, 0.0f, "",     2, false },
    { "rvMix",     "Mix",        0.0f,   1.0f,  0.25f,     0.0f, 0.0f, "",     2, false },

    { "liveOut",   "Live out", -60.0f,  12.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
};

static_assert (sizeof (specs) / sizeof (specs[0]) == (size_t) NumParams,
               "parameter spec table is out of sync with ParamId");

const ParamSpec& spec (int paramId) noexcept
{
    jassert (juce::isPositiveAndBelow (paramId, (int) NumParams));
    return specs[(size_t) paramId];
}

ParamStore::ParamStore()
{
    resetToDefaults();
}

void ParamStore::resetToDefaults() noexcept
{
    for (int i = 0; i < NumParams; ++i)
        set (i, specs[(size_t) i].defaultValue);
}

juce::var ParamStore::toVar() const
{
    auto* obj = new juce::DynamicObject();

    for (int i = 0; i < NumParams; ++i)
        obj->setProperty (juce::Identifier (specs[(size_t) i].id), get (i));

    return juce::var (obj);
}

void ParamStore::fromVar (const juce::var& v)
{
    if (auto* obj = v.getDynamicObject())
    {
        for (int i = 0; i < NumParams; ++i)
        {
            const auto& s = specs[(size_t) i];
            const juce::Identifier key (s.id);

            if (obj->hasProperty (key))
                set (i, juce::jlimit (s.minValue, s.maxValue, (float) (double) obj->getProperty (key)));
        }
    }
}

} // namespace rig
