#include "MainComponent.h"

#include <CoreAudio/CoreAudio.h>

namespace rig
{

// Which sub-device owns each input channel of the named device — empty for
// anything that is not an aggregate, and when CoreAudio won't say. An
// aggregate's channels run sub-device by sub-device in its list order, so
// "Input 1" can introduce itself as the Scarlett's and "Front Left" as the
// USB mic's.
static juce::StringArray inputChannelOwners (const juce::String& deviceName, int channelCount)
{
    juce::StringArray owners;

    AudioObjectPropertyAddress addr { kAudioHardwarePropertyDevices,
                                      kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    UInt32 size = 0;

    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr || size == 0)
        return owners;

    std::vector<AudioDeviceID> devices (size / sizeof (AudioDeviceID));

    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, &size, devices.data()) != noErr)
        return owners;

    const auto nameOf = [] (AudioObjectID id) -> juce::String
    {
        CFStringRef cf = nullptr;
        UInt32 sz = sizeof (cf);
        AudioObjectPropertyAddress a { kAudioObjectPropertyName,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain };

        if (AudioObjectGetPropertyData (id, &a, 0, nullptr, &sz, &cf) != noErr || cf == nullptr)
            return {};

        auto s = juce::String::fromCFString (cf);
        CFRelease (cf);
        return s;
    };

    AudioDeviceID aggregate = kAudioObjectUnknown;

    for (auto id : devices)
    {
        if (nameOf (id) == deviceName)
        {
            aggregate = id;
            break;
        }
    }

    if (aggregate == kAudioObjectUnknown)
        return owners;

    AudioObjectPropertyAddress subAddr { kAudioAggregateDevicePropertyActiveSubDeviceList,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };

    if (AudioObjectGetPropertyDataSize (aggregate, &subAddr, 0, nullptr, &size) != noErr || size == 0)
        return owners;

    std::vector<AudioDeviceID> subs (size / sizeof (AudioDeviceID));

    if (AudioObjectGetPropertyData (aggregate, &subAddr, 0, nullptr, &size, subs.data()) != noErr)
        return owners;

    const auto inputCountOf = [] (AudioDeviceID id) -> int
    {
        AudioObjectPropertyAddress a { kAudioDevicePropertyStreamConfiguration,
                                       kAudioDevicePropertyScopeInput,
                                       kAudioObjectPropertyElementMain };
        UInt32 sz = 0;

        if (AudioObjectGetPropertyDataSize (id, &a, 0, nullptr, &sz) != noErr || sz == 0)
            return 0;

        std::vector<char> raw (sz);
        auto* list = reinterpret_cast<AudioBufferList*> (raw.data());

        if (AudioObjectGetPropertyData (id, &a, 0, nullptr, &sz, list) != noErr)
            return 0;

        int n = 0;

        for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
            n += (int) list->mBuffers[i].mNumberChannels;

        return n;
    };

    for (auto sub : subs)
    {
        const auto owner = nameOf (sub);

        for (int n = inputCountOf (sub); n > 0; --n)
            owners.add (owner);
    }

    if (owners.size() != channelCount)
        owners.clear();   // the map does not line up; no labels beats wrong labels

    return owners;
}

static constexpr int panelGap = 7;

MainComponent::MainComponent (ParamStore& p, AudioEngine& e, SettingsStore& s)
    : params (p), engine (e), settings (s)
{
    presets.load();

    // The engine already runs; tell it which channels the lanes live on,
    // and whose hands play the keys.
    engine.setInputRoles (settings.bassInput, settings.micInput);
    engine.setMidiSource (settings.midiSource);

    for (auto* b : { &menuButton, &loopButton,
                     &deviceButton, &savePresetButton, &deletePresetButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    transportButton.setWantsKeyboardFocus (false);
    addAndMakeVisible (transportButton);
    transportButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3f9d4a));
    loopButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd87b3a));

    // The file actions live behind the menu, and the menu lives with the songs it acts on.
    menuButton.onClick = [this]
    {
        juce::PopupMenu menu;
        menu.addItem ("Load folder...", [this] { chooseSongFolder(); });
        menu.addItem ("New song...", [this] { startNewSong(); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (menuButton));
    };

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

    songList.setRowHeight (28);
    songList.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff17181c));
    songList.setColour (juce::ListBox::outlineColourId, juce::Colour (0xff26272d));
    songList.setOutlineThickness (1);
    addAndMakeVisible (songList);

    presetBox.setEditableText (true);
    presetBox.onChange = [this] { applySelectedPreset(); };
    addAndMakeVisible (presetBox);

    savePresetButton.onClick = [this] { saveCurrentPreset(); };
    deletePresetButton.onClick = [this] { deleteSelectedPreset(); };

    for (int i = 0; i < StemPlayer::numStems; ++i)
        addAndMakeVisible (stemStrips.add (new StemStrip (uiParams,
                                                          juce::String (StemPlayer::stemNames[i]).toUpperCase(),
                                                          pStemGain0 + i,
                                                          pStemMute0 + i)));

    // The whole song's volume stands at the end of the stem row — the mix
    // board's TOTAL fader, not a rider under the transport.
    addAndMakeVisible (stemStrips.add (new StemStrip (uiParams, "TOTAL", pTrackGain, -1)));

    fxDefs = {
        { "input",  -1,                false, "",       "Input",      pInMute,   { pInGain } },
        { "vocal",  -1,                false, "",       "Autotune",   pVocalOn,  { pVocalGain, pVocalGate, pVocalLevel } },
        { "vocoder",-1,                false, "",       "Vocoder",    pVocoderOn,{ pVocalGain, pVocalGate, pVocoderLevel } },
        { "keys",   -1,                false, "keys",   "Keys",       pInstOn,   { pInstLevel } },
        { "midi",   -1,                false, "midi",   "MIDI",       pMidiOn,   { pMidiChannel, pMidiTranspose, pMidiGate, pMidiHold, pMidiRelease, pMidiBendRange, pMidiExpression, pMidiThru } },
        { "comp",   FxChain::stComp,   true,  "comp",   "Compressor", pCompOn,   { pCompThresh, pCompRatio, pCompAttack, pCompRelease, pCompMakeup, pCompBlend, pCompSidechain } },
        { "eq",     FxChain::stEq,     true,  "eq",     "EQ",         pEqOn,     { pEq1Freq, pEq1Gain, pEq1Q, pEq2Freq, pEq2Gain, pEq2Q, pEq3Freq, pEq3Gain, pEq3Q, pEq4Freq, pEq4Gain, pEq4Q, pEq5Freq, pEq5Gain, pEq5Q } },
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
        { "freeze", FxChain::stFreeze, true,  "freeze", "Freeze",     pFreezeOn, { pFreezeLevel, pFreezeFadeIn, pFreezeFadeOut, pFreezeMotion } },
        { "reverb", -1,                false, "reverb", "Reverb",     pRevOn,    { pRevSize, pRevDamp, pRevPre, pRevMod, pRevMix } },
        { "out",    -1,                false, "",       "Live out",   pLiveMute, { pLiveOut } },
        { "master", -1,                false, "",       "Master",     pMasterMute,{ pMasterGain } },
    };

    for (const auto& def : fxDefs)
    {
        // The meters live on the channel strips now; only the MIDI row keeps
        // an extra — the note readout and PANIC.
        //
        // A mute is not a bypass: the input, live out and master rows wear the
        // red MUTE cap, and lit means silenced.
        const bool muteRow = def.onParam == pInMute || def.onParam == pLiveMute
                               || def.onParam == pMasterMute;

        auto* panel = panels.add (new FxPanel (uiParams, def.name, def.onParam, def.movable,
                                               def.key == "midi" ? &midiStatus : nullptr, 84,
                                               muteRow));

        // The freeze's switch is the hold itself, and says so.
        if (isMomentaryParam (def.onParam))
            panel->setSwitchText ("HOLD");

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

        if (isLoadable (def))
        {
            panel->onContextMenu = [this, panel]
            {
                const int i = panels.indexOf (panel);

                if (! juce::isPositiveAndBelow (i, (int) fxDefs.size()))
                    return;

                juce::PopupMenu m;
                m.addItem ("Remove " + fxDefs[(size_t) i].name,
                           [this, key = fxDefs[(size_t) i].key] { removeFxFromChannel (key); });
                m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (panel));
            };

            panel->setRemovable (true);
        }

        fxHolder.addAndMakeVisible (panel);
    }

    renumberFxTitles();

    // The rail: strips come from the settings' channel list, and the player
    // grows or shrinks it by hand. The + at the end offers what can be added.
    addChannelButton.setWantsKeyboardFocus (false);
    addChannelButton.onClick = [this] { showAddChannelMenu(); };
    addAndMakeVisible (addChannelButton);

    rebuildChannelRail();
    refreshChainWiring();

    addFxButton.setWantsKeyboardFocus (false);
    addFxButton.onClick = [this] { showAddFxMenu(); };
    fxHolder.addAndMakeVisible (addFxButton);

    fxViewport.setViewedComponent (&fxHolder, false);
    fxViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (fxViewport);

    editorTabs.setTabBarDepth (30);
    editorTabs.setOutline (0);
    editorTabs.addTab ("EFFECT", juce::Colour (0xff101114), &fxEditor, false);
    editorTabs.addTab ("TUNER", juce::Colour (0xff101114), &tunerView, false);
    editorTabs.addTab ("FOOT", juce::Colour (0xff101114), &footView, false);
    addAndMakeVisible (editorTabs);

    // The foot tab asks the window what the rig can do and what to call it;
    // the map it edits is kept beside the settings. Commands from a foot
    // arrive on the MIDI thread and are carried out here, on this one.
    footView.buildFunctionMenu = [this] (juce::PopupMenu& menu, const FootView::Chooser& choose)
    {
        buildFootMenu (menu, choose);
    };
    footView.describeAction = [this] (const juce::String& action) { return describeFootAction (action); };
    footView.onEdited = [this] { engine.getFoot().getMap().saveTo (FootMap::getFile()); };
    footView.hiddenDevices.add (MidiSender::portName);
    footView.reload();   // the rail exists now, so the jobs can be named
    engine.getFoot().setCommandNotifier ([this] { triggerAsyncUpdate(); });
    fxEditor.clear();

    tunerView.setBassMode (settings.tuner != "guitar");
    tunerView.onModeChanged = [this] { saveSettings(); };

    midiStatus.onPanic = [this] { engine.getMidi().panic(); };

    // The cached list is on disk, so the pickers fill the moment the window
    // opens. Only scan when there is nothing cached — identifying an Audio
    // Unit means instantiating it, and paying that on every launch to
    // rediscover the same plugins is not a trade worth making. An old cache
    // from before effects were recorded reads as an empty effect list, which
    // triggers the rescan that upgrades it.
    if (engine.getInstrument().getAvailableNames().isEmpty()
         || engine.getInstrument().getEffectNames().isEmpty())
        engine.getInstrument().beginScan();

    instrumentControls.instrumentBox.onChange = [this]
    {
        const auto name = instrumentControls.instrumentBox.getText();

        if (name.isEmpty() || name == engine.getInstrument().getLoadedName())
            return;

        // The window may be showing the editor of the instance about to be
        // retired; close it before the ground moves.
        pluginWindow.reset();

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

    instrumentControls.rescan.onClick = [this]
    {
        engine.getInstrument().beginScan();
        refreshInstrumentControls();
    };

    // Restore first: the picker and its buttons describe a loaded
    // instrument, and describing one that has not arrived yet is how the row
    // came back empty over a sound that was already playing.
    if (settings.instrument.isNotEmpty())
        engine.getInstrument().restore (settings.instrument, settings.instrumentState);

    refreshInstrumentControls();

    autotuneControls.open.onClick = [this]
    {
        openPluginWindowFor (engine.getTuneSlot().getLiveProcessor(),
                             engine.getTuneSlot().getLoadedName());
    };

    vocoderControls.open.onClick = [this]
    {
        openPluginWindowFor (engine.getVocoderSlot().getLiveProcessor(),
                             engine.getVocoderSlot().getLoadedName());
    };

    engine.getTuneSlot().restore (engine.getInstrument(), settings.tunePlugin, settings.tunePluginState);
    engine.getVocoderSlot().restore (engine.getInstrument(), settings.vocoderPlugin, settings.vocoderPluginState);
    autoBindHostedRoles();
    refreshSlotControls();

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
    engine.getFoot().setCommandNotifier (nullptr);
    cancelPendingUpdate();
    stopTimer();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff101114));

    // The songs section sits in its own card.
    g.setColour (juce::Colour (0xff17181c));
    g.fillRoundedRectangle (songsBox.toFloat(), 6.0f);
    g.setColour (juce::Colour (0xff26272d));
    g.drawRoundedRectangle (songsBox.toFloat().reduced (0.5f), 6.0f, 1.0f);
}

