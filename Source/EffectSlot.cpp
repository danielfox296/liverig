#include "EffectSlot.h"

#include "InstrumentHost.h"

namespace rig
{

// Stereo main in, stereo out, and — when asked — a second input bus for the
// carrier. Everything past that (extra sidechains, aux buses) is disabled.
// A plugin free to keep a layout it insists on: whatever survives negotiation
// is read back and fed correctly rather than assumed.
static constexpr int maxScratchChannels = 8;

EffectSlot::EffectSlot (const juce::String& roleName, bool sidechain)
    : role (roleName), wantSidechain (sidechain)
{
    scratch.setSize (maxScratchChannels, 512);
    startTimer (250);
}

EffectSlot::~EffectSlot()
{
    stopTimer();
    live.store (nullptr, std::memory_order_relaxed);
    retired.reset();
    owned.reset();
}

void EffectSlot::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    maxBlock = juce::jmax (16, maxBlockSize);

    // A fixed worst-case scratch, so a later load never reallocates a buffer
    // the audio thread might be standing in.
    scratch.setSize (maxScratchChannels, maxBlock, false, true, true);

    if (owned != nullptr)
    {
        owned->prepareToPlay (sr, maxBlock);
        latencySamples.store (owned->getLatencySamples(), std::memory_order_relaxed);
    }
}

bool EffectSlot::load (InstrumentHost& registry, const juce::String& name, juce::String& error)
{
    auto instance = registry.createEffectInstance (name, sr, maxBlock, error);

    if (instance == nullptr)
    {
        lastError = name + ": " + (error.isEmpty() ? "failed to instantiate" : error);
        return false;
    }

    // --- shape the buses: stereo main in and out, carrier on bus 1 if asked ---
    const auto stereo = juce::AudioChannelSet::stereo();

    auto layout = instance->getBusesLayout();

    if (! layout.outputBuses.isEmpty())
        layout.outputBuses.getReference (0) = stereo;

    if (! layout.inputBuses.isEmpty())
        layout.inputBuses.getReference (0) = stereo;

    for (int bus = 1; bus < layout.inputBuses.size(); ++bus)
        layout.inputBuses.getReference (bus) = (wantSidechain && bus == 1)
                                                  ? stereo
                                                  : juce::AudioChannelSet::disabled();

    if (! instance->setBusesLayout (layout) && wantSidechain && layout.inputBuses.size() > 1)
    {
        // Stereo sidechain refused — a mono one is just as good a carrier.
        layout.inputBuses.getReference (1) = juce::AudioChannelSet::mono();
        instance->setBusesLayout (layout);   // best effort; actuals read back below
    }

    // --- read back what actually stuck ---
    const int inBuses = instance->getBusCount (true);
    const int mains = inBuses > 0 ? instance->getChannelCountOfBus (true, 0) : 0;
    const int sc = inBuses > 1 ? instance->getChannelCountOfBus (true, 1) : 0;
    const int ins = instance->getTotalNumInputChannels();
    const int outs = instance->getTotalNumOutputChannels();

    if (juce::jmax (ins, outs) > maxScratchChannels || outs < 1)
    {
        error = name + " wants " + juce::String (juce::jmax (ins, outs))
                     + " channels, which is not a shape this slot hosts";
        lastError = error;
        return false;
    }

    instance->prepareToPlay (sr, maxBlock);

    // --- swap in, retiring the old instance off the audio thread ---
    live.store (nullptr, std::memory_order_release);

    mainInChannels.store (mains, std::memory_order_relaxed);
    sidechainChannels.store (sc, std::memory_order_relaxed);
    totalIns.store (ins, std::memory_order_relaxed);
    totalOuts.store (outs, std::memory_order_relaxed);
    latencySamples.store (instance->getLatencySamples(), std::memory_order_relaxed);

    // No sidechain bus on a role that needs a carrier: fall back to the old
    // L=voice / R=carrier convention on the main pair. Degraded but audible,
    // and the UI says which mode is in force.
    carrierOnRight.store (wantSidechain && sc == 0 && mains >= 2, std::memory_order_relaxed);

    if (retired == nullptr)
        retired = std::move (owned);
    else
        owned.reset();

    owned = std::move (instance);
    retireCountdown.store (8, std::memory_order_relaxed);
    failed.store (false, std::memory_order_relaxed);
    live.store (owned.get(), std::memory_order_release);
    loadedName = name;
    lastError = {};
    return true;
}

void EffectSlot::unload()
{
    live.store (nullptr, std::memory_order_release);

    if (retired == nullptr)
        retired = std::move (owned);
    else
        owned.reset();

    retireCountdown.store (8, std::memory_order_relaxed);
    failed.store (false, std::memory_order_relaxed);
    loadedName = {};
}

