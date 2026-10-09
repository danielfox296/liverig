#include "Looper.h"

#include <cmath>

namespace rig
{

Looper::Looper() = default;

int Looper::maxBarsAt (float beatsPerMinute) noexcept
{
    const double barSeconds = beatsPerBar * 60.0 / juce::jmax (1.0, (double) beatsPerMinute);
    return juce::jlimit (1, maxBars, (int) std::floor (maxLoopSeconds / barSeconds + 1.0e-6));
}

// Called with the audio callback stopped. A loop does not survive the
// device restarting under it: the rate it was recorded at may be gone.
void Looper::prepare (double sampleRate, int)
{
    const juce::ScopedLock sl (allocLock);

    const bool rateChanged = ! juce::exactlyEqual (rate.load(), sampleRate);
    rate.store (sampleRate);

    if (allocated.load() && rateChanged)
    {
        allocated.store (false);
        base.setSize (0, 0);

        for (auto& layer : layers)
            layer.setSize (0, 0);
    }
    else if (allocated.load())
    {
        base.clear();

        for (auto& layer : layers)
            layer.clear();
    }

    state = State::empty;
    loopLen = countLen = totalBeats = 0;
    beatLen = 0.0;
    clock = 0;
    nextBeat = 0;
    capturing = latchLatency = false;
    latency = 0;
    source = -1;
    playPos = 0;
    newest = -1;
    undone = dubbing = wantLayer = false;
    dubTail = 0;
    clickAge = -1;

    for (int b = 0; b < 2; ++b)
    {
        job[b] = Job::none;
        jobPos[b] = jobLen[b] = 0;
        layerGain[b].reset (sampleRate, 0.010);
        layerGain[b].setCurrentAndTargetValue (1.0f);
    }

    playGain.reset (sampleRate, 0.010);
    playGain.setCurrentAndTargetValue (0.0f);
    levelGain.reset (sampleRate, 0.020);
    levelGain.setCurrentAndTargetValue (1.0f);
    dubGain.reset (sampleRate, 0.005);
    dubGain.setCurrentAndTargetValue (0.0f);

    clickLen = (int) (0.060 * sampleRate);
    clickDecay = sampleRate > 0.0 ? (float) std::exp (-1.0 / (0.012 * sampleRate)) : 0.0f;

    publish (0.0);
}

// Three stereo buffers of the longest loop is tens of megabytes, so a rig
// that never loops never pays for them. The audio thread touches nothing
// here until the flag goes up.
bool Looper::ensureBuffers()
{
    if (allocated.load (std::memory_order_acquire))
        return true;

    const juce::ScopedLock sl (allocLock);

    if (allocated.load())
        return true;

    const double r = rate.load();

    if (r <= 0.0)
        return false;   // no device running; nothing could be recorded anyway

    capacity = (int) std::ceil (maxLoopSeconds * r) + 1;
    base.setSize (2, capacity, false, true, false);

    for (auto& layer : layers)
        layer.setSize (2, capacity, false, true, false);

    fadeLen = juce::jmax (8, (int) std::lround (0.008 * r));

    for (auto& channel : preRoll)
        channel.assign ((size_t) fadeLen, 0.0f);

    allocated.store (true, std::memory_order_release);
    return true;
}

//==============================================================================
void Looper::tapTempo (juce::uint32 nowMs)
{
    if (getState() != State::empty)
        return;

    ensureBuffers();

    if (numTempoTaps > 0 && nowMs - tempoTaps[numTempoTaps - 1] > 2500)
        numTempoTaps = 0;

    if (numTempoTaps == (int) std::size (tempoTaps))
    {
        for (int i = 1; i < numTempoTaps; ++i)
            tempoTaps[i - 1] = tempoTaps[i];

        --numTempoTaps;
    }

    tempoTaps[numTempoTaps++] = nowMs;

    if (numTempoTaps < 2)
        return;

    const auto span = (double) (tempoTaps[numTempoTaps - 1] - tempoTaps[0]);

    if (span <= 0.0)
        return;

    const float tapped = (float) std::round (60000.0 * (numTempoTaps - 1) / span);
    const float tempo = juce::jlimit (minBpm, maxBpm, tapped);

    bpm.store (tempo, std::memory_order_relaxed);
    bars.store (juce::jmin (bars.load (std::memory_order_relaxed), maxBarsAt (tempo)),
                std::memory_order_relaxed);
}

void Looper::tapBars (juce::uint32)
{
    if (getState() != State::empty)
        return;

    ensureBuffers();

    // One more bar a tap; the tap past the longest loop is one bar again.
    const int next = bars.load (std::memory_order_relaxed) + 1;
    bars.store (next > maxBarsAt (bpm.load (std::memory_order_relaxed)) ? 1 : next,
                std::memory_order_relaxed);
}

void Looper::push (Command type, int arg) noexcept
{
    const auto scope = queue.write (1);

    if (scope.blockSize1 > 0)
    {
        queuedType[scope.startIndex1] = (int) type;
        queuedArg[scope.startIndex1] = arg;
    }
}

void Looper::pressRecord (int sourceSlot)
{
    if (ensureBuffers())
        push (cmdRecord, sourceSlot);
}

void Looper::pressPlayPause()   { push (cmdPlayPause, 0); }
void Looper::pressUndo()        { push (cmdUndo, 0); }
void Looper::pressClear()       { push (cmdClear, 0); }

//==============================================================================
int Looper::beginBlock() noexcept
{
    if (! allocated.load (std::memory_order_acquire))
        return -1;

    {
        const auto scope = queue.read (queue.getNumReady());

        scope.forEach ([this] (int index)
        {
            handle ((Command) queuedType[index], queuedArg[index]);
        });
    }

    return state != State::empty ? source : -1;
}

void Looper::handle (Command type, int arg) noexcept
{
    switch (type)
    {
        case cmdRecord:
            switch (state)
            {
                case State::empty:
                {
                    const float tempo = bpm.load (std::memory_order_relaxed);
                    const int length = juce::jmin (bars.load (std::memory_order_relaxed), maxBarsAt (tempo));
                    const double beat = rate.load (std::memory_order_relaxed) * 60.0 / (double) tempo;
                    const auto samples = std::llround (beat * beatsPerBar * length);

                    if (samples > capacity || samples <= 4 * fadeLen)
                        break;

                    beatLen = beat;
                    countLen = (int) std::llround (beat * countInBeats);
                    loopLen = (int) samples;
                    totalBeats = countInBeats + beatsPerBar * length;

                    clock = 0;
                    nextBeat = 0;
                    playPos = 0;
                    capturing = true;
                    latchLatency = true;
                    source = arg;
                    playGain.setCurrentAndTargetValue (0.0f);
                    state = State::countIn;
                    break;
                }

                case State::countIn:
                    toEmpty();   // nothing has been played yet; the take is called off
                    break;

                case State::recording:
                    closeTake();
                    break;

                case State::paused:
                    playPos = pausedAt;
                    playGain.setTargetValue (1.0f);
                    state = State::playing;
                    [[fallthrough]];

                case State::playing:
                    source = arg;
                    wantLayer = true;
                    latchLatency = ! capturing;
                    break;

                case State::overdubbing:
                    endLayer (true);
                    break;
            }
            break;

        case cmdPlayPause:
            switch (state)
            {
                case State::empty:
                    break;

                case State::countIn:
                case State::recording:
                    toEmpty();
                    break;

                case State::playing:
                case State::overdubbing:
                    endLayer (false);
                    wantLayer = false;
                    pausedAt = playPos;   // the loop runs on under its fade; Play picks up from here
                    state = State::paused;
                    playGain.setTargetValue (0.0f);
                    break;

                case State::paused:
                    playPos = pausedAt;
                    playGain.setTargetValue (1.0f);
                    state = State::playing;
                    break;
            }
            break;

        case cmdUndo:
            if (state == State::empty || state == State::countIn || state == State::recording)
                break;

            if (wantLayer)
            {
                wantLayer = false;   // the layer had not begun; nothing to take back
                break;
            }

            if (dubbing)
            {
                endLayer (false);
                undone = true;
            }
            else if (newest >= 0)
            {
                undone = ! undone;
            }

            if (newest >= 0)
                layerGain[newest].setTargetValue (undone ? 0.0f : 1.0f);

            break;

        case cmdClear:
            toEmpty();
            break;
    }
}

void Looper::toEmpty() noexcept
{
    // A take cancelled before it closed has touched no layer; the wipes of
    // an earlier loop, if any are still running, keep their own lengths.
    if (state != State::countIn && state != State::recording)
    {
        for (int b = 0; b < 2; ++b)
        {
            if (b == newest || job[b] != Job::none)
            {
                // A wipe still running from a longer loop keeps its reach.
                jobLen[b] = job[b] != Job::none ? juce::jmax (jobLen[b], loopLen) : loopLen;
                job[b] = Job::wipe;
                jobPos[b] = 0;
            }
        }
    }

    newest = -1;
    undone = dubbing = wantLayer = capturing = false;
    dubTail = 0;
    clickAge = -1;
    source = -1;
    state = State::empty;
    playGain.setTargetValue (0.0f);
}

// Record pressed again during the take: the loop ends on the bar line
// nearest the press. A press just after a bar line closes the loop on that
// line and playback is already that far into it, so the beat never moves;
// a press before one lets the take run on to it. Either way the loop is a
// whole number of bars, and at least one.
void Looper::closeTake() noexcept
{
    const double barLen = beatLen * beatsPerBar;
    const int planned = (totalBeats - countInBeats) / beatsPerBar;
    const juce::int64 pos = clock - countLen;   // how far into the take the press fell
    const int kept = juce::jlimit (1, planned, (int) std::llround ((double) pos / barLen));

    if (kept == planned)
        return;   // the take is about to close itself on that same line

    const int length = (int) std::llround (barLen * kept);

    // The seam's fade belongs at the new end. Whatever of it has already
    // been written is blended here; the rest is blended as it arrives.
    const juce::int64 written = clock - latency - countLen;

    for (int k = 0; k < fadeLen; ++k)
    {
        const int index = length - fadeLen + k;

        if (index >= written)
            break;

        const float w = (float) (k + 1) / (float) (fadeLen + 1);

        for (int ch = 0; ch < 2; ++ch)
        {
            float* data = base.getWritePointer (ch);
            data[index] += w * (preRoll[ch][(size_t) k] - data[index]);
        }
    }

    loopLen = length;
    totalBeats = countInBeats + beatsPerBar * kept;
    bars.store (kept, std::memory_order_relaxed);

    if (written >= length)
        capturing = false;

    if (pos >= length)
    {
        state = State::playing;
        playPos = (int) (pos - length);
        playGain.setCurrentAndTargetValue (0.0f);
        playGain.setTargetValue (1.0f);
    }
}

// A new layer needs a clean buffer. The layer before it is folded into the
// first take — or thrown away, if it had been undone — and the other buffer
// takes the new one. If that buffer is still being cleaned the start waits
// a few blocks rather than record onto leftovers.
void Looper::tryStartLayer() noexcept
{
    int target = -1;

    if (newest >= 0)
    {
        target = 1 - newest;

        if (job[target] != Job::none)
            return;

        job[newest] = undone ? Job::wipe : Job::fold;
        jobPos[newest] = 0;
        jobLen[newest] = loopLen;
    }
    else
    {
        target = job[0] == Job::none ? 0 : (job[1] == Job::none ? 1 : -1);

        if (target < 0)
            return;
    }

    newest = target;
    undone = false;
    layerGain[target].setCurrentAndTargetValue (1.0f);
    dubbing = true;
    dubTail = 0;
    wantLayer = false;
    state = State::overdubbing;
}

// What was played up to the press is still on its way in; with keepTail the
// writing runs on that long before it fades.
void Looper::endLayer (bool keepTail) noexcept
{
    if (dubbing)
        dubTail = keepTail ? latency : 0;

    dubbing = false;

    if (state == State::overdubbing)
        state = State::playing;
}

void Looper::runJobs (int budget) noexcept
{
    for (int b = 0; b < 2 && budget > 0; ++b)
    {
        if (job[b] == Job::none)
            continue;

        // Never pull a buffer out from under a fade that is still sounding.
        if (state == State::empty ? playGain.isSmoothing() : layerGain[b].isSmoothing())
            continue;

        const int count = juce::jmin (budget, jobLen[b] - jobPos[b]);

        for (int ch = 0; ch < 2; ++ch)
        {
            float* layer = layers[b].getWritePointer (ch) + jobPos[b];

            if (job[b] == Job::fold)
                juce::FloatVectorOperations::add (base.getWritePointer (ch) + jobPos[b], layer, count);

            juce::FloatVectorOperations::clear (layer, count);
        }

        jobPos[b] += count;
        budget -= count;

        if (jobPos[b] >= jobLen[b])
            job[b] = Job::none;
    }
}

void Looper::publish (double heardAtMs) noexcept
{
    const bool counting = state == State::countIn || state == State::recording;

    uiState.store ((int) state, std::memory_order_relaxed);
    uiPos.store (counting ? (int) (state == State::countIn ? clock : clock - countLen) : playPos,
                 std::memory_order_relaxed);
    uiHeardAt.store (heardAtMs, std::memory_order_relaxed);
    uiLoopLen.store (loopLen, std::memory_order_relaxed);
    uiBeatLen.store ((float) beatLen, std::memory_order_relaxed);
    uiSource.store (source, std::memory_order_relaxed);
    uiHasLayer.store (newest >= 0, std::memory_order_relaxed);
    uiUndone.store (undone, std::memory_order_relaxed);
}

float Looper::getBeatPosition (double nowMs) const noexcept
{
    const float beat = uiBeatLen.load (std::memory_order_relaxed);

    if (beat <= 0.0f)
        return 0.0f;

    double pos = (double) uiPos.load (std::memory_order_relaxed);
    const double heardAt = uiHeardAt.load (std::memory_order_relaxed);
    const auto shownState = getState();

    if (nowMs > 0.0 && heardAt > 0.0 && shownState != State::empty && shownState != State::paused)
    {
        // The published sample is heard at heardAt; the one heard now is
        // that far before or after it. Should the audio stop answering,
        // the beat goes no further than a tenth of a second past the last
        // block rather than run on alone.
        pos += juce::jmin (100.0, nowMs - heardAt) * 0.001 * rate.load (std::memory_order_relaxed);

        const int length = uiLoopLen.load (std::memory_order_relaxed);

        if ((shownState == State::playing || shownState == State::overdubbing) && length > 0)
        {
            pos = std::fmod (pos, (double) length);

            if (pos < 0.0)
                pos += length;
        }
    }

    return (float) (pos / beat);
}

void Looper::process (const float* srcL, const float* srcR, float* outL, float* outR, int numSamples,
                      int latencySamples, float level, double heardAtMs) noexcept
{
    if (! allocated.load (std::memory_order_acquire))
        return;

    // How late the player arrives is fixed for the length of a take: a
    // board whose delay moved mid-pass must not tear the writing.
    if (latchLatency)
    {
        latency = juce::jlimit (0, juce::jmin (countLen, loopLen) / 2, latencySamples);
        latchLatency = false;
    }

    runJobs (numSamples * 64);

    if (wantLayer && state == State::playing)
        tryStartLayer();

    levelGain.setTargetValue (level);

    float* const baseL = base.getWritePointer (0);
    float* const baseR = base.getWritePointer (1);
    float* const layerL[2] = { layers[0].getWritePointer (0), layers[1].getWritePointer (0) };
    float* const layerR[2] = { layers[0].getWritePointer (1), layers[1].getWritePointer (1) };

    for (int i = 0; i < numSamples; ++i)
    {
        const float inL = srcL[i], inR = srcR[i];
        float addL = 0.0f, addR = 0.0f;

        if (state == State::countIn && clock >= countLen)
            state = State::recording;

        if (state == State::recording && clock >= (juce::int64) countLen + loopLen)
        {
            state = State::playing;
            playPos = 0;
            playGain.setCurrentAndTargetValue (1.0f);
        }

        const bool counting = state == State::countIn || state == State::recording;

        // The click: four to count in, then every beat of the take, the
        // first of each bar a little higher.
        if (counting && nextBeat < totalBeats && clock >= std::llround ((double) nextBeat * beatLen))
        {
            const bool accent = nextBeat % beatsPerBar == 0;
            clickAge = 0;
            clickPhase = 0.0;
            clickStep = juce::MathConstants<double>::twoPi * (accent ? 1500.0 : 1000.0)
                          / rate.load (std::memory_order_relaxed);
            clickEnv = accent ? 0.40f : 0.28f;
            ++nextBeat;
        }

        // The first take. What arrives now was played `latency` ago, so the
        // take's first sample is the one that arrives that long after the
        // fourth click's bar ends. The few milliseconds before it are kept
        // and faded in under the take's last few, so the loop's end runs
        // into its beginning the way the playing ran into the downbeat.
        if (capturing)
        {
            const juce::int64 g = clock - latency - countLen;

            if (g >= -fadeLen && g < 0)
            {
                preRoll[0][(size_t) (g + fadeLen)] = inL;
                preRoll[1][(size_t) (g + fadeLen)] = inR;
            }
            else if (g >= 0 && g < loopLen)
            {
                float vL = inL, vR = inR;
                const int tail = (int) g - (loopLen - fadeLen);

                if (tail >= 0)
                {
                    const float w = (float) (tail + 1) / (float) (fadeLen + 1);
                    vL += w * (preRoll[0][(size_t) tail] - vL);
                    vR += w * (preRoll[1][(size_t) tail] - vR);
                }

                baseL[g] = vL;
                baseR[g] = vR;

                if (g == loopLen - 1)
                    capturing = false;
            }
        }

        if (counting || capturing)
            ++clock;

        const float lg = levelGain.getNextValue();

        if (state == State::playing || state == State::overdubbing || playGain.isSmoothing())
        {
            const float pg = playGain.getNextValue() * lg;
            const float g0 = layerGain[0].getNextValue();
            const float g1 = layerGain[1].getNextValue();
            const int p = playPos;

            addL = (baseL[p] + layerL[0][p] * g0 + layerL[1][p] * g1) * pg;
            addR = (baseR[p] + layerR[0][p] * g0 + layerR[1][p] * g1) * pg;

            const bool feeding = dubbing || dubTail > 0;

            if (newest >= 0 && (feeding || dubGain.isSmoothing() || dubGain.getCurrentValue() > 0.0f))
            {
                if (! dubbing && dubTail > 0)
                    --dubTail;

                dubGain.setTargetValue (feeding ? 1.0f : 0.0f);
                const float dg = dubGain.getNextValue();

                int w = p - latency;

                if (w < 0)
                    w += loopLen;

                layerL[newest][w] += inL * dg;
                layerR[newest][w] += inR * dg;
            }

            if (++playPos >= loopLen)
                playPos = 0;
        }

        if (clickAge >= 0)
        {
            const float c = (float) std::sin (clickPhase) * clickEnv;
            clickPhase += clickStep;
            clickEnv *= clickDecay;
            addL += c;
            addR += c;

            if (++clickAge >= clickLen)
                clickAge = -1;
        }

        outL[i] += addL;
        outR[i] += addR;
    }

    // What is published is where the next block starts, so its time is the
    // end of this one.
    const double r = rate.load (std::memory_order_relaxed);
    publish (heardAtMs > 0.0 && r > 0.0 ? heardAtMs + numSamples * 1000.0 / r : 0.0);
}

} // namespace rig
