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

    // A vocoder bound for the bass-carrier role must listen to the bass and
    // to nothing else. TAL ships idling on an internal synth waiting for
    // MIDI nobody sends, and its routing takes two switches whose meaning
    // was measured, not read: "inputmode" takes the carrier from the MAIN
    // input in place of the synth, and "sidechain" moves the MODULATOR — the
    // voice — onto the sidechain bus. (TAL is built to sit on a synth track
    // with the voice fed in from the side; the first reading of those names
    // had the buses exactly backwards, and the bass ended up as the words.)
    // The internal oscillators are zeroed so the bass is the only voice box.
    // The parameter names are TAL's own, so other vocoders are untouched,
    // and renderBlock lays the buffers to match whichever mode is in force.
    bool hasInputMode = false, hasSidechainSwitch = false;

    if (wantSidechain)
    {
        for (auto* parameter : instance->getParameters())
        {
            const auto pname = parameter->getName (32);
            hasInputMode = hasInputMode || pname.equalsIgnoreCase ("inputmode");
            hasSidechainSwitch = hasSidechainSwitch || pname.equalsIgnoreCase ("sidechain");
        }

        applyCarrierSwitches (*instance, sc);
    }

    const bool talMode = hasInputMode && (sc == 0 || hasSidechainSwitch);

    // --- swap in, retiring the old instance off the audio thread ---
    live.store (nullptr, std::memory_order_release);

    mainInChannels.store (mains, std::memory_order_relaxed);
    sidechainChannels.store (sc, std::memory_order_relaxed);
    totalIns.store (ins, std::memory_order_relaxed);
    totalOuts.store (outs, std::memory_order_relaxed);
    latencySamples.store (instance->getLatencySamples(), std::memory_order_relaxed);

    // Which way the buffers get laid, per the measured switch semantics: a
    // TAL with a sidechain bus takes the voice there and the carrier on the
    // mains; a TAL without one wants carrier LEFT, voice right. A non-TAL
    // sidechain vocoder keeps the generic shape (voice main, carrier SC),
    // and the no-bus fallback keeps the old L=voice / R=carrier split.
    micOnSidechain.store (talMode && sc > 0, std::memory_order_relaxed);
    carrierOnLeft.store (talMode && sc == 0 && mains >= 2, std::memory_order_relaxed);
    carrierOnRight.store (! talMode && wantSidechain && sc == 0 && mains >= 2, std::memory_order_relaxed);

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

    // The saved state wins on everything musical — and would also quietly
    // flip the carrier switches back to whatever era it was written in, so
    // the rig re-asserts them on top. A state saved before the switch
    // policy existed put the internal synth back in charge on every launch.
    applyCarrierSwitches (*owned, sidechainChannels.load (std::memory_order_relaxed));
}

// The rig owns the routing, wherever the parameter values came from. As
// measured against TAL-Vocoder-2: "inputmode" takes the carrier from the
// main input in place of the internal synth, "sidechain" moves the
// modulator onto the sidechain bus (engaged only when such a bus was
// actually negotiated), and the oscillator volumes are zeroed so the bass
// is the only voice box. Plugins without these parameter names are
// untouched.
void EffectSlot::applyCarrierSwitches (juce::AudioProcessor& plugin, int scChannels) const
{
    if (! wantSidechain)
        return;

    for (auto* parameter : plugin.getParameters())
    {
        const auto pname = parameter->getName (32);

        if (pname.equalsIgnoreCase ("inputmode"))
            parameter->setValueNotifyingHost (1.0f);
        else if (pname.equalsIgnoreCase ("sidechain"))
            parameter->setValueNotifyingHost (scChannels > 0 ? 1.0f : 0.0f);
        else if (pname.equalsIgnoreCase ("pulsevolume")
                  || pname.equalsIgnoreCase ("sawvolume")
                  || pname.equalsIgnoreCase ("suboscvolume")
                  || pname.equalsIgnoreCase ("noisevolume"))
            parameter->setValueNotifyingHost (0.0f);
    }
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

    // Who drives which bus. The generic shape is voice on the main pair and
    // carrier on the sidechain; a TAL flips both, and the split modes share
    // the main pair between them.
    const float* mainL = in;
    const float* mainR = in;
    const float* scSrc = sidechain;

    if (sidechain != nullptr)
    {
        if (micOnSidechain.load (std::memory_order_relaxed))
        {
            mainL = mainR = sidechain;
            scSrc = in;
        }
        else if (carrierOnLeft.load (std::memory_order_relaxed))
        {
            mainL = sidechain;
        }
        else if (carrierOnRight.load (std::memory_order_relaxed))
        {
            mainR = sidechain;
        }
    }

    if (mains > 0)
        scratch.copyFrom (0, 0, mainL, numSamples);

    if (mains > 1)
        scratch.copyFrom (1, 0, mainR, numSamples);

    if (scSrc != nullptr)
        for (int c = 0; c < sc && mains + c < channels; ++c)
            scratch.copyFrom (mains + c, 0, scSrc, numSamples);

    juce::AudioBuffer<float> block (scratch.getArrayOfWritePointers(), channels, 0, numSamples);
    midi.clear();

    // TAL's band envelopes open per MIDI note even when the carrier comes in
    // on the sidechain — it is a played vocoder. This role wants it open for
    // good: the lane's own gate decides when anything is heard, so a note is
    // held down and re-asserted every half second, because a note sent into
    // a freshly-prepared engine can be swallowed while it resets its voices.
    // The same note with no note-off between just stays at sustain, and
    // vocoders that ignore MIDI are unbothered.
    if (wantSidechain)
    {
        noteHoldCounter += numSamples;

        if (noteHoldCounter >= (int) (sr * 0.5))
        {
            noteHoldCounter = 0;
            midi.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 0);
        }
    }

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
