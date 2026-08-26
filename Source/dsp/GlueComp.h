#pragma once

#include <cmath>

#include "Biquad.h"

namespace rig
{

// Feed-forward compressor with the two behaviours that separate a studio
// unit from a utility: a soft knee, and a release that listens. A brief poke
// over the threshold recovers quickly on its own; a passage that has been
// leaning on the compressor releases at the set time, the way an opto cell's
// memory does. The sidechain is high-passed at 80 Hz so a low E's
// fundamental doesn't pump the whole guitar.
//
// Gain computing happens in the log domain with a 6 dB knee; the parameters
// keep the meanings they had on the stock unit, so presets carry over.
struct GlueComp
{
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        setSidechain (80.0f);
        avgCoef = coefFor (400.0);
        setAttack (attackMs);
        setRelease (releaseMs);
        reset();
    }

    void reset() noexcept
    {
        sidechain.reset();
        grDb = 0.0f;
        avgGr = 0.0f;
    }

    // The detector's high-pass. Above roughly 100 Hz a low E stops ducking
    // the whole instrument, which is the difference between compression and
    // pumping on a bass.
    void setSidechain (float hz) noexcept { sidechain.setHighpass (sr, hz, 0.7071); }

    void setThreshold (float db) noexcept { threshDb = db; }
    void setRatio (float r) noexcept      { slope = 1.0f - 1.0f / (r < 1.0f ? 1.0f : r); }
    void setAttack (float ms) noexcept    { attackMs = ms; aAtk = coefFor (ms); }

    void setRelease (float ms) noexcept
    {
        releaseMs = ms;
        aRelSlow = coefFor (ms);
        aRelFast = coefFor (ms < 80.0f ? ms : 80.0f);
    }

    inline float process (float x) noexcept
    {
        const float sc = sidechain.process (x);
        const float a = std::abs (sc);
        const float lvlDb = 20.0f * std::log10 (a > 1.0e-6f ? a : 1.0e-6f);
        const float over = lvlDb - threshDb;

        float target;

        if (over <= -halfKnee)
            target = 0.0f;
        else if (over >= halfKnee)
            target = slope * over;
        else
        {
            const float t = over + halfKnee;
            target = slope * t * t / (4.0f * halfKnee);
        }

        if (target > grDb)
        {
            grDb += aAtk * (target - grDb);
        }
        else
        {
            // Program-dependent release: the longer the compressor has been
            // leaning in, the more the set release time takes over from the
            // fast transient recovery.
            const float lean = avgGr * (1.0f / 6.0f);
            const float w = lean > 1.0f ? 1.0f : lean;
            const float aRel = aRelFast + (aRelSlow - aRelFast) * w;
            grDb += aRel * (target - grDb);
        }

        avgGr += avgCoef * (grDb - avgGr);

        return x * std::pow (10.0f, grDb * (-1.0f / 20.0f));
    }

    float getReductionDb() const noexcept { return grDb; }

private:
    float coefFor (double ms) const noexcept
    {
        return (float) (1.0 - std::exp (-1.0 / (sr * 0.001 * (ms < 0.05 ? 0.05 : ms))));
    }

    static constexpr float halfKnee = 3.0f;

    double sr = 48000.0;
    Biquad sidechain;
    float threshDb = -18.0f, slope = 0.75f;
    float attackMs = 3.0f, releaseMs = 120.0f;
    float aAtk = 0.01f, aRelSlow = 0.001f, aRelFast = 0.001f, avgCoef = 0.0001f;
    float grDb = 0.0f, avgGr = 0.0f;
};

} // namespace rig
