#pragma once

#include <cmath>
#include <vector>

namespace rig
{

// Pitch-synchronous delay-line pitch shifter.
//
// Two read taps run through a circular buffer at the shift ratio. When a tap
// runs out of headroom it is relocated by exactly one detected period and
// crossfaded in. Because the taps are one period apart, the crossfade is
// between waveform-aligned copies of the same signal: no comb, no warble, no
// grain flutter — the artefacts that make generic granular shifters unusable
// on low bass. Latency is roughly three quarters of a period (about 18 ms on
// a low E, 9 ms an octave up) and is reported to the UI.
//
// Falls back to the last known period when the tracker is unsure, so it
// degrades to a plain granular shifter rather than dropping out.
class OctaveShifter
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;

    void setSemitones (float semitones) noexcept;
    void setPeriod (float fullRatePeriodSamples) noexcept;

    // In place, 100% wet. Dry/wet is handled by the chain.
    void process (float* data, int numSamples) noexcept;

    float getLagSamples() const noexcept { return lagReport; }

private:
    inline float readInterp (double p) const noexcept
    {
        const auto i = (long long) std::floor (p);
        const float f = (float) (p - (double) i);

        const float xm1 = buf[(size_t) ((i - 1) & mask)];
        const float x0  = buf[(size_t) ( i      & mask)];
        const float x1  = buf[(size_t) ((i + 1) & mask)];
        const float x2  = buf[(size_t) ((i + 2) & mask)];

        // 4-point, 3rd-order Hermite.
        const float c = (x1 - xm1) * 0.5f;
        const float v = x0 - x1;
        const float w = c + v;
        const float a = w + v + (x2 - x0) * 0.5f;
        const float b = w + a;

        return ((a * f - b) * f + c) * f + x0;
    }

    void startJump (double delta) noexcept;

    std::vector<float> buf;
    long long mask = 0;
    long long writePos = 0;

    double pos[2] { 0.0, 0.0 };
    float gain = 0.0f, gainTarget = 0.0f, gainStep = 0.0f;
    bool fading = false;

    double alpha = 2.0;
    float period = 600.0f;
    float jumpPeriod = 600.0f;
    int xfadeLen = 128;

    double minLag = 8.0;
    float lagReport = 0.0f;
    double sr = 48000.0;
};

} // namespace rig
