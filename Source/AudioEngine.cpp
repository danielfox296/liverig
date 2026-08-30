#include "AudioEngine.h"

namespace rig
{

AudioEngine::AudioEngine (ParamStore& p) : params (p), stems (p), fx (p)
{
    for (int i = 0; i < ParamStore::maxChannels - 3; ++i)
        spareChains[i] = std::make_unique<FxChain> (params, 3 + i);

    for (auto& slot : sourceSlot)
        slot.store (-1, std::memory_order_relaxed);
}

AudioEngine::~AudioEngine()
{
    shutdown();
}

juce::String AudioEngine::initialise (std::unique_ptr<juce::XmlElement> savedState)
{
    midi.open();   // a virtual port, so Logic and MainStage just see it
    const bool hadSavedState = savedState != nullptr;
    auto error = deviceManager.initialise (2, 2, savedState.get(), true);

    if (error.isEmpty())
    {
        // A fresh install lands on the OS default buffer — typically 512
        // samples, which is over 20 ms of round trip before the chain does
        // anything. A live rig would rather start small. Only the untouched
        // default is overridden: pick a size in the device dialog and that
        // choice is saved and honoured from then on.
        if (! hadSavedState)
            preferLowLatencyBuffer();

        deviceManager.addAudioCallback (this);
        callbackAdded = true;
    }

    enableAllMidiInputs();
    deviceManager.addMidiInputDeviceCallback ({}, this);

    return error;
}

void AudioEngine::enableAllMidiInputs()
{
    for (const auto& device : juce::MidiInput::getAvailableDevices())
        if (! deviceManager.isMidiInputDeviceEnabled (device.identifier))
            deviceManager.setMidiInputDeviceEnabled (device.identifier, true);
}

void AudioEngine::handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& message)
{
    // The mic lanes, foot-controller shaped: Program Change 0 is silence,
    // 1 sings through the autotune, 2 makes the bass sing. Values land
    // straight in the parameter store, which is atomic — no thread
    // gymnastics. Stacking both lanes stays a hands-on-the-screen choice.
    if (message.isProgramChange())
    {
        switch (message.getProgramChangeNumber())
        {
            case 0: params.set (pVocalOn, 0.0f); params.set (pVocoderOn, 0.0f); break;
            case 1: params.set (pVocalOn, 1.0f); params.set (pVocoderOn, 0.0f); break;
            case 2: params.set (pVocalOn, 0.0f); params.set (pVocoderOn, 1.0f); break;
            default: break;
        }

        return;
    }

    // Played notes head for the instrument. This runs on the MIDI thread:
    // the lock guards only the chosen-device id, never the audio path.
    if (! (message.isNoteOnOrOff() || message.isPitchWheel()))
        return;

    {
        const juce::ScopedLock lock (midiSourceLock);

        if (midiSourceId.isNotEmpty()
             && (source == nullptr || source->getIdentifier() != midiSourceId))
            return;
    }

    NoteEvent event;

    if (message.isNoteOn())
        event = { NoteEvent::noteOn, (int16_t) message.getNoteNumber(),
                  (int16_t) juce::jmax (1, (int) message.getVelocity()) };
    else if (message.isNoteOff())
        event = { NoteEvent::noteOff, (int16_t) message.getNoteNumber(), 0 };
    else
        event = { NoteEvent::bend, (int16_t) message.getPitchWheelValue(), 0 };

    const int w = midiWrite.load (std::memory_order_relaxed);
    const int next = (w + 1) % midiQueueSize;

    if (next != midiRead.load (std::memory_order_acquire))   // full = drop, never block
    {
        midiQueue[w] = event;
        midiWrite.store (next, std::memory_order_release);
    }
}

void AudioEngine::preferLowLatencyBuffer()
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return;

    int best = 0;

    for (int size : device->getAvailableBufferSizes())
        if (size >= 64 && (best == 0 || size < best))
            best = size;

    if (best == 0 || best >= device->getCurrentBufferSizeSamples())
        return;

    auto setup = deviceManager.getAudioDeviceSetup();
    setup.bufferSize = best;
    deviceManager.setAudioDeviceSetup (setup, true);
}

