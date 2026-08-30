#include "PresetStore.h"

namespace rig
{

// Bumped when the factory content changes. On load, an older file gets its
// factory presets refreshed by name and any new ones added — presets the
// user saved under their own names are never touched.
static constexpr int kFactoryVersion = 13;

// The order the factory chain presets were voiced in. Recalling one rewires
// the chain to match, the same as any preset saved in the app.
static const char* const kDefaultChainOrder
    = "comp,eq,filter,boost,drive,amp,octave,synth,phaser,chorus,delay,tape";

namespace
{

struct FactorySetting { int id; float value; };

struct FactoryEffect
{
    const char* effect;
    const char* name;
    std::vector<FactorySetting> settings;
};

struct FactoryChain
{
    const char* name;
    std::vector<FactorySetting> settings;
};

const std::vector<FactoryEffect>& factoryEffects()
{
    static const std::vector<FactoryEffect> list
    {
        { "midi", "Pad an octave up", { { pMidiTranspose, 12.0f }, { pMidiGate, -42.0f },
                                    { pMidiHold, 45.0f }, { pMidiRelease, 180.0f },
                                    { pMidiBendRange, 2.0f }, { pMidiExpression, 2.0f } } },
        { "midi", "Pad two up",   { { pMidiTranspose, 24.0f }, { pMidiGate, -42.0f },
                                    { pMidiHold, 45.0f }, { pMidiRelease, 180.0f },
                                    { pMidiBendRange, 2.0f }, { pMidiExpression, 2.0f } } },
        { "midi", "Unison",       { { pMidiTranspose, 0.0f }, { pMidiGate, -42.0f },
                                    { pMidiHold, 45.0f }, { pMidiRelease, 180.0f },
                                    { pMidiBendRange, 2.0f }, { pMidiExpression, 2.0f } } },
        { "midi", "Slow swell",   { { pMidiTranspose, 12.0f }, { pMidiGate, -46.0f },
                                    { pMidiHold, 90.0f }, { pMidiRelease, 500.0f },
                                    { pMidiBendRange, 2.0f }, { pMidiExpression, 2.0f } } },

        { "comp", "Gentle glue",  { { pCompThresh, -16.0f }, { pCompRatio, 2.0f }, { pCompAttack, 20.0f },
                                    { pCompRelease, 220.0f }, { pCompMakeup, 2.0f }, { pCompBlend, 1.0f } } },
        { "comp", "Fast FET",     { { pCompThresh, -22.0f }, { pCompRatio, 4.0f }, { pCompAttack, 0.8f },
                                    { pCompRelease, 120.0f }, { pCompMakeup, 5.0f }, { pCompBlend, 1.0f } } },
        { "comp", "Squash",       { { pCompThresh, -30.0f }, { pCompRatio, 12.0f }, { pCompAttack, 1.0f },
                                    { pCompRelease, 90.0f }, { pCompMakeup, 9.0f }, { pCompBlend, 1.0f } } },
        { "comp", "Parallel iron",{ { pCompThresh, -32.0f }, { pCompRatio, 10.0f }, { pCompAttack, 0.5f },
                                    { pCompRelease, 160.0f }, { pCompMakeup, 10.0f }, { pCompBlend, 0.45f } } },

        // Levelling-amp behaviour for an upright: lean on the note early and
        // gently, so the pluck rounds off and the tail rides up — sustain
        // without squash. The sidechain sits above the A string so a low
        // fundamental leans on the gain no harder than the note itself.
        { "comp", "Upright sustain", { { pCompThresh, -26.0f }, { pCompRatio, 2.5f }, { pCompAttack, 4.5f },
                                       { pCompRelease, 260.0f }, { pCompMakeup, 6.0f }, { pCompBlend, 1.0f },
                                       { pCompSidechain, 110.0f } } },

        { "eq", "Flat",           { { pEq1Freq, 80.0f }, { pEq1Gain, 0.0f }, { pEq2Gain, 0.0f },
                                    { pEq3Freq, 900.0f }, { pEq3Gain, 0.0f }, { pEq4Gain, 0.0f },
                                    { pEq5Freq, 8000.0f }, { pEq5Gain, 0.0f } } },
        { "eq", "Fat lows",       { { pEq1Freq, 80.0f }, { pEq1Gain, 4.5f }, { pEq2Gain, 0.0f },
                                    { pEq3Freq, 600.0f }, { pEq3Gain, -2.0f }, { pEq3Q, 0.8f },
                                    { pEq4Gain, 0.0f }, { pEq5Freq, 3000.0f }, { pEq5Gain, 1.0f } } },
        { "eq", "Cut mud",        { { pEq1Freq, 90.0f }, { pEq1Gain, 1.0f }, { pEq2Freq, 320.0f },
                                    { pEq2Gain, -5.0f }, { pEq2Q, 1.4f }, { pEq3Gain, 0.0f },
                                    { pEq4Gain, 0.0f }, { pEq5Freq, 2500.0f }, { pEq5Gain, 2.0f } } },
        { "eq", "Scooped",        { { pEq1Freq, 90.0f }, { pEq1Gain, 5.0f }, { pEq2Gain, 0.0f },
                                    { pEq3Freq, 800.0f }, { pEq3Gain, -7.0f }, { pEq3Q, 0.7f },
                                    { pEq4Gain, 0.0f }, { pEq5Freq, 3500.0f }, { pEq5Gain, 4.0f } } },

        // An upright's piezo, wing-mounted on the E side: the foot hears the
        // low strings bloom and the bridge talk, not the instrument. Trim the
        // rumble and boom the placement adds, pull the bridge honk and the
        // 3 kHz clack, soften the brittle top — what is left sounds like wood.
        { "eq", "Upright piezo (E side)",
                                  { { pEq1Freq, 50.0f },   { pEq1Gain, -3.0f }, { pEq1Q, 0.71f },
                                    { pEq2Freq, 130.0f },  { pEq2Gain, -2.5f }, { pEq2Q, 1.1f },
                                    { pEq3Freq, 550.0f },  { pEq3Gain, -2.0f }, { pEq3Q, 1.3f },
                                    { pEq4Freq, 2800.0f }, { pEq4Gain, -4.0f }, { pEq4Q, 1.6f },
                                    { pEq5Freq, 8000.0f }, { pEq5Gain, -3.0f }, { pEq5Q, 0.71f } } },

        { "filter", "Quack",        { { pFltSens, 0.55f }, { pFltAttack, 3.0f }, { pFltRelease, 160.0f },
                                      { pFltFrom, 120.0f }, { pFltTo, 2200.0f }, { pFltRes, 5.0f },
                                      { pFltMix, 1.0f } } },
        { "filter", "Deep funk",    { { pFltSens, 0.6f }, { pFltAttack, 5.0f }, { pFltRelease, 220.0f },
                                      { pFltFrom, 90.0f }, { pFltTo, 1200.0f }, { pFltRes, 6.5f },
                                      { pFltMix, 1.0f } } },
        { "filter", "Down sweep",   { { pFltSens, 0.5f }, { pFltAttack, 4.0f }, { pFltRelease, 260.0f },
                                      { pFltFrom, 900.0f }, { pFltTo, 150.0f }, { pFltRes, 4.0f },
                                      { pFltMix, 1.0f } } },
        { "filter", "Subtle touch", { { pFltSens, 0.45f }, { pFltAttack, 8.0f }, { pFltRelease, 300.0f },
                                      { pFltFrom, 150.0f }, { pFltTo, 900.0f }, { pFltRes, 1.8f },
                                      { pFltMix, 0.6f } } },

        { "boost", "Lift",       { { pBoostGain, 6.0f }, { pBoostSheen, 1.5f }, { pBoostHair, 0.1f } } },
        { "boost", "Solo push",  { { pBoostGain, 10.0f }, { pBoostSheen, 2.5f }, { pBoostHair, 0.2f } } },
        { "boost", "Slam",       { { pBoostGain, 18.0f }, { pBoostSheen, 1.0f }, { pBoostHair, 0.65f } } },

        { "drive", "Edge",          { { pDriveVoice, 1.0f }, { pDriveAmount, 3.0f }, { pDriveTone, 5000.0f },
                                      { pDriveLevel, -1.0f }, { pDriveStack, 0.0f }, { pDriveBite, 0.4f } } },
        { "drive", "Grit",          { { pDriveVoice, 1.0f }, { pDriveAmount, 12.0f }, { pDriveTone, 3200.0f },
                                      { pDriveLevel, -4.0f }, { pDriveStack, 0.2f }, { pDriveBite, 0.4f } } },
        { "drive", "Bass tight",    { { pDriveVoice, 1.0f }, { pDriveAmount, 18.0f }, { pDriveTone, 3000.0f },
                                      { pDriveLevel, -5.0f }, { pDriveStack, 0.15f }, { pDriveBite, 0.55f },
                                      { pDriveXover, 180.0f }, { pDriveBlend, 1.0f } } },
        { "drive", "Gold lift",     { { pDriveVoice, 2.0f }, { pDriveAmount, 4.0f }, { pDriveTone, 5500.0f },
                                      { pDriveLevel, 0.0f }, { pDriveStack, 0.0f }, { pDriveBite, 0.3f } } },
        { "drive", "Fusion rhythm", { { pDriveVoice, 2.0f }, { pDriveAmount, 7.0f }, { pDriveTone, 3800.0f },
                                      { pDriveLevel, -1.0f }, { pDriveStack, 0.35f }, { pDriveBite, 0.5f } } },
        { "drive", "Plexi crunch",  { { pDriveVoice, 3.0f }, { pDriveAmount, 14.0f }, { pDriveTone, 3400.0f },
                                      { pDriveLevel, -3.0f }, { pDriveStack, 0.45f }, { pDriveBite, 0.5f } } },
        { "drive", "Fusion lead",   { { pDriveVoice, 3.0f }, { pDriveAmount, 10.0f }, { pDriveTone, 3200.0f },
                                      { pDriveLevel, -4.0f }, { pDriveStack, 0.7f }, { pDriveBite, 0.65f } } },
        { "drive", "Wall",          { { pDriveVoice, 3.0f }, { pDriveAmount, 34.0f }, { pDriveTone, 2200.0f },
                                      { pDriveLevel, -8.0f }, { pDriveStack, 0.6f }, { pDriveBite, 0.4f } } },

        { "cab", "Sealed 8x10",   { { pCabBumpFreq, 100.0f }, { pCabBumpGain, 5.5f },
                                    { pCabTop, 3500.0f }, { pCabPresence, -2.5f } } },
        { "cab", "Ported 4x10",   { { pCabBumpFreq, 75.0f }, { pCabBumpGain, 4.0f },
                                    { pCabTop, 6000.0f }, { pCabPresence, 1.5f } } },
        { "cab", "15 flip-top",   { { pCabBumpFreq, 68.0f }, { pCabBumpGain, 7.0f },
                                    { pCabTop, 2600.0f }, { pCabPresence, -4.0f } } },
        { "cab", "Grind cab",     { { pCabBumpFreq, 110.0f }, { pCabBumpGain, 3.0f },
                                    { pCabTop, 5200.0f }, { pCabPresence, 4.0f } } },
        { "cab", "Barely there",  { { pCabBumpFreq, 90.0f }, { pCabBumpGain, 1.5f },
                                    { pCabTop, 7500.0f }, { pCabPresence, 0.0f } } },

        { "octave", "Classic +12",{ { pOctInterval, 12.0f }, { pOctMix, 0.5f }, { pOct2Level, 0.0f } } },
        { "octave", "Just a hint",{ { pOctInterval, 12.0f }, { pOctMix, 0.25f }, { pOct2Level, 0.0f } } },
        { "octave", "Full up",    { { pOctInterval, 12.0f }, { pOctMix, 1.0f }, { pOct2Level, 0.0f } } },
        { "octave", "Sub -12",    { { pOctInterval, -12.0f }, { pOctMix, 0.4f }, { pOct2Level, 0.0f } } },
        { "octave", "Fifth up",   { { pOctInterval, 7.0f }, { pOctMix, 0.4f }, { pOct2Level, 0.0f } } },
        { "octave", "Power stack",{ { pOctInterval, 12.0f }, { pOctMix, 0.45f },
                                    { pOct2Interval, 7.0f }, { pOct2Level, 0.6f } } },
        { "octave", "Organ",      { { pOctInterval, 12.0f }, { pOctMix, 0.65f },
                                    { pOct2Interval, -12.0f }, { pOct2Level, 0.7f } } },

        { "sub", "Divider classic", { { pSubLevel, 0.6f }, { pSubTone, 450.0f }, { pSubDivide, 0.0f } } },
        { "sub", "Just weight",     { { pSubLevel, 0.3f }, { pSubTone, 260.0f }, { pSubDivide, 0.0f } } },
        { "sub", "Square buzz",     { { pSubLevel, 0.75f }, { pSubTone, 1400.0f }, { pSubDivide, 0.0f } } },
        { "sub", "Two octaves down",{ { pSubLevel, 0.5f }, { pSubTone, 300.0f }, { pSubDivide, 1.0f } } },

        { "synth", "Square sub", { { pSynShape, 0.15f }, { pSynSub, 0.7f }, { pSynCutoff, 300.0f },
                                   { pSynEnv, 0.55f }, { pSynRes, 2.2f }, { pSynGlide, 12.0f },
                                   { pSynMix, 0.8f } } },
        { "synth", "Acid saw",   { { pSynShape, 1.0f }, { pSynSub, 0.25f }, { pSynCutoff, 180.0f },
                                   { pSynEnv, 0.85f }, { pSynRes, 7.0f }, { pSynGlide, 25.0f },
                                   { pSynMix, 1.0f } } },
        { "synth", "Fat layer",  { { pSynShape, 0.3f }, { pSynSub, 0.5f }, { pSynCutoff, 500.0f },
                                   { pSynEnv, 0.4f }, { pSynRes, 1.5f }, { pSynGlide, 10.0f },
                                   { pSynMix, 0.45f } } },

        { "phaser", "Slow sweep", { { pPhaserRate, 0.25f }, { pPhaserDepth, 0.7f }, { pPhaserFeedback, 0.35f },
                                    { pPhaserStages, 4.0f } } },
        { "phaser", "Funk",       { { pPhaserRate, 1.6f }, { pPhaserDepth, 0.85f }, { pPhaserFeedback, 0.55f },
                                    { pPhaserStages, 4.0f } } },
        { "phaser", "Deep",       { { pPhaserRate, 0.6f }, { pPhaserDepth, 1.0f }, { pPhaserFeedback, 0.8f },
                                    { pPhaserStages, 8.0f } } },

        { "chorus", "Silky",    { { pChoRate, 0.55f }, { pChoDepth, 0.35f }, { pChoVoices, 2.0f },
                                  { pChoTone, 7000.0f }, { pChoLows, 120.0f }, { pChoMix, 0.45f } } },
        { "chorus", "Tri lush", { { pChoRate, 0.7f }, { pChoDepth, 0.55f }, { pChoVoices, 3.0f },
                                  { pChoTone, 6000.0f }, { pChoLows, 140.0f }, { pChoMix, 0.6f } } },
        { "chorus", "Fretless", { { pChoRate, 0.8f }, { pChoDepth, 0.5f }, { pChoVoices, 2.0f },
                                  { pChoTone, 4500.0f }, { pChoLows, 100.0f }, { pChoMix, 0.55f } } },
        { "chorus", "Vibrato",  { { pChoRate, 2.8f }, { pChoDepth, 0.5f }, { pChoVoices, 1.0f },
                                  { pChoTone, 8000.0f }, { pChoLows, 40.0f }, { pChoMix, 1.0f } } },

        { "delay", "Slapback",    { { pDelayTime, 110.0f }, { pDelayFeedback, 0.15f }, { pDelayMix, 0.25f },
                                    { pDelayHighcut, 4500.0f }, { pDelayModRate, 0.9f },
                                    { pDelayModDepth, 0.0f }, { pDelaySat, 0.0f }, { pDelaySmear, 0.0f } } },
        { "delay", "Quarter-ish", { { pDelayTime, 420.0f }, { pDelayFeedback, 0.35f }, { pDelayMix, 0.3f },
                                    { pDelayHighcut, 3500.0f }, { pDelayModRate, 0.9f },
                                    { pDelayModDepth, 0.0f }, { pDelaySat, 0.0f }, { pDelaySmear, 0.0f } } },
        { "delay", "Long wash",   { { pDelayTime, 780.0f }, { pDelayFeedback, 0.6f }, { pDelayMix, 0.4f },
                                    { pDelayHighcut, 2200.0f }, { pDelayModRate, 0.9f },
                                    { pDelayModDepth, 0.0f }, { pDelaySat, 0.0f }, { pDelaySmear, 0.35f } } },
        { "delay", "Carbon copy", { { pDelayTime, 380.0f }, { pDelayFeedback, 0.5f }, { pDelayMix, 0.33f },
                                    { pDelayHighcut, 2400.0f }, { pDelayModRate, 0.9f },
                                    { pDelayModDepth, 0.4f }, { pDelaySat, 0.6f }, { pDelaySmear, 0.12f } } },
        { "delay", "Warble slap", { { pDelayTime, 120.0f }, { pDelayFeedback, 0.2f }, { pDelayMix, 0.3f },
                                    { pDelayHighcut, 3000.0f }, { pDelayModRate, 1.8f },
                                    { pDelayModDepth, 0.5f }, { pDelaySat, 0.4f }, { pDelaySmear, 0.0f } } },
        { "delay", "Tape echo",   { { pDelayTime, 340.0f }, { pDelayFeedback, 0.45f }, { pDelayMix, 0.32f },
                                    { pDelayHighcut, 2800.0f }, { pDelayModRate, 0.7f },
                                    { pDelayModDepth, 0.35f }, { pDelaySat, 0.5f }, { pDelaySmear, 0.25f } } },
        { "delay", "Diffuse wash",{ { pDelayTime, 520.0f }, { pDelayFeedback, 0.55f }, { pDelayMix, 0.38f },
                                    { pDelayHighcut, 2600.0f }, { pDelayModRate, 0.7f },
                                    { pDelayModDepth, 0.3f }, { pDelaySat, 0.3f }, { pDelaySmear, 0.8f } } },

        { "tape", "Gentle warmth", { { pTapeDrive, 4.0f }, { pTapeSoften, 0.35f }, { pTapeBump, 0.3f },
                                     { pTapeMix, 1.0f } } },
        { "tape", "Thick",         { { pTapeDrive, 9.0f }, { pTapeSoften, 0.5f }, { pTapeBump, 0.55f },
                                     { pTapeMix, 1.0f } } },
        { "tape", "Crush",         { { pTapeDrive, 16.0f }, { pTapeSoften, 0.65f }, { pTapeBump, 0.4f },
                                     { pTapeMix, 1.0f } } },
        { "tape", "Parallel glow", { { pTapeDrive, 12.0f }, { pTapeSoften, 0.45f }, { pTapeBump, 0.3f },
                                     { pTapeMix, 0.4f } } },

        { "reverb", "Room",       { { pRevSize, 0.3f }, { pRevDamp, 0.55f }, { pRevPre, 8.0f },
                                    { pRevMod, 0.35f }, { pRevMix, 0.18f } } },
        { "reverb", "Hall",       { { pRevSize, 0.68f }, { pRevDamp, 0.4f }, { pRevPre, 20.0f },
                                    { pRevMod, 0.5f }, { pRevMix, 0.3f } } },
        { "reverb", "Wash",       { { pRevSize, 0.92f }, { pRevDamp, 0.25f }, { pRevPre, 28.0f },
                                    { pRevMod, 0.65f }, { pRevMix, 0.45f } } },
    };

    return list;
}

const std::vector<FactoryChain>& factoryChains()
{
    static const std::vector<FactoryChain> list
    {
        { "Clean DI", {} },

        { "Fingerstyle comp", { { pCompOn, 1.0f }, { pCompThresh, -20.0f }, { pCompRatio, 3.0f },
                                { pCompAttack, 8.0f }, { pCompRelease, 140.0f }, { pCompMakeup, 4.0f },
                                { pEqOn, 1.0f }, { pEq1Gain, 2.0f }, { pEq3Freq, 500.0f },
                                { pEq3Gain, -2.0f }, { pEq5Gain, 1.5f } } },

        { "Octave lead", { { pCompOn, 1.0f }, { pCompThresh, -22.0f }, { pCompRatio, 4.0f },
                           { pCompAttack, 1.0f }, { pCompMakeup, 5.0f },
                           { pOctOn, 1.0f }, { pOctInterval, 12.0f }, { pOctMix, 0.55f },
                           { pEqOn, 1.0f }, { pEq5Gain, 2.5f } } },

        { "Dirty octave", { { pCompOn, 1.0f }, { pCompThresh, -20.0f }, { pCompRatio, 4.0f },
                            { pCompAttack, 2.0f }, { pCompMakeup, 3.0f },
                            { pDriveOn, 1.0f }, { pDriveVoice, 1.0f }, { pDriveAmount, 12.0f },
                            { pDriveTone, 3000.0f }, { pDriveLevel, -3.0f },
                            { pOctOn, 1.0f }, { pOctInterval, 12.0f }, { pOctMix, 0.5f } } },

        { "Phaser funk", { { pCompOn, 1.0f }, { pCompThresh, -18.0f }, { pCompRatio, 6.0f },
                           { pCompAttack, 1.0f }, { pCompMakeup, 4.0f },
                           { pPhaserOn, 1.0f }, { pPhaserRate, 1.2f }, { pPhaserDepth, 0.8f },
                           { pPhaserFeedback, 0.5f } } },

        { "Ambient swell", { { pOctOn, 1.0f }, { pOctInterval, 12.0f }, { pOctMix, 0.35f },
                             { pDelayOn, 1.0f }, { pDelayTime, 420.0f }, { pDelayFeedback, 0.45f },
                             { pDelayMix, 0.35f }, { pDelayHighcut, 3000.0f },
                             { pRevOn, 1.0f }, { pRevSize, 0.7f }, { pRevDamp, 0.4f },
                             { pRevPre, 20.0f }, { pRevMod, 0.55f }, { pRevMix, 0.35f } } },

        { "Funk filter", { { pCompOn, 1.0f }, { pCompThresh, -18.0f }, { pCompRatio, 6.0f },
                           { pCompAttack, 1.0f }, { pCompMakeup, 4.0f },
                           { pFltOn, 1.0f }, { pFltSens, 0.55f }, { pFltAttack, 3.0f },
                           { pFltRelease, 160.0f }, { pFltFrom, 120.0f }, { pFltTo, 2200.0f },
                           { pFltRes, 5.0f }, { pFltMix, 1.0f } } },

        { "Synth bass", { { pCompOn, 1.0f }, { pCompThresh, -20.0f }, { pCompRatio, 4.0f },
                          { pCompAttack, 2.0f }, { pCompMakeup, 3.0f },
                          { pSynOn, 1.0f }, { pSynShape, 0.15f }, { pSynSub, 0.7f },
                          { pSynCutoff, 300.0f }, { pSynEnv, 0.55f }, { pSynRes, 2.2f },
                          { pSynGlide, 12.0f }, { pSynMix, 0.85f } } },

        { "Stacked lead", { { pBoostOn, 1.0f }, { pBoostGain, 8.0f }, { pBoostSheen, 2.0f },
                            { pBoostHair, 0.2f },
                            { pDriveOn, 1.0f }, { pDriveVoice, 3.0f }, { pDriveAmount, 8.0f },
                            { pDriveTone, 3400.0f }, { pDriveLevel, -3.0f }, { pDriveStack, 0.6f },
                            { pDriveBite, 0.6f },
                            { pOctOn, 1.0f }, { pOctInterval, 12.0f }, { pOctMix, 0.45f },
                            { pDelayOn, 1.0f }, { pDelayTime, 140.0f }, { pDelayFeedback, 0.18f },
                            { pDelayMix, 0.22f }, { pDelayHighcut, 3600.0f } } },

        { "Warm DI", { { pCompOn, 1.0f }, { pCompThresh, -16.0f }, { pCompRatio, 2.0f },
                       { pCompAttack, 20.0f }, { pCompRelease, 220.0f }, { pCompMakeup, 2.0f },
                       { pTapeOn, 1.0f }, { pTapeDrive, 5.0f }, { pTapeSoften, 0.4f },
                       { pTapeBump, 0.35f }, { pTapeMix, 1.0f } } },

        { "Analog wash", { { pChoOn, 1.0f }, { pChoRate, 0.7f }, { pChoDepth, 0.55f },
                           { pChoVoices, 3.0f }, { pChoTone, 6000.0f }, { pChoLows, 140.0f },
                           { pChoMix, 0.55f },
                           { pDelayOn, 1.0f }, { pDelayTime, 380.0f }, { pDelayFeedback, 0.5f },
                           { pDelayMix, 0.33f }, { pDelayHighcut, 2400.0f },
                           { pDelayModRate, 0.9f }, { pDelayModDepth, 0.4f }, { pDelaySat, 0.6f },
                           { pRevOn, 1.0f }, { pRevSize, 0.68f }, { pRevDamp, 0.4f },
                           { pRevPre, 20.0f }, { pRevMod, 0.5f }, { pRevMix, 0.3f } } },

    };

    return list;
}

juce::var settingsToVar (const std::vector<FactorySetting>& settings)
{
    auto* obj = new juce::DynamicObject();

    for (const auto& setting : settings)
        obj->setProperty (juce::Identifier (spec (setting.id).id), setting.value);

    return juce::var (obj);
}

} // namespace

juce::File PresetStore::getFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
              .getChildFile ("Application Support")
              .getChildFile ("LiveRig")
              .getChildFile ("presets.json");
}

