# LiveRig

A macOS app for live performance: it plays backing-track stems while routing
your bass or guitar through a real-time effect chain, and sums both to one
stereo output.

Two independent lanes, one master bus:

```
Song folder (stems) ─> Stem player (4 tracks, gain/mute) ─┐
                                                          ├─> Master ─> Output
Live input (ch 1) ──> FX chain ──> live lane gain ────────┘
```

---

## Using it

**Double-click LiveRig on the Desktop.** It also lives in /Applications, so
Spotlight and Launchpad find it. Grant the microphone prompt the first time or
the live lane stays silent — macOS treats an audio interface input as a
microphone.

Everything happens in the window. There is nothing to run in a terminal.

- **SONGS** lists every song you have split. Tap one to load it. It refreshes
  itself whenever a new split finishes.
- **New song...** takes any audio file — a Suno download, an MP3, a WAV — and
  splits it into four stems. **It runs in the background**: the dialog closes as
  soon as you hit Split, a progress strip appears under the top bar, and the
  rest of the app stays live. Keep playing over whatever is already loaded; the
  new song drops into the list when it is done. The first time, it offers to
  install the splitter (about 900 MB, once).
- **Load folder...** opens a song folder from anywhere else on disk.
- **Spacebar** or the transport button starts and stops. Stop returns to the top.
- **STEMS**: the four stems as a mixer — fader and mute each. Missing stems are
  greyed out. You will normally pull `bass` down or mute it.
- **Effects** are collapsed to a single row each: name, and an ON toggle you can
  hit without looking. Tap the row or the **+** to open its controls, tap again
  to put it away. Which ones are open is remembered between sessions.
- **Presets** come at two scales. The bar across the top of the effect column
  stores the **whole chain** — everything from input trim to live out, but not
  the stem faders, which belong to the song rather than the sound. Six ship
  with it: Clean DI, Fingerstyle comp, Octave lead, Dirty octave, Phaser funk,
  Ambient swell.

  Each effect also has **its own preset row** when you open it, for building a
  sound rather than recalling one — Fast FET on the compressor, Cut mud on the
  EQ, Sub -12 on the octave, Slapback on the delay, and so on. Type a name and
  hit **Save** on either bar to add your own.

  An effect preset deliberately does *not* carry the ON state. Recalling how
  something sounds should not decide whether it is engaged — that is what the
  ON button is for, mid-song. A whole-chain preset does carry it, because there
  the point is to recall an entire sound.
- **Footer**: the audio device picker, the current rate and buffer size, and the
  chain's reported latency.

The controls are horizontal faders rather than rotary knobs on purpose. A knob
needs a slow, accurate circular drag; a fader is a wide target you can shove
with one hand while the other keeps playing, and tapping anywhere on the track
jumps straight there. Everything double-clicks back to its default.

All parameters, the last song, the audio device choice and which effects are
open are saved to `~/Library/Application Support/LiveRig/settings.json` and
restored on launch. Both kinds of preset live beside it in `presets.json`.

Songs live in `/Users/fox296/LiveRig/Songs/<song name>/`, four WAVs named
`drums`, `bass`, `other`, `vocals`. Anything with that shape loads, so you can
drop stems in by hand if you have them from somewhere else.

### Signal chain

| # | Effect | Controls |
|---|--------|----------|
| 1 | Input gain | trim, meter (a 5 Hz DC blocker sits here too) |
| 2 | Compressor | threshold, ratio, attack, release, makeup |
| 3 | EQ | low shelf, parametric mid (freq/gain/Q), high shelf |
| 4 | Overdrive | drive, tone, level — `tanh` soft clip, 4x oversampled |
| 5 | Octave | interval (±12 semitones), dry/wet |
| 6 | Phaser | rate, depth, feedback — 4-stage |
| 7 | Delay | time, feedback, mix, high cut in the feedback path |
| 8 | Reverb | size, damping, mix — Freeverb, mono in / stereo out |
| 9 | Live out | lane gain |

Attack and release are exposed on the compressor because "fast attack
available" is the whole point of a FET-style feel on bass.

---

## How it works

### Real-time discipline

The callback allocates nothing, locks nothing, opens no files and logs nothing.
Parameters move through a flat table of `std::atomic<float>`; gains are
per-sample smoothed, filter coefficients recomputed only when a value actually
changes. Stems are decoded and resampled to the device rate on load and live
entirely in RAM; the audio thread only ever try-locks around the song pointer,
and a failed try-lock costs one block of playback, never a stall.

**Every effect runs on every block, bypassed or not.** Bypass is a 15 ms
crossfade between an effect's dry input and its output. That costs a little
idle CPU and buys two things worth more on stage: click-free toggling with live
delay and reverb tails, and a constant per-block cost, so nothing spikes when
you stamp on something at 64 samples.

Splitting a song runs on a worker thread driving a child process, so a
two-minute separation never touches the audio thread — and never blocks the
UI either. The separator is niced and told to leave two cores alone, so the
device keeps its headroom while a track is being pulled apart.

### The octave shifter

