#pragma once

#include "Biquad.h"

namespace rig
{

// A speaker cabinet is a filter, and most of what people call "amp tone" is
// this rather than the valves: a resonant low-mid bump, nothing below the
// cab's tuning, and almost nothing above 4-5 kHz. Without it a clipping stage
// sounds like fizzing insects.
//
// The Amp stage covers this when a capture is loaded, since a NAM capture is
// taken through a real cab. This is what stands in when none is.
class CabSim
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        rumble.setHighpass (sr, 42.0, 0.7071);
        reset();
    }

    void reset() noexcept
    {
        rumble.reset();
        bump.reset();
        presence.reset();
        top[0].reset();
        top[1].reset();
    }

    void setParams (float bumpFreq, float bumpGainDb, float topHz, float presenceDb) noexcept
    {
        bump.setPeak (sr, bumpFreq, 1.1, bumpGainDb);
        presence.setPeak (sr, 2200.0, 0.9, presenceDb);
        top[0].setLowpass (sr, topHz, 0.5412);   // 4th-order Butterworth
        top[1].setLowpass (sr, topHz, 1.3066);
    }

    void process (float* data, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float x = rumble.process (data[i]);
            x = presence.process (bump.process (x));
            data[i] = top[1].process (top[0].process (x));
        }
    }

private:
    double sr = 48000.0;
    Biquad rumble, bump, presence, top[2];
};

} // namespace rig