static juce::var capture (const ParamStore& params)
{
    auto* obj = new juce::DynamicObject();

    for (int i = pInGain; i < NumParams; ++i)
        obj->setProperty (juce::Identifier (spec (i).id), params.get (i));

    return juce::var (obj);
}

void PresetStore::load()
{
    names.clear();
    values.clear();
    orders.clear();
    ampCaptures.clear();
    effectPresets.clear();

    int storedVersion = 0;
    const auto file = storageFile();

    if (file.existsAsFile())
    {
        const auto parsed = juce::JSON::parse (file.loadFileAsString());

        if (auto* root = parsed.getDynamicObject())
        {
            storedVersion = (int) root->getProperty ("factoryVersion");

            const auto list = root->getProperty ("presets");

            if (auto* array = list.getArray())
            {
                for (const auto& entry : *array)
                {
                    if (auto* obj = entry.getDynamicObject())
                    {
                        const auto name = obj->getProperty ("name").toString();

                        if (name.isNotEmpty() && ! names.contains (name))
                        {
                            names.add (name);
                            values.add (obj->getProperty ("params"));
                            orders.add (obj->getProperty ("order").toString());
                            ampCaptures.add (obj->getProperty ("ampCapture"));   // void when absent
                        }
                    }
                }
            }

            const auto effects = root->getProperty ("effects");

            if (auto* array = effects.getArray())
            {
                for (const auto& entry : *array)
                {
                    if (auto* obj = entry.getDynamicObject())
                    {
                        EffectPreset preset;
                        preset.effect = obj->getProperty ("effect").toString();
                        preset.name = obj->getProperty ("name").toString();
                        preset.params = obj->getProperty ("params");
                        preset.capture = obj->getProperty ("capture");   // void when absent

                        if (preset.effect.isNotEmpty() && preset.name.isNotEmpty()
                            && indexOfEffectPreset (preset.effect, preset.name) < 0)
                            effectPresets.add (preset);
                    }
                }
            }
        }
    }

    bool changed = false;

    if (names.isEmpty())
    {
        addFactoryPresets();
        changed = true;
    }

    if (effectPresets.isEmpty())
    {
        addFactoryEffectPresets();
        changed = true;
    }

    if (storedVersion < kFactoryVersion)
    {
        mergeFactoryContent();
        changed = true;
    }

    if (changed)
        save();
}

