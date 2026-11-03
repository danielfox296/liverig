#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "Biquad.h"
#include "OctaveShifter.h"
#include "PitchTracker.h"

namespace rig
{

// The mic lanes' built-in tenants, for when no hosted plugin holds the role
// (an iPhone usually has neither Graillon nor TAL-Vocoder). The lanes prefer
// a hosted plugin whenever one is loaded; these are what plays otherwise.

//==============================================================================
// Pitch correction: the voice's own tracker steers the rig's
// pitch-synchronous shifter onto the nearest note of the key and scale.
// Retune is how long the correction takes to arrive (0 is the hard,
// robotic snap); Amount scales how far it goes.
class BuiltInAutotune
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        // A voice: up to ~1 kHz, so a higher decimated rate than the bass.
        tracker.prepare (sampleRate, 8000.0, 70.0, 1000.0);
        shifter.prepare (sampleRate);
        reset();
    }

    void reset() noexcept
    {
        tracker.reset();
        shifter.reset();
        shift = 0.0f;
        target = -1.0f;
    }

    // In place. key 0-11 (C..B), scale an index into scales().
    void process (float* data, int numSamples, int key, int scale, float retuneMs, float amount) noexcept
    {
        tracker.process (data, numSamples);

        if (tracker.getConfidence() > 0.6f)
        {
            const float midi = 69.0f + 12.0f * std::log2 (tracker.getFrequency() / 440.0f);
            const auto& notes = scales()[(size_t) std::clamp (scale, 0, (int) scales().size() - 1)];

            float best = midi, bestDistance = 1.0e9f;
            const int base = (int) std::floor (midi) - 12;

            for (int m = base; m <= base + 24; ++m)
            {
                const int degree = ((m - key) % 12 + 12) % 12;

                if (std::find (notes.begin(), notes.end(), degree) == notes.end())
                    continue;

                const float d = std::abs ((float) m - midi);

                if (d < bestDistance)
                {
                    bestDistance = d;
                    best = (float) m;
                }
            }

            // Hold the note it is on until the voice is clearly nearer another:
            // a note sung between two scale steps must not flip between them.
            if (target < 0.0f || std::abs (target - midi) > bestDistance + 0.3f
                 || std::abs (target - midi) > 1.5f)
                target = best;

            const float wanted = (target - midi) * std::clamp (amount, 0.0f, 1.0f);
            const float coef = retuneMs < 1.0f ? 1.0f
                                               : 1.0f - std::exp (-(float) numSamples * 1000.0f / ((float) sr * retuneMs));
            shift += coef * (wanted - shift);
        }

        shifter.setPeriod (tracker.getPeriodSamples());
        shifter.setSemitones (shift);
        shifter.process (data, numSamples);
    }

    float getLatencySamples() const noexcept { return shifter.getLagSamples(); }

    static const std::array<std::vector<int>, 6>& scales()
    {
        static const std::array<std::vector<int>, 6> table {
            std::vector<int> { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 },   // chromatic
            std::vector<int> { 0, 2, 4, 5, 7, 9, 11 },                   // major
            std::vector<int> { 0, 2, 3, 5, 7, 8, 10 },                   // minor
            std::vector<int> { 0, 2, 4, 7, 9 },                          // major pentatonic
            std::vector<int> { 0, 3, 5, 7, 10 },                         // minor pentatonic
            std::vector<int> { 0, 3, 5, 6, 7, 10 },                      // blues
        };
        return table;
    }

private:
    double sr = 48000.0;
    PitchTracker tracker;
    OctaveShifter shifter;
    float shift = 0.0f;
    float target = -1.0f;   // the scale note being held, as a MIDI number
};

//==============================================================================
// A 20-band channel vocoder. The mic is the modulator; the carrier is the
// bass after its whole board. Each carrier band is flattened by its own
// envelope and re-shaped by the voice's, which is what makes words land on
// a bass line. Consonants mixes the mic's own top end back in, because an
// "s" has no pitch for a carrier to give it.
class BuiltInVocoder
{
public:
    static constexpr int numBands = 20;

    void prepare (double sampleRate)
    {
        sr = sampleRate;

        for (int b = 0; b < numBands; ++b)
        {
            const double f = 120.0 * std::pow (7000.0 / 120.0, (double) b / (numBands - 1));

            for (auto* q : { &bands[(size_t) b].m1, &bands[(size_t) b].m2,
                             &bands[(size_t) b].c1, &bands[(size_t) b].c2 })
                q->setBandpass (sr, f, 7.0);
        }

        sibilance.setHighpass (sr, 5000.0, 0.7071);
        attack = 1.0f - std::exp (-1.0f / (0.003f * (float) sr));
        carrierRelease = 1.0f - std::exp (-1.0f / (0.25f * (float) sr));
        lastReleaseMs = -1.0f;
        reset();
    }

    void reset() noexcept
    {
        for (auto& b : bands)
        {
            b.m1.reset(); b.m2.reset(); b.c1.reset(); b.c2.reset();
            b.modEnv = b.carEnv = 0.0f;
        }

        sibilance.reset();
    }

    void process (const float* modulator, const float* carrier, float* out, int numSamples,
                  float consonants, float releaseMs) noexcept
    {
        if (releaseMs != lastReleaseMs)
        {
            lastReleaseMs = releaseMs;
            release = 1.0f - std::exp (-1.0f / (std::max (5.0f, releaseMs) * 0.001f * (float) sr));
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const float m = modulator[i];
            const float c = carrier != nullptr ? carrier[i] : 0.0f;
            float acc = 0.0f;

            for (auto& b : bands)
            {
                const float mb = b.m2.process (b.m1.process (m));
                const float cb = b.c2.process (b.c1.process (c));
                const float ma = std::abs (mb), ca = std::abs (cb);

                b.modEnv += (ma > b.modEnv ? attack : release) * (ma - b.modEnv);
                b.carEnv += (ca > b.carEnv ? attack : carrierRelease) * (ca - b.carEnv);
                acc += cb * std::min (8.0f, b.modEnv / (b.carEnv + 1.0e-4f));
            }

            out[i] = acc * 0.35f + sibilance.process (m) * consonants;
        }
    }

private:
    struct Band
    {
        Biquad m1, m2, c1, c2;
        float modEnv = 0.0f, carEnv = 0.0f;
    };

    double sr = 48000.0;
    std::array<Band, numBands> bands;
    Biquad sibilance;
    float attack = 0.01f, release = 0.001f, carrierRelease = 0.0001f, lastReleaseMs = -1.0f;
};

} // namespace rig
