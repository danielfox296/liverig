#include "SongMaker.h"

namespace rig
{

static constexpr const char* modelName = "htdemucs";
static const char* const stemOrder[] { "drums", "bass", "other", "vocals" };

SongMaker::SongMaker() : juce::Thread ("LiveRig song maker") {}

SongMaker::~SongMaker()
{
    cancel();
    stopThread (4000);
}

juce::File SongMaker::songsDir()
{
    return juce::File ("/Users/fox296/LiveRig/Songs");
}

juce::File SongMaker::venvDir()
{
    return juce::File ("/Users/fox296/LiveRig/venv");
}

juce::File SongMaker::demucsExecutable()
{
    return venvDir().getChildFile ("bin").getChildFile ("demucs");
}

bool SongMaker::isSplitterInstalled()
{
    return demucsExecutable().existsAsFile();
}

juce::File SongMaker::findPython()
{
    for (auto* candidate : { "/opt/homebrew/bin/python3",
                             "/usr/local/bin/python3",
                             "/usr/bin/python3" })
    {
        const juce::File f (candidate);

        if (f.existsAsFile())
            return f;
    }

    return {};
}

juce::String SongMaker::getStatusLine() const
{
    const juce::ScopedLock sl (dataLock);
    return statusLine;
}

juce::String SongMaker::getMessage() const
{
    const juce::ScopedLock sl (dataLock);
    return message;
}

juce::String SongMaker::getSongName() const
{
    const juce::ScopedLock sl (dataLock);
    return songName;
}

juce::File SongMaker::getResultFolder() const
{
    const juce::ScopedLock sl (dataLock);
    return resultFolder;
}

void SongMaker::setStatus (const juce::String& text)
{
    const juce::ScopedLock sl (dataLock);
    statusLine = text;
}

void SongMaker::fail (const juce::String& text)
{
    {
        const juce::ScopedLock sl (dataLock);
        message = text;
    }

    state.store (State::failed);
}

void SongMaker::clearFinishedState()
{
    if (! isBusy())
    {
        state.store (State::idle);
        progress.store (0.0);
        setStatus ({});
    }
}

void SongMaker::installSplitter()
{
    if (isBusy())
        return;

    stopThread (2000);
    cancelled.store (false);
    installing = true;
    progress.store (-1.0);

    {
        const juce::ScopedLock sl (dataLock);
        message = {};
        statusLine = "Starting...";
    }

    state.store (State::installing);
    startThread (juce::Thread::Priority::background);
}

void SongMaker::makeSong (const juce::File& audio, const juce::String& name)
{
    if (isBusy())
        return;

    stopThread (2000);
    cancelled.store (false);
    installing = false;
    progress.store (0.0);

    {
        const juce::ScopedLock sl (dataLock);
        inputFile = audio;
        songName = name;
        resultFolder = juce::File();
        message = {};
        statusLine = "Starting...";
    }

    state.store (State::splitting);
    startThread (juce::Thread::Priority::background);
}

void SongMaker::cancel()
{
    cancelled.store (true);
    signalThreadShouldExit();

    std::shared_ptr<juce::ChildProcess> p;

    {
        const juce::ScopedLock sl (procLock);
        p = currentProcess;
    }

    if (p != nullptr)
        p->kill();
}

void SongMaker::run()
{
    if (installing)
        doInstall();
    else
        doSplit();
}

// Streams a child process, keeping the last useful line and any percentage it
// prints. Returns false if it failed, was cancelled, or could not start.
bool SongMaker::runCommand (const juce::StringArray& args, bool parsePercent)
{
    auto proc = std::make_shared<juce::ChildProcess>();

    {
        const juce::ScopedLock sl (procLock);
        currentProcess = proc;
    }

    const bool started = proc->start (args, juce::ChildProcess::wantStdOut
                                                | juce::ChildProcess::wantStdErr);

    if (! started)
    {
        const juce::ScopedLock sl (procLock);
        currentProcess = nullptr;
        fail ("Could not start " + args[0]);
        return false;
    }

    char buffer[4096];
    juce::String pending;

    while (! threadShouldExit())
    {
        const int got = proc->readProcessOutput (buffer, (int) sizeof (buffer));

        if (got <= 0)
        {
            if (! proc->isRunning())
                break;

            wait (30);
            continue;
        }

        const juce::String chunk (juce::CharPointer_UTF8 (buffer),
                                  juce::CharPointer_UTF8 (buffer + got));

        // Progress bars overwrite themselves with carriage returns; treat both
        // line endings the same and keep only the last fragment.
        pending = (pending + chunk).replaceCharacter ('\r', '\n');
        auto lines = juce::StringArray::fromLines (pending);
        pending = lines[lines.size() - 1];

        for (int i = 0; i < lines.size(); ++i)
        {
            const auto line = lines[i].trim();

            if (line.isEmpty())
                continue;

            if (parsePercent)
            {
                const int pc = line.indexOfChar ('%');

                if (pc > 0)
                {
                    int start = pc - 1;

                    while (start >= 0 && juce::CharacterFunctions::isDigit (line[start]))
                        --start;

                    const auto digits = line.substring (start + 1, pc);

                    if (digits.isNotEmpty())
                        progress.store (juce::jlimit (0.0, 1.0, digits.getIntValue() / 100.0));
                }
            }

            // The tail of a progress bar is noise; keep the last plain line.
            if (! line.containsChar ('|'))
                setStatus (line.substring (0, 140));
        }
    }

    const bool wasCancelled = cancelled.load();

    if (proc->isRunning())
        proc->kill();

    const auto exitCode = proc->getExitCode();

    {
        const juce::ScopedLock sl (procLock);
        currentProcess = nullptr;
    }

    if (wasCancelled)
    {
        fail ("Cancelled.");
        return false;
    }

    if (exitCode != 0)
    {
        fail (juce::File (args[0]).getFileName() + " failed (exit code "
                + juce::String ((int) exitCode) + ").\n\n" + getStatusLine());
        return false;
    }

    return true;
}

void SongMaker::doInstall()
{
    const auto python = findPython();

    if (python == juce::File())
    {
        fail ("No python3 found. Install Python 3, or Homebrew's python3, and try again.");
        return;
    }

    const auto venv = venvDir();
    const auto pip = venv.getChildFile ("bin").getChildFile ("pip");

    venv.getParentDirectory().createDirectory();

    if (! pip.existsAsFile())
    {
        setStatus ("Creating the Python environment...");

        if (! runCommand ({ python.getFullPathName(), "-m", "venv", venv.getFullPathName() }, false))
            return;
    }

    progress.store (-1.0);
    setStatus ("Downloading the splitter (about 900 MB)...");

    if (! runCommand ({ pip.getFullPathName(), "install", "--upgrade", "pip" }, false))
        return;

    // numpy is listed because demucs 4.1.0 imports it without declaring it.
    if (! runCommand ({ pip.getFullPathName(), "install", "demucs", "numpy" }, false))
        return;

    if (! isSplitterInstalled())
    {
        fail ("The install finished but demucs is still missing from " + venvDir().getFullPathName());
        return;
    }

    setStatus ("Ready.");
    progress.store (1.0);
    state.store (State::done);
}

void SongMaker::doSplit()
{
    juce::File audio, destination;
    juce::String name;

    {
        const juce::ScopedLock sl (dataLock);
        audio = inputFile;
        name = songName;
    }

    if (! audio.existsAsFile())
    {
        fail ("That file has gone missing: " + audio.getFullPathName());
        return;
    }

    if (! isSplitterInstalled())
    {
        fail ("The splitter is not installed yet.");
        return;
    }

    const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("LiveRig-split-" + juce::Uuid().toDashedString());
    temp.createDirectory();

    setStatus ("Separating " + audio.getFileName() + "...");

    const bool ok = runCommand ({ demucsExecutable().getFullPathName(),
                                  "--name", modelName,
                                  "--device", "cpu",
                                  "--int24",
                                  "--out", temp.getFullPathName(),
                                  "--filename", "{stem}.{ext}",
                                  audio.getFullPathName() }, true);

    if (! ok)
    {
        temp.deleteRecursively();
        return;
    }

    const auto separated = temp.getChildFile (modelName);
    destination = songsDir().getChildFile (name);

    if (! destination.createDirectory())
    {
        temp.deleteRecursively();
        fail ("Could not create " + destination.getFullPathName());
        return;
    }

    setStatus ("Filing the stems...");

    for (auto* stem : stemOrder)
    {
        const auto from = separated.getChildFile (juce::String (stem) + ".wav");
        const auto to = destination.getChildFile (juce::String (stem) + ".wav");

        if (! from.existsAsFile())
        {
            temp.deleteRecursively();
            fail (juce::String ("The splitter did not produce ") + stem + ".wav");
            return;
        }

        to.deleteFile();

        if (! from.moveFileTo (to))
        {
            temp.deleteRecursively();
            fail ("Could not write " + to.getFullPathName());
            return;
        }
    }

    temp.deleteRecursively();

    {
        const juce::ScopedLock sl (dataLock);
        resultFolder = destination;
    }

    progress.store (1.0);
    setStatus ("Done.");
    state.store (State::done);
}

} // namespace rig
