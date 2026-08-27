#include "Parameters.h"

namespace rig
{

static const char* const voiceChoices[] { "Green", "Gold", "Plexi" };
static const char* const expressionChoices[] { "Off", "CC1 mod", "CC11 expr" };
static const char* const divideChoices[] { "-1 oct", "-2 oct" };

// id, label, min, max, default, skewCentre, step, suffix, decimals, isToggle, choices
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
    { "compBlend", "Blend",      0.0f,   1.0f,   1.0f,     0.0f, 0.0f, "",     2, false },
    { "compSc",    "Sidechain", 20.0f, 400.0f,  80.0f,   100.0f, 0.0f, " Hz",  0, false },

    { "eqOn",      "EQ",         0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "eqLoF",     "Low F",     40.0f, 400.0f, 100.0f,   120.0f, 0.0f, " Hz",  0, false },
    { "eqLoG",     "Low",      -15.0f,  15.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "eqMidF",    "Mid F",    100.0f,4000.0f, 700.0f,   700.0f, 0.0f, " Hz",  0, false },
    { "eqMidG",    "Mid",      -15.0f,  15.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "eqMidQ",    "Q",          0.2f,   8.0f,   0.9f,     1.2f, 0.0f, "",     2, false },
    { "eqHiF",     "High F",  1000.0f,12000.0f,3000.0f, 3500.0f, 0.0f, " Hz",  0, false },
    { "eqHiG",     "High",     -15.0f,  15.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },

    { "fltOn",     "Filter",     0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "fltSens",   "Sens",       0.0f,   1.0f,   0.5f,     0.0f, 0.0f, "",     2, false },
    { "fltAtk",    "Attack",     1.0f,  80.0f,   4.0f,    10.0f, 0.0f, " ms",  1, false },
    { "fltRel",    "Release",   30.0f, 800.0f, 180.0f,   200.0f, 0.0f, " ms",  0, false },
    { "fltFrom",   "From",      60.0f,1000.0f, 120.0f,   250.0f, 0.0f, " Hz",  0, false },
    { "fltTo",     "To",       100.0f,4000.0f,1600.0f,   900.0f, 0.0f, " Hz",  0, false },
    { "fltRes",    "Res",        0.5f,  10.0f,   3.5f,     3.0f, 0.0f, "",     1, false },
    { "fltMix",    "Mix",        0.0f,   1.0f,   1.0f,     0.0f, 0.0f, "",     2, false },

    { "bstOn",     "Boost",      0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "bstGain",   "Boost",      0.0f,  30.0f,   6.0f,    10.0f, 0.0f, " dB",  1, false },
    { "bstSheen",  "Sheen",     -6.0f,   6.0f,   1.5f,     0.0f, 0.0f, " dB",  1, false },
    { "bstHair",   "Hair",       0.0f,   1.0f,  0.15f,     0.0f, 0.0f, "",     2, false },

    { "odOn",      "Drive",      0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "odVoice",   "Voice",      1.0f,   3.0f,   1.0f,     0.0f, 1.0f, "",     0, false, voiceChoices },
    { "odDrive",   "Drive",      1.0f,  50.0f,   6.0f,    10.0f, 0.0f, "",     1, false },
    { "odTone",    "Tone",     300.0f,12000.0f,4000.0f, 2500.0f, 0.0f, " Hz",  0, false },
    { "odLevel",   "Level",    -24.0f,  12.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "odStack",   "Stack",      0.0f,   1.0f,   0.0f,     0.0f, 0.0f, "",     2, false },
    { "odBite",    "Bite",       0.0f,   1.0f,   0.4f,     0.0f, 0.0f, "",     2, false },
    { "odXover",   "Crossover", 40.0f, 900.0f,  40.0f,   200.0f, 0.0f, " Hz",  0, false },
    { "odBlend",   "Blend",      0.0f,   1.0f,   1.0f,     0.0f, 0.0f, "",     2, false },

    { "ampOn",     "Amp",        0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "ampIn",     "Gain in",  -24.0f,  24.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "ampOut",    "Gain out", -24.0f,  24.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },

