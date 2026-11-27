#include "TimbreVoice.h"

#include <algorithm>
#include <cmath>

namespace rig
{

namespace
{

constexpr double twoPi = 6.283185307179586;

// The tracker's own line between a note and not one.
constexpr float confidenceFloor = 0.5f;

// The fundamental has to carry at least this share of the power before its
// phase is worth following. A low E whose second harmonic is five times its
// fundamental still clears it four times over; an octave-down misread, where
// there is nothing at the tracked pitch at all, does not.
constexpr float minFundamentalShare = 0.01f;

// The string may sit this far from the tracker's pitch, as a fraction of it,
// and still be the note the tracker named: about a semitone, which a wide
// vibrato or a bend the tracker is still catching up with stays inside. A
// new note the tracker has not reached yet does not.
constexpr float maxDrift = 0.06f;

// log2 of each harmonic number, so placing a partial on the resonance curve
// is an add.
const std::array<float, TimbreVoice::maxPartials> log2Harmonic = []
{
    std::array<float, TimbreVoice::maxPartials> t {};

    for (size_t n = 0; n < t.size(); ++n)
        t[n] = (float) std::log2 ((double) (n + 1));

    return t;
}();

int nextPow2 (int n)
{
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

} // namespace

void TimbreVoice::prepare (double sampleRate)
{
    sr = sampleRate;

    // Room for the longest period the tracker reports (a little under 26 Hz).
    const int size = nextPow2 ((int) (sr / 20.0) + 16);
    heard.assign ((size_t) size, 0.0f);
    demodRe.assign ((size_t) size, 0.0f);
    demodIm.assign ((size_t) size, 0.0f);
    squared.assign ((size_t) size, 0.0f);
    mask = size - 1;

    controlLength = std::max (16, (int) std::lround (32.0 * sr / 48000.0));
    ampCoef = (float) (1.0 - std::exp (-(double) controlLength / (0.010 * sr)));

    gateDown = (float) (1.0 - std::exp (-1.0 / (0.004 * sr)));

    setBloom (bloomMs);   // the same time, at the new rate

    profileIndex = -1;
    setProfile (0);
    reset();
}

void TimbreVoice::reset() noexcept
{
    std::fill (heard.begin(), heard.end(), 0.0f);
    std::fill (demodRe.begin(), demodRe.end(), 0.0f);
    std::fill (demodIm.begin(), demodIm.end(), 0.0f);
    std::fill (squared.begin(), squared.end(), 0.0f);
    write = 0;
    sumRe = sumIm = sumSq = 0.0;

    oscRe = 1.0;
    oscIm = 0.0;

    lockRe = lockIm = meanSquare = 0.0f;
    dirRe = 1.0f;
    dirIm = 0.0f;

    lastRe = lastIm = drift = 0.0f;
    settling = 0;
    gate = 0.0f;
    tracked = false;

    amp.fill (0.0f);
    ampStep.fill (0.0f);
    freeAmp.fill (0.0f);
    freeStep.fill (0.0f);
    freeRe.fill (1.0);
    freeIm.fill (0.0);
    active = 0;
    freeActive = 0;
    untilControl = 0;

    period = (float) (sr / 80.0);
    window = (int) std::lround (period);
    setPitch (period, 0.0f);
}

void TimbreVoice::setProfile (int index) noexcept
{
    index = std::clamp (index, 0, numTimbreProfiles - 1);

    if (index == profileIndex)
        return;

    profileIndex = index;
    profile = &timbreProfiles[index];

    const auto& p = *profile;
    const float last = p.harmonic[TimbreProfile::numHarmonics - 1];

    for (int n = 0; n < maxPartials; ++n)
    {
        if (n < TimbreProfile::numHarmonics)
            base[(size_t) n] = std::max (0.0f, p.harmonic[n]);
        else if (p.tailDbPerOctave < 0.0f && last > 0.0f)
            base[(size_t) n] = last * std::pow (10.0f, p.tailDbPerOctave
                                                         * (log2Harmonic[(size_t) n] - 4.0f) / 20.0f);
        else
            base[(size_t) n] = 0.0f;
    }

    for (int i = 0; i < shapeSize; ++i)
    {
        const double log2Hz = 4.0 + i / 12.0;
        double db = 0.0;

        for (const auto& b : p.body)
        {
            if (b.hz > 0.0f && b.octaves > 0.0f && b.db != 0.0f)
            {
                const double d = (log2Hz - std::log2 ((double) b.hz)) / b.octaves;
                db += b.db * std::exp (-0.5 * d * d);
            }
        }

        const double r = std::exp2 (log2Hz) / std::max (100.0, (double) p.topHz);
        shape[(size_t) i] = (float) (std::pow (10.0, db / 20.0) / std::sqrt (1.0 + r * r * r * r));
    }

    numFree = 0;

    while (numFree < TimbreProfile::maxFree && p.free[numFree].ratio > 0.0f)
    {
        freeLog2[(size_t) numFree] = std::log2 (p.free[numFree].ratio);
        ++numFree;
    }

    // The off-series partials turn at their own rates, set from the pitch.
    setPitch (period, tracked ? 1.0f : 0.0f);
}

void TimbreVoice::setBloom (float milliseconds) noexcept
{
    bloomMs = milliseconds;

    // Never quicker than 5 ms: at nothing, the voice would arrive as a
    // click.
    bloomStep = (float) (1.0 / (std::max (5.0, (double) milliseconds) * 0.001 * sr));
}

void TimbreVoice::setWindow (int length) noexcept
{
    length = std::clamp (length, 8, mask - 8);

    // The sums always cover the newest `window` samples; grow or shrink them
    // from the old end.
    while (window < length)
    {
        ++window;
        const auto i = (size_t) ((write - window) & mask);
        sumRe += demodRe[i];
        sumIm += demodIm[i];
        sumSq += squared[i];
    }

    while (window > length)
    {
        const auto i = (size_t) ((write - window) & mask);
        sumRe -= demodRe[i];
        sumIm -= demodIm[i];
        sumSq -= squared[i];
        --window;
    }
}

// The tracker has named a different note. Everything in the one-period
// history was measured against the old pitch and says nothing about the new
// one, and waiting a period for it to turn over is a period of every note
// lost. The tracker only names a note once it has heard a period or two of
// it, so that stretch is already here: measure it again against the phasor
// at the new pitch, run backwards from now, and the lock is good at once.
void TimbreVoice::relock() noexcept
{
    double c = oscRe, s = oscIm;
    sumRe = sumIm = sumSq = 0.0;

    // A little past the window, so a period that then lengthens a few
    // samples does not reach back into the old note's measurements.
    const int span = std::min (mask - 8, window + window / 8 + 8);

    for (int k = 1; k <= span; ++k)
    {
        const auto i = (size_t) ((write - k) & mask);
        const float x = heard[i];

        demodRe[i] = x * (float) c;
        demodIm[i] = -x * (float) s;

        if (k <= window)
        {
            sumRe += (double) demodRe[i];
            sumIm += (double) demodIm[i];
            sumSq += (double) squared[i];
        }

        // One step back: the conjugate turn.
        const double t = c * rotRe + s * rotIm;
        s = s * rotRe - c * rotIm;
        c = t;
    }

    lockRe = lastRe = (float) (sumRe / window);
    lockIm = lastIm = (float) (sumIm / window);
    meanSquare = std::max (0.0f, (float) (sumSq / window));
    drift = 0.0f;
    settling = window / 2;
}

void TimbreVoice::setPitch (float periodSamples, float confidence) noexcept
{
    const float before = period;

    period = std::clamp (periodSamples, 16.0f, (float) (mask - 8));
    tracked = confidence >= confidenceFloor;

    const double step = twoPi / (double) period;
    stepAngle = (float) step;
    rotRe = std::cos (step);
    rotIm = std::sin (step);

    for (int k = 0; k < numFree; ++k)
    {
        const double a = step * (double) profile->free[k].ratio;
        freeRotRe[(size_t) k] = std::cos (a);
        freeRotIm[(size_t) k] = std::sin (a);
    }

    setWindow ((int) std::lround (period));

    // After the one-period average, a little more smoothing at half the
    // note's own rate: enough to steady the phase, quick enough for a bend.
    lockCoef = (float) (1.0 - std::exp (-3.141592653589793 / (double) period));

    if (std::abs (period - before) > maxDrift * before)
        relock();
}

float TimbreVoice::shapeAt (float log2Hz) const noexcept
{
    const float pos = std::clamp ((log2Hz - 4.0f) * 12.0f, 0.0f, (float) (shapeSize - 1) - 0.001f);
    const auto i = (size_t) pos;
    const float f = pos - (float) i;

    return shape[i] + (shape[i + 1] - shape[i]) * f;
}

void TimbreVoice::updatePartials() noexcept
{
    const float f0 = (float) sr / period;
    const float log2F0 = std::log2 (f0);
    const float ceiling = 0.45f * (float) sr;

    float target[maxPartials];
    float freeTarget[TimbreProfile::maxFree];
    float power = 0.0f;

    for (int n = 0; n < maxPartials; ++n)
    {
        const float level = base[(size_t) n];
        float t = 0.0f;

        if (level > 0.0f && (float) (n + 1) * f0 < ceiling)
            t = level * shapeAt (log2F0 + log2Harmonic[(size_t) n]);

        target[n] = t;
        power += t * t;
    }

    for (int k = 0; k < numFree; ++k)
    {
        float t = 0.0f;

        if (profile->free[k].ratio * f0 < ceiling)
            t = profile->free[k].level * shapeAt (log2F0 + freeLog2[(size_t) k]);

        freeTarget[k] = t;
        power += t * t;
    }

    // The whole set at unit level: partials whose squares sum to two make a
    // tone with an RMS of one, which the input's own RMS then scales.
    const float norm = power > 1.0e-12f ? std::sqrt (2.0f / power) : 0.0f;
    const float perSample = 1.0f / (float) controlLength;

    active = 0;

    for (int n = 0; n < maxPartials; ++n)
    {
        const float want = target[n] * norm;
        float& a = amp[(size_t) n];

        if (want <= 0.0f && a < 1.0e-6f)
        {
            a = 0.0f;
            ampStep[(size_t) n] = 0.0f;
            continue;
        }

        ampStep[(size_t) n] = ampCoef * (want - a) * perSample;
        active = n + 1;
    }

    freeActive = 0;

    for (int k = 0; k < TimbreProfile::maxFree; ++k)
    {
        const float want = k < numFree ? freeTarget[k] * norm : 0.0f;
        float& a = freeAmp[(size_t) k];

        if (want <= 0.0f && a < 1.0e-6f)
        {
            a = 0.0f;
            freeStep[(size_t) k] = 0.0f;
        }
        else
        {
            freeStep[(size_t) k] = ampCoef * (want - a) * perSample;
            freeActive = k + 1;   // one a profile change left behind fades at its old rate
        }

        // Keep the turning phasors on the unit circle.
        const double m = freeRe[(size_t) k] * freeRe[(size_t) k] + freeIm[(size_t) k] * freeIm[(size_t) k];

        if (m > 1.0e-12)
        {
            const double inv = 1.0 / std::sqrt (m);
            freeRe[(size_t) k] *= inv;
            freeIm[(size_t) k] *= inv;
        }
        else
        {
            freeRe[(size_t) k] = 1.0;
            freeIm[(size_t) k] = 0.0;
        }
    }
}

void TimbreVoice::process (float* data, int numSamples, bool audible) noexcept
{
    // Rounding lets the phasor's length creep; put it back once a block.
    {
        const double m = oscRe * oscRe + oscIm * oscIm;

        if (m > 1.0e-12)
        {
            const double inv = 1.0 / std::sqrt (m);
            oscRe *= inv;
            oscIm *= inv;
        }
        else
        {
            oscRe = 1.0;
            oscIm = 0.0;
        }
    }

    const float invWindow = 1.0f / (float) window;

    for (int i = 0; i < numSamples; ++i)
    {
        const float x = data[i];

        const double turned = oscRe * rotRe - oscIm * rotIm;
        oscIm = oscIm * rotRe + oscRe * rotIm;
        oscRe = turned;

        const float c = (float) oscRe, s = (float) oscIm;

        // The input against the phasor, averaged over the newest period.
        const float dr = x * c, di = -x * s, sq = x * x;
        const auto leaving = (size_t) ((write - window) & mask);
        const auto entering = (size_t) write;

        heard[entering] = x;

        sumRe += (double) dr - (double) demodRe[leaving];
        sumIm += (double) di - (double) demodIm[leaving];
        sumSq += (double) sq - (double) squared[leaving];

        demodRe[entering] = dr;
        demodIm[entering] = di;
        squared[entering] = sq;
        write = (write + 1) & mask;

        lockRe += lockCoef * ((float) sumRe * invWindow - lockRe);
        lockIm += lockCoef * ((float) sumIm * invWindow - lockIm);
        meanSquare += lockCoef * (std::max (0.0f, (float) sumSq * invWindow) - meanSquare);

        const float found = lockRe * lockRe + lockIm * lockIm;

        if (found > 1.0e-16f)
        {
            const float inv = 1.0f / std::sqrt (found);
            dirRe = lockRe * inv;
            dirIm = lockIm * inv;
        }

        // The fundamental's power is twice `found`: is it a real share of
        // what is coming in?
        const bool present = meanSquare > 1.0e-9f
                               && 2.0f * found > minFundamentalShare * meanSquare;

        // And is it standing still? The cross product of this look and the
        // last is the angle it turned in one sample.
        if (found > 1.0e-16f)
            drift += lockCoef * ((lockIm * lastRe - lockRe * lastIm) / found - drift);

        lastRe = lockRe;
        lastIm = lockIm;

        if (settling > 0)
            --settling;

        const bool steady = settling == 0 && std::abs (drift) < maxDrift * stepAngle;

        const float want = tracked && present && steady ? 1.0f : 0.0f;
        if (want > gate)
            gate = std::min (1.0f, gate + bloomStep);
        else
            gate += gateDown * (want - gate);

        if (! audible)
            continue;

        if (--untilControl <= 0)
        {
            updatePartials();
            untilControl = controlLength;
        }

        if (gate < 1.0e-4f)
        {
            if (want == 0.0f)
                gate = 0.0f;

            continue;   // nothing named: the input stands
        }

        // The string's fundamental as a unit phasor, turned a quarter so
        // that sines of its multiples start with the string's own cosine.
        // Sine phases keep the wave a rounded saw instead of a spike train.
        const float wr = c * dirRe - s * dirIm;
        const float wi = s * dirRe + c * dirIm;
        const float pr = -wi, pi = wr;

        float hr = 1.0f, hi = 0.0f, sum = 0.0f;

        for (int n = 0; n < active; ++n)
        {
            const float t = hr * pr - hi * pi;
            hi = hr * pi + hi * pr;
            hr = t;

            sum += amp[(size_t) n] * hi;
            amp[(size_t) n] += ampStep[(size_t) n];
        }

        for (int k = 0; k < freeActive; ++k)
        {
            const double t = freeRe[(size_t) k] * freeRotRe[(size_t) k] - freeIm[(size_t) k] * freeRotIm[(size_t) k];
            freeIm[(size_t) k] = freeIm[(size_t) k] * freeRotRe[(size_t) k] + freeRe[(size_t) k] * freeRotIm[(size_t) k];
            freeRe[(size_t) k] = t;

            sum += freeAmp[(size_t) k] * (float) freeIm[(size_t) k];
            freeAmp[(size_t) k] += freeStep[(size_t) k];
        }

        const float tone = std::sqrt (meanSquare) * sum;
        data[i] = x + (tone - x) * gate;
    }
}

} // namespace rig
