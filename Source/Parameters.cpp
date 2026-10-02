#include "Parameters.h"

namespace rig
{

static const char* const voiceChoices[] { "Green", "Gold", "Plexi" };
static const char* const offOnChoices[] { "Off", "On" };
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
    { "inMute",    "Mute",       0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },

    { "compOn",    "Comp",       0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "compThr",   "Thresh",   -60.0f,   0.0f, -18.0f,     0.0f, 0.0f, " dB",  1, false },
    { "compRatio", "Ratio",      1.0f,  20.0f,   4.0f,     4.0f, 0.0f, ":1",   1, false },
    { "compAtk",   "Attack",     0.1f, 100.0f,   3.0f,     8.0f, 0.0f, " ms",  1, false },
    { "compRel",   "Release",   10.0f,1000.0f, 120.0f,   150.0f, 0.0f, " ms",  0, false },
    { "compMkup",  "Makeup",     0.0f,  24.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "compBlend", "Blend",      0.0f,   1.0f,   1.0f,     0.0f, 0.0f, "",     2, false },
    { "compSc",    "Sidechain", 20.0f, 400.0f,  80.0f,   100.0f, 0.0f, " Hz",  0, false },

    { "eqOn",      "EQ",         0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "eq1F",      "Freq",      20.0f,20000.0f,  80.0f,    80.0f, 0.0f, " Hz",  0, false },
    { "eq1G",      "Gain",     -15.0f,  15.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "eq1Q",      "Q",          0.3f,   8.0f,  0.71f,     1.0f, 0.0f, "",     2, false },
    { "eq2F",      "Freq",      20.0f,20000.0f, 250.0f,   250.0f, 0.0f, " Hz",  0, false },
    { "eq2G",      "Gain",     -15.0f,  15.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "eq2Q",      "Q",          0.3f,   8.0f,   1.0f,     1.0f, 0.0f, "",     2, false },
    { "eq3F",      "Freq",      20.0f,20000.0f, 900.0f,   900.0f, 0.0f, " Hz",  0, false },
    { "eq3G",      "Gain",     -15.0f,  15.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "eq3Q",      "Q",          0.3f,   8.0f,   1.0f,     1.0f, 0.0f, "",     2, false },
    { "eq4F",      "Freq",      20.0f,20000.0f,3000.0f,  3000.0f, 0.0f, " Hz",  0, false },
    { "eq4G",      "Gain",     -15.0f,  15.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "eq4Q",      "Q",          0.3f,   8.0f,   1.0f,     1.0f, 0.0f, "",     2, false },
    { "eq5F",      "Freq",      20.0f,20000.0f,8000.0f,  8000.0f, 0.0f, " Hz",  0, false },
    { "eq5G",      "Gain",     -15.0f,  15.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "eq5Q",      "Q",          0.3f,   8.0f,  0.71f,     1.0f, 0.0f, "",     2, false },

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

    { "frzOn",     "Freeze",     0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "frzHold",   "Hold",       0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, false, offOnChoices },
    { "frzLevel",  "Level",      0.0f,   1.0f,   1.0f,     0.0f, 0.0f, "",     2, false },
    { "frzIn",     "Fade in",    0.0f,1000.0f, 100.0f,   200.0f, 0.0f, " ms",  0, false },
    { "frzOut",    "Fade out",   0.0f,1000.0f, 150.0f,   200.0f, 0.0f, " ms",  0, false },
    { "frzMotion", "Motion",     0.0f,   1.0f,   0.3f,     0.0f, 0.0f, "",     2, false },

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
    { "midiThru",  "Live thru",  0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, false, offOnChoices },

    { "instOn",    "Keys",       0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, false, offOnChoices },
    { "instLevel", "Level",    -60.0f,  12.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },

    { "vocalOn",   "Autotune",   0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "vocalGain", "Mic gain", -24.0f,  24.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "vocalGate", "Mic gate", -80.0f, -20.0f, -50.0f,     0.0f, 0.0f, " dB",  1, false },
    { "vocalLevel","Level",    -60.0f,  12.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },

