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

// 4-point, 3rd-order Hermite read on a masked circular buffer — the same
// interpolator the octave shifter uses. The delay and chorus taps move, and a
// moving tap through linear interpolation dulls and hisses.
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

// Band-limiting residual for a step edge at phase t with per-sample increment
// dt. Keeps the synth's square and saw from spraying aliases across the mix.
static inline float polyBlep (double t, double dt) noexcept
{
    if (t < dt)
    {
        const double x = t / dt;
        return (float) (x + x - x * x - 1.0);
    }

    if (t > 1.0 - dt)
    {
        const double x = (t - 1.0) / dt;
        return (float) (x * x + x + x + 1.0);
    }

    return 0.0f;
}

// The three chorus voices: base tap, LFO rate multiplier, depth multiplier.
// Voice 1 alone is a vibrato-ish single chorus, two is the classic pair, all
// three is the offset stack that makes a tri-chorus sound wide.
static constexpr float choBaseMs[3]    { 8.0f, 13.0f, 19.0f };
static constexpr float choRateMult[3]  { 1.0f, 0.87f, 1.13f };
static constexpr float choDepthMult[3] { 1.0f, 0.80f, 1.25f };

void FxChain::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    maxBlock = std::max (16, maxBlockSize);

    comp.prepare (sr);

    os = std::make_unique<juce::dsp::Oversampling<float>> (
             1, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    os->initProcessing ((size_t) maxBlock);

    // The drive's band splits live at the oversampled rate, where the
    // clippers run.
    const double osRate = sr * (double) os->getOversamplingFactor();
    drvGreenLp.setCutoff (osRate, 720.0);
    drvGoldLp.setCutoff (osRate, 120.0);
    sagEnv.setTimes (sr, 4.0, 160.0);

    amp.prepare (sr, maxBlock);

    tracker.prepare (sr);
    segmenter.prepare (sr);
    shifter.prepare (sr);
    shifter2.prepare (sr);
    octBuf.assign ((size_t) maxBlock, 0.0f);
    dcBlock.prepare (sr);
    driveDC.prepare (sr);
    tapeDC.prepare (sr);

    synEnv.setTimes (sr, 2.5, 90.0);
    fltCutCoef = synCutCoef = (float) (1.0 - std::exp (-1.0 / (0.003 * sr)));

    const int delaySize = nextPow2 ((int) (sr * 2.2));
    delayBuf.assign ((size_t) delaySize, 0.0f);
    delayMask = delaySize - 1;

    dlSmearLen1 = std::max (16, (int) (sr * 0.00647));
    dlSmearLen2 = std::max (8,  (int) (sr * 0.00311));
    dlSmear1.prepare (dlSmearLen1 + 8);
    dlSmear2.prepare (dlSmearLen2 + 8);
    dlSmear1.g = 0.55f;
    dlSmear2.g = 0.45f;
    dlFlutCoef = (float) (1.0 - std::exp (-6.283185307179586 * 6.5 / sr));

    const int choSize = nextPow2 ((int) (sr * 0.05));
    choBuf.assign ((size_t) choSize, 0.0f);
    choMask = choSize - 1;

    reverb.prepare (sr, maxBlock);

    dry.assign ((size_t) maxBlock, 0.0f);

    juce::SmoothedValue<float>* gains[] { &inGain, &makeup, &compBlend, &driveAmt, &driveLevel, &liveOut,
                                          &ampIn, &ampOut, &octMix, &oct2Lvl,
                                          &delayFb, &delayMix, &reverbMix,
                                          &fltMix, &boostGain, &boostHair, &stackAmt,
                                          &synShape, &synSubLvl, &synMix,
                                          &choVoiceGain[0], &choVoiceGain[1], &choVoiceGain[2],
                                          &choMix, &dlSatAmt, &dlSmearAmt, &tapeDrive, &tapeMix,
                                          &subLevel, &driveBlend };
    for (auto* g : gains)
        g->reset (sr, 0.02);

    delayTime.reset (sr, 0.15);
    choDepth.reset (sr, 0.05);
    dlModDepth.reset (sr, 0.05);

    cab.prepare (sr);
    subOctave.prepare (sr);
    subBuf.assign ((size_t) maxBlock, 0.0f);
    driveLow.assign ((size_t) maxBlock, 0.0f);
    driveClean.assign ((size_t) maxBlock, 0.0f);

    juce::SmoothedValue<float>* bypasses[] { &bypComp, &bypEq, &bypFilter, &bypBoost, &bypCab, &bypSub,
                                             &bypDrive, &bypAmp, &bypOct, &bypSynth, &bypPhaser,
                                             &bypChorus, &bypDelay, &bypTape, &bypReverb };
    for (auto* b : bypasses)
        b->reset (sr, 0.015);

    primed = false;
    reset();
    updateFromParams();

    // Start settled rather than ramping in from zero on device start.
    for (auto* g : gains)      g->setCurrentAndTargetValue (g->getTargetValue());
    for (auto* b : bypasses)   b->setCurrentAndTargetValue (b->getTargetValue());
    delayTime.setCurrentAndTargetValue (delayTime.getTargetValue());
    choDepth.setCurrentAndTargetValue (choDepth.getTargetValue());
    dlModDepth.setCurrentAndTargetValue (dlModDepth.getTargetValue());
}

