#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Parameters.h"
#include "../PresetStore.h"
#include "Type.h"

namespace rig
{

class RigLookAndFeel : public juce::LookAndFeel_V4
{
public:
    RigLookAndFeel()
    {
        auto scheme = juce::LookAndFeel_V4::getDarkColourScheme();
        scheme.setUIColour (ColourScheme::widgetBackground, juce::Colour (0xff17181c));
        scheme.setUIColour (ColourScheme::windowBackground, juce::Colour (0xff101114));
        scheme.setUIColour (ColourScheme::defaultFill,      juce::Colour (0xffd87b3a));
        scheme.setUIColour (ColourScheme::highlightedFill,  juce::Colour (0xfff0954a));
        setColourScheme (scheme);

        setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour (0xffd87b3a));
        setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff32343c));
        setColour (juce::Slider::thumbColourId,               juce::Colour (0xffe8e8ea));
        setColour (juce::Slider::textBoxOutlineColourId,      juce::Colours::transparentBlack);
        setColour (juce::Label::textColourId,                 juce::Colour (0xffc8c9ce));
    }

    // Buttons, combo boxes and slider readouts pick their faces up from the
    // look-and-feel rather than explicit setFont calls — scale them here so
    // kTypeScale really does cover everything.
    juce::Font getTextButtonFont (juce::TextButton& button, int buttonHeight) override
    {
        const auto base = LookAndFeel_V4::getTextButtonFont (button, buttonHeight);
        return base.withHeight (base.getHeight() * kTypeScale);
    }

    juce::Font getComboBoxFont (juce::ComboBox& box) override
    {
        const auto base = LookAndFeel_V4::getComboBoxFont (box);
        return base.withHeight (base.getHeight() * kTypeScale);
    }

    juce::Font getPopupMenuFont() override
    {
        const auto base = LookAndFeel_V4::getPopupMenuFont();
        return base.withHeight (base.getHeight() * kTypeScale);
    }

    juce::Label* createSliderTextBox (juce::Slider& slider) override
    {
        auto* label = LookAndFeel_V4::createSliderTextBox (slider);
        label->setFont (label->getFont().withHeight (label->getFont().getHeight() * kTypeScale));
        return label;
    }

    // A mixer fader rather than the default slab of colour.
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        if (style == juce::Slider::LinearHorizontal)
        {
            const float centreY = (float) y + (float) height * 0.5f;
            const float trackHeight = 6.0f;

            const juce::Rectangle<float> track ((float) x, centreY - trackHeight * 0.5f,
                                                (float) width, trackHeight);
            g.setColour (juce::Colour (0xff2a2c33));
            g.fillRoundedRectangle (track, trackHeight * 0.5f);

            if (sliderPos > (float) x)
            {
                g.setColour (slider.isEnabled() ? juce::Colour (0xffd87b3a)
                                                : juce::Colour (0xff43454e));
                g.fillRoundedRectangle (track.withRight (sliderPos), trackHeight * 0.5f);
            }

            const float thumbWidth = 15.0f;
            const float thumbHeight = juce::jmin (24.0f, (float) height - 2.0f);
            const juce::Rectangle<float> thumb (sliderPos - thumbWidth * 0.5f,
                                                centreY - thumbHeight * 0.5f,
                                                thumbWidth, thumbHeight);

            g.setColour (slider.isEnabled() ? juce::Colour (0xffe8e8ea) : juce::Colour (0xff6a6c75));
            g.fillRoundedRectangle (thumb, 3.0f);
            g.setColour (juce::Colour (0xff101114));
            g.drawLine (sliderPos, thumb.getY() + 5.0f, sliderPos, thumb.getBottom() - 5.0f, 1.0f);
            return;
        }

        if (style != juce::Slider::LinearVertical)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos,
                                              minSliderPos, maxSliderPos, style, slider);
            return;
        }

        const float centreX = (float) x + (float) width * 0.5f;
        const float trackWidth = 5.0f;
        const float bottom = (float) (y + height);

        const juce::Rectangle<float> track (centreX - trackWidth * 0.5f, (float) y,
                                            trackWidth, (float) height);
        g.setColour (juce::Colour (0xff2a2c33));
        g.fillRoundedRectangle (track, trackWidth * 0.5f);

        if (bottom > sliderPos)
        {
            g.setColour (slider.isEnabled() ? juce::Colour (0xffd87b3a)
                                            : juce::Colour (0xff43454e));
            g.fillRoundedRectangle (track.withTop (sliderPos), trackWidth * 0.5f);
        }

        const float thumbWidth = juce::jmin (28.0f, (float) width - 6.0f);
        const float thumbHeight = 13.0f;
        const juce::Rectangle<float> thumb (centreX - thumbWidth * 0.5f,
                                            sliderPos - thumbHeight * 0.5f,
                                            thumbWidth, thumbHeight);

        g.setColour (slider.isEnabled() ? juce::Colour (0xffe8e8ea) : juce::Colour (0xff6a6c75));
        g.fillRoundedRectangle (thumb, 3.0f);
        g.setColour (juce::Colour (0xff101114));
        g.drawLine (thumb.getX() + 4.0f, sliderPos, thumb.getRight() - 4.0f, sliderPos, 1.0f);
    }
};

