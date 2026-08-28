#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Parameters.h"
#include "../dsp/Biquad.h"
#include "Widgets.h"

namespace rig
{

// The parametric EQ's control surface: the response curve is the interface.
// Each band is a handle on the curve — drag it for frequency and gain, roll
// the wheel over it for Q, double-click it to flatten. The strip below shows
// the touched band's three values as ordinary faders, for the times a number
// has to be exact. The view only ever talks to the ParamStore, and a slow
// poll keeps it honest against preset loads and anything else that moves
// parameters behind its back.
class EqView : public juce::Component,
               private juce::Timer
{
public:
    static constexpr int preferredHeight = 306;

    explicit EqView (ParamStore& p) : params (p)
    {
        caption.setFont (rigFont (11.0f, true));
        caption.setColour (juce::Label::textColourId, juce::Colour (0xff9a9da6));
        addAndMakeVisible (caption);

        selectBand (0);
        pollValues();
        recomputeDisplay();
        startTimerHz (15);
    }

    // Shown again after anything could have moved the values — preset load,
    // chain recall — so catch up immediately rather than on the next poll.
    void parentHierarchyChanged() override
    {
        refreshFromParams();
    }

    void resized() override
    {
        auto b = getLocalBounds();

        auto strip = b.removeFromBottom (3 * ParamRow::preferredHeight + captionHeight);
        caption.setBounds (strip.removeFromTop (captionHeight));

        for (auto* r : rows)
            r->setBounds (strip.removeFromTop (ParamRow::preferredHeight));

        plot = b.reduced (0, 2).toFloat();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff101114));
        g.fillRoundedRectangle (plot, 4.0f);
        g.setColour (juce::Colour (0xff26272d));
        g.drawRoundedRectangle (plot.reduced (0.5f), 4.0f, 1.0f);

        const auto area = plotInner();
        const bool on = params.getBool (pEqOn);
        const float dim = on ? 1.0f : 0.4f;

        drawGrid (g, area);

        // Band curves first, faint; the sum rides on top in the accent.
        for (int band = 0; band < 5; ++band)
            drawResponse (g, area, band, bandColour (band).withMultipliedAlpha ((band == selected ? 0.75f : 0.4f) * dim), 1.2f);

        drawResponse (g, area, -1, juce::Colour (0xffd87b3a).withMultipliedAlpha (dim), 2.2f);

        for (int band = 0; band < 5; ++band)
            if (band != selected)
                drawHandle (g, area, band, dim);

        drawHandle (g, area, selected, dim);   // painted last so it never hides

        // While a handle is held or hovered, its numbers sit in the corner —
        // the eye stays on the curve instead of dropping to the faders.
        const int readoutBand = dragging >= 0 ? dragging : hovered;

        if (readoutBand >= 0)
        {
            const int base = pEq1Freq + readoutBand * 3;
            const float f = params.get (base);
            const float gainDb = params.get (base + 1);
            const float q = params.get (base + 2);

            g.setColour (bandColour (readoutBand));
            g.fillEllipse (area.getX() + 8.0f, area.getY() + 9.0f, 7.0f, 7.0f);
            g.setColour (juce::Colour (0xffe8e8ea));
            g.setFont (rigFont (11.0f, true));
            g.drawText (frequencyText (f) + "   " + juce::String (gainDb >= 0 ? "+" : "")
                          + juce::String (gainDb, 1) + " dB   Q " + juce::String (q, 2),
                        (int) area.getX() + 20, (int) area.getY() + 4, 220, 16,
                        juce::Justification::centredLeft);
        }

        g.setColour (juce::Colour (0xff565963));
        g.setFont (rigFont (9.5f));
        g.drawText (juce::String (juce::CharPointer_UTF8 ("drag band \xc2\xb7 wheel Q \xc2\xb7 double-click flat")),
                    (int) area.getRight() - 214, (int) area.getY() + 4, 208, 12,
                    juce::Justification::centredRight);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int hit = handleAt (e.position);

        if (hit != hovered)
        {
            hovered = hit;
            repaint();
        }

