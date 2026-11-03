#include "PitchTracker.h"

#include <algorithm>
#include <cmath>

namespace rig
{

static int nextPow2 (int n)
{
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

void PitchTracker::prepare (double sampleRate, double decimatedRate, double lowestHz, double highestHz)
{
    sr = sampleRate;

    decim   = std::max (1, (int) std::lround (sr / decimatedRate));
    decRate = sr / decim;

    // 6th-order Butterworth at 700 Hz: passes every bass/guitar fundamental,
    // ~40 dB down by the decimated Nyquist.
    const double aaFreq = std::min (highestHz * 1.4, decRate * 0.24);
    aa[0].setLowpass (sr, aaFreq, 0.51764);
    aa[1].setLowpass (sr, aaFreq, 0.70711);
    aa[2].setLowpass (sr, aaFreq, 1.93185);

    tauMin = std::max (3, (int) std::floor (decRate / highestHz));   // up to 500 Hz
    tauMax = std::min (200, (int) std::ceil (decRate / lowestHz));   // down to 26 Hz: a
                                                                 // 5-string low B, and
                                                                 // drop tunings under it
    window = 256;                                                // ~85 ms: two and a bit
                                                                 // periods even down there
    analysisHop = 32;                                            // ~10 ms

    const int decNeeded = nextPow2 (window + tauMax + 8);
    decBuf.assign ((size_t) decNeeded, 0.0f);
    decMask = decNeeded - 1;

    // Room for the correlation window plus the longest period we can report,
    // twice over, so the refinement never has to clamp its own answer.
    maxPeriod = (float) ((tauMax + 1) * decim);
    const int frNeeded = nextPow2 (std::max (4096, 2048 + 2 * (int) maxPeriod));
    frBuf.assign ((size_t) frNeeded, 0.0f);
    frMask = frNeeded - 1;

    scratch.assign ((size_t) (window + tauMax + 4), 0.0f);
    frScratch.assign ((size_t) frNeeded, 0.0f);
    diff.assign ((size_t) (tauMax + 2), 0.0f);
    cmnd.assign ((size_t) (tauMax + 2), 0.0f);
    diffFull.assign ((size_t) (2 * decim + 4), 0.0f);

    reset();
}

void PitchTracker::reset() noexcept
{
    for (auto& f : aa) f.reset();
    std::fill (decBuf.begin(), decBuf.end(), 0.0f);
    std::fill (frBuf.begin(), frBuf.end(), 0.0f);
    decCounter = decWrite = frWrite = sinceAnalysis = 0;
    periodSamples = (float) (sr / 80.0);
    confidence = 0.0f;
}

void PitchTracker::process (const float* in, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
    {
        const float x = in[i];

        frBuf[(size_t) (frWrite & frMask)] = x;
        ++frWrite;

        float f = aa[0].process (x);
        f = aa[1].process (f);
        f = aa[2].process (f);

        if (++decCounter >= decim)
        {
            decCounter = 0;
            decBuf[(size_t) (decWrite & decMask)] = f;
            ++decWrite;

            if (++sinceAnalysis >= analysisHop)
            {
                sinceAnalysis = 0;
                analyse();
            }
        }
    }
}

void PitchTracker::analyse() noexcept
{
    const int W = window;
    const int tMax = tauMax;
    const int total = W + tMax;

    for (int i = 0; i < total; ++i)
        scratch[(size_t) i] = decBuf[(size_t) ((decWrite - total + i) & decMask)];

    // Analysis window is the newest W samples; lags reach backwards from there.
    const float* w = scratch.data() + tMax;

    double energy = 0.0;
    for (int j = 0; j < W; ++j)
        energy += (double) w[j] * (double) w[j];

    const float rms = (float) std::sqrt (energy / W);

    if (rms < 0.0007f)   // ~ -63 dBFS: nothing worth tracking
    {
        confidence = 0.0f;
        return;
    }

    for (int tau = tauMin; tau <= tMax; ++tau)
    {
        float sum = 0.0f;
        const float* lagged = w - tau;

        for (int j = 0; j < W; ++j)
        {
            const float d = w[j] - lagged[j];
            sum += d * d;
        }

        diff[(size_t) tau] = sum;
    }

    // Cumulative mean normalised difference.
    float running = 0.0f;
    for (int tau = tauMin; tau <= tMax; ++tau)
    {
        running += diff[(size_t) tau];
        cmnd[(size_t) tau] = running > 1.0e-12f
                                ? diff[(size_t) tau] * (float) (tau - tauMin + 1) / running
                                : 1.0f;
    }

    // Absolute threshold: take the *first* dip below it, which is what keeps
    // YIN off the sub-octave.
    constexpr float threshold = 0.15f;
    int best = -1;

    for (int tau = tauMin; tau <= tMax - 1; ++tau)
    {
        if (cmnd[(size_t) tau] < threshold)
        {
            int t = tau;
            while (t + 1 <= tMax && cmnd[(size_t) (t + 1)] < cmnd[(size_t) t])
                ++t;

            best = t;
            break;
        }
    }

    if (best < 0)
    {
        best = tauMin;
        for (int tau = tauMin + 1; tau <= tMax; ++tau)
            if (cmnd[(size_t) tau] < cmnd[(size_t) best])
                best = tau;
    }

    // The first dip keeps YIN off the sub-octave, but a dominant second
    // harmonic sinks a dip at half the true period too — the classic way a
    // bass tracker reads an octave up. The tell is asymmetric: a false
    // half-period dip is always mediocre (the fundamental refuses to line
    // up), while the true period below it is excellent. So a rescue is only
    // considered when the winning dip is imperfect, and only a clearly
    // better dip at double the lag takes it — on a clean tone both dips sit
    // at the noise floor and nothing moves.
    if (2 * best + 2 <= tMax && cmnd[(size_t) best] > 0.02f)
    {
        int doubled = 2 * best;

        for (int tau = 2 * best - 2; tau <= 2 * best + 2; ++tau)
            if (cmnd[(size_t) tau] < cmnd[(size_t) doubled])
                doubled = tau;

        if (cmnd[(size_t) doubled] < 0.5f * cmnd[(size_t) best]
             && cmnd[(size_t) doubled] < 0.03f)
            best = doubled;
    }

    const float quality = cmnd[(size_t) best];

    if (quality > 0.4f)          // noisy, chordal or transient — hold the last period
    {
        confidence = 0.0f;
        return;
    }

    float tauEst = (float) best;

    if (best > tauMin && best < tMax)
    {
        const float s0 = cmnd[(size_t) (best - 1)];
        const float s1 = cmnd[(size_t) best];
        const float s2 = cmnd[(size_t) (best + 1)];
        const float denom = s0 + s2 - 2.0f * s1;

        if (std::abs (denom) > 1.0e-9f)
            tauEst = (float) best + 0.5f * (s0 - s2) / denom;
    }

    const float conf = std::clamp (1.0f - quality, 0.0f, 1.0f);
    const float coarse = tauEst * (float) decim;
    coarsePeriod = coarse;
    const float refined = refineAtFullRate (coarse);

    // Reject single-frame outliers unless we are confident about them.
    const float delta = std::abs (refined - periodSamples);

    if (conf < 0.7f && delta > 0.05f * periodSamples && confidence > 0.5f)
    {
        confidence = conf * 0.5f;
        return;
    }

    periodSamples = refined;
    confidence = conf;
}

float PitchTracker::refineAtFullRate (float coarsePeriodIn) noexcept
{
    const int p0 = (int) std::lround (coarsePeriodIn);
    const int search = std::max (1, decim);
    const int lo = std::max (8, p0 - search);
    const int hi = p0 + search;

    const int Wf = std::clamp (p0, 256, 1024);

    if (Wf + hi + 4 >= (int) frBuf.size())
        return coarsePeriodIn;

    const int total = Wf + hi;

    for (int i = 0; i < total; ++i)
        frScratch[(size_t) i] = frBuf[(size_t) ((frWrite - total + i) & frMask)];

    const float* w = frScratch.data() + hi;

    double energy = 0.0;

    for (int j = 0; j < Wf; ++j)
        energy += (double) w[j] * w[j];

    if (energy < 1.0e-12)
        return coarsePeriodIn;

    int bestLag = p0;
    float bestVal = -2.0f;

    for (int lag = lo; lag <= hi; ++lag)
    {
        const float* lagged = w - lag;
        double num = 0.0, laggedEnergy = 0.0;

        for (int j = 0; j < Wf; ++j)
        {
            num += (double) w[j] * lagged[j];
            laggedEnergy += (double) lagged[j] * lagged[j];
        }

        // Normalised, so a decaying note cannot bias the answer towards
        // shorter lags the way a plain difference function does.
        const auto score = (float) (num / (std::sqrt (energy * laggedEnergy) + 1.0e-18));
        diffFull[(size_t) (lag - lo)] = score;

        if (score > bestVal)
        {
            bestVal = score;
            bestLag = lag;
        }
    }

    float result = (float) bestLag;

    if (bestLag > lo && bestLag < hi)
    {
        const float s0 = diffFull[(size_t) (bestLag - 1 - lo)];
        const float s1 = diffFull[(size_t) (bestLag - lo)];
        const float s2 = diffFull[(size_t) (bestLag + 1 - lo)];
        const float denom = s0 + s2 - 2.0f * s1;

        if (std::abs (denom) > 1.0e-12f)
            result = (float) bestLag - 0.5f * (s2 - s0) / denom;
    }

    return std::clamp (result, 8.0f, maxPeriod);
}

} // namespace rig
