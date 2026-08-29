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

void InstrumentHost::scanOnThisThread()
{
    juce::Array<juce::PluginDescription> foundInstruments, foundEffects;

    for (int i = 0; i < formats.getNumFormats(); ++i)
    {
        auto* format = formats.getFormat (i);

        if (format == nullptr)
            continue;

        const auto identifiers = format->searchPathsForPlugins (
                                     format->getDefaultLocationsToSearch(), true, false);

        // Audio Units carry their type in the identifier, and instantiating a
        // plugin to ask what it is costs real time — this machine has 341
        // components and one of them refuses to open at all. Filter to the
        // types the rig can use — instruments, effects, music effects — and
        // the converters, panners and codecs stay untouched.
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

            juce::OwnedArray<juce::PluginDescription> types;
            format->findAllTypesForFile (types, identifier);

            for (auto* type : types)
                (type->isInstrument ? foundInstruments : foundEffects).add (*type);
        }
    }

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

juce::String InstrumentHost::preferredPitchPlugin() const
{
    // Real pitch correctors only, preference first. Graillon is the intended
    // tenant; MAutoPitch is the stopgap already on this machine; the rest
    // are the usual suspects should one of them be what is installed.
    static const char* const wanted[] = { "Graillon", "MAutoPitch", "Auto-Tune",
                                          "Auto-Key", "MetaTune", "KeroVee", "Voloco" };

    const auto names = getEffectNames();

    for (auto* candidate : wanted)
        for (const auto& name : names)
            if (name.containsIgnoreCase (candidate))
                return name;

    return {};
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
        owned->setStateInformation (block.getData(), (int) block.getSize());
}

void InstrumentHost::process (const NoteEvent* events, int numEvents,
                              float* left, float* right, int numSamples, float gain) noexcept
{
    auto* plugin = live.load (std::memory_order_acquire);

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
