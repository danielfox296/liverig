#include "VocalChannel.h"

#include <cmath>

namespace rig
{

void VocalChannel::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    const auto n = (size_t) juce::jmax (16, maxBlockSize);

    gated.assign (n, 0.0f);
    laneL.assign (n, 0.0f);
    laneR.assign (n, 0.0f);

    // Fast enough to keep a plosive's onset, slow enough not to chatter.
    gateEnv.setTimes (sr, 0.5, 60.0);
    gateEnv.reset();
    gateState = 0.0f;
    gateOpen = false;
    gateCoef = (float) (1.0 - std::exp (-1.0 / (0.008 * sr)));

    for (auto* s : { &gain, &level, &onRamp })
        s->reset (sr, 0.015);

    gain.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (params.get (pVocalGain), -60.0f));
    level.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (params.get (pVocalLevel), -60.0f));
    onRamp.setCurrentAndTargetValue (params.getBool (pVocalOn) ? 1.0f : 0.0f);
}

void VocalChannel::process (const float* mic, float* left, float* right, int numSamples) noexcept
{
    if ((size_t) numSamples > gated.size())
        return;

    gain.setTargetValue (juce::Decibels::decibelsToGain (params.get (pVocalGain), -60.0f));
    level.setTargetValue (juce::Decibels::decibelsToGain (params.get (pVocalLevel), -60.0f));
    onRamp.setTargetValue (params.getBool (pVocalOn) ? 1.0f : 0.0f);

    // Fully faded out means fully skipped — the plugin costs nothing while
    // the row is off.
    const bool alive = onRamp.getCurrentValue() > 1.0e-4f
                         || onRamp.getTargetValue() > 0.5f;

    if (! alive)
    {
        gain.skip (numSamples);
        level.skip (numSamples);
        onRamp.skip (numSamples);
        return;
    }

    // Hysteresis around the threshold: open at Gate, close 6 dB under it,
    // so a level sitting right on the line does not flutter the door.
    const float openThr = juce::Decibels::decibelsToGain (params.get (pVocalGate), -80.0f);
    const float closeThr = openThr * 0.5f;

    for (int i = 0; i < numSamples; ++i)
    {
        // The detector listens to the raw mic, ahead of the gain fader: Gate
        // is a fact about the microphone, and pulling Gain down must make
        // the lane quieter, never lock its door.
        const float e = gateEnv.process (mic[i]);

        gateOpen = gateOpen ? (e > closeThr) : (e > openThr);
        gateState += gateCoef * ((gateOpen ? 1.0f : 0.0f) - gateState);
        gated[(size_t) i] = mic[i] * gain.getNextValue() * gateState;
    }

    // An empty slot writes silence here, which is the honest result of
    // "autotune on, no pitch plugin installed".
    tuneSlot.process (gated.data(), nullptr, laneL.data(), laneR.data(), numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        const float g = onRamp.getNextValue() * level.getNextValue();
        left[i] += laneL[(size_t) i] * g;
        right[i] += laneR[(size_t) i] * g;
    }
}

} // namespace rig
