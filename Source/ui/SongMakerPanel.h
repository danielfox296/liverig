#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../SongMaker.h"
#include "Type.h"

namespace rig
{

// The decisions in the song pipeline: install the splitter, name the song,
// report a failure. It is only up while it is waiting on you — the actual
// work happens behind a strip so the rest of the app stays live.
class SongMakerPanel : public juce::Component
{
public:
    explicit SongMakerPanel (SongMaker& m) : maker (m)
    {
        title.setFont (rigFont (17.0f, true));
        addAndMakeVisible (title);

        body.setFont (rigFont (13.0f));
        body.setJustificationType (juce::Justification::topLeft);
        body.setColour (juce::Label::textColourId, juce::Colour (0xff9a9da6));
        addAndMakeVisible (body);

        nameLabel.setFont (rigFont (12.0f));
        nameLabel.setText ("Song name", juce::dontSendNotification);
        nameLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9a9da6));
        addAndMakeVisible (nameLabel);

        nameEditor.setFont (rigFont (15.0f));
        nameEditor.onReturnKey = [this] { primary.triggerClick(); };
        addAndMakeVisible (nameEditor);

        primary.setWantsKeyboardFocus (false);
        secondary.setWantsKeyboardFocus (false);
        primary.onClick = [this] { primaryClicked(); };
        secondary.onClick = [this] { dismiss(); };
        addAndMakeVisible (primary);
        addAndMakeVisible (secondary);

        setInterceptsMouseClicks (true, true);
        setVisible (false);
    }

    std::function<void()> onJobStarted;

    void offerInstall()
    {
        mode = Mode::offerInstall;
        title.setText ("Song splitter needs installing", juce::dontSendNotification);
        body.setText ("Splitting a track into stems uses Demucs, which is not on this Mac yet. "
                      "It is about 900 MB and installs itself into the LiveRig folder. "
                      "This only happens once.",
                      juce::dontSendNotification);
        show();
    }

    void confirmSong (const juce::File& audioFile)
    {
        mode = Mode::confirm;
        pendingFile = audioFile;
        title.setText ("New song from a track", juce::dontSendNotification);
        body.setText (audioFile.getFileName() + "\n\nSplitting takes a minute or two. It runs in "
                      "the background — you can keep playing while it works, and the song "
                      "appears in the list when it is done.",
                      juce::dontSendNotification);
        nameEditor.setText (audioFile.getFileNameWithoutExtension(), juce::dontSendNotification);
        show();
        nameEditor.grabKeyboardFocus();
        nameEditor.selectAll();
    }

    void showFailure (const juce::String& text)
    {
        mode = Mode::failed;
        title.setText ("That did not work", juce::dontSendNotification);
        body.setText (text, juce::dontSendNotification);
        show();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xc0000000));

        const auto card = cardBounds().toFloat();
        g.setColour (juce::Colour (0xff1b1c21));
        g.fillRoundedRectangle (card, 8.0f);
        g.setColour (juce::Colour (0xff34363f));
        g.drawRoundedRectangle (card.reduced (0.5f), 8.0f, 1.0f);
    }

    void resized() override { updateLayout(); }

