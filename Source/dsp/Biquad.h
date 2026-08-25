#pragma once

#include <cmath>

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
