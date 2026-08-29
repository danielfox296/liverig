#pragma once

#include <atomic>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

namespace rig
{

class InstrumentHost;

// One hosted effect plugin in a fixed role — the vocal channel's pitch
// corrector, its vocoder, the pedal slot on the bass chain. Three roles, three
// slots, each bound to a plugin picked from the machine's registry; no generic
// rack. Effects become configuration, not builds.
//
// Loading happens on the message thread and the finished instance reaches the
// audio thread through one atomic pointer, the replaced instance retiring to a
// slot the message thread empties — the same lifecycle the AU instrument uses,
// because it is the one that works.
//
// A slot is defensive by design, because this is stage software. A plugin that
// fails to load leaves the slot empty and audio running; one that throws in
// processBlock is dropped on the spot and cleaned up off-thread; an empty slot
// passes dry signal (the pedal) or silence (the vocal roles). The show goes on.
class EffectSlot : private juce::Timer
{
public:
    // wantSidechain asks for a second input bus after the main one, for the
    // vocoder role's second signal. Which of voice and carrier rides which
    // bus depends on the plugin: a TAL (measured, see load()) takes the
    // carrier on its MAIN input and the voice on the sidechain; the generic
    // shape is the reverse; and with no sidechain bus at all the two share
    // the main pair. The status line reports the mode in force.
    explicit EffectSlot (const juce::String& roleName, bool wantSidechain = false);
    ~EffectSlot() override;

    // --- device thread, callback stopped ---
    void prepare (double sampleRate, int maxBlockSize);

    // --- message thread ---
    bool load (InstrumentHost& registry, const juce::String& name, juce::String& error);
    void unload();
    void restore (InstrumentHost& registry, const juce::String& name,
                  const juce::String& stateBase64);

    juce::String getLoadedName() const              { return loadedName; }
    bool isLoaded() const noexcept                  { return live.load (std::memory_order_relaxed) != nullptr; }
    juce::AudioProcessor* getLiveProcessor() const noexcept
    {
        return live.load (std::memory_order_relaxed);
    }

    juce::String getStateBase64() const;

    // 0 when no sidechain bus was negotiated. The vocal UI reads this to say
    // whether the carrier really runs through a sidechain.
    int getSidechainChannels() const noexcept       { return sidechainChannels.load (std::memory_order_relaxed); }

    // True in TAL mode: the voice rides the sidechain and the carrier the
    // main input. The status line words its route report accordingly.
    bool isModulatorOnSidechain() const noexcept    { return micOnSidechain.load (std::memory_order_relaxed); }

    // What the plugin adds, for the latency readout. Live use wants zero;
    // anything visible here is the plugin's own lookahead, surfaced rather
    // than silently compensated.
    float getLatencyMs() const noexcept;
    int getLatencySamples() const noexcept          { return latencySamples.load (std::memory_order_relaxed); }

    // True after a plugin blew up in processBlock and was ejected.
    bool hasFailed() const noexcept                 { return failed.load (std::memory_order_relaxed); }

    // Why the last load refused, empty after a success — the status line
    // shows it, because a swallowed error reads as "the thing doesn't work".
    juce::String getLastError() const               { return lastError; }

    // --- audio thread ---
    // In-place mono insert for the pedal slot: dry passes untouched when the
    // slot is empty.
    void processMono (float* data, int numSamples) noexcept;

    // Mono in, stereo out, replacing outL/outR — the vocal roles. sidechain
    // may be nullptr. An empty slot writes silence.
    void process (const float* in, const float* sidechain,
                  float* outL, float* outR, int numSamples) noexcept;

private:
    void timerCallback() override;
    void applyCarrierSwitches (juce::AudioProcessor& plugin, int scChannels) const;
    bool renderBlock (juce::AudioProcessor* plugin, const float* in,
                      const float* sidechain, int numSamples) noexcept;

    juce::String role;
    const bool wantSidechain;

    std::unique_ptr<juce::AudioPluginInstance> owned;      // message thread owns
    std::atomic<juce::AudioProcessor*> live { nullptr };   // audio thread reads
    std::unique_ptr<juce::AudioPluginInstance> retired;
    std::atomic<int> retireCountdown { 0 };
    std::atomic<bool> failed { false };

    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midi;

    std::atomic<int> mainInChannels { 2 }, sidechainChannels { 0 };
    std::atomic<bool> micOnSidechain { false }, carrierOnLeft { false };
    std::atomic<int> totalIns { 2 }, totalOuts { 2 };
    std::atomic<int> latencySamples { 0 };
    std::atomic<bool> carrierOnRight { false };
    int noteHoldCounter = 1 << 30;   // audio-thread only; huge so the first block sends

    juce::String loadedName, lastError;
    double sr = 48000.0;
    int maxBlock = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectSlot)
};

} // namespace rig
