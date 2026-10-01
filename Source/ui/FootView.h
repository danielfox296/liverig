#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../FootControl.h"
#include "../Parameters.h"
#include "Type.h"

namespace rig
{

// The foot tab: what is under the player's foot, and what each thing there
// does. A card per control — its name, what it sends, a lamp that shows it
// being stepped on — and under it the jobs the layout in force gives it.
// Layouts swap what the same controls do; learning a control again swaps
// the hardware under the same jobs.
//
// Every edit goes straight to the engine, so the next press already obeys
// it. The window says what the rig can do (the function menu) and how to
// name it; this view knows nothing about channels or effects itself.
class FootView : public juce::Component
{
public:
    using Chooser = std::function<void (const juce::String& action)>;

    FootView (ParamStore& p, FootEngine& e) : params (p), engine (e)
    {
        layoutBox.setEditableText (true);
        layoutBox.setTextWhenNothingSelected ("Layout");
        layoutBox.onChange = [this] { layoutBoxChanged(); };
        addAndMakeVisible (layoutBox);

        copyButton.setButtonText ("Copy");
        copyButton.setWantsKeyboardFocus (false);
        copyButton.onClick = [this] { copyLayout(); };
        addAndMakeVisible (copyButton);

        deleteButton.setButtonText ("Delete");
        deleteButton.setWantsKeyboardFocus (false);
        deleteButton.onClick = [this] { confirmDeleteLayout(); };
        addChildComponent (deleteButton);

        for (auto* l : { &deviceLabel, &heardLabel })
        {
            l->setFont (rigFont (12.0f));
            l->setColour (juce::Label::textColourId, juce::Colour (0xff9a9da6));
            addAndMakeVisible (l);
        }

        deviceLabel.setText ("Listening to", juce::dontSendNotification);
        heardLabel.setText ("Last heard", juce::dontSendNotification);

        deviceBox.onChange = [this] { deviceBoxChanged(); };
        addAndMakeVisible (deviceBox);

        heard.setFont (rigFont (12.0f));
        heard.setColour (juce::Label::textColourId, juce::Colour (0xff70737c));
        heard.setText ("nothing yet", juce::dontSendNotification);
        heard.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (heard);

        addButton.setWantsKeyboardFocus (false);
        addButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd87b3a));
        addButton.onClick = [this] { learnTapped ({}); };
        holder.addAndMakeVisible (addButton);

        viewport.setViewedComponent (&holder, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        reload();
    }

    ~FootView() override
    {
        engine.cancelLearn();
    }

    // The rig's functions, offered as a menu; picking one calls the chooser
    // with its action.
    std::function<void (juce::PopupMenu&, const Chooser&)> buildFunctionMenu;

    // An action in the player's words: "Bass · Overdrive".
    std::function<juce::String (const juce::String&)> describeAction;

    // The map changed and should be kept.
    std::function<void()> onEdited;

    // MIDI inputs that can never be a foot controller — the rig's own port.
    juce::StringArray hiddenDevices;

    // The engine's map is the truth again — at launch, and for the snapshots.
    void reload()
    {
        map = engine.getMap();
        refreshLayoutBox();
        refreshDeviceBox();
        rebuildCards();
    }

    // A layout chosen from outside the tab — by a foot, usually.
    void selectLayout (const juce::String& name)
    {
        for (const auto& layout : map.layouts)
        {
            if (layout.name == name && map.active != name)
            {
                map.active = name;
                push (true);
                refreshLayoutBox();
                rebuildCards();
                return;
            }
        }
    }

    juce::StringArray getLayoutNames() const
    {
        juce::StringArray names;

        for (const auto& layout : map.layouts)
            names.add (layout.name);

        return names;
    }

