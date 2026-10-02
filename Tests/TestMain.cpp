// Offline validation harness for the parts of the rig you cannot hear by
// looking at the UI: pitch tracking accuracy in the bass register, the octave
// shifter's actual output pitch, glitch-freeness at grain boundaries, and
// worst-case per-block cost at small buffer sizes.
//
//   rig_tests                 synthetic tests
//   rig_tests <file.wav>      also run the tracker + shifter over a real DI take

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "EffectSlot.h"
#include "FootControl.h"
#include "FxChain.h"
#include "Looper.h"
#include "InstrumentHost.h"
#include "PresetStore.h"
#include "VocalChannel.h"
#include "dsp/NoteSegmenter.h"
#include "dsp/TunerFilter.h"
#include "StemPlayer.h"
#include "Parameters.h"
#include "dsp/AmpModel.h"
#include "dsp/GlueComp.h"
#include "dsp/Limiter.h"
#include "dsp/OctaveShifter.h"
#include "dsp/PitchTracker.h"
#include "dsp/PlateVerb.h"

using namespace rig;

static int failures = 0;

static void report (const char* label, bool ok, const juce::String& detail)
{
    if (! ok)
        ++failures;

    std::printf ("  %-42s %s   %s\n", label, ok ? "PASS" : "FAIL", detail.toRawUTF8());
}

static double cents (double a, double b)
{
    return 1200.0 * std::log2 (a / b);
}

//==============================================================================
// A plucked-bass proxy: strong fundamental, decaying harmonic stack, slow
// vibrato, so the tracker sees something with the same difficulties as a DI.
static void makeNote (std::vector<float>& out, double sr, double f0, double seconds, float amp = 0.4f)
{
    const int n = (int) (sr * seconds);
    out.assign ((size_t) n, 0.0f);

    static constexpr int harmonics = 8;
    double phase[harmonics] {};

    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double env = std::exp (-t * 0.8) * (1.0 - std::exp (-t * 220.0));
        const double vib = 1.0 + 0.0015 * std::sin (6.283185307179586 * 4.5 * t);
        double s = 0.0;

        for (int h = 0; h < harmonics; ++h)
        {
            const double amplitude = 1.0 / std::pow ((double) (h + 1), 1.35);
            s += amplitude * std::sin (phase[h]);
            phase[h] += 6.283185307179586 * f0 * vib * (h + 1) / sr;
        }

        out[(size_t) i] = (float) (s * env * amp * 0.55);
    }
}

// Independent pitch estimate of a rendered buffer (plain autocorrelation, so a
// bug in PitchTracker cannot hide behind itself).
static double estimateF0 (const float* x, int n, double sr, double lowHz, double highHz)
{
    const int minLag = (int) (sr / highHz);
    const int maxLag = juce::jmin (n / 2, (int) (sr / lowHz));

    std::vector<double> scores ((size_t) (maxLag + 1), -1.0e30);
    double bestScore = -1.0e30;

    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double num = 0.0, e0 = 0.0, e1 = 0.0;

        for (int i = lag; i < n; ++i)
        {
            num += (double) x[i] * x[i - lag];
            e0  += (double) x[i] * x[i];
            e1  += (double) x[i - lag] * x[i - lag];
        }

        scores[(size_t) lag] = num / (std::sqrt (e0 * e1) + 1.0e-12);
        bestScore = juce::jmax (bestScore, scores[(size_t) lag]);
    }

    // Prefer the earliest *peak* that explains the signal: correlation scores
    // 2P as highly as P, so an argmax lands on octaves at random, and anything
    // that triggers on the rising flank reports a period that is too short.
    for (int lag = minLag + 1; lag < maxLag; ++lag)
    {
        const double s1 = scores[(size_t) lag];

        if (s1 < bestScore * 0.9)
            continue;

        const double s0 = scores[(size_t) (lag - 1)];
        const double s2 = scores[(size_t) (lag + 1)];

        if (! (s1 > s0 && s1 >= s2))
            continue;

        const double denom = s0 + s2 - 2.0 * s1;
        const double offset = std::abs (denom) > 1.0e-12 ? 0.5 * (s0 - s2) / denom : 0.0;

        return sr / (lag + offset);
    }

    return sr / minLag;
}

//==============================================================================
static void testTracker (double sr, int block)
{
    std::printf ("\nPitch tracker  (%.0f Hz, %d-sample blocks)\n", sr, block);

    const double notes[] { 30.87, 34.65, 41.20, 49.00, 55.00, 61.74, 73.42, 82.41, 98.00, 110.00, 123.47, 164.81, 246.94 };

    double worst = 0.0;

    for (double f0 : notes)
    {
        std::vector<float> note;
        makeNote (note, sr, f0, 0.9);

        PitchTracker tracker;
        tracker.prepare (sr);

        for (int i = 0; i + block <= (int) note.size(); i += block)
            tracker.process (note.data() + i, block);

        const double detected = sr / tracker.getPeriodSamples();
        const double err = cents (detected, f0);
        worst = juce::jmax (worst, std::abs (err));

        report (juce::String (f0, 2).paddedLeft (' ', 6).toRawUTF8(),
                std::abs (err) < 8.0,
                juce::String (detected, 2) + " Hz, " + juce::String (err, 2) + " cents, conf "
                    + juce::String (tracker.getConfidence(), 2)
                    + ", coarse " + juce::String (sr / tracker.getCoarsePeriod(), 2) + " Hz");
    }

    std::printf ("  worst error across the set: %.2f cents\n", worst);
}

static void testTrackerOnSweep (double sr, int block)
{
    std::printf ("\nPitch tracker on a 40 -> 120 Hz glide\n");

    const double seconds = 6.0;
    const int n = (int) (sr * seconds);
    std::vector<float> sweep ((size_t) n, 0.0f);

    double phase = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double f = 40.0 * std::pow (3.0, t / seconds);   // 40 -> 120 Hz
        double s = 0.0;
        double ph = phase;

        for (int h = 1; h <= 6; ++h)
        {
            s += std::sin (ph * h) / std::pow ((double) h, 1.3);
        }

        sweep[(size_t) i] = (float) (s * 0.22);
        phase += 6.283185307179586 * f / sr;
    }

    PitchTracker tracker;
    tracker.prepare (sr);

    double worst = 0.0, sum = 0.0;
    int count = 0, tracked = 0;

    for (int i = 0; i + block <= n; i += block)
    {
        tracker.process (sweep.data() + i, block);

        const double t = (i + block) / sr;

        if (t < 0.4 || t > seconds - 0.05)
            continue;

        const double expected = 40.0 * std::pow (3.0, t / seconds);
        const double detected = sr / tracker.getPeriodSamples();
        const double err = std::abs (cents (detected, expected));

        ++count;
        sum += err;
        worst = juce::jmax (worst, err);

        if (err < 25.0)
            ++tracked;
    }

    const double within = 100.0 * tracked / juce::jmax (1, count);

    report ("glide tracked within 25 cents", within > 97.0,
            juce::String (within, 1) + "% of frames, mean "
                + juce::String (sum / juce::jmax (1, count), 1) + " cents, worst "
                + juce::String (worst, 1));
}

//==============================================================================
static void testShifter (double sr, int block)
{
    std::printf ("\nOctave shifter  (%.0f Hz, %d-sample blocks)\n", sr, block);

    const double notes[] { 30.87, 41.20, 55.00, 73.42, 98.00, 123.47 };
    const int intervals[] { 12, 7, -12 };

    for (int semis : intervals)
    {
        for (double f0 : notes)
        {
            std::vector<float> note;
            makeNote (note, sr, f0, 2.0, 0.35f);

            PitchTracker tracker;
            OctaveShifter shifter;
            tracker.prepare (sr);
            shifter.prepare (sr);
            shifter.setSemitones ((float) semis);

            std::vector<float> out ((size_t) note.size(), 0.0f);

            for (int i = 0; i + block <= (int) note.size(); i += block)
            {
                tracker.process (note.data() + i, block);
                shifter.setPeriod (tracker.getPeriodSamples());
                std::copy (note.begin() + i, note.begin() + i + block, out.begin() + i);
                shifter.process (out.data() + i, block);
            }

            const double expected = f0 * std::pow (2.0, semis / 12.0);

            // Measure over a settled stretch well after the attack.
            const int start = (int) (sr * 0.6);
            const int len = (int) (sr * 0.5);
            const double measured = estimateF0 (out.data() + start, len, sr,
                                                expected * 0.45, expected * 2.2);
            const double err = cents (measured, expected);

            // Discontinuity check: compare the largest sample-to-sample step
            // against what the band-limited signal alone can produce.
            double maxStep = 0.0, rms = 0.0;

            for (int i = start; i < start + len; ++i)
            {
                maxStep = juce::jmax (maxStep, (double) std::abs (out[(size_t) i] - out[(size_t) (i - 1)]));
                rms += (double) out[(size_t) i] * out[(size_t) i];
            }

            rms = std::sqrt (rms / len);
            const double stepBudget = 6.283185307179586 * expected * 8.0 / sr * rms * 4.0;

            report ((juce::String (semis > 0 ? "+" : "") + juce::String (semis) + " st from "
                        + juce::String (f0, 1) + " Hz").toRawUTF8(),
                    std::abs (err) < 12.0 && maxStep < stepBudget,
                    juce::String (measured, 2) + " Hz (" + juce::String (err, 1) + " cents), lag "
                        + juce::String (shifter.getLagSamples() / sr * 1000.0, 1) + " ms, step "
                        + juce::String (maxStep / juce::jmax (1.0e-9, stepBudget), 2) + " of budget");
        }
    }
}

// Notes changing under the shifter: the case a steady tone never exercises.
static void testNoteChanges (double sr, int block)
{
    std::printf ("\nOctave shifter across note changes  (+12 st)\n");

    const double sequence[] { 41.20, 55.00, 98.00, 73.42, 123.47, 41.20 };
    const double noteSeconds = 0.7;

    std::vector<float> signal;

    for (double f0 : sequence)
    {
        std::vector<float> note;
        makeNote (note, sr, f0, noteSeconds, 0.4f);
        signal.insert (signal.end(), note.begin(), note.end());
    }

    PitchTracker tracker;
    OctaveShifter shifter;
    tracker.prepare (sr);
    shifter.prepare (sr);
    shifter.setSemitones (12.0f);

    std::vector<float> out ((size_t) signal.size(), 0.0f);

    for (int i = 0; i + block <= (int) signal.size(); i += block)
    {
        tracker.process (signal.data() + i, block);
        shifter.setPeriod (tracker.getPeriodSamples());
        std::copy (signal.begin() + i, signal.begin() + i + block, out.begin() + i);
        shifter.process (out.data() + i, block);
    }

    const int noteLen = (int) (sr * noteSeconds);

    for (size_t n = 0; n < std::size (sequence); ++n)
    {
        const double expected = sequence[n] * 2.0;
        const int start = (int) n * noteLen + (int) (sr * 0.25);
        const int len = (int) (sr * 0.3);
        const double measured = estimateF0 (out.data() + start, len, sr, expected * 0.45, expected * 2.2);
        const double err = cents (measured, expected);

        report (("note " + juce::String ((int) n + 1) + ": " + juce::String (sequence[n], 1)
                    + " -> " + juce::String (expected, 1) + " Hz").toRawUTF8(),
                std::abs (err) < 15.0,
                juce::String (measured, 2) + " Hz, " + juce::String (err, 1) + " cents, settles within 250 ms");
    }
}

//==============================================================================
// Magnitude of one frequency in a buffer, normalised so a full-scale sine at
// that frequency reads about 1. Lets a test ask "is there a second harmonic"
// or "did the sub-octave appear" without an FFT.
static double goertzel (const float* x, int n, double sr, double freq)
{
    const double w = 2.0 * 3.141592653589793 * freq / sr;
    const double c = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const double s0 = (double) x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    return std::sqrt (std::max (0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / (0.5 * n);
}

static double rmsOf (const float* x, int n)
{
    double e = 0.0;

    for (int i = 0; i < n; ++i)
        e += (double) x[i] * x[i];

    return std::sqrt (e / juce::jmax (1, n));
}

static void makeSine (std::vector<float>& out, double sr, double freq, double seconds, float amp)
{
    const int n = (int) (sr * seconds);
    out.assign ((size_t) n, 0.0f);

    for (int i = 0; i < n; ++i)
        out[(size_t) i] = (float) (amp * std::sin (6.283185307179586 * freq * i / sr));
}

// Sustained harmonic tone — constant level once faded in, unlike makeNote's
// plucked decay, so a test can compare loud playing against quiet playing
// without the envelope moving underneath the comparison.
static void makeHarmonics (std::vector<float>& out, double sr, double f0, double seconds, float amp)
{
    const int n = (int) (sr * seconds);
    out.assign ((size_t) n, 0.0f);

    static constexpr int harmonics = 6;
    double phase[harmonics] {};

    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double env = 1.0 - std::exp (-t * 60.0);
        double s = 0.0;

        for (int h = 0; h < harmonics; ++h)
        {
            s += std::sin (phase[h]) / std::pow ((double) (h + 1), 1.2);
            phase[h] += 6.283185307179586 * f0 * (h + 1) / sr;
        }

        out[(size_t) i] = (float) (s * env * amp * 0.5);
    }
}

// Runs a mono buffer through a fresh chain and returns the left output.
static std::vector<float> renderChain (ParamStore& params, const std::vector<float>& in,
                                       double sr, int block, const int* stageOrder = nullptr)
{
    FxChain chain (params);
    chain.prepare (sr, block);

    if (stageOrder != nullptr)
        chain.setStageOrder (stageOrder);

    std::vector<float> mono ((size_t) block), left ((size_t) block), right ((size_t) block);
    std::vector<float> out;
    out.reserve (in.size());

    for (int i = 0; i + block <= (int) in.size(); i += block)
    {
        std::copy (in.begin() + i, in.begin() + i + block, mono.begin());
        juce::ScopedNoDenormals noDenormals;   // as the audio callback runs
        chain.process (mono.data(), left.data(), right.data(), block);

        for (int k = 0; k < block; ++k)
            out.push_back (left[(size_t) k]);
    }

    return out;
}

// Pitch spread across consecutive windows, in cents — how a test hears
// vibrato and delay warble.
static double pitchSpreadCents (const float* x, int n, double sr,
                                double lowHz, double highHz, double windowSeconds)
{
    const int win = (int) (sr * windowSeconds);
    double minHz = 1.0e9, maxHz = 0.0;
    int windows = 0;

    for (int start = 0; start + win <= n; start += win)
    {
        const double f = estimateF0 (x + start, win, sr, lowHz, highHz);

        if (f <= 0.0)
            continue;

        minHz = juce::jmin (minHz, f);
        maxHz = juce::jmax (maxHz, f);
        ++windows;
    }

    if (windows < 2)
        return 0.0;

    return cents (maxHz, minHz);
}

//==============================================================================
static void testEnvelopeFilter (double sr, int block)
{
    std::printf ("\nEnvelope filter  (%.0f Hz, %d-sample blocks)\n", sr, block);

    std::vector<float> loud, quiet;
    makeHarmonics (loud, sr, 98.0, 1.5, 0.5f);
    makeHarmonics (quiet, sr, 98.0, 1.5, 0.04f);

    const int from = (int) (sr * 0.5);

    // Upper harmonics relative to the fundamental — how open the filter sits.
    const auto upperShare = [sr] (const std::vector<float>& x, int start)
    {
        const int len = (int) x.size() - start;
        return (goertzel (x.data() + start, len, sr, 490.0)
                  + goertzel (x.data() + start, len, sr, 588.0))
                 / (goertzel (x.data() + start, len, sr, 98.0) + 1.0e-9);
    };

    ParamStore params;
    params.set (pFltOn, 1.0f);

    const double openLoudUp = upperShare (renderChain (params, loud, sr, block), from);
    const double openQuietUp = upperShare (renderChain (params, quiet, sr, block), from);

    report ("digging in opens the up sweep", openLoudUp > openQuietUp * 3.0,
            "upper harmonics " + juce::String (openQuietUp, 4) + " quiet -> "
                + juce::String (openLoudUp, 4) + " loud");

    params.set (pFltFrom, 900.0f);
    params.set (pFltTo, 150.0f);

    const double openLoudDown = upperShare (renderChain (params, loud, sr, block), from);
    const double openQuietDown = upperShare (renderChain (params, quiet, sr, block), from);

    report ("To below From sweeps the other way", openLoudDown < openQuietDown * 0.7,
            "upper harmonics " + juce::String (openQuietDown, 4) + " quiet -> "
                + juce::String (openLoudDown, 4) + " loud");

    params.set (pFltFrom, 120.0f);
    params.set (pFltTo, 4000.0f);
    params.set (pFltRes, 10.0f);
    params.set (pFltSens, 1.0f);

    const auto outHot = renderChain (params, loud, sr, block);

    float peak = 0.0f;
    bool finite = true;

    for (float v : outHot)
    {
        if (! std::isfinite (v))
            finite = false;

        peak = juce::jmax (peak, std::abs (v));
    }

    report ("stable at full resonance and sensitivity", finite && peak < 4.0f,
            "peak " + juce::String (peak, 3));
}

static void testBoost (double sr, int block)
{
    std::printf ("\nBoost  (%.0f Hz, %d-sample blocks)\n", sr, block);

    std::vector<float> sine;
    makeSine (sine, sr, 220.0, 1.0, 0.05f);

    ParamStore params;
    params.set (pBoostOn, 1.0f);
    params.set (pBoostGain, 12.0f);
    params.set (pBoostSheen, 0.0f);
    params.set (pBoostHair, 0.0f);

    const auto out = renderChain (params, sine, sr, block);

    const int from = (int) (sr * 0.4);
    const double gainDb = 20.0 * std::log10 (rmsOf (out.data() + from, (int) out.size() - from)
                                               / rmsOf (sine.data() + from, (int) sine.size() - from));

    report ("clean setting boosts by the number on the fader", std::abs (gainDb - 12.0) < 0.4,
            juce::String (gainDb, 2) + " dB for 12 dB set");

    std::vector<float> hot;
    makeSine (hot, sr, 220.0, 1.0, 0.5f);

    params.set (pBoostGain, 24.0f);
    params.set (pBoostHair, 1.0f);

    const auto slammed = renderChain (params, hot, sr, block);

    float peak = 0.0f;
    bool finite = true;

    for (float v : slammed)
    {
        if (! std::isfinite (v))
            finite = false;

        peak = juce::jmax (peak, std::abs (v));
    }

    const float linearPeak = 0.5f * juce::Decibels::decibelsToGain (24.0f);

    report ("full hair folds a slammed input over", finite && peak < linearPeak * 0.25f && peak > 0.05f,
            "peak " + juce::String (peak, 3) + " vs " + juce::String (linearPeak, 2) + " linear");
}

static void testDriveVoices (double sr, int block)
{
    std::printf ("\nDrive voices  (%.0f Hz, %d-sample blocks)\n", sr, block);

    const int from = (int) (sr * 0.5);
    const int len = (int) (sr * 0.5);

    const auto render = [&] (float voice, float drive, float stack, double freq, float amp)
    {
        std::vector<float> sine;
        makeSine (sine, sr, freq, 1.2, amp);

        ParamStore params;
        params.set (pDriveOn, 1.0f);
        params.set (pDriveVoice, voice);
        params.set (pDriveAmount, drive);
        params.set (pDriveStack, stack);
        params.set (pDriveTone, 12000.0f);   // keep the post filter out of the measurements

        return renderChain (params, sine, sr, block);
    };

    const auto harmonicShare = [&] (const std::vector<float>& x, double f0, int h)
    {
        return goertzel (x.data() + from, len, sr, f0 * h)
                 / (goertzel (x.data() + from, len, sr, f0) + 1.0e-9);
    };

    // Green's clean-low split: the same hard drive distorts an 82 Hz
    // fundamental far less than the plexi cascade does.
    {
        const auto green = render (1.0f, 20.0f, 0.0f, 82.0, 0.3f);
        const auto plexi = render (3.0f, 20.0f, 0.0f, 82.0, 0.3f);

        const double greenThd = harmonicShare (green, 82.0, 3);
        const double plexiThd = harmonicShare (plexi, 82.0, 3);

        report ("green keeps the lows clean", greenThd < plexiThd * 0.5,
                "3rd/1st at 82 Hz: " + juce::String (greenThd, 3) + " green vs "
                    + juce::String (plexiThd, 3) + " plexi");
    }

    // Gold at the bottom of the knob is transparent — the clean path carries
    // everything until the drive brings the clipper's share in.
    {
        std::vector<float> sine;
        makeSine (sine, sr, 220.0, 1.2, 0.3f);

        const auto gold = render (2.0f, 1.0f, 0.0f, 220.0, 0.3f);

        const double gainDb = 20.0 * std::log10 (rmsOf (gold.data() + from, len)
                                                   / rmsOf (sine.data() + from, len));
        const double thd = harmonicShare (gold, 220.0, 3);

        report ("gold at minimum drive is transparent", std::abs (gainDb) < 1.5 && thd < 0.02,
                juce::String (gainDb, 2) + " dB, 3rd/1st " + juce::String (thd, 4));
    }

    // Gold's germanium bias rounds the halves unevenly — even harmonics the
    // symmetric green clipper cannot make.
    {
        const auto green = render (1.0f, 8.0f, 0.0f, 110.0, 0.3f);
        const auto gold = render (2.0f, 8.0f, 0.0f, 110.0, 0.3f);

        const double evenGreen = harmonicShare (green, 110.0, 2);
        const double evenGold = harmonicShare (gold, 110.0, 2);

        report ("gold adds the even harmonics", evenGold > evenGreen * 3.0 && evenGold > 0.01,
                "2nd/1st " + juce::String (evenGreen, 4) + " green -> "
                    + juce::String (evenGold, 4) + " gold");
    }

    // Plexi's drooping rail: hit it hard and the effective gain comes down —
    // the compress-and-bloom a static waveshaper cannot do.
    {
        const auto quiet = render (3.0f, 12.0f, 0.3f, 220.0, 0.04f);
        const auto loud = render (3.0f, 12.0f, 0.3f, 220.0, 0.4f);

        const double gQuiet = rmsOf (quiet.data() + from, len) / 0.04;
        const double gLoud = rmsOf (loud.data() + from, len) / 0.4;

        report ("plexi compresses when leaned on", gQuiet > gLoud * 1.4,
                "gain x" + juce::String (gQuiet, 2) + " quiet vs x" + juce::String (gLoud, 2) + " loud");
    }

    // Every voice, slammed as hard as the panel allows, stays bounded.
    for (int v = 1; v <= 3; ++v)
    {
        const auto out = render ((float) v, 50.0f, 1.0f, 110.0, 0.9f);

        float peak = 0.0f;
        bool finite = true;

        for (float s : out)
        {
            if (! std::isfinite (s))
                finite = false;

            peak = juce::jmax (peak, std::abs (s));
        }

        report ((juce::String ("voice ") + juce::String (v) + " bounded at full tilt").toRawUTF8(),
                finite && peak < 2.5f, "peak " + juce::String (peak, 3));
    }
}

//==============================================================================
static void testGlueComp (double sr, int block)
{
    std::printf ("\nCompressor  (%.0f Hz, %d-sample blocks)\n", sr, block);

    // Static accuracy: 12 dB over a -20 dB threshold at 4:1 through a 6 dB
    // knee should shave about 9 dB.
    {
        GlueComp comp;
        comp.prepare (sr);
        comp.setThreshold (-20.0f);
        comp.setRatio (4.0f);
        comp.setAttack (1.0f);
        comp.setRelease (150.0f);

        std::vector<float> sine;
        makeSine (sine, sr, 220.0, 1.0, 0.4f);   // about -8 dBFS

        std::vector<float> out (sine.size());

        for (size_t i = 0; i < sine.size(); ++i)
            out[i] = comp.process (sine[i]);

        const int from = (int) (sr * 0.5);
        const int len = (int) (sr * 0.4);
        const double grDb = -20.0 * std::log10 (rmsOf (out.data() + from, len)
                                                  / rmsOf (sine.data() + from, len));

        report ("4:1 over the threshold shaves the right amount", grDb > 7.5 && grDb < 10.5,
                juce::String (grDb, 2) + " dB reduction for 9 expected");
    }

    // The release listens: a short stab recovers fast, a leaned-on passage
    // lets go at the set time.
    {
        const auto recoveryAfter = [&] (double holdSeconds)
        {
            GlueComp comp;
            comp.prepare (sr);
            comp.setThreshold (-20.0f);
            comp.setRatio (6.0f);
            comp.setAttack (0.5f);
            comp.setRelease (400.0f);

            const int hold = (int) (sr * holdSeconds);

            for (int i = 0; i < hold; ++i)
                comp.process (0.4f * (float) std::sin (6.283185307179586 * 220.0 * i / sr));

            int samples = 0;

            while (comp.getReductionDb() > 1.0f && samples < (int) sr)
            {
                comp.process (0.0f);
                ++samples;
            }

            return samples / sr * 1000.0;
        };

        const double afterStab = recoveryAfter (0.03);
        const double afterLean = recoveryAfter (2.0);

        report ("release recovers faster after a stab than a lean",
                afterStab < afterLean * 0.6 && afterLean > 100.0,
                juce::String (afterStab, 0) + " ms vs " + juce::String (afterLean, 0) + " ms");
    }

    // Gain reduction grows smoothly and monotonically through the knee.
    {
        GlueComp comp;
        double lastGr = -1.0;
        bool monotone = true;

        for (double levelDb = -30.0; levelDb <= 0.0; levelDb += 2.0)
        {
            comp.prepare (sr);
            comp.setThreshold (-18.0f);
            comp.setRatio (4.0f);
            comp.setAttack (0.5f);
            comp.setRelease (100.0f);

            const float amp = (float) std::pow (10.0, levelDb / 20.0);

            for (int i = 0; i < (int) (sr * 0.4); ++i)
                comp.process (amp * (float) std::sin (6.283185307179586 * 220.0 * i / sr));

            const double gr = comp.getReductionDb();

            if (gr < lastGr - 0.1)
                monotone = false;

            lastGr = gr;
        }

        report ("knee is smooth and monotone", monotone && lastGr > 10.0,
                "GR at 0 dBFS " + juce::String (lastGr, 1) + " dB");
    }

    // Through the chain: blend 0 with the compressor engaged equals the
    // compressor switched off, exactly.
    {
        std::vector<float> note;
        makeNote (note, sr, 110.0, 1.0, 0.4f);

        ParamStore params;
        const auto off = renderChain (params, note, sr, block);

        params.set (pCompOn, 1.0f);
        params.set (pCompBlend, 0.0f);
        params.set (pCompMakeup, 6.0f);

        const auto zeroBlend = renderChain (params, note, sr, block);

        double worst = 0.0;

        for (size_t i = 0; i < off.size(); ++i)
            worst = juce::jmax (worst, (double) std::abs (zeroBlend[i] - off[i]));

        report ("blend at zero is the dry path exactly", worst < 1.0e-6,
                "worst deviation " + juce::String (worst, 8));
    }
}

static void testSynth (double sr, int block)
{
    std::printf ("\nSynth voice  (%.0f Hz, %d-sample blocks)\n", sr, block);

    std::vector<float> note;
    makeNote (note, sr, 98.0, 2.0, 0.4f);

    ParamStore params;
    params.set (pSynOn, 1.0f);
    params.set (pSynMix, 1.0f);
    params.set (pSynShape, 1.0f);
    params.set (pSynSub, 0.0f);

    const auto saw = renderChain (params, note, sr, block);

    const int from = (int) (sr * 0.6);
    const int len = (int) (sr * 0.8);
    const double f0 = estimateF0 (saw.data() + from, len, sr, 40.0, 300.0);
    const double err = cents (f0, 98.0);

    report ("voice sits on the tracked pitch", std::abs (err) < 25.0,
            juce::String (f0, 2) + " Hz vs 98 Hz, " + juce::String (err, 1) + " cents");

    const double subOff = goertzel (saw.data() + from, len, sr, 49.0)
                            / (goertzel (saw.data() + from, len, sr, 98.0) + 1.0e-9);

    params.set (pSynShape, 0.0f);
    params.set (pSynSub, 1.0f);

    const auto withSub = renderChain (params, note, sr, block);
    const double subOn = goertzel (withSub.data() + from, len, sr, 49.0)
                           / (goertzel (withSub.data() + from, len, sr, 98.0) + 1.0e-9);

    report ("sub fader brings in the octave-down square", subOn > subOff * 4.0 && subOn > 0.2,
            "sub/fundamental " + juce::String (subOff, 3) + " -> " + juce::String (subOn, 3));

    const double vRms = rmsOf (withSub.data() + from, len);
    const double inRms = rmsOf (note.data() + from, len);

    report ("keyed level lands near the playing level",
            vRms > inRms * 0.25 && vRms < inRms * 4.0,
            juce::String (20.0 * std::log10 (vRms / (inRms + 1.0e-12)), 1) + " dB relative to input");

    std::vector<float> silence ((size_t) (sr * 1.0), 0.0f);
    const auto quiet = renderChain (params, silence, sr, block);

    report ("silence in, silence out", rmsOf (quiet.data(), (int) quiet.size()) < 1.0e-4,
            "rms " + juce::String (rmsOf (quiet.data(), (int) quiet.size()), 6));
}

static void testChorus (double sr, int block)
{
    std::printf ("\nChorus  (%.0f Hz, %d-sample blocks)\n", sr, block);

    std::vector<float> sine;
    makeSine (sine, sr, 440.0, 2.5, 0.3f);

    ParamStore params;
    params.set (pChoOn, 1.0f);
    params.set (pChoMix, 1.0f);
    params.set (pChoVoices, 1.0f);
    params.set (pChoRate, 2.0f);
    params.set (pChoDepth, 0.8f);
    params.set (pChoLows, 20.0f);

    const auto wet = renderChain (params, sine, sr, block);
    const int from = (int) (sr * 0.5);
    const double spreadOn = pitchSpreadCents (wet.data() + from, (int) wet.size() - from,
                                              sr, 300.0, 600.0, 0.06);

    params.set (pChoDepth, 0.0f);
    const auto still = renderChain (params, sine, sr, block);
    const double spreadOff = pitchSpreadCents (still.data() + from, (int) still.size() - from,
                                               sr, 300.0, 600.0, 0.06);

    report ("depth actually bends pitch", spreadOn > 15.0 && spreadOff < 3.0,
            juce::String (spreadOn, 1) + " cents moving vs " + juce::String (spreadOff, 1) + " still");

    // A low note through full tri-chorus: "keep lows" should hand it back
    // essentially dry, where a full-range chorus would smear it.
    std::vector<float> low;
    makeSine (low, sr, 60.0, 2.0, 0.3f);

    params.set (pChoDepth, 0.8f);
    params.set (pChoVoices, 3.0f);

    params.set (pChoLows, 250.0f);
    const auto kept = renderChain (params, low, sr, block);

    params.set (pChoLows, 20.0f);
    const auto smeared = renderChain (params, low, sr, block);

    double keptDiff = 0.0, smearedDiff = 0.0;

    for (int i = from; i < (int) low.size(); ++i)
    {
        const double a = (double) kept[(size_t) i] - low[(size_t) i];
        const double b = (double) smeared[(size_t) i] - low[(size_t) i];
        keptDiff += a * a;
        smearedDiff += b * b;
    }

    report ("keep-lows leaves a low E's foundation dry", keptDiff < smearedDiff * 0.35,
            "residual " + juce::String (std::sqrt (keptDiff / juce::jmax (1.0, smearedDiff)), 3)
                + " of full-range chorus");
}

static void testAnalogDelay (double sr, int block)
{
    std::printf ("\nAnalog delay character  (%.0f Hz, %d-sample blocks)\n", sr, block);

    std::vector<float> sine;
    makeSine (sine, sr, 300.0, 3.0, 0.3f);

    ParamStore params;
    params.set (pDelayOn, 1.0f);
    params.set (pDelayMix, 1.0f);
    params.set (pDelayFeedback, 0.0f);
    params.set (pDelayTime, 150.0f);
    params.set (pDelayModRate, 2.0f);
    params.set (pDelayModDepth, 1.0f);

    const auto warble = renderChain (params, sine, sr, block);
    const int from = (int) (sr * 1.0);
    const double spreadOn = pitchSpreadCents (warble.data() + from, (int) warble.size() - from,
                                              sr, 200.0, 450.0, 0.06);

    params.set (pDelayModDepth, 0.0f);
    const auto steady = renderChain (params, sine, sr, block);
    const double spreadOff = pitchSpreadCents (steady.data() + from, (int) steady.size() - from,
                                               sr, 200.0, 450.0, 0.06);

    report ("mod puts tape warble on the repeats", spreadOn > 10.0 && spreadOff < 3.0,
            juce::String (spreadOn, 1) + " cents moving vs " + juce::String (spreadOff, 1) + " still");

    // Hot regeneration with the compander engaged: the loop must squash and
    // ring, not run away or go non-finite.
    std::vector<float> burst;
    makeNote (burst, sr, 98.0, 1.0, 0.9f);
    burst.resize ((size_t) (sr * 6.0), 0.0f);

    params.set (pDelayModDepth, 0.4f);
    params.set (pDelayFeedback, 0.95f);
    params.set (pDelaySat, 1.0f);
    params.set (pDelayHighcut, 16000.0f);

    const auto regen = renderChain (params, burst, sr, block);

    float peak = 0.0f;
    bool finite = true;

    for (size_t i = (size_t) (sr * 2.0); i < regen.size(); ++i)
    {
        if (! std::isfinite (regen[i]))
            finite = false;

        peak = juce::jmax (peak, std::abs (regen[i]));
    }

    report ("hot regen folds over instead of running away", finite && peak < 2.0f,
            "tail peak " + juce::String (peak, 3));

    // Smear diffuses the regeneration: later repeats lose their sharp edge
    // but keep their energy, so the tail thickens instead of thinning.
    {
        const auto repeats = [&] (float smear)
        {
            std::vector<float> in ((size_t) (sr * 1.2), 0.0f);
            in[(size_t) (sr * 0.01)] = 0.9f;

            ParamStore p2;
            p2.set (pDelayOn, 1.0f);
            p2.set (pDelayMix, 1.0f);
            p2.set (pDelayTime, 150.0f);
            p2.set (pDelayFeedback, 0.65f);
            p2.set (pDelayHighcut, 16000.0f);
            p2.set (pDelayModDepth, 0.0f);
            p2.set (pDelaySmear, smear);

            return renderChain (p2, in, sr, block);
        };

        const auto sharp = repeats (0.0f);
        const auto soft = repeats (1.0f);

        // The third repeat has passed through the loop twice.
        const int from3 = (int) (sr * 0.44);
        const int len3 = (int) (sr * 0.10);

        float peakSharp = 0.0f, peakSoft = 0.0f;

        for (int i = from3; i < from3 + len3; ++i)
        {
            peakSharp = juce::jmax (peakSharp, std::abs (sharp[(size_t) i]));
            peakSoft = juce::jmax (peakSoft, std::abs (soft[(size_t) i]));
        }

        const double eSharp = rmsOf (sharp.data() + from3, len3);
        const double eSoft = rmsOf (soft.data() + from3, len3);

        report ("smear softens repeats without losing them",
                peakSoft < peakSharp * 0.75f && eSoft > eSharp * 0.4,
                "3rd repeat peak " + juce::String (peakSharp, 3) + " -> " + juce::String (peakSoft, 3)
                    + ", rms " + juce::String (eSharp, 4) + " -> " + juce::String (eSoft, 4));
    }
}

//==============================================================================
static void testPhaser (double sr, int block)
{
    std::printf ("\nPhaser  (%.0f Hz, %d-sample blocks)\n", sr, block);

    std::vector<float> noiseIn ((size_t) (sr * 2.0), 0.0f);
    uint32_t seed = 1234567u;

    for (auto& v : noiseIn)
    {
        seed = seed * 1664525u + 1013904223u;
        v = 0.2f * (float) (int32_t) seed * 4.6566129e-10f;
    }

    const auto render = [&] (float stages)
    {
        ParamStore params;
        params.set (pPhaserOn, 1.0f);
        params.set (pPhaserRate, 0.5f);
        params.set (pPhaserDepth, 0.8f);
        params.set (pPhaserFeedback, 0.5f);
        params.set (pPhaserStages, stages);
        return renderChain (params, noiseIn, sr, block);
    };

    const auto four = render (4.0f);
    const auto eight = render (8.0f);

    double diff = 0.0, ref = 0.0;
    bool finite = true;

    for (size_t i = (size_t) (sr * 0.2); i < four.size(); ++i)
    {
        diff += std::abs ((double) four[i] - eight[i]);
        ref += std::abs ((double) four[i]);

        if (! std::isfinite (four[i]) || ! std::isfinite (eight[i]))
            finite = false;
    }

    report ("eight stages carve a different sweep than four",
            finite && diff > ref * 0.1,
            "divergence " + juce::String (diff / (ref + 1.0e-9), 3));

    // Full feedback and depth at every stage count stays bounded.
    for (float stages : { 4.0f, 6.0f, 8.0f })
    {
        ParamStore params;
        params.set (pPhaserOn, 1.0f);
        params.set (pPhaserRate, 4.0f);
        params.set (pPhaserDepth, 1.0f);
        params.set (pPhaserFeedback, 0.95f);
        params.set (pPhaserStages, stages);

        const auto out = renderChain (params, noiseIn, sr, block);

        float peak = 0.0f;
        bool ok = true;

        for (float v : out)
        {
            if (! std::isfinite (v))
                ok = false;

            peak = juce::jmax (peak, std::abs (v));
        }

        report ((juce::String ((int) stages) + " stages stable at full resonance").toRawUTF8(),
                ok && peak < 4.0f, "peak " + juce::String (peak, 3));
    }
}

//==============================================================================
static void testOctaveStack (double sr, int block)
{
    std::printf ("\nOctave second voice  (%.0f Hz, %d-sample blocks)\n", sr, block);

    std::vector<float> note;
    makeNote (note, sr, 98.0, 2.0, 0.4f);

    ParamStore params;
    params.set (pOctOn, 1.0f);
    params.set (pOctMix, 1.0f);
    params.set (pOctInterval, 12.0f);
    params.set (pOct2Interval, 7.0f);

    const int from = (int) (sr * 0.7);
    const int len = (int) (sr * 0.8);
    const double fifth = 98.0 * std::pow (2.0, 7.0 / 12.0);   // ~146.8 Hz

    params.set (pOct2Level, 0.0f);
    const auto solo = renderChain (params, note, sr, block);
    const double fifthOff = goertzel (solo.data() + from, len, sr, fifth)
                              / (goertzel (solo.data() + from, len, sr, 196.0) + 1.0e-9);

    params.set (pOct2Level, 0.8f);
    const auto stacked = renderChain (params, note, sr, block);
    const double octaveShare = goertzel (stacked.data() + from, len, sr, 196.0);
    const double fifthOn = goertzel (stacked.data() + from, len, sr, fifth)
                             / (octaveShare + 1.0e-9);

    report ("second voice adds the fifth on top", fifthOn > fifthOff * 4.0 && fifthOn > 0.15,
            "fifth/octave " + juce::String (fifthOff, 3) + " -> " + juce::String (fifthOn, 3));

    report ("main voice survives the stack", octaveShare > 0.005,
            "octave partial " + juce::String (octaveShare, 4));
}

//==============================================================================
// The tracker under stage conditions the steady-tone tests never create.
static void testTrackerHard (double sr, int block)
{
    std::printf ("\nPitch tracker, hostile material  (%.0f Hz, %d-sample blocks)\n", sr, block);

    // A fast run: six notes at 140 ms each. The tracker must be on the right
    // pitch by the back half of every note.
    {
        const double run[] { 82.41, 110.0, 146.83, 98.0, 123.47, 82.41 };
        const double noteSeconds = 0.14;
        const int noteLen = (int) (sr * noteSeconds);

        std::vector<float> signal;

        for (double f0 : run)
        {
            std::vector<float> note;
            makeNote (note, sr, f0, noteSeconds, 0.4f);
            signal.insert (signal.end(), note.begin(), note.end());
        }

        PitchTracker tracker;
        tracker.prepare (sr);

        int good = 0;
        juce::String detail;

        for (size_t n = 0; n < std::size (run); ++n)
        {
            // Feed up to the end of this note, then look at the report.
            const int upTo = (int) (n + 1) * noteLen;
            int fed = (int) n * noteLen;

            // (state carries across notes — only feed the new samples)
            for (; fed + block <= upTo; fed += block)
                tracker.process (signal.data() + fed, block);

            const double detected = sr / tracker.getPeriodSamples();
            const double err = cents (detected, run[n]);

            if (std::abs (err) < 30.0)
                ++good;

            detail << juce::String (err, 0) << " ";
        }

        report ("fast 140 ms run lands on every note", good >= 5,
                juce::String (good) + "/6 within 30 cents, errors (cents): " + detail);
    }

    // A bridge-pickup tone: the fundamental buried 16 dB under the second
    // harmonic. Autocorrelation on the raw wave would answer the octave.
    {
        const double f0 = 110.0;
        const int n = (int) (sr * 0.8);
        std::vector<float> thin ((size_t) n, 0.0f);

        static constexpr double amps[] { 0.15, 1.0, 0.7, 0.45, 0.3, 0.2 };
        double phase[6] {};

        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            const double env = std::exp (-t * 0.9) * (1.0 - std::exp (-t * 200.0));
            double s = 0.0;

            for (int h = 0; h < 6; ++h)
            {
                s += amps[h] * std::sin (phase[h]);
                phase[h] += 6.283185307179586 * f0 * (h + 1) / sr;
            }

            thin[(size_t) i] = (float) (s * env * 0.18);
        }

        PitchTracker tracker;
        tracker.prepare (sr);

        for (int i = 0; i + block <= n; i += block)
            tracker.process (thin.data() + i, block);

        const double detected = sr / tracker.getPeriodSamples();
        const double err = cents (detected, f0);

        report ("buried fundamental still reads as the root", std::abs (err) < 25.0,
                juce::String (detected, 2) + " Hz vs 110, " + juce::String (err, 1) + " cents");
    }
}

