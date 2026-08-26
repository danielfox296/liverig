#include "StemPlayer.h"

#include <algorithm>
#include <cmath>

namespace rig
{

const char* const StemPlayer::stemNames[StemPlayer::numStems] { "drums", "bass", "other", "vocals" };

static constexpr double maxSongMinutes = 30.0;

StemPlayer::StemPlayer (ParamStore& p) : params (p)
{
    formats.registerBasicFormats();
}

void StemPlayer::prepare (double sampleRate, int /*maxBlockSize*/)
{
    preparedRate.store (sampleRate);

    for (int i = 0; i < numStems; ++i)
    {
        gains[i].reset (sampleRate, 0.02);
        gains[i].setCurrentAndTargetValue (0.0f);
    }
}

void StemPlayer::unload()
{
    {
        const juce::SpinLock::ScopedLockType lock (songLock);
        song = nullptr;
    }

    for (auto& s : source)
    {
        s.audio.setSize (0, 0);
        s.present = false;
        s.rate = 0.0;
    }

    for (auto& p : present)
        p = false;

    folder = juce::File();
    songName = juce::String();
    loaded = false;
    builtRate = 0.0;
    lengthSamples = 0;
    playing.store (false);
    playhead.store (0);
    command.store (0);
    clearLoop();
}

juce::Result StemPlayer::loadSongFolder (const juce::File& f)
{
    if (! f.isDirectory())
        return juce::Result::fail ("Not a folder: " + f.getFullPathName());

    auto result = decodeFolder (f);

    if (result.failed())
        return result;

    folder = f;
    songName = f.getFileName();
    loaded = true;

    const double rate = preparedRate.load();
    buildForRate (rate > 0.0 ? rate : 48000.0);
    return juce::Result::ok();
}

juce::Result StemPlayer::decodeFolder (const juce::File& f)
{
    SourceStem decoded[numStems];
    int found = 0;

    for (int i = 0; i < numStems; ++i)
    {
        juce::File file = f.getChildFile (juce::String (stemNames[i]) + ".wav");

        if (! file.existsAsFile())
        {
            // Fall back to any readable file with the right stem name.
            for (const auto& candidate : juce::RangedDirectoryIterator (f, false, "*", juce::File::findFiles))
            {
                if (candidate.getFile().getFileNameWithoutExtension().equalsIgnoreCase (stemNames[i]))
                {
                    file = candidate.getFile();
                    break;
                }
            }
        }

        if (! file.existsAsFile())
            continue;

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr)
            continue;

        if (reader->sampleRate <= 0.0 || reader->numChannels == 0)
            continue;

        const auto lengthIn = reader->lengthInSamples;

        if (lengthIn <= 0)
            continue;

        if ((double) lengthIn / reader->sampleRate > maxSongMinutes * 60.0)
            return juce::Result::fail (file.getFileName() + " is longer than "
                                        + juce::String (maxSongMinutes) + " minutes");

        const int len = (int) lengthIn;
        decoded[i].audio.setSize ((int) reader->numChannels, len + 16, false, true, false);
        decoded[i].audio.clear();
        reader->read (&decoded[i].audio, 0, len, 0, true, true);
        decoded[i].rate = reader->sampleRate;
        decoded[i].present = true;
        ++found;
    }

    if (found == 0)
        return juce::Result::fail ("No stems found in " + f.getFileName()
                                    + " (expected drums.wav / bass.wav / other.wav / vocals.wav)");

    for (int i = 0; i < numStems; ++i)
        source[i] = std::move (decoded[i]);

    return juce::Result::ok();
}

void StemPlayer::buildForRate (double rate)
{
    if (! loaded || rate <= 0.0)
        return;

    SongData::Ptr fresh = new SongData();
    int longest = 0;

    for (int i = 0; i < numStems; ++i)
    {
        if (! source[i].present)
            continue;

        const auto& in = source[i].audio;
        const int inLen = in.getNumSamples() - 16;          // trim the zero pad
        const double ratio = source[i].rate / rate;
        const bool sameRate = juce::exactlyEqual (ratio, 1.0);
        const int outLen = sameRate ? inLen
                                        : (int) std::floor ((double) inLen / ratio) - 1;

        if (outLen <= 0)
            continue;

        fresh->stems[i].setSize (2, outLen, false, true, false);
        fresh->stems[i].clear();

        for (int ch = 0; ch < 2; ++ch)
        {
            const int srcCh = juce::jmin (ch, in.getNumChannels() - 1);

            if (sameRate)
            {
                fresh->stems[i].copyFrom (ch, 0, in, srcCh, 0, outLen);
            }
            else
            {
                juce::LagrangeInterpolator interp;
                interp.reset();
                interp.process (ratio, in.getReadPointer (srcCh),
                                fresh->stems[i].getWritePointer (ch), outLen);
            }
        }

        fresh->present[i] = true;
        longest = juce::jmax (longest, outLen);
    }

    fresh->numSamples = longest;

    {
        const juce::SpinLock::ScopedLockType lock (songLock);
        song = fresh;
    }

    for (int i = 0; i < numStems; ++i)
        present[i] = fresh->present[i];

    builtRate = rate;
    lengthSamples = longest;
    playing.store (false);
    playhead.store (0);
    command.store (0);
    clearLoop();
}

