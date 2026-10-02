#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../FootControl.h"
#include "../Looper.h"
#include "../Parameters.h"
#include "Type.h"
#include "Widgets.h"

namespace rig
{

// The loop pane: what the looper is doing, and which switch under the foot
// does what — read from across a stage, so the switch numbers and the
// tempo are as big as the pane allows. The numbers come from the foot map:
// whichever control the layout in force gives a looper job is the number
// shown beside it, and a function no switch has yet shows a dash.
//
// A row can be clicked to do what its switch would, for trying things with
// no controller plugged in. The window carries the presses out, because it
// knows which channel is selected — that is the channel Record takes.
class LoopView : public juce::Component
{
public:
    LoopView (SlotParams& p, Looper& l, FootEngine& f) : looper (l), foot (f), level (p, pLoopLevel)
    {
        addAndMakeVisible (level);
        refreshSwitches();
    }

    // The looper's functions, in the order a player meets them. The action
    // is the foot map's name for the job.
    static constexpr int numRows = 6;

    static const char* actionFor (int row) noexcept
    {
        static const char* const actions[numRows] = { "do/loopTempo", "do/loopBars", "do/loopRecord",
                                                      "do/loopStop", "do/loopUndo", "do/loopClear" };
        return actions[row];
    }

    // A row was clicked: the window does what the switch would.
    std::function<void (const juce::String& action)> onPress;

    // The channel Record will take, in the player's words.
    void setRecordsName (const juce::String& name)
    {
        if (recordsName != name)
        {
            recordsName = name;
            repaint();
        }
    }

    // The fader, after a foot or a settings load moved it.
    void refresh()   { level.refresh(); }

    // From the window's timer: the looper's state, and now and then the
    // foot map, which a press in the foot tab may have changed.
    void tick()
    {
        Shown now;
        now.state = looper.getState();
        now.bpm = (int) std::lround (looper.getBpm());
        now.bars = looper.getBars();
        now.beat = (int) std::floor (looper.getBeatPosition());
        now.undone = looper.isLayerUndone();

        if (++switchRefresh >= 8)
        {
            switchRefresh = 0;
            refreshSwitches();
        }

        if (now != shown)
        {
            shown = now;
            repaint();
        }
    }

    // Which control does each job, by the name on its card. Called on the
    // timer; public so the offscreen check can ask for it at once.
    void refreshSwitches()
    {
        const auto map = foot.getMap();
        juce::StringArray names;

        for (int row = 0; row < numRows; ++row)
        {
            juce::String name;

            if (const auto* layout = map.activeLayout())
            {
                for (const auto& job : layout->jobs)
                {
                    if (job.action == actionFor (row))
                    {
                        if (const auto* control = map.findControl (job.control))
                            name = control->name;

                        break;
                    }
                }
            }

            names.add (name);
        }

        if (names != switchNames)
        {
            switchNames = names;
            repaint();
        }
    }

    // What a row shows as its switch: the number its name ends in — "Button
    // 3" is 3 — or the whole name when it ends in none.
    juce::String switchTextFor (int row) const
    {
        const auto name = switchNames[row].trim();
        const auto digits = name.getLastCharacters (name.length()
                                - name.lastIndexOfAnyOf (" \t") - 1);

        return digits.isNotEmpty() && digits.containsOnly ("0123456789") ? digits : name;
    }

    juce::String labelFor (int row) const
    {
        using S = Looper::State;
        const bool taking = shown.state == S::countIn || shown.state == S::recording;
        const bool running = shown.state == S::playing || shown.state == S::overdubbing;

        switch (row)
        {
            case 0:  return "TAP TEMPO";
            case 1:  return "BARS";
            case 2:  return shown.state == S::empty || taking ? "RECORD" : "OVERDUB";
            case 3:  return taking ? "CANCEL" : running ? "STOP" : shown.state == S::stopped ? "START" : "STOP / START";
            case 4:  return shown.undone ? "REDO" : "UNDO";
            default: return "CLEAR";
        }
    }

    juce::String stateText() const
    {
        switch (shown.state)
        {
            case Looper::State::empty:        return "READY";
            case Looper::State::countIn:      return "COUNT IN";
            case Looper::State::recording:    return "RECORDING";
            case Looper::State::playing:      return "PLAYING";
            case Looper::State::overdubbing:  return "OVERDUB";
            case Looper::State::stopped:      return "STOPPED";
        }

        return {};
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (12, 10);

        level.setBounds (b.removeFromBottom (ParamRow::preferredHeight));
        b.removeFromBottom (6);
        recordsArea = b.removeFromBottom (30);
        b.removeFromBottom (8);

        stateArea = b.removeFromTop (88);
        b.removeFromTop (10);

        const int gap = 6;
        const int rowHeight = juce::jlimit (40, 84, (b.getHeight() - gap * (numRows - 1)) / numRows);

        for (auto& row : rowAreas)
        {
            row = b.removeFromTop (rowHeight);
            b.removeFromTop (gap);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || onPress == nullptr)
            return;

        for (int row = 0; row < numRows; ++row)
            if (rowAreas[row].contains (e.getPosition()) && rowAreas[row].contains (e.getMouseDownPosition()))
                onPress (actionFor (row));
    }

    void paint (juce::Graphics& g) override
    {
        using S = Looper::State;

        g.setColour (juce::Colour (0xff17181c));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
        g.setColour (juce::Colour (0xff26272d));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);