//==============================================================================
static void testAmpStage (double sr, int block)
{
    std::printf ("\nAmp stage  (%.0f Hz, %d-sample blocks)\n", sr, block);

    // A one-tap Linear capture with weight 0.5 — small enough to write here,
    // real enough to exercise the whole parse, prime, swap and process path.
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("liverig-test-capture.nam");
    file.replaceWithText (
        R"({"version": "0.5.0", "architecture": "Linear",)"
        R"( "config": {"receptive_field": 1, "bias": false}, "weights": [0.5]})");

    std::vector<float> sine;
    makeSine (sine, sr, 220.0, 0.5, 0.4f);

    AmpModel ampModel;
    ampModel.prepare (sr, block);

    std::vector<float> out (sine.size());

    const auto run = [&]
    {
        std::vector<float> chunk ((size_t) block);

        for (int i = 0; i + block <= (int) sine.size(); i += block)
        {
            std::copy (sine.begin() + i, sine.begin() + i + block, chunk.begin());
            ampModel.process (chunk.data(), block);
            std::copy (chunk.begin(), chunk.end(), out.begin() + i);
        }
    };

    run();

    double worstWire = 0.0;

    for (size_t i = 0; i < out.size(); ++i)
        worstWire = juce::jmax (worstWire, (double) std::abs (out[i] - sine[i]));

    report ("no capture loaded is a wire", worstWire < 1.0e-9,
            "worst deviation " + juce::String (worstWire, 9));

    juce::String error;
    const bool loaded = ampModel.load (file, error);

    report ("linear test capture loads", loaded, loaded ? "ok" : error);

    if (loaded)
    {
        run();   // first block swaps the model in

        double worstHalf = 0.0;
        const int settle = block * 2;

        for (size_t i = (size_t) settle; i < out.size(); ++i)
            worstHalf = juce::jmax (worstHalf, (double) std::abs (out[i] - 0.5f * sine[i]));

        report ("capture actually processes the signal", worstHalf < 1.0e-4,
                "worst deviation from x0.5: " + juce::String (worstHalf, 6));

        ampModel.clearModel();
        run();
        ampModel.collectGarbage();

        double worstCleared = 0.0;

        for (size_t i = (size_t) settle; i < out.size(); ++i)
            worstCleared = juce::jmax (worstCleared, (double) std::abs (out[i] - sine[i]));

        report ("cleared capture is a wire again", worstCleared < 1.0e-9,
                "worst deviation " + juce::String (worstCleared, 9));
    }

    // Through the chain: the stage engaged with nothing loaded changes nothing.
    {
        std::vector<float> note;
        makeNote (note, sr, 110.0, 1.0, 0.4f);

        ParamStore params;
        const auto off = renderChain (params, note, sr, block);

        params.set (pAmpOn, 1.0f);
        const auto on = renderChain (params, note, sr, block);

        double worst = 0.0;

        for (size_t i = 0; i < off.size(); ++i)
            worst = juce::jmax (worst, (double) std::abs (on[i] - off[i]));

        report ("empty amp stage in the chain is a wire", worst < 1.0e-6,
                "worst deviation " + juce::String (worst, 8));
    }

    file.deleteFile();
}

//==============================================================================
// The capture library and the preset store's memory of it — the part that
// makes a downloaded capture a permanent, recallable amp.
static void testCaptureLibrary()
{
    std::printf ("\nCapture library and preset persistence\n");

    auto scratch = juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("liverig-capture-lib-test");
    scratch.deleteRecursively();
    scratch.createDirectory();

    const auto libDir = scratch.getChildFile ("captures");
    const auto presetFile = scratch.getChildFile ("presets.json");

    // Import copies the file in and finds it again by name.
    {
        const auto source = scratch.getChildFile ("Big Iron.nam");
        source.replaceWithText ("{}");

        juce::String error;
        const auto imported = AmpModel::importToLibrary (source, error, libDir);

        report ("import copies into the library",
                imported.existsAsFile() && imported.getParentDirectory() == libDir, error);

        const auto listed = AmpModel::listLibrary (libDir);

        report ("library lists what it holds",
                listed.size() == 1 && listed[0].getFileNameWithoutExtension() == "Big Iron",
                juce::String (listed.size()) + " file(s)");

        report ("captures resolve by name",
                AmpModel::libraryFileForName ("Big Iron", libDir) == imported, "");
    }

    // A chain preset remembers its capture across a full save/load cycle;
    // presets from before captures existed stay void.
    {
        ParamStore params;

        PresetStore first;
        first.fileOverride = presetFile;
        first.load();   // seeds factory content into the scratch file

        first.store ("With Amp", params, "comp,eq", juce::var (juce::String ("Big Iron")));
        first.store ("Saved Silent", params, "comp,eq", juce::var (juce::String()));

        PresetStore second;
        second.fileOverride = presetFile;
        second.load();

        report ("chain preset carries its capture name",
                second.getAmpCapture ("With Amp").toString() == "Big Iron",
                second.getAmpCapture ("With Amp").toString());

        report ("saved-silent preset carries the empty string",
                second.getAmpCapture ("Saved Silent").isString()
                    && second.getAmpCapture ("Saved Silent").toString().isEmpty(), "");

        report ("factory presets stay capture-agnostic",
                second.getAmpCapture ("Clean DI").isVoid(), "");

        // Amp effect presets do the same through their own path.
        std::vector<int> ids { pAmpIn, pAmpOut };
        second.storeEffect ("amp", "Iron Rhythm", params, ids, juce::var (juce::String ("Big Iron")));

        PresetStore third;
        third.fileOverride = presetFile;
        third.load();

        juce::var capture;
        const bool applied = third.applyEffect ("amp", "Iron Rhythm", params, &capture);

        report ("amp panel preset recalls its capture",
                applied && capture.toString() == "Big Iron", capture.toString());
    }

    scratch.deleteRecursively();
}

//==============================================================================
static void testTape (double sr, int block)
{
    std::printf ("\nTape saturation  (%.0f Hz, %d-sample blocks)\n", sr, block);

    const int from = (int) (sr * 0.4);
    const int len = (int) (sr * 0.5);

    // Quiet playing passes at unity, whatever the drive is set to.
    std::vector<float> quiet;
    makeSine (quiet, sr, 110.0, 1.0, 0.05f);

    ParamStore params;
    params.set (pTapeOn, 1.0f);
    params.set (pTapeDrive, 15.0f);
    params.set (pTapeSoften, 0.0f);
    params.set (pTapeBump, 0.0f);

    const auto small = renderChain (params, quiet, sr, block);
    const double smallGainDb = 20.0 * std::log10 (rmsOf (small.data() + from, len)
                                                    / rmsOf (quiet.data() + from, len));

    report ("quiet signal stays at unity", std::abs (smallGainDb) < 1.0,
            juce::String (smallGainDb, 2) + " dB at 15 dB drive");

    // Hot playing picks up low-order harmonics and loses peak height.
    std::vector<float> hot;
    makeSine (hot, sr, 110.0, 1.0, 0.5f);

    const auto warmed = renderChain (params, hot, sr, block);

    const double second = goertzel (warmed.data() + from, len, sr, 220.0)
                            / (goertzel (warmed.data() + from, len, sr, 110.0) + 1.0e-9);

    float peakIn = 0.0f, peakOut = 0.0f;

    for (int i = from; i < from + len; ++i)
    {
        peakIn = juce::jmax (peakIn, std::abs (hot[(size_t) i]));
        peakOut = juce::jmax (peakOut, std::abs (warmed[(size_t) i]));
    }

    report ("digging in adds second harmonic and rounds peaks",
            second > 0.01 && peakOut < peakIn,
            "2nd/1st " + juce::String (second, 4) + ", peak " + juce::String (peakIn, 2)
                + " -> " + juce::String (peakOut, 2));

    // Soften takes the top end down: a 4 kHz tone comes through smaller.
    std::vector<float> top;
    makeSine (top, sr, 4000.0, 1.0, 0.3f);

    params.set (pTapeDrive, 12.0f);
    const auto bright = renderChain (params, top, sr, block);

    params.set (pTapeSoften, 0.8f);
    const auto soft = renderChain (params, top, sr, block);

    const double softRatio = rmsOf (soft.data() + from, len) / rmsOf (bright.data() + from, len);

    report ("soften pulls the harsh top down", softRatio < 0.9,
            juce::String (20.0 * std::log10 (softRatio), 1) + " dB at 4 kHz");

    // Bump adds weight right where a bass fundamental lives.
    std::vector<float> low;
    makeSine (low, sr, 60.0, 1.0, 0.15f);

    params.set (pTapeSoften, 0.0f);
    params.set (pTapeDrive, 3.0f);
    const auto flat = renderChain (params, low, sr, block);

    params.set (pTapeBump, 1.0f);
    const auto bumped = renderChain (params, low, sr, block);

    const double bumpRatio = rmsOf (bumped.data() + from, len) / rmsOf (flat.data() + from, len);

    report ("bump lifts the fundamental", bumpRatio > 1.3,
            "+" + juce::String (20.0 * std::log10 (bumpRatio), 1) + " dB at 60 Hz");
}

//==============================================================================
// Renders a mono buffer through a fresh plate and returns both wet channels.
static void renderPlate (PlateVerb& verb, const std::vector<float>& in, int block,
                         std::vector<float>& left, std::vector<float>& right)
{
    left.clear();
    right.clear();
    left.reserve (in.size());
    right.reserve (in.size());

    std::vector<float> mono ((size_t) block), l ((size_t) block), r ((size_t) block);

    for (int i = 0; i + block <= (int) in.size(); i += block)
    {
        std::copy (in.begin() + i, in.begin() + i + block, mono.begin());
        verb.process (mono.data(), l.data(), r.data(), block);

        for (int k = 0; k < block; ++k)
        {
            left.push_back (l[(size_t) k]);
            right.push_back (r[(size_t) k]);
        }
    }
}

// RT60 from the slope of the impulse tail between -5 and -35 dB, so a finite
// render can report an eight-second tail without containing all of it.
static double measurePlateRT60 (float size, float damp, double sr, int block, double renderSeconds)
{
    PlateVerb verb;
    verb.prepare (sr, block);
    verb.setSize (size);
    verb.setDamping (damp);
    verb.setPredelayMs (0.0f);
    verb.setModDepth (0.0f);

    std::vector<float> in ((size_t) (sr * renderSeconds), 0.0f);
    in[0] = 1.0f;

    std::vector<float> l, r;
    renderPlate (verb, in, block, l, r);

    const int win = (int) (sr * 0.05);
    const int windows = (int) l.size() / win;

    std::vector<double> levelDb ((size_t) windows, -200.0);
    double peakDb = -200.0;
    int peakAt = 0;

    for (int w = 0; w < windows; ++w)
    {
        double e = 0.0;

        for (int i = w * win; i < (w + 1) * win; ++i)
            e += (double) l[(size_t) i] * l[(size_t) i] + (double) r[(size_t) i] * r[(size_t) i];

        levelDb[(size_t) w] = 10.0 * std::log10 (e / (2.0 * win) + 1.0e-30);

        if (levelDb[(size_t) w] > peakDb)
        {
            peakDb = levelDb[(size_t) w];
            peakAt = w;
        }
    }

    double t5 = -1.0, t35 = -1.0;

    for (int w = peakAt; w < windows; ++w)
    {
        const double t = (w + 0.5) * win / sr;

        if (t5 < 0.0 && levelDb[(size_t) w] <= peakDb - 5.0)
            t5 = t;

        if (levelDb[(size_t) w] <= peakDb - 35.0)
        {
            t35 = t;
            break;
        }
    }

    if (t5 < 0.0 || t35 < 0.0)
        return 1.0e9;   // render too short to see the slope

    return (t35 - t5) * 2.0;
}

static void testReverb (double sr, int block)
{
    std::printf ("\nPlate reverb  (%.0f Hz, %d-sample blocks)\n", sr, block);

    // Size commands the tail: knob positions land near their RT60 and stay
    // in order. The mapping targets 0.5 s at closed, 2 s mid, 8 s open.
    const double rtSmall = measurePlateRT60 (0.0f, 0.0f, sr, block, 2.0);
    const double rtMid   = measurePlateRT60 (0.5f, 0.0f, sr, block, 4.0);
    const double rtBig   = measurePlateRT60 (1.0f, 0.0f, sr, block, 7.0);

    report ("small size decays fast", rtSmall > 0.2 && rtSmall < 1.1,
            juce::String (rtSmall, 2) + " s for 0.5 s commanded");
    report ("mid size lands near two seconds", rtMid > 1.2 && rtMid < 3.2,
            juce::String (rtMid, 2) + " s for 2.0 s commanded");
    report ("full size opens into a long tail", rtBig > 5.0 && rtBig < 13.0,
            juce::String (rtBig, 2) + " s for 8.0 s commanded");
    report ("tail length rises with the knob", rtSmall < rtMid && rtMid < rtBig,
            juce::String (rtSmall, 2) + " / " + juce::String (rtMid, 2)
                + " / " + juce::String (rtBig, 2) + " s");

    const double rtDamped = measurePlateRT60 (0.5f, 0.9f, sr, block, 4.0);

    report ("damping shortens the tail", rtDamped < rtMid * 0.85,
            juce::String (rtMid, 2) + " s open -> " + juce::String (rtDamped, 2) + " s damped");

    // Damping is frequency-dependent decay: the broadband tail only tightens
    // a little because the lows ride it out, but the top end must collapse.
    {
        const auto highTail = [&] (float damp)
        {
            PlateVerb verb;
            verb.prepare (sr, block);
            verb.setSize (0.5f);
            verb.setDamping (damp);
            verb.setPredelayMs (0.0f);
            verb.setModDepth (0.0f);

            std::vector<float> in ((size_t) (sr * 1.4), 0.0f);
            in[0] = 1.0f;

            std::vector<float> l, r;
            renderPlate (verb, in, block, l, r);

            const int from = (int) (sr * 0.6);
            return goertzel (l.data() + from, (int) (sr * 0.6), sr, 4000.0);
        };

        const double open = highTail (0.0f);
        const double dark = highTail (0.9f);

        report ("damping darkens the tail", open > 1.0e-7 && dark < open * 0.25,
                "4 kHz tail " + juce::String (open, 7) + " open -> "
                    + juce::String (dark, 7) + " damped");
    }

    // The two outputs must be genuinely decorrelated — taps scattered across
    // both tank halves — not a delayed copy of one another.
    {
        PlateVerb verb;
        verb.prepare (sr, block);
        verb.setSize (0.5f);
        verb.setDamping (0.3f);
        verb.setPredelayMs (0.0f);
        verb.setModDepth (0.5f);

        std::vector<float> in ((size_t) (sr * 1.5), 0.0f);
        in[0] = 1.0f;

        std::vector<float> l, r;
        renderPlate (verb, in, block, l, r);

        const int from = (int) (sr * 0.15);
        const int len = (int) (sr * 1.0);
        double lr = 0.0, ll = 0.0, rr = 0.0;
        float peak = 0.0f;
        bool finite = true;

        for (int i = from; i < from + len; ++i)
        {
            lr += (double) l[(size_t) i] * r[(size_t) i];
            ll += (double) l[(size_t) i] * l[(size_t) i];
            rr += (double) r[(size_t) i] * r[(size_t) i];

            if (! std::isfinite (l[(size_t) i]) || ! std::isfinite (r[(size_t) i]))
                finite = false;

            peak = juce::jmax (peak, juce::jmax (std::abs (l[(size_t) i]), std::abs (r[(size_t) i])));
        }

        const double corr = lr / (std::sqrt (ll * rr) + 1.0e-30);
        const double balance = std::sqrt (ll / (rr + 1.0e-30));

        report ("stereo tails are decorrelated", finite && std::abs (corr) < 0.6,
                "correlation " + juce::String (corr, 3));
        report ("both ears carry the same weight", balance > 0.5 && balance < 2.0,
                "L/R rms ratio " + juce::String (balance, 2));

        float wetPeak = 0.0f;

        for (float v : l)
            wetPeak = juce::jmax (wetPeak, std::abs (v));

        report ("unit impulse stays in sane wet territory", wetPeak > 0.05f && wetPeak < 1.5f,
                "wet peak " + juce::String (wetPeak, 3));
    }

    // Modulation is the anti-metal ingredient. A static tank is time-invariant
    // — strike it whenever, the ring is the same. The chorused tank must not
    // be: the same impulse landing at a different point in the LFO's travel
    // has to come back audibly different.
    {
        const auto restrike = [&] (float mod)
        {
            const int offset = 600 * block;
            const int tail = (int) (sr * 2.0);

            const auto strike = [&] (int at, int total)
            {
                PlateVerb verb;
                verb.prepare (sr, block);
                verb.setSize (0.6f);
                verb.setDamping (0.3f);
                verb.setPredelayMs (0.0f);
                verb.setModDepth (mod);

                std::vector<float> in ((size_t) total, 0.0f);
                in[(size_t) at] = 1.0f;

                std::vector<float> l, r;
                renderPlate (verb, in, block, l, r);
                return l;
            };

            const auto first = strike (0, tail);
            const auto later = strike (offset, offset + tail);

            double diff = 0.0, ref = 0.0;

            for (int i = 0; i < tail; ++i)
            {
                diff += std::abs ((double) later[(size_t) (offset + i)] - first[(size_t) i]);
                ref  += std::abs ((double) first[(size_t) i]);
            }

            return diff / (ref + 1.0e-12);
        };

        const double still = restrike (0.0f);
        const double moving = restrike (1.0f);

        report ("mod makes the tank time-varying", still < 1.0e-3 && moving > 0.2,
                "re-strike divergence " + juce::String (still, 6) + " still -> "
                    + juce::String (moving, 3) + " modulated");
    }

    // Predelay moves the whole wet picture later by the number on the knob.
    {
        const auto arrival = [&] (float preMs)
        {
            PlateVerb verb;
            verb.prepare (sr, block);
            verb.setSize (0.5f);
            verb.setDamping (0.3f);
            verb.setPredelayMs (preMs);
            verb.setModDepth (0.0f);

            std::vector<float> in ((size_t) (sr * 0.6), 0.0f);
            in[0] = 1.0f;

            std::vector<float> l, r;
            renderPlate (verb, in, block, l, r);

            for (int i = 0; i < (int) l.size(); ++i)
                if (std::abs (l[(size_t) i]) > 1.0e-4f || std::abs (r[(size_t) i]) > 1.0e-4f)
                    return i / sr * 1000.0;

            return -1.0;
        };

        const double at0 = arrival (0.0f);
        const double at60 = arrival (60.0f);

        report ("predelay shifts the wet by the knob", at0 >= 0.0 && std::abs ((at60 - at0) - 60.0) < 5.0,
                juce::String (at0, 1) + " ms -> " + juce::String (at60, 1) + " ms for 60 ms set");
    }

    // Silence in, silence out: the recirculating tank must starve cleanly,
    // with no denormal grind and no idle tone.
    {
        PlateVerb verb;
        verb.prepare (sr, block);
        verb.setSize (1.0f);
        verb.setDamping (0.0f);
        verb.setPredelayMs (20.0f);
        verb.setModDepth (1.0f);

        std::vector<float> in ((size_t) (sr * 0.5), 0.0f);
        in[0] = 1.0f;

        std::vector<float> l, r;
        renderPlate (verb, in, block, l, r);   // excite, then...

        std::vector<float> quiet ((size_t) (sr * 12.0), 0.0f);
        renderPlate (verb, quiet, block, l, r);   // ...starve for 12 s

        const int from = (int) l.size() - (int) sr;
        const double residue = rmsOf (l.data() + from, (int) sr);

        report ("starved tank goes properly silent", residue < 1.0e-5,
                "rms " + juce::String (residue, 8) + " after 12 s of silence");
    }

    // Through the chain: the mix law holds — mix 0 is bit-exact dry, and the
    // wet a note leaves behind actually rings past the note.
    {
        std::vector<float> note;
        makeNote (note, sr, 196.0, 0.5, 0.4f);
        note.resize ((size_t) (sr * 3.0), 0.0f);

        ParamStore params;
        const auto dry = renderChain (params, note, sr, block);   // reverb off

        params.set (pRevOn, 1.0f);
        params.set (pRevMix, 0.0f);

        const auto zeroMix = renderChain (params, note, sr, block);

        double worst = 0.0;

        for (int i = 0; i < (int) dry.size(); ++i)
            worst = juce::jmax (worst, (double) std::abs (zeroMix[(size_t) i] - dry[(size_t) i]));

        report ("mix at zero matches reverb off exactly", worst < 1.0e-6,
                "worst deviation " + juce::String (worst, 8));

        params.set (pRevMix, 0.35f);
        params.set (pRevSize, 0.7f);

        const auto wet = renderChain (params, note, sr, block);

        const int from = (int) (sr * 1.0);
        const int len = (int) (sr * 1.5);
        const double tailWet = rmsOf (wet.data() + from, len);
        const double tailDry = rmsOf (dry.data() + from, len);

        bool finite = true;
        float peak = 0.0f;

        for (float v : wet)
        {
            if (! std::isfinite (v))
                finite = false;

            peak = juce::jmax (peak, std::abs (v));
        }

        report ("the tail rings past the note", finite && peak < 4.0f && tailWet > tailDry * 5.0 + 1.0e-6,
                "tail rms " + juce::String (tailDry, 6) + " dry -> " + juce::String (tailWet, 6) + " wet");
    }
}

