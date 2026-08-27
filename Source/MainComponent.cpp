#include "MainComponent.h"

namespace rig
{

static constexpr int panelGap = 7;

MainComponent::MainComponent (ParamStore& p, AudioEngine& e, SettingsStore& s)
    : params (p), engine (e), settings (s)
{
    presets.load();

    for (auto* b : { &loadButton, &newSongButton, &transportButton, &loopButton,
                     &deviceButton, &savePresetButton, &deletePresetButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    transportButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3f9d4a));
    loopButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd87b3a));
    loadButton.onClick = [this] { chooseSongFolder(); };
    newSongButton.onClick = [this] { startNewSong(); };
    deviceButton.onClick = [this] { showDeviceSettings(); };
    transportButton.onClick = [this] { engine.getStemPlayer().togglePlay(); };
    loopButton.onClick = [this] { loopTapped(); };

    songsHeader.setText ("SONGS", juce::dontSendNotification);
    stemsHeader.setText ("STEMS", juce::dontSendNotification);

    for (auto* l : { &songsHeader, &stemsHeader })
    {
        l->setFont (rigFont (11.0f, true));
        l->setColour (juce::Label::textColourId, juce::Colour (0xff70737c));
        addAndMakeVisible (l);
    }

    songLabel.setFont (rigFont (15.0f, true));
    songLabel.setText ("no song loaded", juce::dontSendNotification);
    addAndMakeVisible (songLabel);

    statusLabel.setFont (rigFont (12.0f));
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8b8e97));
    addAndMakeVisible (statusLabel);

    latencyLabel.setFont (rigFont (12.0f));
    latencyLabel.setJustificationType (juce::Justification::centredRight);
    latencyLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8b8e97));
    addAndMakeVisible (latencyLabel);

    grLabel.setFont (rigFont (11.0f, true));
    grLabel.setJustificationType (juce::Justification::centredRight);
    grLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd87b3a));
    addAndMakeVisible (grLabel);

    addAndMakeVisible (masterMeter);

    songList.setRowHeight (28);
    songList.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff17181c));
    songList.setColour (juce::ListBox::outlineColourId, juce::Colour (0xff26272d));
    songList.setOutlineThickness (1);
    addAndMakeVisible (songList);

    presetBox.setEditableText (true);
    presetBox.setTextWhenNothingSelected ("Whole chain preset");
    presetBox.onChange = [this] { applySelectedPreset(); };
    addAndMakeVisible (presetBox);

    savePresetButton.onClick = [this] { saveCurrentPreset(); };
    deletePresetButton.onClick = [this] { deleteSelectedPreset(); };

    for (int i = 0; i < StemPlayer::numStems; ++i)
        addAndMakeVisible (stemStrips.add (new StemStrip (params,
                                                          juce::String (StemPlayer::stemNames[i]),
                                                          pStemGain0 + i,
                                                          pStemMute0 + i)));

    fxDefs = {
        { "input",  -1,                false, "",       "Input",      -1,        { pInGain } },
        { "midi",   -1,                false, "midi",   "MIDI",       pMidiOn,   { pMidiChannel, pMidiTranspose, pMidiGate, pMidiHold, pMidiRelease, pMidiBendRange, pMidiExpression } },
        { "instrument", -1,            false, "instrument", "Instrument", pInstOn, { pInstLevel } },
        { "comp",   FxChain::stComp,   true,  "comp",   "Compressor", pCompOn,   { pCompThresh, pCompRatio, pCompAttack, pCompRelease, pCompMakeup, pCompBlend, pCompSidechain } },
        { "eq",     FxChain::stEq,     true,  "eq",     "EQ",         pEqOn,     { pEqLoFreq, pEqLoGain, pEqMidFreq, pEqMidGain, pEqMidQ, pEqHiFreq, pEqHiGain } },
        { "filter", FxChain::stFilter, true,  "filter", "Filter",     pFltOn,    { pFltSens, pFltAttack, pFltRelease, pFltFrom, pFltTo, pFltRes, pFltMix } },
        { "boost",  FxChain::stBoost,  true,  "boost",  "Boost",      pBoostOn,  { pBoostGain, pBoostSheen, pBoostHair } },
        { "drive",  FxChain::stDrive,  true,  "drive",  "Overdrive",  pDriveOn,  { pDriveVoice, pDriveAmount, pDriveTone, pDriveLevel, pDriveStack, pDriveBite, pDriveXover, pDriveBlend } },
        { "amp",    FxChain::stAmp,    true,  "amp",    "Amp",        pAmpOn,    { pAmpIn, pAmpOut } },
        { "cab",    FxChain::stCab,    true,  "cab",    "Cab (B)",    pCabOn,    { pCabBumpFreq, pCabBumpGain, pCabTop, pCabPresence } },
        { "octave", FxChain::stOctave, true,  "octave", "Octave",     pOctOn,    { pOctInterval, pOctMix, pOct2Interval, pOct2Level } },
        { "sub",    FxChain::stSub,    true,  "sub",    "Sub (B)",    pSubOn,    { pSubLevel, pSubTone, pSubDivide } },
        { "synth",  FxChain::stSynth,  true,  "synth",  "Synth",      pSynOn,    { pSynShape, pSynSub, pSynCutoff, pSynEnv, pSynRes, pSynGlide, pSynMix } },
        { "phaser", FxChain::stPhaser, true,  "phaser", "Phaser",     pPhaserOn, { pPhaserRate, pPhaserDepth, pPhaserFeedback, pPhaserStages } },
        { "chorus", FxChain::stChorus, true,  "chorus", "Chorus",     pChoOn,    { pChoRate, pChoDepth, pChoVoices, pChoTone, pChoLows, pChoMix } },
        { "delay",  FxChain::stDelay,  true,  "delay",  "Delay",      pDelayOn,  { pDelayTime, pDelayFeedback, pDelayMix, pDelayHighcut, pDelayModRate, pDelayModDepth, pDelaySat, pDelaySmear } },
        { "tape",   FxChain::stTape,   true,  "tape",   "Tape",       pTapeOn,   { pTapeDrive, pTapeSoften, pTapeBump, pTapeMix } },
        { "reverb", -1,                false, "reverb", "Reverb",     pRevOn,    { pRevSize, pRevDamp, pRevPre, pRevMod, pRevMix } },
        { "out",    -1,                false, "",       "Live out",   -1,        { pLiveOut } },
        { "master", -1,                false, "",       "Master",     -1,        { pMasterGain } },
    };

    applyChainOrder (settings.chainOrder);

    for (const auto& def : fxDefs)
    {
        auto* panel = panels.add (new FxPanel (params, def.name, def.onParam, def.movable,
                                               def.key == "input" ? &inputMeter
                                                    : (def.key == "midi" ? (juce::Component*) &midiStatus : nullptr)));

        // Tapping the selected row again puts its controls away. Either way,
        // the effect tab comes forward — tapping a row means "show me this".
        panel->onSelect = [this, panel]
        {
            const int i = panels.indexOf (panel);
            selectFx (selectedFx == i ? -1 : i);
            editorTabs.setCurrentTabIndex (0);
            saveSettings();
        };

        if (def.movable)
        {
            panel->onDragMove = [this, panel] (const juce::MouseEvent& event) { dragFxRow (panel, event); };
            panel->onDragEnd = [this, panel] { dropFxRow (panel); };
        }

        fxHolder.addAndMakeVisible (panel);
    }

    renumberFxTitles();
    pushOrderToChain();

    fxViewport.setViewedComponent (&fxHolder, false);
    fxViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (fxViewport);

    editorTabs.setTabBarDepth (30);
    editorTabs.setOutline (0);
    editorTabs.addTab ("EFFECT", juce::Colour (0xff101114), &fxEditor, false);
    editorTabs.addTab ("TUNER", juce::Colour (0xff101114), &tunerView, false);
    addAndMakeVisible (editorTabs);
    fxEditor.clear();

    tunerView.setBassMode (settings.tuner != "guitar");
    tunerView.onModeChanged = [this] { saveSettings(); };

    midiStatus.onPanic = [this] { engine.getMidi().panic(); };

    engine.getInstrument().beginScan();

    instrumentControls.instrumentBox.onChange = [this]
    {
        const auto name = instrumentControls.instrumentBox.getText();

        if (name.isEmpty() || name == engine.getInstrument().getLoadedName())
            return;

        juce::String error;

        if (! engine.getInstrument().load (name, error))
            songPanel.showFailure ("Could not load " + name + ".\n\n" + error);

        refreshInstrumentControls();
        saveSettings();
    };

    instrumentControls.clear.onClick = [this]
    {
        pluginWindow.reset();
        engine.getInstrument().unload();
        refreshInstrumentControls();
        saveSettings();
    };

    instrumentControls.open.onClick = [this] { openPluginWindow(); };

    if (settings.instrument.isNotEmpty())
        engine.getInstrument().restore (settings.instrument, settings.instrumentState);

    songPanel.onJobStarted = [this] { resized(); grabKeyboardFocus(); };
    addChildComponent (songPanel);
    addChildComponent (jobStrip);

    setWantsKeyboardFocus (true);

    applyExpandedState (settings.expandedPanels);
    refreshPresetList();
    refreshSongList();

    ampControls.load.onClick = [this] { chooseAmpCapture(); };
    ampControls.clear.onClick = [this]
    {
        engine.getFxChain().getAmp().clearModel();
        settings.ampModel.clear();
        refreshAmpControls();
        saveSettings();
    };
    ampControls.captureBox.onChange = [this]
    {
        const auto name = ampControls.captureBox.getText();

        if (name.isNotEmpty() && name != engine.getFxChain().getAmp().getLoadedName())
            loadAmpFile (AmpModel::libraryFileForName (name));
    };

    fxEditor.ampCaptureProvider = [this] { return engine.getFxChain().getAmp().getLoadedName(); };
    fxEditor.onAmpCapture = [this] (const juce::var& captureName) { applyAmpCapture (captureName); };

    // Bring last session's capture back before the first note. A capture
    // loaded before the library existed gets moved into it on the way.
    if (settings.ampModel.isNotEmpty())
    {
        juce::File capture (settings.ampModel);

        if (capture.existsAsFile() && ! capture.isAChildOf (AmpModel::libraryDir()))
        {
            juce::String error;
            const auto imported = AmpModel::importToLibrary (capture, error);

            if (imported != juce::File())
                capture = imported;
        }

        juce::String error;

        if (capture.existsAsFile() && engine.getFxChain().getAmp().load (capture, error))
            settings.ampModel = capture.getFullPathName();
        else
            settings.ampModel.clear();
    }

    refreshAmpControls();

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
    top.removeFromLeft (8);
    loopButton.setBounds (top.removeFromLeft (96).reduced (0, 6));
    top.removeFromLeft (10);
    masterMeter.setBounds (top.removeFromRight (170).reduced (0, 13));
    top.removeFromRight (6);
    grLabel.setBounds (top.removeFromRight (86));
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
    latencyLabel.setBounds (footer.removeFromRight (250));
    statusLabel.setBounds (footer);

    b.removeFromBottom (8);

    auto left = b.removeFromLeft (300);

    songsHeader.setBounds (left.removeFromTop (18));
    left.removeFromTop (4);
    songList.setBounds (left.removeFromTop (juce::jmin (208, left.getHeight() / 3)));
    left.removeFromTop (12);
    stemsHeader.setBounds (left.removeFromTop (18));
    left.removeFromTop (4);

    const int stripWidth = juce::jmin (58, (left.getWidth() - 3 * 6) / 4);

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

    // The chain list on the left, the editor tabs beside it.
    fxViewport.setBounds (b.removeFromLeft (280));
    b.removeFromLeft (8);
    editorTabs.setBounds (b);

    layoutFxPanels();
}

