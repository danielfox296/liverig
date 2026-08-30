#include "InstrumentHost.h"

namespace rig
{

static juce::File cacheFile()
{
    // "plugins" rather than the old "instruments": the scan now records
    // effects too, and an old cache without them must trigger a fresh scan.
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
              .getChildFile ("Application Support")
              .getChildFile ("LiveRig")
              .getChildFile ("plugins.xml");
}

InstrumentHost::InstrumentHost()
{
    formats.addDefaultFormats();
    scratch.setSize (2, 512);
    midi.ensureSize (512);

    // A cached list means the pickers are populated the moment they open; a
    // live scan instantiates plugins and some of them are slow or broken.
    if (auto xml = juce::XmlDocument::parse (cacheFile()))
    {
        const juce::ScopedLock sl (listLock);

        for (auto* group : xml->getChildIterator())
        {
            auto& into = group->hasTagName ("effects") ? effects : instruments;

            for (auto* child : group->getChildIterator())
            {
                juce::PluginDescription description;

                if (description.loadFromXml (*child))
                    into.add (description);
            }
        }
    }

    startTimer (250);
}

InstrumentHost::~InstrumentHost()
{
    stopTimer();

    abortScan.store (true, std::memory_order_relaxed);

    if (scanThread != nullptr)
    {
        scanThread->stopThread (4000);
        scanThread.reset();
    }

    live.store (nullptr, std::memory_order_relaxed);
    retired.reset();
    owned.reset();
}

void InstrumentHost::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    maxBlock = juce::jmax (16, maxBlockSize);
    scratch.setSize (juce::jmax (2, pluginOutputs.load()), maxBlock, false, true, true);
    midi.ensureSize (1024);

    if (owned != nullptr)
    {
        owned->setPlayConfigDetails (0, 2, sr, maxBlock);
        owned->prepareToPlay (sr, maxBlock);
    }
}

void InstrumentHost::beginScan()
{
    if (scanning.load() || scanThread != nullptr)
        return;

    scanning.store (true, std::memory_order_relaxed);

    struct ScanJob : juce::Thread
    {
        explicit ScanJob (InstrumentHost& h) : juce::Thread ("LiveRig plugin scan"), host (h) {}
        void run() override { host.scanOnThisThread(); }
        InstrumentHost& host;
    };

    scanThread = std::make_unique<ScanJob> (*this);
    scanThread->startThread (juce::Thread::Priority::background);
}

void InstrumentHost::runScanWorker (const juce::String& formatName,
                                    const juce::String& identifier)
{
    juce::AudioPluginFormatManager workerFormats;
    workerFormats.addDefaultFormats();

    for (int i = 0; i < workerFormats.getNumFormats(); ++i)
    {
        auto* format = workerFormats.getFormat (i);

        if (format == nullptr || format->getName() != formatName)
            continue;

        juce::OwnedArray<juce::PluginDescription> types;
        format->findAllTypesForFile (types, identifier);

        for (auto* type : types)
            if (auto xml = type->createXml())
                std::printf ("<rig-found>\n%s\n</rig-found>\n", xml->toString().toRawUTF8());
    }

    std::printf ("<rig-scan-done/>\n");
    std::fflush (stdout);

    // Straight out, no destructors: a plugin may have posted a deferred
    // lambda against an instance the scan already destroyed, and giving the
    // message loop one more turn is how this process earns its own crash
    // report. The results are on stdout; nothing else here matters.
    std::_Exit (0);
}

