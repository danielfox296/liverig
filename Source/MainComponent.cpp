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

    // The engine already runs; tell it which channels the lanes live on.
    engine.setInputRoles (settings.bassInput, settings.micInput);

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
        addAndMakeVisible (stemStrips.add (new StemStrip (params,
                                                          juce::String (StemPlayer::stemNames[i]).toUpperCase(),
                                                          pStemGain0 + i,
                                                          pStemMute0 + i)));

    // The whole song's volume stands at the end of the stem row — the mix
    // board's TOTAL fader, not a rider under the transport.
    addAndMakeVisible (stemStrips.add (new StemStrip (params, "TOTAL", pTrackGain, -1)));

    fxDefs = {
        { "input",  -1,                false, "",       "Input",      pInMute,   { pInGain } },
        { "vocal",  -1,                false, "",       "Autotune",   pVocalOn,  { pVocalGain, pVocalGate, pVocalLevel } },
        { "vocoder",-1,                false, "",       "Vocoder",    pVocoderOn,{ pVocalGain, pVocalGate, pVocoderLevel } },
        { "midi",   -1,                false, "midi",   "MIDI",       pMidiOn,   { pMidiChannel, pMidiTranspose, pMidiGate, pMidiHold, pMidiRelease, pMidiBendRange, pMidiExpression, pMidiThru, pInstOn, pInstLevel } },
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
        { "reverb", -1,                false, "reverb", "Reverb",     pRevOn,    { pRevSize, pRevDamp, pRevPre, pRevMod, pRevMix } },
        { "out",    -1,                false, "",       "Live out",   -1,        { pLiveOut } },
        { "master", -1,                false, "",       "Master",     -1,        { pMasterGain } },
    };

    applyChainOrder (settings.chainOrder);

    for (const auto& def : fxDefs)
    {
        // The meters live on the channel strips now; only the MIDI row keeps
        // an extra — the note readout and PANIC.
        auto* panel = panels.add (new FxPanel (params, def.name, def.onParam, def.movable,
                                               def.key == "midi" ? &midiStatus : nullptr, 90,
                                               def.onParam == pInMute));

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
        }

        fxHolder.addAndMakeVisible (panel);
    }

    renumberFxTitles();
    pushOrderToChain();

    // The rail: strips come from the settings' channel list, and the player
    // grows or shrinks it by hand. The + at the end offers what can be added.
    addChannelButton.setWantsKeyboardFocus (false);
    addChannelButton.onClick = [this] { showAddChannelMenu(); };
    addAndMakeVisible (addChannelButton);

    rebuildChannelRail();

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
    addAndMakeVisible (editorTabs);
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

    refreshInstrumentControls();

    if (settings.instrument.isNotEmpty())
        engine.getInstrument().restore (settings.instrument, settings.instrumentState);

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
    fxViewport.setBounds (b.removeFromLeft (244));
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

    if (key == "midi")
        return "keys";

    if (key == "master")
        return "master";

    return "bass";
}

juce::Array<int> MainComponent::visibleFxIndices() const
{
    juce::Array<int> visible;
    auto loaded = juce::StringArray::fromTokens (loadoutFor (selectedChannel), ",", "");
    loaded.trim();

    for (int i = 0; i < (int) fxDefs.size(); ++i)
    {
        const auto& def = fxDefs[(size_t) i];

        if (channelOfKey (def.key) != selectedChannel)
            continue;

        if (isLoadable (def) && ! loaded.contains (def.key))
            continue;

        visible.add (i);
    }

    return visible;
}

juce::String MainComponent::loadoutFor (const juce::String& channel) const
{
    return channel == "bass" ? settings.bassLoadout
         : channel == "mic"  ? settings.micLoadout
         : channel == "keys" ? settings.keysLoadout
                             : juce::String();
}

juce::String& MainComponent::loadoutRef (const juce::String& channel)
{
    static juce::String none;   // the master loads nothing; writes here go nowhere

    return channel == "bass" ? settings.bassLoadout
         : channel == "mic"  ? settings.micLoadout
         : channel == "keys" ? settings.keysLoadout
                             : none;
}

