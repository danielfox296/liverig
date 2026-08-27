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
- **LOOP** (or the **L** key) is the section looper, and it is a tap
  instrument, not a drag one: while the song plays, tap at the top of the
  section, tap again at the end, and the song cycles that stretch with a
  crossfaded seam until you tap it off. Your taps *are* the loop points —
  musicians place a downbeat with a key press far better than anyone places
  one with a mouse. The loop survives stop (play runs back into it) and
  clears when you switch songs.
- **STEMS**: the four stems as a mixer — fader and mute each. Missing stems are
  greyed out. You will normally pull `bass` down or mute it.
- **Effects** are a list: each row is a name and an ON toggle you can hit
  without looking. Tap a row and its controls come up in the **parameter
  panel** beside the list — one effect's controls at a time, nothing stacked,
  nothing to scroll past. Tap the row again to put them away; with nothing
  selected the panel just says so. Which effect is up is remembered between
  sessions.
- **The list is the signal chain**, top to bottom, and the rows with a
  gripper can be **dragged into any order** — put drive before the filter,
  chorus before the octave, whatever the song wants. Input stays at the top,
  Live out at the bottom, and Reverb keeps the second-to-last slot because it
  is where the lane becomes stereo. The order applies live as you drop the
  row and is saved with your settings — and **whole-chain presets capture it
  too**: recalling a preset rewires the chain the way it sounded when it was
  saved. A preset from before order was captured carries none, and leaves
  your wiring alone.
- **Presets** come at two scales. The bar across the top of the effect column
  stores the **whole chain** — everything from input trim to live out, but not
  the stem faders, which belong to the song rather than the sound. Eleven
  ship with it: Clean DI, Warm DI, Fingerstyle comp, Octave lead, Dirty
  octave, Phaser funk, Ambient swell, Funk filter, Synth bass, Stacked lead,
  Analog wash.

  Each effect also has **its own preset row** at the top of the parameter
  panel, for building a sound rather than recalling one — Fast FET on the
  compressor, Quack on the filter, Fusion lead on the drive, Square sub on the
  synth, Tri lush on the chorus, Carbon copy on the delay, and so on. Type a
  name and hit **Save** on either bar to add your own. When an update ships
  new factory presets, they are merged in once by name; anything you saved
  yourself is never touched.

  An effect preset deliberately does *not* carry the ON state. Recalling how
  something sounds should not decide whether it is engaged — that is what the
  ON button is for, mid-song. A whole-chain preset does carry it, because there
  the point is to recall an entire sound.
- **The panel beside the effect list has tabs.** EFFECT is the parameter
  editor; **TUNER** is a chromatic tuner running on the same pitch tracker as
  the octave and synth — accurate to well under a cent across the whole bass
  register. Big note name, cents needle with a green in-tune zone, and a
  string row for reference — the BASS/GUITAR switch picks which strings are
  drawn, and is remembered. Tap any effect row to come back to the editor.
- **Master** is the last row: one gain over the whole mix — stems and live
  summed — feeding an always-on safety limiter. When the limiter is working,
  an orange **LIM** readout appears beside the master meter with the dB it is
  catching. Occasional flickers on peaks are healthy; a constant large number
  means the mix is genuinely too hot — pull the master or the stems down.
- **Footer**: the audio device picker, the current rate and buffer size, and
  the latency you are actually playing against, split into its two honest
  parts — what the device costs (I/O) and what the chain adds (FX).

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

The stages between the input tap and the reverb are reorderable — drag a row.
Two carry a **(B)**: they were added for bass specifically, and they are the two
places where the general-purpose version of the effect falls down on a low
instrument.


The default order. Everything between Input and Reverb can be dragged into
any order in the app; Input, Reverb and Live out hold their places.