    { "vocoderOn", "Vocoder",    0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
    { "vocoderLvl","Level",    -60.0f,  12.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },

    { "liveOut",   "Live out", -60.0f,  12.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "liveMute",  "Mute",       0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },

    { "trackGain", "Track",    -60.0f,  12.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "masterGain","Gain",     -12.0f,  12.0f,   0.0f,     0.0f, 0.0f, " dB",  1, false },
    { "masterMute","Mute",       0.0f,   1.0f,   0.0f,     0.0f, 1.0f, "",     0, true  },
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

int channelSlotForKey (const juce::String& channelKey) noexcept
{
    if (channelKey == "bass")   return 0;
    if (channelKey == "mic")    return 1;
    if (channelKey == "keys")   return 2;

    if (channelKey.startsWith ("ch"))
    {
        const int slot = channelKey.substring (2).getIntValue() - 1;

        if (slot >= 3 && slot < ParamStore::maxChannels)
            return slot;
    }

    return -1;
}

ParamStore::ParamStore()
{
    resetToDefaults();
}

void ParamStore::resetToDefaults() noexcept
{
    for (int i = 0; i < NumParams; ++i)
        for (int c = 0; c < maxChannels; ++c)
            set (c, i, specs[(size_t) i].defaultValue);
}

juce::var ParamStore::toVar() const
{
    // The flat top level is channel 0 plus the globals — the shape the
    // settings file has always had, so an older build still reads it.
    auto* obj = new juce::DynamicObject();

    for (int i = 0; i < NumParams; ++i)
        if (! isMomentaryParam (i))
            obj->setProperty (juce::Identifier (specs[(size_t) i].id), get (0, i));

    // Channels beyond the first ride alongside: one dense object per slot
    // that has left its defaults, void for slots still untouched.
    juce::Array<juce::var> extra;

    for (int c = 1; c < maxChannels; ++c)
    {
        bool used = false;

        for (int i = 0; i < NumParams && ! used; ++i)
            used = isChannelParam (i) && ! isMomentaryParam (i)
                     && get (c, i) != specs[(size_t) i].defaultValue;

        if (! used)
        {
            extra.add (juce::var());
            continue;
        }

        auto* co = new juce::DynamicObject();

        for (int i = 0; i < NumParams; ++i)
            if (isChannelParam (i) && ! isMomentaryParam (i))
                co->setProperty (juce::Identifier (specs[(size_t) i].id), get (c, i));

        extra.add (juce::var (co));
    }

    obj->setProperty ("channels", extra);
    return juce::var (obj);
}

void ParamStore::fromVar (const juce::var& v)
{
    auto* obj = v.getDynamicObject();

    if (obj == nullptr)
        return;

    for (int i = 0; i < NumParams; ++i)
    {
        const auto& s = specs[(size_t) i];
        const juce::Identifier key (s.id);

        if (obj->hasProperty (key) && ! isMomentaryParam (i))
            set (0, i, juce::jlimit (s.minValue, s.maxValue, (float) (double) obj->getProperty (key)));
    }

    // A file from before channels simply has no such array, and slots
    // beyond the first stay at their defaults.
    if (auto* extra = obj->getProperty ("channels").getArray())
    {
        for (int c = 1; c < maxChannels; ++c)
        {
            auto* co = c - 1 < extra->size() ? (*extra)[c - 1].getDynamicObject() : nullptr;

            if (co == nullptr)
                continue;

            for (int i = 0; i < NumParams; ++i)
            {
                if (! isChannelParam (i) || isMomentaryParam (i))
                    continue;

                const auto& s = specs[(size_t) i];
                const juce::Identifier key (s.id);

                if (co->hasProperty (key))
                    set (c, i, juce::jlimit (s.minValue, s.maxValue, (float) (double) co->getProperty (key)));
            }
        }
    }
}

} // namespace rig