    { "cabOn",     "Cab",        0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "cabBumpF",  "Bump F",    50.0f, 250.0f,  90.0f,   100.0f, 0.0f, " Hz",  0, false },
    { "cabBumpG",  "Bump",       0.0f,  12.0f,   5.0f,     0.0f, 0.0f, " dB",  1, false },
    { "cabTop",    "Top",     1500.0f,8000.0f,4500.0f,  3500.0f, 0.0f, " Hz",  0, false },
    { "cabPres",   "Presence",  -8.0f,   8.0f,  -2.0f,     0.0f, 0.0f, " dB",  1, false },

    { "octOn",     "Octave",     0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "octInt",    "Interval", -12.0f,  12.0f,  12.0f,     0.0f, 1.0f, " st",  0, false },
    { "octMix",    "Mix",        0.0f,   1.0f,   0.5f,     0.0f, 0.0f, "",     2, false },
    { "oct2Int",   "Voice 2",  -12.0f,  12.0f,   7.0f,     0.0f, 1.0f, " st",  0, false },
    { "oct2Lvl",   "V2 level",   0.0f,   1.0f,   0.0f,     0.0f, 0.0f, "",     2, false },

    { "subOn",     "Sub",        0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "subLevel",  "Level",      0.0f,   1.0f,   0.6f,     0.0f, 0.0f, "",     2, false },
    { "subTone",   "Tone",     100.0f,2000.0f, 500.0f,   450.0f, 0.0f, " Hz",  0, false },
    { "subDiv",    "Divide",     0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, false, divideChoices },

    { "syOn",      "Synth",      0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "syShape",   "Shape",      0.0f,   1.0f,  0.35f,     0.0f, 0.0f, "",     2, false },
    { "sySub",     "Sub",        0.0f,   1.0f,   0.5f,     0.0f, 0.0f, "",     2, false },
    { "syCut",     "Cutoff",    80.0f,3000.0f, 350.0f,   500.0f, 0.0f, " Hz",  0, false },
    { "syEnv",     "Env",        0.0f,   1.0f,   0.6f,     0.0f, 0.0f, "",     2, false },
    { "syRes",     "Res",        0.5f,  10.0f,   2.5f,     3.0f, 0.0f, "",     1, false },
    { "syGlide",   "Glide",      0.0f, 200.0f,  15.0f,    40.0f, 0.0f, " ms",  0, false },
    { "syMix",     "Mix",        0.0f,   1.0f,   0.6f,     0.0f, 0.0f, "",     2, false },

    { "phOn",      "Phaser",     0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "phRate",    "Rate",       0.05f,  8.0f,   0.5f,     1.0f, 0.0f, " Hz",  2, false },
    { "phDepth",   "Depth",      0.0f,   1.0f,   0.6f,     0.0f, 0.0f, "",     2, false },
    { "phFb",      "Feedback",   0.0f,  0.95f,   0.4f,     0.0f, 0.0f, "",     2, false },
    { "phStages",  "Stages",     4.0f,   8.0f,   4.0f,     0.0f, 2.0f, "",     0, false },

    { "chOn",      "Chorus",     0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "chRate",    "Rate",       0.05f,  5.0f,   0.7f,     1.0f, 0.0f, " Hz",  2, false },
    { "chDepth",   "Depth",      0.0f,   1.0f,  0.45f,     0.0f, 0.0f, "",     2, false },
    { "chVoices",  "Voices",     1.0f,   3.0f,   2.0f,     0.0f, 1.0f, "",     0, false },
    { "chTone",    "Tone",    1000.0f,12000.0f,6000.0f, 4000.0f, 0.0f, " Hz",  0, false },
    { "chLows",    "Keep lows", 20.0f, 300.0f, 120.0f,   100.0f, 0.0f, " Hz",  0, false },
    { "chMix",     "Mix",        0.0f,   1.0f,   0.5f,     0.0f, 0.0f, "",     2, false },

