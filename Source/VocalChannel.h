#pragma once

#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "EffectSlot.h"
#include "Parameters.h"
#include "dsp/Biquad.h"

namespace rig
{

// The mic lanes: the second interface input, into the mix, doing one of two
// things — or both, for the brave.
//
//   Autotune — the mic through a hosted pitch corrector.
//   Vocoder  — the mic is the *modulator*; the carrier is the bass chain's
//              post-FX tap, octave-up and drive included, because that patch
//              is the carrier conditioner. Words ride the bass line. The raw
//              mic never reaches the mix through this lane.
//
// The front end is shared — gain, then a gate whose detector listens to the
// raw mic ahead of the fader, so Gain can only make the lanes quieter, never
// lock the door. Each lane has its own switch and level; switches are
// crossfades a footswitch cannot click, and a lane fully faded out costs
// nothing. Empty slots are silence, never a crash.
//
// Both lanes sum into the live output, so they count as performance: they
// duck the backing stems and ride the master limiter like everything the
// player does.
class VocalChannel
{
public:
    VocalChannel (ParamStore& p, EffectSlot& tune, EffectSlot& vocoder)
        : params (p), tuneSlot (tune), vocoderSlot (vocoder) {}

    void prepare (double sampleRate, int maxBlockSize);

    // Adds into left/right. mic is mono; carrier is the bass tap, always live.
    void process (const float* mic, const float* carrier,
                  float* left, float* right, int numSamples) noexcept;

private:
    ParamStore& params;
    EffectSlot& tuneSlot;
    EffectSlot& vocoderSlot;

    double sr = 48000.0;

    juce::SmoothedValue<float> gain, tuneLevel, vocoderLevel, tuneRamp, vocoderRamp;

    // Gate: follower with hysteresis into a smoothed gain, so breath noise
    // between phrases dies without the words losing their first consonant.
    EnvFollower gateEnv;
    float gateState = 0.0f, gateCoef = 0.01f;
    bool gateOpen = false;

    std::vector<float> gated, tuneL, tuneR, vocL, vocR;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalChannel)
};

} // namespace rig
