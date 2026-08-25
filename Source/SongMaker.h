#pragma once

#include <atomic>
#include <memory>

#include <juce_core/juce_core.h>

namespace rig
{

// Runs the stem-splitting pipeline as a background job so the app never needs
// a terminal: installs Demucs into a private virtualenv if it is missing, then
// separates a dropped-in track into a LiveRig song folder.
//
// Everything happens on a worker thread; the UI polls the getters.
class SongMaker : private juce::Thread
{
public:
    enum class State { idle, installing, splitting, done, failed };

    SongMaker();
    ~SongMaker() override;

    static juce::File songsDir();
    static juce::File venvDir();
    static juce::File demucsExecutable();
    static bool isSplitterInstalled();
    static juce::File findPython();

    void installSplitter();
    void makeSong (const juce::File& inputAudio, const juce::String& name);
    void cancel();

    bool isBusy() const noexcept
    {
        const auto s = state.load();
        return s == State::installing || s == State::splitting;
    }

    State getState() const noexcept    { return state.load(); }
    double getProgress() const noexcept { return progress.load(); }   // < 0 means indeterminate

    juce::String getStatusLine() const;
    juce::String getMessage() const;
    juce::String getSongName() const;
    juce::File getResultFolder() const;

    void clearFinishedState();

private:
    void run() override;
    bool runCommand (const juce::StringArray& args, bool parsePercent);
    void doInstall();
    void doSplit();
    void setStatus (const juce::String& text);
    void fail (const juce::String& text);

    std::atomic<State> state { State::idle };
    std::atomic<double> progress { 0.0 };
    std::atomic<bool> cancelled { false };

    juce::CriticalSection dataLock;
    juce::String statusLine, message, songName;
    juce::File inputFile, resultFolder;

    juce::CriticalSection procLock;
    std::shared_ptr<juce::ChildProcess> currentProcess;

    bool installing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongMaker)
};

} // namespace rig
