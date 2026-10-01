#include "FootControl.h"

namespace rig
{

static const char* const triggerNames[] { "program", "controller", "start", "stop", "continue" };
static const char* const kindNames[]    { "preset", "latch", "momentary", "sweep" };
static const char* const verbNames[]    { "follow", "flip", "on", "off" };

template <size_t N>
static int indexOfName (const char* const (&names)[N], const juce::String& wanted, int fallback)
{
    for (size_t i = 0; i < N; ++i)
        if (wanted == names[i])
            return (int) i;

    return fallback;
}

static bool usesNumber (FootTrigger::Type type) noexcept
{
    return type == FootTrigger::program || type == FootTrigger::controller;
}

bool FootTrigger::matches (const FootTrigger& heard) const noexcept
{
    return type == heard.type
            && (! usesNumber (type) || number == heard.number)
            && (channel == 0 || ! usesNumber (type) || channel == heard.channel);
}

bool FootTrigger::sameWire (const FootTrigger& other) const noexcept
{
    return type == other.type
            && (! usesNumber (type) || (number == other.number && channel == other.channel));
}

juce::String FootTrigger::describe() const
{
    static const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));

    switch (type)
    {
        case start:   return "Start";
        case stop:    return "Stop";
        case resume:  return "Continue";
        case program:
        case controller:
            break;
    }

    return (type == program ? "Program change " : "Controller ") + juce::String (number)
             + dot + (channel == 0 ? juce::String ("any channel") : "ch " + juce::String (channel));
}

//==============================================================================
FootLayout* FootMap::activeLayout() noexcept
{
    for (auto& layout : layouts)
        if (layout.name == active)
            return &layout;

    return layouts.empty() ? nullptr : &layouts.front();
}

const FootLayout* FootMap::activeLayout() const noexcept
{
    return const_cast<FootMap*> (this)->activeLayout();
}

const FootControl* FootMap::findControl (const juce::String& id) const noexcept
{
    for (const auto& control : controls)
        if (control.id == id)
            return &control;

    return nullptr;
}

juce::String FootMap::freshControlId() const
{
    for (int n = 1;; ++n)
    {
        const juce::String id = "c" + juce::String (n);

        if (findControl (id) == nullptr)
            return id;
    }
}

juce::var FootMap::toVar() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("device", device);
    root->setProperty ("active", active);

    juce::Array<juce::var> controlList;

    for (const auto& c : controls)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", c.id);
        o->setProperty ("name", c.name);
        o->setProperty ("type", triggerNames[c.trigger.type]);
        o->setProperty ("channel", c.trigger.channel);
        o->setProperty ("number", c.trigger.number);
        o->setProperty ("kind", kindNames[c.kind]);
        controlList.add (juce::var (o));
    }

    root->setProperty ("controls", controlList);

    juce::Array<juce::var> layoutList;

    for (const auto& layout : layouts)
    {
        juce::Array<juce::var> jobList;

        for (const auto& job : layout.jobs)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("control", job.control);
            o->setProperty ("action", job.action);
            o->setProperty ("verb", verbNames[job.verb]);
            o->setProperty ("lo", job.lo);
            o->setProperty ("hi", job.hi);
            jobList.add (juce::var (o));
        }

        auto* o = new juce::DynamicObject();
        o->setProperty ("name", layout.name);
        o->setProperty ("jobs", jobList);
        layoutList.add (juce::var (o));
    }

    root->setProperty ("layouts", layoutList);
    return juce::var (root);
}