    // From the window's timer: the readout, the lamps, and a learn in flight.
    void tick()
    {
        const auto last = engine.getLastHeard();

        if (last.serial != heardSerial)
        {
            heardSerial = last.serial;
            heardGlow = 12;

            static const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
            auto text = last.trigger.describe();

            if (last.trigger.type == FootTrigger::controller)
                text << " = " << last.value;

            if (last.device.isNotEmpty())
                text << dot << last.device;

            heard.setText (text, juce::dontSendNotification);

            for (auto* card : cards)
                if (last.controls.contains (card->controlId))
                    card->flash();
        }

        if (heardGlow >= 0)
        {
            heard.setColour (juce::Label::textColourId,
                             juce::Colour (heardGlow > 0 ? 0xffd87b3a : 0xff9a9da6));
            --heardGlow;
        }

        for (auto* card : cards)
            card->tick (engine.getControlValue (card->controlId));

        FootEngine::Learned got;

        if (engine.pollLearn (got))
            learned (got);

        const bool learning = engine.isLearning();

        if (learning != showingLearn)
        {
            showingLearn = learning;

            if (! learning)
                learnTarget.clear();

            refreshAddButton();

            for (auto* card : cards)
                card->setLearning (learning && card->controlId == learnTarget);
        }

        if (++deviceRefresh >= 90 && ! deviceBox.isPopupActive())
        {
            deviceRefresh = 0;
            refreshDeviceBox();
        }
    }

    void visibilityChanged() override
    {
        // A learn left armed behind a hidden tab would eat the next press.
        if (! isShowing())
            engine.cancelLearn();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff17181c));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        g.setColour (juce::Colour (0xff26272d));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (12, 8);

        auto top = b.removeFromTop (30).reduced (0, 3);

        if (deleteButton.isVisible())
        {
            deleteButton.setBounds (top.removeFromRight (62));
            top.removeFromRight (4);
        }

        copyButton.setBounds (top.removeFromRight (52));
        top.removeFromRight (6);
        layoutBox.setBounds (top);

        b.removeFromTop (2);
        auto deviceRow = b.removeFromTop (28).reduced (0, 2);
        deviceLabel.setBounds (deviceRow.removeFromLeft (88));
        deviceBox.setBounds (deviceRow);

        auto heardRow = b.removeFromTop (24);
        heardLabel.setBounds (heardRow.removeFromLeft (88));
        heard.setBounds (heardRow);

        b.removeFromTop (6);
        viewport.setBounds (b);
        layoutCards();
    }