float AudioEngine::getIoLatencyMs() noexcept
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const double rate = device->getCurrentSampleRate();

        if (rate > 0.0)
            return (float) ((device->getInputLatencyInSamples() + device->getOutputLatencyInSamples())
                              / rate * 1000.0);
    }

    return 0.0f;
}

void AudioEngine::shutdown()
{
    deviceManager.removeMidiInputDeviceCallback ({}, this);
    midi.close();
    if (callbackAdded)
    {
        deviceManager.removeAudioCallback (this);
        callbackAdded = false;
    }

    deviceManager.closeAudioDevice();
}

float AudioEngine::getFxLatencyMs() const noexcept
{
    const double rate = currentRate.load (std::memory_order_relaxed);
    return rate > 0.0 ? (float) (fx.getLatencySamples() / rate * 1000.0) : 0.0f;
}

void AudioEngine::setInputRoles (int bassChannel, int micChannel) noexcept
{
    bassWanted.store (bassChannel, std::memory_order_relaxed);
    micWanted.store (micChannel, std::memory_order_relaxed);
    updateInputSlots();
}

void AudioEngine::updateInputSlots() noexcept
{
    const juce::uint64 mask = activeInputMask.load (std::memory_order_relaxed);

    // A role's slot is its channel's position among the active channels —
    // the compaction the device callback applies. Unassigned or inactive
    // roles fall back to the old rule: bass first, mic second.
    const auto slotOf = [mask] (int channel, int fallback)
    {
        if (channel < 1 || channel > 64 || (mask & (1ull << (channel - 1))) == 0)
            return fallback;

        int slot = 0;

        for (int bit = 0; bit < channel - 1; ++bit)
            if ((mask & (1ull << bit)) != 0)
                ++slot;

        return slot;
    };

    bassSlot.store (slotOf (bassWanted.load (std::memory_order_relaxed), 0),
                    std::memory_order_relaxed);
    micSlot.store (slotOf (micWanted.load (std::memory_order_relaxed), 1),
                   std::memory_order_relaxed);

    // The generic strips get no fallback: an unclaimed or vanished jack
    // leaves the strip silent rather than guessing.
    for (int s = 3; s < ParamStore::maxChannels; ++s)
    {
        const int want = sourceWanted[(size_t) s].load (std::memory_order_relaxed);
        sourceSlot[(size_t) s].store (want >= 1 ? slotOf (want, -1) : -1,
                                      std::memory_order_relaxed);
    }
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double rate = device->getCurrentSampleRate();
    maxBlock = juce::jmax (16, device->getCurrentBufferSizeSamples());

    currentRate.store (rate, std::memory_order_relaxed);
    currentBlock.store (maxBlock, std::memory_order_relaxed);

    const auto activeChans = device->getActiveInputChannels();
    activeInputs.store (activeChans.countNumberOfSetBits(), std::memory_order_relaxed);

    juce::uint64 mask = 0;

    for (int bit = activeChans.findNextSetBit (0);
         bit >= 0 && bit < 64;
         bit = activeChans.findNextSetBit (bit + 1))
        mask |= 1ull << bit;

    activeInputMask.store (mask, std::memory_order_relaxed);
    updateInputSlots();

    scratch.setSize (11, maxBlock, false, true, false);
    scratch.clear();

    fx.prepare (rate, maxBlock);
    micChain.prepare (rate, maxBlock);
    keysChain.prepare (rate, maxBlock);

    for (auto& chain : spareChains)
        if (chain != nullptr)
            chain->prepare (rate, maxBlock);

    for (auto& gain : passGains)
        gain.reset (rate, 0.02);

    alignRings.setSize (ParamStore::maxChannels * 2, alignRingSize, false, true, false);
    alignRings.clear();

    for (auto& pos : alignPos)
        pos = 0;

    for (auto& peak : slotPeaks)
        peak.store (0.0f);

    instrument.prepare (rate, maxBlock);
    stems.prepare (rate, maxBlock);

    tuneSlot.prepare (rate, maxBlock);
    vocoderSlot.prepare (rate, maxBlock);
    vocal.prepare (rate, maxBlock);

    ducker.prepare (rate);
    limiter.prepare (rate);
    trackGain.reset (rate, 0.02);
    trackGain.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (params.get (pTrackGain), -60.0f));
    masterGain.reset (rate, 0.02);
    masterGain.setCurrentAndTargetValue (
        params.getBool (pMasterMute) ? 0.0f
                                     : juce::Decibels::decibelsToGain (params.get (pMasterGain), -60.0f));

    inPeak.store (0.0f);
    vocalPeak.store (0.0f);
    outPeakL.store (0.0f);
    outPeakR.store (0.0f);
    limiterGr.store (0.0f);
}