//==============================================================================
static void testMasterLimiter (double sr)
{
    std::printf ("\nMaster limiter  (%.0f Hz)\n", sr);

    MasterLimiter limiter;
    limiter.prepare (sr);

    // Quiet material passes untouched.
    double worstError = 0.0;

    for (int i = 0; i < (int) sr; ++i)
    {
        const float x = 0.3f * (float) std::sin (6.283185307179586 * 220.0 * i / sr);
        float l = x, r = x;
        limiter.process (l, r);
        worstError = juce::jmax (worstError, (double) std::abs (l - x));
    }

    report ("under the ceiling it is a wire", worstError < 1.0e-6,
            "worst deviation " + juce::String (worstError, 8));

    // A sum 6 dB over full scale comes out under the ceiling, not squashed to
    // nothing, and finite.
    float peak = 0.0f, floor = 1.0f;
    bool finite = true;

    for (int i = 0; i < (int) sr; ++i)
    {
        const float x = 2.0f * (float) std::sin (6.283185307179586 * 110.0 * i / sr);
        float l = x, r = x;
        limiter.process (l, r);

        if (! std::isfinite (l))
            finite = false;

        if (i > (int) (sr * 0.1))
        {
            peak = juce::jmax (peak, std::abs (l));
            floor = juce::jmin (floor, limiter.lastGain);
        }
    }

    report ("+6 dB over stays under the ceiling",
            finite && peak <= MasterLimiter::ceiling + 0.001f && peak > 0.8f,
            "peak " + juce::String (peak, 3) + ", deepest gain " + juce::String (floor, 3));

    // After the loud passage, quiet material recovers to unity within a second.
    double afterError = 0.0;

    for (int i = 0; i < (int) sr; ++i)
    {
        const float x = 0.3f * (float) std::sin (6.283185307179586 * 220.0 * i / sr);
        float l = x, r = x;
        limiter.process (l, r);

        if (i > (int) (sr * 0.9))
            afterError = juce::jmax (afterError, (double) std::abs (l - x));
    }

    report ("releases back to a wire", afterError < 1.0e-3,
            "worst deviation " + juce::String (afterError, 6) + " after 0.9 s");
}

//==============================================================================
// Amplitude of one tone inside a mix, via quadrature projection over a window.
static double toneLevel (const std::vector<float>& x, int i0, int i1, double sr, double hz)
{
    double c = 0.0, s = 0.0;

    for (int i = i0; i < i1; ++i)
    {
        const double ph = 6.283185307179586 * hz * i / sr;
        c += x[(size_t) i] * std::cos (ph);
        s += x[(size_t) i] * std::sin (ph);
    }

    const double half = (double) (i1 - i0) * 0.5;
    return std::sqrt (c * c + s * s) / half;
}

// The bug this stage exists for: a slammed live lane (steady, just under the
// ceiling) summed with a mastered song used to pin the master limiter 4-6 dB
// into reduction, and the instrument went down with it. The ducker must hold
// the instrument's level, keep the song audible underneath, let the song run
// untouched when nobody plays, and never break the master ceiling.
static void testStemDucker (double sr)
{
    std::printf ("\nStem ducker  (%.0f Hz)\n", sr);

    const double instHz = 196.0, songHz = 110.0;
    const float instAmp = 0.78f;

    // instSeconds: how long the instrument plays from t=0; the song runs the
    // whole render. Returns the left channel.
    auto render = [&] (double seconds, bool withSong, double instSeconds, std::vector<float>& out)
    {
        StemDucker duck;
        MasterLimiter lim;
        duck.prepare (sr);
        lim.prepare (sr);

        const int n = (int) (sr * seconds);
        out.assign ((size_t) n, 0.0f);

        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            const float inst = t < instSeconds
                                 ? instAmp * (float) std::sin (6.283185307179586 * instHz * t)
                                 : 0.0f;

            float sl = 0.0f, sr2 = 0.0f;

            if (withSong)
            {
                sl = 0.9f * (float) std::sin (6.283185307179586 * songHz * t)
                   + 0.35f * (float) std::sin (6.283185307179586 * songHz * 3.01 * t);
                sr2 = sl;
            }

            duck.process (sl, sr2, inst, inst);

            float l = sl + inst;
            float r = sr2 + inst;
            lim.process (l, r);
            out[(size_t) i] = l;
        }
    };

    std::vector<float> instSolo, songSolo, mixed, quiet;
    render (2.0, false, 2.0, instSolo);
    render (2.0, true, 0.0, songSolo);
    render (3.0, true, 1.8, mixed);

    const double instA = toneLevel (instSolo, (int) sr, (int) (2.0 * sr), sr, instHz);
    const double songB = toneLevel (songSolo, (int) sr, (int) (2.0 * sr), sr, songHz);
    const double instC = toneLevel (mixed, (int) (0.8 * sr), (int) (1.8 * sr), sr, instHz);
    const double songC = toneLevel (mixed, (int) (0.8 * sr), (int) (1.8 * sr), sr, songHz);
    const double songAfter = toneLevel (mixed, (int) (2.6 * sr), (int) (3.0 * sr), sr, songHz);

    report ("instrument keeps its level under a song", instC >= 0.85 * instA,
            "solo " + juce::String (instA, 3) + " -> in mix " + juce::String (instC, 3)
              + " (" + juce::String (100.0 * instC / instA, 0) + "%)");

    report ("song ducks under the playing, stays audible",
            songC >= 0.10 && songC <= 0.35,
            "song " + juce::String (songB, 3) + " -> " + juce::String (songC, 3)
              + " while playing");

    report ("song alone is not ducked", songB >= 0.55,
            "level " + juce::String (songB, 3) + " with nobody playing");

    report ("song swells back after the phrase", songAfter >= 0.75 * songB,
            juce::String (songAfter, 3) + " of " + juce::String (songB, 3)
              + " within 0.8 s");

    float peak = 0.0f;

    for (float v : mixed)
        peak = juce::jmax (peak, std::abs (v));

    report ("master ceiling still holds", peak <= MasterLimiter::ceiling + 0.001f,
            "mix peak " + juce::String (peak, 3));

    // A quiet song with nobody playing passes through untouched.
    StemDucker duck;
    duck.prepare (sr);
    double worstError = 0.0;

    for (int i = 0; i < (int) (0.5 * sr); ++i)
    {
        const float x = 0.3f * (float) std::sin (6.283185307179586 * songHz * i / sr);
        float sl = x, sr2 = x;
        duck.process (sl, sr2, 0.0f, 0.0f);
        worstError = juce::jmax (worstError, (double) std::abs (sl - x));
    }

    report ("under the bed it is a wire", worstError < 1.0e-6,
            "worst deviation " + juce::String (worstError, 8));
}

//==============================================================================
// End-to-end scenario: the user's actual settings.json and an actual song
// folder through the exact AudioEngine bus (FxChain -> stems -> ducker ->
// master gain -> limiter), measuring the instrument's heard level with the
// transport stopped vs playing. Answers "does THIS rig, at THESE settings,
// duck the instrument when a song runs" with the shipped code.
static void runScenario (const juce::File& settingsFile, const juce::File& songFolder)
{
    const auto parsed = juce::JSON::parse (settingsFile.loadFileAsString());

    std::printf ("\nScenario: %s\n          %s\n",
                 settingsFile.getFullPathName().toRawUTF8(),
                 songFolder.getFileName().toRawUTF8());

    const double sr = 48000.0;
    const int block = 480;
    const double seconds = 30.0;
    const int measureFrom = (int) (5.0 * sr);

    {
        ParamStore check;
        check.fromVar (parsed["params"]);
        std::printf ("  loaded: gains %+.1f %+.1f %+.1f %+.1f dB | mutes %d %d %d %d"
                     " | mkup %+.1f | liveOut %+.1f\n",
                     check.get (pStemGain0), check.get (pStemGain1),
                     check.get (pStemGain2), check.get (pStemGain3),
                     (int) check.getBool (pStemMute0), (int) check.getBool (pStemMute1),
                     (int) check.getBool (pStemMute2), (int) check.getBool (pStemMute3),
                     check.get (pCompMakeup), check.get (pLiveOut));
    }

    for (double inputPeakDb : { -18.0, -6.0 })
    {
        // A bass line: the plucked-note proxy, retriggered every half second.
        std::vector<float> note;
        makeNote (note, sr, 98.0, 0.5, 1.0f);
        float notePeak = 0.0f;

        for (float v : note)
            notePeak = juce::jmax (notePeak, std::abs (v));

        const float noteGain = (float) juce::Decibels::decibelsToGain (inputPeakDb) / notePeak;

        struct Result { double heard = 0.0, duckMinGain = 1.0, limMaxGr = 0.0, stemPeak = 0.0; };

        auto render = [&] (bool songOn, bool duckerOn)
        {
            ParamStore params;
            params.fromVar (parsed["params"]);

            FxChain fx (params);
            StemPlayer stems (params);
            StemDucker duck;
            MasterLimiter lim;

            fx.prepare (sr, block);
            stems.prepare (sr, block);
            duck.prepare (sr);
            lim.prepare (sr);

            juce::SmoothedValue<float> masterGain;
            masterGain.reset (sr, 0.02);
            masterGain.setCurrentAndTargetValue (
                juce::Decibels::decibelsToGain (params.get (pMasterGain), -60.0f));

            if (songOn)
            {
                const auto ok = stems.loadSongFolder (songFolder);

                if (ok.failed())
                {
                    std::printf ("  cannot load song: %s\n", ok.getErrorMessage().toRawUTF8());
                    return Result {};
                }

                stems.play();
            }

            std::vector<float> monoIn (block), liveL (block), liveR (block),
                               mixL (block), mixR (block);
            Result res;
            long heardCount = 0;
            const int total = (int) (sr * seconds);

            for (int base = 0; base < total; base += block)
            {
                for (int i = 0; i < block; ++i)
                    monoIn[(size_t) i] = note[(size_t) ((base + i) % (int) note.size())] * noteGain;

                fx.process (monoIn.data(), liveL.data(), liveR.data(), block);

                std::fill (mixL.begin(), mixL.end(), 0.0f);
                std::fill (mixR.begin(), mixR.end(), 0.0f);
                stems.process (mixL.data(), mixR.data(), block);

                masterGain.setTargetValue (
                    juce::Decibels::decibelsToGain (params.get (pMasterGain), -60.0f));

                for (int i = 0; i < block; ++i)
                {
                    res.stemPeak = juce::jmax (res.stemPeak,
                                               (double) juce::jmax (std::abs (mixL[(size_t) i]),
                                                                    std::abs (mixR[(size_t) i])));

                    if (duckerOn)
                        duck.process (mixL[(size_t) i], mixR[(size_t) i],
                                      liveL[(size_t) i], liveR[(size_t) i]);

                    const float gm = masterGain.getNextValue();
                    float l = (mixL[(size_t) i] + liveL[(size_t) i]) * gm;
                    float r = (mixR[(size_t) i] + liveR[(size_t) i]) * gm;
                    lim.process (l, r);

                    if (base + i >= measureFrom)
                    {
                        res.heard += std::abs (liveL[(size_t) i]) * gm * lim.lastGain;
                        ++heardCount;
                        res.duckMinGain = juce::jmin (res.duckMinGain, (double) duck.gain);
                        res.limMaxGr = juce::jmax (res.limMaxGr, (double) lim.getReductionDb());
                    }
                }
            }

            if (heardCount > 0)
                res.heard /= (double) heardCount;

            return res;
        };

        const auto solo = render (false, true);
        const auto withSongNew = render (true, true);
        const auto withSongOld = render (true, false);

        std::printf ("  input peak %.0f dBFS | stems peak %.2f\n",
                     inputPeakDb, withSongNew.stemPeak);
        std::printf ("    instrument heard: solo %.4f | song playing %.4f (%.0f%%)"
                     " | pre-fix chain %.4f (%.0f%%)\n",
                     solo.heard, withSongNew.heard,
                     solo.heard > 0 ? 100.0 * withSongNew.heard / solo.heard : 0.0,
                     withSongOld.heard,
                     solo.heard > 0 ? 100.0 * withSongOld.heard / solo.heard : 0.0);
        std::printf ("    ducker deepest gain %.2f | master limiter worst %.1f dB"
                     " (pre-fix worst %.1f dB)\n",
                     withSongNew.duckMinGain, withSongNew.limMaxGr, withSongOld.limMaxGr);
    }
}

//==============================================================================
// The five-band parametric through the real chain: each band type must do
// what its name says at its own frequency and leave the rest of the spectrum
// alone. Flat settings must be a wire.
static void testParametricEq (double sr, int block)
{
    std::printf ("\nParametric EQ  (%.0f Hz, %d-sample blocks)\n", sr, block);

    // Renders a sine through FxChain with only the EQ enabled and returns the
    // output level relative to the input, in dB, measured after settling.
    auto responseDb = [&] (std::function<void (ParamStore&)> setup, double hz)
    {
        ParamStore params;
        params.set (pEqOn, 1.0f);
        setup (params);

        FxChain fx (params);
        fx.prepare (sr, block);

        const double seconds = 1.5;
        const int total = (int) (sr * seconds);
        const float amp = 0.1f;

        std::vector<float> mono ((size_t) block), l ((size_t) block), r ((size_t) block);
        std::vector<float> out;
        out.reserve ((size_t) total);

        for (int base = 0; base < total; base += block)
        {
            for (int i = 0; i < block; ++i)
                mono[(size_t) i] = amp * (float) std::sin (6.283185307179586 * hz * (base + i) / sr);

            fx.process (mono.data(), l.data(), r.data(), block);

            for (int i = 0; i < block; ++i)
                out.push_back (l[(size_t) i]);
        }

        const double level = toneLevel (out, (int) (sr * 0.5), total, sr, hz);
        return 20.0 * std::log10 (level / amp);
    };

    auto flat = [] (ParamStore&) {};

    report ("flat is a wire (100 Hz)", std::abs (responseDb (flat, 100.0)) < 0.1,
            juce::String (responseDb (flat, 100.0), 3) + " dB");
    report ("flat is a wire (5 kHz)", std::abs (responseDb (flat, 5000.0)) < 0.1,
            juce::String (responseDb (flat, 5000.0), 3) + " dB");

    auto bellBoost = [] (ParamStore& p)
    {
        p.set (pEq3Freq, 900.0f);
        p.set (pEq3Gain, 9.0f);
        p.set (pEq3Q, 1.0f);
    };

    const double atCentre = responseDb (bellBoost, 900.0);
    const double offCentre = responseDb (bellBoost, 100.0);

    report ("bell boosts its centre", std::abs (atCentre - 9.0) < 0.5,
            juce::String (atCentre, 2) + " dB at 900 Hz for +9 set");
    report ("bell leaves the far field", std::abs (offCentre) < 0.4,
            juce::String (offCentre, 2) + " dB at 100 Hz");

    auto lowCut = [] (ParamStore& p)
    {
        p.set (pEq1Freq, 80.0f);
        p.set (pEq1Gain, -9.0f);
    };

    const double belowShelf = responseDb (lowCut, 35.0);
    const double aboveShelf = responseDb (lowCut, 2000.0);

    report ("low shelf cuts below the corner", std::abs (belowShelf + 9.0) < 0.8,
            juce::String (belowShelf, 2) + " dB at 35 Hz for -9 set");
    report ("low shelf spares the mids", std::abs (aboveShelf) < 0.3,
            juce::String (aboveShelf, 2) + " dB at 2 kHz");

    auto highBoost = [] (ParamStore& p)
    {
        p.set (pEq5Freq, 8000.0f);
        p.set (pEq5Gain, 6.0f);
    };

    const double aboveHigh = responseDb (highBoost, 16000.0);
    const double belowHigh = responseDb (highBoost, 300.0);

    report ("high shelf lifts the top", std::abs (aboveHigh - 6.0) < 0.8,
            juce::String (aboveHigh, 2) + " dB at 16 kHz for +6 set");
    report ("high shelf spares the lows", std::abs (belowHigh) < 0.3,
            juce::String (belowHigh, 2) + " dB at 300 Hz");

    // A resonant bell must be narrower than a wide one — Q reaching the sound.
    auto narrow = [] (ParamStore& p)
    {
        p.set (pEq3Freq, 900.0f);
        p.set (pEq3Gain, 9.0f);
        p.set (pEq3Q, 6.0f);
    };

    const double wideSkirt = responseDb (bellBoost, 450.0);
    const double narrowSkirt = responseDb (narrow, 450.0);

    report ("Q narrows the bell", narrowSkirt < wideSkirt - 1.5,
            "octave-down skirt " + juce::String (wideSkirt, 2) + " dB at Q1 vs "
              + juce::String (narrowSkirt, 2) + " dB at Q6");
}

//==============================================================================
static void testStageOrder (double sr, int block)
{
    std::printf ("\nStage reordering  (%.0f Hz, %d-sample blocks)\n", sr, block);

    std::vector<float> sine;
    makeSine (sine, sr, 110.0, 1.2, 0.15f);

    ParamStore params;
    params.set (pBoostOn, 1.0f);
    params.set (pBoostGain, 18.0f);
    params.set (pBoostSheen, 0.0f);
    params.set (pBoostHair, 0.0f);
    params.set (pDriveOn, 1.0f);
    params.set (pDriveVoice, 3.0f);   // the full-band voice — green would shrug a 110 Hz sine off
    params.set (pDriveAmount, 4.0f);

    const int from = (int) (sr * 0.5);
    const int len = (int) (sr * 0.5);

    const auto thd = [&] (const std::vector<float>& x)
    {
        return goertzel (x.data() + from, len, sr, 330.0)
                 / (goertzel (x.data() + from, len, sr, 110.0) + 1.0e-9);
    };

    // Default order: boost slams the drive's front end — heavy clipping.
    const double hot = thd (renderChain (params, sine, sr, block));

    // Swapped: the same boost merely makes a mildly driven signal louder.
    const int swapped[FxChain::numMovableStages]
    {
        FxChain::stComp, FxChain::stEq, FxChain::stFilter, FxChain::stDrive,
        FxChain::stBoost, FxChain::stOctave, FxChain::stSynth, FxChain::stPhaser,
        FxChain::stChorus, FxChain::stDelay, FxChain::stTape
    };

    const double mild = thd (renderChain (params, sine, sr, block, swapped));

    report ("boost before drive clips harder than after", hot > mild * 3.0 && hot > 0.05,
            "3rd/1st " + juce::String (mild, 4) + " after vs " + juce::String (hot, 4) + " before");

    // A fully scrambled chain with everything engaged still behaves.
    ParamStore all;

    for (int id : { pCompOn, pEqOn, pFltOn, pBoostOn, pDriveOn, pOctOn,
                    pSynOn, pPhaserOn, pChoOn, pDelayOn, pRevOn })
        all.set (id, 1.0f);

    const int scrambled[FxChain::numMovableStages]
    {
        FxChain::stTape, FxChain::stDelay, FxChain::stChorus, FxChain::stPhaser,
        FxChain::stSynth, FxChain::stOctave, FxChain::stDrive, FxChain::stBoost,
        FxChain::stFilter, FxChain::stEq, FxChain::stComp
    };

    std::vector<float> note;
    makeNote (note, sr, 55.0, 3.0, 0.4f);

    const auto out = renderChain (all, note, sr, block, scrambled);

    float peak = 0.0f;
    bool finite = true;

    for (float v : out)
    {
        if (! std::isfinite (v))
            finite = false;

        peak = juce::jmax (peak, std::abs (v));
    }

    report ("reversed chain stays finite and bounded", finite && peak < 8.0f,
            "peak " + juce::String (peak, 3));
}

//==============================================================================
static void testChain (double sr, int block)
{
    std::printf ("\nFull chain  (%.0f Hz, %d-sample blocks, everything engaged)\n", sr, block);

    ParamStore params;
    params.set (pCompOn, 1.0f);
    params.set (pEqOn, 1.0f);
    params.set (pEq1Gain, 4.0f);
    params.set (pEq3Gain, -3.0f);
    params.set (pEq5Gain, 2.0f);
    params.set (pFltOn, 1.0f);
    params.set (pBoostOn, 1.0f);
    params.set (pBoostGain, 4.0f);
    params.set (pDriveOn, 1.0f);
    params.set (pDriveStack, 0.5f);
    params.set (pDriveBite, 0.6f);
    params.set (pOctOn, 1.0f);
    params.set (pSynOn, 1.0f);
    params.set (pSynMix, 0.5f);
    params.set (pPhaserOn, 1.0f);
    params.set (pChoOn, 1.0f);
    params.set (pChoVoices, 3.0f);
    params.set (pDelayOn, 1.0f);
    params.set (pDelayModDepth, 0.5f);
    params.set (pDelaySat, 0.5f);
    params.set (pTapeOn, 1.0f);
    params.set (pRevOn, 1.0f);
    params.set (pCompMakeup, 3.0f);

    FxChain chain (params);
    chain.prepare (sr, block);

    std::vector<float> note;
    makeNote (note, sr, 49.0, 4.0, 0.4f);

    std::vector<float> mono ((size_t) block), left ((size_t) block), right ((size_t) block);

    std::vector<double> blockMicros;
    blockMicros.reserve (note.size() / (size_t) block);

    double totalMicros = 0.0;
    int blocks = 0;
    bool finite = true;
    float peak = 0.0f;

    for (int i = 0; i + block <= (int) note.size(); i += block)
    {
        std::copy (note.begin() + i, note.begin() + i + block, mono.begin());

        const auto t0 = std::chrono::steady_clock::now();
        juce::ScopedNoDenormals noDenormals;   // as the audio callback runs
        chain.process (mono.data(), left.data(), right.data(), block);
        const auto t1 = std::chrono::steady_clock::now();

        const double micros = std::chrono::duration<double, std::micro> (t1 - t0).count();

        if (blocks > 4)   // ignore cold-cache blocks
        {
            blockMicros.push_back (micros);
            totalMicros += micros;
        }

        ++blocks;

        for (int k = 0; k < block; ++k)
        {
            if (! std::isfinite (left[(size_t) k]) || ! std::isfinite (right[(size_t) k]))
                finite = false;

            peak = juce::jmax (peak, std::abs (left[(size_t) k]));
        }
    }

    const double budget = block * 1000000.0 / sr;

    report ("output finite and bounded", finite && peak < 8.0f,
            "peak " + juce::String (peak, 3));

    // Judged at the 99.5th percentile: offline timing on a loaded machine
    // catches scheduler preemptions that say nothing about the DSP's cost.
    std::sort (blockMicros.begin(), blockMicros.end());
    const size_t p995 = (size_t) ((double) (blockMicros.size() - 1) * 0.995);
    const double worstMicros = blockMicros.empty() ? 0.0 : blockMicros[p995];

    report ("worst block inside the callback budget", worstMicros < budget * 0.5,
            juce::String (worstMicros, 1) + " us p99.5, "
                + juce::String (totalMicros / juce::jmax (1, blocks - 5), 1) + " us mean, budget "
                + juce::String (budget, 1) + " us ("
                + juce::String (100.0 * worstMicros / budget, 1) + "% of budget)");

    report ("reported latency under 20 ms", chain.getLatencySamples() / sr * 1000.0 < 20.0,
            juce::String (chain.getLatencySamples() / sr * 1000.0, 2) + " ms");
}

static void testBypassIsClickFree (double sr, int block)
{
    std::printf ("\nBypass toggling\n");

    ParamStore params;
    FxChain chain (params);
    chain.prepare (sr, block);

    std::vector<float> note;
    makeNote (note, sr, 55.0, 6.0, 0.4f);

    std::vector<float> mono ((size_t) block), left ((size_t) block), right ((size_t) block);
    std::vector<float> tail;
    tail.reserve (note.size());

    const int toggleIds[] { pCompOn, pEqOn, pFltOn, pBoostOn, pDriveOn, pOctOn,
                            pSynOn, pPhaserOn, pChoOn, pDelayOn, pTapeOn, pRevOn };
    constexpr int numToggles = (int) std::size (toggleIds);
    int blockIndex = 0;

    for (int i = 0; i + block <= (int) note.size(); i += block)
    {
        // Flip something every 40 blocks, mid-note, the way a foot would.
        if (blockIndex % 40 == 0)
        {
            const int id = toggleIds[(blockIndex / 40) % numToggles];
            params.set (id, params.getBool (id) ? 0.0f : 1.0f);
        }

        std::copy (note.begin() + i, note.begin() + i + block, mono.begin());
        juce::ScopedNoDenormals noDenormals;   // as the audio callback runs
        chain.process (mono.data(), left.data(), right.data(), block);

        for (int k = 0; k < block; ++k)
            tail.push_back (left[(size_t) k]);

        ++blockIndex;
    }

    double maxStep = 0.0, rms = 0.0;
    const int from = (int) (sr * 0.2);

    for (int i = from; i < (int) tail.size(); ++i)
    {
        maxStep = juce::jmax (maxStep, (double) std::abs (tail[(size_t) i] - tail[(size_t) (i - 1)]));
        rms += (double) tail[(size_t) i] * tail[(size_t) i];
    }

    rms = std::sqrt (rms / juce::jmax (1, (int) tail.size() - from));

    report ("no step discontinuity while toggling", maxStep < rms * 1.5,
            "largest step " + juce::String (maxStep, 4) + " vs rms " + juce::String (rms, 4));
}

//==============================================================================
static bool writeSine (const juce::File& file, double sr, double freq, double seconds, float amp)
{
    juce::WavAudioFormat wav;
    file.deleteFile();

    auto stream = std::unique_ptr<juce::FileOutputStream> (file.createOutputStream());

    if (stream == nullptr)
        return false;

    auto writer = std::unique_ptr<juce::AudioFormatWriter> (
                      wav.createWriterFor (stream.get(), sr, 2, 24, {}, 0));

    if (writer == nullptr)
        return false;

    stream.release();

    const int n = (int) (sr * seconds);
    juce::AudioBuffer<float> buffer (2, n);

    for (int i = 0; i < n; ++i)
    {
        const auto s = (float) (amp * std::sin (6.283185307179586 * freq * i / sr));
        buffer.setSample (0, i, s);
        buffer.setSample (1, i, s);
    }

    writer->writeFromAudioSampleBuffer (buffer, 0, n);
    return true;
}

