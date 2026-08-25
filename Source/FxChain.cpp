#include "FxChain.h"

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

void FxChain::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    maxBlock = std::max (16, maxBlockSize);

    const juce::dsp::ProcessSpec monoSpec { sr, (juce::uint32) maxBlock, 1 };
    const juce::dsp::ProcessSpec stereoSpec { sr, (juce::uint32) maxBlock, 2 };

    comp.prepare (monoSpec);

    os = std::make_unique<juce::dsp::Oversampling<float>> (
             1, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    os->initProcessing ((size_t) maxBlock);

    tracker.prepare (sr);
    shifter.prepare (sr);
    dcBlock.prepare (sr);

    const int delaySize = nextPow2 ((int) (sr * 2.2));
    delayBuf.assign ((size_t) delaySize, 0.0f);
    delayMask = delaySize - 1;

    reverb.prepare (stereoSpec);

    dry.assign ((size_t) maxBlock, 0.0f);
    dryL.assign ((size_t) maxBlock, 0.0f);
    dryR.assign ((size_t) maxBlock, 0.0f);

    juce::SmoothedValue<float>* gains[] { &inGain, &makeup, &driveAmt, &driveLevel, &liveOut,
                                          &octMix, &delayFb, &delayMix, &reverbMix };
    for (auto* g : gains)
        g->reset (sr, 0.02);

    delayTime.reset (sr, 0.15);

    juce::SmoothedValue<float>* bypasses[] { &bypComp, &bypEq, &bypDrive, &bypOct,
                                             &bypPhaser, &bypDelay, &bypReverb };
    for (auto* b : bypasses)
        b->reset (sr, 0.015);

    primed = false;
    reset();
    updateFromParams();

    // Start settled rather than ramping in from zero on device start.
    for (auto* g : gains)      g->setCurrentAndTargetValue (g->getTargetValue());
    for (auto* b : bypasses)   b->setCurrentAndTargetValue (b->getTargetValue());
    delayTime.setCurrentAndTargetValue (delayTime.getTargetValue());
}

void FxChain::reset() noexcept
{
    dcBlock.reset();
    comp.reset();

    eqLo.reset();
    eqMid.reset();
    eqHi.reset();
    tone.reset();

    if (os != nullptr)
        os->reset();

    tracker.reset();
    shifter.reset();

    for (auto& a : ap)
        a.reset();

    lfoPhase = 0.0;
    phaserFbState = 0.0f;

    std::fill (delayBuf.begin(), delayBuf.end(), 0.0f);
    delayWrite = (int) delayBuf.size();
    delayDamp.reset();

    reverb.reset();
}

bool FxChain::changed (int paramId) noexcept
{
    const float v = params.get (paramId);

    if (! primed || ! juce::exactlyEqual (lastSeen[(size_t) paramId], v))
    {
        lastSeen[(size_t) paramId] = v;
        return true;
    }

    return false;
}