// One disposable child per plugin. A plugin that crashes being described
// takes its child down and is skipped; one that hangs is killed at the
// deadline. Either way the rig's own process never runs a stranger's code
// during a scan, and the show goes on.
static bool describeInChild (const juce::String& formatName, const juce::String& identifier,
                             juce::Array<juce::PluginDescription>& into,
                             std::atomic<bool>& abortFlag)
{
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

    juce::ChildProcess child;
    juce::StringArray args { exe.getFullPathName(), "--scan-worker", formatName, identifier };

    if (! child.start (args, juce::ChildProcess::wantStdOut))
        return false;

    juce::String output;
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 20000.0;

    while (juce::Time::getMillisecondCounterHiRes() < deadline
            && ! abortFlag.load (std::memory_order_relaxed))
    {
        char buffer[4096];
        const int got = child.readProcessOutput (buffer, sizeof (buffer));

        if (got > 0)
        {
            output += juce::String::fromUTF8 (buffer, got);
            continue;
        }

        if (! child.isRunning())
            break;

        juce::Thread::sleep (10);
    }

    child.kill();   // a no-op when it already exited; the end of it when it hung

    bool complete = output.contains ("<rig-scan-done/>");
    int searchFrom = 0;

    for (;;)
    {
        const int start = output.indexOf (searchFrom, "<rig-found>");

        if (start < 0)
            break;

        const int end = output.indexOf (start, "</rig-found>");

        if (end < 0)
            break;

        if (auto xml = juce::parseXML (output.substring (start + 11, end)))
        {
            juce::PluginDescription description;

            if (description.loadFromXml (*xml))
                into.add (description);
        }

        searchFrom = end + 12;
    }

    return complete;
}

void InstrumentHost::scanOnThisThread()
{
    juce::Array<juce::PluginDescription> found;

    for (int i = 0; i < formats.getNumFormats(); ++i)
    {
        auto* format = formats.getFormat (i);

        if (format == nullptr)
            continue;

        const auto identifiers = format->searchPathsForPlugins (
                                     format->getDefaultLocationsToSearch(), true, false);

        // Audio Units carry their type in the identifier, and describing a
        // plugin costs a child process and real time — this machine has 341
        // components. Filter to the types the rig can use — instruments,
        // effects, music effects — and the converters, panners and codecs
        // stay untouched.
        const bool isAudioUnit = format->getName().containsIgnoreCase ("AudioUnit");

        for (const auto& identifier : identifiers)
        {
            if (isAudioUnit && ! (identifier.contains ("aumu")
                                   || identifier.contains ("aufx")
                                   || identifier.contains ("aumf")))
                continue;

            if (abortScan.load (std::memory_order_relaxed))
            {
                scanning.store (false, std::memory_order_relaxed);
                return;
            }

            describeInChild (format->getName(), identifier, found, abortScan);
        }
    }

    juce::Array<juce::PluginDescription> foundInstruments, foundEffects;

    for (const auto& type : found)
        (type.isInstrument ? foundInstruments : foundEffects).add (type);

    struct ByName
    {
        static int compareElements (const juce::PluginDescription& a, const juce::PluginDescription& b)
        {
            return a.name.compareIgnoreCase (b.name);
        }
    };

    ByName sorter;
    foundInstruments.sort (sorter);
    foundEffects.sort (sorter);

    {
        const juce::ScopedLock sl (listLock);
        instruments = foundInstruments;
        effects = foundEffects;
    }

    juce::XmlElement root ("plugins");
    auto* instrumentGroup = root.createNewChildElement ("instruments");
    auto* effectGroup = root.createNewChildElement ("effects");

    for (const auto& description : foundInstruments)
        instrumentGroup->addChildElement (description.createXml().release());

    for (const auto& description : foundEffects)
        effectGroup->addChildElement (description.createXml().release());

    cacheFile().getParentDirectory().createDirectory();
    root.writeTo (cacheFile());

    scanning.store (false, std::memory_order_relaxed);
}

// A plugin installed as both AU and VST3 is one plugin, not two: names are
// deduplicated and lookups prefer the AU, the native citizen here.
static juce::StringArray dedupedNames (const juce::Array<juce::PluginDescription>& list)
{
    juce::StringArray names;

    for (const auto& description : list)
        names.addIfNotAlreadyThere (description.name);

    return names;
}

