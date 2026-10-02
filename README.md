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
  register. The display is deliberately slower than the tracker: a median
  over the last ~300 ms eats octave flicks, the needle glides to it, and the
  note name is locked with hysteresis — steady and actionable, the way the
  tuners people trust behave. A genuinely new note still snaps in under
  100 ms. Big note name, cents needle with a green in-tune zone, and a
  string row for reference — the BASS/GUITAR switch picks which strings are
  drawn, and is remembered. Tap any effect row to come back to the editor.
  **FOOT** is where a foot controller is taught what to do — see
  [The foot controller](#the-foot-controller).
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
restored on launch. Both kinds of preset live beside it in `presets.json`,
and the foot controller's map in `foot.json`.

Songs live in `~/LiveRig/Songs/<song name>/`, four WAVs named
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
| — | MIDI | channel, transpose, gate, hold, release, bend range, expression, live thru, instrument, level |
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
| — | Freeze | hold, level, fade in, fade out, motion — holds what is sounding while a foot stays down |
| 13 | Pedal | a hosted AU/VST3 effect as a chain insert — pick it, drag it anywhere |
| 14 | Reverb | size, damping, mix — Freeverb, mono in / stereo out |
| 15 | Live out | lane gain |
| 16 | Master | gain over the whole mix — stems and live — into the safety limiter |

The **Autotune** and **Vocoder** rows above the chain are the mic's two
lanes, not stages: the second interface input, pitch-corrected or making the
bass sing — see *The mic lanes* below.

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

### The freeze

**Freeze** is a sustain pedal for anything with a jack: play a note or a
chord, put a foot down, and it carries on for as long as the foot stays
there while you play over the top. Lift the foot and it fades away.

It is a pedal on the board like any other — **ADD EFFECT...**, Freeze — and
it does nothing until **Hold** goes down. Hold is the thing to give a foot
(see [The foot controller](#the-foot-controller)); the switch in the editor
is the same Hold for a finger, which is how to try it with no pedal plugged
in.

- **Where it stands on the board is what it hears.** Every pedal above it is
  in the held sound, exactly as it sounded at the press; every pedal below
  it — and the reverb — colours the held sound and the live playing alike.
  So with Freeze below the Overdrive, a clean chord can be held, the drive
  kicked on, and a driven line played over a chord that stays clean.
- **Your playing never goes through it.** The held sound is a layer laid
  beside the live signal. Nothing is delayed and nothing is ducked, which
  also means that while the string is still ringing you hear the string
  *and* the hold of it — it swells at the press and settles as the string
  dies. **Level** is the held layer against what was played: all the way up
  is the level it was caught at.
- **Fade in** and **Fade out** run from nothing to a second: how the layer
  arrives under the note, and how it leaves when the foot comes up.
- **Motion** is a slow chorus on the held layer only. At zero the hold is
  perfectly still, like an organ stop; turned up it drifts. It leaves the
  lows alone, so a held bass note keeps a steady bottom.
- **Step after the note, or with it.** It holds what is sounding as the foot
  lands — the 170 ms around that moment. A foot that comes down *before* the
  note catches whatever came before the note.
- A new press while the last hold is still fading starts a new hold and
  lets the old one finish dying underneath it.

Hold is a gesture, not a setting: it is never saved, and no preset carries
it. The fades, level and motion are saved like any other knob.

What is held is not a loop. A loop of a slice has a seam, and on a low note
the seam pulses. Instead the stage reads the spectrum of the moment, finds
each partial in it — its pitch, its strength — and keeps sounding those
partials as steady tones. That is why a hold can last a minute without a
ripple, and why a press that lands across a note change still comes out
level: there is nothing in the held sound that remembers the edges of the
slice it came from.

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
volume pedal. Pitch bend follows the tracker for slides — within the wheel's
range only, and every note starts on an explicitly recentred wheel, because
the receiver's wheel keeps whatever the previous note left on it and a note
started on a stale bend plays detuned with nothing left to correct it.

**With MIDI on, the bass itself usually should not sound.** Driving a pad
means the rig is a controller, so **Live thru** decides whether the live lane
still reaches the mix: off — the default — mutes the bass and leaves the pad;
on keeps both for layering. The tracker hears the input either way.

Two things are worth knowing. It is monophonic — YIN cannot do double stops —
and it never trusts the period on its own: the tracker deliberately holds its
last good period through unvoiced passages so the octave shifter does not drop
out mid-note, and for MIDI that would sustain a note that has already finished.
Liveness comes from confidence and level only.

A stuck pad note is the worst thing that can happen on stage, so there are three
independent ways a note ends — confidence dropping, the level gate, and a
maximum length — and a **PANIC** button that stays visible in the MIDI row's
header whether or not the row is open.

### The instrument, inside the MIDI row

The notes can go two places, and both live on the MIDI row because neither
exists without it: out of the virtual port to another application, and into an
Audio Unit hosted **inside** the rig. The second is the interesting one — one
app, one buffer, one output, no inter-application routing, so the pad's latency
is just the detection latency with nothing stacked on top.

Pick from the box at the top of the row's editor, hit **Open...** for the
plugin's own window, and set its level. What loads and the patch it is on are
both saved with your settings.

It sits inside MIDI rather than on a row of its own because it is an option for
MIDI and nothing else: the segmenter only runs when MIDI is on, so an
instrument on a separate row would have had a switch that did nothing whenever
that row was off, with nothing on screen to say so.

**Logic's instruments cannot appear here.** Alchemy, Sculpture, ES2 and the
rest are registered for Apple's hosts only — they are not installed as Audio
Units and no third-party host can see them. What is reachable is everything
installed as a normal AU. On this machine that is nine: Vital, LABS, Kontakt 5,
SKY Keys, MPowerSynth, Splice Bridge, and Apple's three built-ins.

The first scan takes a while, because identifying an Audio Unit means
instantiating it. It runs on a background thread with the app fully usable and
the result is cached, so it happens once. The same scan records **effect
plugins** for the vocal channel and the Pedal stage — AU types instrument,
effect and music effect are touched; the converters, panners and codecs among
this machine's 341 components are not. A plugin installed as both AU and VST3
appears once, as its AU.

A sample library that starts with no patch loaded will be silent until you
choose one in its own window. That is the plugin's business, not the host's.

### The mic lanes

The second interface input — the mic beside the bass — has two lanes, each
one function with one ON switch, crossfaded so a footswitch press never
clicks. They share the mic's facts: gain, and a gate (open at **Gate**, shut
6 dB under it, so breaths die between phrases without eating consonants)
whose detector listens ahead of the fader. Both lanes count as performance:
they duck the backing stems and ride the master limiter like everything the
player does.

**Autotune** runs the mic through a hosted pitch corrector. **Key &
style...** opens the plugin's own window, where key, scale and how hard the
correction grabs live; those settings persist with the rig's.

**Vocoder** makes the bass sing. The mic is the *modulator*; the carrier is
the bass **after the whole chain** — octave-up, drive, everything, tapped
ahead of Live out. That patch is the carrier conditioner: a bright,
harmonically rich carrier is what makes the words land, so if they come out
muddy, drive the chain brighter. Words ride the bass line's pitch; the raw
mic never reaches the mix through this lane. Pull Live out down and the bass
itself goes silent while it keeps singing — the tap sits ahead of that fader
on purpose. The carrier reaches the plugin on its **sidechain bus** where
one is offered; a vocoder without one gets it on the right of the main pair,
and the status line always names the route in force, because a silently
failed sidechain would just sound like a broken vocoder.

**There is nothing to pick on either row.** The rig chooses each plugin
itself from a short list of the real thing — Graillon 3 first for pitch
(free, auburnsounds.com), TAL-Vocoder first for vocoding (free,
tal-software.com), with MAutoPitch and MVocoder as the stopgaps — and each
row's status line names what it found, its latency, and what to install when
nothing is. Install a better tenant, hit Rescan on the MIDI row or relaunch,
and it takes over automatically. TAL is also *configured* for the role the
moment it binds: its routing takes two switches ("inputmode" selects the
external carrier, "sidechain" says it arrives on the sidechain bus — either
alone is silence), its internal synth is muted so the bass is the only voice
box, and a note is held down for it because its band envelopes are
MIDI-gated even with an external carrier. All of that was measured, not
read, and your own saved state always wins over it.

The lanes are MIDI-mapped out of the box: **Program Change 0/1/2** from any
connected controller is silence / autotune / vocoder — nothing to configure,
controllers plugged in mid-session picked up within seconds. That mapping is
no longer wired in; it is the layout the FOOT tab starts with, and it can be
changed there like any other. Both switches can also be stacked by hand in
the UI.

### The foot controller

The **FOOT** tab maps whatever is under your foot to whatever the rig can
do, in two layers that can each be swapped without touching the other.

**Controls** are the things under the foot, by name. **ADD A FOOT
CONTROL...**, then step on it: the rig captures what it sent instead of
obeying it, and a card appears. A card shows the name (double-click to
retype it), what the control sends, and a lamp that lights when it is
stepped on. Three things arrive from a foot controller and each is its own
kind of control:

- a **program change** — a preset button. It fires once; there is no "off".
- a **controller that is 127 or 0** — a switch. *Latching* is a switch that
  keeps its own light (each press alternates lit and dark); *Momentary* is
  down while held. Learning guesses which from what it heard, and the menu on
  the card corrects it.
- a **controller that sweeps** — an *Expression* pedal.

MIDI Start, Stop and Continue can be learned too.

**Jobs** are what a control does. The **+** on a card opens the rig as a
menu — every strip on the rail, every pedal on its board, each pedal's
switch and each of its knobs, a strip's presets, the song's transport, loop
tap and stems, the master, MIDI panic. A control can carry as many jobs as
you give it, so one preset button can set a whole scene: drive on, chorus
off, delay mix to 0.25. What a job's control looks like depends on what it
moves:

- a **switch** (an effect's ON, a mute) takes *Follows it* (on while the
  pedal is lit, held, or past half way — the one that keeps the pedal's
  light and the rig in agreement), *Turns on*, *Turns off*, or *Flips*;
- a **knob** takes a fader with two caps: where it sits with the pedal up
  and where with it down, and the pedal sweeps between. Right-click a row
  for **Swap ends**. From a preset button the fader has one cap — the value
  the press sends it to;
- a **command** (play/stop, loop tap, recall a preset, panic) just happens
  on the press.

The **×** at the end of a job takes it off. The **...** on a card is that
control's own menu: **Learn again**, or remove the control. A right click on
either does the same.

**Layouts** are named sets of jobs — the box at the top of the tab. The
controls stay; what they do changes. **Copy** starts a new layout from the
one in force, typing a new name renames it, and *Switch foot layout* is
itself a job, so one switch can change what all the others mean.

That is the interchangeable part. A different song or a different gig is a
different layout over the same controls. A different controller — or the
same one reprogrammed, or moved to another MIDI channel — is **Learn again**
on each card, and every layout still does what it did.

**Listening to** narrows the tab to one MIDI input; *Any controller* hears
them all. **Last heard** shows the most recent thing any controller sent,
obeyed or not — the first thing to look at when a pedal seems dead.

When the foot switches on a pedal that is not on its strip's board, the
pedal joins the board, the same way a preset's pedals do.

**A sustain pedal for the Freeze** is one control with one job: learn the
pedal, then **+** → the strip → Freeze → **Hold**, and leave it on *Follows
it*. Down holds, up lets go. Any kind of control works — a momentary switch,
or a pedal that only ever stands at one end or the other — because Follows
it only asks whether the control is past half way.

**No sustain pedal? Two buttons latch it.** Give one preset button the job
Freeze → Hold on *Turns on* and another the same job on *Turns off*: the
first starts the hold and it stays until the second lets it go. One button
on *Flips* does both — press to hold, press again to release. Either way
the hold is of what was sounding when it started; to catch a new chord, let
go and start again.

#### The Ground Control

The Digital Music Corp Ground Control (1998) is a 5-pin MIDI device: its
**MIDI Out** goes to the interface's **MIDI In**, and it needs its own power
adaptor — the phantom power it can take over the MIDI cable comes from a GCX
switcher or a 7-pin box, never from an audio interface. What it sends, from
its manual:

- **Preset buttons** send program changes, one per "device" it has been told
  about, each on that device's MIDI channel. The manual says it filters
  redundant program changes; measured on this unit, that means a device
  whose program is the same in the next preset is skipped, while pressing
  the preset you are already on sends everything again. So a preset button
  is a dependable press, repeats included, as long as its control was
  learned from a program that button always sends. While a GCX switcher is
  enabled in its Setup mode, every preset also sends the eight loop
  controllers, 80–87 on channel 16.
- In **Bank4** preset mode the upper row (5–9 and 0) becomes six
  **instant-access** buttons, each of which can be set in its Setup mode to
  send a controller as 127 / 0 with its LED following. Those are *Latching*
  switches here, and *Follows it* keeps the LED honest.
- **Two expression inputs** take ordinary passive guitar volume pedals,
  enabled and calibrated in Setup mode, and send any controller 0–120.
  Which controller is stored per preset, so a pedal can change what it
  sends when the preset changes. Keep them on a controller: it can also
  send pitch bend or aftertouch, and those are not learned as foot controls
  (pitch bend goes to the keys).
- A button can also be set to **sequencer start/stop**, which arrives as
  MIDI Start and Stop.

It has no jack for a switch pedal as such. A keyboard **sustain pedal** in
one of the expression inputs should read as an expression pedal that only
ever stands at one end or the other, which is all a Freeze's Hold needs —
enable and calibrate that input with the pedal up as the heel and down as
the toe. If it comes out backwards (holding with the foot *up*), calibrate
it the other way round, or flip the pedal's polarity switch if it has one.
**Last heard** shows what it really sends. This has not been tried on this
unit yet.

Its display counts programs from 1 unless told otherwise ("STARTS AT 01"),
so its preset 21 arrives here as program change 20. The cards show what is
on the wire.

### Hosted plugins

Third-party AU and VST3 effects load into three fixed roles — the two mic
lanes' plugins (chosen automatically, above) and the **Pedal** stage in the
bass chain, which does have a picker, because "any effect, anywhere in the
chain" is its whole point. Effects are configuration, not builds.

What the host guarantees, because this is stage software:

- Scanning runs **out of process**: every plugin is described in a
  disposable child, so a plugin that crashes on inspection crashes the
  child, not the rig. One JUCE-built sampler on this machine posts a
  deferred lambda that fires after its instance is destroyed — in-process,
  that was a startup crash waiting for a stage.
- A plugin that fails to load leaves its slot empty and audio running.
- A plugin that blows up mid-song is ejected on the spot and cleaned up off
  the audio thread; the slot's status line says so.
- An empty Pedal slot is a wire; empty mic lanes are silence, not a crash.
- Added latency is *surfaced* in the row's status line, never silently
  compensated. Real pitch correction inherently costs ~20-25 ms — detecting
  a low voice's pitch takes a couple of its periods, and no plugin escapes
  that. Graillon 3 reports 24.4 ms and the row says so, to the decimal.
- Plugin settings persist as state blobs and recall on launch, exactly like
  the AU instrument's.

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
  path; the drive's oversampler costs about a tenth of a millisecond. The
  freeze adds none either: it only ever looks back at what was already
  played, and the live signal does not pass through it.

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

The app installs Demucs into `~/LiveRig/venv` and calls it there,
so nothing is added to your system Python. If you would rather do it by hand:

```bash
python3 -m venv ~/LiveRig/venv
~/LiveRig/venv/bin/pip install --upgrade pip
~/LiveRig/venv/bin/pip install demucs numpy
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

`rig_tests --freeze` runs the freeze alone, at three sample rates: armed and
not held it must be a straight wire, a held tone must keep its level and
pitch with no pulse, a low E must keep the balance of its harmonics, the
layer must join a still-ringing note in phase, letting go must fade in the
time set, three presses inside one fade must leave only the last and never
click, and Hold must never reach the settings file. Then it puts two feet on
a real bass stem — a careless one that comes down wherever, whose holds must
all be steady, and a deliberate one that lands with long notes and after
them, whose holds must come out at the level played.

`rig_tests --foot` runs the foot map alone, in under a second: a lit switch
that the rig follows, a held one, a pedal's sweep and its one press on the
way up, a sustain pedal and a pair of buttons each holding the freeze
through a real chain, a preset button setting a scene across channels,
layouts trading
jobs, learning that captures instead of obeying, and a map file that
survives being saved, damaged or missing.

`rig_uishot <dir>` renders the window to PNGs offscreen, so layout can be
checked without a person having to look at a running app. It also proves
that every function the foot menu offers is one the engine can carry out,
and walks the FOOT tab the way a player would — learn, give jobs, press,
learn again, copy and switch layouts, remove — without a mouse, a pedal, or
a write to the real `foot.json`.

---

## Licensing

Built against JUCE 8 under its AGPLv3 option. That is fine for personal use;
distributing the app to anyone else means distributing the source too, or
buying a JUCE licence.
