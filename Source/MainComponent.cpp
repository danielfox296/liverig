#include "MainComponent.h"

namespace rig
{

static constexpr int panelGap = 4;

MainComponent::MainComponent (ParamStore& p, AudioEngine& e, SettingsStore& s)
    : params (p), engine (e), settings (s)
{
    presets.load();

    for (auto* b : { &loadButton, &newSongButton, &transportButton, &deviceButton,
                     &savePresetButton, &deletePresetButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    transportButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3f9d4a));
    loadButton.onClick = [this] { chooseSongFolder(); };
    newSongButton.onClick = [this] { startNewSong(); };
    deviceButton.onClick = [this] { showDeviceSettings(); };
    transportButton.onClick = [this] { engine.getStemPlayer().togglePlay(); };

    songsHeader.setText ("SONGS", juce::dontSendNotification);
    stemsHeader.setText ("STEMS", juce::dontSendNotification);

    for (auto* l : { &songsHeader, &stemsHeader })
    {
        l->setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        l->setColour (juce::Label::textColourId, juce::Colour (0xff70737c));
        addAndMakeVisible (l);
    }

    songLabel.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
    songLabel.setText ("no song loaded", juce::dontSendNotification);
    addAndMakeVisible (songLabel);

    statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8b8e97));
    addAndMakeVisible (statusLabel);

    latencyLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    latencyLabel.setJustificationType (juce::Justification::centredRight);
    latencyLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8b8e97));
    addAndMakeVisible (latencyLabel);

    addAndMakeVisible (masterMeter);

    songList.setRowHeight (26);
    songList.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff17181c));
    songList.setColour (juce::ListBox::outlineColourId, juce::Colour (0xff26272d));
    songList.setOutlineThickness (1);
    addAndMakeVisible (songList);

    presetBox.setEditableText (true);
    presetBox.setTextWhenNothingSelected ("Presets");
    presetBox.onChange = [this] { applySelectedPreset(); };
    addAndMakeVisible (presetBox);

    savePresetButton.onClick = [this] { saveCurrentPreset(); };
    deletePresetButton.onClick = [this] { deleteSelectedPreset(); };

    for (int i = 0; i < StemPlayer::numStems; ++i)
        addAndMakeVisible (stemStrips.add (new StemStrip (params,
                                                          juce::String (StemPlayer::stemNames[i]),
                                                          pStemGain0 + i,
                                                          pStemMute0 + i)));

    panels.add (new FxPanel (params, "1  Input",      -1,         { pInGain }, &inputMeter));
    panels.add (new FxPanel (params, "2  Compressor", pCompOn,    { pCompThresh, pCompRatio, pCompAttack, pCompRelease, pCompMakeup }));
    panels.add (new FxPanel (params, "3  EQ",         pEqOn,      { pEqLoFreq, pEqLoGain, pEqMidFreq, pEqMidGain, pEqMidQ, pEqHiFreq, pEqHiGain }));
    panels.add (new FxPanel (params, "4  Overdrive",  pDriveOn,   { pDriveAmount, pDriveTone, pDriveLevel }));
    panels.add (new FxPanel (params, "5  Octave",     pOctOn,     { pOctInterval, pOctMix }));
    panels.add (new FxPanel (params, "6  Phaser",     pPhaserOn,  { pPhaserRate, pPhaserDepth, pPhaserFeedback }));
    panels.add (new FxPanel (params, "7  Delay",      pDelayOn,   { pDelayTime, pDelayFeedback, pDelayMix, pDelayHighcut }));
    panels.add (new FxPanel (params, "8  Reverb",     pRevOn,     { pRevSize, pRevDamp, pRevMix }));
    panels.add (new FxPanel (params, "9  Live out",   -1,         { pLiveOut }));

    for (auto* panel : panels)
    {
        panel->onExpandChanged = [this] { layoutFxPanels(); };
        fxHolder.addAndMakeVisible (panel);
    }

    fxViewport.setViewedComponent (&fxHolder, false);
    fxViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (fxViewport);

    songPanel.onJobStarted = [this] { resized(); grabKeyboardFocus(); };
    addChildComponent (songPanel);
    addChildComponent (jobStrip);

    setWantsKeyboardFocus (true);

    applyExpandedState (settings.expandedPanels);
    refreshPresetList();
    refreshSongList();

    for (int i = 0; i < NumParams; ++i)
        snapshot[(size_t) i] = params.get (i);

    if (settings.songFolder.isNotEmpty())
    {
        const juce::File folder (settings.songFolder);

        if (folder.isDirectory())
            loadSong (folder);
    }

    startTimerHz (30);
    setSize (1020, 780);
}

