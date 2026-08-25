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

    pCompOn, pCompThresh, pCompRatio, pCompAttack, pCompRelease, pCompMakeup,

    pEqOn, pEqLoFreq, pEqLoGain, pEqMidFreq, pEqMidGain, pEqMidQ, pEqHiFreq, pEqHiGain,

    pDriveOn, pDriveAmount, pDriveTone, pDriveLevel,

    pOctOn, pOctInterval, pOctMix,

    pPhaserOn, pPhaserRate, pPhaserDepth, pPhaserFeedback,

    pDelayOn, pDelayTime, pDelayFeedback, pDelayMix, pDelayHighcut,

    pRevOn, pRevSize, pRevDamp, pRevMix,

    pLiveOut,

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
};

const ParamSpec& spec (int paramId) noexcept;

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