void MainComponent::selectFxForSnapshot()
{
    // By key rather than index: inserting a stage used to move this silently,
    // and the snapshot then documented whatever happened to be in that slot.
    for (int i = 0; i < (int) fxDefs.size(); ++i)
    {
        if (fxDefs[(size_t) i].key == "instrument")
        {
            selectFx (i);
            return;
        }
    }

    selectFx (1);
}

void MainComponent::showTunerForSnapshot()
{
    editorTabs.setCurrentTabIndex (1);
    tunerView.update (55.3f, 1.0f, 0.2f);   // a slightly sharp low A
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

    const int pitch = FxPanel::headerHeight + panelGap;
    int slot = 0;

    for (auto* panel : panels)
    {
        if (panel == draggedPanel)
            continue;   // it rides with the mouse

        if (slot == dragTargetSlot)
            ++slot;     // the hole where it would land

        panel->setBounds (0, slot * pitch, width, FxPanel::headerHeight);
        ++slot;
    }

    fxHolder.setSize (width, panels.size() * pitch);
}

void MainComponent::selectFx (int index)
{
    selectedFx = juce::isPositiveAndBelow (index, (int) fxDefs.size()) ? index : -1;

    for (int i = 0; i < panels.size(); ++i)
        panels[i]->setSelected (i == selectedFx);

    if (selectedFx >= 0)
    {
        const auto& def = fxDefs[(size_t) selectedFx];
        fxEditor.show (def.scope, def.name, def.ids,
                       def.key == "amp" ? (juce::Component*) &ampControls
                                        : (def.key == "instrument" ? (juce::Component*) &instrumentControls
                                                                   : nullptr));
    }
    else
    {
        fxEditor.clear();
    }
}

