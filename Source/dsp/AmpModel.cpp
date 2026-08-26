#include "AmpModel.h"

#include <cmath>
#include <filesystem>

#include "NAM/get_dsp.h"

namespace rig
{

// Captures are normalised towards this playback level, so switching between
// two captures compares their tone rather than how hot they were measured.
static constexpr double kTargetLoudnessDb = -18.0;

AmpModel::AmpModel() = default;

AmpModel::~AmpModel()
{
    collectGarbage();

    if (auto* p = pending.exchange (nullptr))
        delete p;

    delete active;
    active = nullptr;
}

void AmpModel::prepare (double sampleRate, int maxBlockSize)
{
    sr.store (sampleRate, std::memory_order_relaxed);
    maxBlock.store (maxBlockSize, std::memory_order_relaxed);

    scratch.assign ((size_t) maxBlockSize, 0.0f);

    // The callback is stopped here, so the handoff state can be settled
    // directly: adopt anything in flight, then re-prime at the new rate.
    if (auto* p = pending.exchange (nullptr))
    {
        delete active;
        active = p;
    }

    if (clearRequest.exchange (false))
    {
        delete active;
        active = nullptr;
    }

    if (active != nullptr)
        active->Reset (sampleRate, maxBlockSize);   // prewarms by default
}

bool AmpModel::load (const juce::File& file, juce::String& error)
{
    std::unique_ptr<nam::DSP> model;

    try
    {
        model = nam::get_dsp (std::filesystem::path (file.getFullPathName().toStdString()));
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }

    if (model == nullptr)
    {
        error = "not a readable .nam capture";
        return false;
    }

    // Priming — buffer allocation and prewarm — happens here, off the audio
    // thread, so the first processed block costs the same as every other.
    model->Reset (sr.load (std::memory_order_relaxed), maxBlock.load (std::memory_order_relaxed));

    float gain = 1.0f;

    if (model->HasLoudness())
    {
        const double makeupDb = juce::jlimit (-20.0, 20.0, kTargetLoudnessDb - model->GetLoudness());
        gain = (float) std::pow (10.0, makeupDb / 20.0);
    }

    normGain.store (gain, std::memory_order_relaxed);

    if (auto* unconsumed = pending.exchange (model.release()))
        delete unconsumed;   // replaced before the audio thread ever saw it

    clearRequest.store (false, std::memory_order_relaxed);
    loadedName = file.getFileNameWithoutExtension();
    loadedPath = file.getFullPathName();
    modelLive.store (true, std::memory_order_relaxed);
    return true;
}

juce::File AmpModel::libraryDir()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
              .getChildFile ("Application Support")
              .getChildFile ("LiveRig")
              .getChildFile ("captures");
}

juce::File AmpModel::importToLibrary (const juce::File& source, juce::String& error,
                                      const juce::File& dirOverride)
{
    const auto dir = dirOverride == juce::File() ? libraryDir() : dirOverride;
    dir.createDirectory();

    const auto dest = dir.getChildFile (source.getFileName());

    if (dest == source)
        return dest;   // already lives in the library

    if (! source.copyFileTo (dest))
    {
        error = "could not copy " + source.getFileName() + " into the capture library";
        return {};
    }

    return dest;
}

juce::Array<juce::File> AmpModel::listLibrary (const juce::File& dirOverride)
{
    const auto dir = dirOverride == juce::File() ? libraryDir() : dirOverride;

    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.nam");
    files.sort();
    return files;
}

juce::File AmpModel::libraryFileForName (const juce::String& name, const juce::File& dirOverride)
{
    const auto dir = dirOverride == juce::File() ? libraryDir() : dirOverride;
    return dir.getChildFile (name + ".nam");
}

void AmpModel::clearModel()
{
    if (auto* unconsumed = pending.exchange (nullptr))
        delete unconsumed;

    clearRequest.store (true, std::memory_order_relaxed);
    loadedName.clear();
    loadedPath.clear();
    modelLive.store (false, std::memory_order_relaxed);
}

void AmpModel::collectGarbage() noexcept
{
    for (auto& slot : retired)
        if (auto* m = slot.exchange (nullptr))
            delete m;
}

void AmpModel::process (float* data, int numSamples) noexcept
{
    // Swap only when a retirement slot is free, so the outgoing model always
    // has somewhere safe to go; otherwise try again next block.
    int freeSlot = -1;

    for (int i = 0; i < (int) std::size (retired); ++i)
    {
        if (retired[(size_t) i].load (std::memory_order_relaxed) == nullptr)
        {
            freeSlot = i;
            break;
        }
    }

    if (freeSlot >= 0)
    {
        if (auto* p = pending.exchange (nullptr))
        {
            retired[(size_t) freeSlot].store (active, std::memory_order_release);
            active = p;
        }
        else if (clearRequest.exchange (false))
        {
            retired[(size_t) freeSlot].store (active, std::memory_order_release);
            active = nullptr;
        }
    }

    if (active == nullptr || numSamples > (int) scratch.size())
        return;

    float* in[1] { data };
    float* out[1] { scratch.data() };
    active->process (in, out, numSamples);

    const float g = normGain.load (std::memory_order_relaxed);

    for (int i = 0; i < numSamples; ++i)
        data[i] = scratch[(size_t) i] * g;
}

} // namespace rig
