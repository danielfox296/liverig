#pragma once

#include <cmath>

namespace rig
{

// Safety limiter for the master sum. Zero lookahead — this is a live path —
// so the envelope attacks instantly and releases over ~120 ms. Instant
// attack means the output mathematically never passes the ceiling: gain is
// ceiling/env and env is never below the current peak. On sustained hot
// material the envelope rides the waveform's crest, so the gain sits still
// and the sound stays clean; only transient tips get ducked.
//
// The alternative to this stage is the DAC hard-clipping, which is never
// the better sound.
struct MasterLimiter
{
    float env = 0.0f;
    float rel = 0.9999f;
    float lastGain = 1.0f;

    static constexpr float ceiling = 0.891f;   // -1 dBFS

    void prepare (double sr) noexcept
    {
        rel = (float) std::exp (-1.0 / (0.12 * sr));
        reset();
    }

    void reset() noexcept
    {
        env = 0.0f;
        lastGain = 1.0f;
    }

    inline void process (float& l, float& r) noexcept
    {
        const float al = std::abs (l);
        const float ar = std::abs (r);
        const float peak = al > ar ? al : ar;

        env = peak > env ? peak : peak + (env - peak) * rel;

        const float g = env > ceiling ? ceiling / env : 1.0f;
        lastGain = g;

        l *= g;
        r *= g;
    }

    // How hard it is working right now, as positive dB of reduction.
    float getReductionDb() const noexcept
    {
        return lastGain < 1.0f ? -20.0f * std::log10 (lastGain) : 0.0f;
    }
};

} // namespace rig
