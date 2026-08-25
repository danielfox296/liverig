#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../SongMaker.h"

namespace rig
{

// The whole song pipeline as a panel over the main window: install the
// splitter, name the song, watch it run. No terminal anywhere.
class SongMakerPanel : public juce::Component
{
public:
    explicit SongMakerPanel (SongMaker& m) : maker (m)
    {
        title.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
        title.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (title);

        body.setFont (juce::Font (juce::FontOptions (13.0f)));
        body.setJustificationType (juce::Justification::topLeft);
        body.setColour (juce::Label::textColourId, juce::Colour (0xff9a9da6));
        addAndMakeVisible (body);

        nameLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        nameLabel.setText ("Song name", juce::dontSendNotification);
        nameLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9a9da6));
        addAndMakeVisible (nameLabel);

        nameEditor.setFont (juce::Font (juce::FontOptions (15.0f)));
        nameEditor.onReturnKey = [this] { primary.triggerClick(); };
        addAndMakeVisible (nameEditor);

        status.setFont (juce::Font (juce::FontOptions (12.0f)));
        status.setColour (juce::Label::textColourId, juce::Colour (0xff8b8e97));
        addAndMakeVisible (status);

        addAndMakeVisible (bar);

        primary.onClick = [this] { primaryClicked(); };
        secondary.onClick = [this] { secondaryClicked(); };
        addAndMakeVisible (primary);
        addAndMakeVisible (secondary);

        setInterceptsMouseClicks (true, true);
        setVisible (false);
    }

    std::function<void (juce::File)> onSongReady;
    std::function<void()> onSplitterInstalled;

    void offerInstall()
    {
        mode = Mode::offerInstall;
        title.setText ("Song splitter needs installing", juce::dontSendNotification);
        body.setText ("Splitting a track into stems uses Demucs, which is not on this Mac yet. "
                      "It is about 900 MB and installs itself into the LiveRig folder. "
                      "This only happens once.",
                      juce::dontSendNotification);
        primary.setButtonText ("Install it");
        secondary.setButtonText ("Not now");
        setVisible (true);
        updateLayout();
    }

    void confirmSong (const juce::File& audioFile)
    {
        mode = Mode::confirm;
        pendingFile = audioFile;
        title.setText ("New song from a track", juce::dontSendNotification);
        body.setText (audioFile.getFileName() + "\n\nSplitting takes a minute or two. "
                      "The song loads by itself when it is done.",
                      juce::dontSendNotification);
        nameEditor.setText (audioFile.getFileNameWithoutExtension(), juce::dontSendNotification);
        primary.setButtonText ("Split it");
        secondary.setButtonText ("Cancel");
        setVisible (true);
        updateLayout();
        nameEditor.grabKeyboardFocus();
        nameEditor.selectAll();
    }

    // Polled by the main window's timer while this panel is up.
    void refresh()
    {
        if (mode != Mode::working)
            return;

        const auto state = maker.getState();

        if (state == SongMaker::State::done)
        {
            const auto folder = maker.getResultFolder();
            maker.clearFinishedState();
            setVisible (false);
            mode = Mode::hidden;

            if (folder.isDirectory())
            {
                if (onSongReady != nullptr)
                    onSongReady (folder);
            }
            else if (onSplitterInstalled != nullptr)
            {
                onSplitterInstalled();
            }

            return;
        }

        if (state == SongMaker::State::failed)
        {
            mode = Mode::failed;
            title.setText ("That did not work", juce::dontSendNotification);
            body.setText (maker.getMessage(), juce::dontSendNotification);
            secondary.setButtonText ("Close");
            updateLayout();
            return;
        }

        progressValue = maker.getProgress();
        status.setText (maker.getStatusLine(), juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xd0000000));

        const auto card = cardBounds().toFloat();
        g.setColour (juce::Colour (0xff1b1c21));
        g.fillRoundedRectangle (card, 8.0f);
        g.setColour (juce::Colour (0xff34363f));
        g.drawRoundedRectangle (card.reduced (0.5f), 8.0f, 1.0f);
    }

    void resized() override { updateLayout(); }

private:
    enum class Mode { hidden, offerInstall, confirm, working, failed };

    juce::Rectangle<int> cardBounds() const
    {
        return juce::Rectangle<int> (0, 0, juce::jmin (520, getWidth() - 40), cardHeight())
                   .withCentre (getLocalBounds().getCentre());
    }

    int cardHeight() const
    {
        switch (mode)
        {
            case Mode::confirm:      return 268;
            case Mode::working:      return 210;
            case Mode::failed:       return 220;
            case Mode::offerInstall: return 190;
            case Mode::hidden:       return 200;
        }
    }

    void updateLayout()
    {
        const bool showName = mode == Mode::confirm;
        const bool showBar = mode == Mode::working;

        nameLabel.setVisible (showName);
        nameEditor.setVisible (showName);
        bar.setVisible (showBar);
        status.setVisible (showBar);
        primary.setVisible (mode == Mode::offerInstall || mode == Mode::confirm);

        auto b = cardBounds().reduced (22, 20);

        title.setBounds (b.removeFromTop (24));
        b.removeFromTop (8);

        auto buttons = b.removeFromBottom (30);
        secondary.setBounds (buttons.removeFromRight (110));
        buttons.removeFromRight (8);

        if (primary.isVisible())
            primary.setBounds (buttons.removeFromRight (130));

        b.removeFromBottom (14);

        if (showBar)
        {
            status.setBounds (b.removeFromBottom (18));
            b.removeFromBottom (8);
            bar.setBounds (b.removeFromBottom (20));
            b.removeFromBottom (10);
        }

        if (showName)
        {
            nameEditor.setBounds (b.removeFromBottom (30));
            nameLabel.setBounds (b.removeFromBottom (18));
            b.removeFromBottom (10);
        }

        body.setBounds (b);
        repaint();
    }

    void primaryClicked()
    {
        if (mode == Mode::offerInstall)
        {
            maker.installSplitter();
            startWorking ("Installing the song splitter");
        }
        else if (mode == Mode::confirm)
        {
            auto name = nameEditor.getText().trim().replaceCharacter ('/', '-');

            if (name.isEmpty())
                name = pendingFile.getFileNameWithoutExtension();

            maker.makeSong (pendingFile, name);
            startWorking ("Splitting " + name);
        }
    }

    void secondaryClicked()
    {
        if (mode == Mode::working)
        {
            maker.cancel();
            return;
        }

        maker.clearFinishedState();
        setVisible (false);
        mode = Mode::hidden;
    }

    void startWorking (const juce::String& what)
    {
        mode = Mode::working;
        title.setText (what, juce::dontSendNotification);
        body.setText ("You can leave this running. Audio keeps playing.", juce::dontSendNotification);
        status.setText ("Starting...", juce::dontSendNotification);
        secondary.setButtonText ("Cancel");
        progressValue = maker.getProgress();
        updateLayout();
    }

    SongMaker& maker;
    Mode mode = Mode::hidden;
    juce::File pendingFile;

    juce::Label title, body, nameLabel, status;
    juce::TextEditor nameEditor;
    double progressValue = 0.0;
    juce::ProgressBar bar { progressValue };
    juce::TextButton primary, secondary;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongMakerPanel)
};

} // namespace rig