//==============================================================================
// One parameter as a labelled control, and the control matches the shape of
// the choice: a continuum gets a wide fader you can shove with one hand
// mid-song, two options get a switch, and a handful of named options gets a
// menu. The spec decides — no module can opt a binary into a fader.
class ParamRow : public juce::Component
{
public:
    static constexpr int preferredHeight = 30;

    ParamRow (ParamStore& p, int paramId) : params (p), id (paramId)
    {
        const auto& s = spec (id);

        const int steps = s.step > 0.0f
                            ? (int) std::lround ((s.maxValue - s.minValue) / s.step) + 1
                            : 0;

        mode = steps == 2 ? Mode::toggle
                          : (steps >= 3 && steps <= 5 ? Mode::menu : Mode::fader);

        name.setText (s.label, juce::dontSendNotification);
        name.setFont (rigFont (12.0f));
        name.setColour (juce::Label::textColourId, juce::Colour (0xff9a9da6));
        addAndMakeVisible (name);

        value.setFont (rigFont (12.0f));
        value.setJustificationType (juce::Justification::centredRight);

        if (mode == Mode::menu)
        {
            for (int i = 0; i < steps; ++i)
                menu.addItem (itemName (s, i), i + 1);

            menu.setSelectedId (stepIndex() + 1, juce::dontSendNotification);
            menu.onChange = [this]
            {
                const auto& sp = spec (id);
                params.set (id, sp.minValue + (float) (menu.getSelectedId() - 1) * sp.step);
            };
            addAndMakeVisible (menu);
            return;
        }

        addAndMakeVisible (value);

        if (mode == Mode::toggle)
        {
            toggle.onClick = [this]
            {
                const auto& sp = spec (id);
                params.set (id, params.getBool (id) ? sp.minValue : sp.maxValue);
                toggle.setOn (params.getBool (id));
                updateValueText();
            };
            toggle.setOn (params.getBool (id));
            addAndMakeVisible (toggle);
            updateValueText();
            return;
        }

        juce::NormalisableRange<double> range ((double) s.minValue, (double) s.maxValue, (double) s.step);

        if (s.skewCentre > 0.0f)
            range.setSkewForCentre ((double) s.skewCentre);

        slider.setNormalisableRange (range);
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setDoubleClickReturnValue (true, (double) s.defaultValue);
        slider.setSliderSnapsToMousePosition (true);   // tap anywhere to jump there
        slider.setValue ((double) params.get (id), juce::dontSendNotification);
        slider.onValueChange = [this]
        {
            params.set (id, (float) slider.getValue());
            updateValueText();
        };
        addAndMakeVisible (slider);

        updateValueText();
    }

