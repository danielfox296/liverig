#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Parameters.h"
#include "../PresetStore.h"

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
// One parameter as a labelled horizontal fader. Rotary knobs need a slow,
// accurate drag; this is a wide target you can shove with one hand mid-song.
class ParamRow : public juce::Component
{
public:
    static constexpr int preferredHeight = 30;

    ParamRow (ParamStore& p, int paramId) : params (p), id (paramId)
    {
        const auto& s = spec (id);

        name.setText (s.label, juce::dontSendNotification);
        name.setFont (juce::Font (juce::FontOptions (12.0f)));
        name.setColour (juce::Label::textColourId, juce::Colour (0xff9a9da6));
        addAndMakeVisible (name);

        value.setFont (juce::Font (juce::FontOptions (12.0f)));
        value.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (value);

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
        slider.setValue ((double) params.get (id), juce::dontSendNotification);
        updateValueText();
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (0, 2);
        name.setBounds (b.removeFromLeft (74));
        value.setBounds (b.removeFromRight (72));
        b.removeFromRight (6);
        slider.setBounds (b);
    }

private:
    void updateValueText()
    {
        const auto& s = spec (id);
        value.setText (juce::String (slider.getValue(), s.decimals) + s.suffix,
                       juce::dontSendNotification);
    }

    ParamStore& params;
    int id;
    juce::Label name, value;
    juce::Slider slider;
};

//==============================================================================
class LevelMeter : public juce::Component
{
public:
    explicit LevelMeter (int numBars = 1) : bars ((size_t) juce::jmax (1, numBars), 0.0f) {}

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
        name.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        name.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (name);

        status.setJustificationType (juce::Justification::centred);
        status.setFont (juce::Font (juce::FontOptions (10.0f)));
        status.setColour (juce::Label::textColourId, juce::Colour (0xff70737c));
        addAndMakeVisible (status);

        const auto& s = spec (gainId);
        gain.setRange ((double) s.minValue, (double) s.maxValue, 0.1);
        gain.setSliderStyle (juce::Slider::LinearVertical);
        gain.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 54, 17);
        gain.setDoubleClickReturnValue (true, (double) s.defaultValue);
        gain.textFromValueFunction = [] (double v) { return juce::String (v, 1) + " dB"; };
        gain.setValue ((double) params.get (gainId), juce::dontSendNotification);
        gain.updateText();
        gain.onValueChange = [this] { params.set (gainId, (float) gain.getValue()); };
        addAndMakeVisible (gain);

        mute.setButtonText ("MUTE");
        mute.setWantsKeyboardFocus (false);
        mute.setClickingTogglesState (true);
        mute.setToggleState (params.getBool (muteId), juce::dontSendNotification);
        mute.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe4483c));
        mute.onClick = [this] { params.set (muteId, mute.getToggleState() ? 1.0f : 0.0f); };
        addAndMakeVisible (mute);
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

        name.setBounds (b.removeFromTop (16));
        status.setBounds (b.removeFromTop (13));
        mute.setBounds (b.removeFromBottom (24));
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
// One effect. Collapsed it is a single row — name, on/off, and nothing to
// catch a sleeve on. Expanded it shows its faders.
class FxPanel : public juce::Component
{
public:
    static constexpr int headerHeight = 34;

    FxPanel (ParamStore& p, PresetStore& ps, const juce::String& effectKey,
             const juce::String& titleText, int bypassParam,
             const std::vector<int>& paramIds, juce::Component* extra = nullptr)
        : params (p), presets (ps), effect (effectKey), bypassId (bypassParam),
          ids (paramIds), extraComponent (extra)
    {
        title.setText (titleText, juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        title.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (title);

        disclosure.setButtonText ("+");
        disclosure.setWantsKeyboardFocus (false);
        disclosure.onClick = [this] { setExpanded (! expanded); };
        addAndMakeVisible (disclosure);

        if (bypassId >= 0)
        {
            power.setButtonText ("ON");
            power.setWantsKeyboardFocus (false);
            power.setClickingTogglesState (true);
            power.setToggleState (params.getBool (bypassId), juce::dontSendNotification);
            power.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3f9d4a));
            power.onClick = [this] { params.set (bypassId, power.getToggleState() ? 1.0f : 0.0f); };
            addAndMakeVisible (power);
        }

        for (int id : paramIds)
            addAndMakeVisible (rows.add (new ParamRow (params, id)));

        // One knob is not worth a preset.
        hasPresets = effect.isNotEmpty() && paramIds.size() > 1;

