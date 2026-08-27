#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>

#include "dsp/NoteSegmenter.h"

namespace rig
{

// Owns a virtual MIDI port and carries events off the audio thread.
//
// On macOS this appears as a source called "LiveRig", so Logic, MainStage or
// anything else with a MIDI input just sees it — no cable, no interface.
//
// CoreMIDI allocates and can block, so the callback only ever calls push(),
// which writes into a lock-free ring. A small thread drains it every couple of
// milliseconds. That jitter is irrelevant for pads and swells, and the
// alternative — sending from the callback — is not something an audio thread
// survives.
class MidiSender : private juce::Thread
{
public:
    MidiSender();
    ~MidiSender() override;

    // --- message thread ---
    bool open();
    void close();
    bool isOpen() const noexcept  { return running.load (std::memory_order_relaxed); }
    juce::String getStatus() const;

    // Ends everything sounding, on every channel. Wired to a button you can
    // hit without looking, because a stuck pad note is the worst thing that
    // can happen on stage.
    void panic();

    void setChannel (int oneToSixteen) noexcept
    {
        channel.store (juce::jlimit (1, 16, oneToSixteen), std::memory_order_relaxed);
    }

    // 0 none, 1 CC1, 2 CC11.
    void setExpressionController (int mode) noexcept
    {
        expression.store (mode, std::memory_order_relaxed);
    }

    // --- audio thread ---
    void push (const NoteEvent& event) noexcept;

    int getDroppedCount() const noexcept { return dropped.load (std::memory_order_relaxed); }

private:
    void run() override;
    void drain();

    static constexpr int capacity = 2048;

    std::unique_ptr<juce::MidiOutput> output;
    juce::AbstractFifo fifo { capacity };
    std::vector<NoteEvent> queue;

    std::atomic<bool> running { false };
    std::atomic<int> channel { 1 };
    std::atomic<int> expression { 2 };
    std::atomic<int> dropped { 0 };
    juce::String failure;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiSender)
};

} // namespace rig