    void refresh()
    {
        if (mode == Mode::menu)
        {
            menu.setSelectedId (stepIndex() + 1, juce::dontSendNotification);
            return;
        }

        if (mode == Mode::toggle)
        {
            toggle.setOn (params.getBool (id));
            updateValueText();
            return;
        }

        slider.setValue ((double) params.get (id), juce::dontSendNotification);
        updateValueText();
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (0, 2);
        name.setBounds (b.removeFromLeft (82));

        if (mode == Mode::menu)
        {
            menu.setBounds (b);
            return;
        }

        value.setBounds (b.removeFromRight (78));
        b.removeFromRight (6);

        if (mode == Mode::toggle)
            toggle.setBounds (b.removeFromLeft (40).reduced (0, 4));
        else
            slider.setBounds (b);
    }

private:
    enum class Mode { fader, toggle, menu };

    // The two-state control: a pill that reads as a switch, not a checkbox.
    struct Switch : juce::Button
    {
        Switch() : juce::Button ({}) { setWantsKeyboardFocus (false); }

        void setOn (bool shouldBeOn)
        {
            if (on != shouldBeOn)
            {
                on = shouldBeOn;
                repaint();
            }
        }

        void paintButton (juce::Graphics& g, bool, bool) override
        {
            auto b = getLocalBounds().toFloat();
            const float h = juce::jmin (18.0f, b.getHeight());
            const juce::Rectangle<float> pill (b.getX(), b.getCentreY() - h * 0.5f,
                                               juce::jmin (36.0f, b.getWidth()), h);

            g.setColour (juce::Colour (on ? 0xff3f9d4a : 0xff32343c));
            g.fillRoundedRectangle (pill, h * 0.5f);

            const float knob = h - 4.0f;
            g.setColour (juce::Colour (0xffe8e8ea));
            g.fillEllipse (on ? pill.getRight() - knob - 2.0f : pill.getX() + 2.0f,
                           pill.getY() + 2.0f, knob, knob);
        }

        bool on = false;
    };

    int stepIndex() const noexcept
    {
        const auto& s = spec (id);
        return juce::jlimit (0, menu.getNumItems() - 1,
                             (int) std::lround ((params.get (id) - s.minValue) / s.step));
    }

    static juce::String itemName (const ParamSpec& s, int index)
    {
        if (s.choices != nullptr)
            return s.choices[index];

        return juce::String (s.minValue + (float) index * s.step, s.decimals) + s.suffix;
    }

    void updateValueText()
    {
        const auto& s = spec (id);
        const float current = mode == Mode::fader ? (float) slider.getValue() : params.get (id);

        // A stepped parameter with names shows the name. "Green" beats "1".
        if (s.choices != nullptr)
        {
            const int index = juce::jlimit (0, (int) (s.maxValue - s.minValue),
                                            (int) std::lround (current - s.minValue));
            value.setText (s.choices[index], juce::dontSendNotification);
            return;
        }

        value.setText (juce::String (current, s.decimals) + s.suffix,
                       juce::dontSendNotification);
    }

    ParamStore& params;
    int id;
    Mode mode = Mode::fader;
    juce::Label name, value;
    juce::Slider slider;
    juce::ComboBox menu;
    Switch toggle;
};

//==============================================================================
class LevelMeter : public juce::Component
{
public:
    explicit LevelMeter (int numBars = 1) : bars ((size_t) juce::jmax (1, numBars), 0.0f)
    {
        // A readout, not a control. The chain rows carry meters in their
        // headers, and a tap on the meter should select the row like a tap
        // anywhere else on it.
        setInterceptsMouseClicks (false, false);
    }

    void setLevel (int bar, float peak)
    {
        if (juce::isPositiveAndBelow (bar, (int) bars.size()) && std::abs (bars[(size_t) bar] - peak) > 0.001f)
        {
            bars[(size_t) bar] = peak;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff202128));
        g.fillRoundedRectangle (b, 2.0f);
        g.setColour (juce::Colour (0xff34363f));
        g.drawRoundedRectangle (b.reduced (0.5f), 2.0f, 1.0f);

