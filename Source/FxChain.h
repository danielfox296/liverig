#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <juce_dsp/juce_dsp.h>

#include "Parameters.h"
#include "dsp/AmpModel.h"
#include "dsp/Biquad.h"
#include "dsp/CabSim.h"
#include "dsp/GlueComp.h"
#include "dsp/NoteSegmenter.h"
#include "dsp/OctaveShifter.h"
#include "dsp/PitchTracker.h"
#include "dsp/PlateVerb.h"
#include "dsp/SubOctave.h"

namespace rig
{

class EffectSlot;

// The live lane: mono in, stereo out, fixed effect order.
//
// Every effect runs on every block whether or not it is bypassed, and bypass
// is a 15 ms crossfade between the effect's dry input and its output. That
// costs a little idle CPU and buys two things worth more on stage: click-free
// toggling with live tails, and a constant per-block cost — no load spikes
// when a footswitch-sized decision lands at 64 samples.
class FxChain
{
public:
    // The reorderable stages, numbered in default chain order. Input, reverb
    // and live out are fixed: input is the tap everything hangs off, reverb
    // is where mono becomes stereo.
    enum Stage
    {
        stComp = 0, stEq, stFilter, stBoost, stDrive, stAmp, stCab,
        stOctave, stSub, stSynth, stPhaser, stChorus, stDelay, stTape,
        stPedal,   // a hosted plugin as a chain insert — 15 stages still packs into the order word
        numMovableStages
    };

    explicit FxChain (ParamStore& p) : params (p) {}

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    void process (float* mono, float* left, float* right, int numSamples) noexcept;

    // The order the movable stages run in, as an array of Stage values —
    // one atomic word, so the UI can reshuffle while audio runs.
    void setStageOrder (const int* stages) noexcept
    {
        uint64_t packed = 0;

        for (int i = 0; i < numMovableStages; ++i)
            packed |= (uint64_t) (stages[i] & 15) << (4 * i);

        orderPacked.store (packed, std::memory_order_relaxed);
    }

    // Total added latency of the chain in samples (oversampler + shifter).
    float getLatencySamples() const noexcept { return reportedLatency.load (std::memory_order_relaxed); }

    // What the pitch tracker hears right now — the tuner reads these.
    float getTrackedHz() const noexcept         { return liveHz.load (std::memory_order_relaxed); }
    float getTrackedConfidence() const noexcept { return liveConf.load (std::memory_order_relaxed); }

    // MIDI events the segmenter produced during the last process() call.
    // FxChain deliberately knows nothing about ports or devices: the engine
    // drains these and hands them to the sender.
    const NoteEvent* getMidiEvents (int& count) const noexcept
    {
        count = midiEventCount;
        return midiEvents;
    }

    int getSoundingNote() const noexcept { return soundingNote.load (std::memory_order_relaxed); }

    // The NAM capture player behind the Amp stage — the UI loads into it.
    AmpModel& getAmp() noexcept { return amp; }

    // The Pedal stage's hosted plugin. Set once before audio starts; the
    // slot handles its own thread-safety, so the chain just calls it.
    void setPedalSlot (EffectSlot* slot) noexcept { pedal = slot; }

    // The bass after every movable stage — octave, drive, all of it — before
    // the reverb makes it stereo and before Live out scales it. This is the
    // vocoder's carrier: it stays alive even when MIDI thru mutes the lane,
    // which is exactly the "bass sings, bass silent" configuration.
    const float* getCarrier() const noexcept { return carrierBuf.data(); }

private:
    void updateFromParams() noexcept;
    bool changed (int paramId) noexcept;

    template <typename Fn>
    void runStage (float* data, int numSamples,
                   juce::SmoothedValue<float>& bypassRamp,
                   juce::SmoothedValue<float>* mixRamp,
                   Fn&& fn) noexcept;

    void processFilter (float* data, int numSamples) noexcept;
    void processBoost (float* data, int numSamples) noexcept;
    void processDrive (float* data, int numSamples) noexcept;
    void processSub (float* data, int numSamples) noexcept;
    void processSynth (float* data, int numSamples) noexcept;
    void processPhaser (float* data, int numSamples) noexcept;
    void processChorus (float* data, int numSamples) noexcept;
    void processDelay (float* data, int numSamples) noexcept;
    void processTape (float* data, int numSamples) noexcept;

    ParamStore& params;

    double sr = 48000.0;
    int maxBlock = 512;

    float lastSeen[(size_t) NumParams] {};
    bool primed = false;

    DCBlocker dcBlock;
    juce::SmoothedValue<float> inGain, makeup, driveAmt, driveLevel, liveOut;

    GlueComp comp;
    juce::SmoothedValue<float> compBlend;

    // Parametric EQ: low shelf, three bells, high shelf, run in series.
    Biquad eqBand[5];

    // Envelope filter: follower drives a swept SVF between From and To.
    EnvFollower fltEnv;
    TptSvf fltSvf;
    juce::SmoothedValue<float> fltMix;
    float fltSensGain = 6.0f, fltFrom = 120.0f, fltRatio = 13.3f;
    float fltK = 0.3f, fltComp = 1.0f;
    float fltCutState = 120.0f, fltCutCoef = 0.007f;

    // Boost: clean gain into a soft JFET-style knee, then a character shelf.
    juce::SmoothedValue<float> boostGain, boostHair;
    Biquad boostSheen;