// Anything the file gets wrong is dropped rather than obeyed: a control
// without an id, a second control wearing the same one, a job for a control
// that is gone. What survives is always a map the engine can run.
FootMap FootMap::fromVar (const juce::var& v)
{
    FootMap map;

    if (auto* root = v.getDynamicObject())
    {
        map.device = root->getProperty ("device").toString();
        map.active = root->getProperty ("active").toString();

        if (auto* controlList = root->getProperty ("controls").getArray())
        {
            for (const auto& item : *controlList)
            {
                auto* o = item.getDynamicObject();

                if (o == nullptr)
                    continue;

                FootControl c;
                c.id = o->getProperty ("id").toString();
                c.name = o->getProperty ("name").toString();
                c.trigger.type = (FootTrigger::Type) indexOfName (triggerNames, o->getProperty ("type").toString(), 0);
                c.trigger.channel = juce::jlimit (0, 16, (int) o->getProperty ("channel"));
                c.trigger.number = juce::jlimit (0, 127, (int) o->getProperty ("number"));
                c.kind = (FootControl::Kind) indexOfName (kindNames, o->getProperty ("kind").toString(), 0);

                // Only a controller carries a value; everything else is a press.
                if (c.trigger.type != FootTrigger::controller)
                    c.kind = FootControl::preset;

                if (c.id.isNotEmpty() && map.findControl (c.id) == nullptr)
                    map.controls.push_back (c);
            }
        }

        if (auto* layoutList = root->getProperty ("layouts").getArray())
        {
            for (const auto& item : *layoutList)
            {
                auto* o = item.getDynamicObject();

                if (o == nullptr)
                    continue;

                FootLayout layout;
                layout.name = o->getProperty ("name").toString();

                bool taken = layout.name.isEmpty();

                for (const auto& other : map.layouts)
                    taken = taken || other.name == layout.name;

                if (taken)
                    continue;

                if (auto* jobList = o->getProperty ("jobs").getArray())
                {
                    for (const auto& jobItem : *jobList)
                    {
                        auto* jo = jobItem.getDynamicObject();

                        if (jo == nullptr)
                            continue;

                        FootJob job;
                        job.control = jo->getProperty ("control").toString();
                        job.action = jo->getProperty ("action").toString();
                        job.verb = (FootJob::Verb) indexOfName (verbNames, jo->getProperty ("verb").toString(), 0);
                        job.lo = (float) (double) jo->getProperty ("lo");
                        job.hi = jo->hasProperty ("hi") ? (float) (double) jo->getProperty ("hi") : 1.0f;

                        if (job.action.isNotEmpty() && map.findControl (job.control) != nullptr)
                            layout.jobs.push_back (job);
                    }
                }

                map.layouts.push_back (layout);
            }
        }
    }

    if (map.layouts.empty())
        map.layouts.push_back ({ "Default", {} });

    map.active = map.activeLayout()->name;
    return map;
}

FootMap FootMap::factory()
{
    FootMap map;
    FootLayout layout;
    layout.name = "Default";

    // 0 silence, 1 autotune, 2 vocoder — one press sets both lanes.
    const bool lanes[3][2] { { false, false }, { true, false }, { false, true } };

    for (int n = 0; n < 3; ++n)
    {
        FootControl c;
        c.id = "c" + juce::String (n + 1);
        c.name = "Program " + juce::String (n);
        c.trigger = { FootTrigger::program, 0, n };
        c.kind = FootControl::preset;
        map.controls.push_back (c);

        layout.jobs.push_back ({ c.id, "mic/vocalOn",   lanes[n][0] ? FootJob::on : FootJob::off, 0.0f, 1.0f });
        layout.jobs.push_back ({ c.id, "mic/vocoderOn", lanes[n][1] ? FootJob::on : FootJob::off, 0.0f, 1.0f });
    }

    map.layouts.push_back (layout);
    map.active = layout.name;
    return map;
}

juce::File FootMap::getFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
              .getChildFile ("Application Support")
              .getChildFile ("LiveRig")
              .getChildFile ("foot.json");
}

FootMap FootMap::loadFrom (const juce::File& file)
{
    if (! file.existsAsFile())
        return factory();

    const auto parsed = juce::JSON::parse (file.loadFileAsString());

    if (parsed.getDynamicObject() == nullptr)
    {
        // Unreadable is not the same as absent: the next edit would write
        // over whatever was in there, so set it aside first.
        file.copyFileTo (file.getSiblingFile (file.getFileName() + ".unreadable"));
        return factory();
    }

    return fromVar (parsed);
}

bool FootMap::saveTo (const juce::File& file) const
{
    file.getParentDirectory().createDirectory();
    return file.replaceWithText (juce::JSON::toString (toVar(), false));
}

//==============================================================================
bool isFootCommand (const juce::String& action) noexcept
{
    return action.startsWith ("do/");
}

