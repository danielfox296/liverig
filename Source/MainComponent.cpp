#include "MainComponent.h"

namespace rig
{

static constexpr int panelHeight = 84;
static constexpr int panelGap = 4;

MainComponent::MainComponent (ParamStore& p, AudioEngine& e, SettingsStore& s)
    : params (p), engine (e), settings (s)
{
    for (auto* b : { &loadButton, &newSongButton, &transportButton, &deviceButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    transportButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3f9d4a));
    loadButton.onClick = [this] { chooseSongFolder(); };
    newSongButton.onClick = [this] { startNewSong(); };
    deviceButton.onClick = [this] { showDeviceSettings(); };
    transportButton.onClick = [this] { engine.getStemPlayer().togglePlay(); };

    stemsHeader.setText ("STEMS", juce::dontSendNotification);
    stemsHeader.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    stemsHeader.setColour (juce::Label::textColourId, juce::Colour (0xff70737c));
    addAndMakeVisible (stemsHeader);

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
        fxHolder.addAndMakeVisible (panel);

    fxViewport.setViewedComponent (&fxHolder, false);
    fxViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (fxViewport);

    songPanel.onSongReady = [this] (juce::File folder) { loadSong (folder); grabKeyboardFocus(); };
    songPanel.onSplitterInstalled = [this] { chooseAudioFile(); };
    addChildComponent (songPanel);

    setWantsKeyboardFocus (true);

    for (int i = 0; i < NumParams; ++i)
        snapshot[(size_t) i] = params.get (i);

    if (settings.songFolder.isNotEmpty())
    {
        const juce::File folder (settings.songFolder);

        if (folder.isDirectory())
            loadSong (folder);
    }

    startTimerHz (30);
    setSize (960, 920);
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
    auto b = getLocalBounds().reduced (10);

    songPanel.setBounds (getLocalBounds());

    auto top = b.removeFromTop (44);
    loadButton.setBounds (top.removeFromLeft (110).reduced (0, 6));
    top.removeFromLeft (8);
    newSongButton.setBounds (top.removeFromLeft (110).reduced (0, 6));
    top.removeFromLeft (8);
    transportButton.setBounds (top.removeFromLeft (88).reduced (0, 6));
    top.removeFromLeft (10);
    masterMeter.setBounds (top.removeFromRight (170).reduced (0, 13));
    top.removeFromRight (10);
    songLabel.setBounds (top);

    b.removeFromTop (8);

    auto footer = b.removeFromBottom (32);
    deviceButton.setBounds (footer.removeFromLeft (132).reduced (0, 3));
    footer.removeFromLeft (10);
    latencyLabel.setBounds (footer.removeFromRight (190));
    statusLabel.setBounds (footer);

    b.removeFromBottom (8);

    auto left = b.removeFromLeft (268);
    stemsHeader.setBounds (left.removeFromTop (18));
    left.removeFromTop (4);

    const int stripWidth = (left.getWidth() - 3 * 6) / 4;

    for (auto* strip : stemStrips)
    {
        strip->setBounds (left.removeFromLeft (stripWidth));
        left.removeFromLeft (6);
    }

    b.removeFromLeft (10);
    fxViewport.setBounds (b);

    const int w = b.getWidth() - fxViewport.getScrollBarThickness() - 2;
    int y = 0;

    for (auto* panel : panels)
    {
        panel->setBounds (0, y, w, panelHeight);
        y += panelHeight + panelGap;
    }

    fxHolder.setSize (w, y);
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

void MainComponent::chooseSongFolder()
{
    juce::File start ("/Users/fox296/LiveRig/Songs");

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

    songLabel.setColour (juce::Label::textColourId, juce::Colour (0xffc8c9ce));
    songLabel.setText (engine.getStemPlayer().getSongName()
                        + "   " + juce::String (engine.getStemPlayer().getLengthSeconds(), 1) + " s",
                       juce::dontSendNotification);

    settings.songFolder = folder.getFullPathName();
    saveSettings();
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

    settings.save (params);
}

void MainComponent::timerCallback()
{
    if (songPanel.isVisible())
        songPanel.refresh();

    auto& player = engine.getStemPlayer();

    if (player.reloadNeeded())
        player.reloadForCurrentRate();

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