void MainComponent::renumberFxTitles()
{
    // The list order is the signal order — that is the information; numbers
    // on top of it were noise.
    for (int i = 0; i < panels.size(); ++i)
        panels[i]->setTitle (fxDefs[(size_t) i].name);
}

void MainComponent::pushOrderToChain()
{
    int order[FxChain::numMovableStages] {};
    int count = 0;

    for (const auto& def : fxDefs)
        if (def.movable && count < FxChain::numMovableStages)
            order[count++] = def.stage;

    if (count == FxChain::numMovableStages)
        engine.getFxChain().setStageOrder (order);
}

juce::String MainComponent::chainOrderString() const
{
    juce::StringArray keys;

    for (const auto& def : fxDefs)
        if (def.movable)
            keys.add (def.key);

    return keys.joinIntoString (",");
}

void MainComponent::applyChainOrder (const juce::String& saved)
{
    if (saved.isEmpty())
        return;

    std::vector<FxDef> movable;

    // Saved keys first, in their saved order...
    for (const auto& key : juce::StringArray::fromTokens (saved, ",", ""))
    {
        for (auto it = fxDefs.begin(); it != fxDefs.end(); ++it)
        {
            if (it->movable && it->key == key.trim())
            {
                movable.push_back (*it);
                fxDefs.erase (it);
                break;
            }
        }
    }

    // ...then any stage the saved list does not know about — one added since
    // it was written — at the end of the movable run.
    for (auto it = fxDefs.begin(); it != fxDefs.end();)
    {
        if (it->movable)
        {
            movable.push_back (*it);
            it = fxDefs.erase (it);
        }
        else
        {
            ++it;
        }
    }

    // Reassemble: input, the movable stages, reverb, live out.
    fxDefs.insert (fxDefs.begin() + 1, movable.begin(), movable.end());
}