void MainComponent::resized()
{
    songPanel.setBounds (getLocalBounds());

    auto b = getLocalBounds().reduced (10);

    if (jobStripShowing)
    {
        jobStrip.setBounds (b.removeFromTop (32));
        b.removeFromTop (8);
    }

    auto left = b.removeFromLeft (300);

    deviceButton.setBounds (left.removeFromTop (26).removeFromLeft (132));
    left.removeFromTop (8);

    // The stems keep a short, fixed footprint at the bottom; the songs card
    // above them takes every row the window can spare for the setlist.
    auto stemsArea = left.removeFromBottom (212);

    stemsHeader.setBounds (stemsArea.removeFromTop (18));
    stemsArea.removeFromTop (4);

    const int stripWidth = juce::jmin (58, (stemsArea.getWidth() - (stemStrips.size() - 1) * 6)
                                             / juce::jmax (1, stemStrips.size()));

    for (auto* strip : stemStrips)
    {
        strip->setBounds (stemsArea.removeFromLeft (stripWidth));
        stemsArea.removeFromLeft (6);
    }

    left.removeFromBottom (12);
    songsBox = left;

    auto box = songsBox.reduced (10);

    auto songsHead = box.removeFromTop (26);
    menuButton.setBounds (songsHead.removeFromRight (36).reduced (0, 1));
    songsHead.removeFromRight (8);
    songsHeader.setBounds (songsHead);
    box.removeFromTop (4);
    songLabel.setBounds (box.removeFromTop (22));
    box.removeFromTop (6);

    // The transport is the loaded song's: it lives with the setlist.
    auto transportRow = box.removeFromTop (28);
    transportButton.setBounds (transportRow.removeFromLeft (44).reduced (0, 1));
    transportRow.removeFromLeft (6);
    loopButton.setBounds (transportRow.removeFromLeft (92).reduced (0, 1));
    box.removeFromTop (8);
    songList.setBounds (box);

    b.removeFromLeft (10);

    // The channel rail first; the preset bar sits INSIDE the channel scope,
    // because a preset is one channel's, not the whole window's.
    auto rail = b.removeFromTop (ChannelStrip::preferredHeight);
    const int stripGap = 8;
    const int plusWidth = 36;
    const int count = juce::jmax (1, channelStrips.size());
    const int stripWidth2 = juce::jmin (166, (rail.getWidth() - plusWidth - stripGap * count) / count);

    for (auto* strip : channelStrips)
    {
        strip->setBounds (rail.removeFromLeft (stripWidth2));
        rail.removeFromLeft (stripGap);
    }

    addChannelButton.setBounds (rail.removeFromLeft (plusWidth).reduced (0, 12));

    b.removeFromTop (8);

    auto presetBar = b.removeFromTop (30);
    deletePresetButton.setBounds (presetBar.removeFromRight (72));
    presetBar.removeFromRight (6);
    savePresetButton.setBounds (presetBar.removeFromRight (66));
    presetBar.removeFromRight (8);
    presetBox.setBounds (presetBar);

    b.removeFromTop (8);

    // The chain list on the left — narrow, since caps titles and the dot
    // grip are all a row carries — and the editor tabs beside it.
    fxViewport.setBounds (b.removeFromLeft (268));
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
        if (fxDefs[(size_t) i].key == "midi")
        {
            selectFx (i);
            return;
        }
    }

    selectFx (1);
}

void MainComponent::selectInputForSnapshot()
{
    for (int i = 0; i < (int) fxDefs.size(); ++i)
    {
        if (fxDefs[(size_t) i].key == "input")
        {
            selectFx (i);
            return;
        }
    }
}

void MainComponent::selectEqForSnapshot()
{
    for (int i = 0; i < (int) fxDefs.size(); ++i)
    {
        if (fxDefs[(size_t) i].key == "eq")
        {
            selectFx (i);
            return;
        }
    }
}

void MainComponent::selectFreezeForSnapshot()
{
    for (int i = 0; i < (int) fxDefs.size(); ++i)
    {
        if (fxDefs[(size_t) i].key == "freeze")
        {
            selectFx (i);
            return;
        }
    }
}

void MainComponent::selectVocalForSnapshot()
{
    for (int i = 0; i < (int) fxDefs.size(); ++i)
    {
        if (fxDefs[(size_t) i].key == "vocal")
        {
            selectFx (i);
            return;
        }
    }
}

void MainComponent::selectKeysForSnapshot()
{
    for (int i = 0; i < (int) fxDefs.size(); ++i)
    {
        if (fxDefs[(size_t) i].key == "keys")
        {
            selectFx (i);
            return;
        }
    }
}

void MainComponent::selectVocoderForSnapshot()
{
    for (int i = 0; i < (int) fxDefs.size(); ++i)
    {
        if (fxDefs[(size_t) i].key == "vocoder")
        {
            selectFx (i);
            return;
        }
    }
}

void MainComponent::showTunerForSnapshot()
{
    editorTabs.setCurrentTabIndex (1);
    tunerView.update (55.3f, 1.0f, 0.2f);   // a slightly sharp low A
}

// The foot tab with a Ground Control's worth of things on it: a preset
// button, a lit instant-access switch and a volume pedal, mid-press.
void MainComponent::showFootForSnapshot()
{
    FootMap map;

    const auto control = [&map] (const char* name, FootTrigger::Type type, int channel, int number,
                                 FootControl::Kind kind)
    {
        FootControl c;
        c.id = map.freshControlId();
        c.name = name;
        c.trigger = { type, channel, number };
        c.kind = kind;
        map.controls.push_back (c);
        return c.id;
    };

    const auto verse = control ("Verse", FootTrigger::program, 1, 0, FootControl::preset);
    const auto dirt = control ("Dirt", FootTrigger::controller, 16, 80, FootControl::latch);
    const auto wah = control ("Volume pedal", FootTrigger::controller, 1, 7, FootControl::sweep);
    const auto go = control ("Start", FootTrigger::start, 0, 0, FootControl::preset);

    FootLayout layout;
    layout.name = "Set one";
    layout.jobs = {
        { verse, "bass/odOn",    FootJob::off,    0.0f, 1.0f },
        { verse, "bass/chOn",    FootJob::on,     0.0f, 1.0f },
        { verse, "bass/dlMix",   FootJob::follow, 0.25f, 0.25f },
        { dirt,  "bass/odOn",    FootJob::follow, 0.0f, 1.0f },
        { wah,   "bass/dlMix",   FootJob::follow, 0.1f, 0.6f },
        { wah,   "mic/vocoderLvl", FootJob::follow, 0.0f, -24.0f },
        { go,    "do/play",      FootJob::follow, 0.0f, 1.0f },
    };

    map.layouts = { layout, { "Set two", {} } };
    map.active = layout.name;

    engine.getFoot().setMap (map);
    footView.reload();
    engine.getFoot().handleMidi ("Scarlett 4i4 USB", juce::MidiMessage::controllerEvent (1, 7, 90));
    engine.getFoot().handleMidi ("Scarlett 4i4 USB", juce::MidiMessage::controllerEvent (16, 80, 127));
    footView.tick();
    editorTabs.setCurrentTabIndex (2);
}

// Every function the foot menu offers must be one the engine can carry out
// and the tab can name; a menu item that does nothing is a dead switch on
// stage. Returns how many are not.
int MainComponent::checkFootFunctionsForSnapshot()
{
    juce::StringArray actions;
    juce::PopupMenu menu;
    buildFootMenu (menu, [&actions] (const juce::String& action) { actions.add (action); });

    for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next();)
        if (auto& item = it.getItem(); item.action != nullptr && item.subMenu == nullptr)
            item.action();

    int problems = actions.isEmpty() ? 1 : 0;

    for (const auto& action : actions)
    {
        FootParamTarget target;
        const bool runs = isFootCommand (action) || parseFootParamAction (action, target);

        if (! runs || describeFootAction (action) == action)
        {
            std::printf ("  DEAD FOOT FUNCTION %s\n", action.toRawUTF8());
            ++problems;
        }
    }

    std::printf ("  foot menu offers %d functions\n", actions.size());
    return problems;
}