        const bool taking = shown.state == S::countIn || shown.state == S::recording;
        const bool writing = shown.state == S::recording || shown.state == S::overdubbing;

        const juce::Colour red (0xffe4483c), green (0xff5fbf6a), orange (0xffd87b3a),
                           quiet (0xff9a9da6), text (0xffc8c9ce), dim (0xff565963);

        const auto stateColour = writing ? red
                               : shown.state == S::countIn ? orange
                               : shown.state == S::playing ? green : quiet;

        // --- what the looper is doing ---
        {
            auto area = stateArea;

            g.setColour (stateColour);
            g.setFont (rigFont (30.0f, true));
            g.drawText (stateText(), area.removeFromTop (38), juce::Justification::centredLeft);

            // Where it is: the count, or the bar of the loop.
            const int beat = juce::jmax (0, shown.beat);
            juce::String where;

            if (shown.state == S::countIn)
                where = juce::String (juce::jmin (Looper::countInBeats, beat + 1));
            else if (shown.state != S::empty)
                where = "BAR " + juce::String (juce::jmin (shown.bars, beat / Looper::beatsPerBar + 1))
                          + " OF " + juce::String (shown.bars);

            g.setColour (text);
            g.setFont (rigFont (15.0f, true));
            g.drawText (where, area.removeFromTop (22), juce::Justification::centredLeft);

            // The beat of the bar, one block a beat.
            area.removeFromTop (8);
            auto blocks = area.removeFromTop (12).toFloat();
            const float blockGap = 6.0f;
            const float blockWidth = (blocks.getWidth() - blockGap * (Looper::beatsPerBar - 1)) / Looper::beatsPerBar;
            const bool moving = shown.state != S::empty && shown.state != S::stopped;

            for (int i = 0; i < Looper::beatsPerBar; ++i)
            {
                g.setColour (moving && beat % Looper::beatsPerBar == i ? stateColour : juce::Colour (0xff2a2c33));
                g.fillRoundedRectangle (blocks.removeFromLeft (blockWidth), 3.0f);
                blocks.removeFromLeft (blockGap);
            }
        }

        // --- which switch does what ---
        for (int row = 0; row < numRows; ++row)
        {
            const auto area = rowAreas[row];
            const bool lit = row == 2 && (taking || shown.state == S::overdubbing);

            g.setColour (lit ? red.withAlpha (0.22f) : juce::Colour (0xff1c1e24));
            g.fillRoundedRectangle (area.toFloat(), 4.0f);
            g.setColour (lit ? red : juce::Colour (0xff41444e));
            g.drawRoundedRectangle (area.toFloat().reduced (0.5f), 4.0f, 1.0f);

            auto inner = area.reduced (10, 4);
            const float rowScale = (float) area.getHeight() / 72.0f;

            // The switch: its number as big as the row, or its name, or a
            // dash while no switch has the job.
            auto switchArea = inner.removeFromLeft (58);
            const auto switchText = switchTextFor (row);
            const bool isNumber = switchText.isNotEmpty() && switchText.containsOnly ("0123456789");

            g.setColour (switchText.isEmpty() ? dim : juce::Colour (0xffe8e8ea));
            g.setFont (rigFont (isNumber ? juce::jmin (44.0f, 40.0f * rowScale) : 12.0f, true));

            if (isNumber || switchText.isEmpty())
                g.drawText (switchText.isEmpty() ? juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93")) : switchText,
                            switchArea, juce::Justification::centred);
            else
                g.drawFittedText (switchText.toUpperCase(), switchArea, juce::Justification::centred, 3, 0.8f);

            inner.removeFromLeft (8);

            // The settings the two taps make ride their rows.
            const auto value = row == 0 ? juce::String (shown.bpm)
                             : row == 1 ? juce::String (shown.bars) : juce::String();

            if (value.isNotEmpty())
            {
                g.setColour (orange);
                g.setFont (rigFont (juce::jmin (32.0f, 30.0f * rowScale), true));
                g.drawText (value, inner.removeFromRight (68), juce::Justification::centredRight);
            }

            g.setColour (lit ? juce::Colour (0xffe8e8ea) : text);
            g.setFont (rigFont (juce::jmin (21.0f, 20.0f * juce::jmax (0.8f, rowScale)), true));
            g.drawText (labelFor (row), inner, juce::Justification::centredLeft);
        }

        // --- which channel Record takes ---
        {
            auto area = recordsArea;

            g.setColour (juce::Colour (0xff70737c));
            g.setFont (rigFont (11.0f, true));
            g.drawText ("RECORDS", area.removeFromLeft (70), juce::Justification::centredLeft);

            g.setColour (text);
            g.setFont (rigFont (17.0f, true));
            g.drawText (recordsName.toUpperCase(), area, juce::Justification::centredLeft);
        }
    }

private:
    // Everything the pane draws from; a repaint only when one of them moved.
    struct Shown
    {
        Looper::State state = Looper::State::empty;
        int bpm = 120, bars = 4, beat = 0;
        bool undone = false;

        bool operator!= (const Shown& other) const noexcept
        {
            return state != other.state || bpm != other.bpm || bars != other.bars
                     || beat != other.beat || undone != other.undone;
        }
    };

    Looper& looper;
    FootEngine& foot;
    ParamRow level;

    Shown shown;
    juce::StringArray switchNames;
    juce::String recordsName;
    int switchRefresh = 0;

    juce::Rectangle<int> stateArea, recordsArea, rowAreas[numRows];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoopView)
};

} // namespace rig