void FxChain::updateFromParams() noexcept
{
    inGain.setTargetValue (juce::Decibels::decibelsToGain (params.get (pInGain), -60.0f));
    liveOut.setTargetValue (juce::Decibels::decibelsToGain (params.get (pLiveOut), -60.0f));

    if (changed (pCompThresh))  comp.setThreshold (params.get (pCompThresh));
    if (changed (pCompRatio))   comp.setRatio (params.get (pCompRatio));
    if (changed (pCompAttack))  comp.setAttack (params.get (pCompAttack));
    if (changed (pCompRelease)) comp.setRelease (params.get (pCompRelease));
    makeup.setTargetValue (juce::Decibels::decibelsToGain (params.get (pCompMakeup), -60.0f));

    bool eqDirty = false;
    eqDirty |= changed (pEqLoFreq);
    eqDirty |= changed (pEqLoGain);
    eqDirty |= changed (pEqMidFreq);
    eqDirty |= changed (pEqMidGain);
    eqDirty |= changed (pEqMidQ);
    eqDirty |= changed (pEqHiFreq);
    eqDirty |= changed (pEqHiGain);

    if (eqDirty)
    {
        eqLo.setLowShelf  (sr, params.get (pEqLoFreq), params.get (pEqLoGain));
        eqMid.setPeak     (sr, params.get (pEqMidFreq), params.get (pEqMidQ), params.get (pEqMidGain));
        eqHi.setHighShelf (sr, params.get (pEqHiFreq), params.get (pEqHiGain));
    }

    const float driveValue = std::max (1.0f, params.get (pDriveAmount));
    driveAmt.setTargetValue (driveValue);
    driveLevel.setTargetValue (juce::Decibels::decibelsToGain (params.get (pDriveLevel), -60.0f)
                                   / std::tanh (driveValue));

    if (changed (pDriveTone))
        tone.setLowpass (sr, params.get (pDriveTone), 0.7071);

    shifter.setSemitones (params.get (pOctInterval));
    octMix.setTargetValue (params.get (pOctMix));

    phaserRate  = params.get (pPhaserRate);
    phaserDepth = params.get (pPhaserDepth);
    phaserFb    = std::clamp (params.get (pPhaserFeedback), 0.0f, 0.95f);

    delayTime.setTargetValue (std::clamp (params.get (pDelayTime) * 0.001f * (float) sr,
                                          4.0f, (float) (delayBuf.size() - 4)));
    delayFb.setTargetValue (std::clamp (params.get (pDelayFeedback), 0.0f, 0.95f));
    delayMix.setTargetValue (params.get (pDelayMix));

    if (changed (pDelayHighcut))
        delayDamp.setCutoff (sr, params.get (pDelayHighcut));

    bool revDirty = false;
    revDirty |= changed (pRevSize);
    revDirty |= changed (pRevDamp);

    if (revDirty)
    {
        juce::dsp::Reverb::Parameters rp;
        rp.roomSize   = std::clamp (params.get (pRevSize), 0.0f, 1.0f);
        rp.damping    = std::clamp (params.get (pRevDamp), 0.0f, 1.0f);
        rp.wetLevel   = 1.0f;   // the chain does its own dry/wet
        rp.dryLevel   = 0.0f;
        rp.width      = 1.0f;
        rp.freezeMode = 0.0f;
        reverb.setParameters (rp);
    }

    reverbMix.setTargetValue (params.get (pRevMix));

    bypComp.setTargetValue   (params.getBool (pCompOn)   ? 1.0f : 0.0f);
    bypEq.setTargetValue     (params.getBool (pEqOn)     ? 1.0f : 0.0f);
    bypDrive.setTargetValue  (params.getBool (pDriveOn)  ? 1.0f : 0.0f);
    bypOct.setTargetValue    (params.getBool (pOctOn)    ? 1.0f : 0.0f);
    bypPhaser.setTargetValue (params.getBool (pPhaserOn) ? 1.0f : 0.0f);
    bypDelay.setTargetValue  (params.getBool (pDelayOn)  ? 1.0f : 0.0f);
    bypReverb.setTargetValue (params.getBool (pRevOn)    ? 1.0f : 0.0f);

    float latency = 0.0f;

    if (params.getBool (pDriveOn) && os != nullptr)
        latency += (float) os->getLatencyInSamples();

    if (params.getBool (pOctOn))
        latency += shifter.getLagSamples();

    reportedLatency.store (latency, std::memory_order_relaxed);

    primed = true;
}

template <typename Fn>
void FxChain::runStage (float* data, int numSamples,
                        juce::SmoothedValue<float>& bypassRamp,
                        juce::SmoothedValue<float>* mixRamp,
                        Fn&& fn) noexcept
{
    juce::FloatVectorOperations::copy (dry.data(), data, numSamples);

    fn (data, numSamples);

    if (mixRamp != nullptr)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = bypassRamp.getNextValue() * mixRamp->getNextValue();
            data[i] = dry[(size_t) i] + (data[i] - dry[(size_t) i]) * g;
        }
    }
    else
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = bypassRamp.getNextValue();
            data[i] = dry[(size_t) i] + (data[i] - dry[(size_t) i]) * g;
        }
    }
}

void FxChain::processDrive (float* data, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
        data[i] *= driveAmt.getNextValue();

    float* chans[1] { data };
    juce::dsp::AudioBlock<float> block (chans, 1, (size_t) numSamples);

    auto up = os->processSamplesUp (block);
    auto* u = up.getChannelPointer (0);
    const int upN = (int) up.getNumSamples();

    for (int i = 0; i < upN; ++i)
        u[i] = std::tanh (u[i]);

    os->processSamplesDown (block);

    for (int i = 0; i < numSamples; ++i)
        data[i] = tone.process (data[i]) * driveLevel.getNextValue();
}