private:
    enum class Mode { hidden, offerInstall, confirm, failed };

    void show()
    {
        applyModeText();
        setVisible (true);
        toFront (false);
        updateLayout();
    }

    // Every label the mode owns, in one place.
    void applyModeText()
    {
        switch (mode)
        {
            case Mode::offerInstall:
                primary.setButtonText ("Install it");
                secondary.setButtonText ("Not now");
                break;

            case Mode::confirm:
                primary.setButtonText ("Split it");
                secondary.setButtonText ("Cancel");
                break;

            case Mode::failed:
                primary.setButtonText ({});
                secondary.setButtonText ("Close");
                break;

            case Mode::hidden:
                break;
        }
    }

    void dismiss()
    {
        maker.clearFinishedState();
        mode = Mode::hidden;
        setVisible (false);
    }

    juce::Rectangle<int> cardBounds() const
    {
        const int height = mode == Mode::confirm ? 276 : (mode == Mode::failed ? 224 : 196);

        return juce::Rectangle<int> (0, 0, juce::jmin (520, getWidth() - 40), height)
                   .withCentre (getLocalBounds().getCentre());
    }

    void updateLayout()
    {
        const bool showName = mode == Mode::confirm;

        nameLabel.setVisible (showName);
        nameEditor.setVisible (showName);
        primary.setVisible (mode == Mode::offerInstall || mode == Mode::confirm);

        auto b = cardBounds().reduced (22, 20);

        title.setBounds (b.removeFromTop (24));
        b.removeFromTop (8);

        auto buttons = b.removeFromBottom (32);
        secondary.setBounds (buttons.removeFromRight (110));
        buttons.removeFromRight (8);

        if (primary.isVisible())
            primary.setBounds (buttons.removeFromRight (130));

        b.removeFromBottom (14);

        if (showName)
        {
            nameEditor.setBounds (b.removeFromBottom (32));
            nameLabel.setBounds (b.removeFromBottom (18));
            b.removeFromBottom (10);
        }

        body.setBounds (b);
        repaint();
    }

    void primaryClicked()
    {
        if (mode == Mode::offerInstall)
            maker.installSplitter();
        else if (mode == Mode::confirm)
            maker.makeSong (pendingFile, cleanName());
        else
            return;

        mode = Mode::hidden;
        setVisible (false);

        if (onJobStarted != nullptr)
            onJobStarted();
    }

    juce::String cleanName() const
    {
        auto name = nameEditor.getText().trim().replaceCharacter ('/', '-');
        return name.isEmpty() ? pendingFile.getFileNameWithoutExtension() : name;
    }

    SongMaker& maker;
    Mode mode = Mode::hidden;
    juce::File pendingFile;

    juce::Label title, body, nameLabel;
    juce::TextEditor nameEditor;
    juce::TextButton primary, secondary;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongMakerPanel)
};

//==============================================================================
// A job running in the background, reported without taking the app hostage.
class JobStrip : public juce::Component
{
public:
    explicit JobStrip (SongMaker& m) : maker (m)
    {
        label.setFont (rigFont (12.0f));
        label.setColour (juce::Label::textColourId, juce::Colour (0xffc8c9ce));
        addAndMakeVisible (label);

        detail.setFont (rigFont (11.0f));
        detail.setColour (juce::Label::textColourId, juce::Colour (0xff70737c));
        detail.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (detail);

        addAndMakeVisible (bar);

        cancel.setButtonText ("Cancel");
        cancel.setWantsKeyboardFocus (false);
        cancel.onClick = [this] { maker.cancel(); };
        addAndMakeVisible (cancel);

        setVisible (false);
    }

    void refresh()
    {
        const bool installing = maker.getState() == SongMaker::State::installing;

        label.setText (installing ? "Installing the song splitter"
                                  : "Splitting " + maker.getSongName(),
                       juce::dontSendNotification);
        detail.setText (maker.getStatusLine(), juce::dontSendNotification);
        progressValue = maker.getProgress();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff1b1c21));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        g.setColour (juce::Colour (0xff34363f));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (10, 5);
        cancel.setBounds (b.removeFromRight (76));
        b.removeFromRight (10);
        label.setBounds (b.removeFromLeft (200));
        bar.setBounds (b.removeFromLeft (juce::jmax (80, b.getWidth() / 2)).reduced (0, 3));
        b.removeFromLeft (10);
        detail.setBounds (b);
    }

private:
    SongMaker& maker;
    juce::Label label, detail;
    double progressValue = 0.0;
    juce::ProgressBar bar { progressValue };
    juce::TextButton cancel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JobStrip)
};

} // namespace rig