This is the part worth reading. It is a pitch-synchronous delay-line shifter,
not a generic granular one.

A pitch tracker runs as a side chain on the compressor output — clean, before
the overdrive adds harmonics that fool detectors. It lowpasses and decimates to
about 3 kHz, runs YIN (squared difference function, cumulative mean
normalisation, first dip below threshold so it does not fall for the
sub-octave), then refines the winning lag at full rate with a normalised
cross-correlation. That second stage is what gets the period accurate to a
fraction of a sample.

The shifter itself keeps two read taps moving through a circular buffer at the
shift ratio. When a tap runs out of headroom it is relocated by *exactly one
detected period* and crossfaded in over a quarter of a period, linearly.
Because the two taps are one period apart, the crossfade is between
waveform-aligned copies of the same signal — no comb, no warble, no grain
flutter, which is what usually makes shifters unusable on a low B or E. When
the tracker is unsure it holds the last period, so it degrades to an ordinary
granular shifter rather than dropping out.

The tracker runs from 26 Hz to 500 Hz, so a 5-string low B and the drop tunings
under it are inside the range rather than pinned against its floor.

Latency is roughly half a period plus the crossfade, so it scales with the note
rather than being a fixed worst case: 20 ms on a low B, 15 ms on a low E,
5–9 ms an octave up. The footer reports the live figure, smoothed, so it reads
as the number you feel rather than wherever in the grain cycle the UI sampled.

The backing lane is deliberately **not** compensated against it.

---

## Rebuilding from source

Requirements: Apple Silicon Mac, CMake 3.22+, Command Line Tools, JUCE 8.

```bash
brew install cmake ninja
git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git JUCE
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
cmake --build build --target install-app
```

`install-app` puts the bundle in /Applications, ad-hoc signs it so macOS keeps
a stable identity for the microphone permission, and refreshes the Desktop
icon. If `JUCE/` is missing, CMake fetches it instead.

The icon is generated, not drawn by hand: `python3 scripts/make-icon.py`
rewrites `Resources/icon.png` (pure stdlib, no Pillow).

### A note about this machine's toolchain

`/Library/Developer/CommandLineTools/usr/include/c++/v1` here is a leftover
from an older toolchain: it holds four headers and shadows the SDK's complete
libc++, so plain `c++ -c anything.cpp` fails with `'algorithm' file not found`.
The build detects that and compiles against the SDK's libc++ instead, so it is
already handled — but it will bite any other C++ project on this Mac. The
permanent fix, when convenient:

```bash
sudo rm -rf /Library/Developer/CommandLineTools && sudo xcode-select --install
```

The CMake workaround becomes a no-op once that directory is gone.

---

## Under the hood: the splitter

The app installs Demucs into `/Users/fox296/LiveRig/venv` and calls it there,
so nothing is added to your system Python. If you would rather do it by hand:

```bash
python3 -m venv /Users/fox296/LiveRig/venv
/Users/fox296/LiveRig/venv/bin/pip install --upgrade pip
/Users/fox296/LiveRig/venv/bin/pip install demucs numpy
```

`numpy` is listed explicitly because demucs 4.1.0 imports it but forgot to
declare it — without it every run dies with `ModuleNotFoundError: No module
named 'numpy'`. The first separation downloads the htdemucs model (about
300 MB) into `~/.cache/torch`.

`scripts/make-song.sh` is the same pipeline for the terminal, if you ever want
it in a shell script:

```bash
./scripts/make-song.sh ~/Downloads/suno-take-3.mp3 "Fly Me To The Moon"
```

It prints the folder path on stdout, progress on stderr. A 2:34 Suno track took
67 seconds on CPU here and produced four 44.1 kHz 24-bit stereo stems.

---

## Tests

`rig_tests` validates the parts you cannot check by looking at the app:

```bash
./build/rig_tests_artefacts/RelWithDebInfo/rig_tests
```

It covers pitch tracking accuracy across the bass register and a 40 → 120 Hz
glide, the shifter's actual output pitch at +12/+7/−12 semitones and across
note changes, grain-boundary discontinuity, bypass toggling, stem loading and
sample-rate conversion, and worst-case per-block cost at 32 and 64 samples.

Pass it a **file** to run the tracker and shifter over real playing — a DI take,
or a separated bass stem. It reports how much of the take has notes sounding,
how much of *that* tracked confidently, the detected range, and whether the
shifted output really sits an octave above the input window by window. It also
writes `<name>-octave.wav` next to the input — 100% wet — so you can listen to
the shifter alone.

Pass it a **folder** to load a real song through the actual stem player and see
the per-stem levels.

Measured on the separated bass of a real Suno track: notes sounding in 28% of
the first minute, 94% of those frames tracked confidently, and the octave
landed within 30 cents in 41 of 42 half-second windows.

`rig_uishot <dir>` renders the window to PNGs offscreen, so layout can be
checked without a person having to look at a running app.

---

## Licensing

Built against JUCE 8 under its AGPLv3 option. That is fine for personal use;
distributing the app to anyone else means distributing the source too, or
buying a JUCE licence.
