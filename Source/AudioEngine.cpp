#include "AudioEngine.h"

namespace rig
{

AudioEngine::AudioEngine (ParamStore& p) : stems (p), fx (p) {}

AudioEngine::~AudioEngine()
{
    shutdown();
}

juce::String AudioEngine::initialise (std::unique_ptr<juce::XmlElement> savedState)
{
    auto error = deviceManager.initialise (2, 2, savedState.get(), true);

    if (error.isEmpty())
    {
        deviceManager.addAudioCallback (this);
        callbackAdded = true;
    }

    return error;
}

void AudioEngine::shutdown()
{
    if (callbackAdded)
    {
        deviceManager.removeAudioCallback (this);
        callbackAdded = false;
    }

    deviceManager.closeAudioDevice();
}

float AudioEngine::getFxLatencyMs() const noexcept
{
    const double rate = currentRate.load (std::memory_order_relaxed);
    return rate > 0.0 ? (float) (fx.getLatencySamples() / rate * 1000.0) : 0.0f;
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double rate = device->getCurrentSampleRate();
    maxBlock = juce::jmax (16, device->getCurrentBufferSizeSamples());

    currentRate.store (rate, std::memory_order_relaxed);
    currentBlock.store (maxBlock, std::memory_order_relaxed);

    scratch.setSize (5, maxBlock, false, true, false);
    scratch.clear();

    fx.prepare (rate, maxBlock);
    stems.prepare (rate, maxBlock);

    inPeak.store (0.0f);
    outPeakL.store (0.0f);
    outPeakR.store (0.0f);
}

void AudioEngine::audioDeviceStopped()
{
    currentRate.store (0.0, std::memory_order_relaxed);
    currentBlock.store (0, std::memory_order_relaxed);
    inPeak.store (0.0f);
    outPeakL.store (0.0f);
    outPeakR.store (0.0f);
}

static void updatePeak (std::atomic<float>& slot, const float* data, int n, float decay) noexcept
{
    float peak = 0.0f;

    for (int i = 0; i < n; ++i)
        peak = juce::jmax (peak, std::abs (data[i]));

    const float previous = slot.load (std::memory_order_relaxed) * decay;
    slot.store (juce::jmax (peak, previous), std::memory_order_relaxed);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;

    const double rate = currentRate.load (std::memory_order_relaxed);
    const float decay = rate > 0.0 ? std::exp (-(float) numSamples / (float) (rate * 0.35)) : 0.0f;

    float* monoIn = scratch.getWritePointer (0);
    float* liveL  = scratch.getWritePointer (1);
    float* liveR  = scratch.getWritePointer (2);
    float* mixL   = scratch.getWritePointer (3);
    float* mixR   = scratch.getWritePointer (4);

    for (int offset = 0; offset < numSamples;)
    {
        const int n = juce::jmin (maxBlock, numSamples - offset);

        // --- live lane: instrument on the first active input channel ---
        if (numInputChannels > 0 && inputChannelData[0] != nullptr)
            juce::FloatVectorOperations::copy (monoIn, inputChannelData[0] + offset, n);
        else
            juce::FloatVectorOperations::clear (monoIn, n);

        updatePeak (inPeak, monoIn, n, decay);

        fx.process (monoIn, liveL, liveR, n);

        // --- playback lane ---
        juce::FloatVectorOperations::clear (mixL, n);
        juce::FloatVectorOperations::clear (mixR, n);

        stems.process (mixL, mixR, n);

        // --- master sum ---
        juce::FloatVectorOperations::add (mixL, liveL, n);
        juce::FloatVectorOperations::add (mixR, liveR, n);

        updatePeak (outPeakL, mixL, n, decay);
        updatePeak (outPeakR, mixR, n, decay);

        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            if (outputChannelData[ch] == nullptr)
                continue;

            const float* src = ch == 0 ? mixL : (ch == 1 ? mixR : nullptr);

            if (src != nullptr)
                juce::FloatVectorOperations::copy (outputChannelData[ch] + offset, src, n);
            else
                juce::FloatVectorOperations::clear (outputChannelData[ch] + offset, n);
        }

        offset += n;
    }
}

} // namespace rig
