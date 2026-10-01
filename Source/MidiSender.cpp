#include "MidiSender.h"

#include <algorithm>

namespace rig
{

MidiSender::MidiSender() : juce::Thread ("LiveRig MIDI out")
{
    queue.resize ((size_t) capacity);
}

MidiSender::~MidiSender()
{
    close();
}

bool MidiSender::open()
{
    if (output != nullptr)
        return true;

    // MIDIServer refuses the occasional request when clients are churning —
    // plugin scans and app restarts both churn. One refusal at launch would
    // mean a whole show without the port, so ask more than once.
    for (int attempt = 0; attempt < 3 && output == nullptr; ++attempt)
    {
        if (attempt > 0)
            juce::Thread::sleep (150);

        output = juce::MidiOutput::createNewDevice (portName);
    }

    if (output == nullptr)
    {
        failure = "could not create the LiveRig MIDI port";
        return false;
    }

    failure = {};
    fifo.reset();
    activeChannel = channel.load (std::memory_order_relaxed);
    std::fill (noteChannel, noteChannel + 128, (uint8_t) 0);
    dropped.store (0, std::memory_order_relaxed);
    running.store (true, std::memory_order_relaxed);
    startThread (juce::Thread::Priority::high);
    return true;
}

void MidiSender::close()
{
    if (output == nullptr)
        return;

    signalThreadShouldExit();
    stopThread (500);
    panic();
    running.store (false, std::memory_order_relaxed);
    output.reset();
}

juce::String MidiSender::getStatus() const
{
    if (failure.isNotEmpty())
        return failure;

    return output != nullptr ? "port \"LiveRig\" open" : "port closed";
}

void MidiSender::panic()
{
    if (output == nullptr)
        return;

    for (int ch = 1; ch <= 16; ++ch)
    {
        output->sendMessageNow (juce::MidiMessage::allNotesOff (ch));
        output->sendMessageNow (juce::MidiMessage::allSoundOff (ch));

        // A stuck note is only half the disaster — a stuck bend detunes
        // every note that follows it.
        output->sendMessageNow (juce::MidiMessage::pitchWheel (ch, 8192));
    }
}

void MidiSender::push (const NoteEvent& event) noexcept
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToWrite (1, start1, size1, start2, size2);

    if (size1 + size2 < 1)
    {
        dropped.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    queue[(size_t) (size1 > 0 ? start1 : start2)] = event;
    fifo.finishedWrite (1);
}

void MidiSender::run()
{
    while (! threadShouldExit())
    {
        drain();
        wait (2);
    }

    drain();
}

void MidiSender::drain()
{
    if (output == nullptr)
        return;

    const int ch = channel.load (std::memory_order_relaxed);
    const int expr = expression.load (std::memory_order_relaxed);

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToRead (capacity, start1, size1, start2, size2);

    for (int block = 0; block < 2; ++block)
    {
        const int start = block == 0 ? start1 : start2;
        const int size = block == 0 ? size1 : size2;

        for (int i = 0; i < size; ++i)
        {
            const auto event = queue[(size_t) (start + i)];

            switch (event.type)
            {
                case NoteEvent::noteOn:
                    // A fresh channel may hold a wheel some earlier note bent;
                    // the segmenter's recentre went to the previous channel.
                    if (ch != activeChannel)
                        output->sendMessageNow (juce::MidiMessage::pitchWheel (ch, 8192));

                    noteChannel[event.a & 127] = (uint8_t) ch;
                    activeChannel = ch;
                    output->sendMessageNow (juce::MidiMessage::noteOn (ch, event.a, (juce::uint8) event.b));
                    break;

                case NoteEvent::noteOff:
                {
                    const uint8_t started = noteChannel[event.a & 127];
                    output->sendMessageNow (juce::MidiMessage::noteOff (started != 0 ? started : ch, event.a));
                    break;
                }

                case NoteEvent::bend:
                    output->sendMessageNow (juce::MidiMessage::pitchWheel (activeChannel, event.a));
                    break;

                case NoteEvent::expression:
                    if (expr > 0)
                        output->sendMessageNow (juce::MidiMessage::controllerEvent (
                                                    activeChannel, expr == 1 ? 1 : 11, event.a));
                    break;
            }
        }
    }

    fifo.finishedRead (size1 + size2);
}

} // namespace rig
