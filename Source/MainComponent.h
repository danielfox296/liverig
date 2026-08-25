#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "AudioEngine.h"
#include "Parameters.h"
#include "PresetStore.h"
#include "SettingsStore.h"
#include "SongMaker.h"
#include "ui/SongMakerPanel.h"
#include "ui/Widgets.h"

namespace rig
{

class MainComponent : public juce::Component,
                      private juce::Timer,
                      private juce::ListBoxModel
{
public:
    MainComponent (ParamStore& params, AudioEngine& engine, SettingsStore& settings);
    ~MainComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void parentHierarchyChanged() override;

    void saveSettings();

    // Used by the offscreen snapshot tool.
    void expandAllForSnapshot();
    void showJobStripForSnapshot();

private:
    void timerCallback() override;

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent& e) override;

    void refreshSongList();
    void chooseSongFolder();
    void startNewSong();
    void chooseAudioFile();
    void showDeviceSettings();
    void loadSong (const juce::File& folder);
    void layoutFxPanels();

    void refreshPresetList (const juce::String& selectName = {});
    void applySelectedPreset();
    void saveCurrentPreset();
    void deleteSelectedPreset();
    void refreshAllControls();

    bool paramsChangedSinceSnapshot();
    juce::String expandedStateString() const;
    void applyExpandedState (const juce::String& state);

    ParamStore& params;
    AudioEngine& engine;
    SettingsStore& settings;
    PresetStore presets;

    juce::TextButton loadButton { "Load folder..." };
    juce::TextButton newSongButton { "New song..." };
    juce::TextButton transportButton { "PLAY" };
    juce::TextButton deviceButton { "Audio device..." };
    juce::Label songLabel, statusLabel, latencyLabel, stemsHeader, songsHeader;

    LevelMeter masterMeter { 2 };
    LevelMeter inputMeter { 1 };

    juce::ListBox songList { "songs", this };
    juce::StringArray songNames;
    juce::File loadedFolder;

    juce::ComboBox presetBox;
    juce::TextButton savePresetButton { "Save" };
    juce::TextButton deletePresetButton { "Delete" };

    juce::OwnedArray<StemStrip> stemStrips;
    juce::Viewport fxViewport;
    juce::Component fxHolder;
    juce::OwnedArray<FxPanel> panels;

    std::unique_ptr<juce::FileChooser> chooser;

    SongMaker songMaker;
    SongMakerPanel songPanel { songMaker };
    JobStrip jobStrip { songMaker };
    bool jobStripShowing = false;

    bool presentCache[StemPlayer::numStems] { true, true, true, true };
    float snapshot[(size_t) NumParams] {};
    int saveCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace rig
