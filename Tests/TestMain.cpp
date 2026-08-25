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

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "FxChain.h"
#include "StemPlayer.h"
#include "Parameters.h"
#include "dsp/OctaveShifter.h"
#include "dsp/PitchTracker.h"

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
static void testChain (double sr, int block)
{
    std::printf ("\nFull chain  (%.0f Hz, %d-sample blocks, everything engaged)\n", sr, block);

    ParamStore params;
    params.set (pCompOn, 1.0f);
    params.set (pEqOn, 1.0f);
    params.set (pEqLoGain, 4.0f);
    params.set (pEqMidGain, -3.0f);
    params.set (pEqHiGain, 2.0f);
    params.set (pDriveOn, 1.0f);
    params.set (pOctOn, 1.0f);
    params.set (pPhaserOn, 1.0f);
    params.set (pDelayOn, 1.0f);
    params.set (pRevOn, 1.0f);
    params.set (pCompMakeup, 3.0f);

    FxChain chain (params);
    chain.prepare (sr, block);

    std::vector<float> note;
    makeNote (note, sr, 49.0, 4.0, 0.4f);

    std::vector<float> mono ((size_t) block), left ((size_t) block), right ((size_t) block);

    double worstMicros = 0.0, totalMicros = 0.0;
    int blocks = 0;
    bool finite = true;
    float peak = 0.0f;

    for (int i = 0; i + block <= (int) note.size(); i += block)
    {
        std::copy (note.begin() + i, note.begin() + i + block, mono.begin());

        const auto t0 = std::chrono::steady_clock::now();
        chain.process (mono.data(), left.data(), right.data(), block);
        const auto t1 = std::chrono::steady_clock::now();

        const double micros = std::chrono::duration<double, std::micro> (t1 - t0).count();

        if (blocks > 4)   // ignore cold-cache blocks
        {
            worstMicros = juce::jmax (worstMicros, micros);
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

    report ("worst block inside the callback budget", worstMicros < budget * 0.5,
            juce::String (worstMicros, 1) + " us worst, "
                + juce::String (totalMicros / juce::jmax (1, blocks - 5), 1) + " us mean, budget "
                + juce::String (budget, 1) + " us ("
                + juce::String (100.0 * worstMicros / budget, 1) + "% worst case)");

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

    const int toggleIds[] { pCompOn, pEqOn, pDriveOn, pOctOn, pPhaserOn, pDelayOn, pRevOn };
    int blockIndex = 0;

    for (int i = 0; i + block <= (int) note.size(); i += block)
    {
        // Flip something every 40 blocks, mid-note, the way a foot would.
        if (blockIndex % 40 == 0)
        {
            const int id = toggleIds[(blockIndex / 40) % 7];
            params.set (id, params.getBool (id) ? 0.0f : 1.0f);
        }

        std::copy (note.begin() + i, note.begin() + i + block, mono.begin());
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

    const auto outFile = file.getSiblingFile (file.getFileNameWithoutExtension() + "-octave.wav");

    if (auto* wav = formats.findFormatForFileExtension ("wav"))
    {
        outFile.deleteFile();

        if (auto stream = std::unique_ptr<juce::FileOutputStream> (outFile.createOutputStream()))
        {
            if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (
                                  wav->createWriterFor (stream.get(), sr, 1, 24, {}, 0)))
            {
                stream.release();
                const float* channels[1] { out.data() };
                writer->writeFromFloatArrays (channels, 1, n);
                std::printf ("  wrote %s\n", outFile.getFullPathName().toRawUTF8());
            }
        }
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
int main (int argc, char* argv[])
{
    std::printf ("LiveRig DSP validation\n======================\n");

    for (double sr : { 48000.0, 44100.0 })
        testTracker (sr, 64);

    testTrackerOnSweep (48000.0, 64);
    testShifter (48000.0, 64);
    testShifter (44100.0, 32);
    testNoteChanges (48000.0, 64);
    testChain (48000.0, 64);
    testChain (48000.0, 32);
    testBypassIsClickFree (48000.0, 64);
    testStemPlayer (48000.0, 44100.0, 64);
    testStemPlayer (48000.0, 48000.0, 32);

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