void FxChain::reset() noexcept
{
    cab.reset();
    subOctave.reset();
    driveSplit.reset();
    dcBlock.reset();
    comp.reset();

    for (auto& band : eqBand)
        band.reset();

    tone.reset();

    fltEnv.reset();
    fltSvf.reset();
    fltCutState = 120.0f;

    boostSheen.reset();
    biteTilt.reset();
    drvGreenLp.reset();
    drvGoldLp.reset();
    sagEnv.reset();
    sagState = 0.0f;
    driveDC.reset();

    if (os != nullptr)
        os->reset();

    tracker.reset();
    shifter.reset();
    shifter2.reset();

    synPhase = 0.0;
    synSubPhase = 0.0;
    synPeriod = 600.0f;
    synTargetPeriod = 600.0f;
    synCutState = 350.0f;
    synEnv.reset();
    synSvf.reset();

    for (auto& a : ap)
        a.reset();

    lfoPhase = 0.0;
    phaserFbState = 0.0f;
    phaserLfoState = 0.0f;

    std::fill (choBuf.begin(), choBuf.end(), 0.0f);
    choWrite = (int) choBuf.size();
    choPhase[0] = 0.0;
    choPhase[1] = 1.0 / 3.0;
    choPhase[2] = 2.0 / 3.0;
    choTone.reset();
    choLows.reset();

    std::fill (delayBuf.begin(), delayBuf.end(), 0.0f);
    delayWrite = (int) delayBuf.size();
    delayDamp.reset();
    dlSmear1.reset();
    dlSmear2.reset();
    dlNoise = 2463534242u;   // fixed seed: renders are repeatable
    dlWow = dlFlut = 0.0f;

    tapePre.reset();
    tapePost.reset();
    tapeBump.reset();
    tapeRolloff.reset();
    tapeDC.reset();

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

    // With MIDI on and Thru off, the rig is a controller: the live lane
    // leaves the mix and only the pad speaks. The tracker keeps hearing the
    // input either way — it taps the chain ahead of this gain.
    const bool thruMuted = params.getBool (pMidiOn) && ! params.getBool (pMidiThru);
    liveOut.setTargetValue (thruMuted ? 0.0f
                                      : juce::Decibels::decibelsToGain (params.get (pLiveOut), -60.0f));

    if (changed (pCompThresh))  comp.setThreshold (params.get (pCompThresh));
    if (changed (pCompRatio))   comp.setRatio (params.get (pCompRatio));
    if (changed (pCompAttack))  comp.setAttack (params.get (pCompAttack));
    if (changed (pCompRelease)) comp.setRelease (params.get (pCompRelease));
    makeup.setTargetValue (juce::Decibels::decibelsToGain (params.get (pCompMakeup), -60.0f));
    compBlend.setTargetValue (std::clamp (params.get (pCompBlend), 0.0f, 1.0f));

    for (int band = 0; band < 5; ++band)
    {
        const int base = pEq1Freq + band * 3;

        bool bandDirty = false;
        bandDirty |= changed (base);
        bandDirty |= changed (base + 1);
        bandDirty |= changed (base + 2);

        if (bandDirty)
        {
            const float f = params.get (base);
            const float gainDb = params.get (base + 1);
            const float q = params.get (base + 2);

            if (band == 0)
                eqBand[0].setLowShelfQ (sr, f, q, gainDb);
            else if (band == 4)
                eqBand[4].setHighShelfQ (sr, f, q, gainDb);
            else
                eqBand[band].setPeak (sr, f, q, gainDb);
        }
    }

    if (changed (pFltSens))
        fltSensGain = 6.0f * std::pow (4.0f, (params.get (pFltSens) - 0.5f) * 2.0f);

    bool fltTimesDirty = false;
    fltTimesDirty |= changed (pFltAttack);
    fltTimesDirty |= changed (pFltRelease);

    if (fltTimesDirty)
        fltEnv.setTimes (sr, params.get (pFltAttack), params.get (pFltRelease));

    bool fltRangeDirty = false;
    fltRangeDirty |= changed (pFltFrom);
    fltRangeDirty |= changed (pFltTo);

    if (fltRangeDirty)
    {
        fltFrom = std::max (20.0f, params.get (pFltFrom));
        fltRatio = std::max (20.0f, params.get (pFltTo)) / fltFrom;
    }

    if (changed (pFltRes))
    {
        const float q = std::max (0.5f, params.get (pFltRes));
        fltK = 1.0f / q;
        fltComp = 1.0f / std::sqrt (std::max (1.0f, q * 0.35f));
    }

    fltMix.setTargetValue (params.get (pFltMix));

    boostGain.setTargetValue (juce::Decibels::decibelsToGain (params.get (pBoostGain), -60.0f));
    boostHair.setTargetValue (params.get (pBoostHair));

    if (changed (pBoostSheen))
        boostSheen.setHighShelf (sr, 2800.0, params.get (pBoostSheen));

    if (changed (pCompSidechain))
        comp.setSidechain (params.get (pCompSidechain));

    if (changed (pDriveXover))
        driveSplit.setCutoff (sr, params.get (pDriveXover));

    driveBlend.setTargetValue (params.get (pDriveBlend));

    bool cabDirty = false;
    cabDirty |= changed (pCabBumpFreq);
    cabDirty |= changed (pCabBumpGain);
    cabDirty |= changed (pCabTop);
    cabDirty |= changed (pCabPresence);

    if (cabDirty)
        cab.setParams (params.get (pCabBumpFreq), params.get (pCabBumpGain),
                       params.get (pCabTop), params.get (pCabPresence));

    subLevel.setTargetValue (params.get (pSubLevel));
    subDivisor = params.get (pSubDivide) > 0.5f ? 2 : 1;

    if (changed (pSubTone))
        subOctave.setTone (params.get (pSubTone));

    driveVoice = std::clamp ((int) std::lround (params.get (pDriveVoice)), 1, 3);
    driveAmt.setTargetValue (std::max (1.0f, params.get (pDriveAmount)));
    driveLevel.setTargetValue (juce::Decibels::decibelsToGain (params.get (pDriveLevel), -60.0f));

    if (changed (pDriveTone))
        tone.setLowpass (sr, params.get (pDriveTone), 0.7071);

    stackAmt.setTargetValue (std::clamp (params.get (pDriveStack), 0.0f, 1.0f));

    if (changed (pDriveBite))
    {
        const double osRate = sr * (os != nullptr ? (double) os->getOversamplingFactor() : 4.0);
        biteTilt.setPeak (osRate, 900.0, 0.8, 2.0 + 10.0 * params.get (pDriveBite));
    }

    ampIn.setTargetValue (juce::Decibels::decibelsToGain (params.get (pAmpIn), -60.0f));
    ampOut.setTargetValue (juce::Decibels::decibelsToGain (params.get (pAmpOut), -60.0f));

    shifter.setSemitones (params.get (pOctInterval));
    shifter2.setSemitones (params.get (pOct2Interval));
    octMix.setTargetValue (params.get (pOctMix));
    oct2Lvl.setTargetValue (std::clamp (params.get (pOct2Level), 0.0f, 1.0f));

    synShape.setTargetValue (params.get (pSynShape));
    synSubLvl.setTargetValue (params.get (pSynSub));
    synMix.setTargetValue (params.get (pSynMix));

    if (changed (pSynCutoff))  synCutBase = params.get (pSynCutoff);
    if (changed (pSynEnv))     synEnvAmt = params.get (pSynEnv);

    if (changed (pSynRes))
    {
        const float q = std::max (0.5f, params.get (pSynRes));
        synK = 1.0f / q;
        synComp = 1.0f / std::sqrt (std::max (1.0f, q * 0.35f));
    }

    if (changed (pSynGlide))
    {
        const float ms = params.get (pSynGlide);
        synGlideCoef = ms < 0.5f ? 1.0f
                                 : (float) (1.0 - std::exp (-1000.0 / (sr * ms)));
    }

    phaserRate  = params.get (pPhaserRate);
    phaserDepth = params.get (pPhaserDepth);
    phaserFb    = std::clamp (params.get (pPhaserFeedback), 0.0f, 0.95f);
    phaserStages = std::clamp ((int) std::lround (params.get (pPhaserStages)), 4, 8);

    choRate = params.get (pChoRate);
    choDepth.setTargetValue (params.get (pChoDepth) * 0.0035f * (float) sr);

    if (changed (pChoVoices))
    {
        const int voices = std::clamp ((int) std::lround (params.get (pChoVoices)), 1, 3);
        const float share = 1.0f / (float) voices;

        for (int v = 0; v < 3; ++v)
            choVoiceGain[v].setTargetValue (v < voices ? share : 0.0f);
    }

    if (changed (pChoTone))
        choTone.setCutoff (sr, params.get (pChoTone));

    if (changed (pChoLows))
        choLows.setHighpass (sr, params.get (pChoLows), 0.7071);

    choMix.setTargetValue (params.get (pChoMix));

    delayTime.setTargetValue (std::clamp (params.get (pDelayTime) * 0.001f * (float) sr,
                                          4.0f, (float) (delayBuf.size() - 4)));
    delayFb.setTargetValue (std::clamp (params.get (pDelayFeedback), 0.0f, 0.95f));
    delayMix.setTargetValue (params.get (pDelayMix));

    if (changed (pDelayHighcut))
        delayDamp.setCutoff (sr, params.get (pDelayHighcut));

    dlModRate = params.get (pDelayModRate);

    if (changed (pDelayModRate))
    {
        // Wow bandwidth follows the rate knob; the amplitude compensation
        // keeps the drift's depth constant as its speed changes.
        dlWowCoef = (float) (1.0 - std::exp (-6.283185307179586 * (dlModRate * 0.6) / sr));
        dlWowAmp = 2.2f / std::sqrt (std::max (1.0e-6f, dlWowCoef));
    }

    dlModDepth.setTargetValue (params.get (pDelayModDepth) * 0.0025f * (float) sr);
    dlSatAmt.setTargetValue (std::clamp (params.get (pDelaySat), 0.0f, 1.0f));
    dlSmearAmt.setTargetValue (std::clamp (params.get (pDelaySmear), 0.0f, 1.0f));

    tapeDrive.setTargetValue (juce::Decibels::decibelsToGain (params.get (pTapeDrive), -60.0f));
    tapeMix.setTargetValue (params.get (pTapeMix));

    if (changed (pTapeSoften))
    {
        const float s = std::clamp (params.get (pTapeSoften), 0.0f, 1.0f);
        tapePre.setHighShelf  (sr, 2800.0, 9.0 * s);
        tapePost.setHighShelf (sr, 2800.0, -9.0 * s);
        tapeRolloff.setCutoff (sr, 16000.0 - 9000.0 * s);
    }

    if (changed (pTapeBump))
        tapeBump.setLowShelf (sr, 80.0, 4.5 * params.get (pTapeBump));

    reverb.setSize (params.get (pRevSize));
    reverb.setDamping (params.get (pRevDamp));
    reverb.setPredelayMs (params.get (pRevPre));
    reverb.setModDepth (params.get (pRevMod));

    reverbMix.setTargetValue (params.get (pRevMix));

    bypComp.setTargetValue   (params.getBool (pCompOn)   ? 1.0f : 0.0f);
    bypEq.setTargetValue     (params.getBool (pEqOn)     ? 1.0f : 0.0f);
    bypFilter.setTargetValue (params.getBool (pFltOn)    ? 1.0f : 0.0f);
    bypBoost.setTargetValue  (params.getBool (pBoostOn)  ? 1.0f : 0.0f);
    bypDrive.setTargetValue  (params.getBool (pDriveOn)  ? 1.0f : 0.0f);
    bypAmp.setTargetValue    (params.getBool (pAmpOn)    ? 1.0f : 0.0f);
    bypOct.setTargetValue    (params.getBool (pOctOn)    ? 1.0f : 0.0f);
    bypSynth.setTargetValue  (params.getBool (pSynOn)    ? 1.0f : 0.0f);
    bypPhaser.setTargetValue (params.getBool (pPhaserOn) ? 1.0f : 0.0f);
    bypChorus.setTargetValue (params.getBool (pChoOn)    ? 1.0f : 0.0f);
    bypDelay.setTargetValue  (params.getBool (pDelayOn)  ? 1.0f : 0.0f);
    bypTape.setTargetValue   (params.getBool (pTapeOn)   ? 1.0f : 0.0f);
    bypReverb.setTargetValue (params.getBool (pRevOn)    ? 1.0f : 0.0f);
    bool midiDirty = false;
    midiDirty |= changed (pMidiGate);
    midiDirty |= changed (pMidiHold);
    midiDirty |= changed (pMidiRelease);
    midiDirty |= changed (pMidiTranspose);
    midiDirty |= changed (pMidiBendRange);

    if (midiDirty)
        segmenter.setParams (params.get (pMidiGate), params.get (pMidiHold),
                             params.get (pMidiRelease),
                             (int) std::lround (params.get (pMidiTranspose)),
                             (int) std::lround (params.get (pMidiBendRange)));

    bypCab.setTargetValue    (params.getBool (pCabOn)    ? 1.0f : 0.0f);
    bypSub.setTargetValue    (params.getBool (pSubOn)    ? 1.0f : 0.0f);

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

void FxChain::processFilter (float* data, int numSamples) noexcept
{
    const float k = fltK;
    const float compGain = fltComp;

    for (int i = 0; i < numSamples; ++i)
    {
        const float e = fltEnv.process (data[i] * fltSensGain);
        const float sweep = e > 1.0f ? 1.0f : e;
        const float fc = fltFrom * std::pow (fltRatio, sweep);

        fltCutState += fltCutCoef * (fc - fltCutState);

        const float g = TptSvf::gForFreq (sr, fltCutState);
        data[i] = fltSvf.processLowpass (data[i], g, k) * compGain;
    }
}

void FxChain::processBoost (float* data, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
    {
        const float h = boostHair.getNextValue();
        float x = data[i] * boostGain.getNextValue();

        // Soft knee that only matters when the signal is hot — clean at stage
        // level, hair when slammed, the way a cascaded JFET gives out.
        const float k = 1.2f + 4.0f * h;
        const float sat = std::tanh (x * k) / k;
        x += (sat - x) * h;

        data[i] = boostSheen.process (x);
    }
}

void FxChain::processSub (float* data, int numSamples) noexcept
{
    subOctave.process (data, subBuf.data(), numSamples, subDivisor);

    for (int i = 0; i < numSamples; ++i)
        data[i] += subBuf[(size_t) i] * subLevel.getNextValue();
}

void FxChain::processDrive (float* data, int numSamples) noexcept
{
    // Lows go around the clipper rather than through it. Distorting the
    // fundamental is what turns bass overdrive to mud: the note intermodulates
    // with everything above it and stops being a note. Only the band above
    // Crossover reaches the voicings below; Blend then crossfades that driven
    // band back against its own clean copy.
    for (int i = 0; i < numSamples; ++i)
    {
        const float low = driveSplit.process (data[i]);
        driveLow[(size_t) i] = low;
        data[i] -= low;
        driveClean[(size_t) i] = data[i];
    }

    // Track the playing level at base rate — the plexi voice's supply sag —
    // before the block is upsampled out from under it.
    const float sag0 = std::min (1.0f, sagState);

    for (int i = 0; i < numSamples; ++i)
        sagState = sagEnv.process (data[i] * 2.5f);

    const float sag1 = std::min (1.0f, sagState);

    float* chans[1] { data };
    juce::dsp::AudioBlock<float> block (chans, 1, (size_t) numSamples);

    auto up = os->processSamplesUp (block);
    auto* u = up.getChannelPointer (0);
    const int upN = (int) up.getNumSamples();

    // Smoothers ramp at base rate; walk them linearly across the oversampled
    // block so their clocks stay honest.
    const float d0 = driveAmt.getCurrentValue();
    driveAmt.skip (numSamples);
    const float d1 = driveAmt.getCurrentValue();

    const float s0 = stackAmt.getCurrentValue();
    stackAmt.skip (numSamples);
    const float s1 = stackAmt.getCurrentValue();

    const float invN = 1.0f / (float) std::max (1, upN);
    const float dStep = (d1 - d0) * invN;
    const float sStep = (s1 - s0) * invN;
    const float eStep = (sag1 - sag0) * invN;

    float d = d0, s = s0, e = sag0;

    if (driveVoice == 1)
    {
        // Green: the mid-focused feedback clipper. Lows pass clean around
        // the diode pair, so palm-muted low strings stay tight however hard
        // the drive is pushed; Bite focuses what the clipper leans on. Stack
        // squeezes a second soft stage on top of the first.
        for (int i = 0; i < upN; ++i)
        {
            const float x = u[i];
            const float low = drvGreenLp.process (x);
            const float hp = biteTilt.process (x - low);
            float y = low + std::tanh (hp * d) * 0.95f;

            const float y2 = std::tanh (y * (1.0f + 3.0f * s));
            y += (y2 - y) * s * 0.9f;

            u[i] = y;
            d += dStep; s += sStep;
        }
    }
    else if (driveVoice == 2)
    {
        // Gold: a clean path in parallel with a germanium-style clipper —
        // soft, early, and biased so the halves round off unevenly — whose
        // share grows with the drive. Transparent at the bottom of the knob,
        // thick at the top, with the clean path holding the note's core.
        constexpr float geBias = 0.13f;
        constexpr float geRest = 0.12927f;   // tanh (geBias)

        for (int i = 0; i < upN; ++i)
        {
            const float x = u[i];
            const float b = (d - 1.0f) / (d + 8.0f);
            float dirt = biteTilt.process (x - drvGoldLp.process (x));
            dirt = std::tanh (dirt * d * (0.8f + 1.6f * s) * 1.4f + geBias) - geRest;
            u[i] = x * (1.0f - 0.6f * b) + dirt * b * 1.5f;
            d += dStep; s += sStep;
        }
    }
    else
    {
        // Plexi: two cascaded stages into a rail that droops when leaned on
        // — the bloom-and-compress of a cranked head. Stack drives the
        // second stage harder; the asymmetric first stage supplies the even
        // harmonics a push-pull output section would.
        constexpr float bias = 0.22f;
        constexpr float biasRest = 0.21652f;   // tanh (bias)

        for (int i = 0; i < upN; ++i)
        {
            const float head = 1.0f - 0.30f * e;
            float v = biteTilt.process (u[i]) * d * 0.55f;
            v = std::tanh (v + bias) - biasRest;
            const float k2 = 1.7f + 6.0f * s;
            u[i] = std::tanh (v * k2 / head) * head * 1.15f;
            d += dStep; s += sStep; e += eStep;
        }
    }

    os->processSamplesDown (block);

    // Two of the voices clip asymmetrically, which rectifies a little DC out
    // of the signal — block it before it reaches the time effects.
    for (int i = 0; i < numSamples; ++i)
    {
        const float driven = driveDC.process (tone.process (data[i]));
        const float blend = driveBlend.getNextValue();

        data[i] = (driveLow[(size_t) i]
                    + driveClean[(size_t) i] * (1.0f - blend)
                    + driven * blend)
                  * driveLevel.getNextValue();
    }
}

void FxChain::processSynth (float* data, int numSamples) noexcept
{
    const float k = synK;
    const float compGain = synComp;
    const float envOctaves = synEnvAmt * 4.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        // Keyed by what the stage hears: play harder, the voice opens and
        // swells; stop, and it dies with the string. No gate to mistrigger.
        const float key = synEnv.process (data[i] * 2.2f);
        const float keyed = key > 1.0f ? 1.0f : key;

        synPeriod += synGlideCoef * (synTargetPeriod - synPeriod);
        const float period = synPeriod < 16.0f ? 16.0f : synPeriod;
        const double inc = 1.0 / (double) period;

        synPhase += inc;
        if (synPhase >= 1.0) synPhase -= 1.0;

        synSubPhase += inc * 0.5;
        if (synSubPhase >= 1.0) synSubPhase -= 1.0;

        const float shape = synShape.getNextValue();
        const float sub = synSubLvl.getNextValue();

        const float saw = (float) (2.0 * synPhase - 1.0) - polyBlep (synPhase, inc);

        double half = synPhase + 0.5;
        if (half >= 1.0) half -= 1.0;
        float square = synPhase < 0.5 ? 1.0f : -1.0f;
        square += polyBlep (synPhase, inc);
        square -= polyBlep (half, inc);

        double subHalf = synSubPhase + 0.5;
        if (subHalf >= 1.0) subHalf -= 1.0;
        float subSquare = synSubPhase < 0.5 ? 1.0f : -1.0f;
        subSquare += polyBlep (synSubPhase, inc * 0.5);
        subSquare -= polyBlep (subHalf, inc * 0.5);

        float v = square + (saw - square) * shape + subSquare * sub;

        // The envelope opens the filter as well as the VCA — the sweep that
        // makes it read as a synth rather than a fuzz.
        const float fc = synCutBase * std::exp2 (envOctaves * keyed);
        synCutState += synCutCoef * (fc - synCutState);

        const float g = TptSvf::gForFreq (sr, synCutState);
        v = synSvf.processLowpass (v, g, k) * compGain;

        data[i] = v * keyed * 0.8f;
    }
}