// The same reorder, but with the panels already alive — a preset recalling
// the wiring it was saved with. An empty string (a preset from before order
// was captured) leaves the chain exactly as it is.
void MainComponent::applyChainOrderLive (const juce::String& saved)
{
    if (saved.isEmpty() || saved == chainOrderString())
        return;

    FxPanel* wasSelected = juce::isPositiveAndBelow (selectedFx, panels.size())
                               ? panels[selectedFx] : nullptr;

    // Panels are index-aligned with fxDefs; remember who belongs to which key
    // before the defs move.
    std::vector<std::pair<juce::String, FxPanel*>> byKey;

    for (int i = 0; i < panels.size(); ++i)
        byKey.emplace_back (fxDefs[(size_t) i].key, panels[i]);

    applyChainOrder (saved);

    for (int i = 0; i < (int) fxDefs.size(); ++i)
    {
        for (const auto& [key, panel] : byKey)
        {
            if (key == fxDefs[(size_t) i].key)
            {
                const int current = panels.indexOf (panel);

                if (current != i)
                    panels.move (current, i);

                break;
            }
        }
    }

    renumberFxTitles();
    pushOrderToChain();
    selectFx (wasSelected != nullptr ? panels.indexOf (wasSelected) : -1);
    layoutFxPanels();
    saveSettings();
}