        const float gap = 2.0f;
        const float h = (b.getHeight() - gap * (float) (bars.size() - 1)) / (float) bars.size();

        for (size_t i = 0; i < bars.size(); ++i)
        {
            auto row = b.withHeight (h).withY (b.getY() + (float) i * (h + gap)).reduced (1.0f);
            const float db = juce::Decibels::gainToDecibels (bars[i], -60.0f);
            const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);

            if (norm > 0.001f)
            {
                g.setColour (bars[i] >= 0.99f ? juce::Colour (0xffe4483c)
                                              : juce::Colour (0xff5fbf6a).interpolatedWith (juce::Colour (0xffe4c23c),
                                                                                            juce::jlimit (0.0f, 1.0f, (norm - 0.72f) / 0.28f)));
                g.fillRoundedRectangle (row.withWidth (row.getWidth() * norm), 1.5f);
            }
        }
    }

private:
    std::vector<float> bars;
};

//==============================================================================
class StemStrip : public juce::Component
{
public:
    StemStrip (ParamStore& p, const juce::String& stemName, int gainParam, int muteParam)
        : params (p), gainId (gainParam), muteId (muteParam)
    {
        name.setText (stemName, juce::dontSendNotification);
        name.setFont (rigFont (11.0f, true));
        name.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (name);

        status.setJustificationType (juce::Justification::centred);
        status.setFont (rigFont (10.0f));
        status.setColour (juce::Label::textColourId, juce::Colour (0xff70737c));
        addAndMakeVisible (status);

        const auto& s = spec (gainId);
        gain.setRange ((double) s.minValue, (double) s.maxValue, 0.1);
        gain.setSliderStyle (juce::Slider::LinearVertical);
        gain.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 19);
        gain.setDoubleClickReturnValue (true, (double) s.defaultValue);
        gain.textFromValueFunction = [] (double v) { return juce::String (v, 1) + " dB"; };
        gain.setValue ((double) params.get (gainId), juce::dontSendNotification);
        gain.updateText();
        gain.onValueChange = [this] { params.set (gainId, (float) gain.getValue()); };
        addAndMakeVisible (gain);

        // A negative mute id means the strip has no mute — the TOTAL fader
        // sums the stems and only turns down.
        if (muteId >= 0)
        {
            mute.setButtonText ("MUTE");
            mute.setWantsKeyboardFocus (false);
            mute.setClickingTogglesState (true);
            mute.setToggleState (params.getBool (muteId), juce::dontSendNotification);
            mute.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe4483c));
            mute.onClick = [this] { params.set (muteId, mute.getToggleState() ? 1.0f : 0.0f); };
            addAndMakeVisible (mute);
        }
    }

    void setPresent (bool isPresent)
    {
        status.setText (isPresent ? "" : "missing", juce::dontSendNotification);
        gain.setEnabled (isPresent);
        setAlpha (isPresent ? 1.0f : 0.55f);
    }

    void refresh()
    {
        gain.setValue ((double) params.get (gainId), juce::dontSendNotification);
        gain.updateText();

        if (muteId >= 0)
            mute.setToggleState (params.getBool (muteId), juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff17181c));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (5, 8);

        name.setBounds (b.removeFromTop (18).expanded (4, 0));   // caps need the full strip width
        status.setBounds (b.removeFromTop (14));

        // The mute row is reserved even on a strip without the button, so
        // every fader in the row runs the same length.
        auto muteRow = b.removeFromBottom (24);
        if (muteId >= 0)
            mute.setBounds (muteRow);

        b.removeFromBottom (8);
        gain.setBounds (b.reduced (1, 4));
    }

private:
    ParamStore& params;
    int gainId, muteId;
    juce::Label name, status;
    juce::Slider gain;
    juce::TextButton mute;
};

