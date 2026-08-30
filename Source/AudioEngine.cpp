#include "AudioEngine.h"

namespace rig
{

AudioEngine::AudioEngine (ParamStore& p) : params (p), stems (p), fx (p)
{
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

void AudioEngine::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message)
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

    scratch.setSize (6, maxBlock, false, true, false);
    scratch.clear();

    fx.prepare (rate, maxBlock);
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
        juce::Decibels::decibelsToGain (params.get (pMasterGain), -60.0f));

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

    const int bassIn = bassSlot.load (std::memory_order_relaxed);
    const int micIn = micSlot.load (std::memory_order_relaxed);

    for (int offset = 0; offset < numSamples;)
    {
        const int n = juce::jmin (maxBlock, numSamples - offset);

        // --- live lane: instrument on its assigned input channel ---
        if (bassIn < numInputChannels && inputChannelData[bassIn] != nullptr)
            juce::FloatVectorOperations::copy (monoIn, inputChannelData[bassIn] + offset, n);
        else
            juce::FloatVectorOperations::clear (monoIn, n);

        updatePeak (inPeak, monoIn, n, decay);

        fx.process (monoIn, liveL, liveR, n);

        int midiCount = 0;
        const auto* events = fx.getMidiEvents (midiCount);

        for (int e = 0; e < midiCount; ++e)
            midi.push (events[e]);

        if (params.getBool (pInstOn))
        {
            const float target = juce::Decibels::decibelsToGain (params.get (pInstLevel), -60.0f);
            instrumentGain += 0.25f * (target - instrumentGain);
            instrument.process (events, midiCount, liveL, liveR, n, instrumentGain);
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
        vocal.process (vocalIn, fx.getCarrier(), liveL, liveR, n);

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

        masterGain.setTargetValue (juce::Decibels::decibelsToGain (params.get (pMasterGain), -60.0f));

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
