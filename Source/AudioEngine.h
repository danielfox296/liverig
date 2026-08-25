#pragma once

#include <atomic>

#include <juce_audio_devices/juce_audio_devices.h>

#include "FxChain.h"
#include "Parameters.h"
#include "StemPlayer.h"

namespace rig
{

// Owns the device, both lanes and the master sum. The callback allocates
// nothing, locks nothing and logs nothing.
class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    explicit AudioEngine (ParamStore& p);
    ~AudioEngine() override;

    juce::String initialise (std::unique_ptr<juce::XmlElement> savedState);
    void shutdown();

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    StemPlayer& getStemPlayer() noexcept                  { return stems; }
    FxChain& getFxChain() noexcept                        { return fx; }

    double getSampleRate() const noexcept   { return currentRate.load (std::memory_order_relaxed); }
    int getBufferSize() const noexcept      { return currentBlock.load (std::memory_order_relaxed); }
    float getFxLatencyMs() const noexcept;

    float getInputPeak() const noexcept     { return inPeak.load (std::memory_order_relaxed); }
    float getOutputPeakL() const noexcept   { return outPeakL.load (std::memory_order_relaxed); }
    float getOutputPeakR() const noexcept   { return outPeakR.load (std::memory_order_relaxed); }

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    juce::AudioDeviceManager deviceManager;

    StemPlayer stems;
    FxChain fx;

    juce::AudioBuffer<float> scratch;   // monoIn, liveL, liveR, mixL, mixR
    int maxBlock = 512;

    std::atomic<double> currentRate { 0.0 };
    std::atomic<int> currentBlock { 0 };
    std::atomic<float> inPeak { 0.0f }, outPeakL { 0.0f }, outPeakR { 0.0f };
    bool callbackAdded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace rig