void AudioEngine::audioDeviceStopped()
{
    currentRate.store (0.0, std::memory_order_relaxed);
    currentBlock.store (0, std::memory_order_relaxed);
    inPeak.store (0.0f);
    vocalPeak.store (0.0f);
    outPeakL.store (0.0f);
    outPeakR.store (0.0f);

    for (auto& peak : slotPeaks)
        peak.store (0.0f);
}

// A signal through its strip's board, rendered into the path buffers. With
// pedals loaded it folds to mono into the chain and comes back stereo;
// with the board empty it passes in stereo, shaped only by the strip's
// input and live-out rows.
void AudioEngine::renderBoard (FxChain& chain, bool active, juce::SmoothedValue<float>& passGain,
                               int slot, const float* inL, const float* inR,
                               float* pathL, float* pathR, float* busIn, int n) noexcept
{
    if (active)
    {
        for (int i = 0; i < n; ++i)
            busIn[i] = 0.5f * (inL[i] + inR[i]);

        chain.process (busIn, pathL, pathR, n);
        return;
    }

    // Either mute on the strip closes the empty board the same way a chain
    // would: nothing arrives and nothing leaves.
    const bool muted = params.getBool (slot, pInMute) || params.getBool (slot, pLiveMute);
    passGain.setTargetValue (muted ? 0.0f
                                   : juce::Decibels::decibelsToGain (params.get (slot, pInGain), -60.0f)
                                       * juce::Decibels::decibelsToGain (params.get (slot, pLiveOut), -60.0f));

    for (int i = 0; i < n; ++i)
    {
        const float g = passGain.getNextValue();
        pathL[i] = inL[i] * g;
        pathR[i] = inR[i] * g;
    }
}

// A path joins the live sum through its alignment ring. The ring is always
// written so a delay can arrive without serving stale history; at zero
// delay the fresh sample reads straight back out.
void AudioEngine::alignedAdd (int slot, int delaySamples, const float* inL, const float* inR,
                              float* outL, float* outR, int n) noexcept
{
    const int d = juce::jlimit (0, alignRingSize - 1, delaySamples);
    float* ringL = alignRings.getWritePointer (slot * 2);
    float* ringR = alignRings.getWritePointer (slot * 2 + 1);
    int pos = alignPos[slot];

    for (int i = 0; i < n; ++i)
    {
        ringL[pos] = inL[i];
        ringR[pos] = inR[i];

        const int read = pos - d >= 0 ? pos - d : pos - d + alignRingSize;
        outL[i] += ringL[read];
        outR[i] += ringR[read];

        if (++pos == alignRingSize)
            pos = 0;
    }

    alignPos[slot] = pos;
}