bool PresetStore::apply (const juce::String& name, ParamStore& params,
                         const juce::Array<int>& onlyIds, int slot) const
{
    const int index = names.indexOf (name);

    if (index < 0)
        return false;

    auto* obj = values[index].getDynamicObject();

    if (obj == nullptr)
        return false;

    for (int i : onlyIds)
    {
        const auto& s = spec (i);
        const juce::Identifier key (s.id);

        if (obj->hasProperty (key))
            params.set (slot, i, juce::jlimit (s.minValue, s.maxValue, (float) (double) obj->getProperty (key)));
    }

    return true;
}

void PresetStore::store (const juce::String& name, const ParamStore& params,
                         const juce::String& chainOrder, const juce::var& ampCapture,
                         const juce::Array<int>& onlyIds, int slot)
{
    if (name.isEmpty())
        return;

    auto* obj = new juce::DynamicObject();

    for (int i : onlyIds)
        obj->setProperty (juce::Identifier (spec (i).id), (double) params.get (slot, i));

    const juce::var subset (obj);
    const int index = names.indexOf (name);

    if (index >= 0)
    {
        values.set (index, subset);
        orders.set (index, chainOrder);
        ampCaptures.set (index, ampCapture);
    }
    else
    {
        names.add (name);
        values.add (subset);
        orders.add (chainOrder);
        ampCaptures.add (ampCapture);
    }

    save();
}