// Renders the player to a buffer with one stem soloed, and reports its pitch.
static double soloAndMeasure (StemPlayer& player, ParamStore& params, int soloIndex,
                              double sr, int block, double* peakOut,
                              double startSeconds = 0.1, double windowSeconds = 0.6,
                              double* rmsOut = nullptr)
{
    if (soloIndex != -2)
        for (int i = 0; i < StemPlayer::numStems; ++i)
            params.set (pStemMute0 + i, (soloIndex >= 0 && i == soloIndex) ? 0.0f : 1.0f);

    player.play();

    // There is no seek in phase 1, so play from the top and keep the window we
    // actually want to look at.
    const int from = (int) (sr * startSeconds);
    const int len = (int) (sr * windowSeconds);
    const int total = from + len + block;

    std::vector<float> left ((size_t) total, 0.0f);
    std::vector<float> bl ((size_t) block), br ((size_t) block);

    for (int i = 0; i + block <= total; i += block)
    {
        std::fill (bl.begin(), bl.end(), 0.0f);
        std::fill (br.begin(), br.end(), 0.0f);
        player.process (bl.data(), br.data(), block);
        std::copy (bl.begin(), bl.end(), left.begin() + i);
    }

    double peak = 0.0, energy = 0.0;

    for (int i = from; i < from + len; ++i)
    {
        peak = juce::jmax (peak, (double) std::abs (left[(size_t) i]));
        energy += (double) left[(size_t) i] * left[(size_t) i];
    }

    if (peakOut != nullptr)
        *peakOut = peak;

    if (rmsOut != nullptr)
        *rmsOut = std::sqrt (energy / juce::jmax (1, len));

    if (peak < 1.0e-4)
        return 0.0;

    return estimateF0 (left.data() + from, len, sr, 30.0, 900.0);
}

static void testStemPlayer (double deviceRate, double fileRate, int block)
{
    std::printf ("\nStem player  (%.0f Hz files into a %.0f Hz device, %d-sample blocks)\n",
                 fileRate, deviceRate, block);

    auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("liverig-test-song");
    folder.deleteRecursively();
    folder.createDirectory();

    const double tones[] { 200.0, 60.0, 400.0 };   // drums, bass, other; vocals deliberately absent

    for (int i = 0; i < 3; ++i)
    {
        if (! writeSine (folder.getChildFile (juce::String (StemPlayer::stemNames[i]) + ".wav"),
                         fileRate, tones[i], 1.0, 0.3f))
        {
            report ("write test stems", false, "could not create WAV files");
            return;
        }
    }

    ParamStore params;
    StemPlayer player (params);
    player.prepare (deviceRate, block);

    const auto result = player.loadSongFolder (folder);

    report ("loads a folder with a stem missing", result.wasOk(),
            result.wasOk() ? "ok" : result.getErrorMessage());

    if (result.failed())
        return;

    report ("missing stem reported, others present",
            player.isStemPresent (0) && player.isStemPresent (1)
                && player.isStemPresent (2) && ! player.isStemPresent (3),
            juce::String (player.isStemPresent (3) ? "vocals wrongly present" : "vocals skipped"));

    report ("length survives the sample rate conversion",
            std::abs (player.getLengthSeconds() - 1.0) < 0.01,
            juce::String (player.getLengthSeconds(), 4) + " s");

    for (int i = 0; i < 3; ++i)
    {
        double peak = 0.0;
        const double measured = soloAndMeasure (player, params, i, deviceRate, block, &peak);
        const double err = measured > 0.0 ? cents (measured, tones[i]) : 9999.0;

        report ((juce::String (StemPlayer::stemNames[i]) + " solo plays at the right pitch").toRawUTF8(),
                std::abs (err) < 10.0 && peak > 0.05,
                juce::String (measured, 2) + " Hz vs " + juce::String (tones[i], 1)
                    + " Hz, " + juce::String (err, 1) + " cents, peak " + juce::String (peak, 3));
    }

    double mutedPeak = 0.0;
    soloAndMeasure (player, params, -1, deviceRate, block, &mutedPeak);
    report ("all stems muted is silent", mutedPeak < 0.001,
            "peak " + juce::String (mutedPeak, 5));

    // Transport: stop returns to the top, and the end of the song stops it.
    for (int i = 0; i < StemPlayer::numStems; ++i)
        params.set (pStemMute0 + i, 0.0f);

    player.play();

    std::vector<float> bl ((size_t) block, 0.0f), br ((size_t) block, 0.0f);
    const int blocksInSong = (int) (deviceRate * 1.2) / block;

    for (int i = 0; i < blocksInSong; ++i)
    {
        std::fill (bl.begin(), bl.end(), 0.0f);
        std::fill (br.begin(), br.end(), 0.0f);
        player.process (bl.data(), br.data(), block);
    }

    report ("stops itself at the end of the song", ! player.isPlaying(), "transport idle");

    player.play();
    player.process (bl.data(), br.data(), block);
    player.stop();
    player.process (bl.data(), br.data(), block);

    double afterStop = 0.0;

    for (int i = 0; i < 8; ++i)
    {
        std::fill (bl.begin(), bl.end(), 0.0f);
        player.process (bl.data(), br.data(), block);

        for (int k = 0; k < block; ++k)
            afterStop = juce::jmax (afterStop, (double) std::abs (bl[(size_t) k]));
    }

    report ("stop silences the playback lane", afterStop < 1.0e-6,
            "peak " + juce::String (afterStop, 6));

    folder.deleteRecursively();
}

//==============================================================================
static void testSectionLoop (double sr, int block)
{
    std::printf ("\nSection loop  (%.0f Hz, %d-sample blocks)\n", sr, block);

    auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("liverig-loop-song");
    folder.deleteRecursively();
    folder.createDirectory();

    if (! writeSine (folder.getChildFile ("bass.wav"), sr, 60.0, 1.0, 0.3f))
    {
        report ("write loop test stem", false, "could not create WAV");
        return;
    }

    ParamStore params;

    for (int i = 0; i < StemPlayer::numStems; ++i)
        params.set (pStemMute0 + i, i == 1 ? 0.0f : 1.0f);   // bass solo

    StemPlayer player (params);
    player.prepare (sr, block);

    if (player.loadSongFolder (folder).failed())
    {
        report ("load loop test song", false, "load failed");
        return;
    }

    report ("too-short region is refused", ! player.setLoop (0, (int) (0.05 * sr)),
            "50 ms rejected");

    // Loop 0.3 s to 0.6033 s — deliberately not a whole number of 60 Hz
    // cycles, so a bare jump would click hard and only the crossfade saves it.
    player.play();
    player.setLoop ((int) (0.3 * sr), (int) (0.6033 * sr));

    const double renderSeconds = 2.5;
    std::vector<float> out;
    out.reserve ((size_t) (sr * renderSeconds));

    std::vector<float> bl ((size_t) block), br ((size_t) block);

    for (int i = 0; i + block <= (int) (sr * renderSeconds); i += block)
    {
        std::fill (bl.begin(), bl.end(), 0.0f);
        std::fill (br.begin(), br.end(), 0.0f);
        player.process (bl.data(), br.data(), block);

        for (int k = 0; k < block; ++k)
            out.push_back (bl[(size_t) k]);
    }

    report ("still playing long past the song's end", player.isPlaying(),
            juce::String (renderSeconds, 1) + " s into a 1.0 s song");

    const int from = (int) (sr * 0.7);
    double maxStep = 0.0;

    for (int i = from + 1; i < (int) out.size(); ++i)
        maxStep = juce::jmax (maxStep, (double) std::abs (out[(size_t) i] - out[(size_t) (i - 1)]));

    report ("seam is crossfaded, not clicked", maxStep < 0.03,
            "largest step " + juce::String (maxStep, 4)
                + " (a bare jump would be ~0.35)");

    const double f0 = estimateF0 (out.data() + from, (int) (sr * 1.5), sr, 30.0, 200.0);

    report ("looped material keeps its pitch", std::abs (cents (f0, 60.0)) < 15.0,
            juce::String (f0, 2) + " Hz");

    // Tap off: the song runs on to its end and stops itself.
    player.clearLoop();

    for (int i = 0; i < (int) (sr * 1.2); i += block)
    {
        std::fill (bl.begin(), bl.end(), 0.0f);
        player.process (bl.data(), br.data(), block);
    }

    report ("loop off runs to the end and stops", ! player.isPlaying(), "transport idle");

    folder.deleteRecursively();
}

//==============================================================================
static void testRealFile (const juce::File& file, double block)
{
    std::printf ("\nReal recording: %s\n", file.getFileName().toRawUTF8());

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
    {
        std::printf ("  could not read that file\n");
        ++failures;
        return;
    }

    const double sr = reader->sampleRate;
    const int n = (int) juce::jmin ((juce::int64) (sr * 60.0), reader->lengthInSamples);
    juce::AudioBuffer<float> buffer (1, n);
    buffer.clear();

    {
        juce::AudioBuffer<float> tmp ((int) reader->numChannels, n);
        reader->read (&tmp, 0, n, 0, true, true);
        buffer.copyFrom (0, 0, tmp, 0, 0, n);
    }

    const int blk = (int) block;
    PitchTracker tracker;
    OctaveShifter shifter;
    tracker.prepare (sr);
    shifter.prepare (sr);
    shifter.setSemitones (12.0f);

    std::vector<float> out ((size_t) n, 0.0f);
    const float* in = buffer.getReadPointer (0);

    int playing = 0, confidentWhilePlaying = 0, frames = 0;
    double minHz = 1.0e9, maxHz = 0.0;

    for (int i = 0; i + blk <= n; i += blk)
    {
        tracker.process (in + i, blk);
        shifter.setPeriod (tracker.getPeriodSamples());
        std::copy (in + i, in + i + blk, out.begin() + i);
        shifter.process (out.data() + i, blk);

        ++frames;

        double energy = 0.0;

        for (int k = 0; k < blk; ++k)
            energy += (double) in[i + k] * in[i + k];

        const bool isPlaying = std::sqrt (energy / blk) > 0.003;   // about -50 dBFS
        const bool isConfident = tracker.getConfidence() > 0.6f;

        if (isPlaying) ++playing;
        if (isPlaying && isConfident) ++confidentWhilePlaying;

        if (isConfident)
        {
            const double hz = sr / tracker.getPeriodSamples();
            minHz = juce::jmin (minHz, hz);
            maxHz = juce::jmax (maxHz, hz);
        }
    }

    std::printf ("  %.1f s at %.0f Hz: notes sounding in %d%% of the take, tracked confidently in"
                 " %d%% of those, detected range %.1f - %.1f Hz\n",
                 n / sr, sr,
                 (int) (100.0 * playing / juce::jmax (1, frames)),
                 (int) (100.0 * confidentWhilePlaying / juce::jmax (1, playing)),
                 minHz, maxHz);

    // Does the output actually sit an octave above the input, window by window?
    const int win = (int) (sr * 0.5);
    int checked = 0, withinAnOctave = 0;
    double worstCents = 0.0;

    for (int start = 0; start + win <= n; start += win)
    {
        double energy = 0.0;

        for (int i = start; i < start + win; ++i)
            energy += (double) in[i] * in[i];

        if (std::sqrt (energy / win) < 0.01)   // skip rests and the intro
            continue;

        const double f0In  = estimateF0 (in + start, win, sr, 25.0, 400.0);
        const double f0Out = estimateF0 (out.data() + start, win, sr, 50.0, 800.0);
        const double err = cents (f0Out, f0In * 2.0);

        ++checked;

        if (std::abs (err) < 30.0)
        {
            ++withinAnOctave;
            worstCents = juce::jmax (worstCents, std::abs (err));
        }
    }

    const double hitRate = 100.0 * withinAnOctave / juce::jmax (1, checked);

    report ("output sits an octave above the input", hitRate > 85.0,
            juce::String (withinAnOctave) + " of " + juce::String (checked)
                + " half-second windows within 30 cents (" + juce::String (hitRate, 0)
                + "%), worst of those " + juce::String (worstCents, 1) + " cents");

    const auto writeMono = [&formats] (const juce::File& target, double rate, const float* data, int count)
    {
        if (auto* wav = formats.findFormatForFileExtension ("wav"))
        {
            target.deleteFile();

            if (auto stream = std::unique_ptr<juce::FileOutputStream> (target.createOutputStream()))
            {
                if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (
                                      wav->createWriterFor (stream.get(), rate, 1, 24, {}, 0)))
                {
                    stream.release();
                    const float* channels[1] { data };
                    writer->writeFromFloatArrays (channels, 1, count);
                    std::printf ("  wrote %s\n", target.getFullPathName().toRawUTF8());
                }
            }
        }
    };

    writeMono (file.getSiblingFile (file.getFileNameWithoutExtension() + "-octave.wav"),
               sr, out.data(), n);

    // The showcase chains over the same take, so a new sound can be judged by
    // ear without plugging in. Each lands next to the input as <name>-<fx>.wav.
    struct Demo
    {
        const char* suffix;
        std::vector<std::pair<int, float>> settings;
    };

    const Demo demos[]
    {
        { "funk-filter", { { pCompOn, 1.0f }, { pCompThresh, -18.0f }, { pCompRatio, 6.0f },
                           { pCompAttack, 1.0f }, { pCompMakeup, 4.0f },
                           { pFltOn, 1.0f }, { pFltSens, 0.55f }, { pFltAttack, 3.0f },
                           { pFltRelease, 160.0f }, { pFltFrom, 120.0f }, { pFltTo, 2200.0f },
                           { pFltRes, 5.0f } } },

        { "synth-bass", { { pCompOn, 1.0f }, { pCompThresh, -20.0f }, { pCompRatio, 4.0f },
                          { pCompAttack, 2.0f }, { pCompMakeup, 3.0f },
                          { pSynOn, 1.0f }, { pSynShape, 0.15f }, { pSynSub, 0.7f },
                          { pSynCutoff, 300.0f }, { pSynEnv, 0.55f }, { pSynRes, 2.2f },
                          { pSynGlide, 12.0f }, { pSynMix, 0.85f } } },

        { "stacked-lead", { { pBoostOn, 1.0f }, { pBoostGain, 8.0f }, { pBoostSheen, 2.0f },
                            { pBoostHair, 0.2f },
                            { pDriveOn, 1.0f }, { pDriveAmount, 8.0f }, { pDriveTone, 3400.0f },
                            { pDriveLevel, -3.0f }, { pDriveStack, 0.6f }, { pDriveBite, 0.6f } } },

        { "analog-wash", { { pChoOn, 1.0f }, { pChoRate, 0.7f }, { pChoDepth, 0.55f },
                           { pChoVoices, 3.0f }, { pChoTone, 6000.0f }, { pChoLows, 140.0f },
                           { pChoMix, 0.55f },
                           { pDelayOn, 1.0f }, { pDelayTime, 380.0f }, { pDelayFeedback, 0.5f },
                           { pDelayMix, 0.33f }, { pDelayHighcut, 2400.0f },
                           { pDelayModRate, 0.9f }, { pDelayModDepth, 0.4f }, { pDelaySat, 0.6f },
                           { pRevOn, 1.0f }, { pRevSize, 0.68f }, { pRevDamp, 0.4f },
                           { pRevMix, 0.3f } } },
    };

    std::vector<float> input (in, in + n);

    for (const auto& demo : demos)
    {
        ParamStore chainParams;

        for (const auto& [id, value] : demo.settings)
            chainParams.set (id, value);

        const auto rendered = renderChain (chainParams, input, sr, blk);

        double peak = 0.0;
        bool finite = true;

        for (float v : rendered)
        {
            if (! std::isfinite (v))
                finite = false;

            peak = juce::jmax (peak, (double) std::abs (v));
        }

        report ((juce::String (demo.suffix) + " render is sane").toRawUTF8(),
                finite && peak > 0.001 && peak < 4.0,
                "peak " + juce::String (peak, 3));

        writeMono (file.getSiblingFile (file.getFileNameWithoutExtension() + "-" + demo.suffix + ".wav"),
                   sr, rendered.data(), (int) rendered.size());
    }
}

// Loads a real song folder through the actual player and reports what came out.
static void testRealSongFolder (const juce::File& folder, double deviceRate, int block)
{
    std::printf ("\nReal song folder: %s\n", folder.getFileName().toRawUTF8());

    ParamStore params;
    StemPlayer player (params);
    player.prepare (deviceRate, block);

    const auto result = player.loadSongFolder (folder);

    report ("loads through StemPlayer", result.wasOk(),
            result.wasOk() ? juce::String (player.getLengthSeconds(), 2) + " s at "
                                 + juce::String (deviceRate / 1000.0, 1) + " kHz"
                           : result.getErrorMessage());

    if (result.failed())
        return;

    const double look = juce::jmin (40.0, player.getLengthSeconds() * 0.4);

    for (int i = 0; i < StemPlayer::numStems; ++i)
    {
        double peak = 0.0, rms = 0.0;
        soloAndMeasure (player, params, i, deviceRate, block, &peak, look, 8.0, &rms);

        std::printf ("  %-8s %s  peak %.3f  rms %.1f dBFS\n", StemPlayer::stemNames[i],
                     player.isStemPresent (i) ? "present" : "MISSING", peak,
                     20.0 * std::log10 (juce::jmax (1.0e-9, rms)));
    }

    // Everything up, then bass muted: the way it gets used on stage.
    for (int i = 0; i < StemPlayer::numStems; ++i)
        params.set (pStemMute0 + i, 0.0f);

    double fullPeak = 0.0;
    double fullEnergy = 0.0, mutedEnergy = 0.0;

    for (int pass = 0; pass < 2; ++pass)
    {
        for (int i = 0; i < StemPlayer::numStems; ++i)
            params.set (pStemMute0 + i, 0.0f);

        params.set (pStemMute1, pass == 0 ? 0.0f : 1.0f);

        double peak = 0.0, rms = 0.0;
        soloAndMeasure (player, params, -2, deviceRate, block, &peak, look, 8.0, &rms);

        if (pass == 0) { fullPeak = peak; fullEnergy = rms; }
        else           { mutedEnergy = rms; }
    }

    report ("full mix plays", fullPeak > 0.05 && fullPeak < 4.0,
            "peak " + juce::String (fullPeak, 3) + ", rms " + juce::String (fullEnergy, 4));

    report ("muting bass drops the mix without gutting it",
            mutedEnergy < fullEnergy * 0.99 && mutedEnergy > fullEnergy * 0.3,
            "rms " + juce::String (fullEnergy, 4) + " -> " + juce::String (mutedEnergy, 4)
                + " (" + juce::String (20.0 * std::log10 (mutedEnergy / juce::jmax (1.0e-9, fullEnergy)), 2) + " dB)");
}


//==============================================================================
// Bass-centric additions: the crossover around the drive, the divider sub,
// the cabinet filter, and the compressor's sidechain.