juce::Array<int> MainComponent::availableFxIndices() const
{
    juce::Array<int> out;
    auto loaded = juce::StringArray::fromTokens (loadoutFor (selectedChannel), ",", "");
    loaded.trim();

    for (int i = 0; i < (int) fxDefs.size(); ++i)
    {
        const auto& def = fxDefs[(size_t) i];

        if (channelOfKey (def.key) == selectedChannel && isLoadable (def)
             && ! loaded.contains (def.key))
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
void MainComponent::addFxToChannel (const juce::String& key)
{
    auto list = juce::StringArray::fromTokens (loadoutFor (selectedChannel), ",", "");
    list.trim();
    list.removeEmptyStrings();

    if (! list.contains (key))
        list.add (key);

    loadoutRef (selectedChannel) = list.joinIntoString (",");

    for (int i = 0; i < (int) fxDefs.size(); ++i)
    {
        if (fxDefs[(size_t) i].key != key)
            continue;

        if (fxDefs[(size_t) i].onParam >= 0)
            params.set (fxDefs[(size_t) i].onParam, 1.0f);

        panels[i]->refresh();
        layoutFxPanels();
        selectFx (i);
        break;
    }

    saveSettings();
}

// Off the board and off in the engine — a hidden pedal must not sound. Its
// knobs keep their values for a return.
void MainComponent::removeFxFromChannel (const juce::String& key)
{
    const auto channel = channelOfKey (key);

    auto list = juce::StringArray::fromTokens (loadoutFor (channel), ",", "");
    list.trim();
    list.removeString (key);
    loadoutRef (channel) = list.joinIntoString (",");

    for (int i = 0; i < (int) fxDefs.size(); ++i)
    {
        if (fxDefs[(size_t) i].key != key)
            continue;

        if (fxDefs[(size_t) i].onParam >= 0)
            params.set (fxDefs[(size_t) i].onParam, 0.0f);

        panels[i]->refresh();

        if (selectedFx == i)
            selectFx (-1);

        break;
    }

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
        if (channelOfKey (def.key) != channel || ! isLoadable (def) || def.onParam < 0)
            continue;

        if (params.getBool (def.onParam) && ! list.contains (def.key))
        {
            list.add (def.key);
            grown = true;
        }
    }

    if (grown)
    {
        loadoutRef (channel) = list.joinIntoString (",");
        layoutFxPanels();
        saveSettings();
    }
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
    channelStrips.clear();
    masterStrip = nullptr;

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
        for (const auto& rd : defs)
        {
            if (key != rd.key)
                continue;

            auto* strip = channelStrips.add (new ChannelStrip (rd.key, rd.name, rd.meter));
            strip->onSelect = [this, key] { selectChannel (key); };
            addAndMakeVisible (strip);

            if (rd.savedName->isNotEmpty())
                strip->setChannelName (*rd.savedName);

            strip->onRename = [this, saved = rd.savedName] (const juce::String& newName)
            {
                *saved = newName;
                saveSettings();
                refreshPresetList();   // the preset placeholder wears the channel's name
            };

            strip->onContextMenu = [this, key, strip]
            {
                juce::PopupMenu m;
                m.addItem ("Remove " + strip->getName() + "...",
                           [this, key, name = strip->getName()]
                {
                    juce::AlertWindow::showOkCancelBox (
                        juce::MessageBoxIconType::QuestionIcon,
                        "Remove " + name + "?",
                        "The strip leaves the rail and its sound stops. "
                        "Its knobs stay saved for a return.",
                        "Remove", "Keep", this,
                        juce::ModalCallbackFunction::create ([this, key] (int choice)
                        {
                            if (choice == 1)
                                removeChannel (key);
                        }));
                });

                m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (strip));
            };
        }
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

// What the + can offer: one strip per kind the engine truly runs today.
// More kinds join the menu as their engines arrive.
void MainComponent::showAddChannelMenu()
{
    juce::StringArray present;

    for (auto* strip : channelStrips)
        present.add (strip->getComponentID());

    struct Kind { const char* key; const char* label; };
    const Kind kinds[] = { { "bass", "Bass" }, { "mic", "Vocal" }, { "keys", "Keys" } };

    juce::PopupMenu m;

    for (const auto& kind : kinds)
        m.addItem (kind.label, ! present.contains (kind.key), false,
                   [this, key = juce::String (kind.key)] { addChannel (key); });

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addChannelButton));
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

    if (removingSelected)
        selectChannel (channelStrips.getFirst()->getComponentID());

    refreshAllControls();   // the ON switches the removal flipped
    saveSettings();
    resized();
}

