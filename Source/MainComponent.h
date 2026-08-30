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
    void selectInputForSnapshot();
    void selectEqForSnapshot();
    void selectVocalForSnapshot();
    void selectVocoderForSnapshot();
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
    void selectChannel (const juce::String& channelKey);
    void refreshStripSelection();
    juce::Array<int> visibleFxIndices() const;
    static juce::String channelOfKey (const juce::String& key);
    static int slotOf (const juce::String& channelKey);
    void renumberFxTitles();
    void pushOrderToChain (const juce::String& channelKey);
    juce::String orderStringFor (const juce::String& channelKey) const;
    void applyPresetOrder (const juce::String& channelKey, const juce::String& saved);
    void dragFxRow (FxPanel* panel, const juce::MouseEvent& e);
    void dropFxRow (FxPanel* panel);
    void loopTapped();
    void refreshLoopButton();

    void rebuildChannelRail();
    void showAddChannelMenu();
    void addChannel (const juce::String& key);
    void addGenericChannel();
    void removeChannel (const juce::String& key);

    juce::String loadoutFor (const juce::String& channel) const;
    void setLoadout (const juce::String& channel, const juce::String& value);
    juce::String genericField (const juce::String& key, int index) const;
    void setGenericField (const juce::String& key, int index, const juce::String& value);
    juce::Array<int> availableFxIndices() const;
    void showAddFxMenu();
    void addFxToChannel (const juce::String& key);
    void removeFxFromChannel (const juce::String& key);
    void syncLoadoutFromParams (const juce::String& channel);
    void refreshChainWiring();
    bool boardHasStages (const juce::String& channelKey) const;
    void rebuildSourceControls();

    void refreshPresetList (const juce::String& selectName = {});
    juce::String presetKeyFor (const juce::String& displayName) const;
    juce::String channelDisplayName() const;
    juce::Array<int> channelParamIds() const;
    juce::Array<int> applicableParamIds() const;
    void applySelectedPreset();
    void saveCurrentPreset();
    void deleteSelectedPreset();
    void refreshAllControls();

    void chooseAmpCapture();
    void refreshAmpControls();
    void refreshInstrumentControls();
    void refreshSlotControls();
    void autoBindHostedRoles();
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

    // Every chain row and editor fader reads and writes through this one
    // view; selecting a strip retunes it to that channel's slot.
    SlotParams uiParams { params };

    juce::TextButton menuButton { juce::CharPointer_UTF8 ("\xe2\x89\xa1") };   // the file menu
    IconButton transportButton;   // play / stop, drawn rather than written
    juce::TextButton loopButton { "LOOP IN" };
    juce::TextButton deviceButton { "Audio device..." };
    juce::Label songLabel, stemsHeader, songsHeader;

    LevelMeter masterMeter { 2 };
    LevelMeter inputMeter { 1 };
    LevelMeter vocalMeter { 1 };
    MidiStatusView midiStatus;

    juce::ListBox songList { "songs", this };
    juce::StringArray songNames;
    juce::File loadedFolder;
    juce::Rectangle<int> songsBox;   // the painted card behind the songs section

    juce::ComboBox presetBox;
    juce::TextButton savePresetButton { "Save" };
    juce::TextButton deletePresetButton { "Delete" };
    juce::TextButton addChannelButton { "+" };   // grows the rail
    juce::TextButton addFxButton { "ADD EFFECT..." };   // grows the board

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

    // What the player can put on and take off a board. Input, live out and
    // master are the strip's anatomy and always show; everything else is a
    // pedal that exists only when loaded.
    static bool isLoadable (const FxDef& def)
    {
        return def.movable || def.key == "reverb" || def.key == "vocal"
                           || def.key == "vocoder" || def.key == "midi";
    }

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

    // The INPUT row's editor extra: where this strip listens. Audio strips
    // pick a jack by its owner's name; the keys strip picks the MIDI device
    // under the player's hands.
    struct SourceControls : juce::Component
    {
        SourceControls() { addAndMakeVisible (box); }

        void resized() override { box.setBounds (getLocalBounds()); }

        juce::ComboBox box;
        juce::StringArray midiIds;   // parallel to the box on the keys strip
    };

    SourceControls sourceControls;

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

    // A hosted role's editor header: no picker, no rack — the rig chooses
    // the plugin itself from a short list of the real thing. The label says
    // what it found (or what to install), and the button opens the plugin's
    // own window, where its musical settings live.
    struct RoleControls : juce::Component
    {
        explicit RoleControls (const juce::String& buttonText)
        {
            status.setFont (juce::Font (juce::FontOptions (12.0f)));
            status.setColour (juce::Label::textColourId, juce::Colour (0xff9a9da6));
            open.setButtonText (buttonText);
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

    RoleControls autotuneControls { "Key & style..." };
    RoleControls vocoderControls { "Voicing..." };

    // The EQ's whole control surface — the curve is the interface, so the
    // editor shows it in place of fader rows.
    EqView eqControls { uiParams };
    std::unique_ptr<juce::DocumentWindow> pluginWindow;

    juce::OwnedArray<StemStrip> stemStrips;
    juce::OwnedArray<ChannelStrip> channelStrips;
    ChannelStrip* masterStrip = nullptr;   // owned by channelStrips; wears the limiter readout
    juce::String selectedChannel { "bass" };
    juce::Viewport fxViewport;
    juce::Component fxHolder;
    juce::OwnedArray<FxPanel> panels;
    std::vector<FxDef> fxDefs;
    FxEditor fxEditor { uiParams, presets };
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
    int lastMicLanes = -1;
    int midiInputRefresh = 0;
    int lastSlotFailures = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace rig