// Anything that can be taken away must show the way to take it: every pedal
// row on every strip's board, and every strip but the master. And nothing
// that cannot leave may wear the button. Returns how many get it wrong.
int MainComponent::checkRemoveButtonsForSnapshot()
{
    int problems = 0, pedals = 0, strips = 0;
    const auto startedOn = selectedChannel;

    for (auto* strip : channelStrips)
    {
        const auto key = strip->getComponentID();
        const bool canLeave = key != "master";

        if (strip->hasRemoveButton() != canLeave)
        {
            std::printf ("  REMOVE BUTTON wrong on the %s strip\n", key.toRawUTF8());
            ++problems;
        }

        strips += canLeave ? 1 : 0;
        selectChannel (key);

        for (int i = 0; i < panels.size(); ++i)
        {
            if (! panels[i]->isVisible())
                continue;

            const bool loadable = isLoadable (fxDefs[(size_t) i]);

            if (panels[i]->hasRemoveButton() != loadable)
            {
                std::printf ("  REMOVE BUTTON wrong on %s / %s\n", key.toRawUTF8(),
                             fxDefs[(size_t) i].key.toRawUTF8());
                ++problems;
            }

            pedals += loadable ? 1 : 0;
        }
    }

    selectChannel (startedOn);
    std::printf ("  remove buttons on %d pedals and %d strips\n", pedals, strips);
    return problems + (pedals == 0 || strips == 0 ? 1 : 0);
}

// The foot tab used the way a player would use it, with no mouse and no
// hardware: learn a switch, give it jobs, press it, turn it into a pedal,
// learn it again as something else, copy the layout, switch layouts by
// foot, take it all away. Nothing is written to the real map file. Returns
// how many things did not happen.
int MainComponent::exerciseFootForSnapshot()
{
    auto& foot = engine.getFoot();
    const auto cc = [] (int ch, int n, int v) { return juce::MidiMessage::controllerEvent (ch, n, v); };

    int problems = 0, kept = 0;
    const auto expect = [&problems] (bool ok, const char* what)
    {
        if (! ok)
        {
            std::printf ("  FOOT TAB: %s\n", what);
            ++problems;
        }
    };

    auto keeper = std::move (footView.onEdited);
    footView.onEdited = [&kept] { ++kept; };

    foot.setMap (FootMap::factory());
    footView.reload();
    const auto& map = footView.getMap();

    footView.learnTapped ({});
    foot.handleMidi ("box", cc (16, 80, 127), 0);
    footView.tick();
    expect (map.controls.size() == 4 && map.controls.back().kind == FootControl::latch
              && map.controls.back().name == "Switch 80" && ! foot.isLearning(),
            "a learned switch did not become a card");

    footView.learnTapped ({});
    foot.handleMidi ("box", cc (16, 80, 0), 0);
    footView.tick();
    expect (map.controls.size() == 4, "the same wire learned twice made a second card");

    const auto id = map.controls.back().id;
    const int base = (int) map.activeLayout()->jobs.size();

    params.set (pDriveOn, 0.0f);
    footView.addJob (id, "bass/odOn");
    foot.handleMidi ("box", cc (16, 80, 127));
    expect (params.getBool (pDriveOn), "a new switch job did not follow the press");

    footView.setVerb (base, FootJob::off);
    foot.handleMidi ("box", cc (16, 80, 0));
    expect (! params.getBool (pDriveOn), "Turns off did not turn off");

    footView.setKind (id, FootControl::sweep);
    footView.addJob (id, "bass/dlMix");
    foot.handleMidi ("box", cc (16, 80, 127));
    expect (params.get (pDelayMix) == spec (pDelayMix).maxValue, "a new knob job did not sweep its whole travel");

    footView.setRange (base + 1, 0.2f, 0.4f);
    footView.swapEnds (base + 1);
    foot.handleMidi ("box", cc (16, 80, 0));
    expect (std::abs (params.get (pDelayMix) - 0.4f) < 1.0e-5f, "swapped ends did not swap");

    footView.learnTapped (id);
    foot.handleMidi ("box", juce::MidiMessage::programChange (1, 5), 0);
    footView.tick();
    expect (map.controls.size() == 4 && map.controls.back().trigger.type == FootTrigger::program
              && map.controls.back().kind == FootControl::preset
              && (int) map.activeLayout()->jobs.size() == base + 2,
            "learning a control again lost it or its jobs");

    footView.addJob (id, "do/layout:Default 2");
    footView.copyLayout();
    expect (map.layouts.size() == 2 && map.active == "Default 2"
              && foot.getActiveLayout() == "Default 2"
              && map.activeLayout()->jobs.size() == map.layouts.front().jobs.size(),
            "copying a layout did not copy it");

    footView.removeJob (base);
    expect (map.layouts.front().jobs.size() == map.activeLayout()->jobs.size() + 1,
            "removing a job reached into another layout");

    runFootCommand ("do/layout:Default");
    expect (map.active == "Default" && foot.getActiveLayout() == "Default", "a foot could not switch layouts");

    foot.handleMidi ("box", juce::MidiMessage::programChange (1, 5));
    handleAsyncUpdate();
    expect (map.active == "Default 2", "the switch-layout job did not carry through");

    footView.deleteLayout ("Default 2");
    expect (map.layouts.size() == 1 && map.active == "Default"
              && (int) map.activeLayout()->jobs.size() == base + 2,
            "deleting a layout left it, or left a job pointing at it");

    // Taking a job back off, by the button a player can see on its row.
    {
        const int before = (int) map.activeLayout()->jobs.size();
        footView.addJob (id, "bass/chOn");
        expect ((int) map.activeLayout()->jobs.size() == before + 1 && footView.clickRemoveOn (before)
                  && (int) map.activeLayout()->jobs.size() == before,
                "the remove button on a job's row is missing, covered, or did not remove the job");
        expect (footView.everyCardHasItsMenu(), "a card has no visible way to its Learn again / Remove menu");
    }

    footView.renameControl (id, "Chorus scene");
    footView.removeControl (id);
    expect (map.controls.size() == 3 && (int) map.activeLayout()->jobs.size() == base,
            "removing a control left its jobs behind");

    expect (kept > 0 && juce::JSON::toString (foot.getMap().toVar(), false)
                          == juce::JSON::toString (map.toVar(), false),
            "the engine and the tab disagree about the map");

    footView.onEdited = std::move (keeper);
    std::printf ("  foot tab exercised: %d edits kept\n", kept);
    return problems;
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
    const auto visible = visibleFxIndices();

    for (int i = 0; i < panels.size(); ++i)
        panels[i]->setVisible (visible.contains (i));

    int slot = 0;

    for (int index : visible)
    {
        auto* panel = panels[index];

        if (panel == draggedPanel)
            continue;   // it rides with the mouse

        if (slot == dragTargetSlot)
            ++slot;     // the hole where it would land

        panel->setBounds (0, slot * pitch, width, FxPanel::headerHeight);
        ++slot;
    }

    // The board ends with the offer of more, when the catalog has more.
    const bool offerAdd = ! availableFxIndices().isEmpty();
    addFxButton.setVisible (offerAdd);

    if (offerAdd)
        addFxButton.setBounds (0, slot * pitch, width, FxPanel::headerHeight);

    fxHolder.setSize (width, (visible.size() + (offerAdd ? 1 : 0)) * pitch);
}

// Which strip a chain row belongs to. Input, the movable stages, reverb and
// live out are the instrument channel; the mic lanes and the keys lane are
// their own; master is the sum.
juce::String MainComponent::channelOfKey (const juce::String& key)
{
    if (key == "vocal" || key == "vocoder")
        return "mic";

    if (key == "keys")
        return "keys";

    if (key == "master")
        return "master";

    return "bass";
}

// The one list that decides what a strip shows, in what order: the strip's
// anatomy (input at the head, live out at the tail), its lanes when loaded
// here, and the movable run in the board's own sequence with reverb holding
// the tail. The master strip shows only its own row.
juce::Array<int> MainComponent::visibleFxIndices() const
{
    return visibleFxIndicesFor (selectedChannel);
}

juce::Array<int> MainComponent::visibleFxIndicesFor (const juce::String& channelKey) const
{
    juce::Array<int> visible;

    auto indexOfKey = [this] (const juce::String& key) -> int
    {
        for (int i = 0; i < (int) fxDefs.size(); ++i)
            if (fxDefs[(size_t) i].key == key)
                return i;

        return -1;
    };

    if (channelKey == "master")
    {
        visible.add (indexOfKey ("master"));
        return visible;
    }

    auto loaded = juce::StringArray::fromTokens (loadoutFor (channelKey), ",", "");
    loaded.trim();

    visible.add (indexOfKey ("input"));

    for (const auto* laneKey : { "vocal", "vocoder", "midi", "keys" })
        if (channelOfKey (laneKey) == channelKey && loaded.contains (laneKey))
            visible.add (indexOfKey (laneKey));

    for (const auto& key : loaded)
    {
        const int i = indexOfKey (key);

        if (i >= 0 && fxDefs[(size_t) i].movable && ! visible.contains (i))
            visible.add (i);
    }

    if (loaded.contains ("reverb"))
        visible.add (indexOfKey ("reverb"));

    visible.add (indexOfKey ("out"));
    return visible;
}

juce::String MainComponent::loadoutFor (const juce::String& channel) const
{
    if (channel == "bass") return settings.bassLoadout;
    if (channel == "mic")  return settings.micLoadout;
    if (channel == "keys") return settings.keysLoadout;

    if (channel.startsWith ("ch"))
        return genericField (channel, 1);

    return {};
}

void MainComponent::setLoadout (const juce::String& channel, const juce::String& value)
{
    if      (channel == "bass") settings.bassLoadout = value;
    else if (channel == "mic")  settings.micLoadout = value;
    else if (channel == "keys") settings.keysLoadout = value;
    else if (channel.startsWith ("ch")) setGenericField (channel, 1, value);
}

// The generic channels' names, boards and jacks live in one settings
// string, "ch4=Name;comp,eq;3|ch5=;;0" — field 0 the name, field 1 the
// loadout, field 2 the 1-based input channel. Split by hand so an empty
// field holds its place.
juce::String MainComponent::genericField (const juce::String& key, int index) const
{
    for (const auto& entry : juce::StringArray::fromTokens (settings.extraChannels, "|", ""))
    {
        if (entry.upToFirstOccurrenceOf ("=", false, false) != key)
            continue;

        auto rest = entry.fromFirstOccurrenceOf ("=", false, false);

        for (int i = 0; i < index; ++i)
            rest = rest.fromFirstOccurrenceOf (";", false, false);

        return rest.upToFirstOccurrenceOf (";", false, false);
    }

    return {};
}