// Energy at one frequency, by Goertzel — enough to measure a harmonic.
static double toneEnergy (const float* x, int n, double sr, double hz)
{
    const double w = 2.0 * 3.141592653589793 * hz / sr;
    const double c = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const double s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    return std::sqrt (std::max (0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / n;
}

static void testDriveCrossover (double sr, int block)
{
    std::printf ("\n Drive crossover (%.0f Hz, %d-sample blocks)\n", sr, block);

    const double f0 = 50.0;
    const int n = (int) (sr * 1.5);
    std::vector<float> in ((size_t) n, 0.0f);

    for (int i = 0; i < n; ++i)
        in[(size_t) i] = 0.45f * (float) std::sin (6.283185307179586 * f0 * i / sr);

    // Same hot drive twice: once with the fundamental going through the
    // clipper, once with it routed around.
    auto distortionAt = [&] (float crossover)
    {
        ParamStore params;
        params.set (pDriveOn, 1.0f);
        params.set (pDriveAmount, 40.0f);
        params.set (pDriveXover, crossover);
        params.set (pDriveBlend, 1.0f);

        const auto out = renderChain (params, in, sr, block);
        const int from = (int) (sr * 0.5), len = (int) (sr * 0.9);

        const double fundamental = toneEnergy (out.data() + from, len, sr, f0);
        double harmonics = 0.0;

        for (int h = 2; h <= 9; ++h)
            harmonics += toneEnergy (out.data() + from, len, sr, f0 * h);

        return harmonics / std::max (1.0e-9, fundamental);
    };

    const double through = distortionAt (40.0f);     // crossover parked at the bottom
    const double around  = distortionAt (700.0f);    // fundamental bypasses the clipper

    report ("crossover keeps the fundamental clean", around < through * 0.35,
            juce::String (through, 3) + " harmonic-to-fundamental through the clipper, "
                + juce::String (around, 3) + " around it");

    // Blend at zero must be a wire, whatever the drive is set to.
    ParamStore blended;
    blended.set (pDriveOn, 1.0f);
    blended.set (pDriveAmount, 40.0f);
    blended.set (pDriveXover, 300.0f);
    blended.set (pDriveBlend, 0.0f);

    ParamStore off;   // the same chain with the stage bypassed, so the input
                      // conditioning either side of it is identical
    const auto withStage = renderChain (blended, in, sr, block);
    const auto without = renderChain (off, in, sr, block);
    const int from = (int) (sr * 0.5), len = (int) (sr * 0.9);

    double worst = 0.0;

    for (int i = from; i < from + len; ++i)
        worst = std::max (worst, (double) std::abs (withStage[(size_t) i] - without[(size_t) i]));

    report ("blend at zero is a wire around the clipper", worst < 0.002,
            "largest departure " + juce::String (worst, 6));
}

static void testSubOctave (double sr, int block)
{
    std::printf ("\n Divider sub (%.0f Hz, %d-sample blocks)\n", sr, block);

    for (double f0 : { 55.0, 82.41, 110.0 })
    {
        for (int divide = 0; divide < 2; ++divide)
        {
            std::vector<float> note;
            makeNote (note, sr, f0, 2.0, 0.4f);

            ParamStore params;
            params.set (pSubOn, 1.0f);
            params.set (pSubLevel, 1.0f);
            params.set (pSubDivide, (float) divide);
            params.set (pSubTone, 2000.0f);

            ParamStore off;
            const auto out = renderChain (params, note, sr, block);
            const auto dry = renderChain (off, note, sr, block);

            const int from = (int) (sr * 0.6), len = (int) (sr * 0.8);
            const double expected = f0 / (divide == 0 ? 2.0 : 4.0);

            // The divider adds a voice under the note rather than replacing
            // it, so measure the octave it puts there against the same
            // rendering without it.
            const double added = toneEnergy (out.data() + from, len, sr, expected);
            const double already = toneEnergy (dry.data() + from, len, sr, expected);
            const double lift = added / std::max (1.0e-9, already);

            report ((juce::String (f0, 1) + " Hz, divide by "
                        + juce::String (divide == 0 ? 2 : 4)).toRawUTF8(),
                    lift > 8.0,
                    juce::String (expected, 2) + " Hz lifted "
                        + juce::String (20.0 * std::log10 (lift), 1) + " dB by the divider");
        }
    }
}

static void testCabFilter (double sr, int block)
{
    std::printf ("\n Cabinet filter (%.0f Hz)\n", sr);

    auto levelAt = [&] (double hz, bool cabOn)
    {
        const int n = (int) (sr * 0.8);
        std::vector<float> in ((size_t) n, 0.0f);

        for (int i = 0; i < n; ++i)
            in[(size_t) i] = 0.3f * (float) std::sin (6.283185307179586 * hz * i / sr);

        ParamStore params;
        params.set (pCabOn, cabOn ? 1.0f : 0.0f);

        const auto out = renderChain (params, in, sr, block);
        const int from = (int) (sr * 0.4);

        return toneEnergy (out.data() + from, (int) (sr * 0.3), sr, hz);
    };

    const double ref = levelAt (400.0, true) / std::max (1.0e-9, levelAt (400.0, false));
    const double top = levelAt (9000.0, true) / std::max (1.0e-9, levelAt (9000.0, false));
    const double sub = levelAt (25.0, true) / std::max (1.0e-9, levelAt (25.0, false));

    report ("rolls off above the cab's top", top < ref * 0.1,
            juce::String (20.0 * std::log10 (top / ref), 1) + " dB at 9 kHz relative to 400 Hz");

    report ("loses the subsonics a cab cannot move", sub < ref * 0.5,
            juce::String (20.0 * std::log10 (sub / ref), 1) + " dB at 25 Hz relative to 400 Hz");
}

static void testCompSidechain (double sr, int block)
{
    std::printf ("\n Compressor sidechain (%.0f Hz)\n", sr);

    std::vector<float> note;
    makeNote (note, sr, 45.0, 2.0, 0.5f);

    auto reductionWith = [&] (float sidechainHz)
    {
        ParamStore params;
        params.set (pCompOn, 1.0f);
        params.set (pCompThresh, -28.0f);
        params.set (pCompRatio, 8.0f);
        params.set (pCompMakeup, 0.0f);
        params.set (pCompBlend, 1.0f);
        params.set (pCompSidechain, sidechainHz);

        const auto out = renderChain (params, note, sr, block);
        const int from = (int) (sr * 0.3), len = (int) (sr * 1.0);

        return rmsOf (out.data() + from, len) / std::max (1.0e-9, rmsOf (note.data() + from, len));
    };

    const double wideOpen = reductionWith (20.0f);    // detector hears the fundamental
    const double filtered = reductionWith (300.0f);   // detector above it

    report ("a high sidechain stops a low note ducking the signal", filtered > wideOpen * 1.15,
            juce::String (20.0 * std::log10 (wideOpen), 2) + " dB of reduction at 20 Hz, "
                + juce::String (20.0 * std::log10 (filtered), 2) + " dB at 300 Hz");
}


// The one drive preset that leans on the crossover. If it is ever edited back
// to a full-band drive its name stops being true, so the claim is asserted
// against the shipped preset rather than a copy of its numbers.
static void testBassTightPreset (double sr, int block)
{
    std::printf ("\n Bass tight preset (%.0f Hz)\n", sr);

    const auto presetFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("liverig-bass-tight-test.json");
    presetFile.deleteFile();

    PresetStore presets;
    presets.fileOverride = presetFile;
    presets.load();

    const double f0 = 50.0;
    const int n = (int) (sr * 1.5);
    std::vector<float> in ((size_t) n, 0.0f);

    for (int i = 0; i < n; ++i)
        in[(size_t) i] = 0.45f * (float) std::sin (6.283185307179586 * f0 * i / sr);

    auto dirtOf = [&] (const juce::String& presetName, bool& found)
    {
        ParamStore params;
        params.set (pDriveOn, 1.0f);
        found = presets.applyEffect ("drive", presetName, params);

        const auto out = renderChain (params, in, sr, block);
        const int from = (int) (sr * 0.5), len = (int) (sr * 0.9);

        const double fundamental = toneEnergy (out.data() + from, len, sr, f0);
        double harmonics = 0.0;

        for (int h = 2; h <= 9; ++h)
            harmonics += toneEnergy (out.data() + from, len, sr, f0 * h);

        return harmonics / std::max (1.0e-9, fundamental);
    };

    bool haveTight = false, haveGrit = false;
    const double tight = dirtOf ("Bass tight", haveTight);
    const double grit = dirtOf ("Grit", haveGrit);

    report ("ships a Bass tight drive preset", haveTight, haveTight ? "found" : "missing");

    report ("it keeps a low fundamental out of the clipper", haveTight && tight < 0.02,
            juce::String (tight, 4) + " harmonic-to-fundamental at 50 Hz");

    report ("tighter down there than the full-band preset beside it",
            haveTight && haveGrit && tight < grit * 0.4,
            "Bass tight " + juce::String (tight, 4) + " against Grit " + juce::String (grit, 4));

    presetFile.deleteFile();
}


//==============================================================================
// MIDI note segmentation. Aimed at pads and swells, so the standard is not
// "fast" — it is "never wrong, never stuck, never chattering".

struct CapturedNote { int note = 0; double on = 0.0, off = -1.0; int velocity = 0; };

static std::vector<CapturedNote> segmentNotes (const std::vector<float>& in, double sr, int block,
                                               int* stuckOut = nullptr, int transpose = 0,
                                               float gateDb = -42.0f)
{
    PitchTracker tracker;
    NoteSegmenter seg;
    tracker.prepare (sr);
    seg.prepare (sr);
    seg.setParams (gateDb, 45.0f, 180.0f, transpose, 2);

    std::vector<CapturedNote> notes;
    NoteEvent events[8];

    for (int i = 0; i + block <= (int) in.size(); i += block)
    {
        tracker.process (in.data() + i, block);
        const int count = seg.process (in.data() + i, block, tracker.getPeriodSamples(),
                                       tracker.getConfidence(), events, 8);
        const double t = (i + block) / sr;

        for (int e = 0; e < count; ++e)
        {
            if (events[e].type == NoteEvent::noteOn)
            {
                notes.push_back ({ events[e].a, t, -1.0, events[e].b });
            }
            else if (events[e].type == NoteEvent::noteOff)
            {
                for (int k = (int) notes.size() - 1; k >= 0; --k)
                {
                    if (notes[(size_t) k].note == events[e].a && notes[(size_t) k].off < 0.0)
                    {
                        notes[(size_t) k].off = t;
                        break;
                    }
                }
            }
        }
    }

    int stuck = 0;

    for (const auto& n : notes)
        if (n.off < 0.0)
            ++stuck;

    if (stuckOut != nullptr)
        *stuckOut = stuck;

    return notes;
}

static void testNoteSegmentation (double sr, int block)
{
    std::printf ("\n MIDI note segmentation (%.0f Hz, %d-sample blocks)\n", sr, block);

    const double pitches[] { 41.20, 55.00, 73.42, 98.00 };
    const int expected[] { 28, 33, 38, 43 };

    std::vector<float> seq;

    for (double f0 : pitches)
    {
        std::vector<float> note;
        makeNote (note, sr, f0, 1.2, 0.45f);
        seq.insert (seq.end(), note.begin(), note.end());
        seq.insert (seq.end(), (size_t) (sr * 0.45), 0.0f);
    }

    int stuck = 0;
    const auto notes = segmentNotes (seq, sr, block, &stuck);

    report ("one note per phrase, nothing extra", notes.size() == 4,
            juce::String ((int) notes.size()) + " notes from 4 phrases");

    bool pitchesRight = notes.size() == 4;

    for (size_t i = 0; i < notes.size() && i < 4; ++i)
        pitchesRight = pitchesRight && notes[i].note == expected[i];

    juce::String got;

    for (const auto& n : notes)
        got << n.note << " ";

    report ("the right notes", pitchesRight, "got " + got.trim() + ", wanted 28 33 38 43");

    report ("nothing left sounding", stuck == 0, juce::String (stuck) + " stuck");

    double shortest = 1.0e9;

    for (const auto& n : notes)
        if (n.off > 0.0)
            shortest = std::min (shortest, n.off - n.on);

    report ("no note is a blip", notes.empty() || shortest > 0.5,
            "shortest " + juce::String (shortest, 2) + " s");

    // Transpose is what makes this useful: play the bass, sound the pad up top.
    const auto up = segmentNotes (seq, sr, block, nullptr, 12);
    bool shifted = up.size() == notes.size();

    for (size_t i = 0; i < up.size() && i < notes.size(); ++i)
        shifted = shifted && up[i].note == notes[i].note + 12;

    report ("transpose moves every note", shifted,
            juce::String ((int) up.size()) + " notes, an octave up");
}

static void testVibratoDoesNotChatter (double sr, int block)
{
    std::printf ("\n MIDI under vibrato and decay\n");

    // A held note with vibrato deep enough to cross the semitone boundary
    // either side. A pad that retriggers on every wobble is unusable.
    const double f0 = 55.0, depth = 0.035, rate = 2.5;   // about +/- 60 cents, slow
    const int n = (int) (sr * 3.0);
    std::vector<float> in ((size_t) n, 0.0f);

    double phase[6] {};

    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double f = f0 * (1.0 + depth * std::sin (6.283185307179586 * rate * t));
        double s = 0.0;

        for (int h = 0; h < 6; ++h)
        {
            s += std::sin (phase[h]) / std::pow ((double) (h + 1), 1.2);
            phase[h] += 6.283185307179586 * f * (h + 1) / sr;
        }

        in[(size_t) i] = (float) (s * 0.35);
    }

    int stuck = 0;
    const auto notes = segmentNotes (in, sr, block, &stuck);

    report ("vibrato across a semitone does not retrigger", notes.size() == 1,
            juce::String ((int) notes.size()) + " notes over 3 s of vibrato");

    // Silence must end it, and within the release time.
    std::vector<float> withTail = in;
    withTail.insert (withTail.end(), (size_t) (sr * 1.0), 0.0f);

    const auto tailed = segmentNotes (withTail, sr, block, &stuck);

    report ("silence ends the note", stuck == 0 && ! tailed.empty(),
            tailed.empty() ? "no note at all"
                           : juce::String (tailed.back().off - 3.0, 3) + " s after the audio stopped");
}

// Play a pad two octaves up, bring the transpose back, and the pitch must
// return to true. The receiving synth's wheel is invisible from here, so the
// only safe protocol is to recentre it with every note this side starts — and
// never to chase a transpose jump with a bend the wheel cannot reach anyway.
static void testTransposeComesBackTrue (double sr, int block)
{
    std::printf ("\n MIDI transpose returns to true pitch (%.0f Hz, %d-sample blocks)\n", sr, block);

    PitchTracker tracker;
    NoteSegmenter seg;
    tracker.prepare (sr);
    seg.prepare (sr);
    seg.setParams (-42.0f, 45.0f, 180.0f, 24, 2);

    // One long steady note; the transpose drops to 0 midway through it.
    std::vector<float> in;
    makeNote (in, sr, 55.0, 4.5, 0.45f);
    in.insert (in.end(), (size_t) (sr * 0.6), 0.0f);

    const int switchAt = (int) (sr * 2.2);

    NoteEvent events[8];
    std::vector<NoteEvent> wire;
    bool switched = false;

    for (int i = 0; i + block <= (int) in.size(); i += block)
    {
        if (! switched && i >= switchAt)
        {
            seg.setParams (-42.0f, 45.0f, 180.0f, 0, 2);
            switched = true;
        }

        tracker.process (in.data() + i, block);
        const int count = seg.process (in.data() + i, block, tracker.getPeriodSamples(),
                                       tracker.getConfidence(), events, 8);

        for (int e = 0; e < count; ++e)
            wire.push_back (events[e]);
    }

    int noteOns = 0, uncentred = 0, railBends = 0;
    int lastBendOnWire = 8192;
    int firstNote = -1, lastNote = -1;

    for (const auto& e : wire)
    {
        if (e.type == NoteEvent::bend)
        {
            lastBendOnWire = e.a;

            if (e.a == 0 || e.a == 16383)
                ++railBends;
        }
        else if (e.type == NoteEvent::noteOn)
        {
            ++noteOns;

            if (lastBendOnWire != 8192)
                ++uncentred;

            if (firstNote < 0)
                firstNote = e.a;

            lastNote = e.a;
        }
    }

    report ("the transposed and true notes both speak", noteOns == 2 && firstNote == 57 && lastNote == 33,
            juce::String (noteOns) + " notes: first " + juce::String (firstNote)
              + ", last " + juce::String (lastNote) + " (wanted 57 then 33)");

    // The previous note's bend is still latched in the receiver at the moment
    // a new note starts; nothing but an explicit recentre clears it.
    report ("every note starts on a centred wheel", noteOns > 0 && uncentred == 0,
            juce::String (uncentred) + " of " + juce::String (noteOns) + " notes started detuned");

    report ("the transpose jump is not chased to the rails", railBends == 0,
            juce::String (railBends) + " full-scale bends on the wire");
}

// The tracker's confidence flickers on real playing. One doubtful frame must
// not send the hold back to zero, and a pitch that straddles a semitone
// boundary must not flip the candidate forever — both read as "tracking is
// broken" on stage even though the tracker itself is fine.
static void testHoldSurvivesRealTracking (double sr, int block)
{
    std::printf ("\n MIDI hold under real-world confidence (%.0f Hz, %d-sample blocks)\n", sr, block);

    std::vector<float> tone ((size_t) block, 0.3f);   // the env only reads level
    NoteEvent events[8];

    // -- a periodic one-frame confidence dropout --
    {
        NoteSegmenter seg;
        seg.prepare (sr);
        seg.setParams (-42.0f, 45.0f, 180.0f, 0, 2);

        const float period = (float) (sr / 110.0);
        const int flickerEvery = juce::jmax (1, (int) (sr * 0.030) / block);   // every 30 ms

        double onAt = -1.0;
        int blocks = 0;

        for (int i = 0; i * block < (int) (sr * 0.5); ++i, ++blocks)
        {
            const bool flicker = (i > 0 && (i % flickerEvery) == 0);
            const int count = seg.process (tone.data(), block, period,
                                           flicker ? 0.1f : 0.9f, events, 8);

            for (int e = 0; e < count; ++e)
                if (events[e].type == NoteEvent::noteOn && onAt < 0.0)
                    onAt = (i + 1) * block / sr;
        }

        report ("a one-frame flicker does not restart the hold", onAt >= 0.0 && onAt < 0.15,
                onAt < 0.0 ? "no note within 500 ms"
                           : "note on after " + juce::String (onAt * 1000.0, 0) + " ms");
    }

    // -- a pitch sat between two note names --
    {
        NoteSegmenter seg;
        seg.prepare (sr);
        seg.setParams (-42.0f, 45.0f, 180.0f, 0, 2);

        // Alternate between 45 cents either side of the A/A# boundary, the way
        // a slightly sharp string reads from frame to frame.
        const float hzLow  = 440.0f * std::pow (2.0f, (45.45f - 69.0f) / 12.0f);
        const float hzHigh = 440.0f * std::pow (2.0f, (45.55f - 69.0f) / 12.0f);

        int noteOns = 0;
        double onAt = -1.0;

        for (int i = 0; i * block < (int) (sr * 1.0); ++i)
        {
            const float hz = (i & 1) != 0 ? hzHigh : hzLow;
            const int count = seg.process (tone.data(), block, (float) sr / hz, 0.9f, events, 8);

            for (int e = 0; e < count; ++e)
            {
                if (events[e].type == NoteEvent::noteOn)
                {
                    ++noteOns;

                    if (onAt < 0.0)
                        onAt = (i + 1) * block / sr;
                }
            }
        }

        report ("a boundary pitch settles instead of flip-flopping", noteOns == 1 && onAt < 0.15,
                noteOns == 0 ? "never spoke"
                             : juce::String (noteOns) + " notes, first after "
                                 + juce::String (onAt * 1000.0, 0) + " ms");
    }
}

// Plucked bass through a pickup often hands the tracker more second harmonic
// than fundamental — the classic way a bass tracker reads an octave up. YIN's
// first-dip rule alone takes the bait; the true period leaves a much deeper
// dip at double the lag, and that is checkable.
static void testTrackerOnDominantSecondHarmonic (double sr, int block)
{
    std::printf ("\n Tracker with a dominant second harmonic (%.0f Hz, %d-sample blocks)\n", sr, block);

    for (double f0 : { 55.0, 41.2 })
    {
        const int n = (int) (sr * 2.0);
        std::vector<float> in ((size_t) n, 0.0f);
        double p1 = 0.3, p2 = 1.1, p3 = 0.7;

        for (int i = 0; i < n; ++i)
        {
            const double s = 0.2 * std::sin (p1) + 1.0 * std::sin (p2) + 0.15 * std::sin (p3);
            p1 += 6.283185307179586 * f0 / sr;
            p2 += 6.283185307179586 * 2.0 * f0 / sr;
            p3 += 6.283185307179586 * 3.0 * f0 / sr;
            in[(size_t) i] = (float) (s * 0.35);
        }

        PitchTracker tracker;
        tracker.prepare (sr);

        for (int i = 0; i + block <= n; i += block)
            tracker.process (in.data() + i, block);

        const double got = tracker.getFrequency();
        const double off = cents (got, f0);

        report ((juce::String (f0, 1) + " Hz with 2nd harmonic 5x the fundamental").toRawUTF8(),
                std::abs (off) < 60.0,
                juce::String (got, 2) + " Hz, " + juce::String (off, 0) + " cents off");
    }
}

// With MIDI on the rig is usually a controller: the pad speaks and the bass
// does not, unless Thru is deliberately on. Either way the tracker must keep
// hearing the input — it taps the chain ahead of the output gain.
static void testMidiThruGatesTheLiveLane (double sr, int block)
{
    std::printf ("\n MIDI thru gating the live lane (%.0f Hz, %d-sample blocks)\n", sr, block);

    struct Case { const char* label; bool midiOn; bool thru; bool audible; };

    const Case cases[] =
    {
        { "MIDI off: the live lane sounds",        false, false, true  },
        { "MIDI on, thru off: the bass is out",    true,  false, false },
        { "MIDI on, thru on: the layer stays",     true,  true,  true  },
    };

    for (const auto& c : cases)
    {
        ParamStore params;
        params.set (pMidiOn, c.midiOn ? 1.0f : 0.0f);
        params.set (pMidiThru, c.thru ? 1.0f : 0.0f);

        FxChain chain (params);
        chain.prepare (sr, block);

        std::vector<float> mono ((size_t) block), l ((size_t) block), r ((size_t) block);
        double phase = 0.0;
        float peak = 0.0f;
        const int total = (int) (sr * 0.6);

        for (int i = 0; i + block <= total; i += block)
        {
            for (int k = 0; k < block; ++k)
            {
                mono[(size_t) k] = 0.3f * (float) std::sin (phase);
                phase += 6.283185307179586 * 110.0 / sr;
            }

            chain.process (mono.data(), l.data(), r.data(), block);

            if (i > (int) (sr * 0.3))   // past every smoother
                for (int k = 0; k < block; ++k)
                    peak = std::max (peak, std::abs (l[(size_t) k]));
        }

        const bool audible = peak > 0.05f;
        report (c.label, audible == c.audible && (c.audible || peak < 1.0e-4f),
                "peak " + juce::String (peak, 6));

        if (c.midiOn)
            report ("  ...and the tracker still hears", std::abs (chain.getTrackedHz() - 110.0f) < 3.0f,
                    juce::String (chain.getTrackedHz(), 1) + " Hz tracked");
    }

    // The controller rule belongs to the tracked channel, which is the one
    // the tracker taps. Another strip's board is not the bass and keeps
    // sounding — the keys strip, carrying the pad itself, most of all.
    {
        ParamStore params;
        params.set (pMidiOn, 1.0f);
        params.set (pMidiThru, 0.0f);

        FxChain board (params, 2);
        board.prepare (sr, block);

        std::vector<float> mono ((size_t) block), l ((size_t) block), r ((size_t) block);
        double phase = 0.0;
        float peak = 0.0f;
        const int total = (int) (sr * 0.6);

        for (int i = 0; i + block <= total; i += block)
        {
            for (int k = 0; k < block; ++k)
            {
                mono[(size_t) k] = 0.3f * (float) std::sin (phase);
                phase += 6.283185307179586 * 110.0 / sr;
            }

            board.process (mono.data(), l.data(), r.data(), block);

            if (i > (int) (sr * 0.3))
                for (int k = 0; k < block; ++k)
                    peak = std::max (peak, std::abs (l[(size_t) k]));
        }

        report ("MIDI on, thru off: another strip is not the bass and stays",
                peak > 0.05f, "peak " + juce::String (peak, 4));
    }
}

// MUTE on a strip's live out: the one control that has to work while the
// band is playing. It stops that strip and nothing else, and it fades
// rather than edits, because a mute mid-phrase must not click the PA.
static void testLiveOutMute (double sr, int block)
{
    std::printf ("\n Live out mute (%.0f Hz, %d-sample blocks)\n", sr, block);

    ParamStore params;
    FxChain strip (params, 1);
    strip.prepare (sr, block);

    std::vector<float> mono ((size_t) block), l ((size_t) block), r ((size_t) block);
    double phase = 0.0;
    float previous = 0.0f, step = 0.0f;

    const auto run = [&] (FxChain& chain, double seconds, bool measureStep) -> float
    {
        float peak = 0.0f;
        const int blocks = (int) (sr * seconds) / block;

        for (int i = 0; i < blocks; ++i)
        {
            for (int k = 0; k < block; ++k)
            {
                mono[(size_t) k] = 0.3f * (float) std::sin (phase);
                phase += 6.283185307179586 * 110.0 / sr;
            }

            chain.process (mono.data(), l.data(), r.data(), block);

            for (int k = 0; k < block; ++k)
            {
                if (i > blocks / 2)
                    peak = std::max (peak, std::abs (l[(size_t) k]));

                if (measureStep)
                    step = std::max (step, std::abs (l[(size_t) k] - previous));

                previous = l[(size_t) k];
            }
        }

        return peak;
    };

    const float open = run (strip, 0.6, false);
    report ("an open strip reaches the live out", open > 0.05f,
            "peak " + juce::String (open, 4));

    params.set (1, pLiveMute, 1.0f);
    const float muted = run (strip, 0.6, false);
    report ("MUTE stops it there", muted < 1.0e-4f, "peak " + juce::String (muted, 7));

    // Off and on over one continuous note: the largest sample step must stay
    // in sine territory. A hard mute would step by the amplitude.
    params.set (1, pLiveMute, 0.0f);
    run (strip, 0.3, true);
    params.set (1, pLiveMute, 1.0f);
    run (strip, 0.3, true);
    params.set (1, pLiveMute, 0.0f);
    run (strip, 0.3, true);

    report ("the mute is a fade, not an edit", step < 0.02f,
            "largest step " + juce::String (step, 4));

    // One strip's mute is its own: the store keeps a copy per channel.
    params.set (1, pLiveMute, 1.0f);
    FxChain other (params, 0);
    other.prepare (sr, block);
    const float neighbour = run (other, 0.6, false);

    report ("muting one strip leaves the next one playing", neighbour > 0.05f,
            "peak " + juce::String (neighbour, 4));
}

// The tuner display. The tracker is honest about every frame; the filter's
// job is to show the underlying pitch — steady, slightly behind the truth,
// and immune to the flicks that make a raw readout unusable.
static void testTunerFilter()
{
    std::printf ("\n Tuner conditioning (30 Hz UI ticks)\n");

    const auto hzOf = [] (double semis) { return (float) (440.0 * std::pow (2.0, (semis - 69.0) / 12.0)); };

    // -- first frame after silence reads immediately --
    {
        TunerFilter f;
        const auto r = f.push (55.3f, 1.0f, 0.2f);

        report ("first confident frame goes live at once", r.live && r.locked == 33,
                r.live ? "locked " + juce::String (r.locked) : juce::String ("still idle"));
    }

    // -- frame noise and octave flicks must not reach the needle --
    {
        TunerFilter f;
        uint32_t seed = 0x2545f491u;
        float worst = 0.0f;
        bool lockedWrong = false;

        for (int tick = 0; tick < 120; ++tick)
        {
            seed = seed * 1664525u + 1013904223u;
            const float noise = (float) (int32_t) seed * 4.6566129e-10f * 0.08f;   // +-8 cents

            // Two consecutive octave flicks every half second — the worst
            // realistic tracker behaviour, and one frame short of a real move.
            const bool flick = (tick % 15) < 2 && tick > 0;
            const float m = (flick ? 45.0f : 33.0f) + noise;

            const auto r = f.push (hzOf (m), 0.9f, 0.2f);

            if (tick > 5)
            {
                worst = std::max (worst, std::abs (r.note - 33.0f) * 100.0f);
                lockedWrong = lockedWrong || r.locked != 33;
            }
        }

        report ("octave flicks never reach the display", ! lockedWrong && worst < 10.0f,
                "worst " + juce::String (worst, 1) + " cents from A1");
    }

    // -- a real note change snaps rather than glides --
    {
        TunerFilter f;
        int changedAt = -1;
        bool glided = false;

        for (int tick = 0; tick < 60; ++tick)
        {
            const float m = tick < 30 ? 33.0f : 38.0f;
            const auto r = f.push (hzOf (m), 0.9f, 0.2f);

            if (tick >= 30)
            {
                if (r.locked == 38 && changedAt < 0)
                    changedAt = tick - 30;

                if (r.locked != 33 && r.locked != 38)
                    glided = true;
            }
        }

        report ("a new note lands within 200 ms, no glide", ! glided && changedAt >= 0 && changedAt <= 6,
                changedAt < 0 ? juce::String ("never moved")
                              : juce::String (changedAt * 33) + " ms, straight to D2");
    }

    // -- vibrato reads as its centre, not as the wobble --
    {
        TunerFilter f;
        float worst = 0.0f;

        for (int tick = 0; tick < 90; ++tick)
        {
            const double t = tick / 30.0;
            const float m = 33.0f + 0.30f * (float) std::sin (6.283185307179586 * 5.0 * t);
            const auto r = f.push (hzOf (m), 0.9f, 0.2f);

            if (tick > 15)
                worst = std::max (worst, std::abs (r.note - 33.0f) * 100.0f);
        }

        report ("+-30 cent vibrato shows near its centre", worst < 10.0f,
                "worst " + juce::String (worst, 1) + " cents shown");
    }

    // -- silence holds briefly, then admits it --
    {
        TunerFilter f;

        for (int tick = 0; tick < 10; ++tick)
            f.push (hzOf (33.0f), 0.9f, 0.2f);

        bool heldAt10 = false, idleAt12 = false;

        for (int tick = 1; tick <= 12; ++tick)
        {
            const auto r = f.push (0.0f, 0.0f, 0.0f);

            if (tick == 10) heldAt10 = r.live && r.locked == 33;
            if (tick == 12) idleAt12 = ! r.live && r.locked < 0;
        }

        report ("holds ~400 ms through silence, then idles", heldAt10 && idleAt12,
                juce::String (heldAt10 ? "held" : "dropped early") + ", "
                  + juce::String (idleAt12 ? "then idle" : "then stuck"));
    }
}

static void testSegmenterOnRealBass (const juce::File& stem, double, int block)
{
    if (! stem.existsAsFile())
        return;

    std::printf ("\n MIDI segmentation on a real bass stem\n");

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (stem));

    if (reader == nullptr)
        return;

    const int n = (int) juce::jmin ((juce::int64) (reader->sampleRate * 60.0), reader->lengthInSamples);
    juce::AudioBuffer<float> buffer ((int) reader->numChannels, n);
    reader->read (&buffer, 0, n, 0, true, true);

    std::vector<float> in (buffer.getReadPointer (0), buffer.getReadPointer (0) + n);

    // The excerpt is cut mid-song; give the last note the silence it would
    // have had in the room, or it is stuck by construction rather than by bug.
    in.insert (in.end(), (size_t) (reader->sampleRate * 1.0), 0.0f);

    int stuck = 0;
    const auto notes = segmentNotes (in, reader->sampleRate, block, &stuck);

    double shortest = 1.0e9;
    int blips = 0;

    for (const auto& note : notes)
    {
        if (note.off > 0.0)
        {
            const double length = note.off - note.on;
            shortest = std::min (shortest, length);

            if (length < 0.08)
                ++blips;
        }
    }

    std::printf ("  %d notes over %.0f s, shortest %.2f s\n",
                 (int) notes.size(), n / reader->sampleRate,
                 notes.empty() ? 0.0 : shortest);

    report ("nothing stuck on real playing", stuck == 0, juce::String (stuck) + " stuck");
    report ("no chattered blips", blips == 0, juce::String (blips) + " notes under 80 ms");
}


