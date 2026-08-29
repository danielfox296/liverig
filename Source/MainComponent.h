#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "AudioEngine.h"
#include "Parameters.h"
#include "PresetStore.h"
#include "SettingsStore.h"
#include "SongMaker.h"
#include "ui/EqView.h"
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
    void selectEqForSnapshot();
    void selectVocalForSnapshot();
    void showTunerForSnapshot();
    void showJobStripForSnapshot();

    // What the instrument picker actually offers — an empty box was the bug.
    int getInstrumentChoiceCount() const { return instrumentControls.instrumentBox.getNumItems(); }

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
    void refreshSlotControls();
    void autoBindPitchPlugin();
    void openPluginWindow();
    void openPluginWindowFor (juce::AudioProcessor* plugin, const juce::String& title);
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
    LevelMeter vocalMeter { 1 };
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
            rescan.setButtonText ("Rescan");
            open.setWantsKeyboardFocus (false);
            clear.setWantsKeyboardFocus (false);
            rescan.setWantsKeyboardFocus (false);
            instrumentBox.setTextWhenNothingSelected ("No instrument");
            addAndMakeVisible (instrumentBox);
            addAndMakeVisible (open);
            addAndMakeVisible (clear);
            addAndMakeVisible (rescan);
        }

        void resized() override
        {
            auto b = getLocalBounds();
            open.setBounds (b.removeFromRight (66));
            b.removeFromRight (4);
            clear.setBounds (b.removeFromRight (50));
            b.removeFromRight (4);
            rescan.setBounds (b.removeFromRight (60));
            b.removeFromRight (6);
            instrumentBox.setBounds (b);
        }

        juce::ComboBox instrumentBox;
        juce::TextButton open, clear, rescan;
    };

    InstrumentControls instrumentControls;

    // One hosted-plugin role: a label naming it, the picker, and the way
    // into the plugin's own window. "None" empties the slot.
    struct SlotRow : juce::Component
    {
        explicit SlotRow (const juce::String& roleName)
        {
            label.setText (roleName, juce::dontSendNotification);
            label.setColour (juce::Label::textColourId, juce::Colour (0xff9a9da6));
            open.setButtonText ("UI");
            open.setWantsKeyboardFocus (false);
            box.setTextWhenNothingSelected ("None");
            addAndMakeVisible (label);
            addAndMakeVisible (box);
            addAndMakeVisible (open);
        }

        void resized() override
        {
            auto b = getLocalBounds();
            label.setBounds (b.removeFromLeft (64));
            open.setBounds (b.removeFromRight (40));
            b.removeFromRight (4);
            box.setBounds (b);
        }

        juce::Label label;
        juce::ComboBox box;
        juce::TextButton open;
    };

    // The Autotune row's editor header: no picker, no rack — the rig chooses
    // the pitch plugin itself from a short list of real autotunes. The label
    // says what it found (or what to install), and Key... opens the plugin's
    // own window, where key, scale and correction amount live.
    struct AutotuneControls : juce::Component
    {
        AutotuneControls()
        {
            status.setFont (juce::Font (juce::FontOptions (12.0f)));
            status.setColour (juce::Label::textColourId, juce::Colour (0xff9a9da6));
            open.setButtonText ("Key & style...");
            open.setWantsKeyboardFocus (false);
            addAndMakeVisible (status);
            addAndMakeVisible (open);
        }

        void resized() override
        {
            auto b = getLocalBounds();
            open.setBounds (b.removeFromRight (96));
            b.removeFromRight (6);
            status.setBounds (b);
        }

        juce::Label status;
        juce::TextButton open;
    };

    AutotuneControls autotuneControls;

    // The Pedal row's editor header: the insert's picker and its own line of
    // truth about latency.
    struct PedalControls : juce::Component
    {
        static constexpr int preferredHeight = 46;

        PedalControls()
        {
            status.setFont (juce::Font (juce::FontOptions (11.0f)));
            status.setColour (juce::Label::textColourId, juce::Colour (0xff70737c));
            addAndMakeVisible (pedal);
            addAndMakeVisible (status);
        }

        void resized() override
        {
            auto b = getLocalBounds();
            pedal.setBounds (b.removeFromTop (26));
            b.removeFromTop (2);
            status.setBounds (b);
        }

        SlotRow pedal { "Pedal" };
        juce::Label status;
    };

    PedalControls pedalControls;

    // The EQ's whole control surface — the curve is the interface, so the
    // editor shows it in place of fader rows.
    EqView eqControls { params };
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
    int lastVocalOn = -1;
    int midiInputRefresh = 0;
    int lastSlotFailures = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace rig