bool PresetStore::apply (const juce::String& name, ParamStore& params) const
{
    const int index = names.indexOf (name);

    if (index < 0)
        return false;

    auto* obj = values[index].getDynamicObject();

    if (obj == nullptr)
        return false;

    for (int i = pInGain; i < NumParams; ++i)
    {
        const auto& s = spec (i);
        const juce::Identifier key (s.id);

        if (obj->hasProperty (key))
            params.set (i, juce::jlimit (s.minValue, s.maxValue, (float) (double) obj->getProperty (key)));
    }

    return true;
}

void PresetStore::store (const juce::String& name, const ParamStore& params,
                         const juce::String& chainOrder, const juce::var& ampCapture)
{
    if (name.isEmpty())
        return;

    const int index = names.indexOf (name);

    if (index >= 0)
    {
        values.set (index, capture (params));
        orders.set (index, chainOrder);
        ampCaptures.set (index, ampCapture);
    }
    else
    {
        names.add (name);
        values.add (capture (params));
        orders.add (chainOrder);
        ampCaptures.add (ampCapture);
    }

    save();
}

void PresetStore::remove (const juce::String& name)
{
    const int index = names.indexOf (name);

    if (index < 0)
        return;

    names.remove (index);
    values.remove (index);
    orders.remove (index);
    ampCaptures.remove (index);
    save();
}