        if (hasPresets)
        {
            presetBox.setEditableText (true);
            presetBox.setTextWhenNothingSelected ("Preset");
            presetBox.onChange = [this] { applyPreset(); };
            addAndMakeVisible (presetBox);

            presetSave.setButtonText ("Save");
            presetSave.setWantsKeyboardFocus (false);
            presetSave.onClick = [this] { savePreset(); };
            addAndMakeVisible (presetSave);

            presetDelete.setButtonText ("Del");
            presetDelete.setWantsKeyboardFocus (false);
            presetDelete.onClick = [this] { deletePreset(); };
            addAndMakeVisible (presetDelete);

            reloadPresetList();
        }

        if (extraComponent != nullptr)
            addAndMakeVisible (extraComponent);

        setExpanded (false);
    }

    std::function<void()> onExpandChanged;

    bool isExpanded() const noexcept { return expanded; }

    void setExpanded (bool shouldExpand)
    {
        expanded = shouldExpand;
        disclosure.setButtonText (expanded ? "-" : "+");

        for (auto* r : rows)
            r->setVisible (expanded);

        if (hasPresets)
        {
            presetBox.setVisible (expanded);
            presetSave.setVisible (expanded);
            presetDelete.setVisible (expanded);
        }

        if (onExpandChanged != nullptr)
            onExpandChanged();

        resized();
        repaint();
    }

    int getPreferredHeight() const
    {
        if (! expanded)
            return headerHeight;

        return headerHeight + (hasPresets ? presetRowHeight : 0)
                 + rows.size() * ParamRow::preferredHeight + 6;
    }

    // Called when something outside changed the values, so the preset name on
    // show is no longer necessarily what you are hearing.
    void refresh()
    {
        refreshRows();

        if (hasPresets)
            presetBox.setSelectedId (0, juce::dontSendNotification);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.y < headerHeight)
            setExpanded (! expanded);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff17181c));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        g.setColour (juce::Colour (expanded ? 0xff3c3f49 : 0xff26272d));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        auto header = b.removeFromTop (headerHeight).reduced (8, 5);

        disclosure.setBounds (header.removeFromLeft (24));
        header.removeFromLeft (6);

        if (bypassId >= 0)
            power.setBounds (header.removeFromRight (58));

        if (extraComponent != nullptr)
        {
            header.removeFromRight (8);
            extraComponent->setBounds (header.removeFromRight (90).reduced (0, 6));
        }

        title.setBounds (header);

        if (expanded)
        {
            b = b.reduced (10, 0);
            b.removeFromBottom (6);

            if (hasPresets)
            {
                auto row = b.removeFromTop (presetRowHeight).reduced (0, 3);
                presetDelete.setBounds (row.removeFromRight (44));
                row.removeFromRight (4);
                presetSave.setBounds (row.removeFromRight (52));
                row.removeFromRight (6);
                presetBox.setBounds (row);
            }

            for (auto* r : rows)
                r->setBounds (b.removeFromTop (ParamRow::preferredHeight));
        }
    }

private:
    static constexpr int presetRowHeight = 30;

    void refreshRows()
    {
        if (bypassId >= 0)
            power.setToggleState (params.getBool (bypassId), juce::dontSendNotification);

        for (auto* r : rows)
            r->refresh();
    }

    void reloadPresetList (const juce::String& selectName = {})
    {
        presetBox.clear (juce::dontSendNotification);

        const auto names = presets.getEffectNames (effect);

        for (int i = 0; i < names.size(); ++i)
            presetBox.addItem (names[i], i + 1);

        const int index = names.indexOf (selectName);

        if (index >= 0)
            presetBox.setSelectedId (index + 1, juce::dontSendNotification);
        else if (selectName.isNotEmpty())
            presetBox.setText (selectName, juce::dontSendNotification);
    }

    void applyPreset()
    {
        if (presets.applyEffect (effect, presetBox.getText(), params))
            refreshRows();
    }

    void savePreset()
    {
        const auto name = presetBox.getText().trim();

        if (name.isEmpty())
            return;

        presets.storeEffect (effect, name, params, ids);
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
    int bypassId;
    std::vector<int> ids;
    bool expanded = false;
    bool hasPresets = false;
    juce::Label title;
    juce::TextButton disclosure, power;
    juce::ComboBox presetBox;
    juce::TextButton presetSave, presetDelete;
    juce::OwnedArray<ParamRow> rows;
    juce::Component* extraComponent = nullptr;
};

} // namespace rig