void FxChain::processChorus (float* data, int numSamples) noexcept
{
    const int mask = choMask;

    double phaseInc[3];
    float baseSamples[3];

    for (int v = 0; v < 3; ++v)
    {
        phaseInc[v] = (double) choRate * (double) choRateMult[v] / sr;
        baseSamples[v] = (float) (choBaseMs[v] * 0.001 * sr);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const float x = data[i];
        choBuf[(size_t) (choWrite & mask)] = x;

        const float depth = choDepth.getNextValue();
        float wet = 0.0f;

        for (int v = 0; v < 3; ++v)
        {
            choPhase[v] += phaseInc[v];
            if (choPhase[v] >= 1.0) choPhase[v] -= 1.0;

            const float lfo = std::sin ((float) (6.283185307179586 * choPhase[v]));
            const double tap = (double) baseSamples[v] + (double) (depth * choDepthMult[v] * lfo);

            wet += readHermite (choBuf, (double) choWrite - tap, mask)
                     * choVoiceGain[v].getNextValue();
        }

        if (++choWrite >= (int) choBuf.size() * 2)
            choWrite -= (int) choBuf.size();

        // Tone darkens only the wet voices; the highpass keeps everything
        // below "Keep lows" untouched dry signal, so the low end stays tight.
        const float delta = choTone.process (wet) - x;
        data[i] = x + choLows.process (delta);
    }
}