void FxChain::processPhaser (float* data, int numSamples) noexcept
{
    int i = 0;

    while (i < numSamples)
    {
        const int chunk = std::min (32, numSamples - i);

        lfoPhase += (double) phaserRate * (double) chunk / sr;
        lfoPhase -= std::floor (lfoPhase);

        const float mod = 0.5f * (1.0f + std::sin ((float) (6.283185307179586 * lfoPhase)));
        const float f = 200.0f * std::pow (9.0f, mod * phaserDepth);   // 200 Hz .. 1.8 kHz

        for (auto& a : ap)
            a.setBreakFreq (sr, f);

        for (int k = 0; k < chunk; ++k)
        {
            const float x = data[i + k];
            float y = x + phaserFb * phaserFbState;

            for (auto& a : ap)
                y = a.process (y);

            phaserFbState = y;
            data[i + k] = 0.5f * (x + y);
        }

        i += chunk;
    }

    if (! std::isfinite (phaserFbState))
    {
        phaserFbState = 0.0f;

        for (auto& a : ap)
            a.reset();
    }

    phaserFbState = std::clamp (phaserFbState, -8.0f, 8.0f);
}

void FxChain::processDelay (float* data, int numSamples) noexcept
{
    const int mask = delayMask;

    for (int i = 0; i < numSamples; ++i)
    {
        const float x = data[i];
        const float t = delayTime.getNextValue();

        const double rp = (double) delayWrite - (double) t;
        const auto i0 = (long long) std::floor (rp);
        const float fr = (float) (rp - (double) i0);

        const float a = delayBuf[(size_t) (i0 & mask)];
        const float b = delayBuf[(size_t) ((i0 + 1) & mask)];
        const float wet = a + (b - a) * fr;

        delayBuf[(size_t) (delayWrite & mask)] = x + delayDamp.process (wet) * delayFb.getNextValue();

        if (++delayWrite >= (int) delayBuf.size() * 2)
            delayWrite -= (int) delayBuf.size();

        data[i] = wet;
    }
}

void FxChain::process (float* mono, float* left, float* right, int numSamples) noexcept
{
    updateFromParams();

    for (int i = 0; i < numSamples; ++i)
        mono[i] = dcBlock.process (mono[i]) * inGain.getNextValue();

    runStage (mono, numSamples, bypComp, nullptr, [this] (float* d, int n)
    {
        for (int i = 0; i < n; ++i)
            d[i] = comp.processSample (0, d[i]) * makeup.getNextValue();
    });

    // Side-chain: track pitch on the clean, level-controlled signal, before the
    // overdrive adds harmonics that can fool the detector.
    tracker.process (mono, numSamples);
    shifter.setPeriod (tracker.getPeriodSamples());

    runStage (mono, numSamples, bypEq, nullptr, [this] (float* d, int n)
    {
        for (int i = 0; i < n; ++i)
            d[i] = eqHi.process (eqMid.process (eqLo.process (d[i])));
    });

    runStage (mono, numSamples, bypDrive, nullptr, [this] (float* d, int n) { processDrive (d, n); });

    runStage (mono, numSamples, bypOct, &octMix, [this] (float* d, int n) { shifter.process (d, n); });

    runStage (mono, numSamples, bypPhaser, nullptr, [this] (float* d, int n) { processPhaser (d, n); });

    runStage (mono, numSamples, bypDelay, &delayMix, [this] (float* d, int n) { processDelay (d, n); });

    juce::FloatVectorOperations::copy (left, mono, numSamples);
    juce::FloatVectorOperations::copy (right, mono, numSamples);
    juce::FloatVectorOperations::copy (dryL.data(), mono, numSamples);
    juce::FloatVectorOperations::copy (dryR.data(), mono, numSamples);

    float* chans[2] { left, right };
    juce::dsp::AudioBlock<float> block (chans, 2, (size_t) numSamples);
    juce::dsp::ProcessContextReplacing<float> ctx (block);
    reverb.process (ctx);

    for (int i = 0; i < numSamples; ++i)
    {
        const float g = bypReverb.getNextValue() * reverbMix.getNextValue();
        const float out = liveOut.getNextValue();

        left[i]  = (dryL[(size_t) i] + (left[i]  - dryL[(size_t) i]) * g) * out;
        right[i] = (dryR[(size_t) i] + (right[i] - dryR[(size_t) i]) * g) * out;
    }
}

} // namespace rig