void MainComponent::setGenericField (const juce::String& key, int index, const juce::String& value)
{
    auto entries = juce::StringArray::fromTokens (settings.extraChannels, "|", "");
    juce::String fields[3];
    int found = -1;

    for (int i = 0; i < entries.size(); ++i)
    {
        if (entries[i].upToFirstOccurrenceOf ("=", false, false) != key)
            continue;

        auto rest = entries[i].fromFirstOccurrenceOf ("=", false, false);

        for (auto& field : fields)
        {
            field = rest.upToFirstOccurrenceOf (";", false, false);
            rest = rest.fromFirstOccurrenceOf (";", false, false);
        }

        found = i;
    }

    // The format's own characters cannot ride in a name.
    fields[juce::jlimit (0, 2, index)] = index == 0
        ? value.replaceCharacters ("|=;", "   ").trim()
        : value;

    const auto entry = key + "=" + fields[0] + ";" + fields[1] + ";" + fields[2];

    if (found >= 0)
        entries.set (found, entry);
    else
        entries.add (entry);

    settings.extraChannels = entries.joinIntoString ("|");
}

juce::Array<int> MainComponent::availableFxIndices() const
{
    juce::Array<int> out;

    if (selectedChannel == "master")
        return out;

    auto loaded = juce::StringArray::fromTokens (loadoutFor (selectedChannel), ",", "");
    loaded.trim();

    for (int i = 0; i < (int) fxDefs.size(); ++i)
    {
        const auto& def = fxDefs[(size_t) i];

        if (! isLoadable (def) || loaded.contains (def.key))
            continue;

        // Stages go on any board; a lane is a single engine that lives on
        // its own strip until sources make it movable.
        const bool lane = isLane (def.key);

        if (! lane || channelOfKey (def.key) == selectedChannel)
            out.add (i);
    }

    return out;
}

void MainComponent::showAddFxMenu()
{
    juce::PopupMenu m;

    for (int index : availableFxIndices())
        m.addItem (fxDefs[(size_t) index].name,
                   [this, key = fxDefs[(size_t) index].key] { addFxToChannel (key); });

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addFxButton));
}

// A pedal joins the board engaged — adding it is the gesture that says play.
// The exception is a pedal whose ON is itself a gesture: the freeze joins
// dark, because lit it would be holding whatever happened to be sounding.
void MainComponent::addFxToChannel (const juce::String& key)
{
    auto list = juce::StringArray::fromTokens (loadoutFor (selectedChannel), ",", "");
    list.trim();
    list.removeEmptyStrings();

    if (! list.contains (key))
        list.add (key);

    setLoadout (selectedChannel, list.joinIntoString (","));

    for (int i = 0; i < (int) fxDefs.size(); ++i)
    {
        if (fxDefs[(size_t) i].key != key)
            continue;

        if (fxDefs[(size_t) i].onParam >= 0 && ! isMomentaryParam (fxDefs[(size_t) i].onParam))
            uiParams.set (fxDefs[(size_t) i].onParam, 1.0f);

        panels[i]->refresh();
        layoutFxPanels();
        selectFx (i);
        break;
    }

    pushOrderToChain (selectedChannel);
    engine.setChainActive (slotOf (selectedChannel), boardHasStages (selectedChannel));
    saveSettings();
}

// Off the board and off in the engine — a hidden pedal must not sound. Its
// knobs keep their values for a return.
void MainComponent::removeFxFromChannel (const juce::String& key)
{
    auto list = juce::StringArray::fromTokens (loadoutFor (selectedChannel), ",", "");
    list.trim();
    list.removeString (key);
    setLoadout (selectedChannel, list.joinIntoString (","));

    for (int i = 0; i < (int) fxDefs.size(); ++i)
    {
        if (fxDefs[(size_t) i].key != key)
            continue;

        if (fxDefs[(size_t) i].onParam >= 0)
            uiParams.set (fxDefs[(size_t) i].onParam, 0.0f);

        panels[i]->refresh();

        if (selectedFx == i)
            selectFx (-1);

        break;
    }

    pushOrderToChain (selectedChannel);
    engine.setChainActive (slotOf (selectedChannel), boardHasStages (selectedChannel));
    layoutFxPanels();
    saveSettings();
}

// After a preset lands, any stage it switched on must stand on the board —
// a sounding pedal that is not visible would be a lie. Never removes.
void MainComponent::syncLoadoutFromParams (const juce::String& channel)
{
    auto list = juce::StringArray::fromTokens (loadoutFor (channel), ",", "");
    list.trim();
    list.removeEmptyStrings();
    bool grown = false;

    for (const auto& def : fxDefs)
    {
        if (! isLoadable (def) || def.onParam < 0 || list.contains (def.key))
            continue;

        const bool lane = isLane (def.key);

        if (lane && channelOfKey (def.key) != channel)
            continue;

        if (params.getBool (slotOf (channel), def.onParam))
        {
            list.add (def.key);
            grown = true;
        }
    }

    if (grown)
    {
        setLoadout (channel, list.joinIntoString (","));
        pushOrderToChain (channel);
        engine.setChainActive (slotOf (channel), boardHasStages (channel));
        layoutFxPanels();
        saveSettings();
    }
}

// One pass that tells the engine the truth about every strip: each live
// channel's stage order, which boards actually hold pedals, and which
// jacks the generic strips have claimed. A strip off the rail keeps
// nothing running.
void MainComponent::refreshChainWiring()
{
    for (int slot = 0; slot < ParamStore::maxChannels; ++slot)
    {
        engine.setChainActive (slot, false);
        engine.setChannelSource (slot, 0);
    }

    for (auto* strip : channelStrips)
    {
        const auto key = strip->getComponentID();

        if (key == "master")
            continue;

        pushOrderToChain (key);
        engine.setChainActive (slotOf (key), boardHasStages (key));

        if (key.startsWith ("ch"))
            engine.setChannelSource (slotOf (key), genericField (key, 2).getIntValue());
    }
}

// The INPUT editor's picker, rebuilt for whichever strip is up: jacks by
// their owners' names for the audio strips, MIDI devices for the keys.
void MainComponent::rebuildSourceControls()
{
    auto& box = sourceControls.box;
    box.onChange = nullptr;
    box.clear (juce::dontSendNotification);
    sourceControls.midiIds.clear();

    if (selectedChannel == "keys")
    {
        box.setTextWhenNothingSelected ("MIDI keyboard...");
        box.addItem ("All MIDI devices", 1);
        sourceControls.midiIds.add (juce::String());

        for (const auto& device : juce::MidiInput::getAvailableDevices())
        {
            sourceControls.midiIds.add (device.identifier);
            box.addItem (device.name, sourceControls.midiIds.size());
        }

        box.setSelectedId (juce::jmax (1, sourceControls.midiIds.indexOf (settings.midiSource) + 1),
                           juce::dontSendNotification);

        box.onChange = [this]
        {
            const int i = sourceControls.box.getSelectedId() - 1;
            settings.midiSource = juce::isPositiveAndBelow (i, sourceControls.midiIds.size())
                                    ? sourceControls.midiIds[i] : juce::String();
            engine.setMidiSource (settings.midiSource);
            saveSettings();
        };

        return;
    }

    // Bass and mic keep their old rule when unassigned — first free channel
    // steps in — so their blank choice is honestly "Auto", not "No input".
    box.setTextWhenNothingSelected ("Input...");
    box.addItem (selectedChannel.startsWith ("ch") ? "No input" : "Auto", 1);

    if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
    {
        const auto names = device->getInputChannelNames();
        const auto owners = inputChannelOwners (
            engine.getDeviceManager().getAudioDeviceSetup().inputDeviceName, names.size());

        for (int i = 0; i < names.size(); ++i)
        {
            const auto item = i < owners.size() && owners[i].isNotEmpty()
                                ? owners[i] + juce::String (juce::CharPointer_UTF8 (" \xe2\x80\x94 ")) + names[i]
                                : names[i];
            box.addItem (item, i + 2);   // id = 1-based channel + 1
        }
    }

    const int current = selectedChannel == "bass" ? settings.bassInput
                      : selectedChannel == "mic"  ? settings.micInput
                                                  : genericField (selectedChannel, 2).getIntValue();
    box.setSelectedId (current + 1, juce::dontSendNotification);

    box.onChange = [this]
    {
        const int channel = juce::jmax (0, sourceControls.box.getSelectedId() - 1);

        if (selectedChannel == "bass")
            settings.bassInput = channel;
        else if (selectedChannel == "mic")
            settings.micInput = channel;
        else
            setGenericField (selectedChannel, 2, juce::String (channel));

        engine.setInputRoles (settings.bassInput, settings.micInput);
        refreshChainWiring();
        saveSettings();
    };
}

bool MainComponent::boardHasStages (const juce::String& channelKey) const
{
    auto loaded = juce::StringArray::fromTokens (loadoutFor (channelKey), ",", "");
    loaded.trim();

    for (const auto& def : fxDefs)
        if ((def.movable || def.key == "reverb") && loaded.contains (def.key))
            return true;

    return false;
}

void MainComponent::refreshStripSelection()
{
    for (auto* strip : channelStrips)
        strip->setSelected (strip->getComponentID() == selectedChannel);
}