void MainComponent::dragFxRow (FxPanel* panel, const juce::MouseEvent& e)
{
    if (draggedPanel != panel)
    {
        draggedPanel = panel;
        dragStartIndex = panels.indexOf (panel);
        dragTargetSlot = dragStartIndex;
        panel->toFront (false);
    }

    const int pitch = FxPanel::headerHeight + panelGap;
    const int first = 1;                       // below the input row
    const int last = panels.size() - 4;        // above reverb, live out, master

    const int y = juce::jlimit (first * pitch, last * pitch,
                                dragStartIndex * pitch + e.getDistanceFromDragStartY());

    panel->setTopLeftPosition (0, y);
    dragTargetSlot = juce::jlimit (first, last, (y + pitch / 2) / pitch);
    layoutFxPanels();
}

void MainComponent::dropFxRow (FxPanel* panel)
{
    if (draggedPanel != panel)
        return;

    const int from = dragStartIndex;
    const int to = dragTargetSlot;

    draggedPanel = nullptr;
    dragStartIndex = dragTargetSlot = -1;

    if (to != from && juce::isPositiveAndBelow (from, panels.size())
        && juce::isPositiveAndBelow (to, panels.size()))
    {
        FxPanel* wasSelected = juce::isPositiveAndBelow (selectedFx, panels.size())
                                   ? panels[selectedFx] : nullptr;

        panels.move (from, to);

        const FxDef moved = fxDefs[(size_t) from];
        fxDefs.erase (fxDefs.begin() + from);
        fxDefs.insert (fxDefs.begin() + to, moved);

        renumberFxTitles();
        pushOrderToChain();
        selectFx (wasSelected != nullptr ? panels.indexOf (wasSelected) : -1);
        saveSettings();
    }

    layoutFxPanels();
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

    // Space is the loop tap: the rhythm-critical gesture gets the biggest
    // key — IN on the downbeat, OUT at the end of the section, a third tap
    // lets it run on. L still answers for old muscle memory.
    if (key == juce::KeyPress::spaceKey
        || key.getTextCharacter() == 'l' || key.getTextCharacter() == 'L')
    {
        loopTapped();
        return true;
    }

    // Starting a song is not a timing gesture; return does fine.
    if (key == juce::KeyPress::returnKey)
    {
        engine.getStemPlayer().togglePlay();
        return true;
    }

    return false;
}

// One control, three taps: IN on the downbeat, OUT at the end of the section
// — the song now cycles it — and a third tap lets it run on. The tap is the
// loop point; there is nothing to drag.
void MainComponent::loopTapped()
{
    auto& player = engine.getStemPlayer();

    if (player.isLooping())
    {
        player.clearLoop();
        pendingLoopIn = -1;
    }
    else if (! player.isPlaying())
    {
        pendingLoopIn = -1;
    }
    else if (pendingLoopIn < 0)
    {
        pendingLoopIn = player.getPositionSamples();
    }
    else if (player.setLoop (pendingLoopIn, player.getPositionSamples()))
    {
        pendingLoopIn = -1;
    }

    refreshLoopButton();
}

void MainComponent::refreshLoopButton()
{
    auto& player = engine.getStemPlayer();
    const bool looping = player.isLooping();

    if (! player.isPlaying() && ! looping)
        pendingLoopIn = -1;

    loopButton.setToggleState (looping, juce::dontSendNotification);
    loopButton.setButtonText (looping ? "LOOPING"
                                      : (pendingLoopIn >= 0 ? "LOOP OUT" : "LOOP IN"));
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
    g.setFont (rigFont (13.0f));
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
    {
        applyChainOrderLive (presets.getChainOrder (name));
        applyAmpCapture (presets.getAmpCapture (name));
        refreshAllControls();
    }
}

