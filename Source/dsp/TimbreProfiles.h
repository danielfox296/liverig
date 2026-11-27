#pragma once

#include <array>
#include <iterator>

namespace rig
{

// What the Timbre pedal turns a note into: one entry per instrument.
//
// These are experiments voiced by ear-shaped guesswork, not measurements of
// the instruments they are named for. A new one is a new entry here and
// nothing else — the pedal's menu, the saved setting's range and the engine
// all read this table. Add at the end: a saved board remembers its
// instrument by position.
struct TimbreProfile
{
    static constexpr int numHarmonics = 16;
    static constexpr int maxFree = 8;

    const char* name;

    // How strong each of the first sixteen harmonics is against the others.
    // Only the proportions matter: the pedal plays the whole set at the
    // level of what was played.
    float harmonic[numHarmonics];

    // How the series carries on above the sixteenth, in dB per octave from
    // the sixteenth's level. Zero means it stops there.
    float tailDbPerOctave;

    // The part of an instrument that stays put while the note moves: up to
    // two resonances at fixed frequencies. This is what keeps a brass voice
    // bright on a low E, where a table of harmonics alone would leave it
    // dull. A gain of zero is no resonance.
    struct Body { float hz, db, octaves; };
    Body body[2];

    // Everything above this falls away at 12 dB per octave.
    float topHz;

    // Partials that are not whole multiples of the note — the thing a bell
    // is made of and a harmonic table cannot say. Ratio to the played note,
    // and level on the same scale as the harmonics. A ratio of zero ends
    // the list.
    struct Free { float ratio, level; };
    Free free[maxFree];
};

inline constexpr TimbreProfile timbreProfiles[]
{
    // A bowed string: every harmonic present and falling gently, the wood's
    // low resonance and the bridge's high one over the top.
    { "Cello",
      { 1.00f, 0.80f, 0.62f, 0.50f, 0.42f, 0.35f, 0.30f, 0.26f,
        0.22f, 0.19f, 0.17f, 0.15f, 0.13f, 0.12f, 0.11f, 0.10f },
      -9.0f,
      { { 300.0f, 5.0f, 0.8f }, { 2200.0f, 4.0f, 1.0f } },
      5000.0f,
      {} },

    // A stopped pipe: the odd harmonics, with the even ones nearly gone.
    // Hollow and woody.
    { "Clarinet",
      { 1.00f, 0.04f, 0.75f, 0.06f, 0.50f, 0.08f, 0.32f, 0.10f,
        0.22f, 0.10f, 0.15f, 0.09f, 0.11f, 0.07f, 0.08f, 0.06f },
      -12.0f,
      { { 1500.0f, 3.0f, 1.0f }, { 0.0f, 0.0f, 1.0f } },
      4000.0f,
      {} },

    // Brass: the series barely falls at all, and a broad resonance near
    // 1.2 kHz does the shouting whatever the note.
    { "Trumpet",
      { 1.00f, 1.00f, 0.95f, 0.90f, 0.85f, 0.80f, 0.75f, 0.70f,
        0.65f, 0.60f, 0.55f, 0.50f, 0.46f, 0.42f, 0.38f, 0.35f },
      -6.0f,
      { { 1200.0f, 8.0f, 1.2f }, { 0.0f, 0.0f, 1.0f } },
      6000.0f,
      {} },

    // Close to a pure tone: a little octave, a breath of twelfth, nothing
    // above.
    { "Flute",
      { 1.00f, 0.35f, 0.12f, 0.06f, 0.03f, 0.015f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
      0.0f,
      { { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f } },
      3000.0f,
      {} },

    // Drawbars: the footages an organ has and nothing between them — the
    // unison, octave, twelfth, fifteenth, and the ranks above. No seventh.
    { "Organ",
      { 1.00f, 0.80f, 0.60f, 0.50f, 0.25f, 0.35f, 0.0f, 0.40f,
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
      0.0f,
      { { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f } },
      8000.0f,
      {} },

    // A bell whose hum is the played note: the strike an octave up, and
    // above it the minor third and the off-pitch partials that make it
    // metal instead of an organ.
    { "Bell",
      { 1.00f, 0.80f, 0.0f, 0.90f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
      0.0f,
      { { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f } },
      9000.0f,
      { { 2.40f, 0.60f }, { 3.01f, 0.35f }, { 5.03f, 0.30f }, { 6.02f, 0.35f },
        { 8.33f, 0.25f }, { 10.87f, 0.15f }, { 13.60f, 0.10f } } },
};

inline constexpr int numTimbreProfiles = (int) std::size (timbreProfiles);

// The names alone, for the pedal's menu.
inline constexpr auto timbreProfileNames = []
{
    std::array<const char*, (size_t) numTimbreProfiles> names {};

    for (size_t i = 0; i < names.size(); ++i)
        names[i] = timbreProfiles[i].name;

    return names;
}();

} // namespace rig