static void updatePeak (std::atomic<float>& slot, const float* data, int n, float decay) noexcept
{
    float peak = 0.0f;

    for (int i = 0; i < n; ++i)
        peak = juce::jmax (peak, std::abs (data[i]));

    const float previous = slot.load (std::memory_order_relaxed) * decay;
    slot.store (juce::jmax (peak, previous), std::memory_order_relaxed);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;

    const double rate = currentRate.load (std::memory_order_relaxed);
    const float decay = rate > 0.0 ? std::exp (-(float) numSamples / (float) (rate * 0.35)) : 0.0f;

    float* monoIn  = scratch.getWritePointer (0);
    float* liveL   = scratch.getWritePointer (1);
    float* liveR   = scratch.getWritePointer (2);
    float* mixL    = scratch.getWritePointer (3);
    float* mixR    = scratch.getWritePointer (4);
    float* vocalIn = scratch.getWritePointer (5);
    float* laneL   = scratch.getWritePointer (6);
    float* laneR   = scratch.getWritePointer (7);
    float* busIn   = scratch.getWritePointer (8);
    float* tmpL    = scratch.getWritePointer (9);
    float* tmpR    = scratch.getWritePointer (10);

    const int bassIn = bassSlot.load (std::memory_order_relaxed);
    const int micIn = micSlot.load (std::memory_order_relaxed);

    // Alignment at the sum: every sounding jack-fed path is delayed to the
    // slowest of them, so one source through two boards cannot comb. An
    // empty board adds nothing and costs nothing; the lanes stay untouched.
    int alignDelay[ParamStore::maxChannels] {};
    {
        float slowest = fx.getLatencySamples();

        for (int s = 3; s < ParamStore::maxChannels; ++s)
            if (sourceSlot[(size_t) s].load (std::memory_order_relaxed) >= 0
                 && boardActive[(size_t) s].load (std::memory_order_relaxed))
                slowest = juce::jmax (slowest, getChain (s).getLatencySamples());

        alignDelay[0] = (int) std::lround (slowest - fx.getLatencySamples());

        for (int s = 3; s < ParamStore::maxChannels; ++s)
        {
            const float own = boardActive[(size_t) s].load (std::memory_order_relaxed)
                                ? getChain (s).getLatencySamples() : 0.0f;
            alignDelay[s] = (int) std::lround (slowest - own);
        }
    }

    for (int offset = 0; offset < numSamples;)
    {
        const int n = juce::jmin (maxBlock, numSamples - offset);

        // --- live lane: instrument on its assigned input channel ---
        if (bassIn < numInputChannels && inputChannelData[bassIn] != nullptr)
            juce::FloatVectorOperations::copy (monoIn, inputChannelData[bassIn] + offset, n);
        else
            juce::FloatVectorOperations::clear (monoIn, n);

        updatePeak (inPeak, monoIn, n, decay);

        fx.process (monoIn, tmpL, tmpR, n);
        juce::FloatVectorOperations::clear (liveL, n);
        juce::FloatVectorOperations::clear (liveR, n);
        alignedAdd (0, alignDelay[0], tmpL, tmpR, liveL, liveR, n);

        int midiCount = 0;
        const auto* events = fx.getMidiEvents (midiCount);

        for (int e = 0; e < midiCount; ++e)
            midi.push (events[e]);

        // The tracker's notes and the keys player's own hands, one stream —
        // the hardware queue drains here, on the audio thread's clock.
        NoteEvent merged[96];
        int mergedCount = 0;

        for (int e = 0; e < midiCount && mergedCount < 64; ++e)
            merged[mergedCount++] = events[e];

        {
            int r = midiRead.load (std::memory_order_relaxed);
            const int w = midiWrite.load (std::memory_order_acquire);

            while (r != w && mergedCount < 96)
            {
                merged[mergedCount++] = midiQueue[r];
                r = (r + 1) % midiQueueSize;
            }

            midiRead.store (r, std::memory_order_release);
        }

        if (params.getBool (pInstOn))
        {
            const float target = juce::Decibels::decibelsToGain (params.get (pInstLevel), -60.0f);
            instrumentGain += 0.25f * (target - instrumentGain);

            juce::FloatVectorOperations::clear (laneL, n);
            juce::FloatVectorOperations::clear (laneR, n);
            instrument.process (merged, mergedCount, laneL, laneR, n, instrumentGain);
            renderBoard (keysChain, boardActive[2].load (std::memory_order_relaxed), passGains[2],
                         2, laneL, laneR, tmpL, tmpR, busIn, n);
            juce::FloatVectorOperations::add (liveL, tmpL, n);
            juce::FloatVectorOperations::add (liveR, tmpR, n);
        }

        // --- mic lanes: the assigned mic channel, summed in as performance.
        // The vocoder's carrier is the bass after the whole chain, tapped
        // ahead of Live out — so the bass can sing while the bass itself is
        // silent. One active channel means no mic; the lane goes quiet. ---
        if (micIn != bassIn && micIn < numInputChannels && inputChannelData[micIn] != nullptr)
            juce::FloatVectorOperations::copy (vocalIn, inputChannelData[micIn] + offset, n);
        else
            juce::FloatVectorOperations::clear (vocalIn, n);

        updatePeak (vocalPeak, vocalIn, n, decay);

        juce::FloatVectorOperations::clear (laneL, n);
        juce::FloatVectorOperations::clear (laneR, n);
        vocal.process (vocalIn, fx.getCarrier(), laneL, laneR, n);
        renderBoard (micChain, boardActive[1].load (std::memory_order_relaxed), passGains[1],
                     1, laneL, laneR, tmpL, tmpR, busIn, n);
        juce::FloatVectorOperations::add (liveL, tmpL, n);
        juce::FloatVectorOperations::add (liveR, tmpR, n);

        // --- the generic strips: each claimed jack through its own board,
        // aligned into the sum, metered on the way ---
        for (int s = 3; s < ParamStore::maxChannels; ++s)
        {
            const int src = sourceSlot[(size_t) s].load (std::memory_order_relaxed);

            if (src < 0 || src >= numInputChannels || inputChannelData[src] == nullptr)
                continue;

            const float* jack = inputChannelData[src] + offset;
            renderBoard (getChain (s), boardActive[(size_t) s].load (std::memory_order_relaxed),
                         passGains[(size_t) s], s, jack, jack, laneL, laneR, busIn, n);

            updatePeak (slotPeaks[(size_t) s], laneL, n, decay);
            alignedAdd (s, alignDelay[s], laneL, laneR, liveL, liveR, n);
        }

        // --- playback lane ---
        juce::FloatVectorOperations::clear (mixL, n);
        juce::FloatVectorOperations::clear (mixR, n);

        stems.process (mixL, mixR, n);

        // The whole song under one fader, ahead of the ducker — the track
        // rides beneath the playing at whatever size the player sets.
        trackGain.setTargetValue (juce::Decibels::decibelsToGain (params.get (pTrackGain), -60.0f));

        for (int i = 0; i < n; ++i)
        {
            const float tg = trackGain.getNextValue();
            mixL[i] *= tg;
            mixR[i] *= tg;
        }

        for (int i = 0; i < n; ++i)
            ducker.process (mixL[i], mixR[i], liveL[i], liveR[i]);

        // --- master sum: gain, then the safety limiter ---
        juce::FloatVectorOperations::add (mixL, liveL, n);
        juce::FloatVectorOperations::add (mixR, liveR, n);

        // Master mute stands on the whole sum — playing and playback alike.
        masterGain.setTargetValue (params.getBool (pMasterMute)
                                     ? 0.0f
                                     : juce::Decibels::decibelsToGain (params.get (pMasterGain), -60.0f));

        float worstGr = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const float g = masterGain.getNextValue();
            mixL[i] *= g;
            mixR[i] *= g;

            limiter.process (mixL[i], mixR[i]);
            worstGr = juce::jmax (worstGr, limiter.getReductionDb());
        }

        // Meters read what actually reaches the device.
        updatePeak (outPeakL, mixL, n, decay);
        updatePeak (outPeakR, mixR, n, decay);

        const float grDecayed = limiterGr.load (std::memory_order_relaxed) * decay;
        limiterGr.store (juce::jmax (worstGr, grDecayed), std::memory_order_relaxed);

        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            if (outputChannelData[ch] == nullptr)
                continue;

            const float* src = ch == 0 ? mixL : (ch == 1 ? mixR : nullptr);

            if (src != nullptr)
                juce::FloatVectorOperations::copy (outputChannelData[ch] + offset, src, n);
            else
                juce::FloatVectorOperations::clear (outputChannelData[ch] + offset, n);
        }

        offset += n;
    }
}

} // namespace rig
