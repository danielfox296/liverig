#pragma once

#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "EffectSlot.h"
#include "Parameters.h"
#include "dsp/Biquad.h"
#include "dsp/VoiceTools.h"

namespace rig
{

// The mic strip's voice: the second interface input, into the mix. With
// nothing switched on it is simply the microphone — gain, gate, and out —
// because a channel with no effects should still be heard. Two lanes can
// take that voice over:
//
//   Autotune — the mic through a hosted pitch corrector.
//   Vocoder  — the mic is the *modulator*; the carrier is the bass chain's
//              post-FX tap, octave-up and drive included, because that patch
//              is the carrier conditioner. Words ride the bass line. The raw
//              mic never reaches the mix through this lane.
//
// A lane coming up fades the direct voice down as it rises, so the strip
// speaks with one voice and never doubles itself against a delayed copy of
// the same words. Both lanes down and the microphone stands alone.
//
// The front end is shared — gain, then a gate whose detector listens to the
// raw mic ahead of the fader, so Gain can only make the strip quieter, never
// lock the door. Each lane has its own switch and level; switches are
// crossfades a footswitch cannot click, and a lane fully faded out costs
// nothing. Empty slots are silence, never a crash.
//
// Everything here sums into the live output, so it counts as performance: it
// ducks the backing stems and rides the master limiter like everything the
// player does.
class VocalChannel
{
public:
    VocalChannel (ParamStore& p, EffectSlot& tune, EffectSlot& vocoder)
        : params (p), tuneSlot (tune), vocoderSlot (vocoder) {}

    void prepare (double sampleRate, int maxBlockSize);

    // Adds into left/right. mic is mono; carrier is the bass tap, always
    // live. The strip's own input and live-out gains stand downstream.
    void process (const float* mic, const float* carrier,
                  float* left, float* right, int numSamples) noexcept;

    // What the built-in corrector holds the voice back, when it is the one
    // playing; the looper places a sung layer by it.
    float getBuiltInTuneLatency() const noexcept { return nativeTune.getLatencySamples(); }

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

    // Who plays a lane when no hosted plugin holds its role.
    BuiltInAutotune nativeTune;
    BuiltInVocoder nativeVocoder;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalChannel)
};

} // namespace rig
