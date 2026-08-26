#include "PlateVerb.h"

#include <algorithm>
#include <cmath>

namespace rig
{

// All delay lengths, tap points and the excursion are quoted at 29761 Hz —
// the rate Dattorro published — and multiplied up to the running rate. The
// tank lengths additionally stretch with Size; the input diffusers stay put
// so the attack keeps its character while the room grows.
static constexpr float kInDiffLen[4] { 142.0f, 107.0f, 379.0f, 277.0f };
static constexpr float kInDiffG[4]   { 0.75f, 0.75f, 0.625f, 0.625f };

static constexpr float kModApA = 672.0f,  kModApB = 908.0f;
static constexpr float kDelayA1 = 4453.0f, kDelayB1 = 4217.0f;
static constexpr float kApA2 = 1800.0f,   kApB2 = 2656.0f;
static constexpr float kDelayA2 = 3720.0f, kDelayB2 = 3163.0f;
static constexpr float kExcursion = 16.0f;

// One trip around the figure-eight, in 29761-domain samples. The loop gain
// needed for a wanted RT60 falls out of this and the four decay multiplies
// the signal meets per trip.
static constexpr float kCycleLen = kModApA + kDelayA1 + kApA2 + kDelayA2
                                 + kModApB + kDelayB1 + kApB2 + kDelayB2;

static constexpr float kMinScale = 0.35f, kMaxScale = 1.10f;

// Exact change detection without tripping -Wfloat-equal: the targets are
// copied verbatim from the parameter store, so ordering catches any change.
static inline bool differs (float a, float b) noexcept
{
    return a < b || a > b;
}

static int nextPow2 (int n)
{
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

// The same 4-point Hermite the chorus and delay taps use: the tank reads move
// (size glide, LFO excursion), and a moving tap through linear interpolation
// dulls and hisses.
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

void PlateVerb::Line::prepare (int capacity)
{
    const int p = nextPow2 (capacity + 8);
    buf.assign ((size_t) p, 0.0f);
    mask = p - 1;
    write = p;
}

void PlateVerb::Line::clear() noexcept
{
    std::fill (buf.begin(), buf.end(), 0.0f);
}

float PlateVerb::Line::read (float delaySamples) const noexcept
{
    const float lo = 4.0f;
    const float hi = (float) ((int) buf.size() - 8);
    const float d = delaySamples < lo ? lo : (delaySamples > hi ? hi : delaySamples);

    return readHermite (buf, (double) write - (double) d, mask);
}

void PlateVerb::prepare (double sampleRate, int maxBlockSize)
{
    (void) maxBlockSize;   // per-sample algorithm; nothing scales with the block

    sr = sampleRate;
    srScale = (float) (sr / 29761.0);

    const auto tankCap = [this] (float len29k)
    {
        return (int) std::ceil (len29k * srScale * kMaxScale + kExcursion * srScale) + 16;
    };

    pre.prepare ((int) std::ceil (0.130 * sr) + 16);

    for (int i = 0; i < 4; ++i)
    {
        inDiff[i].line.prepare ((int) std::ceil (kInDiffLen[i] * srScale) + 16);
        inDiff[i].g = kInDiffG[i];
    }

    modApA.line.prepare (tankCap (kModApA));
    modApB.line.prepare (tankCap (kModApB));
    apA2.line.prepare (tankCap (kApA2));
    apB2.line.prepare (tankCap (kApB2));
    delayA1.prepare (tankCap (kDelayA1));
    delayB1.prepare (tankCap (kDelayB1));
    delayA2.prepare (tankCap (kDelayA2));
    delayB2.prepare (tankCap (kDelayB2));

    // The tank's first allpasses run inverted relative to the diffusers, per
    // the paper — the output taps' signs assume it.
    modApA.g = -0.70f;
    modApB.g = -0.70f;

    bandwidth.setCutoff (sr, 10500.0);
    lowcutA.setCutoff (sr, 90.0);
    lowcutB.setCutoff (sr, 90.0);

    const auto smoother = [this] (double seconds)
    {
        return (float) (1.0 - std::exp (-1.0 / (seconds * sr)));
    };

    scaleCoef = smoother (0.080);
    decayCoef = smoother (0.030);
    excCoef   = smoother (0.050);
    preCoef   = smoother (0.100);

    lastSize = lastDamp = lastPreMs = lastMod = -1.0f;
    updateTargets();
    reset();
}

void PlateVerb::reset() noexcept
{
    pre.clear();

    for (auto& d : inDiff)
        d.line.clear();

    modApA.line.clear();
    modApB.line.clear();
    apA2.line.clear();
    apB2.line.clear();
    delayA1.clear();
    delayB1.clear();
    delayA2.clear();
    delayB2.clear();

    bandwidth.reset();
    dampA.reset();
    dampB.reset();
    lowcutA.reset();
    lowcutB.reset();

    lfoPhaseA = 0.0;
    lfoPhaseB = 0.5;

    snapOnNext = true;
}

void PlateVerb::updateTargets() noexcept
{
    if (differs (sizeTarget, lastSize))
    {
        lastSize = sizeTarget;
        scaleGoal = kMinScale + (kMaxScale - kMinScale) * sizeTarget;

        // Size commands a tail: half a second when tight up to eight seconds
        // wide open, exponential so the knob feels even. The loop gain that
        // produces it depends on how long one trip around the (stretched)
        // tank takes and on the four decay gains met per trip.
        const float rt60 = 0.5f * std::exp (2.7726f * sizeTarget);
        const float cycleSeconds = kCycleLen * scaleGoal / 29761.0f;

        decayGoal = std::pow (10.0f, -3.0f * cycleSeconds / (4.0f * rt60));
        decayGoal = decayGoal > 0.9995f ? 0.9995f : decayGoal;

        // Decay diffusion tracks decay, capped where the paper caps it.
        g2 = decayGoal + 0.15f;
        g2 = g2 < 0.25f ? 0.25f : (g2 > 0.50f ? 0.50f : g2);
        apA2.g = g2;
        apB2.g = g2;
    }

    if (differs (dampTarget, lastDamp))
    {
        lastDamp = dampTarget;

        // 18 kHz (open) down to 1 kHz (dark), log-spaced so the middle of the
        // knob sits in the musically useful few-kHz region.
        const double fc = 18000.0 * std::exp ((double) dampTarget * std::log (1000.0 / 18000.0));
        dampA.setCutoff (sr, fc);
        dampB.setCutoff (sr, fc);
    }

    if (differs (preMsTarget, lastPreMs))
    {
        lastPreMs = preMsTarget;

        const float cap = (float) ((int) pre.buf.size() - 12);
        const float samples = preMsTarget * 0.001f * (float) sr;
        preGoal = samples < 1.0f ? 1.0f : (samples > cap ? cap : samples);
    }

    if (differs (modTarget, lastMod))
    {
        lastMod = modTarget;
        excGoal = modTarget * kExcursion * srScale;
    }
}

void PlateVerb::process (const float* in, float* left, float* right, int numSamples) noexcept
{
    updateTargets();

    if (snapOnNext)
    {
        snapOnNext = false;
        scaleState = scaleGoal;
        decayState = decayGoal;
        excState = excGoal;
        preState = preGoal;
    }

    const double lfoIncA = 0.87 / sr;
    const double lfoIncB = 1.09 / sr;

    for (int i = 0; i < numSamples; ++i)
    {
        scaleState += scaleCoef * (scaleGoal - scaleState);
        decayState += decayCoef * (decayGoal - decayState);
        excState   += excCoef   * (excGoal   - excState);
        preState   += preCoef   * (preGoal   - preState);

        const float k = srScale * scaleState;

        lfoPhaseA += lfoIncA;
        if (lfoPhaseA >= 1.0) lfoPhaseA -= 1.0;
        lfoPhaseB += lfoIncB;
        if (lfoPhaseB >= 1.0) lfoPhaseB -= 1.0;

        const float excA = excState * std::sin ((float) (6.283185307179586 * lfoPhaseA));
        const float excB = excState * std::sin ((float) (6.283185307179586 * lfoPhaseB));

        // Tails leaving each half a sample ago, before anything is pushed.
        const float fbA = delayA2.read (kDelayA2 * k);
        const float fbB = delayB2.read (kDelayB2 * k);

        // Input: predelay, bandwidth, four diffusers. The diffused signal
        // feeds both halves of the tank.
        float x = pre.read (preState);
        pre.push (in[i]);

        x = bandwidth.process (x);

        for (int d = 0; d < 4; ++d)
            x = inDiff[d].process (x, kInDiffLen[d] * srScale);

        // Half A, fed by B's tail. The tiny alternating offset keeps the
        // recirculation out of denormal territory when the input goes silent.
        float a = x + decayState * fbB + dn;
        dn = -dn;
        a = modApA.process (a, kModApA * k + excA);

        float vA = delayA1.read (kDelayA1 * k);
        delayA1.push (a);
        vA = dampA.process (vA);
        vA -= lowcutA.process (vA);   // low cut: lows die faster than the tail
        vA *= decayState;
        vA = apA2.process (vA, kApA2 * k);
        delayA2.push (vA);

        // Half B, fed by A's tail.
        float b = x + decayState * fbA + dn;
        b = modApB.process (b, kModApB * k + excB);

        float vB = delayB1.read (kDelayB1 * k);
        delayB1.push (b);
        vB = dampB.process (vB);
        vB -= lowcutB.process (vB);
        vB *= decayState;
        vB = apB2.process (vB, kApB2 * k);
        delayB2.push (vB);

        // Each ear gathers from both halves at the paper's tap points — the
        // seven-term sums are what decorrelates the pair.
        const float yL = delayB1.read (266.0f * k)  + delayB1.read (2974.0f * k)
                       - apB2.line.read (1913.0f * k) + delayB2.read (1996.0f * k)
                       - delayA1.read (1990.0f * k) - apA2.line.read (187.0f * k)
                       - delayA2.read (1066.0f * k);

        const float yR = delayA1.read (353.0f * k)  + delayA1.read (3627.0f * k)
                       - apA2.line.read (1228.0f * k) + delayA2.read (2673.0f * k)
                       - delayB1.read (2111.0f * k) - apB2.line.read (335.0f * k)
                       - delayB2.read (121.0f * k);

        left[i]  = 0.6f * yL;
        right[i] = 0.6f * yR;
    }
}

} // namespace rig