//==============================================================================
// One effect in the chain list. The row is the mid-song control surface: tap
// it to bring its controls up in the editor beside the list, hit ON without
// looking — and drag it by anywhere on the row to move the stage in the
// signal chain. The gripper marks the rows that move; input, reverb and live
// out hold their places. The parameters themselves live in FxEditor.
class FxPanel : public juce::Component
{
public:
    static constexpr int headerHeight = 34;

    // muteStyle turns the corner button red and inverts its meaning: lit is
    // silenced, because muting a live input is a feature, not a bypass.
    FxPanel (ParamStore& p, const juce::String& titleText, int bypassParam,
             bool canDrag, juce::Component* extra = nullptr, int extraWidthPx = 90,
             bool muteStyle = false)
        : params (p), bypassId (bypassParam), draggable (canDrag),
          extraComponent (extra), extraWidth (extraWidthPx)
    {
        // Chain rows read as faceplate caps; the editor keeps mixed case.
        title.setText (titleText.toUpperCase(), juce::dontSendNotification);
        title.setFont (rigFont (12.0f, true));
        title.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (title);

        if (bypassId >= 0)
        {
            power.setButtonText (muteStyle ? "MUTE" : "ON");
            power.setWantsKeyboardFocus (false);
            power.setClickingTogglesState (true);
            power.setToggleState (params.getBool (bypassId), juce::dontSendNotification);
            power.setColour (juce::TextButton::buttonOnColourId,
                             juce::Colour (muteStyle ? 0xffe4483c : 0xff3f9d4a));
            power.onClick = [this] { params.set (bypassId, power.getToggleState() ? 1.0f : 0.0f); };
            addAndMakeVisible (power);
        }

        if (extraComponent != nullptr)
            addAndMakeVisible (extraComponent);

        if (draggable)
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    std::function<void()> onSelect;
    std::function<void()> onContextMenu;   // right-click: remove and friends
    std::function<void (const juce::MouseEvent&)> onDragMove;
    std::function<void()> onDragEnd;

    bool isSelected() const noexcept { return selected; }


    void setSelected (bool shouldBeSelected)
    {
        if (selected != shouldBeSelected)
        {
            selected = shouldBeSelected;
            repaint();
        }
    }

    void setTitle (const juce::String& titleText)
    {
        title.setText (titleText.toUpperCase(), juce::dontSendNotification);
    }

    void refresh()
    {
        if (bypassId >= 0)
            power.setToggleState (params.getBool (bypassId), juce::dontSendNotification);
    }

    // Selection happens on release, so starting a drag never also toggles the
    // editor underneath the move.
    void mouseDown (const juce::MouseEvent&) override   { dragging = false; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! draggable)
            return;

        if (! dragging && e.getDistanceFromDragStart() < 5)
            return;

        dragging = true;

        if (onDragMove != nullptr)
            onDragMove (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragging)
        {
            dragging = false;

            if (onDragEnd != nullptr)
                onDragEnd();
        }
        else if (e.mods.isPopupMenu())
        {
            if (onContextMenu != nullptr)
                onContextMenu();
        }
        else if (onSelect != nullptr)
        {
            onSelect();
        }
    }

    void paint (juce::Graphics& g) override
    {
        // Selection is a full border and a raised fill — one convention for
        // every module, no edge bars.
        g.setColour (juce::Colour (selected ? 0xff2a2d36 : 0xff1c1e24));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        g.setColour (juce::Colour (selected ? 0xffd87b3a : 0xff41444e));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (selected ? 0.75f : 0.5f),
                                4.0f, selected ? 1.5f : 1.0f);