private:
    static constexpr int cardGap = 7;
    static constexpr int rowHeight = 30;

    //==========================================================================
    // One job on a control's card. The control matches the shape of the
    // choice, as everywhere in the rig: a switch function takes a short menu
    // of what the press does to it, a knob takes a fader for where it lands
    // — two thumbs when the control travels, one when it only presses — and
    // a command needs nothing at all.
    class JobRow : public juce::Component
    {
    public:
        JobRow (FootView& o, int jobIndex, const FootJob& job, bool controlHasValue)
            : owner (o), index (jobIndex), valued (controlHasValue)
        {
            name.setText (owner.describeAction != nullptr ? owner.describeAction (job.action) : job.action,
                          juce::dontSendNotification);
            name.setFont (rigFont (12.0f));
            name.setMinimumHorizontalScale (0.75f);
            name.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (name);

            FootParamTarget target;

            if (! parseFootParamAction (job.action, target))
                return;   // a command: the name is the whole row

            param = target.param;

            if (target.isSwitch)
            {
                // What cannot happen is absent: a press has no position to follow.
                if (valued)
                    verbs.addItem ("Follows it", FootJob::follow + 1);

                verbs.addItem ("Turns on", FootJob::on + 1);
                verbs.addItem ("Turns off", FootJob::off + 1);
                verbs.addItem ("Flips", FootJob::flip + 1);
                verbs.setSelectedId ((! valued && job.verb == FootJob::follow ? FootJob::on : job.verb) + 1,
                                     juce::dontSendNotification);
                verbs.onChange = [this]
                {
                    owner.setVerb (index, (FootJob::Verb) (verbs.getSelectedId() - 1));
                };
                addAndMakeVisible (verbs);
                return;
            }

            const auto& s = spec (param);
            juce::NormalisableRange<double> range ((double) s.minValue, (double) s.maxValue, (double) s.step);

            if (s.skewCentre > 0.0f)
                range.setSkewForCentre ((double) s.skewCentre);

            fader.setNormalisableRange (range);
            fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);

            lo = juce::jlimit (s.minValue, s.maxValue, job.lo);
            hi = juce::jlimit (s.minValue, s.maxValue, job.hi);
            reversed = lo > hi;

            if (valued)
            {
                fader.setSliderStyle (juce::Slider::TwoValueHorizontal);
                fader.setMinAndMaxValues ((double) juce::jmin (lo, hi), (double) juce::jmax (lo, hi),
                                          juce::dontSendNotification);
            }
            else
            {
                fader.setSliderStyle (juce::Slider::LinearHorizontal);
                fader.setSliderSnapsToMousePosition (true);
                fader.setValue ((double) hi, juce::dontSendNotification);
            }

            fader.onValueChange = [this]
            {
                if (valued)
                {
                    const float a = (float) fader.getMinValue(), b = (float) fader.getMaxValue();
                    lo = reversed ? b : a;
                    hi = reversed ? a : b;
                }
                else
                {
                    lo = hi = (float) fader.getValue();
                }

                owner.setRange (index, lo, hi);
                updateValueText();
            };
            fader.onDragEnd = [this] { owner.keep(); };
            addAndMakeVisible (fader);

            value.setFont (rigFont (11.0f));
            value.setJustificationType (juce::Justification::centredRight);
            value.setMinimumHorizontalScale (0.7f);
            value.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (value);
            updateValueText();
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (! e.mods.isPopupMenu())
                return;

            juce::Component::SafePointer<FootView> view (&owner);
            juce::PopupMenu m;

            if (param >= 0 && valued && fader.isVisible())
                m.addItem ("Swap ends", [view, i = index] { if (view != nullptr) view->swapEnds (i); });

            m.addItem ("Remove", [view, i = index] { if (view != nullptr) view->removeJob (i); });
            m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced (0, 3);

            if (verbs.isVisible())
            {
                verbs.setBounds (b.removeFromRight (112));
                b.removeFromRight (6);
            }
            else if (fader.isVisible())
            {
                value.setBounds (b.removeFromRight (90));
                b.removeFromRight (4);
                fader.setBounds (b.removeFromRight (juce::jmax (90, b.getWidth() / 2)));
                b.removeFromRight (6);
            }

            name.setBounds (b);
        }

    private:
        void updateValueText()
        {
            const auto& s = spec (param);
            const auto text = [&s] (float v) { return juce::String (v, s.decimals); };
            static const juce::String arrow (juce::CharPointer_UTF8 (" \xe2\x86\x92 "));

            value.setText ((valued ? text (lo) + arrow + text (hi) : text (hi)) + s.suffix,
                           juce::dontSendNotification);
        }

        FootView& owner;
        int index;
        int param = -1;
        bool valued;
        bool reversed = false;
        float lo = 0.0f, hi = 1.0f;
        juce::Label name, value;
        juce::ComboBox verbs;
        juce::Slider fader;
    };

    //==========================================================================
    class ControlCard : public juce::Component
    {
    public:
        ControlCard (FootView& o, const FootControl& control) : owner (o), controlId (control.id), kind (control.kind)
        {
            name.setFont (rigFont (12.0f, true));
            name.setInterceptsMouseClicks (false, true);   // clicks fall through; the editor still works
            name.setText (control.name.toUpperCase(), juce::dontSendNotification);
            name.onTextChange = [this]
            {
                const auto typed = name.getText().trim();

                if (typed.isNotEmpty())
                    owner.renameControl (controlId, typed);
                else
                    owner.rebuildSoon();   // an erased name comes back
            };
            addAndMakeVisible (name);

            wire.setFont (rigFont (11.0f));
            wire.setColour (juce::Label::textColourId, juce::Colour (0xff70737c));
            wire.setInterceptsMouseClicks (false, false);
            wire.setText (control.trigger.describe(), juce::dontSendNotification);
            addAndMakeVisible (wire);

            // Only a controller can be more than one kind of thing.
            if (control.trigger.type == FootTrigger::controller)
            {
                kindBox.addItem ("Latching", FootControl::latch + 1);
                kindBox.addItem ("Momentary", FootControl::momentary + 1);
                kindBox.addItem ("Expression", FootControl::sweep + 1);
                kindBox.setSelectedId (control.kind + 1, juce::dontSendNotification);
                kindBox.onChange = [this]
                {
                    owner.setKind (controlId, (FootControl::Kind) (kindBox.getSelectedId() - 1));
                };
                addAndMakeVisible (kindBox);
            }

            add.setButtonText ("+");
            add.setWantsKeyboardFocus (false);
            add.onClick = [this] { owner.showFunctionMenu (controlId, add); };
            addAndMakeVisible (add);
        }

        void addRow (JobRow* row)
        {
            addAndMakeVisible (rows.add (row));
        }

        int preferredHeight() const
        {
            return 50 + rows.size() * rowHeight + (rows.isEmpty() ? 0 : 5);
        }

        void flash()   { flashTicks = 6; }

        void setLearning (bool shouldShow)
        {
            if (learning != shouldShow)
            {
                learning = shouldShow;
                repaint();
            }
        }

        // The lamp is the pedal's own light where it has one: lit while a
        // latching switch is lit or a momentary one is down, as bright as
        // an expression pedal stands, and a blink for a press.
        void tick (float value)
        {
            float level = 0.0f;

            if (kind == FootControl::sweep)
                level = juce::jmax (0.0f, value);
            else if (kind != FootControl::preset)
                level = value >= 0.5f ? 1.0f : 0.0f;

            if (flashTicks > 0)
            {
                --flashTicks;

                if (kind == FootControl::preset)
                    level = 1.0f;
            }

            if (std::abs (level - lamp) > 0.01f)
            {
                lamp = level;
                repaint();
            }
        }

        void mouseDoubleClick (const juce::MouseEvent&) override
        {
            name.showEditor();
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu())
                owner.showControlMenu (controlId, name.getText());
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (juce::Colour (0xff1c1e24));
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
            g.setColour (juce::Colour (learning ? 0xffd87b3a : 0xff41444e));
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (learning ? 0.75f : 0.5f),
                                    4.0f, learning ? 1.5f : 1.0f);

            const juce::Rectangle<float> dot (12.0f, 13.0f, 9.0f, 9.0f);
            g.setColour (juce::Colour (0xff32343c).interpolatedWith (juce::Colour (0xff5fbf6a), lamp));
            g.fillEllipse (dot);
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced (10, 6);
            auto head = b.removeFromTop (24);
            head.removeFromLeft (18);   // the lamp
            add.setBounds (head.removeFromRight (30));

            if (kindBox.isVisible())
            {
                head.removeFromRight (6);
                kindBox.setBounds (head.removeFromRight (112));
            }

            head.removeFromRight (6);
            name.setBounds (head);

            auto sub = b.removeFromTop (16);
            sub.removeFromLeft (18);
            wire.setBounds (sub);

            b.removeFromTop (3);

            for (auto* row : rows)
                row->setBounds (b.removeFromTop (rowHeight).withTrimmedLeft (18));
        }

        FootView& owner;
        const juce::String controlId;

    private:
        FootControl::Kind kind;
        juce::Label name, wire;
        juce::ComboBox kindBox;
        juce::TextButton add;
        juce::OwnedArray<JobRow> rows;
        float lamp = 0.0f;
        int flashTicks = 0;
        bool learning = false;
    };

    //==========================================================================
    // Edits. Each one changes the working map and hands it to the engine;
    // anything that changes what the cards hold rebuilds them afterwards —
    // never from inside a callback of a control the rebuild would delete.
    void push (bool andKeep)
    {
        engine.setMap (map);

        if (andKeep)
            keep();
    }

    void keep()
    {
        if (onEdited != nullptr)
            onEdited();
    }

    void rebuildSoon()
    {
        juce::Component::SafePointer<FootView> view (this);
        juce::MessageManager::callAsync ([view] { if (view != nullptr) view->rebuildCards(); });
    }

    FootControl* findControl (const juce::String& id)
    {
        for (auto& control : map.controls)
            if (control.id == id)
                return &control;

        return nullptr;
    }

    FootJob* jobAt (int index)
    {
        auto* layout = map.activeLayout();

        return layout != nullptr && juce::isPositiveAndBelow (index, (int) layout->jobs.size())
                 ? &layout->jobs[(size_t) index] : nullptr;
    }