juce::String PresetStore::getChainOrder (const juce::String& name) const
{
    const int index = names.indexOf (name);
    return index >= 0 ? orders[index] : juce::String();
}

juce::var PresetStore::getAmpCapture (const juce::String& name) const
{
    const int index = names.indexOf (name);
    return index >= 0 ? ampCaptures[index] : juce::var();
}

void PresetStore::save() const
{
    juce::Array<juce::var> array;

    for (int i = 0; i < names.size(); ++i)
    {
        auto* entry = new juce::DynamicObject();
        entry->setProperty ("name", names[i]);
        entry->setProperty ("params", values[i]);
        entry->setProperty ("order", orders[i]);

        if (! ampCaptures[i].isVoid())
            entry->setProperty ("ampCapture", ampCaptures[i]);

        array.add (juce::var (entry));
    }

    juce::Array<juce::var> effectArray;

    for (const auto& preset : effectPresets)
    {
        auto* entry = new juce::DynamicObject();
        entry->setProperty ("effect", preset.effect);
        entry->setProperty ("name", preset.name);
        entry->setProperty ("params", preset.params);

        if (! preset.capture.isVoid())
            entry->setProperty ("capture", preset.capture);

        effectArray.add (juce::var (entry));
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("factoryVersion", kFactoryVersion);
    root->setProperty ("presets", array);
    root->setProperty ("effects", effectArray);

    const auto file = storageFile();
    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::JSON::toString (juce::var (root), false));
}

