#include "SpectralFreeze.h"

#include <algorithm>
#include <cmath>

namespace rig
{

// The two chorus taps: base delay and LFO rate. Slow and unequal, so the
// drift never settles into a wobble you can count.
static constexpr float motionBaseMs[2] { 9.0f, 14.0f };
static constexpr float motionRateHz[2] { 0.29f, 0.41f };
static constexpr float motionDepthMs = 2.5f;

// No fade is ever shorter than this: a layer that appears or vanishes inside
// one cycle of a bass note is a click, whatever the knob says.
static constexpr float shortestFadeMs = 4.0f;

static inline float readHermite (const std::vector<float>& buf, double pos, int mask) noexcept
{
    const auto i = (long long) std::floor (pos);
    const float f = (float) (pos - (double) i);

    const float xm1 = buf[(size_t) ((i - 1) & mask)];
    const float x0  = buf[(size_t) ( i      & mask)];
    const float x1  = buf[(size_t) ((i + 1) & mask)];
    const float x2  = buf[(size_t) ((i + 2) & mask)];

    const float c = (x1 - xm1) * 0.5f;
    const float v = x0 - x1;
    const float w = c + v;
    const float a = w + v + (x2 - x0) * 0.5f;
    const float b = w + a;

    return ((a * f - b) * f + c) * f + x0;
}

void SpectralFreeze::prepare (double sampleRate)
{
    sr = sampleRate;

    // The window is a length of time, not a count: twice the points at the
    // double rates keeps the same 170 ms and the same reach into the lows.
    const int order = sampleRate > 50000.0 ? 14 : 13;
    size = 1 << order;
    hop = size / 4;
    bins = size / 2 + 1;

    fft = std::make_unique<juce::dsp::FFT> (order);

    window.resize ((size_t) size);
    synthWindow.resize ((size_t) size);

    for (int i = 0; i < size; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (6.283185307179586 * (double) i / (double) size);
        window[(size_t) i] = (float) w;

        // Hann in, Hann out, four frames deep: the squares sum to 1.5.
        synthWindow[(size_t) i] = (float) (w / 1.5);
    }

    work.assign ((size_t) size * 2, 0.0f);
    earlier.assign ((size_t) size * 2, 0.0f);
    power.assign ((size_t) bins, 0.0f);
    shapes.assign ((size_t) bins, 0.0f);

    history.assign ((size_t) size * 2, 0.0f);
    historyMask = size * 2 - 1;

    for (auto& v : voices)
    {
        v.spec.assign ((size_t) bins * 2, 0.0f);
        v.turn.assign ((size_t) bins * 2, 0.0f);
        v.ola.assign ((size_t) size, 0.0f);
    }

    int motionSize = 1;
    while (motionSize < (int) (sampleRate * 0.03))
        motionSize <<= 1;

    motionBuf.assign ((size_t) motionSize, 0.0f);
    motionMask = motionSize - 1;
    for (auto& stage : motionLows)
        stage.setHighpass (sr, 150.0, 0.7071);

    setFadeTimes (100.0f, 150.0f);

    // The first transform of a size pays for the machinery behind it. Pay
    // here, on the device thread, so the first press on stage costs what
    // every later one does.
    analyse (earlier.data(), 0);
    addFrame (voices[0], 0);
    addFrame (voices[1], 0);

    reset();
}

void SpectralFreeze::reset() noexcept
{
    std::fill (history.begin(), history.end(), 0.0f);
    historyWrite = 0;

    for (auto& v : voices)
    {
        std::fill (v.ola.begin(), v.ola.end(), 0.0f);
        v.read = 0;
        v.untilFrame = 0;
        v.gain = 0.0f;
        v.held = false;
        v.active = false;
    }

    wasHolding = false;
    untilCapture = -1;

    std::fill (motionBuf.begin(), motionBuf.end(), 0.0f);
    motionWrite = (int) motionBuf.size();
    motionPhase[0] = 0.0;
    motionPhase[1] = 0.25;
    motion = motionTarget;

    for (auto& stage : motionLows)
        stage.reset();
}

void SpectralFreeze::setFadeTimes (float inMs, float outMs) noexcept
{
    inStep  = (float) (1000.0 / (sr * (double) std::max (shortestFadeMs, inMs)));
    outStep = (float) (1000.0 / (sr * (double) std::max (shortestFadeMs, outMs)));
}

void SpectralFreeze::setMotion (float amount) noexcept
{
    motionTarget = std::clamp (amount, 0.0f, 1.0f);
}

// The windowed spectrum of the stretch that ended samplesAgo samples back.
void SpectralFreeze::analyse (float* dest, int samplesAgo) noexcept
{
    const int start = historyWrite - samplesAgo - size;

    for (int i = 0; i < size; ++i)
        dest[i] = history[(size_t) ((start + i) & historyMask)] * window[(size_t) i];

    fft->performRealOnlyForwardTransform (dest, true);
}

void SpectralFreeze::turnSpectrum (Voice& v) noexcept
{
    float* s = v.spec.data();
    const float* t = v.turn.data();

    for (int k = 0; k < bins; ++k)
    {
        const float re = s[2 * k], im = s[2 * k + 1];
        s[2 * k]     = re * t[2 * k] - im * t[2 * k + 1];
        s[2 * k + 1] = re * t[2 * k + 1] + im * t[2 * k];
    }
}

// One frame of the held spectrum into the overlap-add, at the read point.
// skip drops the head of a frame that began before the press.
void SpectralFreeze::addFrame (Voice& v, int skip) noexcept
{
    std::copy (v.spec.begin(), v.spec.end(), work.begin());
    fft->performRealOnlyInverseTransform (work.data());

    const int mask = size - 1;

    for (int i = skip; i < size; ++i)
        v.ola[(size_t) ((v.read + i - skip) & mask)] += work[(size_t) i] * synthWindow[(size_t) i];
}

// Every bin between two valleys of the spectrum belongs to the peak they
// enclose, and the region is held as what it stands for: one steady partial.
// Its rate is the energy-weighted turn of its bins between the two frames,
// its strength is the energy that was there, and its shape is redrawn as the
// spectrum a steady sinusoid at that rate would have had.
//
// Keeping the bins as they were caught does not survive real playing. A
// frame that held a note change or a pick attack repeats that event every
// hop — a 23 Hz purr under the hold — and bins left to turn at their own
// measured rates drift apart until the note thins. Redrawn, whatever was in
// the window comes out as a level chord of partials, with nothing in it
// that knows where the frame edges were.
void SpectralFreeze::holdRegion (Voice& v, int from, int to, int peak, double floor) noexcept
{
    constexpr double pi = 3.141592653589793;

    double cr = 0.0, ci = 0.0, energy = 0.0;

    for (int k = from; k < to; ++k)
    {
        const double ar = earlier[(size_t) (2 * k)], ai = earlier[(size_t) (2 * k + 1)];
        const double br = work[(size_t) (2 * k)],    bi = work[(size_t) (2 * k + 1)];

        cr += br * ar + bi * ai;
        ci += bi * ar - br * ai;
        energy += br * br + bi * bi;
    }

    // Hiss is not a note. What sits 80 dB under the sound being held is left
    // out rather than turned into a thousand faint whistles — and most of
    // the spectrum is exactly that, so most of the press costs nothing.
    if (energy <= floor)
    {
        std::fill (v.spec.begin() + 2 * from, v.spec.begin() + 2 * to, 0.0f);
        std::fill (v.turn.begin() + 2 * from, v.turn.begin() + 2 * to, 0.0f);
        return;
    }

    // How far past the peak bin the partial really sits: a bin's own centre
    // turns a quarter circle per hop per bin number, and the rest of the
    // measured turn is the offset. Without an earlier frame to measure
    // against — the sound began inside the last hop — it sits on the bin.
    double offset = 0.0;

    if (cr * cr + ci * ci > 1.0e-30)
    {
        const double beyond = std::atan2 (ci, cr) - 0.5 * pi * (double) peak;
        offset = std::clamp ((beyond - 2.0 * pi * std::round (beyond / (2.0 * pi))) * 2.0 / pi, -1.0, 1.0);
    }

    const double centre = (double) peak + offset;
    const auto turnRe = (float) std::cos (0.5 * pi * centre);
    const auto turnIm = (float) std::sin (0.5 * pi * centre);

    // The Hann window's spectrum around a sinusoid at `centre`, bin by bin.
    // The alternating sign of neighbouring bins and of sin (pi d) cancel, so
    // one sine serves the whole region.
    const double lobe = -std::sin (pi * offset) / pi;
    double shapeEnergy = 0.0;

    for (int k = from; k < to; ++k)
    {
        const double d = (double) k - centre;
        const double parity = ((k - peak) & 1) != 0 ? -1.0 : 1.0;

        const double shape = std::abs (d) < 1.0e-4               ? parity
                           : std::abs (std::abs (d) - 1.0) < 1.0e-4 ? 0.5 * parity
                                                                    : lobe / (d * (1.0 - d * d));

        shapes[(size_t) k] = (float) shape;
        shapeEnergy += shape * shape;
    }

    const double peakRe = work[(size_t) (2 * peak)], peakIm = work[(size_t) (2 * peak + 1)];
    const double peakMag = std::sqrt (peakRe * peakRe + peakIm * peakIm);
    const double scale = shapeEnergy > 1.0e-30 && peakMag > 1.0e-30
                           ? std::sqrt (energy / shapeEnergy) / peakMag : 0.0;

    for (int k = from; k < to; ++k)
    {
        // In phase with the peak as it was caught, so the layer still comes
        // in as the continuation of the note it holds.
        v.spec[(size_t) (2 * k)]     = (float) (scale * peakRe * (double) shapes[(size_t) k]);
        v.spec[(size_t) (2 * k + 1)] = (float) (scale * peakIm * (double) shapes[(size_t) k]);
        v.turn[(size_t) (2 * k)]     = turnRe;
        v.turn[(size_t) (2 * k + 1)] = turnIm;
    }
}

void SpectralFreeze::capture (Voice& v) noexcept
{
    analyse (earlier.data(), hop);
    analyse (work.data(), 0);

    double total = 0.0;

    for (int k = 0; k < bins; ++k)
    {
        const float br = work[(size_t) (2 * k)], bi = work[(size_t) (2 * k + 1)];
        power[(size_t) k] = br * br + bi * bi;
        total += (double) power[(size_t) k];
    }

    const double floor = total * 1.0e-8;

    // A peak stands above two neighbours each side; its region runs from the
    // lowest bin since the last peak to the lowest before the next.
    const auto at = [this] (int k) { return k >= 0 && k < bins ? power[(size_t) k] : -1.0f; };

    int regionStart = 0, lastPeak = -1;

    for (int k = 0; k < bins; ++k)
    {
        const float p = at (k);

        if (! (p > at (k - 1) && p > at (k - 2) && p >= at (k + 1) && p >= at (k + 2)))
            continue;

        if (lastPeak >= 0)
        {
            int valley = lastPeak + 1;

            for (int j = lastPeak + 1; j < k; ++j)
                if (power[(size_t) j] < power[(size_t) valley])
                    valley = j;

            holdRegion (v, regionStart, valley, lastPeak, floor);
            regionStart = valley;
        }

        lastPeak = k;
    }

    if (lastPeak >= 0)
    {
        holdRegion (v, regionStart, bins, lastPeak, floor);
    }
    else
    {
        // Silence in, silence held.
        std::fill (v.spec.begin(), v.spec.end(), 0.0f);
        std::fill (v.turn.begin(), v.turn.end(), 0.0f);
    }

    if (! v.active)
    {
        // An idle layer starts whole: the three frames that would already be
        // sounding had it been running are laid in with their heads cut off,
        // so the first sample out is at full depth and Fade in is the only
        // fade there is.
        std::fill (v.ola.begin(), v.ola.end(), 0.0f);
        v.read = 0;
        v.gain = 0.0f;

        for (int ahead = 1; ahead <= 4; ++ahead)
        {
            turnSpectrum (v);
            addFrame (v, (4 - ahead) * hop);
        }
    }
    else
    {
        // A stolen layer keeps what it has in the air and lets it run out
        // under the new frames — the windows are the crossfade.
        for (int ahead = 1; ahead <= 4; ++ahead)
            turnSpectrum (v);

        addFrame (v, 0);
    }

    v.untilFrame = hop;
    v.held = true;
    v.active = true;
}

void SpectralFreeze::render (Voice& v, float* layer, int numSamples) noexcept
{
    const int mask = size - 1;
    int done = 0;

    while (done < numSamples)
    {
        if (v.untilFrame == 0)
        {
            turnSpectrum (v);
            addFrame (v, 0);
            v.untilFrame = hop;
        }

        const int run = std::min (numSamples - done, v.untilFrame);
        const float target = v.held ? 1.0f : 0.0f;
        const float step = v.held ? inStep : outStep;

        for (int i = 0; i < run; ++i)
        {
            const auto at = (size_t) ((v.read + i) & mask);
            const float s = v.ola[at];
            v.ola[at] = 0.0f;

            if (v.gain < target)        v.gain = std::min (target, v.gain + step);
            else if (v.gain > target)   v.gain = std::max (target, v.gain - step);

            layer[done + i] += s * v.gain;
        }

        v.read = (v.read + run) & mask;
        v.untilFrame -= run;
        done += run;

        if (! v.held && v.gain <= 0.0f)
        {
            v.active = false;
            return;
        }
    }
}

void SpectralFreeze::addMotion (float* layer, int numSamples) noexcept
{
    const int mask = motionMask;
    const float depthSamples = motionDepthMs * 0.001f * (float) sr;

    double phaseInc[2];
    float baseSamples[2];

    for (int t = 0; t < 2; ++t)
    {
        phaseInc[t] = (double) motionRateHz[t] / sr;
        baseSamples[t] = motionBaseMs[t] * 0.001f * (float) sr;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        motion += 0.0005f * (motionTarget - motion);

        const float x = layer[i];
        motionBuf[(size_t) (motionWrite & mask)] = x;

        float wet = 0.0f;

        for (int t = 0; t < 2; ++t)
        {
            motionPhase[t] += phaseInc[t];
            if (motionPhase[t] >= 1.0) motionPhase[t] -= 1.0;

            const float lfo = std::sin ((float) (6.283185307179586 * motionPhase[t]));
            const double tap = (double) baseSamples[t] + (double) (depthSamples * motion * lfo);

            wet += 0.5f * readHermite (motionBuf, (double) motionWrite - tap, mask);
        }

        if (++motionWrite >= (int) motionBuf.size() * 2)
            motionWrite -= (int) motionBuf.size();

        // Equal power between the still layer and its two drifting copies,
        // so the knob changes how much it moves and not how loud it is. Only
        // the difference is added back, through a highpass: below it the
        // layer is exactly what was held.
        const float angle = motion * 0.7853982f;
        const float moved = x * std::cos (angle) + wet * std::sin (angle) * 1.4142136f;

        layer[i] = x + motionLows[1].process (motionLows[0].process (moved - x));
    }
}

void SpectralFreeze::process (const float* in, float* layer, int numSamples, bool hold) noexcept
{
    if (hold && ! wasHolding)
    {
        for (auto& v : voices)
            v.held = false;

        untilCapture = hop;
    }
    else if (! hold && wasHolding)
    {
        for (auto& v : voices)
            v.held = false;

        untilCapture = -1;
    }

    if (untilCapture >= 0 && (untilCapture -= numSamples) < 0)
    {
        untilCapture = -1;

        // The idle layer if there is one, else whichever has faded further.
        Voice* next = ! voices[0].active ? &voices[0]
                    : ! voices[1].active ? &voices[1]
                    : voices[0].gain <= voices[1].gain ? &voices[0] : &voices[1];

        if (! isSounding())
        {
            std::fill (motionBuf.begin(), motionBuf.end(), 0.0f);

            for (auto& stage : motionLows)
                stage.reset();
        }

        // Before this block is heard: the capture ends where the layer
        // begins, which is what keeps the two in phase.
        capture (*next);
    }

    wasHolding = hold;

    for (int i = 0; i < numSamples; ++i)
        history[(size_t) ((historyWrite + i) & historyMask)] = in[i];

    historyWrite = (historyWrite + numSamples) & historyMask;

    std::fill (layer, layer + numSamples, 0.0f);

    if (! isSounding())
        return;

    for (auto& v : voices)
        if (v.active)
            render (v, layer, numSamples);

    addMotion (layer, numSamples);
}

} // namespace rig
