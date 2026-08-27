#pragma once

#include <atomic>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

#include "dsp/NoteSegmenter.h"

namespace rig
{

// Plays an Audio Unit instrument from the notes the tracker produces, so a pad
// lives inside the rig instead of in a second application with a second buffer.
//
// Logic's own instruments cannot appear here — Apple keeps Alchemy, Sculpture
// and the rest inside Logic and MainStage, registered for no other host. What
// is reachable is everything installed as a normal AU: SKY Keys, Vital, LABS,
// Kontakt.
//
// Loading happens on the message thread and the finished instrument reaches the
// audio thread through one atomic pointer, the way the NAM captures do. The
// replaced instrument is retired to a slot the message thread empties, so
// nothing is deleted where deleting is not allowed.
class InstrumentHost : private juce::Timer
{
public:
    InstrumentHost();
    ~InstrumentHost() override;

    // --- device thread, callback stopped ---
    void prepare (double sampleRate, int maxBlockSize);

    // --- message thread ---
    void beginScan();                       // background; cached after the first time
    bool isScanning() const noexcept        { return scanning.load (std::memory_order_relaxed); }
    juce::StringArray getAvailableNames() const;

    bool load (const juce::String& name, juce::String& error);
    void unload();

    juce::String getLoadedName() const      { return loadedName; }
    bool isLoaded() const noexcept          { return live.load (std::memory_order_relaxed) != nullptr; }

    juce::AudioProcessor* getLiveInstrument() const noexcept
    {
        return live.load (std::memory_order_relaxed);
    }

    juce::String getStateBase64() const;
    void restore (const juce::String& name, const juce::String& stateBase64);

    // --- audio thread: renders the notes and adds the result in ---
    void process (const NoteEvent* events, int numEvents,
                  float* left, float* right, int numSamples, float gain) noexcept;

private:
    void timerCallback() override;
    void scanOnThisThread();

    juce::AudioPluginFormatManager formats;
    juce::Array<juce::PluginDescription> instruments;
    mutable juce::CriticalSection listLock;

    std::unique_ptr<juce::AudioPluginInstance> owned;      // message thread owns
    std::atomic<juce::AudioProcessor*> live { nullptr };   // audio thread reads
    std::unique_ptr<juce::AudioPluginInstance> retired;    // waiting to be deleted
    std::atomic<int> retireCountdown { 0 };

    // Sized to what the plugin actually asks for. Apple's own DLSMusicDevice
    // reports four outputs, and handing a plugin fewer channels than it
    // expects is how a host reads off the end of a buffer.
    juce::AudioBuffer<float> scratch;
    std::atomic<int> pluginOutputs { 2 };
    juce::MidiBuffer midi;

    std::atomic<bool> scanning { false };
    juce::String loadedName;
    double sr = 48000.0;
    int maxBlock = 512;

    std::unique_ptr<juce::Thread> scanThread;
    std::atomic<bool> abortScan { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstrumentHost)
};

} // namespace rig
