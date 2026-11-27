#pragma once

#include <array>
#include <vector>

#include "TimbreProfiles.h"

namespace rig
{

// Re-voices a tracked note as another instrument's set of harmonics.
//
// The chain's pitch tracker says roughly where the note is. This stage then
// locks onto the note's own fundamental — it multiplies what it hears by a
// turning phasor at the tracked pitch and averages over exactly one period,
// which leaves the fundamental's phase and nothing else, because every other
// harmonic completes a whole number of turns in that time and cancels. Every
// partial it plays is then a multiple of that measured phase. Three things
// follow, and they are why it is built this way rather than as an oscillator
// reading the tracker's number:
//
//   - the new voice sits on the string's pitch exactly, through vibrato and
//     bends, however the tracker's estimate jitters between its looks;
//   - its fundamental is in phase with the string's, so blending it against
//     the dry bass adds the two instead of letting them drift in and out of
//     cancelling — the bottom never drops out at half blend;
//   - nothing is delayed. There is no window to wait for and so no dry path
//     to hold back to match it.
//
// The harmonics are replaced, not rescaled: at full blend the string's own
// second harmonic is gone and the profile's is there instead, whatever the
// string's was. Their levels come from the profile, shaped by its fixed
// resonances, and the whole set is played at the level of what came in,
// measured over the same one period — so the voice is as loud as the note,
// decays with it, and stops with it.
//
// What it cannot name it leaves alone. The voice sounds only while three
// things hold: the tracker is sure of a note, there is really a fundamental
// at that pitch to lock to, and the fundamental found is standing still
// against the tracker's phasor — if it is turning, the string has moved on
// to a note the tracker has not named yet. Otherwise the input goes by
// untouched. So the pluck of each note is the bass's own and the body of
// the note is the new instrument, and dead notes, slaps and chords pass as
// they are.
class TimbreVoice
{
public:
    static constexpr int maxPartials = 48;

    void prepare (double sampleRate);
    void reset() noexcept;

    void setProfile (int index) noexcept;

    // How long the voice takes to rise once it has the note, in ms. It
    // starts rising at the same moment whatever this is set to — a longer
    // bloom is a slower arrival, not a later one — and the bass underneath
    // gives way at the same rate, so each note opens out of its own pluck.
    void setBloom (float milliseconds) noexcept;

    // From the chain's tracker, once a block.
    void setPitch (float periodSamples, float confidence) noexcept;

    // In place. With `audible` false the stage only listens — the lock stays
    // warm for the moment the pedal is stepped on — and the data is left as
    // it came.
    void process (float* data, int numSamples, bool audible) noexcept;

    // How much of the output is the new voice right now, 0 to 1. The
    // offline harness reads it.
    float getVoicing() const noexcept { return gate; }

private:
    void setWindow (int length) noexcept;
    void relock() noexcept;
    void updatePartials() noexcept;
    float shapeAt (float log2Hz) const noexcept;

    double sr = 48000.0;

    const TimbreProfile* profile = &timbreProfiles[0];
    int profileIndex = -1;

    // The profile laid out for the engine: a level for every harmonic the
    // stage can play, and the fixed resonances as one curve over frequency.
    static constexpr int shapeSize = 128;       // twelfth-octave steps up from 16 Hz
    std::array<float, maxPartials> base {};
    std::array<float, shapeSize> shape {};
    std::array<float, TimbreProfile::maxFree> freeLog2 {};
    int numFree = 0;

    // The tracker's word.
    float period = 600.0f;
    bool tracked = false;

    // The phasor that turns at the tracked pitch, and its step.
    double oscRe = 1.0, oscIm = 0.0, rotRe = 1.0, rotIm = 0.0;

    // One period of history: the input, the input against the phasor, and
    // its square.
    std::vector<float> heard, demodRe, demodIm, squared;
    int mask = 0, write = 0, window = 600;
    double sumRe = 0.0, sumIm = 0.0, sumSq = 0.0;

    // The fundamental as found (half its amplitude, at its phase against the
    // phasor), the input's mean square, and the last direction worth trusting.
    float lockRe = 0.0f, lockIm = 0.0f, meanSquare = 0.0f, lockCoef = 0.01f;
    float dirRe = 1.0f, dirIm = 0.0f;

    // How fast the found fundamental is turning against the phasor, in
    // radians a sample: the gap between the string's pitch and the tracker's.
    float lastRe = 0.0f, lastIm = 0.0f, drift = 0.0f, stepAngle = 0.01f;

    // After a new note is named, the drift has to be watched for half a
    // period before its stillness means anything.
    int settling = 0;

    // Up in a straight line over the bloom; down quickly, because a voice
    // that has lost its note has to be out of the way of the next pluck.
    float gate = 0.0f, bloomMs = 0.0f, bloomStep = 0.01f, gateDown = 0.005f;

    // Partial levels move in straight lines between control ticks.
    int controlLength = 32, untilControl = 0, active = 0;
    float ampCoef = 0.1f;
    std::array<float, maxPartials> amp {}, ampStep {};

    int freeActive = 0;
    std::array<float, TimbreProfile::maxFree> freeAmp {}, freeStep {};
    std::array<double, TimbreProfile::maxFree> freeRe {}, freeIm {}, freeRotRe {}, freeRotIm {};
};

} // namespace rig