int PresetStore::indexOfEffectPreset (const juce::String& effect, const juce::String& name) const
{
    for (int i = 0; i < effectPresets.size(); ++i)
        if (effectPresets[i].effect == effect && effectPresets[i].name == name)
            return i;

    return -1;
}

juce::StringArray PresetStore::getEffectNames (const juce::String& effect) const
{
    juce::StringArray result;

    for (const auto& preset : effectPresets)
        if (preset.effect == effect)
            result.add (preset.name);

    return result;
}

bool PresetStore::effectContains (const juce::String& effect, const juce::String& name) const
{
    return indexOfEffectPreset (effect, name) >= 0;
}

bool PresetStore::applyEffect (const juce::String& effect, const juce::String& name,
                               ParamStore& params, juce::var* captureOut, int slot) const
{
    const int index = indexOfEffectPreset (effect, name);

    if (index < 0)
        return false;

    auto* obj = effectPresets[index].params.getDynamicObject();

    if (obj == nullptr)
        return false;

    if (captureOut != nullptr)
        *captureOut = effectPresets[index].capture;

    for (const auto& property : obj->getProperties())
    {
        const int id = paramIndexForId (property.name.toString());

        if (id < 0)
            continue;

        const auto& s = spec (id);
        params.set (slot, id, juce::jlimit (s.minValue, s.maxValue, (float) (double) property.value));
    }

    return true;
}