public:
    // The tab's own verbs — what its menus, boxes and faders do. Public so
    // the offscreen check can press them without a mouse.
    const FootMap& getMap() const noexcept { return map; }

    void setVerb (int index, FootJob::Verb verb)
    {
        if (auto* job = jobAt (index))
        {
            job->verb = verb;
            push (true);
        }
    }

    // Mid-drag: the engine hears every step, the file waits for the release.
    void setRange (int index, float lo, float hi)
    {
        if (auto* job = jobAt (index))
        {
            job->lo = lo;
            job->hi = hi;
            push (false);
        }
    }

    void swapEnds (int index)
    {
        if (auto* job = jobAt (index))
        {
            std::swap (job->lo, job->hi);
            push (true);
            rebuildCards();
        }
    }

    void removeJob (int index)
    {
        if (auto* layout = map.activeLayout())
        {
            if (juce::isPositiveAndBelow (index, (int) layout->jobs.size()))
            {
                layout->jobs.erase (layout->jobs.begin() + index);
                push (true);
                rebuildCards();
            }
        }
    }

    // A new job starts where it is most likely wanted: a switch follows a
    // control that has a position and turns on from one that only presses;
    // a knob sweeps its whole travel, or is sent to where it stands now.
    void addJob (const juce::String& controlId, const juce::String& action)
    {
        auto* layout = map.activeLayout();
        const auto* control = findControl (controlId);

        if (layout == nullptr || control == nullptr)
            return;

        FootJob job;
        job.control = controlId;
        job.action = action;
        job.verb = control->hasValue() ? FootJob::follow : FootJob::on;

        FootParamTarget target;

        if (parseFootParamAction (action, target) && ! target.isSwitch)
        {
            const auto& s = spec (target.param);
            job.lo = control->hasValue() ? s.minValue : params.get (target.slot, target.param);
            job.hi = control->hasValue() ? s.maxValue : job.lo;
        }

        layout->jobs.push_back (job);
        push (true);
        rebuildCards();
    }

    void renameControl (const juce::String& id, const juce::String& newName)
    {
        if (auto* control = findControl (id))
        {
            control->name = newName;
            push (true);
            rebuildSoon();
        }
    }

    void setKind (const juce::String& id, FootControl::Kind kind)
    {
        if (auto* control = findControl (id))
        {
            control->kind = kind;
            push (true);
            rebuildSoon();   // the rows' controls depend on the kind
        }
    }

    void removeControl (const juce::String& id)
    {
        for (auto& layout : map.layouts)
            std::erase_if (layout.jobs, [&id] (const FootJob& job) { return job.control == id; });

        std::erase_if (map.controls, [&id] (const FootControl& control) { return control.id == id; });
        push (true);
        rebuildCards();
    }