// The rail rebuilt from the settings' channel list: known keys become strips
// in order, unknown ones are dropped so the list self-heals, and the master
// sum always stands at the end, unremovable.
void MainComponent::rebuildChannelRail()
{
    channelStrips.clear();   // strips go first: they hold raw meter pointers
    masterStrip = nullptr;
    genericMeters.clear();
    genericMeterKeys.clear();

    struct RailDef { const char* key; const char* name; LevelMeter* meter; juce::String* savedName; };
    const RailDef defs[] = { { "bass", "Bass",  &inputMeter, &settings.bassName },
                             { "mic",  "Vocal", &vocalMeter, &settings.micName },
                             { "keys", "Keys",  nullptr,     &settings.keysName } };

    auto wanted = juce::StringArray::fromTokens (settings.channelList, ",", "");
    wanted.trim();
    wanted.removeEmptyStrings();
    wanted.removeDuplicates (false);

    for (const auto& key : wanted)
    {
        const RailDef* rd = nullptr;

        for (const auto& d : defs)
            if (key == d.key)
                rd = &d;

        ChannelStrip* strip = nullptr;

        if (rd != nullptr)
        {
            strip = channelStrips.add (new ChannelStrip (rd->key, rd->name, rd->meter));

            if (rd->savedName->isNotEmpty())
                strip->setChannelName (*rd->savedName);

            strip->onRename = [this, saved = rd->savedName] (const juce::String& newName)
            {
                *saved = newName;
                saveSettings();
                refreshPresetList();   // the preset placeholder wears the channel's name
            };
        }
        else if (key.startsWith ("ch"))
        {
            // A generic channel: its name lives in the extras map, its knob
            // slot in its key, its meter made fresh with the strip.
            const auto saved = genericField (key, 0);
            auto* meter = genericMeters.add (new LevelMeter (1));
            genericMeterKeys.add (key);
            strip = channelStrips.add (new ChannelStrip (
                key, saved.isNotEmpty() ? saved : "CH " + juce::String (slotOf (key) + 1), meter));

            strip->onRename = [this, key] (const juce::String& newName)
            {
                setGenericField (key, 0, newName);
                saveSettings();
                refreshPresetList();
            };
        }
        else
        {
            continue;
        }

        strip->onSelect = [this, key] { selectChannel (key); };
        addAndMakeVisible (strip);

        // Removing a strip always asks first, whichever door it came in by:
        // the button in the strip's corner, or a right click.
        strip->onRemove = [this, key, strip]
        {
            juce::AlertWindow::showOkCancelBox (
                juce::MessageBoxIconType::QuestionIcon,
                "Remove " + strip->getName() + "?",
                "The strip leaves the rail and its sound stops. "
                "Its knobs stay saved for a return.",
                "Remove", "Keep", this,
                juce::ModalCallbackFunction::create ([this, key] (int choice)
                {
                    if (choice == 1)
                        removeChannel (key);
                }));
        };

        strip->onContextMenu = [this, strip]
        {
            juce::PopupMenu m;
            m.addItem ("Remove " + strip->getName() + "...",
                       [safe = juce::Component::SafePointer<ChannelStrip> (strip)]
            {
                if (safe != nullptr && safe->onRemove != nullptr)
                    safe->onRemove();
            });

            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (strip));
        };

        strip->setRemovable (true);
    }

    masterStrip = channelStrips.add (new ChannelStrip ("master", "Master", &masterMeter));
    masterStrip->onSelect = [this] { selectChannel ("master"); };
    addAndMakeVisible (masterStrip);

    // The selection must point at a strip that exists.
    bool selectedExists = false;

    for (auto* strip : channelStrips)
        selectedExists = selectedExists || strip->getComponentID() == selectedChannel;

    if (! selectedExists)
        selectedChannel = channelStrips.getFirst()->getComponentID();

    refreshStripSelection();
}

// The + grows the rail: a fresh empty channel, or a wired strip coming
// back. Nothing here is ever greyed out — what already stands on the rail
// simply is not an option to add again.
void MainComponent::showAddChannelMenu()
{
    juce::StringArray present;

    for (auto* strip : channelStrips)
        present.add (strip->getComponentID());

    juce::PopupMenu m;
    m.addItem ("New channel", [this] { addGenericChannel(); });

    struct Kind { const char* key; const char* label; };
    const Kind kinds[] = { { "bass", "Bass" }, { "mic", "Vocal" }, { "keys", "Keys" } };

    for (const auto& kind : kinds)
        if (! present.contains (kind.key))
            m.addItem (kind.label, [this, key = juce::String (kind.key)] { addChannel (key); });

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addChannelButton));
}

// A new channel claims the lowest free slot; with all eight strips standing
// the + has nothing left to give.
void MainComponent::addGenericChannel()
{
    auto list = juce::StringArray::fromTokens (settings.channelList, ",", "");
    list.trim();
    list.removeEmptyStrings();

    for (int slot = 3; slot < ParamStore::maxChannels; ++slot)
    {
        const juce::String key = "ch" + juce::String (slot + 1);

        if (! list.contains (key))
        {
            addChannel (key);
            return;
        }
    }
}

void MainComponent::addChannel (const juce::String& key)
{
    auto list = juce::StringArray::fromTokens (settings.channelList, ",", "");
    list.trim();
    list.removeEmptyStrings();

    if (! list.contains (key))
        list.add (key);

    settings.channelList = list.joinIntoString (",");

    rebuildChannelRail();
    refreshChainWiring();
    selectChannel (key);
    saveSettings();
    resized();
}

// The strip leaves the rail and its noise-makers go quiet: a hidden channel
// that kept sounding would be a lie. The knob values stay saved for a return.
void MainComponent::removeChannel (const juce::String& key)
{
    if (key == "bass")
        params.set (pInMute, 1.0f);

    if (key == "mic")
    {
        params.set (pVocalOn, 0.0f);
        params.set (pVocoderOn, 0.0f);
    }

    if (key == "keys")
    {
        params.set (pMidiOn, 0.0f);
        params.set (pInstOn, 0.0f);
    }

    const bool removingSelected = selectedChannel == key;

    auto list = juce::StringArray::fromTokens (settings.channelList, ",", "");
    list.removeString (key);
    settings.channelList = list.joinIntoString (",");

    rebuildChannelRail();
    refreshChainWiring();

    if (removingSelected)
        selectChannel (channelStrips.getFirst()->getComponentID());

    refreshAllControls();   // the ON switches the removal flipped
    saveSettings();
    resized();
}

// Which knob copies a channel's rows edit. The first three strips carry the
// legacy names; generic channels wear their slot in their key ("ch4" = slot
// 3). Master's own row is a global, so its slot never matters.
int MainComponent::slotOf (const juce::String& channelKey)
{
    if (channelKey == "bass")   return 0;
    if (channelKey == "mic")    return 1;
    if (channelKey == "keys")   return 2;

    if (channelKey.startsWith ("ch"))
        return juce::jlimit (0, ParamStore::maxChannels - 1,
                             channelKey.fromFirstOccurrenceOf ("ch", false, false).getIntValue() - 1);

    return 0;
}

void MainComponent::selectChannel (const juce::String& channelKey)
{
    selectedChannel = channelKey;
    uiParams.setChannel (slotOf (channelKey));
    refreshStripSelection();
    refreshPresetList();

    // Landing on a channel opens its first row, so the editor is never a
    // stale view of some other channel's effect.
    if (selectedFx < 0 || channelOfKey (fxDefs[(size_t) selectedFx].key) != channelKey)
    {
        const auto visible = visibleFxIndices();
        selectFx (visible.isEmpty() ? -1 : visible.getFirst());
    }

    layoutFxPanels();
}

void MainComponent::selectFx (int index)
{
    selectedFx = juce::isPositiveAndBelow (index, (int) fxDefs.size()) ? index : -1;

    // Stages live on whatever strip shows them; only a lane or the master
    // row has one home worth jumping to — so preset recalls and the
    // snapshot tool land with the row actually on screen.
    if (selectedFx >= 0)
    {
        const auto& key = fxDefs[(size_t) selectedFx].key;
        const bool hasOneHome = isLane (key) || key == "master";
        const auto channel = channelOfKey (key);

        if (hasOneHome && channel != selectedChannel)
        {
            selectedChannel = channel;
            uiParams.setChannel (slotOf (channel));
            refreshStripSelection();
            refreshPresetList();
            layoutFxPanels();
        }
    }

    for (int i = 0; i < panels.size(); ++i)
        panels[i]->setSelected (i == selectedFx);

    if (selectedFx >= 0)
    {
        const auto& def = fxDefs[(size_t) selectedFx];

        juce::Component* extra = nullptr;
        int extraHeight = 30;
        bool faderRows = true;

        if (def.key == "amp")
        {
            extra = &ampControls;
        }
        else if (def.key == "input")
        {
            rebuildSourceControls();
            extra = &sourceControls;
        }
        else if (def.key == "keys")
        {
            extra = &instrumentControls;
        }
        else if (def.key == "eq")
        {
            // The curve view carries all fifteen band parameters itself.
            extra = &eqControls;
            extraHeight = EqView::preferredHeight;
            faderRows = false;
        }
        else if (def.key == "vocal")
        {
            extra = &autotuneControls;
        }
        else if (def.key == "vocoder")
        {
            extra = &vocoderControls;
        }

        fxEditor.show (def.scope, def.name, def.ids, extra, extraHeight, faderRows);
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

// The board's sequence goes to that channel's chain: loaded movables first,
// in the loadout's order, then every stage the board does not hold — all
// off, but the word must still name each stage exactly once.
void MainComponent::pushOrderToChain (const juce::String& channelKey)
{
    if (channelKey == "master")
        return;

    auto loaded = juce::StringArray::fromTokens (loadoutFor (channelKey), ",", "");
    loaded.trim();

    int order[FxChain::numMovableStages] {};
    int count = 0;
    juce::StringArray placed;

    for (const auto& key : loaded)
        for (const auto& def : fxDefs)
            if (def.movable && def.key == key && ! placed.contains (key)
                 && count < FxChain::numMovableStages)
            {
                order[count++] = def.stage;
                placed.add (key);
            }

    for (const auto& def : fxDefs)
        if (def.movable && ! placed.contains (def.key) && count < FxChain::numMovableStages)
        {
            order[count++] = def.stage;
            placed.add (def.key);
        }

    // A stage added to fxDefs but not the Stage enum — or removed from only
    // one of them — would otherwise just stop reorders reaching the audio
    // thread, with nothing on screen to say so.
    jassert (count == FxChain::numMovableStages);

    if (count == FxChain::numMovableStages)
        engine.getChain (slotOf (channelKey)).setStageOrder (order);
}

juce::String MainComponent::orderStringFor (const juce::String& channelKey) const
{
    auto loaded = juce::StringArray::fromTokens (loadoutFor (channelKey), ",", "");
    loaded.trim();
    juce::StringArray keys;

    for (const auto& key : loaded)
        for (const auto& def : fxDefs)
            if (def.movable && def.key == key && ! keys.contains (key))
                keys.add (key);

    for (const auto& def : fxDefs)
        if (def.movable && ! keys.contains (def.key))
            keys.add (def.key);

    return keys.joinIntoString (",");
}

// A preset recalling the wiring it was saved with: the saved sequence
// reorders this channel's board. Stages the preset does not know keep their
// places after the ones it does; an empty string (a preset from before
// order was captured) leaves the wiring alone.
void MainComponent::applyPresetOrder (const juce::String& channelKey, const juce::String& saved)
{
    if (saved.isEmpty())
        return;

    auto loaded = juce::StringArray::fromTokens (loadoutFor (channelKey), ",", "");
    loaded.trim();
    juce::StringArray reordered;

    for (const auto& key : juce::StringArray::fromTokens (saved, ",", ""))
        if (loaded.contains (key.trim()))
            reordered.add (key.trim());

    for (const auto& key : loaded)
        if (! reordered.contains (key))
            reordered.add (key);

    if (reordered != loaded)
    {
        setLoadout (channelKey, reordered.joinIntoString (","));
        pushOrderToChain (channelKey);
        layoutFxPanels();
        saveSettings();
    }
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

    // The row may land anywhere in the movable run and nowhere else, in
    // VISIBLE slots — the list shows one channel at a time, so slot numbers
    // and fxDefs indices are no longer the same thing.
    const auto visible = visibleFxIndices();
    int first = -1, last = -1, startSlot = -1;

    for (int slot = 0; slot < visible.size(); ++slot)
    {
        if (fxDefs[(size_t) visible[slot]].movable)
        {
            if (first < 0)
                first = slot;

            last = slot;
        }

        if (visible[slot] == dragStartIndex)
            startSlot = slot;
    }

    if (first < 0 || startSlot < 0)
        return;

    const int y = juce::jlimit (first * pitch, last * pitch,
                                startSlot * pitch + e.getDistanceFromDragStartY());

    panel->setTopLeftPosition (0, y);
    dragTargetSlot = juce::jlimit (first, last, (y + pitch / 2) / pitch);
    layoutFxPanels();
}

void MainComponent::dropFxRow (FxPanel* panel)
{
    if (draggedPanel != panel)
        return;

    const int from = dragStartIndex;
    const auto visible = visibleFxIndices();
    const int to = juce::isPositiveAndBelow (dragTargetSlot, visible.size())
                     ? visible[dragTargetSlot] : -1;

    draggedPanel = nullptr;
    dragStartIndex = dragTargetSlot = -1;

    // The move happens in the board's sequence, not in the panel list: this
    // channel's loadout is the only order that changes.
    if (to >= 0 && to != from
        && juce::isPositiveAndBelow (from, (int) fxDefs.size())
        && juce::isPositiveAndBelow (to, (int) fxDefs.size())
        && fxDefs[(size_t) from].movable && fxDefs[(size_t) to].movable)
    {
        auto loaded = juce::StringArray::fromTokens (loadoutFor (selectedChannel), ",", "");
        loaded.trim();

        const int a = loaded.indexOf (fxDefs[(size_t) from].key);
        const int b = loaded.indexOf (fxDefs[(size_t) to].key);

        if (a >= 0 && b >= 0 && a != b)
        {
            loaded.move (a, b);
            setLoadout (selectedChannel, loaded.joinIntoString (","));
            pushOrderToChain (selectedChannel);
            saveSettings();
        }
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

void MainComponent::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (! juce::isPositiveAndBelow (row, songNames.size()))
        return;

    const auto name = songNames[row];
    const auto folder = SongMaker::songsDir().getChildFile (name);

    // A row is a folder of stems on disk, so the library's own menu lives
    // where the row is. The highlight belongs to the loaded song, not to
    // whatever was right-clicked, so it goes back when the menu closes.
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu m;
        m.addSectionHeader (name);
        m.addItem ("Remove from library...", [this, name, folder]
        {
            juce::AlertWindow::showOkCancelBox (
                juce::MessageBoxIconType::QuestionIcon,
                "Remove " + name + "?",
                "The song folder and its stems go to the Trash, where the Finder "
                "can still bring them back. Nothing else in the rig changes.",
                "Remove", "Keep", this,
                juce::ModalCallbackFunction::create ([this, folder] (int choice)
                {
                    if (choice == 1)
                        removeSong (folder);
                }));
        });

        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                         [this] (int) { selectLoadedSongRow(); });
        return;
    }

    loadSong (folder);
}

// Removing the song that is playing stops it first: the rig must not be
// reading stems out of a folder on its way to the Trash.
void MainComponent::removeSong (const juce::File& folder)
{
    if (folder == loadedFolder)
    {
        auto& player = engine.getStemPlayer();
        player.stop();
        player.clearLoop();
        player.unload();

        pendingLoopIn = -1;
        loadedFolder = juce::File();
        settings.songFolder.clear();

        songLabel.setColour (juce::Label::textColourId, juce::Colour (0xffc8c9ce));
        songLabel.setText ("no song loaded", juce::dontSendNotification);
        saveSettings();
        refreshLoopButton();
    }

    if (! folder.moveToTrash())
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon, "Remove song",
            "Could not move " + folder.getFileName() + " to the Trash.");

    refreshSongList();
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
    selectLoadedSongRow();
    songList.repaint();
}

