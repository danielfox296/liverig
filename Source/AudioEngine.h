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

    // A channel's own pedalboard: slot 0 is the bass chain, 1 the mic
    // strip's, 2 the keys strip's, 3 and up the generic strips'.
    FxChain& getChain (int slot) noexcept
    {
        if (slot >= 3 && slot < ParamStore::maxChannels && spareChains[slot - 3] != nullptr)
            return *spareChains[slot - 3];

        return slot == 1 ? micChain : slot == 2 ? keysChain : fx;
    }

    // A signal runs through its strip's board only while the board holds at
    // least one pedal; an empty board passes it through untouched but for
    // the strip's input and live-out gains.
    void setChainActive (int slot, bool active) noexcept
    {
        if (juce::isPositiveAndBelow (slot, ParamStore::maxChannels))
            boardActive[(size_t) slot].store (active, std::memory_order_relaxed);
    }

    // A generic strip's jack, as a 1-based device input channel; 0 takes the
    // jack away and the strip goes quiet.
    void setChannelSource (int slot, int deviceChannel) noexcept
    {
        if (slot >= 3 && slot < ParamStore::maxChannels)
        {
            sourceWanted[(size_t) slot].store (deviceChannel, std::memory_order_relaxed);
            updateInputSlots();
        }
    }

    // Which hardware MIDI device plays the keys; empty means all of them.
    void setMidiSource (const juce::String& identifier)
    {
        const juce::ScopedLock lock (midiSourceLock);
        midiSourceId = identifier;
    }

    EffectSlot& getTuneSlot() noexcept                    { return tuneSlot; }
    EffectSlot& getVocoderSlot() noexcept                 { return vocoderSlot; }

    // Every connected MIDI input feeds the rig; Program Change picks the mic
    // lane the way a foot controller would: 0 both off, 1 autotune, 2
    // vocoder. Called again periodically so a controller plugged in
    // mid-session joins.
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

    // Which device channels play which role, as 1-based channel numbers; 0
    // means unassigned. The callback hands over only the *active* channels,
    // compacted and in channel order — so on an aggregate whose mic device
    // sits first, the mic would land in slot 0 and the bass lane would sing.
    // The roles pin each lane to its channel wherever it compacts to; a role
    // that names an inactive channel falls back to the old rule, bass first,
    // mic second.
    void setInputRoles (int bassChannel, int micChannel) noexcept;
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
    void updateInputSlots() noexcept;
    void throughBoard (FxChain& chain, bool active, juce::SmoothedValue<float>& passGain, int slot,
                       const float* inL, const float* inR, float* outL, float* outR,
                       float* busIn, float* tmpL, float* tmpR, int n) noexcept;
    void handleIncomingMidiMessage (juce::MidiInput* source,
                                    const juce::MidiMessage& message) override;

    juce::AudioDeviceManager deviceManager;

    ParamStore& params;
    StemPlayer stems;
    FxChain fx;

    // The other strips' boards, over their own knob slots. Prepared
    // alongside the bass chain; each runs only while its strip has pedals.
    FxChain micChain { params, 1 };
    FxChain keysChain { params, 2 };
    std::unique_ptr<FxChain> spareChains[ParamStore::maxChannels - 3];   // slots 3 and up
    std::atomic<bool> boardActive[ParamStore::maxChannels] {};

    // The empty-board path still honours each strip's input and live-out
    // rows: one smoothed stereo gain per slot.
    juce::SmoothedValue<float> passGains[ParamStore::maxChannels];

    // The generic strips' jacks: the wanted 1-based device channels and the
    // compacted callback slots they resolve to (-1 stays silent).
    std::atomic<int> sourceWanted[ParamStore::maxChannels] {};
    std::atomic<int> sourceSlot[ParamStore::maxChannels] {};

    // The keys player's own hands, queued from the MIDI thread to the audio
    // thread in a small lock-free ring and merged with the tracker's notes.
    static constexpr int midiQueueSize = 256;
    NoteEvent midiQueue[midiQueueSize];
    std::atomic<int> midiWrite { 0 }, midiRead { 0 };
    juce::CriticalSection midiSourceLock;   // guards the id; never on the audio thread
    juce::String midiSourceId;

    // The mic lanes and their two hosted-plugin roles: the pitch corrector
    // and the vocoder. The vocoder slot asks for a sidechain bus — the
    // carrier arrives there.
    EffectSlot tuneSlot { "tune" };
    EffectSlot vocoderSlot { "vocoder", true };
    VocalChannel vocal { params, tuneSlot, vocoderSlot };

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
    juce::SmoothedValue<float> trackGain, masterGain;
    MasterLimiter limiter;

    juce::AudioBuffer<float> scratch;   // monoIn, liveL/R, mixL/R, vocalIn, laneL/R, busIn, tmpL/R
    int maxBlock = 512;

    std::atomic<double> currentRate { 0.0 };
    std::atomic<int> currentBlock { 0 };
    std::atomic<int> activeInputs { 0 };

    // The active input channels as a bit mask (bit n = channel n+1), the
    // wanted roles, and the slots they resolve to in the compacted callback
    // arrays. Slots are what the audio thread reads.
    std::atomic<juce::uint64> activeInputMask { 0 };
    std::atomic<int> bassWanted { 0 }, micWanted { 0 };
    std::atomic<int> bassSlot { 0 }, micSlot { 1 };
    std::atomic<float> inPeak { 0.0f }, vocalPeak { 0.0f }, outPeakL { 0.0f }, outPeakR { 0.0f };
    std::atomic<float> limiterGr { 0.0f };
    bool callbackAdded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace rig
