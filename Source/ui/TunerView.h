#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Type.h"

namespace rig
{

// The tuner tab. It reads the same pitch tracker that drives the octave and
// the synth — YIN plus full-rate refinement, accurate to well under a cent —
// so there is no separate detector to disagree with the rig.
//
// Chromatic at heart: it names whatever note is sounding and shows the cents
// off. The instrument switch just chooses which string row is drawn, so a
// glance says which string you are near.
class TunerView : public juce::Component
{
public:
    TunerView()
    {
        bassButton.setButtonText ("BASS");
        guitarButton.setButtonText ("GUITAR");

        for (auto* b : { &bassButton, &guitarButton })
        {
            b->setWantsKeyboardFocus (false);
            b->setClickingTogglesState (true);
            b->setRadioGroupId (1);
            b->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd87b3a));
            addAndMakeVisible (b);
        }

        bassButton.setToggleState (true, juce::dontSendNotification);
        bassButton.onClick = [this] { modeChanged(); };
        guitarButton.onClick = [this] { modeChanged(); };
    }

    std::function<void()> onModeChanged;

    bool isBassMode() const noexcept { return bassButton.getToggleState(); }

    void setBassMode (bool bass)
    {
        bassButton.setToggleState (bass, juce::dontSendNotification);
        guitarButton.setToggleState (! bass, juce::dontSendNotification);
    }

    // Fed from the UI timer while the tab is showing.
    void update (float trackedHz, float confidence, float inputPeak)
    {
        const bool sounding = trackedHz > 20.0f && confidence > 0.5f && inputPeak > 0.004f;

        if (sounding)
        {
            hz = trackedHz;
            // The note readout holds briefly through tracker flickers so it
            // reads steady rather than strobing.
            holdCounter = 12;
        }
        else if (holdCounter > 0)
        {
            --holdCounter;
        }

        live = holdCounter > 0;
        repaint();
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (12, 10);
        auto row = b.removeFromTop (28);

        bassButton.setBounds (row.removeFromLeft (90));
        row.removeFromLeft (6);
        guitarButton.setBounds (row.removeFromLeft (90));
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff17181c));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        g.setColour (juce::Colour (0xff26272d));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);

        auto b = getLocalBounds().reduced (12, 10);
        b.removeFromTop (34);   // instrument switch row

        if (! live)
        {
            g.setColour (juce::Colour (0xff565963));
            g.setFont (rigFont (14.0f));
            g.drawText ("listening...", b, juce::Justification::centred);
            drawStrings (g, b.removeFromBottom (56), -1);
            return;
        }

        // Nearest chromatic note and the cents to it.
        const double midi = 69.0 + 12.0 * std::log2 ((double) hz / 440.0);
        const int note = (int) std::lround (midi);
        const double target = 440.0 * std::pow (2.0, (note - 69) / 12.0);
        const double cents = 1200.0 * std::log2 ((double) hz / target);
        const bool inTune = std::abs (cents) < 3.0;

        static const char* names[12] { "C", "C#", "D", "D#", "E", "F",
                                       "F#", "G", "G#", "A", "A#", "B" };

        const juce::String noteName = juce::String (names[((note % 12) + 12) % 12])
                                        + juce::String (note / 12 - 1);

        auto strings = b.removeFromBottom (56);
        auto meter = b.removeFromBottom (46);
        auto readout = b;

        g.setColour (inTune ? juce::Colour (0xff5fbf6a) : juce::Colour (0xffe8e8ea));
        g.setFont (rigFont (44.0f, true));
        g.drawText (noteName, readout.removeFromTop (juce::jmax (48, readout.getHeight() - 22)),
                    juce::Justification::centred);

        g.setColour (juce::Colour (0xff9a9da6));
        g.setFont (rigFont (12.0f));
        g.drawText (juce::String (cents > 0 ? "+" : "") + juce::String (cents, 1) + " cents    "
                        + juce::String (hz, 1) + " Hz",
                    readout, juce::Justification::centred);

        drawCentsMeter (g, meter.reduced (8, 6), cents, inTune);
        drawStrings (g, strings, note);
    }

private:
    void modeChanged()
    {
        repaint();

        if (onModeChanged != nullptr)
            onModeChanged();
    }

    void drawCentsMeter (juce::Graphics& g, juce::Rectangle<int> area, double cents, bool inTune)
    {
        const auto track = area.toFloat().withSizeKeepingCentre ((float) area.getWidth(), 6.0f);

        g.setColour (juce::Colour (0xff2a2c33));
        g.fillRoundedRectangle (track, 3.0f);

        // Centre tick and the +-3 cent green zone.
        const float centre = track.getCentreX();
        const float halfWidth = track.getWidth() * 0.5f;
        const float greenHalf = halfWidth * (3.0f / 50.0f);

        g.setColour (juce::Colour (0xff284a2c));
        g.fillRoundedRectangle (centre - greenHalf, track.getY(), greenHalf * 2.0f,
                                track.getHeight(), 3.0f);

        g.setColour (juce::Colour (0xff565963));
        g.fillRect (centre - 0.5f, (float) area.getY(), 1.0f, (float) area.getHeight());

        // The needle, clamped to +-50 cents.
        const double clamped = juce::jlimit (-50.0, 50.0, cents);
        const float x = centre + (float) (clamped / 50.0) * halfWidth;

        g.setColour (inTune ? juce::Colour (0xff5fbf6a) : juce::Colour (0xffd87b3a));
        g.fillRoundedRectangle (x - 2.0f, (float) area.getY(), 4.0f, (float) area.getHeight(), 2.0f);
    }

    void drawStrings (juce::Graphics& g, juce::Rectangle<int> area, int currentNote)
    {
        struct StringNote { const char* name; int midi; };

        static const StringNote bass[5]   { { "B", 23 }, { "E", 28 }, { "A", 33 },
                                            { "D", 38 }, { "G", 43 } };
        static const StringNote guitar[6] { { "E", 40 }, { "A", 45 }, { "D", 50 },
                                            { "G", 55 }, { "B", 59 }, { "E", 64 } };

        const StringNote* strings = isBassMode() ? bass : guitar;
        const int count = isBassMode() ? 5 : 6;

        const float cell = (float) area.getWidth() / (float) count;

        for (int i = 0; i < count; ++i)
        {
            const bool near = currentNote >= 0 && strings[i].midi == currentNote;
            const auto centre = juce::Point<float> ((float) area.getX() + cell * ((float) i + 0.5f),
                                                    (float) area.getCentreY() - 6.0f);

            g.setColour (near ? juce::Colour (0xffd87b3a) : juce::Colour (0xff32343c));
            g.fillEllipse (centre.x - 7.0f, centre.y - 7.0f, 14.0f, 14.0f);

            g.setColour (near ? juce::Colour (0xffe8e8ea) : juce::Colour (0xff70737c));
            g.setFont (rigFont (11.0f, near));
            g.drawText (strings[i].name,
                        (int) (centre.x - 20.0f), (int) (centre.y + 10.0f), 40, 16,
                        juce::Justification::centred);
        }
    }

    juce::TextButton bassButton, guitarButton;
    float hz = 0.0f;
    bool live = false;
    int holdCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TunerView)
};

} // namespace rig
