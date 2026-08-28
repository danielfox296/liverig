#pragma once

#include <algorithm>
#include <cmath>

namespace rig
{

// Conditions the pitch tracker's raw stream into something a tuner can show.
//
// The tracker is accurate but honest: every analysis frame speaks, including
// the attack of a pluck, a moment of doubt mid-decay and the occasional
// octave flick. Painted raw at 30 Hz that is a needle with a nervous
// disorder. What a player turning a peg needs is the underlying pitch,
// steady and slightly behind the truth — which is exactly what the tuners
// people trust actually display.
//
// Three layers, all in semitone space where octaves are additive:
//
//  - a median over the last ~300 ms eats isolated outliers outright;
//  - the display glides toward that median over ~150 ms, so the needle
//    moves like a meter and not a strobe;
//  - the note name is locked with hysteresis, so a pitch parked on the
//    boundary between two names does not flicker between them.
//
// A genuinely new note must not glide: readings far from the display are
// held out as suspects, and three in a row snap the display and start the
// history over. Silence holds the last reading briefly — long enough to
// survive a flicker, short enough to feel honest.
class TunerFilter
{
public:
    // One conditioned frame. note is the smoothed pitch in fractional MIDI
    // numbering; locked is the note name to read it against, -1 when idle.
    struct Reading
    {
        bool live = false;
        float note = 0.0f;
        int locked = -1;
    };

    void reset() noexcept
    {
        holdTicks = 0;
        ringCount = ringWrite = farRun = 0;
        locked = -1;
    }

    // Call at the UI rate (~30 Hz), signal or not.
    Reading push (float hz, float confidence, float inputPeak) noexcept
    {
        const bool sounding = hz > 20.0f && confidence > 0.5f && inputPeak > 0.004f;

        if (sounding)
        {
            const float m = 69.0f + 12.0f * std::log2 (hz / 440.0f);
            const bool fresh = holdTicks <= 0;
            const bool far = ! fresh && std::abs (m - shown) > 1.5f;

            if (far && farRun < 2)
            {
                // One or two outlying frames are a tracker flick: hold what
                // is shown and wait to be convinced.
                ++farRun;
            }
            else
            {
                if (fresh || far)
                {
                    // First note after silence, or a confirmed move — snap
                    // rather than glide an octave across the dial.
                    ringCount = ringWrite = 0;
                    shown = m;
                }

                farRun = 0;

                ring[ringWrite] = m;
                ringWrite = (ringWrite + 1) % window;
                ringCount = std::min (ringCount + 1, window);

                float sorted[window];
                std::copy (ring, ring + ringCount, sorted);
                std::nth_element (sorted, sorted + ringCount / 2, sorted + ringCount);

                shown += slew * (sorted[ringCount / 2] - shown);
            }

            if (locked < 0 || std::abs (shown - (float) locked) > 0.6f)
                locked = (int) std::lround (shown);

            holdTicks = hold;
        }
        else if (holdTicks > 0)
        {
            --holdTicks;
            farRun = 0;   // a gap breaks any run of suspects
        }

        return { holdTicks > 0, shown, holdTicks > 0 ? locked : -1 };
    }

private:
    static constexpr int window = 9;      // ~300 ms of history at 30 Hz
    static constexpr int hold = 12;       // ~400 ms across a flicker or a chord change
    static constexpr float slew = 0.2f;   // ~150 ms glide to the median

    float ring[window] {};
    float shown = 0.0f;
    int ringCount = 0, ringWrite = 0, farRun = 0;
    int locked = -1;
    int holdTicks = 0;
};

} // namespace rig