static bool pickByName (const juce::Array<juce::PluginDescription>& list,
                        const juce::String& name, juce::PluginDescription& out)
{
    bool matched = false;

    for (const auto& description : list)
    {
        if (description.name != name)
            continue;

        if (! matched || description.pluginFormatName.containsIgnoreCase ("AudioUnit"))
        {
            out = description;
            matched = true;
        }
    }

    return matched;
}

juce::StringArray InstrumentHost::getAvailableNames() const
{
    const juce::ScopedLock sl (listLock);
    return dedupedNames (instruments);
}

juce::StringArray InstrumentHost::getEffectNames() const
{
    const juce::ScopedLock sl (listLock);
    return dedupedNames (effects);
}

static juce::String firstMatch (const juce::StringArray& names,
                                std::initializer_list<const char*> wanted)
{
    for (auto* candidate : wanted)
        for (const auto& name : names)
            if (name.containsIgnoreCase (candidate))
                return name;

    return {};
}

juce::String InstrumentHost::preferredPitchPlugin() const
{
    // Real pitch correctors only, preference first. Graillon is the intended
    // tenant; MAutoPitch is the stopgap already on this machine; the rest
    // are the usual suspects should one of them be what is installed.
    return firstMatch (getEffectNames(),
                       { "Graillon", "MAutoPitch", "Auto-Tune",
                         "Auto-Key", "MetaTune", "KeroVee", "Voloco" });
}

juce::String InstrumentHost::preferredVocoderPlugin() const
{
    return firstMatch (getEffectNames(),
                       { "TAL-Vocoder", "TAL Vocoder", "MVocoder", "OrangeVocoder", "Vocoder" });
}

std::unique_ptr<juce::AudioPluginInstance> InstrumentHost::createEffectInstance (
        const juce::String& name, double sampleRate, int blockSize, juce::String& error)
{
    juce::PluginDescription wanted;

    {
        const juce::ScopedLock sl (listLock);

        if (! pickByName (effects, name, wanted))
        {
            error = "no effect called \"" + name + "\"";
            return nullptr;
        }
    }

    return formats.createPluginInstance (wanted, sampleRate, blockSize, error);
}

bool InstrumentHost::load (const juce::String& name, juce::String& error)
{
    juce::PluginDescription wanted;
    bool matched = false;

    {
        const juce::ScopedLock sl (listLock);
        matched = pickByName (instruments, name, wanted);
    }

    if (! matched)
    {
        error = "no instrument called \"" + name + "\"";
        return false;
    }

    auto instance = formats.createPluginInstance (wanted, sr, maxBlock, error);

    if (instance == nullptr)
        return false;

    instance->setPlayConfigDetails (0, 2, sr, maxBlock);
    instance->prepareToPlay (sr, maxBlock);

    const int outs = juce::jmax (2, instance->getTotalNumOutputChannels());
    pluginOutputs.store (outs, std::memory_order_relaxed);
    scratch.setSize (outs, juce::jmax (maxBlock, scratch.getNumSamples()), false, true, true);

    live.store (nullptr, std::memory_order_release);

    if (retired == nullptr)
        retired = std::move (owned);
    else
        owned.reset();

    owned = std::move (instance);
    retireCountdown.store (8, std::memory_order_relaxed);
    live.store (owned.get(), std::memory_order_release);
    loadedName = name;
    return true;
}

void InstrumentHost::unload()
{
    live.store (nullptr, std::memory_order_release);

    if (retired == nullptr)
        retired = std::move (owned);
    else
        owned.reset();

    retireCountdown.store (8, std::memory_order_relaxed);
    loadedName = {};
}