        setMouseCursor (hit >= 0 ? juce::MouseCursor::PointingHandCursor
                                 : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hovered != -1)
        {
            hovered = -1;
            repaint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int hit = handleAt (e.position);

        if (hit >= 0)
        {
            selectBand (hit);
            dragging = hit;
            dragStartFreq = params.get (pEq1Freq + hit * 3);
            dragStartGain = params.get (pEq1Freq + hit * 3 + 1);
        }
        else if (plot.contains (e.position))
        {
            // A tap on empty curve selects whichever band is nearest by
            // frequency — reach first, adjust second.
            selectBand (nearestBandByX (e.position.x));
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;

        const auto area = plotInner();
        const float fine = e.mods.isShiftDown() ? 0.25f : 1.0f;
        const float dx = (float) e.getDistanceFromDragStartX() * fine;
        const float dy = (float) e.getDistanceFromDragStartY() * fine;

        const int base = pEq1Freq + dragging * 3;
        const auto& fs = spec (base);
        const auto& gs = spec (base + 1);

        const float octavesAcross = std::log2 (freqMax / freqMin);
        const float f = dragStartFreq * std::exp2 (dx / area.getWidth() * octavesAcross);
        const float gainDb = dragStartGain - dy / area.getHeight() * (dbRange * 2.0f);

        params.set (base, juce::jlimit (fs.minValue, fs.maxValue, f));
        params.set (base + 1, juce::jlimit (gs.minValue, gs.maxValue, gainDb));

        refreshFromParams();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0)
        {
            dragging = -1;
            repaint();
        }
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int hit = handleAt (e.position);

        if (hit >= 0)
        {
            params.set (pEq1Freq + hit * 3 + 1, 0.0f);
            refreshFromParams();
        }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (! plot.contains (e.position))
            return;

        const int band = hovered >= 0 ? hovered : selected;
        const int qId = pEq1Freq + band * 3 + 2;
        const auto& qs = spec (qId);

        // Scroll up sharpens. exp keeps the feel even across the whole range.
        const float q = params.get (qId) * std::exp (wheel.deltaY * 1.2f);
        params.set (qId, juce::jlimit (qs.minValue, qs.maxValue, q));

        if (band != selected)
            selectBand (band);

        refreshFromParams();
    }

private:
    static constexpr int captionHeight = 18;
    static constexpr float freqMin = 20.0f, freqMax = 20000.0f;
    static constexpr float dbRange = 15.0f;      // plot shows +/- this
    static constexpr double displayRate = 48000.0;

    static const char* bandTypeName (int band) noexcept
    {
        return band == 0 ? "LOW SHELF" : (band == 4 ? "HIGH SHELF" : "BELL");
    }

    static juce::Colour bandColour (int band) noexcept
    {
        static constexpr juce::uint32 colours[5] {
            0xff7f96e8, 0xff59b389, 0xffd8a13a, 0xffc9766e, 0xffa07fd8 };
        return juce::Colour (colours[juce::jlimit (0, 4, band)]);
    }

    juce::Rectangle<float> plotInner() const noexcept { return plot.reduced (10.0f, 8.0f); }

    float xForFreq (juce::Rectangle<float> area, float f) const noexcept
    {
        return area.getX() + std::log (f / freqMin) / std::log (freqMax / freqMin) * area.getWidth();
    }

    float freqForX (juce::Rectangle<float> area, float x) const noexcept
    {
        const float t = juce::jlimit (0.0f, 1.0f, (x - area.getX()) / area.getWidth());
        return freqMin * std::pow (freqMax / freqMin, t);
    }

    float yForDb (juce::Rectangle<float> area, float db) const noexcept
    {
        return area.getCentreY() - db / dbRange * area.getHeight() * 0.5f;
    }

    static juce::String frequencyText (float f)
    {
        return f >= 1000.0f ? juce::String (f / 1000.0f, f >= 10000.0f ? 1 : 2) + " kHz"
                            : juce::String ((int) std::lround (f)) + " Hz";
    }