private:
    void showFunctionMenu (const juce::String& controlId, juce::Component& target)
    {
        if (buildFunctionMenu == nullptr)
            return;

        juce::Component::SafePointer<FootView> view (this);
        juce::PopupMenu m;
        buildFunctionMenu (m, [view, controlId] (const juce::String& action)
        {
            if (view != nullptr)
                view->addJob (controlId, action);
        });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target));
    }

    void showControlMenu (const juce::String& controlId, const juce::String& shownName)
    {
        juce::Component::SafePointer<FootView> view (this);
        juce::PopupMenu m;

        m.addItem ("Learn again", [view, controlId] { if (view != nullptr) view->learnTapped (controlId); });
        m.addItem ("Remove " + shownName + "...", [view, controlId, shownName]
        {
            if (view == nullptr)
                return;

            juce::AlertWindow::showOkCancelBox (
                juce::MessageBoxIconType::QuestionIcon,
                "Remove " + shownName + "?",
                "The control leaves the foot tab, along with what it does in every layout.",
                "Remove", "Keep", view.getComponent(),
                juce::ModalCallbackFunction::create ([view, controlId] (int choice)
                {
                    if (choice == 1 && view != nullptr)
                        view->removeControl (controlId);
                }));
        });

        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
    }

    //==========================================================================
    // Learning. An empty target means a new control; a control's id means
    // that control is being pointed at a new wire, jobs untouched.
