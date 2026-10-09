#pragma once

#include <atomic>
#include <functional>
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

    // The scan-worker entry point: describe one plugin and hard-exit. Runs
    // in a disposable child process the scan spawns per identifier, because
    // instantiating a stranger's plugin in the rig's own process is how a
    // stranger's bug becomes the rig's crash — one JUCE-built sampler posts
    // a deferred load lambda that fires after the scanner has destroyed the
    // instance. Out of process, that is the child's funeral, not the show's.
    // Prints PluginDescription XML between markers on stdout; never returns
    // normally — it _Exits before any pending lambda can dereference a corpse.
    [[noreturn]] static void runScanWorker (const juce::String& formatName,
                                            const juce::String& identifier);

    // --- message thread ---
    void beginScan();                       // background; cached after the first time
    bool isScanning() const noexcept        { return scanning.load (std::memory_order_relaxed); }
    juce::StringArray getAvailableNames() const;

    // The same scan records effect plugins, so the host doubles as the
    // machine's plugin registry: the vocal channel picks
    // from this list. Where a plugin ships as both AU and VST3 the AU wins —
    // one name, one entry, and AU is the native citizen on this platform.
    juce::StringArray getEffectNames() const;
    std::unique_ptr<juce::AudioPluginInstance> createEffectInstance (const juce::String& name,
                                                                     double sampleRate, int blockSize,
                                                                     juce::String& error);

    // The pitch corrector this machine should use, best first: a curated
    // list of actual autotune plugins matched against what is installed —
    // never a picker full of every effect on the system. Empty when none is
    // installed, and the row's status line says what to install.
    juce::String preferredPitchPlugin() const;

    // Same idea for the vocoder role: real vocoders only, TAL-Vocoder 3
    // first because it is the intended tenant.
    juce::String preferredVocoderPlugin() const;

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

    // How long the instrument itself holds a note back, as it last said
    // while playing.
    int getLatencySamples() const noexcept   { return latencySamples.load (std::memory_order_relaxed); }

private:
    void timerCallback() override;
    void scanOnThisThread();

    // Changing a plugin's state under a running callback is a data race, and
    // a voice change happens mid-set. The live pointer is taken away first,
    // then a callback already inside processBlock is waited out, so the
    // state moves while nobody is playing through it.
    void withPluginParked (const std::function<void (juce::AudioPluginInstance&)>& work);

    juce::AudioPluginFormatManager formats;
    juce::Array<juce::PluginDescription> instruments, effects;
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
    std::atomic<int> latencySamples { 0 };
    juce::MidiBuffer midi;

    std::atomic<bool> scanning { false };
    std::atomic<bool> inProcess { false };   // a callback is inside processBlock
    juce::String loadedName;
    double sr = 48000.0;
    int maxBlock = 512;

    std::unique_ptr<juce::Thread> scanThread;
    std::atomic<bool> abortScan { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstrumentHost)
};

} // namespace rig