// The highlight in the songs list means "this is the song that is loaded",
// so it is always derived, never left wherever the mouse last went.
void MainComponent::selectLoadedSongRow()
{
    const int index = songNames.indexOf (loadedFolder.getFileName());

    if (index >= 0)
        songList.selectRow (index, true, true);
    else
        songList.deselectAllRows();
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
    // The generic channel tick-list was the wrong shape for this rig: it
    // caps at two ticks and evicts one whenever a third lands, it hides the
    // interesting rows of an aggregate below its four visible lines, and it
    // cannot say which tick is the bass. So the dialog asks the actual
    // question — which channel is the bass, which is the mic — by channel
    // name, where "Front Left" reads as the USB mic and "Loopback 1" reads
    // as a thing to avoid. The selector keeps devices, outputs, rate and
    // buffer; its own input channel list is suppressed by a minimum set
    // above any real device's channel count.
    struct DeviceContent : juce::Component,
                           private juce::ChangeListener
    {
        explicit DeviceContent (MainComponent& mc)
            : owner (&mc), manager (mc.engine.getDeviceManager()), settings (mc.settings)
        {
            selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
                           manager, 256, 256, 0, 2, false, false, true, false);
            addAndMakeVisible (*selector);

            caption.setText ("RIG INPUTS", juce::dontSendNotification);
            caption.setFont (rigFont (11.0f, true));
            caption.setColour (juce::Label::textColourId, juce::Colour (0xff70737c));
            addAndMakeVisible (caption);

            bassLabel.setText ("Instrument", juce::dontSendNotification);
            micLabel.setText ("Microphone", juce::dontSendNotification);

            for (auto* label : { &bassLabel, &micLabel })
            {
                label->setFont (rigFont (12.0f));
                label->setColour (juce::Label::textColourId, juce::Colour (0xff9a9da6));
                addAndMakeVisible (label);
            }

            bassBox.setTextWhenNothingSelected (juce::CharPointer_UTF8 ("\xe2\x80\x94"));
            micBox.setTextWhenNothingSelected (juce::CharPointer_UTF8 ("\xe2\x80\x94"));
            bassBox.onChange = [this] { applyRoles(); };
            micBox.onChange = [this] { applyRoles(); };
            addAndMakeVisible (bassBox);
            addAndMakeVisible (micBox);

            manager.addChangeListener (this);
            refreshPickers();
            setSize (520, 470);
        }

        ~DeviceContent() override
        {
            manager.removeChangeListener (this);
        }

        // Device switches land here (so the pickers relist the new device's
        // channels), and so does every apply — repopulating without
        // notification keeps that from looping.
        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            refreshPickers();
        }

        void refreshPickers()
        {
            bassBox.clear (juce::dontSendNotification);
            micBox.clear (juce::dontSendNotification);
            micBox.addItem ("None", noneId);

            auto* device = manager.getCurrentAudioDevice();
            const bool haveDevice = device != nullptr;

            bassBox.setEnabled (haveDevice);
            micBox.setEnabled (haveDevice);

            if (! haveDevice)
                return;

            // Channels introduce themselves with the device they belong to,
            // so an aggregate's rows read "AT2020USB+ — Front Left" rather
            // than leaving the mic to be deduced. The lookup wants the input
            // device's own name: a combined in+out device reports a merged
            // identity of its own.
            const auto names = device->getInputChannelNames();
            const auto owners = inputChannelOwners (manager.getAudioDeviceSetup().inputDeviceName,
                                                    names.size());

            for (int i = 0; i < names.size(); ++i)
            {
                const auto item = i < owners.size()
                                    ? owners[i] + juce::String (juce::CharPointer_UTF8 (" \xe2\x80\x94 ")) + names[i]
                                    : names[i];

                bassBox.addItem (item, i + 1);
                micBox.addItem (item, i + 1);
            }

            // Show what is actually live: the assigned roles where they are
            // among the active channels, compaction order where they are not
            // — the same resolution the engine applies.
            const auto active = manager.getAudioDeviceSetup().inputChannels;
            const int first = active.findNextSetBit (0);
            const int second = first >= 0 ? active.findNextSetBit (first + 1) : -1;

            const int bassChan = settings.bassInput >= 1 && active[settings.bassInput - 1]
                                   ? settings.bassInput : first + 1;
            const int micChan = settings.micInput >= 1 && active[settings.micInput - 1]
                                  && settings.micInput != bassChan
                                   ? settings.micInput : second + 1;

            bassBox.setSelectedId (bassChan, juce::dontSendNotification);
            micBox.setSelectedId (micChan >= 1 ? micChan : noneId, juce::dontSendNotification);
        }

        void applyRoles()
        {
            const int bassChan = bassBox.getSelectedId();
            const int micChan = micBox.getSelectedId() == noneId ? 0 : micBox.getSelectedId();

            if (bassChan < 1)
                return;

            juce::BigInteger chans;
            chans.setBit (bassChan - 1);

            if (micChan >= 1)
                chans.setBit (micChan - 1);

            auto config = manager.getAudioDeviceSetup();

            if (config.inputChannels != chans)
            {
                config.inputChannels = chans;
                config.useDefaultInputChannels = false;
                manager.setAudioDeviceSetup (config, true);
            }

            settings.bassInput = bassChan;
            settings.micInput = micChan;

            if (owner != nullptr)
            {
                owner->engine.setInputRoles (bassChan, micChan);
                owner->saveSettings();
            }
        }

        void resized() override
        {
            // The rig's own question first, at the top where it cannot be
            // missed; the stock selector below. Label widths put the boxes
            // in the same column as the selector's controls.
            auto b = getLocalBounds();
            auto top = b.removeFromTop (86).reduced (10, 2);

            caption.setBounds (top.removeFromTop (18));
            auto row = top.removeFromTop (26);
            bassLabel.setBounds (row.removeFromLeft (172));
            bassBox.setBounds (row.reduced (0, 1));
            top.removeFromTop (4);
            row = top.removeFromTop (26);
            micLabel.setBounds (row.removeFromLeft (172));
            micBox.setBounds (row.reduced (0, 1));

            b.removeFromTop (8);
            selector->setBounds (b);
        }

        enum { noneId = 1000 };

        juce::Component::SafePointer<MainComponent> owner;
        juce::AudioDeviceManager& manager;
        SettingsStore& settings;
        std::unique_ptr<juce::AudioDeviceSelectorComponent> selector;
        juce::Label caption, bassLabel, micLabel;
        juce::ComboBox bassBox, micBox;
    };

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new DeviceContent (*this));
    options.dialogTitle = "Audio device";
    options.dialogBackgroundColour = juce::Colour (0xff17181c);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

//==============================================================================
// Presets are one channel's. Bass presets keep the plain names the store has
// always used (older whole-chain presets keep working as bass presets, and
// only their chain parameters recall); the other channels file theirs under
// a "channel:" prefix the bar never shows.
juce::String MainComponent::presetKeyFor (const juce::String& displayName) const
{
    return presetKeyFor (selectedChannel, displayName);
}