void MainComponent::saveCurrentPreset()
{
    const auto name = presetBox.getText().trim();

    if (name.isEmpty())
        return;

    presets.store (name, params, chainOrderString(),
                   juce::var (engine.getFxChain().getAmp().getLoadedName()));
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

    fxEditor.refresh();

    for (auto* strip : stemStrips)
        strip->refresh();
}

//==============================================================================
// Import copies into the library first, so a capture picked from Downloads
// is still around after Downloads gets cleaned out. Multiple files import in
// one pass; the last one loads.
void MainComponent::chooseAmpCapture()
{
    juce::File start (settings.ampModel);
    start = start.existsAsFile() ? start.getParentDirectory()
                                 : juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                       .getChildFile ("Downloads");

    chooser = std::make_unique<juce::FileChooser> ("Import NAM captures", start, "*.nam");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc)
                          {
                              juce::File lastImported;
                              juce::StringArray problems;

                              for (const auto& file : fc.getResults())
                              {
                                  juce::String error;
                                  const auto imported = AmpModel::importToLibrary (file, error);

                                  if (imported != juce::File())
                                      lastImported = imported;
                                  else
                                      problems.add (error);
                              }

                              if (lastImported != juce::File())
                                  loadAmpFile (lastImported);

                              if (! problems.isEmpty())
                                  juce::AlertWindow::showMessageBoxAsync (
                                      juce::MessageBoxIconType::WarningIcon, "NAM capture",
                                      problems.joinIntoString ("\n"));

                              refreshAmpControls();
                              grabKeyboardFocus();
                          });
}

void MainComponent::loadAmpFile (const juce::File& file)
{
    juce::String error;

    if (engine.getFxChain().getAmp().load (file, error))
    {
        settings.ampModel = file.getFullPathName();
        params.set (pAmpOn, 1.0f);   // loading one means you want to hear it
        refreshAllControls();
        saveSettings();
    }
    else
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "NAM capture", error);
    }

    refreshAmpControls();
}

// A preset's stored capture: void leaves the amp alone (the preset predates
// captures), an empty string means it was saved silent, a name loads from
// the library.
void MainComponent::applyAmpCapture (const juce::var& captureName)
{
    if (captureName.isVoid())
        return;

    const auto name = captureName.toString();

    if (name.isEmpty())
    {
        engine.getFxChain().getAmp().clearModel();
        settings.ampModel.clear();
        refreshAmpControls();
        saveSettings();
        return;
    }

    if (name == engine.getFxChain().getAmp().getLoadedName())
        return;

    const auto file = AmpModel::libraryFileForName (name);

    if (file.existsAsFile())
        loadAmpFile (file);
    else
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "NAM capture",
                                                "\"" + name + "\" is not in the capture library.");
}

void MainComponent::refreshInstrumentControls()
{
    auto& host = engine.getInstrument();
    auto& box = instrumentControls.instrumentBox;

    box.clear (juce::dontSendNotification);

    box.setTextWhenNothingSelected (host.isScanning() ? "Scanning..." : "No instrument");

    int id = 1, selected = 0;

    for (const auto& name : host.getAvailableNames())
    {
        box.addItem (name, id);

        if (host.isLoaded() && name == host.getLoadedName())
            selected = id;

        ++id;
    }

    box.setSelectedId (selected, juce::dontSendNotification);
    instrumentControls.clear.setEnabled (host.isLoaded());
    instrumentControls.open.setEnabled (host.isLoaded());
}

