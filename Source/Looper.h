#pragma once

#include <atomic>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

namespace rig
{

// The instrument looper. Tempo and length are tapped in before a note is
// played: one switch taps the tempo, another taps how many bars, and Record
// gives four clicks and then takes exactly that many bars of one channel —
// the click keeps going under the take and stops when the loop starts to
// play. A second press of Record during the take ends it early, on the
// nearest bar line. After that Record layers on top, Undo drops the newest
// layer (and brings it back), Play / pause holds the loop where it is and
// carries on from there, and Clear wipes it.
//
// What is recorded is what the channel sounded like after its pedals, and
// it is placed where the player heard it: the engine says how late a played
// note arrives (the interface's round trip plus the board), and every
// sample is written that much earlier in the loop.
//
// The first take lives in one buffer; each layer is recorded into one of
// two more, and the layer before it is folded into the first as the new one
// starts — a few thousand samples per block, never one long stall — so the
// newest layer can always be taken back. The longest loop takes about a
// second to fold; a layer started inside that second waits for it.
class Looper
{
public:
    enum class State { empty, countIn, recording, playing, overdubbing, paused };

    static constexpr int beatsPerBar = 4, countInBeats = 4, maxBars = 16;
    static constexpr float minBpm = 40.0f, maxBpm = 240.0f;
    static constexpr double maxLoopSeconds = 64.0;   // sixteen bars at 60

    Looper();

    void prepare (double sampleRate, int maxBlockSize);

    // --- message thread ---

    // Taps count only while there is no loop; a loop keeps the tempo and
    // length it was recorded at. Two taps make a tempo; a tap after a long
    // gap starts a new count. Each bars tap adds a bar, and the tap past
    // the most the loop can hold comes back round to one.
    void tapTempo (juce::uint32 nowMs);
    void tapBars (juce::uint32 nowMs);

    // Record with no loop counts in and takes the first pass of this
    // channel slot; pressed again during the take it ends the take on the
    // nearest bar line (during the count, it calls the take off). With a
    // loop it starts and ends a layer.
    void pressRecord (int sourceSlot);
    void pressPlayPause();
    void pressUndo();
    void pressClear();

    State getState() const noexcept      { return (State) uiState.load (std::memory_order_relaxed); }
    float getBpm() const noexcept        { return bpm.load (std::memory_order_relaxed); }
    int getBars() const noexcept         { return bars.load (std::memory_order_relaxed); }
    int getSource() const noexcept       { return uiSource.load (std::memory_order_relaxed); }
    bool hasLayer() const noexcept       { return uiHasLayer.load (std::memory_order_relaxed); }
    bool isLayerUndone() const noexcept  { return uiUndone.load (std::memory_order_relaxed); }

    // Where things stand in beats: through the count-in while counting,
    // through the loop otherwise.
    float getBeatPosition() const noexcept
    {
        const float beat = uiBeatLen.load (std::memory_order_relaxed);
        return beat > 0.0f ? (float) uiPos.load (std::memory_order_relaxed) / beat : 0.0f;
    }

    static int maxBarsAt (float beatsPerMinute) noexcept;

    // --- audio thread ---

    // Carries out whatever was pressed since the last block and says which
    // channel slot the looper wants to hear this block, or -1 for none.
    int beginBlock() noexcept;

    // The wanted channel's sound in, the loop and the click added to the
    // sum. latencySamples is how late a played note arrives here; level is
    // the loop's own gain, and the click does not ride it.
    void process (const float* srcL, const float* srcR, float* outL, float* outR, int numSamples,
                  int latencySamples, float level) noexcept;

private:
    enum Command { cmdRecord, cmdPlayPause, cmdUndo, cmdClear };
    enum class Job { none, fold, wipe };

    void push (Command type, int arg) noexcept;
    bool ensureBuffers();

    void handle (Command type, int arg) noexcept;
    void toEmpty() noexcept;
    void closeTake() noexcept;
    void tryStartLayer() noexcept;
    void endLayer (bool keepTail) noexcept;
    void runJobs (int budget) noexcept;
    void publish() noexcept;

    // The settings the taps make. Written on the message thread, latched by
    // the audio thread when a take begins.
    std::atomic<float> bpm { 120.0f };
    std::atomic<int> bars { 4 };

    juce::uint32 tempoTaps[5] {};
    int numTempoTaps = 0;

    static constexpr int queueSize = 32;
    juce::AbstractFifo queue { queueSize };
    int queuedType[queueSize] {}, queuedArg[queueSize] {};

    // The loop's memory arrives the first time the looper is touched, from
    // the message thread; until then the audio thread leaves it alone.
    juce::CriticalSection allocLock;   // message thread and prepare only
    std::atomic<bool> allocated { false };
    std::atomic<double> rate { 0.0 };
    int capacity = 0;

    juce::AudioBuffer<float> base, layers[2];
    std::vector<float> preRoll[2];
    int fadeLen = 0;

    // --- audio thread from here down ---
    State state = State::empty;
    int loopLen = 0, countLen = 0, totalBeats = 0;
    double beatLen = 0.0;
    juce::int64 clock = 0;       // samples since the count-in began
    int nextBeat = 0;
    bool capturing = false;      // the first take is still landing
    bool latchLatency = false;
    int latency = 0, source = -1;

    int playPos = 0, pausedAt = 0;
    juce::SmoothedValue<float> playGain, levelGain, layerGain[2], dubGain;

    int newest = -1;             // the newest layer's buffer; -1 when the loop is its first take alone
    bool undone = false;
    bool dubbing = false, wantLayer = false;
    int dubTail = 0;

    Job job[2] { Job::none, Job::none };
    int jobPos[2] {}, jobLen[2] {};

    int clickAge = -1, clickLen = 0;
    double clickPhase = 0.0, clickStep = 0.0;
    float clickEnv = 0.0f, clickDecay = 0.0f;

    std::atomic<int> uiState { 0 }, uiPos { 0 }, uiSource { -1 };
    std::atomic<float> uiBeatLen { 0.0f };
    std::atomic<bool> uiHasLayer { false }, uiUndone { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Looper)
};

} // namespace rig