    { "dlOn",      "Delay",      0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "dlTime",    "Time",      10.0f,2000.0f, 380.0f,   400.0f, 0.0f, " ms",  0, false },
    { "dlFb",      "Feedback",   0.0f,  0.95f,  0.35f,     0.0f, 0.0f, "",     2, false },
    { "dlMix",     "Mix",        0.0f,   1.0f,   0.3f,     0.0f, 0.0f, "",     2, false },
    { "dlHicut",   "High cut", 500.0f,16000.0f,4000.0f, 3000.0f, 0.0f, " Hz",  0, false },
    { "dlModRate", "Mod rate",   0.1f,   4.0f,   0.9f,     1.0f, 0.0f, " Hz",  2, false },
    { "dlModDep",  "Mod depth",  0.0f,   1.0f,   0.0f,     0.0f, 0.0f, "",     2, false },
    { "dlSat",     "Sat",        0.0f,   1.0f,   0.0f,     0.0f, 0.0f, "",     2, false },
    { "dlSmear",   "Smear",      0.0f,   1.0f,   0.0f,     0.0f, 0.0f, "",     2, false },

    { "tpOn",      "Tape",       0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "tpDrive",   "Drive",      0.0f,  18.0f,   6.0f,     0.0f, 0.0f, " dB",  1, false },
    { "tpSoften",  "Soften",     0.0f,   1.0f,   0.4f,     0.0f, 0.0f, "",     2, false },
    { "tpBump",    "Bump",       0.0f,   1.0f,  0.35f,     0.0f, 0.0f, "",     2, false },
    { "tpMix",     "Mix",        0.0f,   1.0f,   1.0f,     0.0f, 0.0f, "",     2, false },

    { "rvOn",      "Reverb",     0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "rvSize",    "Size",       0.0f,   1.0f,   0.5f,     0.0f, 0.0f, "",     2, false },
    { "rvDamp",    "Damping",    0.0f,   1.0f,   0.5f,     0.0f, 0.0f, "",     2, false },
    { "rvPre",     "Predelay",   0.0f, 120.0f,  12.0f,    30.0f, 0.0f, " ms",  0, false },
    { "rvMod",     "Mod",        0.0f,   1.0f,   0.5f,     0.0f, 0.0f, "",     2, false },
    { "rvMix",     "Mix",        0.0f,   1.0f,  0.25f,     0.0f, 0.0f, "",     2, false },

    { "midiOn",    "MIDI",       0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "midiCh",    "Channel",    1.0f,  16.0f,   1.0f,     0.0f, 1.0f, "",     0, false },
    { "midiTrans", "Transpose",-24.0f,  24.0f,  12.0f,     0.0f, 1.0f, " st",  0, false },
    { "midiGate",  "Gate",     -60.0f, -20.0f, -42.0f,     0.0f, 0.0f, " dB",  1, false },
    { "midiHold",  "Hold",      10.0f, 200.0f,  45.0f,    60.0f, 0.0f, " ms",  0, false },
    { "midiRel",   "Release",   20.0f,1000.0f, 180.0f,   200.0f, 0.0f, " ms",  0, false },
    { "midiBend",  "Bend range", 0.0f,  12.0f,   2.0f,     0.0f, 1.0f, " st",  0, false },
    { "midiExpr",  "Expression", 0.0f,   2.0f,   2.0f,     0.0f, 1.0f, "",     0, false, expressionChoices },

    { "liveOut",   "Live out", -60.0f,  12.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },

    { "masterGain","Gain",     -12.0f,  12.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
};

static_assert (sizeof (specs) / sizeof (specs[0]) == (size_t) NumParams,
               "parameter spec table is out of sync with ParamId");

const ParamSpec& spec (int paramId) noexcept
{
    jassert (juce::isPositiveAndBelow (paramId, (int) NumParams));
    return specs[(size_t) paramId];
}

int paramIndexForId (juce::StringRef id) noexcept
{
    for (int i = 0; i < NumParams; ++i)
        if (id == juce::StringRef (specs[(size_t) i].id))
            return i;

    return -1;
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