MainComponent::~MainComponent()
{
    stopTimer();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff101114));
}

void MainComponent::resized()
{
    songPanel.setBounds (getLocalBounds());

    auto b = getLocalBounds().reduced (10);

    auto top = b.removeFromTop (44);
    loadButton.setBounds (top.removeFromLeft (118).reduced (0, 6));
    top.removeFromLeft (8);
    newSongButton.setBounds (top.removeFromLeft (110).reduced (0, 6));
    top.removeFromLeft (8);
    transportButton.setBounds (top.removeFromLeft (88).reduced (0, 6));
    top.removeFromLeft (10);
    masterMeter.setBounds (top.removeFromRight (170).reduced (0, 13));
    top.removeFromRight (10);
    songLabel.setBounds (top);

    b.removeFromTop (8);

    if (jobStripShowing)
    {
        jobStrip.setBounds (b.removeFromTop (32));
        b.removeFromTop (8);
    }

    auto footer = b.removeFromBottom (30);
    deviceButton.setBounds (footer.removeFromLeft (132).reduced (0, 2));
    footer.removeFromLeft (10);
    latencyLabel.setBounds (footer.removeFromRight (190));
    statusLabel.setBounds (footer);

    b.removeFromBottom (8);

    auto left = b.removeFromLeft (300);

    songsHeader.setBounds (left.removeFromTop (18));
    left.removeFromTop (4);
    songList.setBounds (left.removeFromTop (juce::jmin (208, left.getHeight() / 3)));
    left.removeFromTop (12);
    stemsHeader.setBounds (left.removeFromTop (18));
    left.removeFromTop (4);

    const int stripWidth = (left.getWidth() - 3 * 6) / 4;

    for (auto* strip : stemStrips)
    {
        strip->setBounds (left.removeFromLeft (stripWidth));
        left.removeFromLeft (6);
    }

    b.removeFromLeft (10);

    auto presetBar = b.removeFromTop (30);
    deletePresetButton.setBounds (presetBar.removeFromRight (72));
    presetBar.removeFromRight (6);
    savePresetButton.setBounds (presetBar.removeFromRight (66));
    presetBar.removeFromRight (8);
    presetBox.setBounds (presetBar);

    b.removeFromTop (8);
    fxViewport.setBounds (b);

    layoutFxPanels();
}

void MainComponent::expandAllForSnapshot()
{
    for (auto* panel : panels)
        panel->setExpanded (true);
}

void MainComponent::showJobStripForSnapshot()
{
    jobStripShowing = true;
    jobStrip.setVisible (true);
    jobStrip.refresh();
    resized();
}

void MainComponent::layoutFxPanels()
{
    const int width = fxViewport.getWidth() - fxViewport.getScrollBarThickness() - 2;

    if (width <= 0)
        return;

    int y = 0;

    for (auto* panel : panels)
    {
        const int height = panel->getPreferredHeight();
        panel->setBounds (0, y, width, height);
        y += height + panelGap;
    }

    fxHolder.setSize (width, y);
}

void MainComponent::parentHierarchyChanged()
{
    if (isShowing())
        grabKeyboardFocus();
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (songPanel.isVisible())
        return false;

    if (key == juce::KeyPress::spaceKey)
    {
        engine.getStemPlayer().togglePlay();
        return true;
    }

    return false;
}

//==============================================================================
int MainComponent::getNumRows()
{
    return songNames.size();
}

void MainComponent::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, songNames.size()))
        return;

    if (selected)
    {
        g.setColour (juce::Colour (0xff2f3138));
        g.fillRect (0, 0, width, height);
        g.setColour (juce::Colour (0xffd87b3a));
        g.fillRect (0, 0, 3, height);
    }

    g.setColour (juce::Colour (selected ? 0xffe8e8ea : 0xffb4b6bd));
    g.setFont (juce::Font (juce::FontOptions (13.0f)));
    g.drawText (songNames[row], 12, 0, width - 18, height, juce::Justification::centredLeft, true);
}