// The whole feature rests on macOS letting us publish a virtual MIDI source.
// Assuming that works is exactly the kind of platform assumption that bites, so
// it is checked rather than believed.
static void testVirtualMidiPort()
{
    std::printf ("\n Virtual MIDI port\n");

    // MIDIServer garbage-collects dead clients lazily, and back-to-back
    // harness runs each leave one; a request landing mid-cleanup is refused.
    // The app's own retry is short because a launch is never that close to a
    // corpse — the harness waits the cleanup out.
    std::unique_ptr<juce::MidiOutput> port;
    int attempts = 0;

    for (; attempts < 8 && port == nullptr; ++attempts)
    {
        if (attempts > 0)
            juce::Thread::sleep (500);

        port = juce::MidiOutput::createNewDevice ("LiveRig test port");
    }

    report ("macOS publishes a virtual MIDI source", port != nullptr,
            port != nullptr ? "created and visible to other apps (attempt "
                                + juce::String (attempts) + ")"
                            : "createNewDevice refused 8 times over 3.5 s");

    if (port != nullptr)
    {
        port->sendMessageNow (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
        port->sendMessageNow (juce::MidiMessage::noteOff (1, 60));
        report ("it accepts messages without throwing", true, "note on and off sent");
    }
}


static juce::String pickEffect (const juce::StringArray& names,
                                std::initializer_list<const char*> preferred)
{
    for (auto* want : preferred)
        for (const auto& name : names)
            if (name.containsIgnoreCase (want))
                return name;

    return {};
}

// The registry behind every picker: one scan records instruments and effects
// both. First run on a machine pays for the scan; every run after reads the
// cache. The duration is printed because it is the number that decides
// whether "Rescan" needs a progress bar.
static void testPluginRegistry (double sr, int block)
{
    std::printf ("\n Plugin registry (AU + VST3)\n");

    juce::ScopedJuceInitialiser_GUI juceInit;

    InstrumentHost host;
    host.prepare (sr, block);

    if (host.getEffectNames().isEmpty())
    {
        std::printf ("  no cached plugin list — scanning this machine (one-time)...\n");
        std::fflush (stdout);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();

        host.beginScan();

        // Plain sleep is enough: the scan runs in child processes now, so
        // nothing foreign ever needs this process's message loop.
        while (host.isScanning())
            juce::Thread::sleep (100);

        std::printf ("  scan took %.1f s\n", (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0);
        std::fflush (stdout);
    }

    const auto effects = host.getEffectNames();
    const auto instruments = host.getAvailableNames();

    report ("finds effect plugins", effects.size() > 0,
            juce::String (effects.size()) + " effects, " + juce::String (instruments.size()) + " instruments");

    report ("Apple's stock effects are in the list",
            pickEffect (effects, { "AUDelay", "AUDistortion", "AUGraphicEQ" }).isNotEmpty(),
            "e.g. " + pickEffect (effects, { "AUDelay", "AUDistortion", "AUGraphicEQ" }));

    // A plugin shipped as AU and VST3 must appear once, not twice.
    juce::StringArray sorted (effects);
    sorted.sort (true);
    bool duplicated = false;

    for (int i = 1; i < sorted.size(); ++i)
        duplicated = duplicated || sorted[i].equalsIgnoreCase (sorted[i - 1]);

    report ("no duplicate names across formats", ! duplicated, duplicated ? "duplicates present" : "deduped");
}

// One hosted effect, end to end: load, audio through it, state round trip,
// unload back to a wire. This is the layer everything else stands on.
static void testEffectSlot (double sr, int block)
{
    std::printf ("\n Hosting an effect in a slot\n");

    juce::ScopedJuceInitialiser_GUI juceInit;

    InstrumentHost host;
    host.prepare (sr, block);

    const auto name = pickEffect (host.getEffectNames(),
                                  { "AUDistortion", "AUDelay", "AUGraphicEQ", "AULowpass" });

    if (name.isEmpty())
    {
        std::printf ("  no cached plugin list — run the registry test first\n");
        return;
    }

    EffectSlot slot ("test");
    slot.prepare (sr, block);

    juce::String error;
    const bool loaded = slot.load (host, name, error);

    report ((name + " loads into the slot").toRawUTF8(), loaded, loaded ? "instantiated" : error);

    if (! loaded)
        return;

    std::printf ("  latency %.1f ms, sidechain channels %d\n",
                 slot.getLatencyMs(), slot.getSidechainChannels());

    // --- the stereo path the vocal lane uses ---
    const int blocks = (int) (sr * 1.0) / block;
    std::vector<float> in ((size_t) block), outL ((size_t) block), outR ((size_t) block);
    double phase = 0.0;
    float stereoPeak = 0.0f;

    for (int i = 0; i < blocks; ++i)
    {
        for (int k = 0; k < block; ++k)
        {
            in[(size_t) k] = 0.25f * (float) std::sin (phase);
            phase += 6.283185307179586 * 220.0 / sr;
        }

        slot.process (in.data(), nullptr, outL.data(), outR.data(), block);

        if (i > blocks / 4)
            for (int k = 0; k < block; ++k)
                stereoPeak = std::max (stereoPeak, std::abs (outL[(size_t) k]));
    }

    report ("the stereo path sounds too", stereoPeak > 0.01f, "peak " + juce::String (stereoPeak, 3));

    // --- state survives a reload ---
    const auto state = slot.getStateBase64();
    slot.restore (host, name, state);

    report ("state round-trips through a reload", slot.isLoaded() && ! slot.hasFailed(),
            state.isEmpty() ? "stateless plugin, reloaded clean" : juce::String (state.length()) + " bytes recalled");

    // --- an emptied slot is silence, never a crash ---
    slot.unload();

    for (int k = 0; k < block; ++k)
        in[(size_t) k] = 0.1f * (float) k / (float) block;

    slot.process (in.data(), nullptr, outL.data(), outR.data(), block);

    bool silent = true;

    for (int k = 0; k < block; ++k)
        silent = silent && outL[(size_t) k] == 0.0f && outR[(size_t) k] == 0.0f;

    report ("an empty slot writes silence", silent, "clean after unload");
}

// The one nontrivial wiring: a vocoder with the mic on its main input and the
// bass tap on its sidechain. Runs against whatever vocoder this machine has —
// the roles are pickers, so TAL slots in the same way when installed.
static void testVocoderSidechain (double sr, int block)
{
    std::printf ("\n Vocoder sidechain routing\n");

    juce::ScopedJuceInitialiser_GUI juceInit;

    InstrumentHost host;
    host.prepare (sr, block);

    const auto name = pickEffect (host.getEffectNames(),
                                  { "TAL-Vocoder", "TAL Vocoder", "MVocoder", "OrangeVocoder" });

    if (name.isEmpty())
    {
        std::printf ("  no vocoder installed — install TAL-Vocoder 3 and rerun\n");
        return;
    }

    EffectSlot vocoder ("vocoder", true);
    vocoder.prepare (sr, block);

    juce::String error;
    const bool loaded = vocoder.load (host, name, error);

    report ((name + " loads").toRawUTF8(), loaded, loaded ? "instantiated" : error);

    if (! loaded)
        return;

    const int sc = vocoder.getSidechainChannels();
    report ("a carrier route was negotiated", true,
            sc > 0 ? "sidechain bus, " + juce::String (sc) + " channel(s)"
                   : "no sidechain bus — carrier on the right main channel");

    // Modulator: speech-shaped bursts of noise. Carrier: a bright saw.
    std::vector<float> mod ((size_t) block), car ((size_t) block),
                       outL ((size_t) block), outR ((size_t) block);

    uint32_t noise = 22222u;
    double sawPhase = 0.0;
    float peak = 0.0f;
    bool finite = true;

    const int blocks = (int) (sr * 2.0) / block;

    for (int i = 0; i < blocks; ++i)
    {
        const double t0 = i * (double) block / sr;
        const bool talking = std::fmod (t0, 0.5) < 0.3;

        for (int k = 0; k < block; ++k)
        {
            noise = noise * 1664525u + 1013904223u;
            mod[(size_t) k] = talking ? 0.3f * (float) (int32_t) noise * 4.6566129e-10f : 0.0f;

            sawPhase += 110.0 / sr;
            if (sawPhase >= 1.0) sawPhase -= 1.0;
            car[(size_t) k] = 0.3f * (float) (2.0 * sawPhase - 1.0);
        }

        vocoder.process (mod.data(), car.data(), outL.data(), outR.data(), block);

        for (int k = 0; k < block; ++k)
        {
            finite = finite && std::isfinite (outL[(size_t) k]);

            if (i > blocks / 4)
                peak = std::max (peak, std::abs (outL[(size_t) k]));
        }
    }

    report ("voice over carrier produces output", finite && peak > 0.001f,
            "peak " + juce::String (peak, 4) + " — intelligibility is judged by ear, plumbing by meter");
}

// The mic strip: one function with stage manners. Off is the microphone
// itself — a channel with no effects still plays — a lane switched on with
// no plugin is silence (and the row says why), the gate kills the breaths,
// and the switch is a crossfade a footswitch cannot click.
static void testVocalChannel (double sr, int block)
{
    std::printf ("\n Autotune lane (%.0f Hz, %d-sample blocks)\n", sr, block);

    juce::ScopedJuceInitialiser_GUI juceInit;

    InstrumentHost host;
    host.prepare (sr, block);

    ParamStore params;
    EffectSlot tune ("tune"), vocoderSlot ("vocoder", true);
    VocalChannel vocal (params, tune, vocoderSlot);

    tune.prepare (sr, block);
    vocoderSlot.prepare (sr, block);
    vocal.prepare (sr, block);

    double phase = 0.0;

    const auto run = [&] (float amplitude, double seconds, float* largestStep) -> float
    {
        std::vector<float> mic ((size_t) block), car ((size_t) block, 0.0f),
                           l ((size_t) block), r ((size_t) block);
        float peak = 0.0f, previous = 0.0f;

        const int blocks = (int) (sr * seconds) / block;

        for (int i = 0; i < blocks; ++i)
        {
            for (int k = 0; k < block; ++k)
            {
                mic[(size_t) k] = amplitude * (float) std::sin (phase);
                phase += 6.283185307179586 * 220.0 / sr;
            }

            std::fill (l.begin(), l.end(), 0.0f);
            std::fill (r.begin(), r.end(), 0.0f);
            vocal.process (mic.data(), car.data(), l.data(), r.data(), block);

            for (int k = 0; k < block; ++k)
            {
                if (i > blocks / 2)
                    peak = std::max (peak, std::abs (l[(size_t) k]));

                if (largestStep != nullptr)
                    *largestStep = std::max (*largestStep, std::abs (l[(size_t) k] - previous));

                previous = l[(size_t) k];
            }
        }

        return peak;
    };

    // Nothing switched on: the microphone is the output. This is the bug
    // Daniel found on stage — the strip used to be silent until Autotune.
    params.set (pVocalOn, 0.0f);
    const float offPeak = run (0.2f, 0.4, nullptr);
    report ("with no lane on, the mic plays clean", offPeak > 0.15f && offPeak < 0.25f,
            "peak " + juce::String (offPeak, 4) + " from a 0.2 mic");

    params.set (pVocalOn, 1.0f);
    const float unboundPeak = run (0.2f, 0.4, nullptr);
    report ("a lane on with no plugin takes the voice and is silent, not a crash",
            unboundPeak < 1.0e-6f, "peak " + juce::String (unboundPeak, 7));

    params.set (pVocalOn, 0.0f);
    params.set (pVocoderOn, 1.0f);
    const float vocoderUnbound = run (0.2f, 0.4, nullptr);
    params.set (pVocoderOn, 0.0f);
    params.set (pVocalOn, 1.0f);

    report ("the vocoder lane takes it over the same way", vocoderUnbound < 1.0e-6f,
            "peak " + juce::String (vocoderUnbound, 7));

    // The rest needs audio through the slot; any clean-passing effect will do.
    const auto name = pickEffect (host.getEffectNames(),
                                  { "AUGraphicEQ", "AUNBandEQ", "AUDelay", "AUDistortion" });

    if (name.isEmpty())
    {
        std::printf ("  no cached plugin list — run the registry test first\n");
        return;
    }

    juce::String error;

    if (! tune.load (host, name, error))
    {
        report ((name + " loads").toRawUTF8(), false, error);
        return;
    }

    const float onPeak = run (0.2f, 0.6, nullptr);
    report (("on, through " + name + ", the mic sounds").toRawUTF8(), onPeak > 0.02f,
            "peak " + juce::String (onPeak, 3));

    // The gate: a signal well under the threshold must die there, and the
    // real one must bring the lane back.
    const float quiet = run (0.001f, 0.8, nullptr);   // ~-60 dB against the -50 gate
    report ("the gate closes on the quiet", quiet < 0.0005f, "peak " + juce::String (quiet, 5));

    const float loudAgain = run (0.2f, 0.6, nullptr);
    report ("and reopens for the signal", loudAgain > 0.02f, "peak " + juce::String (loudAgain, 3));

    // The night-one bug: Gain pulled well down must make the lane quieter,
    // never starve the gate detector shut. The detector listens pre-fader.
    params.set (pVocalGain, -24.0f);
    const float trimmed = run (0.2f, 0.6, nullptr);
    params.set (pVocalGain, 0.0f);

    report ("gain trims the lane but cannot choke the gate",
            trimmed > 0.008f && trimmed < 0.05f, "peak " + juce::String (trimmed, 4) + " at -24 dB gain");

    // Toggle off and on over one continuous note: the largest sample step
    // must stay in sine territory. A hard mute would step by the amplitude.
    float step = 0.0f;
    run (0.2f, 0.3, &step);
    params.set (pVocalOn, 0.0f);
    run (0.2f, 0.3, &step);
    params.set (pVocalOn, 1.0f);
    run (0.2f, 0.3, &step);

    report ("the switch is a crossfade, not an edit", step < 0.04f,
            "largest step " + juce::String (step, 4));
}

// The acceptance question, answered in cents rather than vibes: the rig
// picks the machine's pitch corrector by itself, a flat tone goes in, and
// the output pitch is measured against true.
static void testAutotunePicksAndCorrects (double sr, int block)
{
    std::printf ("\n Autotune end to end\n");

    juce::ScopedJuceInitialiser_GUI juceInit;

    InstrumentHost host;
    host.prepare (sr, block);

    if (host.getEffectNames().isEmpty())
    {
        std::printf ("  no cached plugin list — run the registry test first\n");
        return;
    }

    const auto pick = host.preferredPitchPlugin();

    report ("a pitch corrector is picked automatically", pick.isNotEmpty(),
            pick.isEmpty() ? "none installed — Graillon 3 is the intended tenant" : pick);

    if (pick.isEmpty())
        return;

    EffectSlot slot ("tune");
    slot.prepare (sr, block);

    juce::String error;

    if (! slot.load (host, pick, error))
    {
        report ((pick + " loads").toRawUTF8(), false, error);
        return;
    }

    // 30 cents flat of A3. The corrector should pull it toward true.
    const double inHz = 220.0 * std::pow (2.0, -30.0 / 1200.0);

    const int total = (int) (sr * 3.0);
    std::vector<float> in ((size_t) block), outL ((size_t) block), outR ((size_t) block);
    std::vector<float> tail;
    double phase = 0.0;

    for (int i = 0; i + block <= total; i += block)
    {
        for (int k = 0; k < block; ++k)
        {
            in[(size_t) k] = 0.3f * (float) std::sin (phase);
            phase += 6.283185307179586 * inHz / sr;
        }

        slot.process (in.data(), nullptr, outL.data(), outR.data(), block);

        if (i > total / 2)
            for (int k = 0; k < block; ++k)
                tail.push_back (outL[(size_t) k]);
    }

    const double got = estimateF0 (tail.data(), (int) tail.size(), sr, 100.0, 500.0);
    const double outCents = cents (got, 220.0);

    report ((pick + " pulls a flat tone toward true").toRawUTF8(),
            std::abs (outCents) < 15.0 && std::abs (outCents) < 30.0 * 0.66,
            "in -30.0 cents, out " + juce::String (outCents, 1) + " cents ("
              + juce::String (got, 2) + " Hz)");
}

// The acceptance sentence made measurable: speak over a played bassline and
// out comes singing bass. Intelligibility is a human call, but its two
// physical halves are not — the output must follow the VOICE's envelope
// (a vocoder passes nothing when nobody talks, however loud the bass runs),
// and it must ride the BASS's pitch (the words come out at the carrier's
// note, not the voice's). Runs through the whole mic lane, gate included,
// with whatever vocoder the registry picks.
static void testVocoderSings (double sr, int block)
{
    std::printf ("\n Vocoder end to end: the bass sings\n");

    juce::ScopedJuceInitialiser_GUI juceInit;

    InstrumentHost host;
    host.prepare (sr, block);

    if (host.getEffectNames().isEmpty())
    {
        std::printf ("  no cached plugin list — run the registry test first\n");
        return;
    }

    const auto pick = host.preferredVocoderPlugin();

    report ("a vocoder is picked automatically", pick.isNotEmpty(),
            pick.isEmpty() ? "none installed — TAL-Vocoder 3 is the intended tenant" : pick);

    if (pick.isEmpty())
        return;

    ParamStore params;
    EffectSlot tune ("tune"), vocoder ("vocoder", true);
    VocalChannel vocal (params, tune, vocoder);

    tune.prepare (sr, block);
    vocoder.prepare (sr, block);
    vocal.prepare (sr, block);

    juce::String error;

    if (! vocoder.load (host, pick, error))
    {
        report ((pick + " loads").toRawUTF8(), false, error);
        return;
    }

    std::printf ("  %s: %.1f ms latency, %s\n", pick.toRawUTF8(), vocoder.getLatencyMs(),
                 vocoder.getSidechainChannels() > 0 ? "carrier on the sidechain"
                                                    : "carrier on the right main channel");

    params.set (pVocoderOn, 1.0f);

    // Half-second bursts of speech-shaped noise, half a second of silence,
    // over a saw bass that never stops.
    const double cycle = 1.0, talk = 0.5;
    const int total = (int) (sr * 6.0);

    std::vector<float> mic ((size_t) block), car ((size_t) block),
                       l ((size_t) block), r ((size_t) block);
    std::vector<float> talkTail;

    uint32_t noise = 77777u;
    double sawPhase = 0.0;
    double talkEnergy = 0.0, quietEnergy = 0.0;
    long talkCount = 0, quietCount = 0;

    for (int i = 0; i + block <= total; i += block)
    {
        const double t0 = i / sr;

        for (int k = 0; k < block; ++k)
        {
            const double t = t0 + k / sr;
            const bool talking = std::fmod (t, cycle) < talk;

            noise = noise * 1664525u + 1013904223u;
            mic[(size_t) k] = talking ? 0.25f * (float) (int32_t) noise * 4.6566129e-10f : 0.0f;

            sawPhase += 110.0 / sr;
            if (sawPhase >= 1.0) sawPhase -= 1.0;
            car[(size_t) k] = 0.3f * (float) (2.0 * sawPhase - 1.0);
        }

        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        vocal.process (mic.data(), car.data(), l.data(), r.data(), block);

        if (i < (int) sr)   // let ramps settle and the plugin wake
            continue;

        for (int k = 0; k < block; ++k)
        {
            const double phase = std::fmod (t0 + k / sr, cycle);
            const double v = l[(size_t) k];

            // Guard bands around the edges absorb the plugin's latency on
            // the way in and its band release on the way out.
            if (phase > 0.15 && phase < talk - 0.02)
            {
                talkEnergy += v * v;
                ++talkCount;

                if (talkTail.size() < (size_t) sr)
                    talkTail.push_back (l[(size_t) k]);
            }
            else if (phase > talk + 0.25 && phase < cycle - 0.02)
            {
                quietEnergy += v * v;
                ++quietCount;
            }
        }
    }

    const double talkRms = std::sqrt (talkEnergy / (double) std::max (1L, talkCount));
    const double quietRms = std::sqrt (quietEnergy / (double) std::max (1L, quietCount));

    report ("talking over the bassline makes sound", talkRms > 0.01,
            "rms " + juce::String (talkRms, 4));

    report ("a silent mic silences the bass's voice", quietRms < talkRms * 0.25,
            "talk rms " + juce::String (talkRms, 4) + " vs quiet " + juce::String (quietRms, 5));

    const double got = estimateF0 (talkTail.data(), (int) talkTail.size(), sr, 60.0, 400.0);

    report ("the words come out at the bass's pitch", std::abs (cents (got, 110.0)) < 100.0,
            juce::String (got, 1) + " Hz against the 110 Hz carrier");
}

// The carrier tap: the bass after every movable stage, alive even when MIDI
// thru has muted the lane — the exact configuration where the bass sings.
static void testCarrierTap (double sr, int block)
{
    std::printf ("\n Vocoder carrier tap (%.0f Hz, %d-sample blocks)\n", sr, block);

    ParamStore params;
    FxChain chain (params);
    chain.prepare (sr, block);

    std::vector<float> mono ((size_t) block), l ((size_t) block), r ((size_t) block);
    double phase = 0.0;
    float carrierPeak = 0.0f, outPeak = 0.0f, diff = 0.0f;

    const int blocks = (int) (sr * 0.5) / block;

    for (int i = 0; i < blocks; ++i)
    {
        for (int k = 0; k < block; ++k)
        {
            mono[(size_t) k] = 0.3f * (float) std::sin (phase);
            phase += 6.283185307179586 * 110.0 / sr;
        }

        chain.process (mono.data(), l.data(), r.data(), block);

        if (i > blocks / 2)
        {
            const float* carrier = chain.getCarrier();

            for (int k = 0; k < block; ++k)
            {
                carrierPeak = std::max (carrierPeak, std::abs (carrier[k]));
                diff = std::max (diff, std::abs (carrier[k] - l[(size_t) k]));
            }
        }
    }

    report ("with everything bypassed the tap is the lane", diff < 1.0e-4f,
            "largest carrier/output difference " + juce::String (diff, 6));

    // MIDI on, thru off: the lane leaves the mix, the carrier must not.
    params.set (pMidiOn, 1.0f);
    carrierPeak = 0.0f;

    for (int i = 0; i < blocks; ++i)
    {
        for (int k = 0; k < block; ++k)
        {
            mono[(size_t) k] = 0.3f * (float) std::sin (phase);
            phase += 6.283185307179586 * 110.0 / sr;
        }

        chain.process (mono.data(), l.data(), r.data(), block);

        if (i > blocks / 2)
        {
            const float* carrier = chain.getCarrier();

            for (int k = 0; k < block; ++k)
            {
                carrierPeak = std::max (carrierPeak, std::abs (carrier[k]));
                outPeak = std::max (outPeak, std::abs (l[(size_t) k]));
            }
        }
    }

    report ("the carrier survives a muted live lane", carrierPeak > 0.2f && outPeak < 1.0e-4f,
            "carrier " + juce::String (carrierPeak, 3) + ", lane " + juce::String (outPeak, 6));
}


// The whole point of hosting is that a real instrument makes a real sound from
// the notes the tracker produced. Discovery is not the claim; audio is.
static void testInstrumentHosting (const juce::String& wanted, double sr, int block,
                                   bool mustSound = false)
{
    std::printf ("\n Hosting an AU instrument\n");

    juce::ScopedJuceInitialiser_GUI juceInit;

    InstrumentHost host;
    host.prepare (sr, block);

    const auto names = host.getAvailableNames();

    if (names.isEmpty())
    {
        std::printf ("  no cached instrument list — run the app once to scan\n");
        return;
    }

    report ("finds instruments on this machine", names.size() > 0,
            juce::String (names.size()) + ": " + names.joinIntoString (", "));

    if (! names.contains (wanted))
    {
        report ((wanted + " is present").toRawUTF8(), false, "not in the list");
        return;
    }

    juce::String error;
    const bool loaded = host.load (wanted, error);

    report ((wanted + " loads").toRawUTF8(), loaded, loaded ? "instantiated" : error);

    if (! loaded)
        return;

    if (auto* plugin = host.getLiveInstrument())
        std::printf ("  buses: %d in / %d out, latency %d, tail %.1f s\n",
                     plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels(),
                     plugin->getLatencySamples(), plugin->getTailLengthSeconds());

    std::vector<float> left ((size_t) block, 0.0f), right ((size_t) block, 0.0f);

    NoteEvent on { NoteEvent::noteOn, 60, 100 };
    double peak = 0.0;

    const int blocks = (int) (sr * 2.0) / block;

    for (int i = 0; i < blocks; ++i)
    {
        std::fill (left.begin(), left.end(), 0.0f);
        std::fill (right.begin(), right.end(), 0.0f);

        // The note goes in on the first block and is held for the rest.
        host.process (i == 0 ? &on : nullptr, i == 0 ? 1 : 0,
                      left.data(), right.data(), block, 1.0f);

        if (i > blocks / 8)   // let the attack start
            for (int k = 0; k < block; ++k)
                peak = std::max (peak, (double) std::abs (left[(size_t) k]));
    }

    if (mustSound)
    {
        report ((wanted + " makes a sound from a note").toRawUTF8(), peak > 0.0005,
                "peak " + juce::String (peak, 5) + " from one held note");
    }
    else
    {
        // Whether a third-party instrument sounds straight out of the box is
        // its business, not the host's — most sample libraries start with no
        // patch loaded. The control above is what proves the plumbing.
        std::printf ("  %s peak %.5f from one held note%s\n", wanted.toRawUTF8(), peak,
                     peak > 0.0005 ? "" : "  (silent — likely needs a patch chosen in its own window)");
    }

    host.unload();
}

//==============================================================================
//==============================================================================
// The parameter store's channel slots: copies are independent, globals are one
// value everywhere, the one-argument view is channel 0, and the settings shape
// still reads in an old build's flat form.
//==============================================================================
// The Freeze pedal held the way a foot holds it: its ON goes on at one block
// and off at another, and the lane comes back whole. presses is a flat list of
// sample positions — on, off, on, off — and a missing last "off" never lets go.
static std::vector<float> renderFreeze (ParamStore& params, const std::vector<float>& in,
                                        double sr, int block, const std::vector<int>& presses,
                                        double* worstMicros = nullptr)
{
    FxChain chain (params);
    chain.prepare (sr, block);

    std::vector<float> mono ((size_t) block), left ((size_t) block), right ((size_t) block);
    std::vector<float> out;
    out.reserve (in.size());

    for (int i = 0; i + block <= (int) in.size(); i += block)
    {
        bool down = false;

        for (int at : presses)
            if (i >= at)
                down = ! down;

        params.set (pFreezeOn, down ? 1.0f : 0.0f);

        std::copy (in.begin() + i, in.begin() + i + block, mono.begin());

        const auto t0 = std::chrono::steady_clock::now();
        {
            juce::ScopedNoDenormals noDenormals;   // as the audio callback runs
            chain.process (mono.data(), left.data(), right.data(), block);
        }
        const auto t1 = std::chrono::steady_clock::now();

        if (worstMicros != nullptr && i > block * 8)
            *worstMicros = juce::jmax (*worstMicros,
                                       std::chrono::duration<double, std::micro> (t1 - t0).count());

        for (int k = 0; k < block; ++k)
            out.push_back (left[(size_t) k]);
    }

    params.set (pFreezeOn, 0.0f);
    return out;
}

static double largestStep (const std::vector<float>& x, int from, int to)
{
    double step = 0.0;

    for (int i = juce::jmax (1, from); i < juce::jmin (to, (int) x.size()); ++i)
        step = juce::jmax (step, (double) std::abs (x[(size_t) i] - x[(size_t) (i - 1)]));

    return step;
}

// How far the loudness wanders across a stretch, in dB, judged over windows
// of whole cycles — a looped slice pulses at its seam and shows up here.
static double levelWander (const std::vector<float>& x, int from, int to, int window)
{
    double lo = 1.0e30, hi = 0.0;

    for (int i = from; i + window <= to; i += window)
    {
        const double r = rmsOf (x.data() + i, window);
        lo = juce::jmin (lo, r);
        hi = juce::jmax (hi, r);
    }

    return 20.0 * std::log10 (hi / (lo + 1.0e-12));
}

static void testFreeze (double sr, int block)
{
    std::printf ("\nFreeze  (%.0f Hz, %d-sample blocks)\n", sr, block);

    const auto at = [sr, block] (double seconds) { return ((int) (sr * seconds) / block) * block; };
    const double db = 20.0;

    // Before a hold and after one has died away, the pedal is a straight wire.
    {
        std::vector<float> note;
        makeNote (note, sr, 55.0, 4.0, 0.4f);

        ParamStore never, once;
        once.set (pFreezeFadeOut, 200.0f);

        const auto plain = renderChain (never, note, sr, block);
        const auto used = renderFreeze (once, note, sr, block, { at (0.5), at (1.5) });

        const int clear = at (2.0);
        const bool before = std::equal (plain.begin(), plain.begin() + at (0.5), used.begin());
        const bool after = std::equal (plain.begin() + clear, plain.end(), used.begin() + clear);
        const bool between = ! std::equal (plain.begin() + at (0.8), plain.begin() + at (1.4), used.begin() + at (0.8));

        report ("a straight wire before a hold and after it has gone", before && after && between,
                "sample for sample the same as a board without the pedal, except while it holds");
    }

    // A steady tone, held, with the playing stopped: the layer alone has to
    // be the same note at the same level, and stay there.
    {
        std::vector<float> in;
        makeSine (in, sr, 110.0, 6.0, 0.2f);

        const int press = at (1.0), quiet = at (1.2);

        for (int i = quiet; i < (int) in.size(); ++i)
            in[(size_t) i] *= (float) std::exp (-(double) (i - quiet) / (sr * 0.01));

        ParamStore params;
        params.set (pFreezeMotion, 0.0f);

        const auto out = renderFreeze (params, in, sr, block, { press });

        const int from = at (2.5), len = at (3.0);
        const double level = db * std::log10 (rmsOf (out.data() + from, len) / (0.2 / std::sqrt (2.0)));
        const double pitch = cents (estimateF0 (out.data() + from, at (0.5), sr, 40.0, 900.0), 110.0);
        const double wander = levelWander (out, from, from + len, (int) (sr / 110.0 * 6.0));

        report ("a held tone keeps its level", std::abs (level) < 0.5,
                juce::String (level, 2) + " dB against what was played");
        report ("a held tone keeps its pitch", std::abs (pitch) < 3.0,
                juce::String (pitch, 2) + " cents");
        report ("no seam: the level does not pulse", wander < 0.2,
                juce::String (wander, 3) + " dB of wander across three seconds");

        // While the string still rings, the layer comes in under it in phase.
        std::vector<float> ringing;
        makeSine (ringing, sr, 110.0, 3.0, 0.2f);
        const auto both = renderFreeze (params, ringing, sr, block, { press });
        const double sum = db * std::log10 (rmsOf (both.data() + at (2.0), at (0.5)) / (0.2 / std::sqrt (2.0)));

        report ("the layer joins the ringing note in phase", std::abs (sum - 6.02) < 0.3,
                juce::String (sum, 2) + " dB — two of the same note, no comb");
    }

    // The thing it is for: a plucked low E, caught mid-decay, with every
    // harmonic where it was.
    {
        const double f0 = 41.2;
        std::vector<float> in;
        makeNote (in, sr, f0, 5.0, 0.4f);

        const int press = at (0.7), quiet = at (0.9);

        for (int i = quiet; i < (int) in.size(); ++i)
            in[(size_t) i] *= (float) std::exp (-(double) (i - quiet) / (sr * 0.01));

        ParamStore params;
        params.set (pFreezeMotion, 0.0f);

        const auto out = renderFreeze (params, in, sr, block, { press });

        const int from = at (2.0), len = at (2.5);
        const int before = press - (int) (sr * 0.17);
        const double pitch = cents (estimateF0 (out.data() + from, at (0.6), sr, 30.0, 400.0), f0);
        const double level = db * std::log10 (rmsOf (out.data() + from, len)
                                                / rmsOf (in.data() + before, press - before));

        // makeNote gives harmonic h an amplitude of h^-1.35 under one shared
        // envelope, so the balance that was played is known exactly. A held
        // partial is a few bins turning at nearly the same rate, so it is
        // weighed as the energy around its harmonic — a Hann-windowed
        // quarter second, averaged along the hold — not as one spectral line.
        const auto partial = [&] (int h)
        {
            const int win = (int) (sr * 0.25);
            const double w = 6.283185307179586 * f0 * h / sr;
            double energy = 0.0;
            int count = 0;

            for (int start = from; start + win <= from + len; start += win / 2, ++count)
            {
                double re = 0.0, im = 0.0;

                for (int i = 0; i < win; ++i)
                {
                    const double hann = 0.5 - 0.5 * std::cos (6.283185307179586 * i / win);
                    re += out[(size_t) (start + i)] * hann * std::cos (w * i);
                    im += out[(size_t) (start + i)] * hann * std::sin (w * i);
                }

                energy += re * re + im * im;
            }

            return std::sqrt (energy / juce::jmax (1, count));
        };

        double worstHarmonic = 0.0;

        for (int h = 2; h <= 5; ++h)
            worstHarmonic = juce::jmax (worstHarmonic,
                                        std::abs (db * std::log10 (partial (h) / partial (1) * std::pow ((double) h, 1.35))));

        report ("a low E holds on its pitch", std::abs (pitch) < 6.0, juce::String (pitch, 2) + " cents");
        report ("it holds at the level it was caught", std::abs (level) < 1.0,
                juce::String (level, 2) + " dB against the last 170 ms before the press");
        report ("its harmonics keep their balance", worstHarmonic < 1.5,
                "worst of harmonics 2-5 off by " + juce::String (worstHarmonic, 2) + " dB");
        report ("the held bass does not pulse", levelWander (out, from, from + len, (int) (sr / f0 * 4.0)) < 0.5,
                juce::String (levelWander (out, from, from + len, (int) (sr / f0 * 4.0)), 3) + " dB of wander");
    }

    // Playing over it, then letting go.
    {
        std::vector<float> in, over;
        makeSine (in, sr, 110.0, 6.0, 0.2f);
        makeSine (over, sr, 330.0, 6.0, 0.1f);

        const int press = at (1.0), change = at (1.5), release = at (3.5);

        for (int i = 0; i < (int) in.size(); ++i)
        {
            const float x = juce::jlimit (0.0f, 1.0f, (float) (i - change) / (float) (sr * 0.02));
            in[(size_t) i] = in[(size_t) i] * (1.0f - x) + over[(size_t) i] * x;
        }

        ParamStore params;
        params.set (pFreezeMotion, 0.0f);
        params.set (pFreezeFadeOut, 200.0f);

        const auto out = renderFreeze (params, in, sr, block, { press, release });

        const int from = at (2.2), len = at (1.0);
        const double held = goertzel (out.data() + from, len, sr, 110.0);
        const double live = goertzel (out.data() + from, len, sr, 330.0);

        report ("the held note stays under new playing",
                std::abs (db * std::log10 (held / 0.2)) < 0.5 && std::abs (db * std::log10 (live / 0.1)) < 0.5,
                "held " + juce::String (db * std::log10 (held / 0.2), 2) + " dB, live "
                    + juce::String (db * std::log10 (live / 0.1), 2) + " dB");

        const int after = release + (int) (sr * 0.2) + block * 2;
        const double left = goertzel (out.data() + after, at (1.0), sr, 110.0);
        const double halfway = goertzel (out.data() + release + (int) (sr * 0.08), (int) (sr * 0.04), sr, 110.0);

        report ("letting go fades it out in the time set", left < 0.2 * 1.0e-4 && halfway > 0.2 * 0.3 && halfway < 0.2 * 0.75,
                juce::String (db * std::log10 (halfway / 0.2), 1) + " dB halfway, "
                    + juce::String (db * std::log10 ((left + 1.0e-12) / 0.2), 0) + " dB after 200 ms");

        // The steepest thing in the render should be the notes themselves.
        const double natural = 0.2 * 6.283185307179586 * 110.0 / sr * 2.0 + 0.1 * 6.283185307179586 * 330.0 / sr;
        const double step = largestStep (out, at (0.5), (int) out.size());

        report ("no click going down or coming up", step < natural * 1.25,
                "largest step " + juce::String (step, 5) + " vs " + juce::String (natural, 5) + " for the notes alone");
    }

    // The report that found the bug: a second of Fade out set, the pedal
    // switched off by a stomp, and the hold cut dead. Off is letting go.
    {
        std::vector<float> in;
        makeSine (in, sr, 110.0, 6.0, 0.2f);

        const int quiet = at (1.2), off = at (3.0);

        for (int i = quiet; i < (int) in.size(); ++i)
            in[(size_t) i] *= (float) std::exp (-(double) (i - quiet) / (sr * 0.01));

        ParamStore params;
        params.set (pFreezeMotion, 0.0f);
        params.set (pFreezeLevel, 0.34f);
        params.set (pFreezeFadeIn, 1000.0f);
        params.set (pFreezeFadeOut, 1000.0f);

        const auto out = renderFreeze (params, in, sr, block, { at (1.0), off });
        const auto levelAt = [&] (double secondsAfterOff)
        {
            return goertzel (out.data() + off + (int) (sr * (secondsAfterOff - 0.03)), (int) (sr * 0.06), sr, 110.0)
                     / (0.2 * 0.34);
        };

        report ("switching the pedal off takes the whole Fade out",
                std::abs (levelAt (0.25) - 0.75) < 0.05 && std::abs (levelAt (0.5) - 0.5) < 0.05
                  && std::abs (levelAt (0.75) - 0.25) < 0.05 && levelAt (1.2) < 1.0e-3,
                "a second set: " + juce::String (levelAt (0.25), 2) + " at 250 ms, " + juce::String (levelAt (0.5), 2)
                    + " at 500, " + juce::String (levelAt (0.75), 2) + " at 750, gone after");
    }

    // A new press while the last hold is still dying takes the other layer;
    // a third inside the same fade steals one. Neither may click, and the
    // last thing caught is the thing left sounding.
    {
        const double tones[] { 110.0, 165.0, 247.5 };
        const int total = at (7.0);
        const double starts[] { 0.0, 1.25, 1.72, 2.4 };
        std::vector<float> in ((size_t) total, 0.0f);

        for (int i = 0; i < total; ++i)
        {
            const double t = i / sr;

            for (int n = 0; n < 3; ++n)
            {
                const double env = juce::jlimit (0.0, 1.0, (t - starts[n]) / 0.02)
                                     * juce::jlimit (0.0, 1.0, (starts[n + 1] + 0.02 - t) / 0.02);
                in[(size_t) i] += (float) (env * 0.2 * std::sin (6.283185307179586 * tones[n] * t));
            }
        }

        ParamStore params;
        params.set (pFreezeMotion, 0.0f);
        params.set (pFreezeFadeOut, 1000.0f);

        // Down on the first tone, up; down on the second, up; down on the
        // third while both earlier holds are still dying — a steal.
        // Timed five times and the best kept: one slow block is the
        // scheduler, the same slow block five times is the code.
        double worst = 1.0e30;
        std::vector<float> out;

        for (int run = 0; run < 5; ++run)
        {
            double thisRun = 0.0;
            out = renderFreeze (params, in, sr, block,
                                { at (1.2), at (1.5), at (1.7), at (1.8), at (1.98) }, &thisRun);
            worst = juce::jmin (worst, thisRun);
        }

        const double budget = block * 1000000.0 / sr;
        report ("a press costs less than half a block", worst < budget * 0.5,
                juce::String (worst, 1) + " us worst block across three presses, budget "
                    + juce::String (budget, 1) + " us");

        bool finite = true;
        for (float v : out)
            finite = finite && std::isfinite (v);

        const int from = at (4.5), len = at (2.0);
        const double last = goertzel (out.data() + from, len, sr, tones[2]);
        const double ghosts = juce::jmax (goertzel (out.data() + from, len, sr, tones[0]),
                                          goertzel (out.data() + from, len, sr, tones[1]));

        report ("three presses inside one fade leave only the last",
                finite && std::abs (db * std::log10 (last / 0.2)) < 0.5 && ghosts < 0.2 * 1.0e-3,
                "last " + juce::String (db * std::log10 (last / 0.2), 2) + " dB, earlier holds "
                    + juce::String (db * std::log10 ((ghosts + 1.0e-12) / 0.2), 0) + " dB");

        const double natural = 3.0 * 0.2 * 6.283185307179586 * tones[2] / sr;
        const double step = largestStep (out, at (1.0), (int) out.size());

        report ("re-pressing never clicks", step < natural,
                "largest step " + juce::String (step, 5) + " vs " + juce::String (natural, 5) + " allowed");
    }

    // Motion moves the held layer and nothing else, and leaves the lows be.
    {
        std::vector<float> in;
        makeSine (in, sr, 660.0, 8.0, 0.2f);

        const int press = at (1.0), quiet = at (1.2);

        for (int i = quiet; i < (int) in.size(); ++i)
            in[(size_t) i] *= (float) std::exp (-(double) (i - quiet) / (sr * 0.01));

        ParamStore params;
        params.set (pFreezeMotion, 1.0f);

        const auto moving = renderFreeze (params, in, sr, block, { press });
        const int from = at (2.0), len = at (5.5);
        const double wander = levelWander (moving, from, from + len, (int) (sr * 0.05));
        const double level = db * std::log10 (rmsOf (moving.data() + from, len) / (0.2 / std::sqrt (2.0)));

        std::vector<float> low;
        makeSine (low, sr, 55.0, 8.0, 0.2f);

        for (int i = quiet; i < (int) low.size(); ++i)
            low[(size_t) i] *= (float) std::exp (-(double) (i - quiet) / (sr * 0.01));

        const auto lows = renderFreeze (params, low, sr, block, { press });
        const double lowWander = levelWander (lows, from, from + len, (int) (sr / 55.0 * 6.0));

        report ("motion makes the held layer drift", wander > 1.0 && std::abs (level) < 3.0,
                juce::String (wander, 2) + " dB of movement at 660 Hz, mean level " + juce::String (level, 2) + " dB");
        report ("motion leaves a low note steady", lowWander < 1.0,
                juce::String (lowWander, 3) + " dB of movement at 55 Hz");
    }

    // A hold is a foot, not a setting: no file remembers the pedal lit.
    {
        ParamStore store;
        store.set (pFreezeOn, 1.0f);
        store.set (2, pFreezeOn, 1.0f);
        store.set (pFreezeFadeOut, 640.0f);

        ParamStore loaded;
        loaded.fromVar (store.toVar());

        report ("a held foot is never written to the settings",
                ! loaded.getBool (pFreezeOn) && ! loaded.getBool (2, pFreezeOn)
                  && loaded.get (pFreezeFadeOut) == 640.0f,
                "the fades come back, the hold does not");
    }
}

// The same pedal on real playing. A held layer is isolated as the held
// render minus the same render never pressed.
//
// Two feet are tried. A careless one comes down every three seconds wherever
// that lands — mid-note, on an attack, across a change — and whatever it
// catches has to come out steady. A deliberate one comes down on long notes,
// from right with the note to well after it, and has to catch the note at
// the level it was played.
static void testFreezeOnRealBass (const juce::File& stem, int block)
{
    if (! stem.existsAsFile())
        return;

    std::printf ("\nFreeze on a real bass stem\n");

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (stem));

    if (reader == nullptr)
        return;

    const double sr = reader->sampleRate;
    const int n = (int) juce::jmin ((juce::int64) (sr * 90.0), reader->lengthInSamples);
    juce::AudioBuffer<float> buffer ((int) reader->numChannels, n);
    reader->read (&buffer, 0, n, 0, true, true);

    const std::vector<float> in (buffer.getReadPointer (0), buffer.getReadPointer (0) + n);
    const auto at = [sr, block] (double seconds) { return ((int) (sr * seconds) / block) * block; };

    ParamStore params;
    params.set (pFreezeMotion, 0.0f);
    params.set (pFreezeFadeIn, 0.0f);
    params.set (pFreezeFadeOut, 0.0f);

    const auto plain = renderFreeze (params, in, sr, block, {});

    const auto layerFor = [&] (const std::vector<int>& presses)
    {
        auto layer = renderFreeze (params, in, sr, block, presses);

        for (size_t i = 0; i < layer.size(); ++i)
            layer[i] -= plain[i];

        return layer;
    };

    const auto pct = [] (std::vector<double> v, double q)
    {
        std::sort (v.begin(), v.end());
        return v.empty() ? 0.0 : v[(size_t) ((double) (v.size() - 1) * q)];
    };

    // The careless foot.
    {
        const int hop = sr > 50000.0 ? 4096 : 2048;
        std::vector<double> flutters, wanders;
        bool finite = true;

        for (double offset : { 0.0, 0.71, 1.52, 2.31 })
        {
            std::vector<int> presses;

            for (double t = 3.0 + offset; t + 3.0 < n / sr; t += 3.0)
            {
                presses.push_back (at (t));
                presses.push_back (at (t + 2.5));
            }

            const auto layer = layerFor (presses);

            for (float v : layer)
                finite = finite && std::isfinite (v);

            for (size_t h = 0; h < presses.size(); h += 2)
            {
                const int from = presses[h] + at (0.6), to = presses[h] + at (2.4);

                if (rmsOf (layer.data() + from, to - from) < 1.0e-3)
                    continue;   // it came down in a rest

                // A frame that repeats itself shows as loudness that rises
                // and falls once per hop: fold the layer's energy onto one
                // hop and see how uneven it is.
                std::vector<double> fold ((size_t) hop, 0.0);

                for (int i = from; i + hop <= to; i += hop)
                    for (int j = 0; j < hop; ++j)
                        fold[(size_t) j] += (double) layer[(size_t) (i + j)] * layer[(size_t) (i + j)];

                double lo = 1.0e30, hi = 0.0;

                for (int j = 0; j < hop; j += 64)
                {
                    double e = 0.0;

                    for (int k = 0; k < hop / 4; ++k)
                        e += fold[(size_t) ((j + k) % hop)];

                    lo = juce::jmin (lo, e);
                    hi = juce::jmax (hi, e);
                }

                flutters.push_back (10.0 * std::log10 (hi / (lo + 1.0e-30)));
                wanders.push_back (levelWander (layer, from, to, (int) (sr * 0.2)));
            }
        }

        report ("whatever a careless foot catches is steady",
                finite && flutters.size() > 20 && pct (flutters, 0.9) < 1.0 && pct (wanders, 0.9) < 0.5,
                juce::String ((int) flutters.size()) + " holds: frame-rate flutter "
                    + juce::String (pct (flutters, 0.5), 2) + " dB typical, " + juce::String (pct (flutters, 0.9), 2)
                    + " dB at the 90th; slow wander " + juce::String (pct (wanders, 0.9), 2) + " dB at the 90th");
    }

    // The deliberate foot. The tracker's note-on is itself some tens of
    // milliseconds after the pick, so "with the note" here is a foot landing
    // about where a player's would.
    {
        std::vector<double> onsets;
        double last = -10.0;

        for (const auto& note : segmentNotes (in, sr, block))
            if (note.off - note.on > 0.45 && note.on - last > 2.2 && note.on > 1.0 && note.on + 2.5 < n / sr)
            {
                onsets.push_back (note.on);
                last = note.on;
            }

        bool caught = onsets.size() > 8;
        juce::String detail;

        for (double after : { -0.03, 0.06, 0.15 })
        {
            std::vector<int> presses;

            for (double on : onsets)
            {
                presses.push_back (at (on + after));
                presses.push_back (at (on + after + 1.6));
            }

            const auto layer = layerFor (presses);
            std::vector<double> levels;

            for (size_t h = 0; h < onsets.size(); ++h)
            {
                const int on = at (onsets[h]);
                const double played = rmsOf (plain.data() + on + (int) (sr * 0.02), (int) (sr * 0.2));
                const int from = presses[h * 2] + at (0.6), to = presses[h * 2] + at (1.5);

                levels.push_back (20.0 * std::log10 ((rmsOf (layer.data() + from, to - from) + 1.0e-9) / played));
            }

            caught = caught && std::abs (pct (levels, 0.5)) < 1.5 && pct (levels, 0.1) > -5.0;
            detail << juce::String (after * 1000.0, 0) << " ms: " << juce::String (pct (levels, 0.5), 1)
                   << " dB typical, " << juce::String (pct (levels, 0.1), 1) << " dB at worst tenth; ";
        }

        report ("a foot landing with the note or after it catches the note", caught,
                juce::String ((int) onsets.size()) + " long notes — " + detail.trimEnd());
    }
}

static void testChannelParams()
{
    std::printf ("\nChannel parameter store\n");

    ParamStore store;

    store.set (2, pCompThresh, -30.0f);
    report ("channel copies are independent",
            store.get (2, pCompThresh) == -30.0f
              && store.get (0, pCompThresh) == spec (pCompThresh).defaultValue
              && store.get (7, pCompThresh) == spec (pCompThresh).defaultValue,
            "slot 2 moved alone");

    store.set (pDriveAmount, 20.0f);
    report ("legacy access is channel 0",
            store.get (0, pDriveAmount) == 20.0f && store.get (pDriveAmount) == 20.0f,
            "one-argument get/set land on slot 0");

    store.set (5, pTrackGain, -6.0f);
    report ("globals are one value",
            store.get (0, pTrackGain) == -6.0f && store.get (3, pTrackGain) == -6.0f,
            "track gain shared across slots");

    store.set (3, pRevMix, 0.8f);
    const auto saved = store.toVar();

    ParamStore loaded;
    loaded.fromVar (saved);
    report ("slots survive the settings round trip",
            loaded.get (2, pCompThresh) == -30.0f
              && loaded.get (3, pRevMix) == 0.8f
              && loaded.get (0, pDriveAmount) == 20.0f
              && loaded.get (0, pTrackGain) == -6.0f
              && loaded.get (1, pCompThresh) == spec (pCompThresh).defaultValue,
            "channels 2 and 3 and the globals came back");

    const bool flatShape = saved.getDynamicObject() != nullptr
                            && saved.getDynamicObject()->hasProperty ("inGain")
                            && saved.getDynamicObject()->hasProperty ("compThr");
    report ("file keeps the old flat shape", flatShape,
            "channel 0 doubles as the legacy view");

    auto* old = new juce::DynamicObject();
    old->setProperty ("compThr", -24.0);
    ParamStore migrated;
    migrated.fromVar (juce::var (old));
    report ("an old settings file loads as channel 0",
            migrated.get (0, pCompThresh) == -24.0f
              && migrated.get (1, pCompThresh) == spec (pCompThresh).defaultValue,
            "flat value landed on slot 0 only");
}

//==============================================================================
// The foot map: what a controller sends becomes a named control, and the
// layout in force says what each control does. Everything here is the wire
// behaviour of a 1998 Ground Control — program changes for presets, 0/127
// controllers for its lit instant-access buttons, a volume pedal as a sweep
// — plus the momentary switch other boxes have.
static FootControl footControl (const char* id, FootTrigger::Type type, int channel, int number,
                                FootControl::Kind kind)
{
    FootControl c;
    c.id = id;
    c.name = id;
    c.trigger = { type, channel, number };
    c.kind = kind;
    return c;
}

static void testFootControl()
{
    std::printf ("\nFoot control\n");

    const auto pc = [] (int ch, int n) { return juce::MidiMessage::programChange (ch, n); };
    const auto cc = [] (int ch, int n, int v) { return juce::MidiMessage::controllerEvent (ch, n, v); };

    {
        ParamStore params;
        FootEngine foot (params);

        foot.handleMidi ("any", pc (1, 1));
        const bool tune = params.getBool (pVocalOn) && ! params.getBool (pVocoderOn);
        foot.handleMidi ("any", pc (7, 2));
        const bool voc = ! params.getBool (pVocalOn) && params.getBool (pVocoderOn);
        foot.handleMidi ("any", pc (1, 0));
        const bool none = ! params.getBool (pVocalOn) && ! params.getBool (pVocoderOn);

        report ("out of the box, programs 0/1/2 still pick the mic lane", tune && voc && none,
                "the old hardwired mapping, now a layout");
    }

    FootMap map;
    map.controls = {
        footControl ("ia1",   FootTrigger::controller, 16, 80, FootControl::latch),
        footControl ("ia2",   FootTrigger::controller, 16, 81, FootControl::latch),
        footControl ("hold",  FootTrigger::controller,  0, 64, FootControl::momentary),
        footControl ("exp",   FootTrigger::controller,  1,  7, FootControl::sweep),
        footControl ("p20",   FootTrigger::program,     1, 20, FootControl::preset),
        footControl ("go",    FootTrigger::start,       0,  0, FootControl::preset),
    };

    FootLayout a;
    a.name = "A";
    a.jobs = {
        { "ia1",  "bass/odOn",      FootJob::follow, 0.0f, 1.0f },
        { "ia2",  "bass/chOn",      FootJob::flip,   0.0f, 1.0f },
        { "hold", "bass/bstOn",     FootJob::follow, 0.0f, 1.0f },
        { "hold", "bass/phOn",      FootJob::flip,   0.0f, 1.0f },
        { "hold", "bass/odLevel",   FootJob::follow, -6.0f, 3.0f },
        { "exp",  "bass/dlMix",     FootJob::follow, 0.1f, 0.6f },
        { "exp",  "bass/rvMix",     FootJob::follow, 0.8f, 0.2f },
        { "exp",  "bass/octInt",    FootJob::follow, -12.0f, 12.0f },
        { "exp",  "bass/compThr",   FootJob::follow, -200.0f, 50.0f },
        { "exp",  "do/loop",        FootJob::follow, 0.0f, 1.0f },
        { "p20",  "bass/odOn",      FootJob::on,     0.0f, 1.0f },
        { "p20",  "bass/chOn",      FootJob::off,    0.0f, 1.0f },
        { "p20",  "bass/odDrive",   FootJob::follow, 0.0f, 20.0f },
        { "p20",  "ch4/compOn",     FootJob::on,     0.0f, 1.0f },
        { "p20",  "bass/nonsense",  FootJob::on,     0.0f, 1.0f },
        { "p20",  "nowhere/odOn",   FootJob::on,     0.0f, 1.0f },
        { "go",   "do/play",        FootJob::follow, 0.0f, 1.0f },
    };

    FootLayout b;
    b.name = "B";
    b.jobs = { { "ia1", "bass/dlOn", FootJob::follow, 0.0f, 1.0f } };

    map.layouts = { a, b };
    map.active = "A";

    ParamStore params;
    FootEngine foot (params);
    foot.setMap (map);

    {
        foot.handleMidi ("gc", cc (16, 80, 127));
        const bool lit = params.getBool (pDriveOn);
        params.set (pDriveOn, 0.0f);                 // a hand on the screen disagrees
        foot.handleMidi ("gc", cc (16, 80, 0));
        const bool dark = ! params.getBool (pDriveOn);
        foot.handleMidi ("gc", cc (16, 80, 127));
        report ("a latching switch on Follow matches the pedal's own light",
                lit && dark && params.getBool (pDriveOn),
                "127 is on and 0 is off, whatever the screen did in between");

        foot.handleMidi ("gc", cc (1, 80, 0));
        report ("a control on channel 16 ignores channel 1", params.getBool (pDriveOn),
                "same controller number, wrong channel");
    }

    {
        foot.handleMidi ("gc", cc (16, 81, 127));
        const bool first = params.getBool (pChoOn);
        foot.handleMidi ("gc", cc (16, 81, 0));
        report ("Flip on a latching switch turns on every press", first && ! params.getBool (pChoOn),
                "lit and dark are both presses");
    }

    {
        foot.handleMidi ("gc", cc (3, 64, 127));
        const bool held = params.getBool (pBoostOn) && params.getBool (pPhaserOn)
                            && params.get (pDriveLevel) == 3.0f;
        foot.handleMidi ("gc", cc (3, 64, 0));
        const bool released = ! params.getBool (pBoostOn) && params.getBool (pPhaserOn)
                                && params.get (pDriveLevel) == -6.0f;
        foot.handleMidi ("gc", cc (3, 64, 127));
        report ("a momentary switch follows while held and flips only on the way down",
                held && released && ! params.getBool (pPhaserOn),
                "boost on while down; phaser toggles per press; level steps hi and back to lo");
    }

    {
        const int before = foot.takeCommands().size();
        foot.handleMidi ("gc", cc (1, 7, 0));
        const float heel = params.get (pDelayMix);
        const bool heelRest = params.get (pRevMix) == 0.8f && params.get (pOctInterval) == -12.0f
                                && params.get (pCompThresh) == spec (pCompThresh).minValue;
        foot.handleMidi ("gc", cc (1, 7, 40));
        foot.handleMidi ("gc", cc (1, 7, 66));
        const float mid = params.get (pOctInterval);
        foot.handleMidi ("gc", cc (1, 7, 100));
        foot.handleMidi ("gc", cc (1, 7, 127));
        const float toe = params.get (pDelayMix);

        report ("an expression pedal sweeps a knob between its two ends",
                std::abs (heel - 0.1f) < 1.0e-5f && std::abs (toe - 0.6f) < 1.0e-5f,
                juce::String (heel, 3) + " at the heel, " + juce::String (toe, 3) + " at the toe");
        report ("a sweep runs backwards, snaps to steps and stays inside the knob",
                heelRest && std::abs (params.get (pRevMix) - 0.2f) < 1.0e-5f
                  && mid == std::round (mid) && params.get (pOctInterval) == 12.0f
                  && params.get (pCompThresh) == spec (pCompThresh).maxValue,
                "reverb falls as the pedal rises; the octave lands on whole semitones");
        report ("a sweep presses once, on the way up through the middle",
                before == 0 && foot.takeCommands().size() == 1,
                "five messages, one loop tap");
    }

    {
        params.set (pDriveOn, 0.0f);
        params.set (pChoOn, 1.0f);
        const auto serial = foot.getChangeSerial();
        foot.handleMidi ("gc", pc (1, 20));
        const bool scene = params.getBool (pDriveOn) && ! params.getBool (pChoOn)
                             && params.get (pDriveAmount) == 20.0f
                             && params.getBool (3, pCompOn) && ! params.getBool (0, pCompOn);
        const bool noticed = foot.getChangeSerial() != serial;
        const auto settled = foot.getChangeSerial();
        foot.handleMidi ("gc", pc (1, 20));
        report ("one preset button sets a whole scene, on the right channels",
                scene && noticed && foot.getChangeSerial() == settled,
                "drive on, chorus off, drive amount 20, channel 4's comp, and nothing for names the rig lacks");
    }

    {
        int woken = 0;
        foot.setCommandNotifier ([&woken] { ++woken; });
        foot.handleMidi ("gc", juce::MidiMessage::midiStart());
        const auto taken = foot.takeCommands();
        foot.setCommandNotifier (nullptr);
        report ("a command waits for the window and wakes it",
                woken == 1 && taken.size() == 1 && taken[0] == "do/play" && foot.takeCommands().isEmpty(),
                "MIDI Start queued do/play");
    }

    {
        foot.setActiveLayout ("B");
        params.set (pDriveOn, 0.0f);
        foot.handleMidi ("gc", cc (16, 80, 127));
        const bool swapped = params.getBool (pDelayOn) && ! params.getBool (pDriveOn);
        foot.setActiveLayout ("no such layout");
        const bool stayed = foot.getActiveLayout() == "B";
        foot.setActiveLayout ("A");
        report ("another layout gives the same switch another job", swapped && stayed,
                "delay instead of drive; an unknown layout name changes nothing");
    }

    {
        auto scoped = map;
        scoped.device = "Scarlett 4i4 USB";
        foot.setMap (scoped);
        params.set (pDriveOn, 0.0f);
        foot.handleMidi ("MPK mini", cc (16, 80, 127));
        const bool ignored = ! params.getBool (pDriveOn);
        const bool shown = foot.getLastHeard().device == "MPK mini" && foot.getLastHeard().controls.isEmpty();
        foot.handleMidi ("Scarlett 4i4 USB", cc (16, 80, 127));
        report ("with a device chosen, only that box is obeyed",
                ignored && shown && params.getBool (pDriveOn)
                  && foot.getLastHeard().controls.contains ("ia1"),
                "the stranger is still shown as heard");
        foot.setMap (map);
    }

    {
        // Learning: the press is captured, not obeyed.
        params.set (pDriveOn, 0.0f);
        foot.beginLearn();
        foot.handleMidi ("gc", pc (1, 20), 1000);
        FootEngine::Learned got;
        const bool program = foot.pollLearn (got, 1000) && got.trigger.type == FootTrigger::program
                               && got.trigger.number == 20 && got.trigger.channel == 1
                               && got.kind == FootControl::preset && got.device == "gc"
                               && ! params.getBool (pDriveOn) && ! foot.isLearning();

        foot.beginLearn();
        foot.handleMidi ("gc", cc (16, 85, 127), 2000);
        const bool early = foot.pollLearn (got, 2100);
        const bool latch = foot.pollLearn (got, 2000 + FootEngine::learnSettleMs)
                             && got.kind == FootControl::latch && got.trigger.number == 85;

        foot.beginLearn();
        foot.handleMidi ("gc", cc (2, 64, 127), 3000);
        foot.handleMidi ("gc", cc (2, 64, 0), 3120);
        const bool momentary = foot.pollLearn (got, 3600) && got.kind == FootControl::momentary;

        foot.beginLearn();
        foot.handleMidi ("gc", cc (1, 11, 3), 4000);
        foot.handleMidi ("gc", cc (1, 11, 9), 4020);
        const bool sweep = foot.pollLearn (got, 4600) && got.kind == FootControl::sweep;

        foot.beginLearn();
        foot.cancelLearn();
        foot.handleMidi ("gc", cc (16, 80, 127), 5000);

        report ("learning captures a press instead of obeying it",
                program && ! early && latch && params.getBool (pDriveOn),
                "program 20 learned without firing its scene; a cancelled learn obeys again");
        report ("learning tells a lit switch, a held switch and a pedal apart", momentary && sweep,
                "one message, a press and release, a run of in-between values");
    }

    {
        // A control learned again keeps every job it had.
        auto relearned = map;
        relearned.controls[0].trigger = { FootTrigger::controller, 1, 12 };
        foot.setMap (relearned);
        params.set (pDriveOn, 0.0f);
        foot.handleMidi ("gc", cc (16, 80, 127));
        const bool oldDead = ! params.getBool (pDriveOn);
        foot.handleMidi ("gc", cc (1, 12, 127));
        report ("a control learned again keeps what it did", oldDead && params.getBool (pDriveOn),
                "new wire, same job in every layout");
        foot.setMap (map);
    }

    {
        // A sustain pedal on Follow is the Freeze pedal's hold, whichever
        // way the box reports it: a held switch, or — through an expression
        // jack — a pedal that only ever stands at one end or the other.
        auto sustain = map;
        sustain.controls.push_back (footControl ("sus", FootTrigger::controller, 1, 64, FootControl::momentary));
        sustain.controls.push_back (footControl ("jack", FootTrigger::controller, 1, 4, FootControl::sweep));
        sustain.layouts[0].jobs.push_back ({ "sus", "bass/frzOn", FootJob::follow, 0.0f, 1.0f });
        sustain.layouts[0].jobs.push_back ({ "jack", "ch4/frzOn", FootJob::follow, 0.0f, 1.0f });
        foot.setMap (sustain);

        foot.handleMidi ("gc", cc (1, 64, 127));
        const bool down = params.getBool (pFreezeOn);
        foot.handleMidi ("gc", cc (1, 64, 0));
        const bool up = ! params.getBool (pFreezeOn);

        foot.handleMidi ("gc", cc (1, 4, 120));
        const bool jackDown = params.getBool (3, pFreezeOn) && ! params.getBool (0, pFreezeOn);
        foot.handleMidi ("gc", cc (1, 4, 0));

        report ("a sustain pedal holds the freeze while it is down",
                down && up && jackDown && ! params.getBool (3, pFreezeOn),
                "as a held switch on the bass, as a two-position pedal on channel 4");
        foot.setMap (map);
    }

    {
        // No sustain pedal to hand: two preset buttons latch the hold, one
        // to start it and one to stop it, and a third does both on Flips.
        // Heard through a real chain, on the wires a Ground Control's
        // buttons use — program changes on channel 1.
        const double sr = 48000.0;
        const int block = 64;

        FootMap latch;
        latch.controls = {
            footControl ("start",  FootTrigger::program, 1, 0, FootControl::preset),
            footControl ("stop",   FootTrigger::program, 1, 1, FootControl::preset),
            footControl ("toggle", FootTrigger::program, 1, 2, FootControl::preset),
        };
        latch.layouts = { { "Latch", { { "start",  "bass/frzOn", FootJob::on,   0.0f, 1.0f },
                                       { "stop",   "bass/frzOn", FootJob::off,  0.0f, 1.0f },
                                       { "toggle", "bass/frzOn", FootJob::flip, 0.0f, 1.0f } } } };
        latch.active = "Latch";

        ParamStore held;
        held.set (pFreezeMotion, 0.0f);

        FootEngine buttons (held);
        buttons.setMap (latch);

        // 110 Hz played for 1.2 s, silence, then 165 Hz from 4.0 to 5.2 s.
        std::vector<float> in ((size_t) (sr * 7.6), 0.0f);

        for (size_t i = 0; i < in.size(); ++i)
        {
            const double t = (double) i / sr;
            const double first = t < 1.2 ? 1.0 : std::exp (-(t - 1.2) / 0.01);
            const double second = juce::jlimit (0.0, 1.0, (t - 4.0) / 0.02) * (t < 5.2 ? 1.0 : std::exp (-(t - 5.2) / 0.01));

            in[i] = (float) (0.2 * (first * std::sin (6.283185307179586 * 110.0 * t)
                                      + second * std::sin (6.283185307179586 * 165.0 * t)));
        }

        // seconds, program
        const std::pair<double, int> presses[] { { 1.0, 0 }, { 1.5, 0 }, { 3.0, 1 }, { 5.0, 2 }, { 6.5, 2 } };

        FxChain chain (held);
        chain.prepare (sr, block);

        std::vector<float> mono ((size_t) block), left ((size_t) block), right ((size_t) block), out;
        size_t next = 0;

        for (int i = 0; i + block <= (int) in.size(); i += block)
        {
            while (next < std::size (presses) && (double) i / sr >= presses[next].first)
                buttons.handleMidi ("gc", pc (1, presses[next++].second));

            std::copy (in.begin() + i, in.begin() + i + block, mono.begin());
            juce::ScopedNoDenormals noDenormals;
            chain.process (mono.data(), left.data(), right.data(), block);
            out.insert (out.end(), left.begin(), left.end());
        }

        const auto level = [&] (double from, double to, double hz)
        {
            return goertzel (out.data() + (int) (sr * from), (int) (sr * (to - from)), sr, hz) / 0.2;
        };

        const double started = level (2.0, 2.9, 110.0), stopped = level (3.5, 3.9, 110.0);
        const double flipped = level (5.6, 6.4, 165.0), flippedOff = level (7.0, 7.5, 165.0);

        report ("two buttons latch the freeze, and one on Flips does both",
                std::abs (started - 1.0) < 0.05 && stopped < 1.0e-3
                  && std::abs (flipped - 1.0) < 0.05 && flippedOff < 1.0e-3 && ! held.getBool (pFreezeOn),
                "start holds " + juce::String (started, 2) + " of the note through a second press, stop leaves "
                    + juce::String (stopped, 4) + "; flip holds " + juce::String (flipped, 2) + ", flip again leaves "
                    + juce::String (flippedOff, 4));
    }

    {
        const auto text = juce::JSON::toString (map.toVar(), false);
        const auto back = FootMap::fromVar (juce::JSON::parse (text));
        report ("the map survives its file", juce::JSON::toString (back.toVar(), false) == text,
                juce::String (text.length()) + " characters, identical on the way back");

        auto messy = map;
        messy.controls.push_back (footControl ("ia1", FootTrigger::program, 1, 99, FootControl::preset));
        messy.layouts[0].jobs.push_back ({ "ghost", "bass/odOn", FootJob::on, 0.0f, 1.0f });
        messy.active = "gone";
        const auto cleaned = FootMap::fromVar (messy.toVar());
        const auto empty = FootMap::fromVar (juce::var());
        report ("a damaged map is trimmed to what can run",
                cleaned.controls.size() == map.controls.size()
                  && cleaned.layouts[0].jobs.size() == map.layouts[0].jobs.size()
                  && cleaned.active == "A"
                  && empty.layouts.size() == 1 && empty.controls.empty(),
                "the twin id, the orphan job and the missing layout are gone");

        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("rig_foot_test");
        dir.deleteRecursively();
        const auto file = dir.getChildFile ("foot.json");
        const bool fresh = FootMap::loadFrom (file).controls.size() == 3;
        const bool saved = map.saveTo (file)
                             && juce::JSON::toString (FootMap::loadFrom (file).toVar(), false) == text;
        file.replaceWithText ("{ not json");
        const bool fallback = FootMap::loadFrom (file).controls.size() == 3
                                && file.getSiblingFile ("foot.json.unreadable").existsAsFile();
        dir.deleteRecursively();
        report ("no file means the factory map, and a broken one is set aside", fresh && saved && fallback,
                "foot.json.unreadable kept beside it");
    }
}

//==============================================================================
// The looper, driven the way the engine drives it: every block it is asked
// what it wants to hear, then handed that. `hands` is what the player is
// doing at a moment in time; it reaches the looper `latency` samples late,
// the way a played note arrives after the interface's round trip.
struct LoopBench
{
    LoopBench (double sampleRate, int blockSize, int latencySamples)
        : sr (sampleRate), block (blockSize), latency (latencySamples),
          src ((size_t) blockSize), outL ((size_t) blockSize), outR ((size_t) blockSize)
    {
        looper.prepare (sr, block);
    }

    void run (juce::int64 samples)
    {
        for (const auto end = now + samples; now < end; now += block)
        {
            wanted = looper.beginBlock();

            for (int i = 0; i < block; ++i)
                src[(size_t) i] = hands != nullptr ? hands (now + i - latency) : 0.0f;

            std::fill (outL.begin(), outL.end(), 0.0f);
            std::fill (outR.begin(), outR.end(), 0.0f);

            const auto start = std::chrono::high_resolution_clock::now();
            looper.process (src.data(), src.data(), outL.data(), outR.data(), block, latency, level);

            if (timing)
                blockUs.push_back (std::chrono::duration<double, std::micro> (
                                       std::chrono::high_resolution_clock::now() - start).count());

            out.insert (out.end(), outL.begin(), outL.end());
        }
    }

    void runSeconds (double seconds)   { run ((juce::int64) (seconds * sr)); }

    // Largest difference between what came out over a stretch and what was
    // expected there.
    double worstError (juce::int64 from, juce::int64 to, const std::function<double (juce::int64)>& expected) const
    {
        double worst = 0.0;

        for (auto t = from; t < to; ++t)
            worst = juce::jmax (worst, std::abs ((double) out[(size_t) t] - expected (t)));

        return worst;
    }

    double peak (juce::int64 from, juce::int64 to) const
    {
        double p = 0.0;

        for (auto t = from; t < to; ++t)
            p = juce::jmax (p, (double) std::abs (out[(size_t) t]));

        return p;
    }

    Looper looper;
    double sr;
    int block, latency;
    juce::int64 now = 0;
    float level = 1.0f;
    int wanted = -1;
    bool timing = false;
    std::vector<double> blockUs;
    std::function<float (juce::int64)> hands;
    std::vector<float> src, outL, outR, out;
};

static void testLooper (double sr, int block)
{
    std::printf ("\nLooper  (%.0f Hz, %d-sample blocks)\n", sr, block);

    const int latency = (int) (0.011 * sr);   // a believable round trip
    const auto tone = [sr] (double hz, float amp)
    {
        return [sr, hz, amp] (juce::int64 t)
        {
            return amp * (float) std::sin (6.283185307179586 * hz * (double) t / sr);
        };
    };

    LoopBench bench (sr, block, latency);
    auto& looper = bench.looper;

    // --- the two taps ---
    looper.tapTempo (1000);
    looper.tapTempo (1500);
    looper.tapTempo (2000);
    looper.tapTempo (2500);
    report ("four taps half a second apart", std::abs (looper.getBpm() - 120.0f) < 0.01f,
            juce::String (looper.getBpm(), 1) + " BPM");

    looper.tapTempo (9000);   // a lone tap after a gap changes nothing yet
    const bool lone = std::abs (looper.getBpm() - 120.0f) < 0.01f;
    looper.tapTempo (9625);
    report ("a new count after a gap", lone && std::abs (looper.getBpm() - 96.0f) < 0.01f,
            juce::String (looper.getBpm(), 1) + " BPM from two taps 625 ms apart");

    looper.tapTempo (20000);
    looper.tapTempo (20500);

    // Bars: a tap is one more bar, and the tap past sixteen is one again.
    const auto tapBarsTo = [&looper] (int wanted)
    {
        for (int i = 0; i < 40 && looper.getBars() != wanted; ++i)
            looper.tapBars (0);
    };

    const int first = looper.getBars();
    looper.tapBars (30000);
    const int second = looper.getBars();
    tapBarsTo (16);
    const int most = looper.getBars();
    looper.tapBars (40000);
    report ("a bars tap adds a bar; past sixteen it is one", first == 4 && second == 5 && most == 16 && looper.getBars() == 1,
            juce::String (first) + ", " + juce::String (second) + " ... " + juce::String (most) + ", "
              + juce::String (looper.getBars()));

    tapBarsTo (16);
    looper.tapTempo (50000);
    looper.tapTempo (51500);   // 40 BPM: six-second bars
    const int held = looper.getBars();
    looper.tapBars (60000);
    report ("a slow tempo shortens the longest loop", held == Looper::maxBarsAt (40.0f) && looper.getBars() == 1
              && Looper::maxBarsAt (60.0f) == 16 && Looper::maxBarsAt (240.0f) == 16,
            juce::String (held) + " bars at 40 BPM, then round to " + juce::String (looper.getBars()));

    // 120 BPM, two bars.
    looper.tapTempo (70000);
    looper.tapTempo (70500);
    tapBarsTo (2);

    const auto beat = (juce::int64) std::llround (sr * 0.5);
    const juce::int64 countLen = beat * 4, loopLen = beat * 8;
    const int edge = (int) (0.02 * sr);   // room around the seam and the ramps

    const auto runUntil = [&bench] (juce::int64 t) { bench.run (t - bench.now); };

    // --- count-in and the first take ---
    const auto x = tone (97.3, 0.25f);
    bench.hands = x;
    bench.runSeconds (0.25);
    report ("silent, asking for nothing before Record", bench.peak (0, bench.now) == 0.0 && bench.wanted == -1,
            "state " + juce::String ((int) looper.getState()));

    const auto t0 = bench.now;   // the count-in's first click
    looper.pressRecord (0);
    bench.run (block);
    report ("Record starts the count, asks for its channel",
            looper.getState() == Looper::State::countIn && bench.wanted == 0, "slot " + juce::String (bench.wanted));

    looper.tapTempo (90000);
    looper.tapTempo (90300);
    looper.tapBars (90600);
    report ("taps are ignored once a take is under way",
            std::abs (looper.getBpm() - 120.0f) < 0.01f && looper.getBars() == 2, "still 120 BPM, 2 bars");

    const auto T0 = t0 + countLen;   // the downbeat
    const auto p0 = T0 + loopLen;    // playback's first sample

    runUntil (p0 - block);
    const bool stillTaking = looper.getState() == Looper::State::recording;
    runUntil (p0 + block);
    report ("the take closes itself after its bars", stillTaking && looper.getState() == Looper::State::playing,
            "state " + juce::String ((int) looper.getState()));

    {
        // Clicks: an onset is a loud sample after a stretch of silence.
        std::vector<juce::int64> onsets;

        for (auto t = t0; t < p0; ++t)
            if (std::abs (bench.out[(size_t) t]) > 0.02f && (onsets.empty() || t - onsets.back() > beat / 2))
                onsets.push_back (t);

        bool spaced = onsets.size() == 12;

        for (size_t k = 0; spaced && k < onsets.size(); ++k)
            spaced = std::abs (onsets[k] - (t0 + (juce::int64) k * beat)) <= 2;

        report ("four clicks in, then a click a beat", spaced,
                juce::String ((int) onsets.size()) + " clicks on the beat");
    }

    runUntil (p0 + loopLen * 3);

    // Playback is the take, placed where the player heard it: the sample
    // played on the downbeat is the loop's first, whatever the round trip.
    {
        double worst = 0.0;

        for (int cycle = 0; cycle < 3; ++cycle)
            worst = juce::jmax (worst, bench.worstError (p0 + cycle * loopLen, p0 + (cycle + 1) * loopLen - edge,
                                                         [&] (juce::int64 t) { return (double) x (T0 + (t - p0) % loopLen); }));

        report ("the loop is what was played, where played", worst < 1.0e-5,
                "worst error " + juce::String (worst, 7) + ", and no click in playback");
    }

    {
        double step = 0.0;

        for (auto t = p0 + loopLen - edge; t < p0 + loopLen + edge; ++t)
            step = juce::jmax (step, (double) std::abs (bench.out[(size_t) t] - bench.out[(size_t) (t - 1)]));

        report ("the seam does not click", step < 0.012,
                "largest step " + juce::String (step, 4) + " (a bare jump would be up to 0.5)");
    }

    // From here the first take is judged against its own clean second pass,
    // seam and all, and each layer against what the hands were doing.
    struct Layer
    {
        juce::int64 a = 0, b = 0;   // pressed on, pressed off
        std::function<float (juce::int64)> played;
    };

    const auto firstTake = [&bench, p0, loopLen] (juce::int64 t)
    {
        return (double) bench.out[(size_t) (p0 + loopLen + (t - p0) % loopLen)];
    };

    // The worst error over one pass of the loop starting at `from`, with
    // the layers in `on` expected to sound. The ramps at each layer's two
    // ends — every layer in `known`, sounding or not — are left unjudged.
    const auto passError = [&] (juce::int64 from, std::vector<const Layer*> known,
                                std::vector<const Layer*> on, double gain = 1.0)
    {
        double worst = 0.0;

        for (auto t = from; t < from + loopLen; ++t)
        {
            bool judged = true;

            for (const auto* layer : known)
            {
                const auto when = layer->a + (t - layer->a) % loopLen;

                if (when < layer->a + edge || when >= layer->a + loopLen - latency - edge
                     || (when >= layer->b - edge && when < layer->b + latency + edge))
                    judged = false;
            }

            if (! judged)
                continue;

            double expected = firstTake (t);

            for (const auto* layer : on)
            {
                const auto when = layer->a + (t - layer->a) % loopLen;

                if (when < layer->b)
                    expected += (double) layer->played (when);
            }

            worst = juce::jmax (worst, std::abs ((double) bench.out[(size_t) t] - gain * expected));
        }

        return worst;
    };

    // Record a layer of `seconds` with the given hands on the given slot.
    const auto layer = [&] (const std::function<float (juce::int64)>& played, int slot, juce::int64 length)
    {
        Layer made;
        made.played = played;
        bench.hands = played;
        made.a = bench.now;
        looper.pressRecord (slot);
        bench.run (length);
        made.b = bench.now;
        looper.pressRecord (slot);
        bench.hands = nullptr;
        bench.run (latency + edge);
        return made;
    };

    // --- a layer ---
    bench.run (loopLen / 4);
    bench.hands = tone (311.0, 0.2f);
    looper.pressRecord (1);
    bench.run (block);
    const bool askedForMic = bench.wanted == 1 && looper.getState() == Looper::State::overdubbing;
    looper.pressUndo();   // take that scrap back off
    bench.hands = nullptr;
    bench.run (edge);

    const Layer y = layer (tone (311.0, 0.2f), 1, loopLen / 2);
    report ("Record on a playing loop layers that channel",
            askedForMic && looper.getState() == Looper::State::playing && looper.hasLayer(), "asked for slot 1");

    auto from = bench.now;
    bench.run (loopLen);
    report ("the layer lands where it was played", passError (from, { &y }, { &y }) < 1.0e-5,
            "worst error " + juce::String (passError (from, { &y }, { &y }), 7));

    // --- undo, redo ---
    looper.pressUndo();
    bench.run (edge);
    from = bench.now;
    bench.run (loopLen);
    report ("Undo takes the layer away", passError (from, { &y }, {}) < 1.0e-5 && looper.isLayerUndone(),
            "worst error " + juce::String (passError (from, { &y }, {}), 7));

    looper.pressUndo();
    bench.run (edge);
    from = bench.now;
    bench.run (loopLen);
    report ("Undo again brings it back", passError (from, { &y }, { &y }) < 1.0e-5 && ! looper.isLayerUndone(),
            "worst error " + juce::String (passError (from, { &y }, { &y }), 7));

    // --- a second layer keeps the first, and only the second comes off ---
    const Layer z = layer (tone (523.0, 0.15f), 0, loopLen / 4);
    from = bench.now;
    bench.run (loopLen);
    report ("a second layer sits on the first", passError (from, { &y, &z }, { &y, &z }) < 1.0e-5,
            "worst error " + juce::String (passError (from, { &y, &z }, { &y, &z }), 7));

    looper.pressUndo();
    bench.run (edge);
    from = bench.now;
    bench.run (loopLen);
    report ("Undo takes only the newest layer", passError (from, { &y, &z }, { &y }) < 1.0e-5,
            "worst error " + juce::String (passError (from, { &y, &z }, { &y }), 7));

    // A layer recorded over an undone one replaces it for good.
    const Layer w = layer (tone (733.0, 0.1f), 0, loopLen / 3);
    from = bench.now;
    bench.run (loopLen);
    report ("a layer over an undone one drops the undone", passError (from, { &y, &z, &w }, { &y, &w }) < 1.0e-5,
            "worst error " + juce::String (passError (from, { &y, &z, &w }, { &y, &w }), 7));

    // A layer that runs round the loop more than once stacks on itself.
    looper.pressUndo();
    bench.run (edge);
    const Layer twice = layer (tone (211.0, 0.05f), 0, loopLen * 2);
    from = bench.now;
    bench.run (loopLen);
    {
        // Two passes of the same hands at every position: the second pass
        // is the first a loop later.
        Layer second = twice;
        second.played = [&twice, loopLen] (juce::int64 t) { return twice.played (t + loopLen); };
        Layer first = twice;
        first.b = twice.a + loopLen;
        second.b = twice.a + loopLen;

        // Only the layer's own two ends are ramps; its middle wrap is not.
        const double error = passError (from, { &y, &twice }, { &y, &first, &second });
        report ("a layer two loops long stacks on itself", error < 1.0e-5, "worst error " + juce::String (error, 7));
    }

    looper.pressUndo();
    bench.run (edge);

    // --- loop level ---
    bench.level = 0.5f;
    bench.run (loopLen / 2);
    from = bench.now;
    bench.run (loopLen);
    report ("the loop's level rides the whole loop", passError (from, { &y, &twice }, { &y }, 0.5) < 1.0e-5,
            "worst error at half gain " + juce::String (passError (from, { &y, &twice }, { &y }, 0.5), 7));
    bench.level = 1.0f;
    bench.run (loopLen / 2);

    // --- play / pause ---
    bench.run (loopLen + loopLen / 3);
    const auto pausedAt = bench.now;
    looper.pressPlayPause();
    bench.run (edge);
    from = bench.now;
    bench.runSeconds (0.5);
    report ("Pause is silence", bench.peak (from, bench.now) == 0.0 && looper.getState() == Looper::State::paused,
            "after a 10 ms fade");

    looper.pressPlayPause();
    const auto resumedAt = bench.now;
    bench.run (loopLen + edge);
    {
        // Play carries on from the sample Pause was pressed on: what comes
        // out now is what came out one loop before the pause, moved along
        // by however long the pause lasted.
        const auto held = resumedAt - pausedAt;
        double worst = 0.0;

        for (auto t = resumedAt + edge; t < resumedAt + loopLen / 8; ++t)
            worst = juce::jmax (worst, std::abs ((double) bench.out[(size_t) t]
                                                   - (double) bench.out[(size_t) (t - held - loopLen)]));

        report ("Play picks up where Pause left it", worst < 1.0e-5 && looper.getState() == Looper::State::playing
                  && bench.peak (resumedAt + edge, resumedAt + loopLen / 8) > 0.05,
                "worst error " + juce::String (worst, 7));
    }

    // --- clear, and a new loop at another tempo with nothing left behind ---
    looper.pressClear();
    bench.run (edge);
    from = bench.now;
    bench.runSeconds (0.3);
    report ("Clear is silence and an empty looper",
            bench.peak (from, bench.now) == 0.0 && looper.getState() == Looper::State::empty
              && ! looper.hasLayer() && bench.wanted == -1,
            "state " + juce::String ((int) looper.getState()));

    looper.tapTempo (200000);
    looper.tapTempo (200625);
    tapBarsTo (1);

    {
        const double beat2 = sr * 0.625;   // not a whole number of samples at 44.1k
        const auto count2 = (juce::int64) std::llround (beat2 * 4.0), loop2 = count2;
        const auto x2 = tone (61.7, 0.3f);
        bench.hands = x2;

        const auto t2 = bench.now;
        const auto p2 = t2 + count2 + loop2;
        looper.pressRecord (0);
        runUntil (p2 + loop2 / 8);

        // Straight into a layer over the short loop, so the buffers the old
        // loop's layers lived in are in play again.
        const auto v = tone (440.0, 0.1f);
        bench.hands = v;
        const auto va = bench.now;
        looper.pressRecord (0);
        bench.run (loop2 / 2);
        const auto vb = bench.now;
        looper.pressRecord (0);
        bench.hands = nullptr;
        bench.run (latency + edge);

        from = bench.now;
        bench.run (loop2);

        double worst = 0.0;

        for (auto t = from; t < from + loop2; ++t)
        {
            const auto pos = (t - p2) % loop2;
            const auto when = va + (t - va) % loop2;

            if (pos >= loop2 - edge || when < va + edge || when >= va + loop2 - latency - edge
                 || (when >= vb - edge && when < vb + latency + edge))
                continue;

            const double expected = (double) x2 (t2 + count2 + pos) + (when < vb ? (double) v (when) : 0.0);
            worst = juce::jmax (worst, std::abs ((double) bench.out[(size_t) t] - expected));
        }

        report ("a new loop carries nothing of the old one", worst < 1.0e-5 && std::abs (looper.getBpm() - 96.0f) < 0.01f,
                "96 BPM, 1 bar, worst error " + juce::String (worst, 7));

        // --- a take cancelled by Stop leaves nothing ---
        looper.pressClear();
        bench.run (edge);
        looper.pressRecord (0);
        bench.run (count2 / 2);
        looper.pressPlayPause();
        bench.run (block);
        from = bench.now + (juce::int64) (0.08 * sr);   // the last click rings out
        bench.runSeconds (1.0);
        report ("Pause during the count-in cancels the take",
                looper.getState() == Looper::State::empty && bench.peak (from, bench.now) == 0.0,
                "empty and silent");

        looper.pressRecord (0);
        bench.run (count2 / 2);
        looper.pressRecord (0);
        bench.run (block);
        from = bench.now + (juce::int64) (0.08 * sr);
        bench.runSeconds (1.0);
        report ("Record again during the count-in calls it off",
                looper.getState() == Looper::State::empty && bench.peak (from, bench.now) == 0.0,
                "empty and silent");
    }

    // --- Record pressed again during the take ends it on the nearest bar ---
    {
        looper.tapTempo (300000);
        looper.tapTempo (300500);   // 120 BPM
        tapBarsTo (8);

        const auto bar = beat * 4;
        const auto x3 = tone (83.1, 0.25f);

        // The loop is `kept` bars of what was played from the downbeat, and
        // the beat never moved: the loop's position is the grid's.
        const auto takeError = [&] (juce::int64 downbeat, int kept, juce::int64 checkFrom)
        {
            const auto length = bar * kept;
            double worst = 0.0;

            for (auto t = checkFrom; t < checkFrom + length; ++t)
            {
                const auto pos = (t - downbeat) % length;

                if (pos < length - edge)
                    worst = juce::jmax (worst, std::abs ((double) bench.out[(size_t) t] - (double) x3 (downbeat + pos)));
            }

            return worst;
        };

        // A press a little late: just past the end of bar one.
        bench.hands = x3;
        auto downbeat = bench.now + countLen;
        looper.pressRecord (0);
        runUntil (downbeat + bar + bar / 10);
        const bool wasTaking = looper.getState() == Looper::State::recording;
        const auto lateAt = bench.now;
        looper.pressRecord (0);
        bench.run (block);
        const bool closedAtOnce = looper.getState() == Looper::State::playing && looper.getBars() == 1;
        bench.hands = nullptr;
        runUntil (lateAt + bar * 3);
        report ("Record just after a bar line closes on that line",
                wasTaking && closedAtOnce && takeError (downbeat, 1, lateAt + edge) < 1.0e-5,
                "1 bar, worst error " + juce::String (takeError (downbeat, 1, lateAt + edge), 7));

        {
            double step = 0.0;

            for (auto t = downbeat + bar * 3 - edge; t < downbeat + bar * 3 + edge; ++t)
                step = juce::jmax (step, (double) std::abs (bench.out[(size_t) t] - bench.out[(size_t) (t - 1)]));

            report ("the early-closed loop's seam does not click", step < 0.012, "largest step " + juce::String (step, 4));
        }

        // A press a little early: most of the way through bar two.
        looper.pressClear();
        bench.run (edge);
        tapBarsTo (8);
        bench.hands = x3;
        downbeat = bench.now + countLen;
        looper.pressRecord (0);
        runUntil (downbeat + bar + (bar * 8) / 10);
        looper.pressRecord (0);
        bench.run (block);
        const bool ranOn = looper.getState() == Looper::State::recording && looper.getBars() == 2;
        runUntil (downbeat + bar * 2 + block);
        const bool closedOnLine = looper.getState() == Looper::State::playing;
        bench.hands = nullptr;
        runUntil (downbeat + bar * 5);
        report ("Record just before a bar line runs on to it",
                ranOn && closedOnLine && takeError (downbeat, 2, downbeat + bar * 2) < 1.0e-5,
                "2 bars, worst error " + juce::String (takeError (downbeat, 2, downbeat + bar * 2), 7));

        {
            // No click after the early close: the take's last beat is the
            // last one sounded.
            int late = 0;

            for (auto t = downbeat + bar * 2 + (juce::int64) (0.08 * sr); t < downbeat + bar * 5; ++t)
                if (std::abs ((double) bench.out[(size_t) t] - (double) x3 (downbeat + (t - downbeat) % (bar * 2))) > 0.02
                     && (t - downbeat) % (bar * 2) < bar * 2 - edge)
                    ++late;

            report ("the click stops when the take does", late == 0, juce::String (late) + " stray samples");
        }

        // In the first half of bar one there is no bar behind to close on.
        looper.pressClear();
        bench.run (edge);
        tapBarsTo (8);
        bench.hands = x3;
        downbeat = bench.now + countLen;
        looper.pressRecord (0);
        runUntil (downbeat + bar / 4);
        looper.pressRecord (0);
        runUntil (downbeat + bar + block);
        bench.hands = nullptr;
        report ("the shortest loop is one bar", looper.getState() == Looper::State::playing && looper.getBars() == 1,
                juce::String (looper.getBars()) + " bar");
        looper.pressClear();
        bench.run (edge);
    }

    // --- the longest loop: folding a layer must not cost a block its time ---
    {
        LoopBench big (sr, block, latency);
        big.looper.tapTempo (0);
        big.looper.tapTempo (1000);   // 60 BPM

        for (int i = 0; i < 40 && big.looper.getBars() != 16; ++i)
            big.looper.tapBars (0);

        big.hands = x;
        big.looper.pressRecord (0);
        big.runSeconds (4.0 + 64.0 + 1.0);

        big.looper.pressRecord (0);
        big.runSeconds (2.0);
        big.looper.pressRecord (0);
        big.runSeconds (1.0);
        big.blockUs.clear();
        big.timing = true;

        // Three layers started in a row: each start folds the one before
        // across all sixty-four seconds while the loop plays on.
        for (int i = 0; i < 3; ++i)
        {
            big.looper.pressRecord (0);
            big.runSeconds (1.5);
            big.looper.pressRecord (0);
            big.runSeconds (1.5);
        }

        // One block in a hundred is the machine's, not the looper's.
        std::sort (big.blockUs.begin(), big.blockUs.end());
        const double typicalWorst = big.blockUs[(size_t) ((double) big.blockUs.size() * 0.99)];
        const double budgetUs = 1.0e6 * block / sr;

        report ("folding a 64 s loop stays inside the block", big.looper.getBars() == 16
                  && big.looper.getState() == Looper::State::playing && typicalWorst < budgetUs * 0.25,
                juce::String (typicalWorst, 0) + " us a block at worst, of " + juce::String (budgetUs, 0));
    }
}

int main (int argc, char* argv[])
{
    // The scan spawns this same binary as its disposable per-plugin worker.
    if (argc >= 4 && juce::String (argv[1]) == "--scan-worker")
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        InstrumentHost::runScanWorker (juce::String (argv[2]), juce::String (argv[3]));
    }

    if (argc == 4 && juce::String (argv[1]) == "--scenario")
    {
        runScenario (juce::File (juce::String (argv[2])),
                     juce::File (juce::String (argv[3])));
        return 0;
    }

    // The foot map on its own: quick enough to run after every edit to it.
    if (argc == 2 && juce::String (argv[1]) == "--foot")
    {
        testFootControl();
        std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
                     failures, failures == 1 ? "" : "s");
        return failures == 0 ? 0 : 1;
    }

    // The looper on its own, for the same reason.
    if (argc == 2 && juce::String (argv[1]) == "--looper")
    {
        testLooper (48000.0, 64);
        testLooper (44100.0, 128);
        std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
                     failures, failures == 1 ? "" : "s");
        return failures == 0 ? 0 : 1;
    }

    // The freeze on its own, for the same reason.
    if (argc == 2 && juce::String (argv[1]) == "--freeze")
    {
        testFreeze (48000.0, 64);
        testFreeze (44100.0, 64);
        testFreeze (96000.0, 256);
        testFreezeOnRealBass (juce::File ("/Users/fox296/LiveRig/Songs/Plan B/bass.wav"), 64);
        std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
                     failures, failures == 1 ? "" : "s");
        return failures == 0 ? 0 : 1;
    }

    std::printf ("LiveRig DSP validation\n======================\n");

    for (double sr : { 48000.0, 44100.0 })
        testTracker (sr, 64);

    testTrackerOnSweep (48000.0, 64);
    testTrackerHard (48000.0, 64);
    testShifter (48000.0, 64);
    testShifter (44100.0, 32);
    testNoteChanges (48000.0, 64);
    testOctaveStack (48000.0, 64);
    testEnvelopeFilter (48000.0, 64);
    testBoost (48000.0, 64);
    testDriveVoices (48000.0, 64);
    testGlueComp (48000.0, 64);
    testSynth (48000.0, 64);
    testPhaser (48000.0, 64);
    testChorus (48000.0, 64);
    testAnalogDelay (48000.0, 64);
    testTape (48000.0, 64);
    testFreeze (48000.0, 64);
    testFreeze (44100.0, 64);
    testFreeze (96000.0, 256);
    testAmpStage (48000.0, 64);
    testCaptureLibrary();
    testReverb (48000.0, 64);
    testReverb (44100.0, 32);
    testMasterLimiter (48000.0);
    testChannelParams();
    testFootControl();
    testStemDucker (48000.0);
    testParametricEq (48000.0, 64);
    testStageOrder (48000.0, 64);
    testChain (48000.0, 64);
    testDriveCrossover (48000.0, 64);
    testBassTightPreset (48000.0, 64);
    testNoteSegmentation (48000.0, 64);
    testVirtualMidiPort();
    testPluginRegistry (48000.0, 64);
    testEffectSlot (48000.0, 64);
    testVocoderSidechain (48000.0, 64);
    testVocalChannel (48000.0, 64);
    testAutotunePicksAndCorrects (48000.0, 64);
    testVocoderSings (48000.0, 64);
    testCarrierTap (48000.0, 64);
    testInstrumentHosting ("SKY Keys", 48000.0, 64);
    testInstrumentHosting ("DLSMusicDevice", 48000.0, 64, true);   // the control
    testInstrumentHosting ("Vital", 48000.0, 64);
    testVibratoDoesNotChatter (48000.0, 64);
    testTransposeComesBackTrue (48000.0, 64);
    testHoldSurvivesRealTracking (48000.0, 64);
    testTrackerOnDominantSecondHarmonic (48000.0, 64);
    testMidiThruGatesTheLiveLane (48000.0, 64);
    testLiveOutMute (48000.0, 64);
    testTunerFilter();
    testSegmenterOnRealBass (juce::File ("/Users/fox296/LiveRig/Songs/Plan B/bass.wav"), 48000.0, 64);
    testFreezeOnRealBass (juce::File ("/Users/fox296/LiveRig/Songs/Plan B/bass.wav"), 64);
    testSubOctave (48000.0, 64);
    testCabFilter (48000.0, 64);
    testCompSidechain (48000.0, 64);
    testChain (48000.0, 32);
    testBypassIsClickFree (48000.0, 64);
    testStemPlayer (48000.0, 44100.0, 64);
    testStemPlayer (48000.0, 48000.0, 32);
    testSectionLoop (48000.0, 64);
    testLooper (48000.0, 64);
    testLooper (44100.0, 128);

    if (argc > 1)
    {
        const juce::File target { juce::String (argv[1]) };

        if (target.isDirectory())
            testRealSongFolder (target, 48000.0, 64);
        else
            testRealFile (target, 64);
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
                 failures, failures == 1 ? "" : "s");

    return failures == 0 ? 0 : 1;
}