| # | Effect | Controls |
|---|--------|----------|
| 1 | Input gain | trim, meter (a 5 Hz DC blocker sits here too) |
| — | MIDI | channel, transpose, gate, hold, release, bend range, expression |
| — | Instrument | an AU instrument played by those notes, and its level |
| 2 | Compressor | threshold, ratio, attack, release, makeup, blend, sidechain |
| 3 | EQ | low shelf, parametric mid (freq/gain/Q), high shelf |
| 4 | Filter | sens, attack, release, from, to, res, mix — envelope filter |
| 5 | Boost | boost, sheen, hair — clean gain with a JFET-style knee |
| 6 | Overdrive | voice, drive, tone, level, stack, bite, crossover, blend — 4x oversampled |
| — | Amp | gain in/out — a Neural Amp Modeler capture |
| — | Cab (B) | bump freq, bump, top, presence — the filter a speaker is |
| 7 | Octave | interval (±12 semitones), dry/wet, second voice |
| — | Sub (B) | level, tone, divide — an analogue-style octave divider |
| 8 | Synth | shape, sub, cutoff, env, res, glide, mix — tracked synth voice |
| 9 | Phaser | rate, depth, feedback — 4-stage |
| 10 | Chorus | rate, depth, voices, tone, keep lows, mix |
| 11 | Delay | time, feedback, mix, high cut, mod rate, mod depth, sat |
| 12 | Tape | drive, soften, bump, mix — tape-machine warmth |
| 13 | Reverb | size, damping, mix — Freeverb, mono in / stereo out |
| 14 | Live out | lane gain |
| 15 | Master | gain over the whole mix — stems and live — into the safety limiter |

Attack and release are exposed on the compressor because "fast attack
available" is the whole point of a FET-style feel on bass.

### The pedal-shaped effects

Five of the stages are modelled on specific boxes rather than invented:

- **Filter** is a Mu-Tron-style envelope filter: your playing level drives a
  state-variable lowpass between **From** and **To**. The two faders replace
  the usual range-and-direction switches — set To *below* From and the sweep
  runs downward. Res is the quack. The filter core is a TPT (Zavalishin) SVF,
  chosen because it stays stable no matter how fast the envelope shoves it.
- **Boost** is the Keeley Katana idea: up to +30 dB that stays honest at
  stage level. **Sheen** is a ±6 dB shelf at 2.8 kHz (cut for warm, boost for
  presence), and **Hair** lowers the knee of a soft clip so a slammed input
  folds over the way cascaded JFETs give out. With the Overdrive on, the
  boost shoves its front end exactly like a boost pedal into a dirt pedal.
- **Overdrive** grew a second voice, after the Wampler Dual Fusion: the
  original `tanh` stage is the smooth side, and **Stack** blends in a second,
  mid-tilted stage that clips around a biased operating point — even
  harmonics, the warmth a symmetric clipper cannot make. **Bite** voices how
  hard its mid-forward tilt pushes. Both run inside the same 4x oversampler,
  and a DC blocker mops up what asymmetric clipping rectifies.
- **Chorus** is Corona-shaped: **Voices** picks one modulated tap (vibrato
  flavour at full mix), the classic pair, or all three offset in rate, depth
  and phase — the tri-chorus wash. **Tone** darkens only the wet voices, and
  **Keep lows** highpasses the *difference* signal so everything below it
  stays untouched dry bass. Chorus that does not eat the low end.
- **Delay** learned the Carbon Copy tricks: **Mod rate/depth** put a slow
  warble on the read tap (the BBD clock wobble that reads as tape), and
  **Sat** squashes the feedback path through a compander-style curve — unity
  for quiet repeats, folding over as they get hot, so high feedback rings
  and swims instead of running away. Repeats read through the same 4-point
  Hermite interpolator as the octave shifter, so the moving tap stays clean.
- **Tape** is the warmth stage — the cure for "harsh and digital". **Drive**
  pushes into a softly biased saturator wrapped in pre/de-emphasis, so the
  highs saturate first: that is the mechanism that makes tape smooth on top,
  and it means digital sheen and string clank get rounded before anything
  else distorts. The bias puts a little second harmonic under it. **Soften**
  sets how strong that spectral tilt is (plus a gentle final rolloff),
  **Bump** is the tape head bump — up to +4.5 dB at 80 Hz, free weight for
  bass — and quiet playing always passes at unity, so it can idle engaged.
  It defaults to sitting just before the reverb and drags anywhere.

### The synth voice

The one that is not a pedal copy — because the app already had the part
every synth pedal struggles with. The same pitch tracker that drives the
octave shifter drives a pair of polyBLEP oscillators: **Shape** morphs square
to saw, **Sub** adds a square an octave down, **Glide** slews between notes.

There is no gate. The voice is keyed by an envelope follower on the live
signal — play harder and it opens, stop and it dies with the string — and the
same envelope sweeps a resonant TPT lowpass (**Cutoff** up to **Env** octaves),
which is what makes it read as a synth rather than a fuzz. At full **Mix** the
bass *is* a synth; at half it is a layer underneath your real sound. It sits
after the octave and before the modulation, so phaser, chorus, delay and
reverb all treat it as a first-class citizen.

