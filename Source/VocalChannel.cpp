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

    // A lane fully faded out is fully skipped — the plugin is the expensive
    // part, and a silent one is not worth running. The front end always
    // runs: with both lanes down the microphone itself is the output.
    const bool tuneAlive = tuneRamp.getCurrentValue() > 1.0e-4f
                             || tuneRamp.getTargetValue() > 0.5f;
    const bool vocoderAlive = vocoderRamp.getCurrentValue() > 1.0e-4f
                                || vocoderRamp.getTargetValue() > 0.5f;

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
        const float tune = tuneRamp.getNextValue();
        const float voc = vocoderRamp.getNextValue();
        const float tuneGain = tune * tuneLevel.getNextValue();
        const float vocGain = voc * vocoderLevel.getNextValue();

        // The direct voice holds the channel until a lane takes it over, so
        // the strip is never heard twice over.
        const float dry = gated[(size_t) i] * (1.0f - juce::jmax (tune, voc));

        float l = dry, r = dry;

        if (tuneAlive)
        {
            l += tuneL[(size_t) i] * tuneGain;
            r += tuneR[(size_t) i] * tuneGain;
        }

        if (vocoderAlive)
        {
            l += vocL[(size_t) i] * vocGain;
            r += vocR[(size_t) i] * vocGain;
        }

        left[i] += l;
        right[i] += r;
    }
}

} // namespace rig
