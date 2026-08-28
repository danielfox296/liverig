#pragma once

#include <atomic>
#include <juce_core/juce_core.h>

namespace rig
{

// Every user-facing value lives in one flat table of atomics. The UI writes,
// the audio thread reads (relaxed) — no locks, no allocation, no messaging.
enum ParamId
{
    pStemGain0, pStemGain1, pStemGain2, pStemGain3,
    pStemMute0, pStemMute1, pStemMute2, pStemMute3,

    pInGain,

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

    pLiveOut,

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

class ParamStore
{
public:
    ParamStore();

    float get (int p) const noexcept        { return values[(size_t) p].load (std::memory_order_relaxed); }
    bool  getBool (int p) const noexcept    { return get (p) > 0.5f; }
    void  set (int p, float v) noexcept     { values[(size_t) p].store (v, std::memory_order_relaxed); }

    void resetToDefaults() noexcept;

    juce::var toVar() const;
    void fromVar (const juce::var& v);

private:
    std::atomic<float> values[(size_t) NumParams];
    static_assert (std::atomic<float>::is_always_lock_free, "need lock-free float atomics");
};

} // namespace rig
