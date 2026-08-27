#pragma once

#include <cmath>

namespace rig
{

// Safety limiter for the master sum. Zero lookahead — this is a live path —
// so the envelope attacks instantly and releases over ~120 ms. Instant
// attack means the output mathematically never passes the ceiling: gain is
// ceiling/env and env is never below the current peak. On sustained hot
// material the envelope rides the waveform's crest, so the gain sits still
// and the sound stays clean; only transient tips get ducked.
//
// The alternative to this stage is the DAC hard-clipping, which is never
// the better sound.
struct MasterLimiter
{
    float env = 0.0f;
    float rel = 0.9999f;
    float lastGain = 1.0f;

    static constexpr float ceiling = 0.891f;   // -1 dBFS

    void prepare (double sr) noexcept
    {
        rel = (float) std::exp (-1.0 / (0.12 * sr));
        reset();
    }

    void reset() noexcept
    {
        env = 0.0f;
        lastGain = 1.0f;
    }

    inline void process (float& l, float& r) noexcept
    {
        const float al = std::abs (l);
        const float ar = std::abs (r);
        const float peak = al > ar ? al : ar;

        env = peak > env ? peak : peak + (env - peak) * rel;

        const float g = env > ceiling ? ceiling / env : 1.0f;
        lastGain = g;

        l *= g;
        r *= g;
    }

    // How hard it is working right now, as positive dB of reduction.
    float getReductionDb() const noexcept
    {
        return lastGain < 1.0f ? -20.0f * std::log10 (lastGain) : 0.0f;
    }
};

// Playback-lane dynamics: the song is limited into the headroom the
// instrument leaves, never the other way round.
//
// The stems are a mastered song, already leaning on full scale, and the live
// lane runs near the ceiling too, so their sum lives several dB over it.
// Left to the master limiter alone, that overage becomes constant gain
// reduction on the whole bus — and the audible casualty is the instrument,
// pushed 4–6 dB down for as long as a song runs. This stage settles the
// contest before the sum instead: the live lane's envelope claims its level
// first and the stems are held to whatever remains, but never below a bed
// 12 dB under the ceiling, so the song stays present through sustained
// playing. Gain moves on a fader's timescale — milliseconds down, half a
// second back up — so the song breathes in the gaps rather than pumping per
// note. The master limiter stays downstream as the sample-exact guarantee:
// this stage is the mix decision, that one is the law.
struct StemDucker
{
    static constexpr float ceiling  = MasterLimiter::ceiling;
    static constexpr float bedFloor = ceiling * 0.251f;   // -12 dB under it

    void prepare (double sr) noexcept
    {
        liveRel = (float) std::exp (-1.0 / (0.15 * sr));
        stemRel = (float) std::exp (-1.0 / (0.12 * sr));
        duckCoef    = (float) (1.0 - std::exp (-1.0 / (0.003 * sr)));
        recoverCoef = (float) (1.0 - std::exp (-1.0 / (0.4 * sr)));
        reset();
    }

    void reset() noexcept
    {
        liveEnv = 0.0f;
        stemEnv = 0.0f;
        gain = 1.0f;
    }

    inline void process (float& sl, float& sr2, float liveL, float liveR) noexcept
    {
        const float lal = std::abs (liveL);
        const float lar = std::abs (liveR);
        const float lp = lal > lar ? lal : lar;
        liveEnv = lp > liveEnv ? lp : lp + (liveEnv - lp) * liveRel;

        const float sal = std::abs (sl);
        const float sar = std::abs (sr2);
        const float sp = sal > sar ? sal : sar;
        stemEnv = sp > stemEnv ? sp : sp + (stemEnv - sp) * stemRel;

        const float room = ceiling - liveEnv;
        const float allowed = room > bedFloor ? room : bedFloor;
        const float target = stemEnv > allowed ? allowed / stemEnv : 1.0f;

        gain += (target < gain ? duckCoef : recoverCoef) * (target - gain);

        sl  *= gain;
        sr2 *= gain;
    }

    // How far the song is pulled down right now, as positive dB.
    float getReductionDb() const noexcept
    {
        return gain < 1.0f ? -20.0f * std::log10 (gain) : 0.0f;
    }

    float liveEnv = 0.0f, stemEnv = 0.0f, gain = 1.0f;
    float liveRel = 0.9999f, stemRel = 0.9999f;
    float duckCoef = 0.01f, recoverCoef = 0.0001f;
};

} // namespace rig
