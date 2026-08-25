#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Parameters.h"

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
class ParamKnob : public juce::Component
{
public:
    ParamKnob (ParamStore& p, int paramId) : params (p), id (paramId)
    {
        const auto& s = spec (id);

        name.setText (s.label, juce::dontSendNotification);
        name.setJustificationType (juce::Justification::centred);
        name.setFont (juce::Font (juce::FontOptions (11.0f)));
        addAndMakeVisible (name);

        juce::NormalisableRange<double> range ((double) s.minValue, (double) s.maxValue, (double) s.step);

        if (s.skewCentre > 0.0f)
            range.setSkewForCentre ((double) s.skewCentre);

        slider.setNormalisableRange (range);
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 58, 15);
        slider.setDoubleClickReturnValue (true, (double) s.defaultValue);

        // Set before the first setValue, or the text box renders the raw double.
        const auto localSpec = s;
        slider.textFromValueFunction = [localSpec] (double v)
        {
            return juce::String (v, localSpec.decimals) + localSpec.suffix;
        };

        slider.setValue ((double) params.get (id), juce::dontSendNotification);
        slider.updateText();
        slider.onValueChange = [this] { params.set (id, (float) slider.getValue()); };
        addAndMakeVisible (slider);
    }

    void refresh()
    {
        slider.setValue ((double) params.get (id), juce::dontSendNotification);
        slider.updateText();
    }

    void resized() override
    {
        auto b = getLocalBounds();
        name.setBounds (b.removeFromTop (13));
        slider.setBounds (b);
    }

private:
    ParamStore& params;
    int id;
    juce::Label name;
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
class FxPanel : public juce::Component
{
public:
    FxPanel (ParamStore& p, const juce::String& titleText, int bypassParam,
             const std::vector<int>& knobIds, juce::Component* extra = nullptr)
        : params (p), bypassId (bypassParam), extraComponent (extra)
    {
        title.setText (titleText, juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        addAndMakeVisible (title);

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

        for (int id : knobIds)
            addAndMakeVisible (knobs.add (new ParamKnob (params, id)));

        if (extraComponent != nullptr)
            addAndMakeVisible (extraComponent);
    }

    void refresh()
    {
        if (bypassId >= 0)
            power.setToggleState (params.getBool (bypassId), juce::dontSendNotification);

        for (auto* k : knobs)
            k->refresh();
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
        auto b = getLocalBounds().reduced (8, 5);
        auto left = b.removeFromLeft (86);
        title.setBounds (left.removeFromTop (16));

        if (bypassId >= 0)
            power.setBounds (left.removeFromTop (22).removeFromLeft (52));

        if (extraComponent != nullptr)
        {
            left.removeFromTop (4);
            extraComponent->setBounds (left.removeFromTop (10).removeFromLeft (78));
        }

        if (! knobs.isEmpty())
        {
            const int each = b.getWidth() / knobs.size();

            for (auto* k : knobs)
                k->setBounds (b.removeFromLeft (each).reduced (2, 0));
        }
    }

private:
    ParamStore& params;
    int bypassId;
    juce::Label title;
    juce::TextButton power;
    juce::OwnedArray<ParamKnob> knobs;
    juce::Component* extraComponent = nullptr;
};

} // namespace rig