void EffectSlot::restore (InstrumentHost& registry, const juce::String& name,
                          const juce::String& stateBase64)
{
    if (name.isEmpty())
        return;

    juce::String error;

    if (! load (registry, name, error) || owned == nullptr || stateBase64.isEmpty())
        return;

    juce::MemoryBlock block;

    if (block.fromBase64Encoding (stateBase64))
        owned->setStateInformation (block.getData(), (int) block.getSize());
}

juce::String EffectSlot::getStateBase64() const
{
    if (owned == nullptr)
        return {};

    juce::MemoryBlock block;
    owned->getStateInformation (block);
    return block.toBase64Encoding();
}

float EffectSlot::getLatencyMs() const noexcept
{
    return sr > 0.0 ? (float) (latencySamples.load (std::memory_order_relaxed) / sr * 1000.0)
                    : 0.0f;
}

void EffectSlot::timerCallback()
{
    // A plugin that threw in processBlock was unhooked on the audio thread;
    // its carcass is retired here, where deleting is allowed.
    if (failed.load (std::memory_order_relaxed) && owned != nullptr
        && live.load (std::memory_order_relaxed) == nullptr)
    {
        if (retired == nullptr)
            retired = std::move (owned);
        else
            owned.reset();

        retireCountdown.store (8, std::memory_order_relaxed);
    }

    if (retired != nullptr && retireCountdown.load (std::memory_order_relaxed) <= 0)
    {
        retired->releaseResources();
        retired.reset();
    }

    if (retireCountdown.load (std::memory_order_relaxed) > 0)
        retireCountdown.fetch_sub (1, std::memory_order_relaxed);
}

bool EffectSlot::renderBlock (juce::AudioProcessor* plugin, const float* in,
                              const float* sidechain, int numSamples) noexcept
{
    const int ins = totalIns.load (std::memory_order_relaxed);
    const int outs = totalOuts.load (std::memory_order_relaxed);
    const int channels = juce::jmax (1, ins, outs);

    if (channels > scratch.getNumChannels() || numSamples > scratch.getNumSamples())
        return false;

    scratch.clear();

    const int mains = mainInChannels.load (std::memory_order_relaxed);
    const int sc = sidechainChannels.load (std::memory_order_relaxed);
    const bool split = carrierOnRight.load (std::memory_order_relaxed);

    if (mains > 0)
        scratch.copyFrom (0, 0, in, numSamples);

    if (mains > 1)
    {
        const float* right = (split && sidechain != nullptr) ? sidechain : in;
        scratch.copyFrom (1, 0, right, numSamples);
    }

    if (sidechain != nullptr)
        for (int c = 0; c < sc && mains + c < channels; ++c)
            scratch.copyFrom (mains + c, 0, sidechain, numSamples);

    juce::AudioBuffer<float> block (scratch.getArrayOfWritePointers(), channels, 0, numSamples);
    midi.clear();

    try
    {
        plugin->processBlock (block, midi);
        return true;
    }
    catch (...)
    {
        // Eject on the spot; the timer retires the instance where deleting
        // is allowed. Audio carries on without the plugin.
        live.store (nullptr, std::memory_order_release);
        failed.store (true, std::memory_order_relaxed);
        return false;
    }
}

void EffectSlot::processMono (float* data, int numSamples) noexcept
{
    auto* plugin = live.load (std::memory_order_acquire);

    if (plugin == nullptr)
        return;   // empty slot: dry passes untouched

    if (! renderBlock (plugin, data, nullptr, numSamples))
        return;

    const int outs = totalOuts.load (std::memory_order_relaxed);

    if (outs == 1)
    {
        juce::FloatVectorOperations::copy (data, scratch.getReadPointer (0), numSamples);
    }
    else
    {
        const float* l = scratch.getReadPointer (0);
        const float* r = scratch.getReadPointer (1);

        for (int i = 0; i < numSamples; ++i)
            data[i] = 0.5f * (l[i] + r[i]);
    }
}

void EffectSlot::process (const float* in, const float* sidechain,
                          float* outL, float* outR, int numSamples) noexcept
{
    auto* plugin = live.load (std::memory_order_acquire);

    if (plugin == nullptr || ! renderBlock (plugin, in, sidechain, numSamples))
    {
        juce::FloatVectorOperations::clear (outL, numSamples);
        juce::FloatVectorOperations::clear (outR, numSamples);
        return;
    }

    const int outs = totalOuts.load (std::memory_order_relaxed);
    juce::FloatVectorOperations::copy (outL, scratch.getReadPointer (0), numSamples);
    juce::FloatVectorOperations::copy (outR, scratch.getReadPointer (outs > 1 ? 1 : 0), numSamples);
}

} // namespace rig