juce::String MainComponent::presetKeyFor (const juce::String& channelKey, const juce::String& displayName) const
{
    return channelKey == "bass" ? displayName
                                : channelKey + ":" + displayName;
}

// A channel's presets, by the names its bar shows.
juce::StringArray MainComponent::presetNamesFor (const juce::String& channelKey) const
{
    juce::StringArray display;

    for (const auto& stored : presets.getNames())
    {
        if (channelKey == "bass" && ! stored.contains (":"))
            display.add (stored);
        else if (stored.startsWith (channelKey + ":"))
            display.add (stored.fromFirstOccurrenceOf (":", false, false));
    }

    return display;
}

juce::String MainComponent::channelDisplayName() const
{
    return channelDisplayNameFor (selectedChannel);
}

juce::String MainComponent::channelDisplayNameFor (const juce::String& channelKey) const
{
    for (auto* strip : channelStrips)
        if (strip->getComponentID() == channelKey)
            return strip->getName();

    return channelKey;
}

// Everything a preset could set on this strip — the anatomy plus every
// pedal that could stand here, loaded or not. Recalls read with this wider
// net so a preset's pedals land even on a board that lacks them; the sync
// pass then pulls the switched-on ones aboard.
juce::Array<int> MainComponent::applicableParamIds() const
{
    return applicableParamIdsFor (selectedChannel);
}

juce::Array<int> MainComponent::applicableParamIdsFor (const juce::String& channelKey) const
{
    juce::Array<int> ids;

    for (const auto& def : fxDefs)
    {
        const bool lane = isLane (def.key);
        const bool applies = channelKey == "master"
                               ? def.key == "master"
                               : def.key == "input" || def.key == "out"
                                   || (isLoadable (def)
                                        && (! lane || channelOfKey (def.key) == channelKey));

        if (! applies)
            continue;

        if (def.onParam >= 0)
            ids.addIfNotAlreadyThere (def.onParam);

        for (int id : def.ids)
            ids.addIfNotAlreadyThere (id);
    }

    return ids;
}

juce::Array<int> MainComponent::channelParamIds() const
{
    juce::Array<int> ids;

    auto loaded = juce::StringArray::fromTokens (loadoutFor (selectedChannel), ",", "");
    loaded.trim();

    // A preset captures the strip's anatomy and its loaded pedals — knobs
    // of pedals not on the board stay out of the picture.
    for (const auto& def : fxDefs)
    {
        const bool captured = selectedChannel == "master"
                                ? def.key == "master"
                                : def.key == "input" || def.key == "out"
                                    || (isLoadable (def) && loaded.contains (def.key));

        if (! captured)
            continue;

        if (def.onParam >= 0)
            ids.addIfNotAlreadyThere (def.onParam);

        for (int id : def.ids)
            ids.addIfNotAlreadyThere (id);
    }

    return ids;
}

void MainComponent::refreshPresetList (const juce::String& selectName)
{
    presetBox.clear (juce::dontSendNotification);
    presetBox.setTextWhenNothingSelected (channelDisplayName() + " preset");

    const auto display = presetNamesFor (selectedChannel);

    for (int i = 0; i < display.size(); ++i)
        presetBox.addItem (display[i], i + 1);

    const int index = display.indexOf (selectName);

    if (index >= 0)
        presetBox.setSelectedId (index + 1, juce::dontSendNotification);
    else
        presetBox.setText (selectName, juce::dontSendNotification);
}

void MainComponent::applySelectedPreset()
{
    recallPreset (selectedChannel, presetBox.getText());
}

// One channel's preset by name — from its bar, or from a foot.
bool MainComponent::recallPreset (const juce::String& channelKey, const juce::String& displayName)
{
    const auto key = presetKeyFor (channelKey, displayName);

    if (! presets.apply (key, params, applicableParamIdsFor (channelKey), slotOf (channelKey)))
        return false;

    // Pull aboard anything the preset switched on, then let its saved
    // sequence wire the board. The amp capture only ever rides bass
    // presets; elsewhere it comes back void and is a no-op.
    syncLoadoutFromParams (channelKey);
    applyPresetOrder (channelKey, presets.getChainOrder (key));
    applyAmpCapture (presets.getAmpCapture (key));
    refreshAllControls();
    return true;
}

void MainComponent::saveCurrentPreset()
{
    const auto name = presetBox.getText().trim();

    if (name.isEmpty())
        return;

    const bool bass = selectedChannel == "bass";

    presets.store (presetKeyFor (name), params,
                   orderStringFor (selectedChannel),
                   bass ? juce::var (engine.getFxChain().getAmp().getLoadedName())
                        : juce::var(),
                   channelParamIds(), slotOf (selectedChannel));
    refreshPresetList (name);
}

void MainComponent::deleteSelectedPreset()
{
    const auto name = presetBox.getText().trim();
    const auto key = presetKeyFor (name);

    if (name.isEmpty() || ! presets.contains (key))
        return;

    presets.remove (key);
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
// What a foot can be given to do. The menu is the rig as it stands: the
// strips on the rail, the pedals on each board, the song, and the other
// foot layouts — nothing that is not there.
juce::String MainComponent::footParamName (int paramId) const
{
    const juce::String label (spec (paramId).label);

    // The EQ's fifteen knobs are five bands of the same three names.
    if (paramId >= pEq1Freq && paramId <= pEq5Q)
        return "Band " + juce::String ((paramId - pEq1Freq) / 3 + 1) + " " + label.toLowerCase();

    return label;
}

void MainComponent::buildFootMenu (juce::PopupMenu& menu, const FootView::Chooser& choose) const
{
    const auto item = [&choose] (juce::PopupMenu& m, const juce::String& text, const juce::String& action)
    {
        m.addItem (text, [choose, action] { choose (action); });
    };

    const auto isMute = [] (int p) { return p == pInMute || p == pLiveMute || p == pMasterMute; };

    for (auto* strip : channelStrips)
    {
        const auto key = strip->getComponentID();
        juce::PopupMenu channel;

        if (key == "master")
        {
            item (channel, "Mute", "rig/masterMute");
            item (channel, "Gain", "rig/masterGain");
            menu.addSubMenu (strip->getName(), channel);
            continue;
        }

        juce::PopupMenu recall;

        for (const auto& name : presetNamesFor (key))
            item (recall, name, "do/preset:" + key + ":" + name);

        if (recall.getNumItems() > 0)
            channel.addSubMenu ("Recall preset", recall);

        for (int index : visibleFxIndicesFor (key))
        {
            const auto& def = fxDefs[(size_t) index];
            juce::PopupMenu fx;

            if (def.onParam >= 0)
                item (fx, isMute (def.onParam) ? "Mute" : isMomentaryParam (def.onParam) ? "Hold" : "On / off",
                      key + "/" + spec (def.onParam).id);

            for (int id : def.ids)
                item (fx, footParamName (id), key + "/" + spec (id).id);

            channel.addSubMenu (def.name, fx);
        }

        menu.addSubMenu (strip->getName(), channel);
    }

    juce::PopupMenu song;
    item (song, "Play / stop", "do/play");
    item (song, "Loop tap", "do/loop");
    item (song, "Track level", "rig/trackGain");

    for (int i = 0; i < StemPlayer::numStems; ++i)
    {
        const auto stem = juce::String (StemPlayer::stemNames[i]);
        const auto name = stem.substring (0, 1).toUpperCase() + stem.substring (1);
        item (song, name + " mute", "rig/stem" + juce::String (i) + "Mute");
        item (song, name + " level", "rig/stem" + juce::String (i) + "Gain");
    }

    menu.addSubMenu ("Song", song);
    item (menu, "MIDI panic", "do/panic");

    const auto layouts = footView.getLayoutNames();

    if (layouts.size() > 1)
    {
        juce::PopupMenu switchTo;

        for (const auto& name : layouts)
            item (switchTo, name, "do/layout:" + name);

        menu.addSubMenu ("Switch foot layout", switchTo);
    }
}

juce::String MainComponent::describeFootAction (const juce::String& action) const
{
    static const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));

    if (action == "do/play")    return "Song" + dot + "Play / stop";
    if (action == "do/loop")    return "Song" + dot + "Loop tap";
    if (action == "do/panic")   return "MIDI panic";

    if (action.startsWith ("do/layout:"))
        return "Foot layout" + dot + action.fromFirstOccurrenceOf (":", false, false);

    if (action.startsWith ("do/preset:"))
    {
        const auto rest = action.fromFirstOccurrenceOf (":", false, false);
        return channelDisplayNameFor (rest.upToFirstOccurrenceOf (":", false, false))
                 + dot + "Preset" + dot + rest.fromFirstOccurrenceOf (":", false, false);
    }

    FootParamTarget target;

    if (! parseFootParamAction (action, target))
        return action;

    const int p = target.param;
    const auto channelKey = action.upToFirstOccurrenceOf ("/", false, false);

    if (channelKey == "rig")
    {
        if (p == pMasterMute)   return "Master" + dot + "Mute";
        if (p == pMasterGain)   return "Master" + dot + "Gain";
        if (p == pTrackGain)    return "Song" + dot + "Track level";

        for (int i = 0; i < StemPlayer::numStems; ++i)
        {
            const auto stem = juce::String (StemPlayer::stemNames[i]);
            const auto name = stem.substring (0, 1).toUpperCase() + stem.substring (1);

            if (p == pStemMute0 + i)    return "Song" + dot + name + " mute";
            if (p == pStemGain0 + i)    return "Song" + dot + name + " level";
        }

        return footParamName (p);
    }

    const auto channelName = channelDisplayNameFor (channelKey);

    for (const auto& def : fxDefs)
    {
        if (def.onParam == p)
            return channelName + dot + def.name
                     + (p == pInMute || p == pLiveMute ? " mute" : "");

        if (std::find (def.ids.begin(), def.ids.end(), p) != def.ids.end())
            return channelName + dot + def.name + dot + footParamName (p);
    }

    return channelName + dot + footParamName (p);
}

void MainComponent::runFootCommand (const juce::String& command)
{
    if (command == "do/play")
    {
        engine.getStemPlayer().togglePlay();
    }
    else if (command == "do/loop")
    {
        loopTapped();
    }
    else if (command == "do/panic")
    {
        engine.getMidi().panic();
    }
    else if (command.startsWith ("do/layout:"))
    {
        footView.selectLayout (command.fromFirstOccurrenceOf (":", false, false));
    }
    else if (command.startsWith ("do/preset:"))
    {
        const auto rest = command.fromFirstOccurrenceOf (":", false, false);
        const auto channelKey = rest.upToFirstOccurrenceOf (":", false, false);
        const auto name = rest.fromFirstOccurrenceOf (":", false, false);

        // The bar should name what is now playing, if it is this strip's.
        if (recallPreset (channelKey, name) && channelKey == selectedChannel)
            refreshPresetList (name);
    }
}

