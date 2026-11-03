#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace rig
{

// RBJ cookbook biquad, transposed direct form II.
// Coefficients are computed in place: no allocation, safe to call from the
// audio thread at control rate.
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    inline float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void reset() noexcept { z1 = z2 = 0.0f; }

    void setCoefficients (double bb0, double bb1, double bb2,
                          double aa0, double aa1, double aa2) noexcept
    {
        const double inv = 1.0 / aa0;
        b0 = (float) (bb0 * inv);
        b1 = (float) (bb1 * inv);
        b2 = (float) (bb2 * inv);
        a1 = (float) (aa1 * inv);
        a2 = (float) (aa2 * inv);
    }

    void setLowpass (double sr, double freq, double q) noexcept
    {
        const double w0 = 6.283185307179586 * clampFreq (sr, freq);
        const double cs = std::cos (w0), sn = std::sin (w0);
        const double al = sn / (2.0 * q);
        setCoefficients ((1.0 - cs) * 0.5, 1.0 - cs, (1.0 - cs) * 0.5,
                         1.0 + al, -2.0 * cs, 1.0 - al);
    }

    // Constant 0 dB peak at the centre: one band of a filter bank.
    void setBandpass (double sr, double freq, double q) noexcept
    {
        const double w0 = 6.283185307179586 * clampFreq (sr, freq);
        const double cs = std::cos (w0), sn = std::sin (w0);
        const double al = sn / (2.0 * q);
        setCoefficients (al, 0.0, -al, 1.0 + al, -2.0 * cs, 1.0 - al);
    }

    void setHighpass (double sr, double freq, double q) noexcept
    {
        const double w0 = 6.283185307179586 * clampFreq (sr, freq);
        const double cs = std::cos (w0), sn = std::sin (w0);
        const double al = sn / (2.0 * q);
        setCoefficients ((1.0 + cs) * 0.5, -(1.0 + cs), (1.0 + cs) * 0.5,
                         1.0 + al, -2.0 * cs, 1.0 - al);
    }

    void setPeak (double sr, double freq, double q, double gainDb) noexcept
    {
        const double A  = std::pow (10.0, gainDb / 40.0);
        const double w0 = 6.283185307179586 * clampFreq (sr, freq);
        const double cs = std::cos (w0), sn = std::sin (w0);
        const double al = sn / (2.0 * q);
        setCoefficients (1.0 + al * A, -2.0 * cs, 1.0 - al * A,
                         1.0 + al / A, -2.0 * cs, 1.0 - al / A);
    }

    void setLowShelf (double sr, double freq, double gainDb, double slope = 0.9) noexcept
    {
        const double A  = std::pow (10.0, gainDb / 40.0);
        const double w0 = 6.283185307179586 * clampFreq (sr, freq);
        const double cs = std::cos (w0), sn = std::sin (w0);
        const double al = sn * 0.5 * std::sqrt ((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);
        const double ts = 2.0 * std::sqrt (A) * al;
        setCoefficients (A * ((A + 1.0) - (A - 1.0) * cs + ts),
                         2.0 * A * ((A - 1.0) - (A + 1.0) * cs),
                         A * ((A + 1.0) - (A - 1.0) * cs - ts),
                         (A + 1.0) + (A - 1.0) * cs + ts,
                         -2.0 * ((A - 1.0) + (A + 1.0) * cs),
                         (A + 1.0) + (A - 1.0) * cs - ts);
    }

    // Shelves parameterised by Q rather than slope — the form the parametric
    // EQ exposes. 0.71 is the flattest knee; pushed past 1 the corner
    // overshoots the way a resonant analogue shelf does.
    void setLowShelfQ (double sr, double freq, double q, double gainDb) noexcept
    {
        const double A  = std::pow (10.0, gainDb / 40.0);
        const double w0 = 6.283185307179586 * clampFreq (sr, freq);
        const double cs = std::cos (w0), sn = std::sin (w0);
        const double al = sn / (2.0 * (q < 0.05 ? 0.05 : q));
        const double ts = 2.0 * std::sqrt (A) * al;
        setCoefficients (A * ((A + 1.0) - (A - 1.0) * cs + ts),
                         2.0 * A * ((A - 1.0) - (A + 1.0) * cs),
                         A * ((A + 1.0) - (A - 1.0) * cs - ts),
                         (A + 1.0) + (A - 1.0) * cs + ts,
                         -2.0 * ((A - 1.0) + (A + 1.0) * cs),
                         (A + 1.0) + (A - 1.0) * cs - ts);
    }

    void setHighShelfQ (double sr, double freq, double q, double gainDb) noexcept
    {
        const double A  = std::pow (10.0, gainDb / 40.0);
        const double w0 = 6.283185307179586 * clampFreq (sr, freq);
        const double cs = std::cos (w0), sn = std::sin (w0);
        const double al = sn / (2.0 * (q < 0.05 ? 0.05 : q));
        const double ts = 2.0 * std::sqrt (A) * al;
        setCoefficients (A * ((A + 1.0) + (A - 1.0) * cs + ts),
                         -2.0 * A * ((A - 1.0) + (A + 1.0) * cs),
                         A * ((A + 1.0) + (A - 1.0) * cs - ts),
                         (A + 1.0) - (A - 1.0) * cs + ts,
                         2.0 * ((A - 1.0) - (A + 1.0) * cs),
                         (A + 1.0) - (A - 1.0) * cs - ts);
    }

    // |H| at hz for the current coefficients — response displays, not audio.
    float magnitudeDb (double sr, double hz) const noexcept
    {
        const double w = 6.283185307179586 * hz / sr;
        const double c1 = std::cos (w),       s1 = std::sin (w);
        const double c2 = std::cos (2.0 * w), s2 = std::sin (2.0 * w);

        const double nr = (double) b0 + (double) b1 * c1 + (double) b2 * c2;
        const double ni = (double) b1 * s1 + (double) b2 * s2;
        const double dr = 1.0 + (double) a1 * c1 + (double) a2 * c2;
        const double di = (double) a1 * s1 + (double) a2 * s2;

        const double den = std::max (1.0e-24, dr * dr + di * di);
        const double m2 = std::max (1.0e-12, (nr * nr + ni * ni) / den);
        return (float) (10.0 * std::log10 (m2));
    }

    void setHighShelf (double sr, double freq, double gainDb, double slope = 0.9) noexcept
    {
        const double A  = std::pow (10.0, gainDb / 40.0);
        const double w0 = 6.283185307179586 * clampFreq (sr, freq);
        const double cs = std::cos (w0), sn = std::sin (w0);
        const double al = sn * 0.5 * std::sqrt ((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);
        const double ts = 2.0 * std::sqrt (A) * al;
        setCoefficients (A * ((A + 1.0) + (A - 1.0) * cs + ts),
                         -2.0 * A * ((A - 1.0) + (A + 1.0) * cs),
                         A * ((A + 1.0) + (A - 1.0) * cs - ts),
                         (A + 1.0) - (A - 1.0) * cs + ts,
                         2.0 * ((A - 1.0) - (A + 1.0) * cs),
                         (A + 1.0) - (A - 1.0) * cs - ts);
    }

private:
    static double clampFreq (double sr, double f) noexcept
    {
        const double norm = f / sr;
        return norm < 0.0001 ? 0.0001 : (norm > 0.49 ? 0.49 : norm);
    }
};

// One-pole lowpass — tone controls, feedback damping, meter ballistics.
struct OnePoleLP
{
    float a = 1.0f, z = 0.0f;

    void setCutoff (double sr, double freq) noexcept
    {
        const double x = std::exp (-6.283185307179586 * freq / sr);
        a = (float) (1.0 - x);
    }

    inline float process (float x) noexcept { z += a * (x - z); return z; }
    void reset() noexcept { z = 0.0f; }
};

// First-order allpass, break (90 degree) frequency at f. Phaser building block.
struct AllpassOne
{
    float c = 0.0f, z = 0.0f;

    void setBreakFreq (double sr, double freq) noexcept
    {
        const double norm = freq / sr;
        const double t = std::tan (3.141592653589793 * (norm < 0.0002 ? 0.0002 : (norm > 0.49 ? 0.49 : norm)));
        c = (float) ((1.0 - t) / (1.0 + t));
    }

    inline float process (float x) noexcept
    {
        const float y = c * x + z;
        z = x - c * y;
        return y;
    }

    void reset() noexcept { z = 0.0f; }
};

// Zavalishin TPT state-variable filter. Unconditionally stable, including
// under the fast per-sample cutoff sweeps an envelope filter or a synth
// filter-env throws at it — the reason it is used instead of a biquad there.
// g = tan (pi * fc / sr), k = 1 / Q; both may change every sample.
struct TptSvf
{
    float ic1 = 0.0f, ic2 = 0.0f;

    inline float processLowpass (float x, float g, float k) noexcept
    {
        const float v1 = (ic1 + g * (x - ic2)) / (1.0f + g * (g + k));
        const float v2 = ic2 + g * v1;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v2;
    }

    void reset() noexcept { ic1 = ic2 = 0.0f; }

    static inline float gForFreq (double sr, float freq) noexcept
    {
        const float norm = (float) (freq / sr);
        const float clamped = norm < 0.0004f ? 0.0004f : (norm > 0.45f ? 0.45f : norm);
        return std::tan (3.14159265f * clamped);
    }
};

// Peak follower with separate attack and release — envelope filter sweeps and
// the synth voice's keyed amplitude.
struct EnvFollower
{
    float env = 0.0f, aAtk = 1.0f, aRel = 1.0f;

    void setTimes (double sr, double attackMs, double releaseMs) noexcept
    {
        aAtk = (float) (1.0 - std::exp (-1000.0 / (sr * (attackMs < 0.05 ? 0.05 : attackMs))));
        aRel = (float) (1.0 - std::exp (-1000.0 / (sr * (releaseMs < 1.0 ? 1.0 : releaseMs))));
    }

    inline float process (float x) noexcept
    {
        const float a = std::abs (x);
        env += (a > env ? aAtk : aRel) * (a - env);
        return env;
    }

    void reset() noexcept { env = 0.0f; }
};

// Schroeder allpass on its own masked buffer — diffusion inside a feedback
// loop (the delay's Smear). Unity magnitude, so it thickens without changing
// the loop's stability margin.
struct BufferedAllpass
{
    std::vector<float> buf;
    int mask = 0, write = 0;
    float g = 0.5f;

    void prepare (int capacity)
    {
        int p = 1;
        while (p < capacity + 4) p <<= 1;
        buf.assign ((size_t) p, 0.0f);
        mask = p - 1;
        write = p;
    }

    void reset() noexcept { std::fill (buf.begin(), buf.end(), 0.0f); }

    inline float process (float x, int delay) noexcept
    {
        const float r = buf[(size_t) ((write - delay) & mask)];
        const float w = x + g * r;
        buf[(size_t) (write & mask)] = w;

        if (++write >= (int) buf.size() * 2)
            write -= (int) buf.size();

        return r - g * w;
    }
};

// Classic DC blocker: y[n] = x[n] - x[n-1] + R y[n-1].
struct DCBlocker
{
    float r = 0.9993f, x1 = 0.0f, y1 = 0.0f;

    void prepare (double sr) noexcept { r = (float) (1.0 - 6.283185307179586 * 5.0 / sr); }

    inline float process (float x) noexcept
    {
        const float y = x - x1 + r * y1;
        x1 = x;
        y1 = y;
        return y;
    }

    void reset() noexcept { x1 = y1 = 0.0f; }
};

} // namespace rig
