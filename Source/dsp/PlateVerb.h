#pragma once

#include <vector>

#include "Biquad.h"

namespace rig
{

// Dattorro's figure-eight plate (Effect Design Part 1, JAES 1997), the
// published design in the Lexicon lineage: predelay and a bandwidth filter
// into four diffusion allpasses, then two cross-coupled tank halves whose
// first allpasses are chorused by slow LFOs. The modulation is the point —
// it smears the tank's standing modes so sustained notes bloom instead of
// ringing metallically, which is what a static comb network can't do.
//
// Mono in, stereo out: the two outputs are accumulated from taps scattered
// across both halves, so the width is real decorrelation, not a delayed copy.
//
// Size scales the tank and sets the decay time together (one musical knob);
// the loop gain is derived from the commanded RT60 so the tail length tracks
// the knob even as the tank stretches. Damping is frequency-dependent decay:
// a one-pole in each half loses highs per pass, and a fixed low cut keeps
// long tails from piling up mud below the guitar's fundamental.
//
// Everything runs at constant per-sample cost with no allocation after
// prepare(), so it can idle engaged like the rest of the chain.
class PlateVerb
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    void setSize (float v) noexcept       { sizeTarget = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
    void setDamping (float v) noexcept    { dampTarget = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
    void setPredelayMs (float v) noexcept { preMsTarget = v < 0.0f ? 0.0f : v; }
    void setModDepth (float v) noexcept   { modTarget = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    // in is left untouched; left/right receive pure wet — the chain owns the
    // dry/wet blend the same way it does for every other stage.
    void process (const float* in, float* left, float* right, int numSamples) noexcept;

private:
    // Masked circular buffer with a Hermite fractional read. read (d) returns
    // the value written d samples before the sample about to be pushed, so
    // every read happens before this sample's push.
    struct Line
    {
        std::vector<float> buf;
        int mask = 0, write = 0;

        void prepare (int capacity);
        void clear() noexcept;

        inline void push (float x) noexcept
        {
            buf[(size_t) (write & mask)] = x;

            if (++write >= (int) buf.size() * 2)
                write -= (int) buf.size();
        }

        float read (float delaySamples) const noexcept;
    };

    // Lattice allpass on a Line: w[n] = x + g w[n-D], y = w[n-D] - g w[n].
    struct Allpass
    {
        Line line;
        float g = 0.5f;

        inline float process (float x, float delaySamples) noexcept
        {
            const float r = line.read (delaySamples);
            const float w = x + g * r;
            line.push (w);
            return r - g * w;
        }
    };

    void updateTargets() noexcept;

    double sr = 48000.0;
    float srScale = 48000.0f / 29761.0f;   // lengths are quoted at Dattorro's rate

    Line pre;
    OnePoleLP bandwidth;
    Allpass inDiff[4];

    // Tank halves: modulated allpass, delay, damping + low cut, second
    // allpass, delay. A's tail feeds B's input and vice versa.
    Allpass modApA, modApB, apA2, apB2;
    Line delayA1, delayA2, delayB1, delayB2;
    OnePoleLP dampA, dampB, lowcutA, lowcutB;

    double lfoPhaseA = 0.0, lfoPhaseB = 0.5;

    // Smoothed per-sample state, one-pole toward the block-rate targets.
    float sizeTarget = 0.5f, dampTarget = 0.5f, preMsTarget = 12.0f, modTarget = 0.5f;
    float scaleState = 0.7250f, decayState = 0.6f, excState = 0.0f, preState = 1.0f;
    float scaleGoal = 0.7250f, decayGoal = 0.6f, excGoal = 0.0f, preGoal = 1.0f;
    float g2 = 0.5f;
    float scaleCoef = 0.0f, decayCoef = 0.0f, excCoef = 0.0f, preCoef = 0.0f;

    float lastSize = -1.0f, lastDamp = -1.0f, lastPreMs = -1.0f, lastMod = -1.0f;

    // First block after prepare/reset starts settled on the targets instead
    // of gliding in from wherever the smoothers were.
    bool snapOnNext = true;

    float dn = 1.0e-20f;   // anti-denormal injection for the recirculating tank
};

} // namespace rig