void MainComponent::handleAsyncUpdate()
{
    for (const auto& command : engine.getFoot().takeCommands())
        runFootCommand (command);
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
    // With no instrument there is nothing to clear and no window to open, so
    // those two are absent rather than sitting there greyed.
    instrumentControls.clear.setVisible (host.isLoaded());
    instrumentControls.open.setVisible (host.isLoaded());

}

void MainComponent::openPluginWindow()
{
    openPluginWindowFor (engine.getInstrument().getLiveInstrument(),
                         engine.getInstrument().getLoadedName());
}

// One plugin window at a time, on purpose: this is a stage rig, not a studio
// session, and a stack of floating editors is how a window ends up covering
// the panic button.
void MainComponent::openPluginWindowFor (juce::AudioProcessor* plugin, const juce::String& title)
{
    if (plugin == nullptr)
        return;

    pluginWindow.reset();

    auto* editor = plugin->createEditorIfNeeded();

    if (editor == nullptr)
        return;

    struct PluginWindow : juce::DocumentWindow
    {
        PluginWindow (const juce::String& titleText, juce::AudioProcessorEditor* content,
                      std::unique_ptr<juce::DocumentWindow>& slot)
            : juce::DocumentWindow (titleText, juce::Colour (0xff17181c), closeButton), owner (slot)
        {
            setUsingNativeTitleBar (true);

            // Some editors — Melda's among them — report 1x1 until their own
            // GUI thread has laid them out. A window sized to that is
            // invisible and reads as "nothing happened". Give such an editor
            // room; it announces its real size a moment later and the window
            // follows, because the content is tracked.
            if (content->getWidth() < 40 || content->getHeight() < 40)
                content->setSize (560, 400);

            setContentOwned (content, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
            toFront (true);
        }

        void closeButtonPressed() override { owner.reset(); }

        std::unique_ptr<juce::DocumentWindow>& owner;
    };

    pluginWindow = std::make_unique<PluginWindow> (title, editor, pluginWindow);
}

// The autotune and vocoder slots bind themselves: the highest-preference
// real plugin of each kind installed on this machine, re-checked on launch
// and after every scan. Nothing to pick, nothing to configure — install
// Graillon or TAL-Vocoder and each simply takes over from the stopgap on
// the next launch or rescan.
//
// Deliberately no saveSettings() here: this runs during construction, and a
// constructor must never write to disk — the snapshot tool builds this
// window with an unloaded settings store, and saving from here once
// flattened the real settings file with blanks. The bindings are re-derived
// every launch anyway; they persist with the next ordinary save.
void MainComponent::autoBindHostedRoles()
{
    const auto bind = [this] (EffectSlot& slot, const juce::String& wanted)
    {
        if (wanted.isEmpty() || wanted == slot.getLoadedName())
            return;

        pluginWindow.reset();

        juce::String error;
        slot.load (engine.getInstrument(), wanted, error);
    };

    bind (engine.getTuneSlot(), engine.getInstrument().preferredPitchPlugin());
    bind (engine.getVocoderSlot(), engine.getInstrument().preferredVocoderPlugin());
    refreshSlotControls();
}

// Both mic-lane status lines. Latency is stated the moment it exists, and
// the autotune line always says which plugin is doing the work — or what to
// install when none is.
void MainComponent::refreshSlotControls()
{
    const auto names = engine.getInstrument().getEffectNames();
    const bool scanning = engine.getInstrument().isScanning();

    const auto describe = [] (EffectSlot& slot) -> juce::String
    {
        if (slot.hasFailed())
            return "failed and was removed";

        if (! slot.isLoaded())
            return {};

        return slot.getLatencyMs() > 0.05f
                 ? "adds " + juce::String (slot.getLatencyMs(), 1) + " ms"
                 : "zero latency";
    };

    // The other half of "nothing happens": with one active input there is
    // no mic path, whatever the lanes are set to.
    const juce::String noMic = engine.getActiveInputCount() < 2 && engine.getSampleRate() > 0.0
        ? "   ·   no mic input is active — assign one in Audio device..."
        : juce::String();

    auto& tune = engine.getTuneSlot();
    juce::String tuneLine;

    if (tune.isLoaded() || tune.hasFailed())
        tuneLine = tune.getLoadedName() + " — " + describe (tune);
    else if (tune.getLastError().isNotEmpty())
        tuneLine = tune.getLastError();
    else if (scanning)
        tuneLine = "looking for a pitch plugin...";
    else
        tuneLine = "no pitch plugin installed — Graillon 3 is free (auburnsounds.com)";

    autotuneControls.status.setText (tuneLine + noMic, juce::dontSendNotification);
    autotuneControls.open.setEnabled (tune.isLoaded());

    auto& vocoder = engine.getVocoderSlot();
    juce::String vocoderLine;

    if (vocoder.isLoaded() || vocoder.hasFailed())
    {
        vocoderLine = vocoder.getLoadedName() + " — " + describe (vocoder);

        // A carrier route that silently failed to negotiate would just sound
        // like a broken vocoder, so the route is always named.
        if (vocoder.isLoaded())
            vocoderLine += vocoder.getSidechainChannels() > 0
                             ? (vocoder.isModulatorOnSidechain()
                                  ? ", bass carrier on the main input, mic on the sidechain"
                                  : ", bass carrier on the sidechain")
                             : ", no sidechain bus — carrier shares the main pair";
    }
    else if (vocoder.getLastError().isNotEmpty())
        vocoderLine = vocoder.getLastError();
    else if (scanning)
        vocoderLine = "looking for a vocoder...";
    else
        vocoderLine = "no vocoder installed — TAL-Vocoder 3 is free (tal-software.com)";

    vocoderControls.status.setText (vocoderLine + noMic, juce::dontSendNotification);
    vocoderControls.open.setEnabled (vocoder.isLoaded());


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
// carries the selected row's key. A name survives reorders and new stages,
// where the one-hot index this field used to hold pointed at whatever row
// happened to move into that slot.
juce::String MainComponent::expandedStateString() const
{
    return juce::isPositiveAndBelow (selectedFx, (int) fxDefs.size())
             ? fxDefs[(size_t) selectedFx].key : juce::String();
}

void MainComponent::applyExpandedState (const juce::String& state)
{
    for (int i = 0; i < (int) fxDefs.size(); ++i)
    {
        if (fxDefs[(size_t) i].key == state)
        {
            selectFx (i);
            return;
        }
    }

    // An older settings file still holds the one-hot string; its position is
    // a decent guess, and no '1' at all means nothing was open.
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
    settings.tunePlugin = engine.getTuneSlot().getLoadedName();
    settings.tunePluginState = engine.getTuneSlot().getStateBase64();
    settings.vocoderPlugin = engine.getVocoderSlot().getLoadedName();
    settings.vocoderPluginState = engine.getVocoderSlot().getStateBase64();
    settings.chainOrder = orderStringFor ("bass");
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

        // A finished scan may have just discovered better tenants.
        if (! instrumentScanning)
            autoBindHostedRoles();

        refreshSlotControls();
    }

    midiStatus.setNote (engine.getFxChain().getSoundingNote());
    engine.getMidi().setChannel ((int) std::lround (params.get (pMidiChannel)));
    engine.getMidi().setExpressionController ((int) std::lround (params.get (pMidiExpression)));

    // The switches can arrive from a foot controller as well as the buttons;
    // when they move underneath the UI, the editor catches up here. The same
    // tick notices a plugin that ejected itself mid-song.
    const int micLanes = (params.getBool (pVocalOn) ? 1 : 0)
                           | (params.getBool (pVocoderOn) ? 2 : 0);

    if (micLanes != lastMicLanes)
    {
        lastMicLanes = micLanes;
        refreshAllControls();
    }

    // Anything else a foot moved: a pedal it switched on must stand on its
    // board — a sounding pedal that is not visible would be a lie — and
    // every row and fader shows where things are now.
    footView.tick();

    if (const auto footSerial = engine.getFoot().getChangeSerial(); footSerial != lastFootSerial)
    {
        lastFootSerial = footSerial;

        for (auto* strip : channelStrips)
            if (strip->getComponentID() != "master")
                syncLoadoutFromParams (strip->getComponentID());

        refreshAllControls();
    }

    const int failureMask = (engine.getTuneSlot().hasFailed() ? 1 : 0)
                              | (engine.getActiveInputCount() < 2 ? 4 : 0)
                              | (engine.getVocoderSlot().hasFailed() ? 8 : 0);

    if (failureMask != lastSlotFailures)
    {
        lastSlotFailures = failureMask;
        refreshSlotControls();
    }

    // A controller plugged in mid-session should just work: re-enable every
    // few seconds. Cheap — enumeration only, no device churn. The same beat
    // retries the virtual port if launch never got one — MIDIServer refuses
    // requests while it digests dead clients, and a show should recover from
    // that by itself rather than run portless.
    if (++midiInputRefresh >= 90)
    {
        midiInputRefresh = 0;
        engine.enableAllMidiInputs();

        if (! engine.getMidi().isOpen())
            engine.getMidi().open();
    }

    inputMeter.setLevel (0, engine.getInputPeak());
    vocalMeter.setLevel (0, engine.getVocalPeak());
    masterMeter.setLevel (0, engine.getOutputPeakL());
    masterMeter.setLevel (1, engine.getOutputPeakR());

    for (int i = 0; i < genericMeterKeys.size(); ++i)
        genericMeters[i]->setLevel (0, engine.getSlotPeak (slotOf (genericMeterKeys[i])));

    if (editorTabs.getCurrentTabIndex() == 1)
        tunerView.update (engine.getFxChain().getTrackedHz(),
                          engine.getFxChain().getTrackedConfidence(),
                          engine.getInputPeak());

    // Working limiter = the sum would have clipped by this much.
    const float gr = engine.getLimiterReductionDb();
    if (masterStrip != nullptr)
        masterStrip->setDetail (gr > 0.2f ? "LIM -" + juce::String (gr, 1) + " dB"
                                          : juce::String());

    const bool playing = player.isPlaying();
    transportButton.setToggleState (playing, juce::dontSendNotification);
    transportButton.setGlyph (playing ? IconButton::Glyph::stop : IconButton::Glyph::play);
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

    if (++saveCounter >= 90)   // every three seconds, if anything moved
    {
        saveCounter = 0;

        if (paramsChangedSinceSnapshot())
            saveSettings();
    }
}

} // namespace rig