---

### MIDI out

The pitch tracker already runs off the trimmed input for the octave and synth
stages, so it costs almost nothing to also treat it as a note source. Turning
MIDI on publishes a virtual port called **LiveRig** — Logic, MainStage and
anything else with a MIDI input simply see it, with no cable and no interface.

It is aimed at **swells and pads**, and that is what makes it work. The reason
bass-to-MIDI is usually disappointing is latency: identifying a low note takes
two or three of its periods, which at the bottom of the range is tens of
milliseconds, and no amount of clever code beats that. Aim at a sound with no
transient to hit and the constraint stops mattering — so the segmenter spends
that time being certain instead. A candidate pitch has to hold for **Hold**
before it may speak, and displacing a note that is already sounding costs twice
that plus a clear move past the semitone boundary, which is what stops a deep
vibrato retriggering a pad every cycle.

**Expression is the control that matters, not velocity.** A swell's loudness
comes from the swell, not the pluck, so the envelope drives CC11 (or CC1)
continuously while the note sustains. That is volume-pedal swells without the
volume pedal. Pitch bend follows the tracker for slides.

Two things are worth knowing. It is monophonic — YIN cannot do double stops —
and it never trusts the period on its own: the tracker deliberately holds its
last good period through unvoiced passages so the octave shifter does not drop
out mid-note, and for MIDI that would sustain a note that has already finished.
Liveness comes from confidence and level only.

A stuck pad note is the worst thing that can happen on stage, so there are three
independent ways a note ends — confidence dropping, the level gate, and a
maximum length — and a **PANIC** button that stays visible in the MIDI row's
header whether or not the row is open.

### Instrument

The MIDI row can drive another application, but the Instrument row plays an
Audio Unit **inside** the rig: one app, one buffer, one output, and no
inter-application routing. The pad's latency is then just the detection
latency, with nothing stacked on top of it.

Pick from the box, hit **Open...** for the plugin's own window, and set its
level. What loads and the patch it is on are both saved with your settings.

**Logic's instruments cannot appear here.** Alchemy, Sculpture, ES2 and the
rest are registered for Apple's hosts only — they are not installed as Audio
Units and no third-party host can see them. What is reachable is everything
installed as a normal AU. On this machine that is nine: Vital, LABS, Kontakt 5,
SKY Keys, MPowerSynth, Splice Bridge, and Apple's three built-ins.

The first scan takes about forty-five seconds, because identifying an Audio
Unit means instantiating it. It runs on a background thread with the app fully
usable and the result is cached, so it happens once. Only components whose type
says instrument are touched — this machine has 341 of them and one refuses to
open at all, so scanning everything would be slow and fragile for no gain.

A sample library that starts with no patch loaded will be silent until you
choose one in its own window. That is the plugin's business, not the host's.

### What the (B) stages are for

Three ideas separate bass processing from the general case, and they are worth
knowing even if you never touch these controls.

**Dirt belongs on top, not underneath.** Run a full-band bass signal into a
clipper and the fundamental intermodulates with everything above it: the note
stops being a note and becomes mud. Every convincing recorded bass distortion is
parallel — the lows go round the clipper and the highs go through it. The
Overdrive's **Crossover** sets where that split happens and **Blend** crossfades
the driven band back against its own clean copy. At Crossover's minimum the
stage behaves exactly as it did before, so nothing you already dialled has
moved; wind it up to 150-250 Hz and the same drive setting suddenly stays tight.
Blend at zero is a bit-exact wire.

**A compressor should not hear the fundamental.** Without a high-passed
detector, every low E ducks the whole instrument — the pumping people mistake
for compression. The **Sidechain** control sets where the detector stops
listening. At 20 Hz a low note pulls 12.5 dB of reduction; at 300 Hz the same
note pulls 1.2 dB.

**An octave divider is not a pitch shifter.** The Octave stage transposes
faithfully, which is not what an OC-2 does and not why people still pay over the
odds for one. **Sub (B)** is a flip-flop: it toggles on rising zero crossings,
so the output is a square wave at half or a quarter of the input frequency with
no relation to the shape of what you played. That squareness, and the way it
stumbles on a chord, is the sound. It sits under the dry note rather than
replacing it. The pitch tracker's period sets the crossing filter, which is what
stops harmonics triggering false edges.

