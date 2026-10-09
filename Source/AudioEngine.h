#pragma once

#include <atomic>

#include <juce_audio_devices/juce_audio_devices.h>

#include "EffectSlot.h"
#include "FootControl.h"
#include "FxChain.h"
#include "InstrumentHost.h"
#include "Looper.h"
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
    FootEngine& getFoot() noexcept                        { return foot; }
    InstrumentHost& getInstrument() noexcept              { return instrument; }

    StemPlayer& getStemPlayer() noexcept                  { return stems; }
    Looper& getLooper() noexcept                          { return looper; }
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

    // Every connected MIDI input feeds the rig: what a foot controller sends
    // goes to the foot map, what a keyboard plays goes to the keys. Called
    // again periodically so a controller plugged in mid-session joins.
    void enableAllMidiInputs();

    double getSampleRate() const noexcept   { return currentRate.load (std::memory_order_relaxed); }
    int getBufferSize() const noexcept      { return currentBlock.load (std::memory_order_relaxed); }
    float getFxLatencyMs() const noexcept;

    // What the device itself costs: input and output buffering plus the
    // driver's safety offsets, as reported by CoreAudio.
    float getIoLatencyMs() noexcept;

    // How late the looper takes a played note to be before the board's own
    // delay is added: the interface's input and output latency, which
    // between them already hold the two buffers of the round trip.
    int getLoopRoundTripSamples() const noexcept { return ioRoundTrip.load (std::memory_order_relaxed); }

    float getInputPeak() const noexcept     { return inPeak.load (std::memory_order_relaxed); }
    float getVocalPeak() const noexcept     { return vocalPeak.load (std::memory_order_relaxed); }

    // A generic strip's own level, for its meter on the rail.
    float getSlotPeak (int slot) const noexcept
    {
        return juce::isPositiveAndBelow (slot, ParamStore::maxChannels)
                 ? slotPeaks[(size_t) slot].load (std::memory_order_relaxed) : 0.0f;
    }

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
    void renderBoard (FxChain& chain, bool active, juce::SmoothedValue<float>& passGain, int slot,
                      const float* inL, const float* inR, float* pathL, float* pathR,
                      float* busIn, int n) noexcept;
    void alignedAdd (int slot, int delaySamples, const float* inL, const float* inR,
                     float* outL, float* outR, int n,
                     float* tapL = nullptr, float* tapR = nullptr) noexcept;
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

    // Alignment at the sum: parallel jack-fed strips are delayed to the
    // slowest of them, so the same source through two boards cannot comb.
    // The lanes are never delayed — a plugin's cost is the player's choice,
    // and nobody delays the player's hands to flatter a phase meter.
    static constexpr int alignRingSize = 8192;
    juce::AudioBuffer<float> alignRings;   // two ring channels per slot
    int alignPos[ParamStore::maxChannels] {};

    // Per-slot output peaks, for the generic strips' meters.
    std::atomic<float> slotPeaks[ParamStore::maxChannels] {};

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

    // What is under the player's foot, and what each thing there does.
    FootEngine foot { params };

    // An AU instrument played by the same notes, summed into the live lane
    // so it counts as part of the performance rather than as backing.
    InstrumentHost instrument;
    float instrumentGain = 1.0f;

    // The instrument looper: one channel's sound in, after its board; the
    // loop and its click out into the live sum. The round trip is what the
    // interface costs a played note — in and out — so the loop can put each
    // note back where the player heard it. A key on the keyboard never
    // comes in through the interface: it is late by the way out, and by
    // the wait for the block that plays it, half a block on average.
    Looper looper;
    std::atomic<int> ioRoundTrip { 0 }, keysRoundTrip { 0 };

    // Playback-lane dynamics: the song is limited into the headroom the
    // instrument leaves, so a song never ducks the playing.
    StemDucker ducker;

    // The master bus: one gain over the whole sum, then the safety limiter,
    // so the device is never handed a sample it would hard-clip.
    juce::SmoothedValue<float> trackGain, masterGain;
    MasterLimiter limiter;

    juce::AudioBuffer<float> scratch;   // monoIn, liveL/R, mixL/R, vocalIn, laneL/R, busIn, tmpL/R, tapL/R
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
