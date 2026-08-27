#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace rig
{

// Sits in the MIDI row's header, so it is visible whether or not the row is
// open: what note is sounding, and the way to stop it. A stuck pad note is the
// worst thing that can happen on stage, and the cure should not be three taps
// deep.
class MidiStatusView : public juce::Component
{
public:
    MidiStatusView()
    {
        note.setJustificationType (juce::Justification::centredRight);
        note.setFont (juce::Font (juce::FontOptions (12.0f)));
        note.setColour (juce::Label::textColourId, juce::Colour (0xff9a9da6));
        note.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (note);

        panic.setButtonText ("PANIC");
        panic.setWantsKeyboardFocus (false);
        panic.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3a2320));
        panic.onClick = [this] { if (onPanic != nullptr) onPanic(); };
        addAndMakeVisible (panic);

        setNote (-1);
    }

    std::function<void()> onPanic;

    void setNote (int midiNote)
    {
        if (midiNote == shown)
            return;

        shown = midiNote;
        note.setText (midiNote < 0 ? "--" : nameOf (midiNote), juce::dontSendNotification);
        note.setColour (juce::Label::textColourId,
                        juce::Colour (midiNote < 0 ? 0xff70737c : 0xffd87b3a));
    }

    void resized() override
    {
        auto b = getLocalBounds();
        panic.setBounds (b.removeFromRight (52));
        b.removeFromRight (6);
        note.setBounds (b);
    }

private:
    static juce::String nameOf (int midiNote)
    {
        static const char* const names[] { "C", "C#", "D", "D#", "E", "F",
                                           "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[midiNote % 12]) + juce::String (midiNote / 12 - 1);
    }

    juce::Label note;
    juce::TextButton panic;
    int shown = -2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiStatusView)
};

} // namespace rig
