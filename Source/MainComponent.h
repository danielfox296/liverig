#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "AudioEngine.h"
#include "Parameters.h"
#include "PresetStore.h"
#include "SettingsStore.h"
#include "SongMaker.h"
#include "ui/MidiStatusView.h"
#include "ui/SongMakerPanel.h"
#include "ui/TunerView.h"
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
    void selectFxForSnapshot();
    void showTunerForSnapshot();
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
    void selectFx (int index);
    void renumberFxTitles();
    void pushOrderToChain();
    juce::String chainOrderString() const;
    void applyChainOrder (const juce::String& saved);
    void applyChainOrderLive (const juce::String& saved);
    void dragFxRow (FxPanel* panel, const juce::MouseEvent& e);
    void dropFxRow (FxPanel* panel);
    void loopTapped();
    void refreshLoopButton();

    void refreshPresetList (const juce::String& selectName = {});
    void applySelectedPreset();
    void saveCurrentPreset();
    void deleteSelectedPreset();
    void refreshAllControls();

    void chooseAmpCapture();
    void refreshAmpControls();
    void refreshInstrumentControls();
    void openPluginWindow();
    void loadAmpFile (const juce::File& file);
    void applyAmpCapture (const juce::var& captureName);

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
    juce::TextButton loopButton { "LOOP IN" };
    juce::TextButton deviceButton { "Audio device..." };
    juce::Label songLabel, statusLabel, latencyLabel, stemsHeader, songsHeader, grLabel;

    LevelMeter masterMeter { 2 };
    LevelMeter inputMeter { 1 };
    MidiStatusView midiStatus;

    juce::ListBox songList { "songs", this };
    juce::StringArray songNames;
    juce::File loadedFolder;

    juce::ComboBox presetBox;
    juce::TextButton savePresetButton { "Save" };
    juce::TextButton deletePresetButton { "Delete" };

    // What each chain stage exposes: a stable key for persistence, the
    // engine's stage id (movable rows only), preset scope, display name, ON
    // parameter and the parameters the editor shows when the row is selected.
    // The vector's order is the display order is the signal order.
    struct FxDef
    {
        juce::String key;
        int stage;       // FxChain::Stage, or -1 for the pinned rows
        bool movable;
        juce::String scope, name;
        int onParam;
        std::vector<int> ids;
    };

    // The Amp stage's capture picker, shown above its faders in the editor:
    // every imported capture in one box, plus import and clear.
    struct AmpControls : juce::Component
    {
        AmpControls()
        {
            load.setButtonText ("Import...");
            clear.setButtonText ("Clear");
            load.setWantsKeyboardFocus (false);
            clear.setWantsKeyboardFocus (false);
            captureBox.setTextWhenNothingSelected ("No capture");
            addAndMakeVisible (captureBox);
            addAndMakeVisible (load);
            addAndMakeVisible (clear);
        }

        void resized() override
        {
            auto b = getLocalBounds();
            load.setBounds (b.removeFromRight (82));
            b.removeFromRight (4);
            clear.setBounds (b.removeFromRight (52));
            b.removeFromRight (6);
            captureBox.setBounds (b);
        }

        juce::ComboBox captureBox;
        juce::TextButton load, clear;
    };

    AmpControls ampControls;

    // The Instrument row's picker, shown above its faders: every AU instrument
    // on the machine, and a way into the plugin's own window.
    struct InstrumentControls : juce::Component
    {
        InstrumentControls()
        {
            open.setButtonText ("Open...");
            clear.setButtonText ("Clear");
            open.setWantsKeyboardFocus (false);
            clear.setWantsKeyboardFocus (false);
            instrumentBox.setTextWhenNothingSelected ("No instrument");
            addAndMakeVisible (instrumentBox);
            addAndMakeVisible (open);
            addAndMakeVisible (clear);
        }

        void resized() override
        {
            auto b = getLocalBounds();
            open.setBounds (b.removeFromRight (72));
            b.removeFromRight (4);
            clear.setBounds (b.removeFromRight (52));
            b.removeFromRight (6);
            instrumentBox.setBounds (b);
        }

        juce::ComboBox instrumentBox;
        juce::TextButton open, clear;
    };

    InstrumentControls instrumentControls;
    std::unique_ptr<juce::DocumentWindow> pluginWindow;

    juce::OwnedArray<StemStrip> stemStrips;
    juce::Viewport fxViewport;
    juce::Component fxHolder;
    juce::OwnedArray<FxPanel> panels;
    std::vector<FxDef> fxDefs;
    FxEditor fxEditor { params, presets };
    TunerView tunerView;
    juce::TabbedComponent editorTabs { juce::TabbedButtonBar::TabsAtTop };
    int selectedFx = -1;

    FxPanel* draggedPanel = nullptr;
    int dragStartIndex = -1, dragTargetSlot = -1;

    std::unique_ptr<juce::FileChooser> chooser;

    SongMaker songMaker;
    SongMakerPanel songPanel { songMaker };
    JobStrip jobStrip { songMaker };
    bool jobStripShowing = false;
    bool instrumentScanning = true;

    bool presentCache[StemPlayer::numStems] { true, true, true, true };
    float snapshot[(size_t) NumParams] {};
    int saveCounter = 0;
    int pendingLoopIn = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace rig