    void drawGrid (juce::Graphics& g, juce::Rectangle<float> area)
    {
        g.setColour (juce::Colour (0xff1d1f26));

        for (float f : { 30.0f, 50.0f, 100.0f, 200.0f, 300.0f, 500.0f, 1000.0f,
                         2000.0f, 3000.0f, 5000.0f, 10000.0f })
        {
            const float x = xForFreq (area, f);
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        }

        for (float db : { -12.0f, -6.0f, 6.0f, 12.0f })
            g.drawHorizontalLine ((int) yForDb (area, db), area.getX(), area.getRight());

        g.setColour (juce::Colour (0xff34363f));
        g.drawHorizontalLine ((int) yForDb (area, 0.0f), area.getX(), area.getRight());

        g.setColour (juce::Colour (0xff5c5f68));
        g.setFont (rigFont (9.0f));

        for (float f : { 100.0f, 1000.0f, 10000.0f })
            g.drawText (f >= 1000.0f ? juce::String ((int) (f / 1000.0f)) + "k"
                                     : juce::String ((int) f),
                        (int) xForFreq (area, f) - 16,
                        (int) area.getBottom() - 12, 32, 11, juce::Justification::centred);

        for (float db : { -12.0f, 12.0f })
            g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db),
                        (int) area.getX() + 2, (int) yForDb (area, db) - 5, 26, 10,
                        juce::Justification::centredLeft);
    }

    // band -1 draws the sum of all five.
    void drawResponse (juce::Graphics& g, juce::Rectangle<float> area, int band,
                       juce::Colour colour, float thickness)
    {
        juce::Path path;
        const int steps = juce::jmax (48, (int) (area.getWidth() / 3.0f));

        for (int i = 0; i <= steps; ++i)
        {
            const float f = freqForX (area, area.getX() + area.getWidth() * (float) i / (float) steps);
            float db = 0.0f;

            if (band >= 0)
            {
                db = display[band].magnitudeDb (displayRate, f);
            }
            else
            {
                for (auto& filter : display)
                    db += filter.magnitudeDb (displayRate, f);
            }

            const float x = xForFreq (area, f);
            const float y = juce::jlimit (area.getY(), area.getBottom(),
                                          yForDb (area, db));

            if (i == 0)
                path.startNewSubPath (x, y);
            else
                path.lineTo (x, y);
        }

        if (band < 0)
        {
            // A whisper of fill under the sum reads as "this is the result".
            juce::Path fill (path);
            fill.lineTo (area.getRight(), yForDb (area, 0.0f));
            fill.lineTo (area.getX(), yForDb (area, 0.0f));
            fill.closeSubPath();
            g.setColour (colour.withMultipliedAlpha (0.08f));
            g.fillPath (fill);
        }

        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (thickness,
                                                  juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    }

    void drawHandle (juce::Graphics& g, juce::Rectangle<float> area, int band, float dim)
    {
        const auto centre = handleCentre (area, band);
        const bool isSelected = band == selected;
        const bool isHot = isSelected || band == hovered;
        const float r = isHot ? 9.0f : 8.0f;
        const auto colour = bandColour (band).withMultipliedAlpha (dim);

        g.setColour (isSelected ? colour : juce::Colour (0xff1c1e24).withMultipliedAlpha (dim));
        g.fillEllipse (centre.x - r, centre.y - r, r * 2.0f, r * 2.0f);

        g.setColour (isSelected ? juce::Colour (0xffe8e8ea).withMultipliedAlpha (dim) : colour);
        g.drawEllipse (centre.x - r, centre.y - r, r * 2.0f, r * 2.0f, isHot ? 1.6f : 1.2f);

        g.setColour ((isSelected ? juce::Colour (0xff101114)
                                 : juce::Colour (0xffc8c9ce)).withMultipliedAlpha (dim));
        g.setFont (rigFont (10.0f, true));
        g.drawText (juce::String (band + 1), (int) (centre.x - r), (int) (centre.y - r),
                    (int) (r * 2.0f), (int) (r * 2.0f), juce::Justification::centred);
    }

    juce::Point<float> handleCentre (juce::Rectangle<float> area, int band) const noexcept
    {
        const int base = pEq1Freq + band * 3;
        return { xForFreq (area, params.get (base)),
                 juce::jlimit (area.getY(), area.getBottom(),
                               yForDb (area, params.get (base + 1))) };
    }

    int handleAt (juce::Point<float> pos) const noexcept
    {
        const auto area = plotInner();
        int best = -1;
        float bestDist = 15.0f;

        for (int band = 0; band < 5; ++band)
        {
            const float d = handleCentre (area, band).getDistanceFrom (pos);

            if (d < bestDist)
            {
                bestDist = d;
                best = band;
            }
        }

        return best;
    }

    int nearestBandByX (float x) const noexcept
    {
        const auto area = plotInner();
        int best = 0;
        float bestDist = 1.0e9f;

        for (int band = 0; band < 5; ++band)
        {
            const float d = std::abs (handleCentre (area, band).x - x);

            if (d < bestDist)
            {
                bestDist = d;
                best = band;
            }
        }

        return best;
    }

    void selectBand (int band)
    {
        selected = juce::jlimit (0, 4, band);

        caption.setText ("BAND " + juce::String (selected + 1)
                           + juce::String (juce::CharPointer_UTF8 ("  \xc2\xb7  "))
                           + bandTypeName (selected), juce::dontSendNotification);
        caption.setColour (juce::Label::textColourId, bandColour (selected));

        const int base = pEq1Freq + selected * 3;
        rows.clear();

        for (int offset = 0; offset < 3; ++offset)
            addAndMakeVisible (rows.add (new ParamRow (params, base + offset)));

        resized();
        repaint();
    }

    void recomputeDisplay()
    {
        for (int band = 0; band < 5; ++band)
        {
            const int base = pEq1Freq + band * 3;
            const float f = params.get (base);
            const float gainDb = params.get (base + 1);
            const float q = params.get (base + 2);

            if (band == 0)
                display[0].setLowShelfQ (displayRate, f, q, gainDb);
            else if (band == 4)
                display[4].setHighShelfQ (displayRate, f, q, gainDb);
            else
                display[band].setPeak (displayRate, f, q, gainDb);
        }
    }

    void refreshFromParams()
    {
        pollValues();
        recomputeDisplay();

        for (auto* r : rows)
            r->refresh();

        repaint();
    }

    bool pollValues()
    {
        bool moved = false;

        for (int i = 0; i < 16; ++i)
        {
            const float v = params.get (pEqOn + i);

            if (! juce::exactlyEqual (seen[(size_t) i], v))
            {
                seen[(size_t) i] = v;
                moved = true;
            }
        }

        return moved;
    }

    // Presets, chain recalls and anything else that writes the ParamStore
    // land here: the poll notices, the display follows.
    void timerCallback() override
    {
        if (pollValues())
        {
            recomputeDisplay();

            for (auto* r : rows)
                r->refresh();

            repaint();
        }
    }

    ParamStore& params;

    Biquad display[5];
    float seen[16] {};

    juce::Rectangle<float> plot;
    juce::Label caption;
    juce::OwnedArray<ParamRow> rows;

    int selected = 0;
    int hovered = -1;
    int dragging = -1;
    float dragStartFreq = 0.0f, dragStartGain = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqView)
};

} // namespace rig
