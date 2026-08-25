#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <juce_dsp/juce_dsp.h>

#include "Parameters.h"
#include "dsp/Biquad.h"
#include "dsp/OctaveShifter.h"
#include "dsp/PitchTracker.h"

namespace rig
{

// The live lane: mono in, stereo out, fixed effect order.
//
// Every effect runs on every block whether or not it is bypassed, and bypass
// is a 15 ms crossfade between the effect's dry input and its output. That
// costs a little idle CPU and buys two things worth more on stage: click-free
// toggling with live tails, and a constant per-block cost — no load spikes
// when a footswitch-sized decision lands at 64 samples.
class FxChain
{
public:
    explicit FxChain (ParamStore& p) : params (p) {}

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    void process (float* mono, float* left, float* right, int numSamples) noexcept;

    // Total added latency of the chain in samples (oversampler + shifter).
    float getLatencySamples() const noexcept { return reportedLatency.load (std::memory_order_relaxed); }

private:
    void updateFromParams() noexcept;
    bool changed (int paramId) noexcept;

    template <typename Fn>
    void runStage (float* data, int numSamples,
                   juce::SmoothedValue<float>& bypassRamp,
                   juce::SmoothedValue<float>* mixRamp,
                   Fn&& fn) noexcept;

    void processDrive (float* data, int numSamples) noexcept;
    void processPhaser (float* data, int numSamples) noexcept;
    void processDelay (float* data, int numSamples) noexcept;

    ParamStore& params;

    double sr = 48000.0;
    int maxBlock = 512;

    float lastSeen[(size_t) NumParams] {};
    bool primed = false;

    DCBlocker dcBlock;
    juce::SmoothedValue<float> inGain, makeup, driveAmt, driveLevel, liveOut;

    juce::dsp::Compressor<float> comp;

    Biquad eqLo, eqMid, eqHi;

    std::unique_ptr<juce::dsp::Oversampling<float>> os;
    Biquad tone;

    PitchTracker tracker;
    OctaveShifter shifter;
    juce::SmoothedValue<float> octMix;

    AllpassOne ap[4];
    double lfoPhase = 0.0;
    float phaserFb = 0.0f, phaserRate = 0.5f, phaserDepth = 0.6f, phaserFbState = 0.0f;

    std::vector<float> delayBuf;
    int delayMask = 0, delayWrite = 0;
    juce::SmoothedValue<float> delayTime, delayFb, delayMix;
    OnePoleLP delayDamp;

    juce::dsp::Reverb reverb;
    juce::SmoothedValue<float> reverbMix;

    juce::SmoothedValue<float> bypComp, bypEq, bypDrive, bypOct, bypPhaser, bypDelay, bypReverb;

    std::vector<float> dry, dryL, dryR;

    std::atomic<float> reportedLatency { 0.0f };
};

} // namespace rig
