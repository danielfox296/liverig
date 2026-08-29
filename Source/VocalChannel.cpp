#include "VocalChannel.h"

#include <cmath>

namespace rig
{

void VocalChannel::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    const auto n = (size_t) juce::jmax (16, maxBlockSize);

    gated.assign (n, 0.0f);
    tuneL.assign (n, 0.0f);
    tuneR.assign (n, 0.0f);
    vocL.assign (n, 0.0f);
    vocR.assign (n, 0.0f);

    // Fast enough to keep a plosive's onset, slow enough not to chatter.
    gateEnv.setTimes (sr, 0.5, 60.0);
    gateEnv.reset();
    gateState = 0.0f;
    gateOpen = false;
    gateCoef = (float) (1.0 - std::exp (-1.0 / (0.008 * sr)));

    for (auto* s : { &gain, &tuneLevel, &vocoderLevel, &tuneRamp, &vocoderRamp })
        s->reset (sr, 0.015);

    gain.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (params.get (pVocalGain), -60.0f));
    tuneLevel.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (params.get (pVocalLevel), -60.0f));
    vocoderLevel.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (params.get (pVocoderLevel), -60.0f));
    tuneRamp.setCurrentAndTargetValue (params.getBool (pVocalOn) ? 1.0f : 0.0f);
    vocoderRamp.setCurrentAndTargetValue (params.getBool (pVocoderOn) ? 1.0f : 0.0f);
}

void VocalChannel::process (const float* mic, const float* carrier,
                            float* left, float* right, int numSamples) noexcept
{
    if ((size_t) numSamples > gated.size())
        return;

    gain.setTargetValue (juce::Decibels::decibelsToGain (params.get (pVocalGain), -60.0f));
    tuneLevel.setTargetValue (juce::Decibels::decibelsToGain (params.get (pVocalLevel), -60.0f));
    vocoderLevel.setTargetValue (juce::Decibels::decibelsToGain (params.get (pVocoderLevel), -60.0f));
    tuneRamp.setTargetValue (params.getBool (pVocalOn) ? 1.0f : 0.0f);
    vocoderRamp.setTargetValue (params.getBool (pVocoderOn) ? 1.0f : 0.0f);

    // A lane fully faded out is fully skipped, and with both lanes down the
    // whole channel costs nothing.
    const bool tuneAlive = tuneRamp.getCurrentValue() > 1.0e-4f
                             || tuneRamp.getTargetValue() > 0.5f;
    const bool vocoderAlive = vocoderRamp.getCurrentValue() > 1.0e-4f
                                || vocoderRamp.getTargetValue() > 0.5f;

    if (! tuneAlive && ! vocoderAlive)
    {
        gain.skip (numSamples);
        tuneLevel.skip (numSamples);
        vocoderLevel.skip (numSamples);
        tuneRamp.skip (numSamples);
        vocoderRamp.skip (numSamples);
        return;
    }

    // Hysteresis around the threshold: open at Gate, close 6 dB under it,
    // so a level sitting right on the line does not flutter the door. The
    // detector listens to the raw mic, ahead of the gain fader.
    const float openThr = juce::Decibels::decibelsToGain (params.get (pVocalGate), -80.0f);
    const float closeThr = openThr * 0.5f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float e = gateEnv.process (mic[i]);

        gateOpen = gateOpen ? (e > closeThr) : (e > openThr);
        gateState += gateCoef * ((gateOpen ? 1.0f : 0.0f) - gateState);
        gated[(size_t) i] = mic[i] * gain.getNextValue() * gateState;
    }

    if (tuneAlive)
        tuneSlot.process (gated.data(), nullptr, tuneL.data(), tuneR.data(), numSamples);

    if (vocoderAlive)
        vocoderSlot.process (gated.data(), carrier, vocL.data(), vocR.data(), numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        float l = 0.0f, r = 0.0f;

        if (tuneAlive)
        {
            const float g = tuneRamp.getNextValue() * tuneLevel.getNextValue();
            l += tuneL[(size_t) i] * g;
            r += tuneR[(size_t) i] * g;
        }
        else
        {
            tuneRamp.skip (1);
            tuneLevel.skip (1);
        }

        if (vocoderAlive)
        {
            const float g = vocoderRamp.getNextValue() * vocoderLevel.getNextValue();
            l += vocL[(size_t) i] * g;
            r += vocR[(size_t) i] * g;
        }
        else
        {
            vocoderRamp.skip (1);
            vocoderLevel.skip (1);
        }

        left[i] += l;
        right[i] += r;
    }
}

} // namespace rig