**Cab (B)** is the fourth: a speaker is a filter, and most of what people call
amp tone is the cabinet rather than the valves. The Amp stage supplies this when
a capture is loaded, since a NAM capture is taken through a real cab. Cab (B) is
what stands in when none is — a low-mid bump, nothing below 42 Hz, and a steep
rolloff above the Top control. Measured: −27.7 dB at 9 kHz relative to 400 Hz.

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

A pitch tracker runs as a side chain on the trimmed input, ahead of all the
movable stages — so no arrangement of dirt or modulation can feed it
harmonics that fool detectors. It lowpasses and decimates to
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

### Loudness, peaks and the master limiter

"It sounds quiet but it's clipping" is one problem, not two. Loudness is the
*average* level; digital clipping happens at the *peaks*. Bass is peaky, the
Demucs stems come back close to full scale, and four stems plus the live lane
is a sum that can pass 0 dBFS on its own — so the peaks were hitting the
ceiling while the average stayed modest. Inside the chain nothing clips
(floats have effectively unlimited headroom); the hard edge is the device
boundary, where anything past ±1.0 is simply cut off. That crackle was the
DAC, not an effect.

The master bus now ends in a **safety limiter**: instant attack, ~120 ms
release, ceiling at −1 dBFS, zero lookahead because this is a live path. By
construction its output cannot pass the ceiling — the alternative sound is
the DAC hard-clipping, which is never the better one. On sustained material
the envelope rides the waveform, so it stays clean; only transient tips get
caught. The **LIM** readout by the master meter shows its gain reduction
live, which is also the honest answer to "was that clipping?" — it is the dB
that *would have been* clipped.

That changes how to set levels: get the input trim so your hardest playing
sits high and green on the input meter, balance stems and live lane to
taste, then use **Master** for the overall level. If LIM flickers a dB or
two on digs and hits, all is well — that is the limiter doing its job. If it
sits on constantly, everything is just turned up too far; pull Master down
and turn your interface or monitors up instead.

### Where the latency lives

Three places, very unevenly:

- **The device.** Input buffer + output buffer + the driver's safety offsets.
  This is the big one nobody sees: the OS default is typically 512 samples,
  which is over 20 ms of round trip before a single effect runs. LiveRig now
  asks a fresh device for the smallest buffer of at least 64 samples instead
  (a few ms round trip), and the footer shows the real figure as "I/O".
  Whatever you pick in the device dialog is saved and wins from then on. The
  whole chain costs under a fifth of the callback budget even at 32-sample
  buffers, so small buffers are safe.
- **The octave shifter**, when it is on. Roughly three quarters of a period —
  9 ms around A, 18 ms on a low E, more below. That is the price of
  period-synchronous shifting and it cannot drop much below one period
  without giving back the phase-aligned crossfades that make it clean on
  bass. It scales with the note, and the footer's FX figure tracks it live.
- **Everything else is free.** Filter, boost, synth, phaser, chorus are
  zero-latency; delay and reverb are wet mixes on top of an uncoloured dry
  path; the drive's oversampler costs about a tenth of a millisecond.

One stage trick falls out of this: the synth's **Sub** is an octave-down
voice with effectively no latency (a 2.5 ms envelope), so when a part needs
tight sub-octave weight rather than the shifter's tracked-down voice, the
synth is the low-latency way to get it.

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
note changes, grain-boundary discontinuity, bypass toggling across all eleven
switches, stem loading and sample-rate conversion, and worst-case per-block
cost at 32 and 64 samples with every stage engaged.

The newer stages are tested by what they claim to do, not by their code: the
envelope filter must get brighter when the same phrase is played harder (and
darker with To below From), the boost must measure exactly the dB on its
fader and fold over with full hair, the stacked drive must add even harmonics
a single `tanh` cannot, the synth must sit on the played pitch with the sub a
clean octave down and stay silent on silence, the chorus must actually bend
pitch and leave a 60 Hz fundamental dry behind Keep lows, and the delay must
warble under mod and stay bounded at 0.95 feedback with the compander engaged.

Pass it a **file** to run the tracker and shifter over real playing — a DI take,
or a separated bass stem. It reports how much of the take has notes sounding,
how much of *that* tracked confidently, the detected range, and whether the
shifted output really sits an octave above the input window by window. It also
writes `<name>-octave.wav` next to the input — 100% wet — so you can listen to
the shifter alone, plus four showcase renders of the new chains over the same
take: `-funk-filter`, `-synth-bass`, `-stacked-lead` and `-analog-wash`.

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