void MainComponent::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    if (juce::isPositiveAndBelow (row, songNames.size()))
        loadSong (SongMaker::songsDir().getChildFile (songNames[row]));
}

void MainComponent::refreshSongList()
{
    songNames.clear();

    const auto dir = SongMaker::songsDir();

    if (dir.isDirectory())
    {
        for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findDirectories))
        {
            const auto folder = entry.getFile();
            bool hasStem = false;

            for (int i = 0; i < StemPlayer::numStems; ++i)
                hasStem = hasStem || folder.getChildFile (juce::String (StemPlayer::stemNames[i]) + ".wav")
                                         .existsAsFile();

            if (hasStem)
                songNames.add (folder.getFileName());
        }
    }

    songNames.sort (true);
    songList.updateContent();

    const int index = songNames.indexOf (loadedFolder.getFileName());

    if (index >= 0)
        songList.selectRow (index, true, true);
    else
        songList.deselectAllRows();

    songList.repaint();
}

//==============================================================================
void MainComponent::chooseSongFolder()
{
    juce::File start = SongMaker::songsDir();

    if (! start.isDirectory())
        start = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    chooser = std::make_unique<juce::FileChooser> ("Choose a song folder", start);

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto folder = fc.getResult();

                              if (folder.isDirectory())
                                  loadSong (folder);

                              grabKeyboardFocus();
                          });
}

void MainComponent::startNewSong()
{
    if (songMaker.isBusy())
        return;

    if (! SongMaker::isSplitterInstalled())
        songPanel.offerInstall();
    else
        chooseAudioFile();
}

void MainComponent::chooseAudioFile()
{
    juce::File start = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                          .getChildFile ("Downloads");

    if (! start.isDirectory())
        start = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    chooser = std::make_unique<juce::FileChooser> ("Choose a track to split", start,
                                                   "*.mp3;*.wav;*.m4a;*.aiff;*.aif;*.flac;*.ogg");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();

                              if (file.existsAsFile())
                                  songPanel.confirmSong (file);
                          });
}

void MainComponent::loadSong (const juce::File& folder)
{
    const auto result = engine.getStemPlayer().loadSongFolder (folder);

    if (result.failed())
    {
        songLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe4483c));
        songLabel.setText (result.getErrorMessage(), juce::dontSendNotification);
        return;
    }

    loadedFolder = folder;

    songLabel.setColour (juce::Label::textColourId, juce::Colour (0xffc8c9ce));
    songLabel.setText (engine.getStemPlayer().getSongName()
                        + "   " + juce::String (engine.getStemPlayer().getLengthSeconds(), 1) + " s",
                       juce::dontSendNotification);

    settings.songFolder = folder.getFullPathName();
    saveSettings();

    const int index = songNames.indexOf (folder.getFileName());

    if (index >= 0)
        songList.selectRow (index, true, true);
}

void MainComponent::showDeviceSettings()
{
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
                        engine.getDeviceManager(), 0, 2, 0, 2, false, false, true, false);
    selector->setSize (520, 400);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (selector.release());
    options.dialogTitle = "Audio device";
    options.dialogBackgroundColour = juce::Colour (0xff17181c);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

//==============================================================================
void MainComponent::refreshPresetList (const juce::String& selectName)
{
    presetBox.clear (juce::dontSendNotification);

    const auto names = presets.getNames();

    for (int i = 0; i < names.size(); ++i)
        presetBox.addItem (names[i], i + 1);

    const int index = names.indexOf (selectName);

    if (index >= 0)
        presetBox.setSelectedId (index + 1, juce::dontSendNotification);
    else
        presetBox.setText (selectName, juce::dontSendNotification);
}

void MainComponent::applySelectedPreset()
{
    const auto name = presetBox.getText();

    if (presets.apply (name, params))
        refreshAllControls();
}

void MainComponent::saveCurrentPreset()
{
    const auto name = presetBox.getText().trim();

    if (name.isEmpty())
        return;

    presets.store (name, params);
    refreshPresetList (name);
}

