#pragma once

#include <atomic>

#include <juce_audio_devices/juce_audio_devices.h>

#include "EffectSlot.h"
#include "FxChain.h"
#include "InstrumentHost.h"
#include "MidiSender.h"
#include "Parameters.h"
#include "StemPlayer.h"
#include "VocalChannel.h"
#include "dsp/Limiter.h"

namespace rig
{

// Owns the device, all three lanes and the master sum. The callback
// allocates nothing, locks nothing and logs nothing.
class AudioEngine : public juce::AudioIODeviceCallback,
                    private juce::MidiInputCallback
{
public:
    explicit AudioEngine (ParamStore& p);
    ~AudioEngine() override;

    juce::String initialise (std::unique_ptr<juce::XmlElement> savedState);
    void shutdown();

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    MidiSender& getMidi() noexcept                        { return midi; }
    InstrumentHost& getInstrument() noexcept              { return instrument; }

    StemPlayer& getStemPlayer() noexcept                  { return stems; }
    FxChain& getFxChain() noexcept                        { return fx; }

    EffectSlot& getTuneSlot() noexcept                    { return tuneSlot; }
    EffectSlot& getPedalSlot() noexcept                   { return pedalSlot; }

    // Every connected MIDI input feeds the rig; Program Change 0/1 switches
    // the autotune off/on, which is how the foot controller will drive it.
    // Called again periodically so a controller plugged in mid-session joins.
    void enableAllMidiInputs();

    double getSampleRate() const noexcept   { return currentRate.load (std::memory_order_relaxed); }
    int getBufferSize() const noexcept      { return currentBlock.load (std::memory_order_relaxed); }
    float getFxLatencyMs() const noexcept;

    // What the device itself costs: input and output buffering plus the
    // driver's safety offsets, as reported by CoreAudio.
    float getIoLatencyMs() noexcept;

    float getInputPeak() const noexcept     { return inPeak.load (std::memory_order_relaxed); }
    float getVocalPeak() const noexcept     { return vocalPeak.load (std::memory_order_relaxed); }

    // How many input channels the device actually opened. The mic lane needs
    // two; on a one-input device the Autotune row says so instead of playing
    // an unexplained silence.
    int getActiveInputCount() const noexcept { return activeInputs.load (std::memory_order_relaxed); }
    float getOutputPeakL() const noexcept   { return outPeakL.load (std::memory_order_relaxed); }
    float getOutputPeakR() const noexcept   { return outPeakR.load (std::memory_order_relaxed); }

    // How hard the master limiter is working, in positive dB of reduction —
    // zero when the sum is under the ceiling.
    float getLimiterReductionDb() const noexcept { return limiterGr.load (std::memory_order_relaxed); }

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    void preferLowLatencyBuffer();
    void handleIncomingMidiMessage (juce::MidiInput* source,
                                    const juce::MidiMessage& message) override;

    juce::AudioDeviceManager deviceManager;

    ParamStore& params;
    StemPlayer stems;
    FxChain fx;

    // The autotune lane and the two hosted-plugin roles: the pitch corrector
    // on the mic, the pedal as a bass-chain insert.
    EffectSlot tuneSlot { "tune" };
    EffectSlot pedalSlot { "pedal" };
    VocalChannel vocal { params, tuneSlot };

    // The tracker as a note source. FxChain produces the events; carrying
    // them to a port is the engine's job, not the chain's.
    MidiSender midi;

    // An AU instrument played by the same notes, summed into the live lane
    // so it counts as part of the performance rather than as backing.
    InstrumentHost instrument;
    float instrumentGain = 1.0f;

    // Playback-lane dynamics: the song is limited into the headroom the
    // instrument leaves, so a song never ducks the playing.
    StemDucker ducker;

    // The master bus: one gain over the whole sum, then the safety limiter,
    // so the device is never handed a sample it would hard-clip.
    juce::SmoothedValue<float> masterGain;
    MasterLimiter limiter;

    juce::AudioBuffer<float> scratch;   // monoIn, liveL, liveR, mixL, mixR, vocalIn
    int maxBlock = 512;

    std::atomic<double> currentRate { 0.0 };
    std::atomic<int> currentBlock { 0 };
    std::atomic<int> activeInputs { 0 };
    std::atomic<float> inPeak { 0.0f }, vocalPeak { 0.0f }, outPeakL { 0.0f }, outPeakR { 0.0f };
    std::atomic<float> limiterGr { 0.0f };
    bool callbackAdded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace rig
