#pragma once

#include <atomic>
#include <functional>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "Parameters.h"

namespace rig
{

// What a foot controller puts on the wire when something is stepped on. A
// channel of 0 answers on any channel.
struct FootTrigger
{
    enum Type { program, controller, start, stop, resume };

    Type type = program;
    int channel = 0;
    int number = 0;     // program or controller number; unused by the transport three

    bool matches (const FootTrigger& heard) const noexcept;
    bool sameWire (const FootTrigger& other) const noexcept;
    juce::String describe() const;
};

// One thing under the foot, by name. The trigger is a fact about the
// hardware and so is the kind: a preset button fires once, a latching switch
// alternates lit and dark with its own LED, a momentary one is down while
// held, and an expression pedal sweeps. Layouts speak to controls by id, so
// a control can be learned again — new box, new cable, reprogrammed button —
// and everything it did still happens.
struct FootControl
{
    enum Kind { preset, latch, momentary, sweep };

    juce::String id, name;
    FootTrigger trigger;
    Kind kind = preset;

    // Whether the control carries a position worth following, or only the
    // news that it was pressed.
    bool hasValue() const noexcept { return kind != preset; }
};

// One thing a control does in a layout. The action names a function of the
// rig: "bass/odOn" is a channel's switch or knob by its parameter id, and
// "do/play" is a command the window carries out. A switch takes a verb; a
// knob travels between lo (pedal up, light off) and hi (pedal down, light
// on), and a plain press sends it to hi.
struct FootJob
{
    enum Verb { follow, flip, on, off };

    juce::String control, action;
    Verb verb = follow;
    float lo = 0.0f, hi = 1.0f;
};

struct FootLayout
{
    juce::String name;
    std::vector<FootJob> jobs;
};

// The whole mapping: what is under the foot, and every named set of things
// it can do. One layout is in force at a time.
struct FootMap
{
    juce::String device;    // the MIDI input the controller arrives on, by name; empty hears all
    juce::String active;
    std::vector<FootControl> controls;
    std::vector<FootLayout> layouts;

    FootLayout* activeLayout() noexcept;
    const FootLayout* activeLayout() const noexcept;
    const FootControl* findControl (const juce::String& id) const noexcept;
    juce::String freshControlId() const;

    juce::var toVar() const;
    static FootMap fromVar (const juce::var& v);

    // What the rig did before any of this could be edited: Program Change
    // 0, 1 and 2 are mic lanes off, autotune, vocoder.
    static FootMap factory();

    static juce::File getFile();
    static FootMap loadFrom (const juce::File& file);
    bool saveTo (const juce::File& file) const;
};

// A parameter action taken apart: which channel's copy, which parameter, and
// whether it is a switch or a knob. Commands ("do/...") do not parse.
struct FootParamTarget
{
    int slot = 0, param = -1;
    bool isSwitch = false;
};

bool parseFootParamAction (const juce::String& action, FootParamTarget& out) noexcept;
bool isFootCommand (const juce::String& action) noexcept;

// The running half. MIDI arrives on CoreMIDI's thread; switches and knobs
// land straight in the parameter store, which is atomic, and commands queue
// for the window. The map is swapped whole under a lock the audio thread
// never sees.
class FootEngine
{
public:
    explicit FootEngine (ParamStore& p);

    void setMap (const FootMap& newMap);
    FootMap getMap() const;
    void setActiveLayout (const juce::String& name);
    juce::String getActiveLayout() const;

    // True when the message was a foot controller's kind — program change,
    // control change, start, stop, continue — whether or not anything was
    // listening for it.
    bool handleMidi (const juce::String& deviceName, const juce::MidiMessage& message,
                     juce::uint32 nowMs = juce::Time::getMillisecondCounter());

    // Learning: the next thing stepped on is captured instead of obeyed. A
    // controller is watched for a moment longer, because one message cannot
    // say whether it came from a switch or the first inch of a sweep.
    struct Learned
    {
        FootTrigger trigger;
        FootControl::Kind kind = FootControl::preset;
        juce::String device;
    };

    void beginLearn();
    void cancelLearn();
    bool isLearning() const;
    bool pollLearn (Learned& out, juce::uint32 nowMs = juce::Time::getMillisecondCounter());

    // The last thing heard from any controller, obeyed or not — the answer
    // to "is this box even talking to me".
    struct Heard
    {
        juce::uint32 serial = 0;
        FootTrigger trigger;
        int value = 0;
        juce::String device;
        juce::StringArray controls;   // ids of the controls it woke
    };

    Heard getLastHeard() const;

    // Where each control last stood, 0..1, or -1 if it has not been heard.
    float getControlValue (const juce::String& controlId) const;

    // Rises whenever the foot moved a switch or a knob, so the window can
    // catch its controls up.
    juce::uint32 getChangeSerial() const noexcept { return changeSerial.load (std::memory_order_relaxed); }

    // Commands wait here for the window. The notifier is called on the MIDI
    // thread and must only wake someone up. Each command remembers the
    // moment its switch was pressed — a tapped tempo is measured between
    // presses, not between the window getting round to them.
    void setCommandNotifier (std::function<void()> notify);
    juce::StringArray takeCommands (juce::Array<juce::uint32>* pressedAtMs = nullptr);

    static constexpr juce::uint32 learnSettleMs = 450;

private:
    struct Step
    {
        FootJob job;
        bool isCommand = false;
        FootParamTarget target;
    };

    struct Live
    {
        FootControl control;
        std::vector<Step> steps;
        float last = -1.0f;
    };

    void rebuild();
    void apply (Live& live, int midiValue, juce::uint32 nowMs);

    ParamStore& params;

    juce::CriticalSection lock;   // guards everything below; MIDI and message threads only
    FootMap map;
    std::vector<Live> live;

    enum class LearnState { idle, armed, settling };
    LearnState learnState = LearnState::idle;
    Learned learned;
    juce::uint32 learnedAt = 0;
    int learnFirstValue = 0;
    bool learnSawMiddle = false, learnSawOther = false;

    Heard lastHeard;
    juce::StringArray commands;
    juce::Array<juce::uint32> commandTimes;   // one per command
    std::function<void()> notifier;

    std::atomic<juce::uint32> changeSerial { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FootEngine)
};

} // namespace rig
