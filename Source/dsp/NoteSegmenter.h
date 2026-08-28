#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rig
{

// One MIDI event on its way out of the audio thread.
struct NoteEvent
{
    enum Type : uint8_t { noteOn, noteOff, bend, expression };

    Type type = noteOn;
    int16_t a = 0;   // note number, or a 14-bit bend, or a CC value
    int16_t b = 0;   // velocity
};

// Turns the pitch tracker's continuous estimate into discrete notes.
//
// Aimed at swells and pads, and that is what makes it tractable. With no
// transient to hit, the segmenter can spend time being certain rather than
// being early: a candidate pitch must hold for Hold milliseconds before it is
// allowed to speak. That wait is what keeps octave errors and the noise of a
// pluck out of a note that is about to sustain for bars.
//
// It never trusts the period on its own. The tracker deliberately holds its
// last good period through unvoiced passages so the octave shifter does not
// drop out mid-note. For MIDI that same kindness would sustain a note that has
// already finished, so liveness here comes from confidence and level only, and
// the period is read for pitch alone.
class NoteSegmenter
{
public:
    static constexpr float confidenceFloor = 0.5f;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        attack = 1.0f - std::exp (-1.0f / (0.004f * (float) sr));
        release = 1.0f - std::exp (-1.0f / (0.090f * (float) sr));
        maxNoteSamples = (int64_t) (sr * 30.0);   // a backstop, not a musical limit
        setParams (-42.0f, 45.0f, 180.0f, 12, 2);
        reset();
    }

    void reset() noexcept
    {
        env = 0.0f;
        sounding = -1;
        candidate = -1;
        candidateSamples = 0;
        quietSamples = 0;
        graceSamples = 0;
        noteSamples = 0;
        lastBend = 8192;
        lastExpression = -1;
    }

    void setParams (float gateDb, float holdMs, float releaseMs,
                    int transposeSemis, int bendRangeSemis) noexcept
    {
        gate = gateDb;
        holdSamples = (int64_t) (holdMs * 0.001 * sr);
        releaseSamples = (int64_t) (releaseMs * 0.001 * sr);
        transpose = transposeSemis;
        bendRange = bendRangeSemis;
    }

    // Was a note left sounding? The UI shows it and the panic button clears it.
    int getSoundingNote() const noexcept { return sounding; }

    // Ends any sounding note. Returns the number of events written.
    int flush (NoteEvent* out, int maxEvents) noexcept
    {
        if (sounding < 0 || maxEvents < 1)
            return 0;

        out[0] = { NoteEvent::noteOff, (int16_t) sounding, 0 };
        sounding = -1;
        candidate = -1;
        candidateSamples = 0;
        return 1;
    }

    int process (const float* in, int numSamples, float periodSamples, float confidence,
                 NoteEvent* out, int maxEvents) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float rect = std::abs (in[i]);
            env += (rect > env ? attack : release) * (rect - env);
        }

        const float levelDb = 20.0f * std::log10 (env + 1.0e-9f);
        const bool live = confidence >= confidenceFloor && levelDb > gate;

        const double hz = sr / std::max (8.0f, periodSamples);
        const double exact = 69.0 + 12.0 * std::log2 (hz / 440.0) + (double) transpose;
        const int nearest = (int) std::lround (exact);

        int written = 0;

        if (live && nearest >= 0 && nearest <= 127)
        {
            quietSamples = 0;
            graceSamples = 0;

            // Measured from the candidate's centre, not re-rounded every
            // frame: a pitch sat on a semitone boundary must not flip the
            // candidate back and forth and reset the hold each time.
            if (candidate >= 0 && std::abs (exact - (double) candidate) <= 0.65)
                candidateSamples += numSamples;
            else
            {
                candidate = nearest;
                candidateSamples = numSamples;
            }
        }
        else
        {
            quietSamples += numSamples;
            graceSamples += numSamples;

            // Confidence flickers frame to frame on real playing. One
            // doubtful frame must not erase a hold that is nearly served, so
            // a candidate survives a short grace — frozen, not accumulating.
            if (graceSamples > holdSamples / 3)
            {
                candidate = -1;
                candidateSamples = 0;
            }
        }

        const bool settled = candidate >= 0 && candidateSamples >= holdSamples;

        // Displacing a note that is already sounding is a bigger claim than
        // starting one, so it costs more evidence: twice the hold, and the
        // pitch has to be clearly past the boundary rather than merely over
        // it. Without the second condition a slow, deep vibrato retriggers
        // every cycle, which on a pad is unusable.
        const bool clearlyElsewhere = sounding >= 0
                                        && std::abs (exact - (double) sounding) > 0.65;
        const bool settledEnoughToMove = candidate >= 0
                                           && candidateSamples >= holdSamples * 2
                                           && clearlyElsewhere;

        // The receiver's wheel is wherever the previous note's bends left it,
        // and nothing but an explicit recentre clears it — a note started on a
        // stale wheel plays detuned until the next bend happens to move, which
        // on a steady pitch is never.
        if (sounding < 0)
        {
            if (settled && written + 1 < maxEvents)
            {
                out[written++] = { NoteEvent::bend, (int16_t) 8192, 0 };
                out[written++] = { NoteEvent::noteOn, (int16_t) candidate, (int16_t) velocity (levelDb) };
                sounding = candidate;
                noteSamples = 0;
                lastBend = 8192;
                lastExpression = -1;
            }
        }
        else
        {
            noteSamples += numSamples;

            if (settledEnoughToMove && candidate != sounding)
            {
                // The new note goes out before the old one stops, so a pad
                // moves rather than gapping.
                if (written + 2 < maxEvents)
                {
                    out[written++] = { NoteEvent::bend, (int16_t) 8192, 0 };
                    out[written++] = { NoteEvent::noteOn, (int16_t) candidate, (int16_t) velocity (levelDb) };
                    out[written++] = { NoteEvent::noteOff, (int16_t) sounding, 0 };
                    sounding = candidate;
                    noteSamples = 0;
                    lastBend = 8192;
                    lastExpression = -1;
                }
            }
            else if (quietSamples >= releaseSamples || noteSamples >= maxNoteSamples)
            {
                if (written < maxEvents)
                {
                    out[written++] = { NoteEvent::noteOff, (int16_t) sounding, 0 };
                    sounding = -1;
                }
            }
        }

        if (sounding >= 0 && live)
        {
            if (bendRange > 0 && written < maxEvents)
            {
                const double offset = exact - (double) sounding;

                // Past the wheel's reach this is a transpose jump or a
                // tracker glitch, not vibrato: leave the wheel where it is
                // and let the retrigger logic deal in whole notes.
                if (std::abs (offset) <= (double) bendRange)
                {
                    const int value = std::clamp ((int) std::lround (8192.0 + 8191.0 * offset / bendRange),
                                                  0, 16383);

                    if (std::abs (value - lastBend) >= 32)
                    {
                        lastBend = value;
                        out[written++] = { NoteEvent::bend, (int16_t) value, 0 };
                    }
                }
            }

            if (written < maxEvents)
            {
                const int value = velocity (levelDb);

                if (lastExpression < 0 || std::abs (value - lastExpression) >= 2)
                {
                    lastExpression = value;
                    out[written++] = { NoteEvent::expression, (int16_t) value, 0 };
                }
            }
        }

        return written;
    }

private:
    // Level above the gate, curved so a swell reads as a rise rather than
    // slamming to full in its first few dB.
    int velocity (float levelDb) const noexcept
    {
        const float span = std::max (6.0f, -gate);
        const float norm = std::clamp ((levelDb - gate) / span, 0.0f, 1.0f);
        return std::clamp ((int) std::lround (127.0f * std::pow (norm, 0.6f)), 1, 127);
    }

    double sr = 48000.0;
    float env = 0.0f, attack = 0.01f, release = 0.001f;
    float gate = -42.0f;
    int transpose = 12, bendRange = 2;

    int64_t holdSamples = 2160, releaseSamples = 8640, maxNoteSamples = 1440000;
    int64_t candidateSamples = 0, quietSamples = 0, graceSamples = 0, noteSamples = 0;

    int sounding = -1, candidate = -1;
    int lastBend = 8192, lastExpression = -1;
};

} // namespace rig