void MainComponent::openPluginWindow()
{
    auto* plugin = engine.getInstrument().getLiveInstrument();

    if (plugin == nullptr)
        return;

    if (pluginWindow != nullptr)
    {
        pluginWindow->toFront (true);
        return;
    }

    auto* editor = plugin->createEditorIfNeeded();

    if (editor == nullptr)
        return;

    struct PluginWindow : juce::DocumentWindow
    {
        PluginWindow (const juce::String& title, juce::AudioProcessorEditor* content,
                      std::unique_ptr<juce::DocumentWindow>& slot)
            : juce::DocumentWindow (title, juce::Colour (0xff17181c), closeButton), owner (slot)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (content, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { owner.reset(); }

        std::unique_ptr<juce::DocumentWindow>& owner;
    };

    pluginWindow = std::make_unique<PluginWindow> (engine.getInstrument().getLoadedName(),
                                                   editor, pluginWindow);
}

void MainComponent::refreshAmpControls()
{
    auto& ampStage = engine.getFxChain().getAmp();
    auto& box = ampControls.captureBox;

    box.clear (juce::dontSendNotification);

    int id = 1, selected = 0;

    for (const auto& file : AmpModel::listLibrary())
    {
        const auto stem = file.getFileNameWithoutExtension();
        box.addItem (stem, id);

        if (ampStage.isLoaded() && stem == ampStage.getLoadedName())
            selected = id;

        ++id;
    }

    box.setSelectedId (selected, juce::dontSendNotification);
    ampControls.clear.setEnabled (ampStage.isLoaded());
}

//==============================================================================
// The settings field kept its name from the expanding-panels days; it now
// carries the single selected row as a one-hot string, and an old multi-open
// state degrades gracefully to "the first panel you had open".
juce::String MainComponent::expandedStateString() const
{
    juce::String state;

    for (int i = 0; i < panels.size(); ++i)
        state << (i == selectedFx ? '1' : '0');

    return state;
}

void MainComponent::applyExpandedState (const juce::String& state)
{
    selectFx (state.indexOfChar ('1'));
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
    settings.instrument = engine.getInstrument().getLoadedName();
    settings.instrumentState = engine.getInstrument().getStateBase64();
    settings.chainOrder = chainOrderString();
    settings.tuner = tunerView.isBassMode() ? "bass" : "guitar";
    settings.save (params);
}

void MainComponent::timerCallback()
{
    // Retired NAM models come back from the audio thread to be freed here.
    engine.getFxChain().getAmp().collectGarbage();

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

    if (instrumentScanning != engine.getInstrument().isScanning())
    {
        instrumentScanning = engine.getInstrument().isScanning();
        refreshInstrumentControls();
    }

    midiStatus.setNote (engine.getFxChain().getSoundingNote());
    engine.getMidi().setChannel ((int) std::lround (params.get (pMidiChannel)));
    engine.getMidi().setExpressionController ((int) std::lround (params.get (pMidiExpression)));

    inputMeter.setLevel (0, engine.getInputPeak());
    masterMeter.setLevel (0, engine.getOutputPeakL());
    masterMeter.setLevel (1, engine.getOutputPeakR());

    if (editorTabs.getCurrentTabIndex() == 1)
        tunerView.update (engine.getFxChain().getTrackedHz(),
                          engine.getFxChain().getTrackedConfidence(),
                          engine.getInputPeak());

    // Working limiter = the sum would have clipped by this much.
    const float gr = engine.getLimiterReductionDb();
    grLabel.setText (gr > 0.2f ? "LIM -" + juce::String (gr, 1) + " dB" : juce::String(),
                     juce::dontSendNotification);

    const bool playing = player.isPlaying();
    transportButton.setToggleState (playing, juce::dontSendNotification);
    transportButton.setButtonText (playing ? "STOP" : "PLAY");
    refreshLoopButton();

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

    const float ioMs = engine.getIoLatencyMs();
    const float fxMs = engine.getFxLatencyMs();

    latencyLabel.setText ("Latency  " + juce::String (ioMs, 1) + " ms I/O  +  "
                            + juce::String (fxMs, 1) + " ms FX",
                          juce::dontSendNotification);

    if (++saveCounter >= 90)   // every three seconds, if anything moved
    {
        saveCounter = 0;

        if (paramsChangedSinceSnapshot())
            saveSettings();
    }
}

} // namespace rig