void MainComponent::deleteSelectedPreset()
{
    const auto name = presetBox.getText().trim();

    if (name.isEmpty() || ! presets.contains (name))
        return;

    presets.remove (name);
    refreshPresetList();
}

void MainComponent::refreshAllControls()
{
    for (auto* panel : panels)
        panel->refresh();

    for (auto* strip : stemStrips)
        strip->refresh();
}

//==============================================================================
juce::String MainComponent::expandedStateString() const
{
    juce::String state;

    for (auto* panel : panels)
        state << (panel->isExpanded() ? '1' : '0');

    return state;
}

void MainComponent::applyExpandedState (const juce::String& state)
{
    if (state.isEmpty())
    {
        panels[0]->setExpanded (true);   // input trim
        panels[4]->setExpanded (true);   // the octave
        return;
    }

    for (int i = 0; i < panels.size() && i < state.length(); ++i)
        panels[i]->setExpanded (state[i] == '1');
}

bool MainComponent::paramsChangedSinceSnapshot()
{
    bool dirty = false;

    for (int i = 0; i < NumParams; ++i)
    {
        const float v = params.get (i);

        if (! juce::exactlyEqual (snapshot[(size_t) i], v))
        {
            snapshot[(size_t) i] = v;
            dirty = true;
        }
    }

    return dirty;
}

void MainComponent::saveSettings()
{
    if (auto state = engine.getDeviceManager().createStateXml())
        settings.deviceState = state->toString();

    settings.expandedPanels = expandedStateString();
    settings.save (params);
}

void MainComponent::timerCallback()
{
    auto& player = engine.getStemPlayer();

    if (player.reloadNeeded())
        player.reloadForCurrentRate();

    // Background job: report it, never block on it.
    const bool busy = songMaker.isBusy();

    if (busy != jobStripShowing)
    {
        jobStripShowing = busy;
        jobStrip.setVisible (busy);
        resized();
    }

    if (busy)
        jobStrip.refresh();

    switch (songMaker.getState())
    {
        case SongMaker::State::done:
        {
            const auto folder = songMaker.getResultFolder();
            songMaker.clearFinishedState();

            if (folder.isDirectory())
            {
                refreshSongList();
                loadSong (folder);
            }
            else
            {
                chooseAudioFile();   // the install finished; carry on to the track
            }

            break;
        }

        case SongMaker::State::failed:
            // A job you stopped on purpose is not an error worth a dialog.
            if (! songMaker.wasCancelledByUser())
                songPanel.showFailure (songMaker.getMessage());

            songMaker.clearFinishedState();
            break;

        case SongMaker::State::idle:
        case SongMaker::State::installing:
        case SongMaker::State::splitting:
            break;
    }

    inputMeter.setLevel (0, engine.getInputPeak());
    masterMeter.setLevel (0, engine.getOutputPeakL());
    masterMeter.setLevel (1, engine.getOutputPeakR());

    const bool playing = player.isPlaying();
    transportButton.setToggleState (playing, juce::dontSendNotification);
    transportButton.setButtonText (playing ? "STOP" : "PLAY");

    for (int i = 0; i < StemPlayer::numStems; ++i)
    {
        const bool present = player.isStemPresent (i);

        if (presentCache[i] != present)
        {
            presentCache[i] = present;
            stemStrips[i]->setPresent (present);
        }
    }

    const double rate = engine.getSampleRate();
    const int block = engine.getBufferSize();

    if (rate > 0.0)
        statusLabel.setText (juce::String (rate / 1000.0, 1) + " kHz    "
                                + juce::String (block) + " samples    "
                                + juce::String (block * 1000.0 / rate, 2) + " ms/block",
                             juce::dontSendNotification);
    else
        statusLabel.setText ("no audio device", juce::dontSendNotification);

    latencyLabel.setText ("FX latency  " + juce::String (engine.getFxLatencyMs(), 1) + " ms",
                          juce::dontSendNotification);

    if (++saveCounter >= 90)   // every three seconds, if anything moved
    {
        saveCounter = 0;

        if (paramsChangedSinceSnapshot())
            saveSettings();
    }
}

} // namespace rig
