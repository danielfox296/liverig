#pragma once

#include <memory>
#include <vector>

#include <juce_dsp/juce_dsp.h>

#include "Biquad.h"

namespace rig
{

// Holds the sound of one moment for as long as a foot stays down.
//
// The stage listens all the time and keeps the last quarter second. When Hold
// goes down it takes two spectra of what was just played, one hop apart: the
// later one is the sound, and how far each partial turned between the two is
// how fast it was running. Resynthesis keeps turning every partial at its
// own rate and overlap-adds the frames, so a held note carries on as the
// same note at the level it was caught — no loop, and so no seam to hear,
// however long the foot stays. The window is long (170 ms at 48 kHz) because a low
// B's harmonics sit 31 Hz apart and a shorter one cannot tell them apart.
//
// What is held is a chord of steady partials, not a recording: each peak of
// the spectrum is redrawn as the sinusoid it stands for (see holdRegion), so
// a press that lands on a note change or a pick attack still comes out level.
//
// The live signal is never delayed — it does not pass through here at all;
// the layer is written beside it for the chain to add. The capture itself
// waits one hop (43 ms) after the foot lands, so that a foot coming down
// with the note, rather than after it, catches the note and not the silence
// before it. The first frames are placed as the continuation of what was
// sounding, so the layer comes in under the still-ringing note in phase
// with it rather than combing against it.
//
// There are two layers, so a press while the last one is still fading takes
// the idle one and the old chord dies away underneath. A third press inside
// the same fade steals a layer, and the stolen one crossfades inside its own
// overlap-add instead of clicking.
//
// Motion is a slow two-tap chorus on the frozen layer alone: a still
// spectrum is an organ stop, and a little drift is what makes it a pad. Its
// lows are left alone, the way the chorus stage leaves them.
class SpectralFreeze
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;

    void setFadeTimes (float inMs, float outMs) noexcept;
    void setMotion (float amount) noexcept;

    // Hears `in` and writes the frozen layer to `layer` — silence unless
    // something is held or still fading. The two must not alias.
    void process (const float* in, float* layer, int numSamples, bool hold) noexcept;

    bool isSounding() const noexcept { return voices[0].active || voices[1].active; }

private:
    struct Voice
    {
        std::vector<float> spec, turn;   // interleaved complex, bins 0..N/2
        std::vector<float> ola;
        int read = 0, untilFrame = 0;
        float gain = 0.0f;
        bool held = false, active = false;
    };

    void capture (Voice& v) noexcept;
    void holdRegion (Voice& v, int from, int to, int peak, double floor) noexcept;
    void analyse (float* dest, int samplesAgo) noexcept;
    void turnSpectrum (Voice& v) noexcept;
    void addFrame (Voice& v, int skip) noexcept;
    void render (Voice& v, float* layer, int numSamples) noexcept;
    void addMotion (float* layer, int numSamples) noexcept;

    double sr = 48000.0;
    int size = 8192, hop = 2048, bins = 4097;

    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, synthWindow;
    std::vector<float> work, earlier, power, shapes;

    std::vector<float> history;
    int historyMask = 0, historyWrite = 0;

    Voice voices[2];
    bool wasHolding = false;
    int untilCapture = -1;
    float inStep = 1.0f, outStep = 1.0f;

    std::vector<float> motionBuf;
    int motionMask = 0, motionWrite = 0;
    double motionPhase[2] { 0.0, 0.25 };
    float motion = 0.0f, motionTarget = 0.0f;
    Biquad motionLows[2];
};

} // namespace rig
