#pragma once

#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "EffectSlot.h"
#include "Parameters.h"
#include "dsp/Biquad.h"

namespace rig
{

// The autotune lane: the second interface input, through a hosted pitch
// corrector, into the mix. One function, one switch — the row is called
// Autotune because that is the whole of what it does.
//
// gain → gate → the pitch plugin → level, with the on/off a crossfade so a
// footswitch press never clicks. Off means nothing reaches the mix; on with
// no plugin bound is silence too, and the row's status line says why.
//
// The lane sums into the live output, so it counts as performance: it ducks
// the backing stems and rides the master limiter like everything the player
// does. Key, scale and how hard the correction grabs live in the plugin's
// own window, saved with the rig's settings.
class VocalChannel
{
public:
    VocalChannel (ParamStore& p, EffectSlot& tune)
        : params (p), tuneSlot (tune) {}

    void prepare (double sampleRate, int maxBlockSize);

    // Adds into left/right. mic is mono.
    void process (const float* mic, float* left, float* right, int numSamples) noexcept;

private:
    ParamStore& params;
    EffectSlot& tuneSlot;

    double sr = 48000.0;

    juce::SmoothedValue<float> gain, level, onRamp;

    // Gate: follower with hysteresis into a smoothed gain, so breath noise
    // between phrases dies without the words losing their first consonant.
    EnvFollower gateEnv;
    float gateState = 0.0f, gateCoef = 0.01f;
    bool gateOpen = false;

    std::vector<float> gated, laneL, laneR;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalChannel)
};

} // namespace rig
