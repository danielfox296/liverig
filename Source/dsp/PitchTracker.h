#pragma once

#include <vector>
#include "Biquad.h"

namespace rig
{

// Monophonic pitch tracker for the octave shifter.
//
// Stage 1: 6th-order Butterworth lowpass + decimation to ~3 kHz, then YIN
//          (squared difference function + cumulative mean normalisation) over
//          the most recent window. Cheap enough to run inside the callback.
// Stage 2: full-rate refinement of the winning lag, so the period handed to
//          the shifter is accurate to a fraction of a sample. That accuracy is
//          what makes the shifter's grain crossfades phase-coherent.
//
// No allocation or locking after prepare().
class PitchTracker
{
public:
    // The defaults are the bass tracker (26-500 Hz, decimated to ~3 kHz);
    // a voice wants a higher rate and range.
    void prepare (double sampleRate, double decimatedRate = 3000.0,
                  double lowestHz = 26.0, double highestHz = 500.0);
    void reset() noexcept;

    void process (const float* in, int numSamples) noexcept;

    // Full-rate period in samples. Never zero after prepare(); holds the last
    // good estimate through unvoiced passages.
    float getPeriodSamples() const noexcept  { return periodSamples; }
    float getFrequency()     const noexcept  { return (float) (sr / periodSamples); }
    float getConfidence()    const noexcept  { return confidence; }

    // diagnostics for the offline harness
    float getCoarsePeriod()  const noexcept  { return coarsePeriod; }

private:
    void analyse() noexcept;
    float refineAtFullRate (float coarsePeriodIn) noexcept;

    double sr = 48000.0;
    int decim = 16;
    double decRate = 3000.0;

    Biquad aa[3];
    int decCounter = 0;

    std::vector<float> decBuf;      // decimated circular buffer
    int decMask = 0, decWrite = 0;
    int sinceAnalysis = 0, analysisHop = 32;

    std::vector<float> frBuf;       // full-rate circular buffer
    int frMask = 0, frWrite = 0;

    std::vector<float> scratch;     // contiguous copy for the YIN inner loop
    std::vector<float> frScratch;
    std::vector<float> diff, cmnd, diffFull;

    int window = 192;
    int tauMin = 6, tauMax = 100;

    float periodSamples = 600.0f;
    float coarsePeriod = 600.0f;
    float maxPeriod = 1600.0f;
    float confidence = 0.0f;
};

} // namespace rig