public:
    void learnTapped (const juce::String& target)
    {
        if (engine.isLearning())
        {
            engine.cancelLearn();
            return;
        }

        learnTarget = target;
        engine.beginLearn();
    }

private:
    void learned (const FootEngine::Learned& got)
    {
        const auto target = learnTarget;
        learnTarget.clear();

        if (auto* control = findControl (target))
        {
            control->trigger = got.trigger;
            control->kind = got.kind;
            push (true);
            rebuildCards();
            return;
        }

        // The same wire twice would be two cards answering one press.
        for (const auto& control : map.controls)
        {
            if (control.trigger.sameWire (got.trigger))
            {
                for (auto* card : cards)
                    if (card->controlId == control.id)
                    {
                        card->flash();
                        viewport.setViewPosition (0, card->getY());
                    }

                return;
            }
        }

        FootControl control;
        control.id = map.freshControlId();
        control.trigger = got.trigger;
        control.kind = got.kind;

        switch (got.trigger.type)
        {
            case FootTrigger::program:     control.name = "Program " + juce::String (got.trigger.number); break;
            case FootTrigger::controller:  control.name = (got.kind == FootControl::sweep ? "Pedal " : "Switch ")
                                                            + juce::String (got.trigger.number); break;
            case FootTrigger::start:       control.name = "Start"; break;
            case FootTrigger::stop:        control.name = "Stop"; break;
            case FootTrigger::resume:      control.name = "Continue"; break;
        }

        map.controls.push_back (control);
        push (true);
        rebuildCards();
        viewport.setViewPosition (0, juce::jmax (0, holder.getHeight() - viewport.getHeight()));
    }

    void refreshAddButton()
    {
        const bool adding = showingLearn && learnTarget.isEmpty();
        addButton.setToggleState (adding, juce::dontSendNotification);
        addButton.setButtonText (showingLearn ? "STEP ON IT...  (TAP TO CANCEL)" : "ADD A FOOT CONTROL...");
    }

    //==========================================================================
    void layoutBoxChanged()
    {
        const auto typed = layoutBox.getText().trim();

        if (typed.isEmpty() || typed == map.active)
        {
            refreshLayoutBox();
            return;
        }

        for (const auto& layout : map.layouts)
        {
            if (layout.name == typed)
            {
                map.active = typed;
                push (true);
                refreshLayoutBox();
                rebuildCards();
                return;
            }
        }

        // A name the list does not hold renames the layout in force, and
        // every switch-to-this-layout job follows it.
        if (auto* layout = map.activeLayout())
        {
            const auto oldAction = "do/layout:" + layout->name;

            for (auto& other : map.layouts)
                for (auto& job : other.jobs)
                    if (job.action == oldAction)
                        job.action = "do/layout:" + typed;

            layout->name = typed;
            map.active = typed;
            push (true);
            refreshLayoutBox();
            rebuildCards();
        }
    }

public:
    void copyLayout()
    {
        const auto* current = map.activeLayout();

        if (current == nullptr)
            return;

        FootLayout copy = *current;
        const auto names = getLayoutNames();

        for (int n = 2;; ++n)
        {
            copy.name = current->name + " " + juce::String (n);

            if (! names.contains (copy.name))
                break;
        }

        map.layouts.push_back (copy);
        map.active = copy.name;
        push (true);
        refreshLayoutBox();
        rebuildCards();
    }

private:
    void confirmDeleteLayout()
    {
        if (map.layouts.size() < 2)
            return;

        juce::Component::SafePointer<FootView> view (this);
        const auto name = map.active;

        juce::AlertWindow::showOkCancelBox (
            juce::MessageBoxIconType::QuestionIcon,
            "Delete " + name + "?",
            "The layout goes, with everything the controls do in it. The controls themselves stay.",
            "Delete", "Keep", this,
            juce::ModalCallbackFunction::create ([view, name] (int choice)
            {
                if (choice == 1 && view != nullptr)
                    view->deleteLayout (name);
            }));
    }

