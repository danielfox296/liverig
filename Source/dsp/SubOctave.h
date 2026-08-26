#pragma once

#include <algorithm>
#include <cmath>

#include "Biquad.h"

namespace rig
{

// An analogue-style octave divider, of the kind people still pay over the odds
// for a discontinued OC-2 to get.
//
// This is not the pitch shifter and not the synth's sub oscillator. A flip-flop
// toggles on every rising zero crossing, so the output is a square at half (or
// a quarter of) the input frequency with no relation to the shape of what you
// played. That squareness, and the way it stumbles on a chord or a dying note,
// is the sound. The tracker's period sets the crossing filter, which is what
// stops harmonics triggering false edges.
class SubOctave
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        attack = 1.0f - std::exp (-1.0f / (0.002f * (float) sr));
        release = 1.0f - std::exp (-1.0f / (0.140f * (float) sr));
        dc.prepare (sr);
        setTone (500.0f);
        setPeriod ((float) sr / 80.0f);
        reset();
    }

    void reset() noexcept
    {
        track[0].reset();
        track[1].reset();
        tone.reset();
        dc.reset();
        env = 0.0f;
        flip = false;
        above = false;
        counter = 0;
    }

    void setPeriod (float periodSamples) noexcept
    {
        const float f0 = (float) sr / std::max (8.0f, periodSamples);
        const double cutoff = std::clamp (1.4 * (double) f0, 55.0, 420.0);
        track[0].setCutoff (sr, cutoff);
        track[1].setCutoff (sr, cutoff);
    }

    void setTone (float hz) noexcept { tone.setCutoff (sr, hz); }

    // Writes the sub voice to out. The chain adds it under the dry signal — an
    // octave that replaces your note is a transposition, not an octave.
    void process (const float* in, float* out, int numSamples, int divisor) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float x = in[i];
            const float rect = std::abs (x);
            env += (rect > env ? attack : release) * (rect - env);

            const float key = track[1].process (track[0].process (x));
            const float hysteresis = 0.06f * env + 1.0e-5f;

            if (! above && key > hysteresis)
            {
                above = true;

                if (++counter >= divisor)
                {
                    counter = 0;
                    flip = ! flip;
                }
            }
            else if (above && key < -hysteresis)
            {
                above = false;
            }

            out[i] = tone.process (dc.process (flip ? env : -env));
        }
    }

private:
    double sr = 48000.0;
    OnePoleLP track[2], tone;
    DCBlocker dc;
    float env = 0.0f, attack = 0.01f, release = 0.001f;
    bool flip = false, above = false;
    int counter = 0;
};

} // namespace rig