bool parseFootParamAction (const juce::String& action, FootParamTarget& out) noexcept
{
    if (isFootCommand (action) || ! action.containsChar ('/'))
        return false;

    const auto channel = action.upToFirstOccurrenceOf ("/", false, false);
    const int slot = channel == "rig" ? 0 : channelSlotForKey (channel);
    const int param = paramIndexForId (action.fromFirstOccurrenceOf ("/", false, false));

    if (slot < 0 || param < 0)
        return false;

    const auto& s = spec (param);

    out.slot = slot;
    out.param = param;
    out.isSwitch = s.isToggle
                    || (juce::exactlyEqual (s.step, 1.0f) && juce::exactlyEqual (s.maxValue - s.minValue, 1.0f));
    return true;
}

//==============================================================================
FootEngine::FootEngine (ParamStore& p) : params (p)
{
    setMap (FootMap::factory());
}

void FootEngine::setMap (const FootMap& newMap)
{
    const juce::ScopedLock sl (lock);
    map = newMap;

    if (map.layouts.empty())
        map.layouts.push_back ({ "Default", {} });

    map.active = map.activeLayout()->name;
    rebuild();
}

FootMap FootEngine::getMap() const
{
    const juce::ScopedLock sl (lock);
    return map;
}

void FootEngine::setActiveLayout (const juce::String& name)
{
    const juce::ScopedLock sl (lock);

    for (const auto& layout : map.layouts)
    {
        if (layout.name == name)
        {
            map.active = name;
            rebuild();
            return;
        }
    }
}

juce::String FootEngine::getActiveLayout() const
{
    const juce::ScopedLock sl (lock);
    return map.active;
}

// The map flattened for the MIDI thread: each control with the steps the
// layout in force gives it, actions already taken apart. A job whose action
// names nothing the rig has is left out — it will mean something again if
// the thing comes back, and until then it must not do anything else.
void FootEngine::rebuild()
{
    std::vector<Live> fresh;
    const auto* layout = map.activeLayout();

    for (const auto& control : map.controls)
    {
        Live entry;
        entry.control = control;

        for (const auto& old : live)
            if (old.control.id == control.id)
                entry.last = old.last;

        if (layout != nullptr)
        {
            for (const auto& job : layout->jobs)
            {
                if (job.control != control.id)
                    continue;

                Step step;
                step.job = job;
                step.isCommand = isFootCommand (job.action);

                if (step.isCommand || parseFootParamAction (job.action, step.target))
                    entry.steps.push_back (step);
            }
        }

        fresh.push_back (std::move (entry));
    }

    live = std::move (fresh);
}

void FootEngine::apply (Live& entry, int midiValue)
{
    // What the hardware said, in the two things a step can care about: where
    // the control stands, and whether this message is a press.
    float value = 1.0f;
    bool pressed = true;

    switch (entry.control.kind)
    {
        case FootControl::preset:
            break;

        case FootControl::latch:
            value = midiValue >= 64 ? 1.0f : 0.0f;
            break;

        case FootControl::momentary:
            value = midiValue >= 64 ? 1.0f : 0.0f;
            pressed = midiValue >= 64;
            break;

        case FootControl::sweep:
            value = (float) juce::jlimit (0, 127, midiValue) / 127.0f;
            pressed = value >= 0.5f && entry.last >= 0.0f && entry.last < 0.5f;
            break;
    }

    entry.last = value;

    bool moved = false, queued = false;

    for (const auto& step : entry.steps)
    {
        if (step.isCommand)
        {
            if (pressed)
            {
                commands.add (step.job.action);
                queued = true;
            }

            continue;
        }

        const auto& s = spec (step.target.param);
        const int slot = step.target.slot, param = step.target.param;

        if (step.target.isSwitch)
        {
            const bool now = params.get (slot, param) > 0.5f * (s.minValue + s.maxValue);
            bool wanted = now;

            switch (step.job.verb)
            {
                case FootJob::follow:  wanted = value >= 0.5f;                break;
                case FootJob::flip:    wanted = pressed ? ! now : now;        break;
                case FootJob::on:      wanted = pressed ? true : now;         break;
                case FootJob::off:     wanted = pressed ? false : now;        break;
            }

            if (wanted != now)
            {
                params.set (slot, param, wanted ? s.maxValue : s.minValue);
                moved = true;
            }

            continue;
        }

        float target = step.job.lo + value * (step.job.hi - step.job.lo);

        if (s.step > 0.0f)
            target = s.minValue + s.step * std::round ((target - s.minValue) / s.step);

        target = juce::jlimit (s.minValue, s.maxValue, target);

        if (! juce::exactlyEqual (target, params.get (slot, param)))
        {
            params.set (slot, param, target);
            moved = true;
        }
    }

    if (moved)
        changeSerial.fetch_add (1, std::memory_order_relaxed);

    if (queued && notifier != nullptr)
        notifier();
}