        if (draggable)
        {
            // The gripper: a three-by-three dot grid — the sign this row can
            // be picked up and moved.
            const float cy = (float) getHeight() * 0.5f;
            g.setColour (juce::Colour (dragging ? 0xff9a9da6 : 0xff565963));

            for (int row = -1; row <= 1; ++row)
                for (int col = -1; col <= 1; ++col)
                    g.fillEllipse (17.5f + (float) col * 4.5f - 1.25f,
                                   cy + (float) row * 4.5f - 1.25f, 2.5f, 2.5f);
        }
    }

    void resized() override
    {
        auto header = getLocalBounds().reduced (12, 5);
        header.removeFromLeft (22);   // gripper zone; kept on every row so titles align

        if (bypassId >= 0)
            power.setBounds (header.removeFromRight (58));

        if (extraComponent != nullptr)
        {
            header.removeFromRight (8);
            extraComponent->setBounds (header.removeFromRight (extraWidth).reduced (0, 6));
        }

        title.setBounds (header);
    }

private:
    ParamStore& params;
    int bypassId;
    bool draggable = false;
    bool selected = false;
    bool dragging = false;
    juce::Label title;
    juce::TextButton power;
    juce::Component* extraComponent = nullptr;
    int extraWidth = 90;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxPanel)
};

//==============================================================================
// The transport's face: a drawn glyph instead of a word. A triangle offers
// play; while the song runs, a square offers the stop that rewinds.
class IconButton : public juce::Button
{
public:
    enum class Glyph { play, stop };

    IconButton() : juce::Button ({}) {}

    void setGlyph (Glyph g)
    {
        if (glyph != g)
        {
            glyph = g;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        getLookAndFeel().drawButtonBackground (g, *this,
            findColour (getToggleState() ? juce::TextButton::buttonOnColourId
                                         : juce::TextButton::buttonColourId),
            highlighted, down);

        g.setColour (findColour (getToggleState() ? juce::TextButton::textColourOnId
                                                  : juce::TextButton::textColourOffId));

        const auto centre = getLocalBounds().toFloat().getCentre();
        const float s = juce::jmin (getWidth(), getHeight()) * 0.42f;

        if (glyph == Glyph::play)
        {
            juce::Path p;
            p.addTriangle (centre.x - s * 0.5f, centre.y - s * 0.62f,
                           centre.x - s * 0.5f, centre.y + s * 0.62f,
                           centre.x + s * 0.72f, centre.y);
            g.fillPath (p);
        }
        else
        {
            g.fillRect (juce::Rectangle<float> (s, s).withCentre (centre));
        }
    }

private:
    Glyph glyph = Glyph::play;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconButton)
};

//==============================================================================
// One channel on the rail: a name, its meter, and selection. Tapping a strip
// scopes the chain list and the editor to that channel — the first visible
// step of the channel rig, over the lanes that exist today.
class ChannelStrip : public juce::Component
{
public:
    static constexpr int preferredHeight = 54;

    ChannelStrip (const juce::String& channelKey, const juce::String& nameText,
                  LevelMeter* meterToShow)
        : meter (meterToShow)
    {
        setComponentID (channelKey);

        name.setFont (rigFont (13.0f, true));
        name.setInterceptsMouseClicks (false, true);   // clicks fall through; the editor still works
        addAndMakeVisible (name);
        setChannelName (nameText);

        name.onTextChange = [this]
        {
            const auto typed = name.getText().trim();

            if (typed.isEmpty())
            {
                setChannelName (getName());   // an erased name comes back
                return;
            }

            setChannelName (typed);

            if (onRename != nullptr)
                onRename (typed);
        };

        detail.setFont (rigFont (11.0f, true));
        detail.setJustificationType (juce::Justification::centredRight);
        detail.setColour (juce::Label::textColourId, juce::Colour (0xffd87b3a));
        detail.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (detail);

        if (meter != nullptr)
            addAndMakeVisible (*meter);
    }

    std::function<void()> onSelect;

    // Right-click: the strip's own menu (remove, one day more). Only strips
    // given the callback offer one — the master sum stays.
    std::function<void()> onContextMenu;

    // Double-click retypes what the channel really is. Only strips given
    // onRename open the editor — the master sum keeps its name.
    std::function<void (const juce::String&)> onRename;

    void setChannelName (const juce::String& newName)
    {
        setName (newName);   // the display name; the id stays the key
        name.setText (newName.toUpperCase(), juce::dontSendNotification);
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (onRename != nullptr)
            name.showEditor();
    }