void PresetStore::storeEffect (const juce::String& effect, const juce::String& name,
                               const ParamStore& params, const std::vector<int>& paramIds,
                               const juce::var& capture, int slot)
{
    if (effect.isEmpty() || name.isEmpty())
        return;

    auto* obj = new juce::DynamicObject();

    for (int id : paramIds)
        obj->setProperty (juce::Identifier (spec (id).id), params.get (slot, id));

    const int index = indexOfEffectPreset (effect, name);

    if (index >= 0)
    {
        effectPresets.getReference (index).params = juce::var (obj);
        effectPresets.getReference (index).capture = capture;
    }
    else
    {
        effectPresets.add ({ effect, name, juce::var (obj), capture });
    }

    save();
}

void PresetStore::removeEffect (const juce::String& effect, const juce::String& name)
{
    const int index = indexOfEffectPreset (effect, name);

    if (index < 0)
        return;

    effectPresets.remove (index);
    save();
}

void PresetStore::addFactoryEffectPresets()
{
    for (const auto& factory : factoryEffects())
        effectPresets.add ({ factory.effect, factory.name, settingsToVar (factory.settings), juce::var() });
}

void PresetStore::addFactoryPresets()
{
    for (const auto& factory : factoryChains())
    {
        ParamStore params;

        for (const auto& setting : factory.settings)
            params.set (setting.id, setting.value);

        names.add (factory.name);
        values.add (capture (params));
        orders.add (kDefaultChainOrder);
        ampCaptures.add (juce::var());   // factory sounds never claim a capture
    }
}

