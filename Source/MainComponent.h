#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "AudioEngine.h"
#include "Parameters.h"
#include "SettingsStore.h"
#include "SongMaker.h"
#include "ui/SongMakerPanel.h"
#include "ui/Widgets.h"

namespace rig
{

class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    MainComponent (ParamStore& params, AudioEngine& engine, SettingsStore& settings);
    ~MainComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void parentHierarchyChanged() override;

    void saveSettings();

private:
    void timerCallback() override;
    void chooseSongFolder();
    void startNewSong();
    void chooseAudioFile();
    void showDeviceSettings();
    void loadSong (const juce::File& folder);
    bool paramsChangedSinceSnapshot();

    ParamStore& params;
    AudioEngine& engine;
    SettingsStore& settings;

    juce::TextButton loadButton { "Load song..." };
    juce::TextButton newSongButton { "New song..." };
    juce::TextButton transportButton { "PLAY" };
    juce::TextButton deviceButton { "Audio device..." };
    juce::Label songLabel, statusLabel, latencyLabel, stemsHeader;

    LevelMeter masterMeter { 2 };
    LevelMeter inputMeter { 1 };

    juce::OwnedArray<StemStrip> stemStrips;
    juce::Viewport fxViewport;
    juce::Component fxHolder;
    juce::OwnedArray<FxPanel> panels;

    std::unique_ptr<juce::FileChooser> chooser;

    SongMaker songMaker;
    SongMakerPanel songPanel { songMaker };

    bool presentCache[StemPlayer::numStems] { true, true, true, true };
    float snapshot[(size_t) NumParams] {};
    int saveCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace rig