void StemPlayer::reloadForCurrentRate()
{
    buildForRate (preparedRate.load());
}

bool StemPlayer::setLoop (int startSample, int endSample) noexcept
{
    const double rate = builtRate > 0.0 ? builtRate : 48000.0;
    const int minLength = (int) (0.15 * rate);

    startSample = juce::jlimit (0, juce::jmax (0, lengthSamples - 1), startSample);
    endSample = juce::jlimit (0, lengthSamples, endSample);

    if (endSample - startSample < minLength)
        return false;

    loopRange.store (((uint64_t) (uint32_t) endSample << 32) | (uint32_t) startSample,
                     std::memory_order_relaxed);
    loopOn.store (true, std::memory_order_relaxed);
    return true;
}

void StemPlayer::process (float* left, float* right, int numSamples) noexcept
{
    const int cmd = command.exchange (0, std::memory_order_acquire);

    if (cmd == 1)
    {
        playhead.store (0, std::memory_order_relaxed);
        playing.store (true, std::memory_order_relaxed);
        fading = false;
    }
    else if (cmd == 2)
    {
        playing.store (false, std::memory_order_relaxed);
        playhead.store (0, std::memory_order_relaxed);
        fading = false;
    }

    for (int i = 0; i < numStems; ++i)
    {
        const bool muted = params.getBool (pStemMute0 + i);
        const float db = params.get (pStemGain0 + i);
        gains[i].setTargetValue (muted ? 0.0f : juce::Decibels::decibelsToGain (db, -60.0f));
    }

    const juce::SpinLock::ScopedTryLockType lock (songLock);

    if (! lock.isLocked() || song == nullptr || ! playing.load (std::memory_order_relaxed))
    {
        for (auto& g : gains)
            g.skip (numSamples);

        return;
    }

    auto* data = song.get();
    int pos = playhead.load (std::memory_order_relaxed);
    const int total = data->numSamples;

    const uint64_t range = loopRange.load (std::memory_order_relaxed);
    const bool looping = loopOn.load (std::memory_order_relaxed) && range != 0;
    const int loopStart = (int) (uint32_t) range;
    const int loopEnd = (int) (uint32_t) (range >> 32);

    int outOffset = 0;
    int remaining = numSamples;

    // Segment by segment: each segment runs straight ahead, cut at the loop
    // seam, where the read jumps back and the old material ghosts across.
    while (remaining > 0)
    {
        if (looping && pos == loopEnd)
        {
            ghostPos = pos;
            fadePos = 0;
            fadeLen = juce::jmin (512, total - pos, loopEnd - loopStart);
            fading = fadeLen > 16;
            pos = loopStart;
        }

        int count = juce::jmin (remaining, total - pos);

        if (looping && pos < loopEnd)
            count = juce::jmin (count, loopEnd - pos);

        if (count <= 0)
        {
            playing.store (false, std::memory_order_relaxed);
            pos = 0;
            fading = false;
            break;
        }

        for (int i = 0; i < numStems; ++i)
        {
            auto& g = gains[i];

            if (! data->present[i])
            {
                g.skip (count);
                continue;
            }

            const int stemLen = data->stems[i].getNumSamples();
            const float* sl = data->stems[i].getReadPointer (0);
            const float* sr2 = data->stems[i].getReadPointer (1);

            for (int n = 0; n < count; ++n)
            {
                const float gain = g.getNextValue();
                const int idx = pos + n;
                float vl = idx < stemLen ? sl[idx] : 0.0f;
                float vr = idx < stemLen ? sr2[idx] : 0.0f;

                if (fading)
                {
                    const int f = fadePos + n;

                    if (f < fadeLen)
                    {
                        const int gi = ghostPos + f;
                        const float glv = gi < stemLen ? sl[gi] : 0.0f;
                        const float grv = gi < stemLen ? sr2[gi] : 0.0f;
                        const float t = (float) (f + 1) / (float) fadeLen;
                        vl = glv + (vl - glv) * t;
                        vr = grv + (vr - grv) * t;
                    }
                }

                left[outOffset + n]  += vl * gain;
                right[outOffset + n] += vr * gain;
            }
        }

        if (fading)
        {
            fadePos += count;

            if (fadePos >= fadeLen)
                fading = false;
        }

        pos += count;
        outOffset += count;
        remaining -= count;

        if (! looping && pos >= total)
        {
            playing.store (false, std::memory_order_relaxed);
            pos = 0;
            fading = false;
            break;
        }
    }

    if (remaining > 0)
        for (auto& g : gains)
            g.skip (remaining);

    playhead.store (pos, std::memory_order_relaxed);
}

} // namespace rig