void MainComponent::selectChannel (const juce::String& channelKey)
{
    selectedChannel = channelKey;
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

    // Selecting a row pulls its channel forward, so preset recalls and the
    // snapshot tool land with the row actually on screen.
    if (selectedFx >= 0)
    {
        const auto channel = channelOfKey (fxDefs[(size_t) selectedFx].key);

        if (channel != selectedChannel)
        {
            selectedChannel = channel;
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
        else if (def.key == "midi")
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

void MainComponent::pushOrderToChain()
{
    int order[FxChain::numMovableStages] {};
    int movableRows = 0;

    for (const auto& def : fxDefs)
    {
        if (! def.movable)
            continue;

        if (movableRows < FxChain::numMovableStages)
            order[movableRows] = def.stage;

        ++movableRows;
    }

    // A stage added to fxDefs but not the Stage enum — or removed from only
    // one of them — would otherwise just stop reorders reaching the audio
    // thread, with nothing on screen to say so.
    jassert (movableRows == FxChain::numMovableStages);

    if (movableRows == FxChain::numMovableStages)
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

    // Reassemble: the pinned head — input and the lanes — then the movable
    // run, then reverb, live out, master. Anchoring on reverb rather than on
    // "index 1" keeps the lanes grouped under the input row however many of
    // them exist, and heals a layout saved while they drifted below the chain.
    auto anchor = fxDefs.begin();

    while (anchor != fxDefs.end() && anchor->key != "reverb")
        ++anchor;

    fxDefs.insert (anchor, movable.begin(), movable.end());
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

    if (to >= 0 && to != from && juce::isPositiveAndBelow (from, panels.size())
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
    return selectedChannel == "bass" ? displayName
                                     : selectedChannel + ":" + displayName;
}

juce::String MainComponent::channelDisplayName() const
{
    for (auto* strip : channelStrips)
        if (strip->getComponentID() == selectedChannel)
            return strip->getName();

    return selectedChannel;
}

juce::Array<int> MainComponent::channelParamIds() const
{
    juce::Array<int> ids;

    for (const auto& def : fxDefs)
    {
        if (channelOfKey (def.key) != selectedChannel)
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

    juce::StringArray display;

    for (const auto& stored : presets.getNames())
    {
        if (selectedChannel == "bass" && ! stored.contains (":"))
            display.add (stored);
        else if (stored.startsWith (selectedChannel + ":"))
            display.add (stored.fromFirstOccurrenceOf (":", false, false));
    }

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
    const auto key = presetKeyFor (presetBox.getText());

    if (presets.apply (key, params, channelParamIds()))
    {
        // Chain order and the amp capture only ever ride bass presets; on
        // the other channels both come back empty and these are no-ops.
        applyChainOrderLive (presets.getChainOrder (key));
        applyAmpCapture (presets.getAmpCapture (key));
        syncLoadoutFromParams (selectedChannel);
        refreshAllControls();
    }
}

void MainComponent::saveCurrentPreset()
{
    const auto name = presetBox.getText().trim();

    if (name.isEmpty())
        return;

    const bool bass = selectedChannel == "bass";

    presets.store (presetKeyFor (name), params,
                   bass ? chainOrderString() : juce::String(),
                   bass ? juce::var (engine.getFxChain().getAmp().getLoadedName())
                        : juce::var(),
                   channelParamIds());
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