public:
    void deleteLayout (const juce::String& name)
    {
        if (map.layouts.size() < 2)
            return;

        std::erase_if (map.layouts, [&name] (const FootLayout& layout) { return layout.name == name; });

        // A job that switched to it has nowhere to go.
        for (auto& layout : map.layouts)
            std::erase_if (layout.jobs, [&name] (const FootJob& job) { return job.action == "do/layout:" + name; });

        map.active = map.layouts.front().name;
        push (true);
        refreshLayoutBox();
        rebuildCards();
    }

private:
    void refreshLayoutBox()
    {
        layoutBox.clear (juce::dontSendNotification);

        for (int i = 0; i < (int) map.layouts.size(); ++i)
        {
            layoutBox.addItem (map.layouts[(size_t) i].name, i + 1);

            if (map.layouts[(size_t) i].name == map.active)
                layoutBox.setSelectedId (i + 1, juce::dontSendNotification);
        }

        // One layout cannot be deleted, so the button is absent, not dead.
        const bool canDelete = map.layouts.size() > 1;

        if (deleteButton.isVisible() != canDelete)
        {
            deleteButton.setVisible (canDelete);
            resized();
        }
    }

    // Any controller, then every MIDI input the Mac can see — and the chosen
    // one even while it is unplugged, because that is still the truth.
    void refreshDeviceBox()
    {
        juce::StringArray names;

        for (const auto& device : juce::MidiInput::getAvailableDevices())
            if (! hiddenDevices.contains (device.name))
                names.addIfNotAlreadyThere (device.name);

        if (map.device.isNotEmpty())
            names.addIfNotAlreadyThere (map.device);

        if (names == deviceNames && deviceBox.getNumItems() > 0)
            return;

        deviceNames = names;
        deviceBox.clear (juce::dontSendNotification);
        deviceBox.addItem ("Any controller", 1);

        for (int i = 0; i < names.size(); ++i)
            deviceBox.addItem (names[i], i + 2);

        deviceBox.setSelectedId (map.device.isEmpty() ? 1 : names.indexOf (map.device) + 2,
                                 juce::dontSendNotification);
    }

    void deviceBoxChanged()
    {
        const int id = deviceBox.getSelectedId();
        map.device = id >= 2 ? deviceNames[id - 2] : juce::String();
        push (true);
    }

    //==========================================================================
    void rebuildCards()
    {
        const int scroll = viewport.getViewPositionY();
        cards.clear();

        const auto* layout = map.activeLayout();

        for (const auto& control : map.controls)
        {
            auto* card = cards.add (new ControlCard (*this, control));

            if (layout != nullptr)
                for (int i = 0; i < (int) layout->jobs.size(); ++i)
                    if (layout->jobs[(size_t) i].control == control.id)
                        card->addRow (new JobRow (*this, i, layout->jobs[(size_t) i], control.hasValue()));

            card->setLearning (showingLearn && control.id == learnTarget);
            holder.addAndMakeVisible (card);
        }

        refreshAddButton();
        layoutCards();
        viewport.setViewPosition (0, scroll);
    }

    void layoutCards()
    {
        const int width = juce::jmax (120, viewport.getWidth() - 12);
        int y = 0;

        for (auto* card : cards)
        {
            card->setBounds (0, y, width, card->preferredHeight());
            y += card->getHeight() + cardGap;
        }

        addButton.setBounds (0, y, width, 34);
        holder.setSize (width, y + 34);
    }

    ParamStore& params;
    FootEngine& engine;
    FootMap map;

    juce::ComboBox layoutBox, deviceBox;
    juce::TextButton copyButton, deleteButton, addButton;
    juce::Label deviceLabel, heardLabel, heard;
    juce::Viewport viewport;
    juce::Component holder;
    juce::OwnedArray<ControlCard> cards;
    juce::StringArray deviceNames;

    juce::String learnTarget;
    bool showingLearn = false;
    juce::uint32 heardSerial = 0;
    int heardGlow = -1;
    int deviceRefresh = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FootView)
};

} // namespace rig