void InstrumentHost::timerCallback()
{
    if (retired != nullptr && retireCountdown.load (std::memory_order_relaxed) <= 0)
    {
        retired->releaseResources();
        retired.reset();
    }

    if (retireCountdown.load (std::memory_order_relaxed) > 0)
        retireCountdown.fetch_sub (1, std::memory_order_relaxed);

    if (scanThread != nullptr && ! scanning.load (std::memory_order_relaxed)
        && ! scanThread->isThreadRunning())
        scanThread.reset();
}

juce::String InstrumentHost::getStateBase64() const
{
    if (owned == nullptr)
        return {};

    juce::MemoryBlock block;
    owned->getStateInformation (block);
    return block.toBase64Encoding();
}

void InstrumentHost::restore (const juce::String& name, const juce::String& stateBase64)
{
    if (name.isEmpty())
        return;

    juce::String error;

    if (! load (name, error) || owned == nullptr || stateBase64.isEmpty())
        return;

    juce::MemoryBlock block;

    if (block.fromBase64Encoding (stateBase64))
        withPluginParked ([&block] (juce::AudioPluginInstance& p)
                          { p.setStateInformation (block.getData(), (int) block.getSize()); });
}

void InstrumentHost::withPluginParked (const std::function<void (juce::AudioPluginInstance&)>& work)
{
    auto* plugin = owned.get();

    if (plugin == nullptr)
        return;

    auto* wasLive = live.load (std::memory_order_seq_cst);
    live.store (nullptr, std::memory_order_seq_cst);

    // A callback already inside processBlock finishes with the instrument it
    // still holds; one that has not started yet reads the null and passes.
    // The wait is bounded — a stalled device must not freeze the window.
    for (int i = 0; i < 400 && inProcess.load (std::memory_order_seq_cst); ++i)
        juce::Thread::sleep (1);

    work (*plugin);

    if (wasLive != nullptr)
        live.store (plugin, std::memory_order_seq_cst);
}

void InstrumentHost::process (const NoteEvent* events, int numEvents,
                              float* left, float* right, int numSamples, float gain) noexcept
{
    // Claim the plugin before reading the pointer, so a message thread that
    // parks it either sees this callback and waits, or is seen by it and
    // hands over a null. Sequential consistency is what makes those two
    // orderings agree; one fence a block is nothing.
    inProcess.store (true, std::memory_order_seq_cst);

    struct Release
    {
        std::atomic<bool>& flag;
        ~Release() { flag.store (false, std::memory_order_seq_cst); }
    } release { inProcess };

    auto* plugin = live.load (std::memory_order_seq_cst);

    if (plugin == nullptr || numSamples > scratch.getNumSamples())
        return;

    midi.clear();

    for (int i = 0; i < numEvents; ++i)
    {
        const auto& e = events[i];

        switch (e.type)
        {
            case NoteEvent::noteOn:
                midi.addEvent (juce::MidiMessage::noteOn (1, e.a, (juce::uint8) e.b), 0);
                break;

            case NoteEvent::noteOff:
                midi.addEvent (juce::MidiMessage::noteOff (1, e.a), 0);
                break;

            case NoteEvent::bend:
                midi.addEvent (juce::MidiMessage::pitchWheel (1, e.a), 0);
                break;

            case NoteEvent::expression:
                midi.addEvent (juce::MidiMessage::controllerEvent (1, 11, e.a), 0);
                break;
        }
    }

    const int channels = juce::jmin (scratch.getNumChannels(),
                                     pluginOutputs.load (std::memory_order_relaxed));
    scratch.clear();

    juce::AudioBuffer<float> block (scratch.getArrayOfWritePointers(), channels, 0, numSamples);
    plugin->processBlock (block, midi);

    // Only the main stereo pair is wanted; anything past it is the plugin's
    // own auxiliary outputs.
    juce::FloatVectorOperations::addWithMultiply (left, block.getReadPointer (0), gain, numSamples);
    juce::FloatVectorOperations::addWithMultiply (right,
                                                  block.getReadPointer (channels > 1 ? 1 : 0),
                                                  gain, numSamples);
}

} // namespace rig