    std::unique_ptr<juce::dsp::Oversampling<float>> os;
    Biquad tone;

    // Drive: three circuit voicings run inside the oversampled region. The
    // splits keep each voicing's clean band out of its clipper; the sag
    // follower is the supply drooping under a hot passage.
    juce::SmoothedValue<float> stackAmt;
    Biquad biteTilt;
    OnePoleLP drvGreenLp, drvGoldLp;
    EnvFollower sagEnv;
    float sagState = 0.0f;
    int driveVoice = 1;
    DCBlocker driveDC;

    // Cabinet: what the Amp stage supplies only when a capture is loaded.
    CabSim cab;

    // Divider sub, added under the dry signal rather than replacing it.
    SubOctave subOctave;
    juce::SmoothedValue<float> subLevel;
    std::vector<float> subBuf;
    int subDivisor = 1;

    // Drive is wrapped in a crossover: lows go round the clipper entirely,
    // and Blend crossfades the driven band against the clean one.
    OnePoleLP driveSplit;
    juce::SmoothedValue<float> driveBlend;
    std::vector<float> driveLow, driveClean;

    // Amp: a loaded NAM capture between its own gain trims.
    AmpModel amp;
    juce::SmoothedValue<float> ampIn, ampOut;

    // The tracker as a note source. Liveness comes from confidence, never
    // from the held period.
    NoteSegmenter segmenter;
    NoteEvent midiEvents[8];
    int midiEventCount = 0;
    std::atomic<int> soundingNote { -1 };

    PitchTracker tracker;
    OctaveShifter shifter, shifter2;
    juce::SmoothedValue<float> octMix, oct2Lvl;
    std::vector<float> octBuf;

    // Synth voice: polyBLEP oscillators keyed by the input envelope, riding
    // the same pitch tracker as the octave shifter.
    double synPhase = 0.0, synSubPhase = 0.0;
    float synPeriod = 600.0f, synTargetPeriod = 600.0f, synGlideCoef = 1.0f;
    EnvFollower synEnv;
    TptSvf synSvf;
    juce::SmoothedValue<float> synShape, synSubLvl, synMix;
    float synCutBase = 350.0f, synEnvAmt = 0.6f, synK = 0.4f, synComp = 1.0f;
    float synCutState = 350.0f, synCutCoef = 0.007f;

    AllpassOne ap[8];
    double lfoPhase = 0.0;
    float phaserFb = 0.0f, phaserRate = 0.5f, phaserDepth = 0.6f, phaserFbState = 0.0f;
    float phaserLfoState = 0.0f;
    int phaserStages = 4;

    // Chorus: up to three modulated taps on one short line.
    std::vector<float> choBuf;
    int choMask = 0, choWrite = 0;
    double choPhase[3] { 0.0, 0.0, 0.0 };
    float choRate = 0.7f;
    juce::SmoothedValue<float> choDepth, choVoiceGain[3], choMix;
    OnePoleLP choTone;
    Biquad choLows;

    std::vector<float> delayBuf;
    int delayMask = 0, delayWrite = 0;
    juce::SmoothedValue<float> delayTime, delayFb, delayMix;
    OnePoleLP delayDamp;
    juce::SmoothedValue<float> dlModDepth, dlSatAmt;

    // Tape motion: wow is a slow random drift, flutter a faster shimmer on
    // top — lowpassed noise, never a sine. Smear diffuses the regeneration.
    uint32_t dlNoise = 2463534242u;
    float dlWow = 0.0f, dlFlut = 0.0f;
    float dlWowCoef = 0.0f, dlFlutCoef = 0.0f, dlWowAmp = 0.0f;
    float dlModRate = 0.9f;
    BufferedAllpass dlSmear1, dlSmear2;
    int dlSmearLen1 = 311, dlSmearLen2 = 149;
    juce::SmoothedValue<float> dlSmearAmt;

    // Tape: pre-emphasis into a softly biased saturator with the inverse
    // shelf after — highs saturate first, which is what rounds a harsh top.
    Biquad tapePre, tapePost, tapeBump;
    OnePoleLP tapeRolloff;
    DCBlocker tapeDC;
    juce::SmoothedValue<float> tapeDrive, tapeMix;

    PlateVerb reverb;
    juce::SmoothedValue<float> reverbMix;

    juce::SmoothedValue<float> bypComp, bypEq, bypFilter, bypBoost, bypDrive, bypAmp, bypOct,
                               bypSynth, bypPhaser, bypChorus, bypDelay, bypTape, bypReverb, bypCab, bypSub,
                               bypPedal;

    EffectSlot* pedal = nullptr;
    std::vector<float> carrierBuf;

    std::vector<float> dry;

    static constexpr uint64_t defaultPackedOrder()
    {
        uint64_t packed = 0;

        for (int i = 0; i < numMovableStages; ++i)
            packed |= (uint64_t) i << (4 * i);

        return packed;
    }

    std::atomic<uint64_t> orderPacked { defaultPackedOrder() };
    static_assert (std::atomic<uint64_t>::is_always_lock_free, "need a lock-free order word");

    std::atomic<float> reportedLatency { 0.0f };
    std::atomic<float> liveHz { 0.0f }, liveConf { 0.0f };
};

} // namespace rig