    // A live readout on the name row — the master strip wears the limiter's.
    // The name only cedes row space while there is a readout to show.
    void setDetail (const juce::String& text)
    {
        if (detail.getText() != text)
        {
            const bool had = detail.getText().isNotEmpty();
            detail.setText (text, juce::dontSendNotification);

            if (had != text.isNotEmpty())
                resized();
        }
    }

    void setSelected (bool shouldBeSelected)
    {
        if (selected != shouldBeSelected)
        {
            selected = shouldBeSelected;
            repaint();
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! getLocalBounds().contains (e.getPosition()))
            return;

        if (e.mods.isPopupMenu())
        {
            if (onContextMenu != nullptr)
                onContextMenu();

            return;
        }

        if (onSelect != nullptr)
            onSelect();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (selected ? 0xff2a2d36 : 0xff1c1e24));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        g.setColour (juce::Colour (selected ? 0xffd87b3a : 0xff41444e));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (selected ? 0.75f : 0.5f),
                                4.0f, selected ? 1.5f : 1.0f);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (10, 7);
        auto nameRow = b.removeFromTop (18);
        detail.setBounds (nameRow.removeFromRight (detail.getText().isNotEmpty() ? 76 : 0));
        name.setBounds (nameRow);
        b.removeFromTop (5);

        if (meter != nullptr)
            meter->setBounds (b.removeFromTop (7));
    }

private:
    LevelMeter* meter = nullptr;   // not owned
    juce::Label name, detail;
    bool selected = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};

//==============================================================================
// The parameter surface beside the effect list. Shows whichever effect is
// selected — its faders and its preset row — and just a name-plate when
// nothing is.
class FxEditor : public juce::Component
{
public:
    FxEditor (ParamStore& p, PresetStore& ps) : params (p), presets (ps)
    {
        placeholder.setText ("Effect parameters", juce::dontSendNotification);
        placeholder.setFont (rigFont (13.0f));
        placeholder.setJustificationType (juce::Justification::centred);
        placeholder.setColour (juce::Label::textColourId, juce::Colour (0xff70737c));
        addAndMakeVisible (placeholder);

        title.setFont (rigFont (13.0f, true));
        addChildComponent (title);

        presetBox.setEditableText (true);
        presetBox.setTextWhenNothingSelected ("Preset");
        presetBox.onChange = [this] { applyPreset(); };
        addChildComponent (presetBox);

        presetSave.setButtonText ("Save");
        presetSave.setWantsKeyboardFocus (false);
        presetSave.onClick = [this] { savePreset(); };
        addChildComponent (presetSave);

        presetDelete.setButtonText ("Del");
        presetDelete.setWantsKeyboardFocus (false);
        presetDelete.onClick = [this] { deletePreset(); };
        addChildComponent (presetDelete);
    }

    // Bring one effect's controls up. An effect can hang one extra component
    // above its faders — the Amp stage's capture loader at its default strip
    // height, or a full control surface like the EQ's curve, which names its
    // own height and stands in for the fader rows entirely.
    void show (const juce::String& effectKey, const juce::String& titleText,
               const std::vector<int>& paramIds, juce::Component* extra = nullptr,
               int extraHeightPx = 30, bool faderRows = true)
    {
        effect = effectKey;
        ids = paramIds;
        extraHeight = extraHeightPx;

        title.setText (titleText, juce::dontSendNotification);

        if (extraComponent != nullptr && extraComponent != extra)
            removeChildComponent (extraComponent);

        extraComponent = extra;

        if (extraComponent != nullptr)
            addAndMakeVisible (extraComponent);

        rows.clear();

        if (faderRows)
            for (int id : ids)
                addAndMakeVisible (rows.add (new ParamRow (params, id)));

        // One knob is not worth a preset.
        hasPresets = effect.isNotEmpty() && ids.size() > 1;

        if (hasPresets)
            reloadPresetList();

        placeholder.setVisible (false);
        title.setVisible (true);
        presetBox.setVisible (hasPresets);
        presetSave.setVisible (hasPresets);
        presetDelete.setVisible (hasPresets);

        resized();
        repaint();
    }

