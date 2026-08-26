#pragma once

#include <atomic>
#include <vector>

#include <juce_core/juce_core.h>

namespace nam { class DSP; }

namespace rig
{

// A Neural Amp Modeler capture as a chain stage. Loading and parsing happen
// on the message thread; the finished model is handed to the audio thread
// through one atomic pointer, and the replaced model rides a small ring back
// out to be deleted where deleting is allowed. The audio thread never
// allocates, frees, or waits.
//
// Captures carry a measured loudness; playback is normalised towards a fixed
// nominal so swapping captures compares tone, not volume. With no model
// loaded the stage is a wire.
class AmpModel
{
public:
    AmpModel();
    ~AmpModel();

    // Device thread with the callback stopped: adopt any half-delivered model
    // and re-prime everything at the (possibly new) rate and block size.
    void prepare (double sampleRate, int maxBlockSize);

    // Message thread.
    bool load (const juce::File& file, juce::String& error);
    void clearModel();
    void collectGarbage() noexcept;
    juce::String getLoadedName() const { return loadedName; }
    juce::String getLoadedPath() const { return loadedPath; }
    bool isLoaded() const noexcept     { return modelLive.load (std::memory_order_relaxed); }

    // The capture library: imported .nam files are copied here so they
    // outlive wherever they were downloaded to, and presets can find them by
    // name forever after. dirOverride lets tests use a scratch folder.
    static juce::File libraryDir();
    static juce::File importToLibrary (const juce::File& source, juce::String& error,
                                       const juce::File& dirOverride = {});
    static juce::Array<juce::File> listLibrary (const juce::File& dirOverride = {});
    static juce::File libraryFileForName (const juce::String& name,
                                          const juce::File& dirOverride = {});

    // Audio thread. In place; unity when no model is loaded.
    void process (float* data, int numSamples) noexcept;

private:
    std::atomic<double> sr { 48000.0 };
    std::atomic<int> maxBlock { 512 };

    nam::DSP* active = nullptr;                    // audio-thread owned
    std::atomic<nam::DSP*> pending { nullptr };    // loader -> audio
    std::atomic<bool> clearRequest { false };
    std::atomic<nam::DSP*> retired[4] {};          // audio -> collector
    std::atomic<float> normGain { 1.0f };

    std::vector<float> scratch;

    juce::String loadedName, loadedPath;           // message-thread bookkeeping
    std::atomic<bool> modelLive { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmpModel)
};

} // namespace rig
