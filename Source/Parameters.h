#pragma once

#include <atomic>
#include <juce_core/juce_core.h>

namespace rig
{

// Every user-facing value lives in one flat table of atomics. The UI writes,
// the audio thread reads (relaxed) — no locks, no allocation, no messaging.
//
// Ids in the run from pInGain to pLiveOut describe one channel's rig and get
// a copy per channel slot; ids outside that run (stems, track, master) are
// the rig's own and exist once. New ids must join the right run.
enum ParamId
{
    pStemGain0, pStemGain1, pStemGain2, pStemGain3,
    pStemMute0, pStemMute1, pStemMute2, pStemMute3,

    pInGain,
    pInMute,

    pCompOn, pCompThresh, pCompRatio, pCompAttack, pCompRelease, pCompMakeup, pCompBlend,
    pCompSidechain,

    // Five-band parametric: band 1 is a low shelf, 2-4 are bells, 5 is a high
    // shelf. Each band is three consecutive ids (freq, gain, Q) — the chain
    // and the EQ view both index bands as pEq1Freq + band * 3.
    pEqOn,
    pEq1Freq, pEq1Gain, pEq1Q,
    pEq2Freq, pEq2Gain, pEq2Q,
    pEq3Freq, pEq3Gain, pEq3Q,
    pEq4Freq, pEq4Gain, pEq4Q,
    pEq5Freq, pEq5Gain, pEq5Q,

    pFltOn, pFltSens, pFltAttack, pFltRelease, pFltFrom, pFltTo, pFltRes, pFltMix,

    pBoostOn, pBoostGain, pBoostSheen, pBoostHair,

    pDriveOn, pDriveVoice, pDriveAmount, pDriveTone, pDriveLevel, pDriveStack, pDriveBite, pDriveXover, pDriveBlend,

    pAmpOn, pAmpIn, pAmpOut,

    // Cabinet: the filter a speaker is, for when no capture is loaded.
    pCabOn, pCabBumpFreq, pCabBumpGain, pCabTop, pCabPresence,

    pOctOn, pOctInterval, pOctMix, pOct2Interval, pOct2Level,

    // Divider sub: a flip-flop, not a transposition.
    pSubOn, pSubLevel, pSubTone, pSubDivide,

    pSynOn, pSynShape, pSynSub, pSynCutoff, pSynEnv, pSynRes, pSynGlide, pSynMix,

    pPhaserOn, pPhaserRate, pPhaserDepth, pPhaserFeedback, pPhaserStages,

    pChoOn, pChoRate, pChoDepth, pChoVoices, pChoTone, pChoLows, pChoMix,

    pDelayOn, pDelayTime, pDelayFeedback, pDelayMix, pDelayHighcut,
    pDelayModRate, pDelayModDepth, pDelaySat, pDelaySmear,

    pTapeOn, pTapeDrive, pTapeSoften, pTapeBump, pTapeMix,

    pRevOn, pRevSize, pRevDamp, pRevPre, pRevMod, pRevMix,

    // MIDI out: the tracker as a note source, aimed at swells and pads.
    // Thru decides whether the live lane still sounds while MIDI is on —
    // off means the rig is a controller and only the pad speaks.
    pMidiOn, pMidiChannel, pMidiTranspose, pMidiGate, pMidiHold, pMidiRelease,
    pMidiBendRange, pMidiExpression, pMidiThru,

    // An AU instrument played by those notes, inside the rig.
    pInstOn, pInstLevel,

    // The mic lanes. Gain and gate are facts about the microphone and are
    // shared; each lane is one function with one switch. Autotune runs the
    // mic through a hosted pitch corrector; Vocoder makes the bass sing —
    // the mic is the modulator, the bass chain's post-FX tap the carrier.
    pVocalOn, pVocalGain, pVocalGate, pVocalLevel,
    pVocoderOn, pVocoderLevel,

    // A hosted plugin as a chain insert.

    pLiveOut,

    pTrackGain,
    pMasterGain,

    NumParams
};

struct ParamSpec
{
    const char* id;
    const char* label;
    float minValue;
    float maxValue;
    float defaultValue;
    float skewCentre;   // value that should sit at the middle of the control; 0 = linear
    float step;         // 0 = continuous
    const char* suffix;
    int decimals;
    bool isToggle;
    const char* const* choices = nullptr;   // or one label per integer step
};

const ParamSpec& spec (int paramId) noexcept;

// -1 if no parameter has that string id.
int paramIndexForId (juce::StringRef id) noexcept;

// A channel-scoped parameter has one value per channel slot; anything else
// (stems, track, master) has a single value shared by the whole rig.
constexpr bool isChannelParam (int paramId) noexcept
{
    return paramId >= pInGain && paramId <= pLiveOut;
}

class ParamStore
{
public:
    // Channel slots are a fixed pool so the audio thread never meets an
    // allocation; slots the user has not claimed yet sit at defaults.
    static constexpr int maxChannels = 8;

    ParamStore();

    float get (int channel, int p) const noexcept   { return values[cell (channel, p)].load (std::memory_order_relaxed); }
    bool  getBool (int channel, int p) const noexcept { return get (channel, p) > 0.5f; }
    void  set (int channel, int p, float v) noexcept  { values[cell (channel, p)].store (v, std::memory_order_relaxed); }

    // The first channel is the rig as it has always been: callers that do
    // not yet speak channels read and write it.
    float get (int p) const noexcept        { return get (0, p); }
    bool  getBool (int p) const noexcept    { return get (0, p) > 0.5f; }
    void  set (int p, float v) noexcept     { set (0, p, v); }

    void resetToDefaults() noexcept;

    juce::var toVar() const;
    void fromVar (const juce::var& v);

private:
    // A global id collapses onto its first cell, whatever channel is asked.
    static size_t cell (int channel, int p) noexcept
    {
        const int c = isChannelParam (p) ? channel : 0;
        jassert (juce::isPositiveAndBelow (c, maxChannels) && juce::isPositiveAndBelow (p, (int) NumParams));
        return (size_t) p * (size_t) maxChannels + (size_t) c;
    }

    std::atomic<float> values[(size_t) NumParams * (size_t) maxChannels];
    static_assert (std::atomic<float>::is_always_lock_free, "need lock-free float atomics");
};

// One channel's window onto the store: the same calls the engine has always
// made, bound to one slot. A processor built around this needs no idea that
// other channels exist.
class ChannelParams
{
public:
    ChannelParams (ParamStore& s, int channelSlot) noexcept : store (s), slot (channelSlot) {}

    float get (int p) const noexcept     { return store.get (slot, p); }
    bool  getBool (int p) const noexcept { return store.getBool (slot, p); }
    void  set (int p, float v) noexcept  { store.set (slot, p, v); }

    int channel() const noexcept { return slot; }

private:
    ParamStore& store;
    int slot;
};

} // namespace rig