void FxChain::processPhaser (float* data, int numSamples) noexcept
{
    int i = 0;

    const int stages = phaserStages;

    while (i < numSamples)
    {
        const int chunk = std::min (32, numSamples - i);

        lfoPhase += (double) phaserRate * (double) chunk / sr;
        lfoPhase -= std::floor (lfoPhase);

        // Triangle sweep with the corners rounded — the lamp-and-LDR shape
        // that hangs at the turnarounds instead of gliding through them.
        const float tri = 1.0f - 4.0f * std::abs ((float) lfoPhase - 0.5f);
        phaserLfoState += 0.35f * (tri - phaserLfoState);

        const float mod = 0.5f * (1.0f + phaserLfoState);
        const float f = 200.0f * std::pow (9.0f, mod * phaserDepth);   // 200 Hz .. 1.8 kHz

        for (int a2 = 0; a2 < stages; ++a2)
            ap[a2].setBreakFreq (sr, f);

        for (int k = 0; k < chunk; ++k)
        {
            const float x = data[i + k];
            float y = x + phaserFb * phaserFbState;

            for (int a2 = 0; a2 < stages; ++a2)
                y = ap[a2].process (y);

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

        // Tape transport, not an LFO: the tap drifts on lowpassed noise
        // (wow) with a faster shimmer riding it (flutter). Real machines
        // never repeat their wobble, and the ear knows.
        dlNoise = dlNoise * 1664525u + 1013904223u;
        const float r1 = (float) (int32_t) dlNoise * 4.6566129e-10f;
        dlNoise = dlNoise * 1664525u + 1013904223u;
        const float r2 = (float) (int32_t) dlNoise * 4.6566129e-10f;

        dlWow  += dlWowCoef  * (r1 * dlWowAmp - dlWow);
        dlFlut += dlFlutCoef * (r2 * 14.0f - dlFlut);

        const float wow = std::clamp (dlWow, -2.2f, 2.2f);
        const float mod = dlModDepth.getNextValue() * (wow + 0.30f * dlFlut);

        const double te = std::clamp ((double) t + (double) mod,
                                      4.0, (double) (delayBuf.size() - 4));

        const float wet = readHermite (delayBuf, (double) delayWrite - te, mask);

        // Repeats darken through the damp filter and squash through the
        // compander curve — hot regen folds over instead of running away.
        float fbIn = delayDamp.process (wet);

        // Smear: diffusion inside the loop, crossfaded in so zero really is
        // the straight wire it was before the knob existed.
        const float sm = dlSmearAmt.getNextValue();
        const float dif = dlSmear2.process (dlSmear1.process (fbIn, dlSmearLen1), dlSmearLen2);
        fbIn += (dif - fbIn) * sm;

        const float s = dlSatAmt.getNextValue();
        const float gS = 1.0f + s * 4.0f;
        fbIn += (std::tanh (fbIn * gS) / gS - fbIn) * s;

        delayBuf[(size_t) (delayWrite & mask)] = x + fbIn * delayFb.getNextValue();

        if (++delayWrite >= (int) delayBuf.size() * 2)
            delayWrite -= (int) delayBuf.size();

        data[i] = wet;
    }
}

void FxChain::processTape (float* data, int numSamples) noexcept
{
    // A touch of bias puts second harmonic under the rounding. No
    // oversampling on purpose: the knee is gentle and the stage is meant to
    // idle engaged — the drive stage is where hard clipping lives.
    constexpr float bias = 0.13f;
    constexpr float biasRest = 0.12927f;   // tanh (bias)

    for (int i = 0; i < numSamples; ++i)
    {
        const float g = tapeDrive.getNextValue();

        float v = tapePre.process (data[i]) * g;
        v = std::tanh (v + bias) - biasRest;
        v = tapePost.process (v / g);          // unity at small signal, squash at peaks
        v = tapeBump.process (v);
        v = tapeRolloff.process (v);
        data[i] = tapeDC.process (v);
    }
}

void FxChain::process (float* mono, float* left, float* right, int numSamples) noexcept
{
    updateFromParams();

    for (int i = 0; i < numSamples; ++i)
        mono[i] = dcBlock.process (mono[i]) * inGain.getNextValue();

    // Side-chain: pitch runs off the trimmed input, ahead of the movable
    // stages, so no arrangement of dirt or modulation can fool the detector.
    tracker.process (mono, numSamples);
    shifter.setPeriod (tracker.getPeriodSamples());
    shifter2.setPeriod (tracker.getPeriodSamples());
    subOctave.setPeriod (tracker.getPeriodSamples());

    midiEventCount = 0;

    if (params.getBool (pMidiOn))
        midiEventCount = segmenter.process (mono, numSamples, tracker.getPeriodSamples(),
                                            tracker.getConfidence(), midiEvents, 8);
    else if (segmenter.getSoundingNote() >= 0)
        midiEventCount = segmenter.flush (midiEvents, 8);

    soundingNote.store (segmenter.getSoundingNote(), std::memory_order_relaxed);
    synTargetPeriod = tracker.getPeriodSamples();
    liveHz.store (tracker.getFrequency(), std::memory_order_relaxed);
    liveConf.store (tracker.getConfidence(), std::memory_order_relaxed);

    const uint64_t order = orderPacked.load (std::memory_order_relaxed);

    for (int slot = 0; slot < numMovableStages; ++slot)
    {
        switch ((int) ((order >> (4 * slot)) & 15))
        {
            case stComp:
                runStage (mono, numSamples, bypComp, &compBlend, [this] (float* d, int n)
                {
                    for (int i = 0; i < n; ++i)
                        d[i] = comp.process (d[i]) * makeup.getNextValue();
                });
                break;

            case stEq:
                runStage (mono, numSamples, bypEq, nullptr, [this] (float* d, int n)
                {
                    for (int i = 0; i < n; ++i)
                    {
                        float v = d[i];

                        for (auto& band : eqBand)
                            v = band.process (v);

                        d[i] = v;
                    }
                });
                break;

            case stFilter:
                runStage (mono, numSamples, bypFilter, &fltMix, [this] (float* d, int n) { processFilter (d, n); });
                break;

            case stBoost:
                runStage (mono, numSamples, bypBoost, nullptr, [this] (float* d, int n) { processBoost (d, n); });
                break;

            case stDrive:
                runStage (mono, numSamples, bypDrive, nullptr, [this] (float* d, int n) { processDrive (d, n); });
                break;

            case stAmp:
                runStage (mono, numSamples, bypAmp, nullptr, [this] (float* d, int n)
                {
                    for (int i = 0; i < n; ++i)
                        d[i] *= ampIn.getNextValue();

                    amp.process (d, n);

                    for (int i = 0; i < n; ++i)
                        d[i] *= ampOut.getNextValue();
                });
                break;

            case stCab:
                runStage (mono, numSamples, bypCab, nullptr, [this] (float* d, int n) { cab.process (d, n); });
                break;

            case stSub:
                runStage (mono, numSamples, bypSub, nullptr, [this] (float* d, int n) { processSub (d, n); });
                break;

            case stOctave:
                runStage (mono, numSamples, bypOct, &octMix, [this] (float* d, int n)
                {
                    // Two tracked voices: the main interval in place, the
                    // second on a copy, both riding the same detected period.
                    juce::FloatVectorOperations::copy (octBuf.data(), d, n);
                    shifter.process (d, n);
                    shifter2.process (octBuf.data(), n);

                    for (int i = 0; i < n; ++i)
                        d[i] += octBuf[(size_t) i] * oct2Lvl.getNextValue();
                });
                break;

            case stSynth:
                runStage (mono, numSamples, bypSynth, &synMix, [this] (float* d, int n) { processSynth (d, n); });
                break;

            case stPhaser:
                runStage (mono, numSamples, bypPhaser, nullptr, [this] (float* d, int n) { processPhaser (d, n); });
                break;

            case stChorus:
                runStage (mono, numSamples, bypChorus, &choMix, [this] (float* d, int n) { processChorus (d, n); });
                break;

            case stDelay:
                runStage (mono, numSamples, bypDelay, &delayMix, [this] (float* d, int n) { processDelay (d, n); });
                break;

            case stTape:
                runStage (mono, numSamples, bypTape, &tapeMix, [this] (float* d, int n) { processTape (d, n); });
                break;

            default:
                break;
        }
    }

    // The tank always runs so a toggle rides the bypass crossfade with its
    // tail intact; left/right carry pure wet until the blend below.
    reverb.process (mono, left, right, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        const float g = bypReverb.getNextValue() * reverbMix.getNextValue();
        const float out = liveOut.getNextValue();

        left[i]  = (mono[i] + (left[i]  - mono[i]) * g) * out;
        right[i] = (mono[i] + (right[i] - mono[i]) * g) * out;
    }
}

} // namespace rig