    // The amp's capture rides its presets: the provider names what is loaded
    // at save time, and the apply hook restores it (string-or-void semantics
    // as PresetStore documents them).
    std::function<juce::String()> ampCaptureProvider;
    std::function<void (const juce::var&)> onAmpCapture;

    // Back to the name-plate.
    void clear()
    {
        effect.clear();
        ids.clear();
        rows.clear();
        hasPresets = false;
        extraHeight = 30;

        if (extraComponent != nullptr)
        {
            removeChildComponent (extraComponent);
            extraComponent = nullptr;
        }

        placeholder.setVisible (true);
        title.setVisible (false);
        presetBox.setVisible (false);
        presetSave.setVisible (false);
        presetDelete.setVisible (false);

        repaint();
    }

    // Called when something outside changed the values, so the preset name on
    // show is no longer necessarily what you are hearing.
    void refresh()
    {
        for (auto* r : rows)
            r->refresh();

        if (hasPresets)
            presetBox.setSelectedId (0, juce::dontSendNotification);
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
        placeholder.setBounds (getLocalBounds());

        auto b = getLocalBounds().reduced (12, 8);

        title.setBounds (b.removeFromTop (26));
        b.removeFromTop (4);

        if (extraComponent != nullptr)
        {
            extraComponent->setBounds (b.removeFromTop (extraHeight).reduced (0, 2));
            b.removeFromTop (4);
        }

        if (hasPresets)
        {
            auto row = b.removeFromTop (presetRowHeight).reduced (0, 3);
            presetDelete.setBounds (row.removeFromRight (44));
            row.removeFromRight (4);
            presetSave.setBounds (row.removeFromRight (52));
            row.removeFromRight (6);
            presetBox.setBounds (row);
            b.removeFromTop (2);
        }

        for (auto* r : rows)
            r->setBounds (b.removeFromTop (ParamRow::preferredHeight));
    }

private:
    static constexpr int presetRowHeight = 30;

    void reloadPresetList (const juce::String& selectName = {})
    {
        presetBox.clear (juce::dontSendNotification);

        const auto names = presets.getEffectNames (effect);

        for (int i = 0; i < names.size(); ++i)
            presetBox.addItem (names[i], i + 1);

        const int index = names.indexOf (selectName);

        if (index >= 0)
            presetBox.setSelectedId (index + 1, juce::dontSendNotification);
        else
            presetBox.setText (selectName, juce::dontSendNotification);
    }

    void applyPreset()
    {
        juce::var capture;

        if (presets.applyEffect (effect, presetBox.getText(), params, &capture))
        {
            for (auto* r : rows)
                r->refresh();

            if (effect == "amp" && onAmpCapture != nullptr)
                onAmpCapture (capture);
        }
    }

    void savePreset()
    {
        const auto name = presetBox.getText().trim();

        if (name.isEmpty())
            return;

        const auto capture = effect == "amp" && ampCaptureProvider != nullptr
                               ? juce::var (ampCaptureProvider()) : juce::var();

        presets.storeEffect (effect, name, params, ids, capture);
        reloadPresetList (name);
    }

    void deletePreset()
    {
        const auto name = presetBox.getText().trim();

        if (name.isEmpty() || ! presets.effectContains (effect, name))
            return;

        presets.removeEffect (effect, name);
        reloadPresetList();
    }

    ParamStore& params;
    PresetStore& presets;
    juce::String effect;
    std::vector<int> ids;
    bool hasPresets = false;
    int extraHeight = 30;
    juce::Label placeholder, title;
    juce::ComboBox presetBox;
    juce::TextButton presetSave, presetDelete;
    juce::OwnedArray<ParamRow> rows;
    juce::Component* extraComponent = nullptr;   // not owned

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxEditor)
};

} // namespace rig
