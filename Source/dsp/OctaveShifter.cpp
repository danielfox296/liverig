#include "OctaveShifter.h"

#include <algorithm>
#include <cmath>

namespace rig
{

void OctaveShifter::prepare (double sampleRate)
{
    sr = sampleRate;

    int size = 1;
    while (size < (int) (sampleRate * 0.35))   // >= 350 ms of history
        size <<= 1;

    buf.assign ((size_t) size, 0.0f);
    mask = (long long) size - 1;

    period = (float) (sampleRate / 80.0);
    jumpPeriod = period;

    reset();
}

void OctaveShifter::reset() noexcept
{
    std::fill (buf.begin(), buf.end(), 0.0f);
    writePos = (long long) buf.size();       // start clear of zero so positions stay positive
    pos[0] = (double) writePos - (minLag + 0.75 * (double) period);
    pos[1] = pos[0];
    gain = gainTarget = 0.0f;
    gainStep = 0.0f;
    fading = false;
    lagReport = (float) (minLag + 0.75 * period);
}

void OctaveShifter::setSemitones (float semitones) noexcept
{
    alpha = std::pow (2.0, (double) std::clamp (semitones, -24.0f, 24.0f) / 12.0);
}

void OctaveShifter::setPeriod (float p) noexcept
{
    period = std::clamp (p, 24.0f, (float) (buf.size() / 4));
}

void OctaveShifter::startJump (double delta) noexcept
{
    const int active = gainTarget > 0.5f ? 1 : 0;
    const int other  = 1 - active;

    pos[(size_t) other] = pos[(size_t) active] + delta;
    gainTarget = other == 1 ? 1.0f : 0.0f;
    gainStep = (gainTarget - gain) / (float) xfadeLen;
    fading = true;
}

void OctaveShifter::process (float* data, int numSamples) noexcept
{
    const double a = alpha;

    for (int n = 0; n < numSamples; ++n)
    {
        buf[(size_t) (writePos & mask)] = data[n];

        const float s0 = readInterp (pos[0]);
        const float s1 = readInterp (pos[1]);

        data[n] = s0 + (s1 - s0) * gain;

        pos[0] += a;
        pos[1] += a;
        ++writePos;

        if (fading)
        {
            gain += gainStep;

            if ((gainStep > 0.0f && gain >= gainTarget) || (gainStep <= 0.0f && gain <= gainTarget))
            {
                gain = gainTarget;
                fading = false;
            }
        }
        else
        {
            // Settled: decide whether the active tap needs relocating.
            const int active = gainTarget > 0.5f ? 1 : 0;
            const double lag = (double) writePos - pos[(size_t) active];

            // Smoothed so the latency readout is the figure you actually feel,
            // not whatever point of the grain cycle the UI happened to sample.
            lagReport += 0.0002f * ((float) lag - lagReport);

            if (std::abs (a - 1.0) > 1.0e-9)
            {
                jumpPeriod = period;
                xfadeLen = std::clamp ((int) (0.25 * jumpPeriod), 32, 512);

                const double fade = (double) xfadeLen;
                const double lagHi = minLag + (double) jumpPeriod + fade;

                if (a > 1.0)
                {
                    // Read pointer is catching up with the write pointer.
                    if (lag < minLag + fade * (a - 1.0) + 1.0)
                        startJump (-(double) jumpPeriod);
                }
                else
                {
                    // Read pointer is falling behind.
                    if (lag > lagHi)
                        startJump ((double) jumpPeriod);
                }
            }
        }
    }

    // Keep the running positions bounded so double precision stays exact.
    constexpr long long wrapAt = 1LL << 40;

    if (writePos > wrapAt)
    {
        const long long shift = (wrapAt / (mask + 1)) * (mask + 1);
        writePos -= shift;
        pos[0] -= (double) shift;
        pos[1] -= (double) shift;
    }
}

} // namespace rig