bool FootEngine::handleMidi (const juce::String& deviceName, const juce::MidiMessage& message,
                             juce::uint32 nowMs)
{
    FootTrigger heard;
    int value = 127;

    if (message.isProgramChange())
    {
        heard = { FootTrigger::program, message.getChannel(), message.getProgramChangeNumber() };
    }
    else if (message.isController())
    {
        heard = { FootTrigger::controller, message.getChannel(), message.getControllerNumber() };
        value = message.getControllerValue();
    }
    else if (message.isMidiStart())     heard = { FootTrigger::start, 0, 0 };
    else if (message.isMidiStop())      heard = { FootTrigger::stop, 0, 0 };
    else if (message.isMidiContinue())  heard = { FootTrigger::resume, 0, 0 };
    else
        return false;

    const juce::ScopedLock sl (lock);

    ++lastHeard.serial;
    lastHeard.trigger = heard;
    lastHeard.value = value;
    lastHeard.device = deviceName;
    lastHeard.controls.clear();

    // With a device chosen, only that box is obeyed — or learned from. The
    // readout above still shows the stranger, which is how a wrong choice
    // gets noticed.
    if (map.device.isNotEmpty() && map.device != deviceName)
        return true;

    if (learnState == LearnState::armed)
    {
        learned = { heard, FootControl::preset, deviceName };
        learnedAt = nowMs;
        learnFirstValue = value;
        learnSawMiddle = heard.type == FootTrigger::controller && value != 0 && value != 127;
        learnSawOther = false;
        learnState = LearnState::settling;
        return true;
    }

    if (learnState == LearnState::settling)
    {
        if (heard.type == FootTrigger::controller && heard.sameWire (learned.trigger))
        {
            learnSawMiddle = learnSawMiddle || (value != 0 && value != 127);
            learnSawOther = learnSawOther || value != learnFirstValue;
        }

        return true;
    }

    for (auto& entry : live)
    {
        if (entry.control.trigger.matches (heard))
        {
            lastHeard.controls.add (entry.control.id);
            apply (entry, value);
        }
    }

    return true;
}

void FootEngine::beginLearn()
{
    const juce::ScopedLock sl (lock);
    learnState = LearnState::armed;
}

void FootEngine::cancelLearn()
{
    const juce::ScopedLock sl (lock);
    learnState = LearnState::idle;
}

bool FootEngine::isLearning() const
{
    const juce::ScopedLock sl (lock);
    return learnState != LearnState::idle;
}

bool FootEngine::pollLearn (Learned& out, juce::uint32 nowMs)
{
    const juce::ScopedLock sl (lock);

    if (learnState != LearnState::settling)
        return false;

    if (learned.trigger.type == FootTrigger::controller)
    {
        if (nowMs - learnedAt < learnSettleMs)
            return false;

        // In-between values are a sweep. Two different ends inside the
        // window are a press and its release. One lone message is a switch
        // that keeps its own light.
        learned.kind = learnSawMiddle ? FootControl::sweep
                     : learnSawOther  ? FootControl::momentary
                                      : FootControl::latch;
    }

    learnState = LearnState::idle;
    out = learned;
    return true;
}

FootEngine::Heard FootEngine::getLastHeard() const
{
    const juce::ScopedLock sl (lock);
    return lastHeard;
}

float FootEngine::getControlValue (const juce::String& controlId) const
{
    const juce::ScopedLock sl (lock);

    for (const auto& entry : live)
        if (entry.control.id == controlId)
            return entry.last;

    return -1.0f;
}

void FootEngine::setCommandNotifier (std::function<void()> notify)
{
    const juce::ScopedLock sl (lock);
    notifier = std::move (notify);
}

juce::StringArray FootEngine::takeCommands()
{
    const juce::ScopedLock sl (lock);
    juce::StringArray taken;
    taken.swapWith (commands);
    return taken;
}

} // namespace rig