// The one-time catch-up for a presets file written by an older build: factory
// presets are refreshed by name so they carry values — and now the chain
// order — that did not exist when they were saved. Presets the user saved
// under their own names are left exactly as they were.
void PresetStore::mergeFactoryContent()
{
    // Factory content that shipped briefly and was withdrawn is swept out by
    // name, so a file that merged it once does not carry ghosts. Presets the
    // user saved under their own names never collide with these.
    static const char* const retiredChains[] = { "Singing bass", "Ventriloquist" };

    for (auto* retired : retiredChains)
    {
        const int index = names.indexOf (retired);

        if (index >= 0)
        {
            names.remove (index);
            values.remove (index);
            orders.remove (index);
            ampCaptures.remove (index);
        }
    }

    for (int i = effectPresets.size(); --i >= 0;)
        if (effectPresets.getReference (i).effect == "vocal")
            effectPresets.remove (i);

    for (const auto& factory : factoryEffects())
    {
        const auto value = settingsToVar (factory.settings);
        const int index = indexOfEffectPreset (factory.effect, factory.name);

        if (index >= 0)
        {
            effectPresets.getReference (index).params = value;
            effectPresets.getReference (index).capture = juce::var();
        }
        else
        {
            effectPresets.add ({ factory.effect, factory.name, value, juce::var() });
        }
    }

    for (const auto& factory : factoryChains())
    {
        ParamStore params;

        for (const auto& setting : factory.settings)
            params.set (setting.id, setting.value);

        const int index = names.indexOf (factory.name);

        if (index >= 0)
        {
            values.set (index, capture (params));
            orders.set (index, kDefaultChainOrder);
            ampCaptures.set (index, juce::var());
        }
        else
        {
            names.add (factory.name);
            values.add (capture (params));
            orders.add (kDefaultChainOrder);
            ampCaptures.add (juce::var());
        }
    }
}

} // namespace rig
