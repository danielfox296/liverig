#pragma once

#include <atomic>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "Parameters.h"

namespace rig
{

// Four-track in-memory stem player. Demucs 4-stem naming, missing stems are
// skipped. Files are decoded and resampled to the device rate on load, so the
// audio thread only ever reads floats out of RAM.
class StemPlayer
{
public:
    static constexpr int numStems = 4;
    static const char* const stemNames[numStems];

    explicit StemPlayer (ParamStore& p);

    void prepare (double sampleRate, int maxBlockSize);

    // --- message thread ---
    juce::Result loadSongFolder (const juce::File& folder);
    void unload();

    bool reloadNeeded() const noexcept   { return loaded && ! juce::exactlyEqual (preparedRate.load(), builtRate); }
    void reloadForCurrentRate();

    juce::File getFolder() const         { return folder; }
    juce::String getSongName() const     { return songName; }
    bool isStemPresent (int i) const     { return i >= 0 && i < numStems && present[i]; }
    double getLengthSeconds() const      { return builtRate > 0.0 ? lengthSamples / builtRate : 0.0; }

    void play() noexcept                 { command.store (1, std::memory_order_release); }
    void stop() noexcept                 { command.store (2, std::memory_order_release); }
    void togglePlay() noexcept           { isPlaying() ? stop() : play(); }
    bool isPlaying() const noexcept      { return playing.load (std::memory_order_relaxed); }

    // --- audio thread ---
    void process (float* left, float* right, int numSamples) noexcept;

private:
    struct SongData : public juce::ReferenceCountedObject
    {
        using Ptr = juce::ReferenceCountedObjectPtr<SongData>;

        juce::AudioBuffer<float> stems[numStems];
        bool present[numStems] {};
        int numSamples = 0;
    };

    struct SourceStem
    {
        juce::AudioBuffer<float> audio;   // as decoded, at the file's own rate
        double rate = 0.0;
        bool present = false;
    };

    juce::Result decodeFolder (const juce::File& f);
    void buildForRate (double rate);

    ParamStore& params;

    juce::AudioFormatManager formats;
    SourceStem source[numStems];

    juce::SpinLock songLock;
    SongData::Ptr song;

    juce::File folder;
    juce::String songName;
    bool present[numStems] {};
    bool loaded = false;
    double builtRate = 0.0;
    int lengthSamples = 0;

    std::atomic<double> preparedRate { 0.0 };

    std::atomic<int> command { 0 };       // 1 = play, 2 = stop
    std::atomic<bool> playing { false };
    std::atomic<int> playhead { 0 };

    juce::SmoothedValue<float> gains[numStems];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemPlayer)
};

} // namespace rig
