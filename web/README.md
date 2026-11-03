# LiveRig for the phone

A browser port of LiveRig's live rig, everything except the backing-track side
(songs, stems, the splitter, the TOTAL fader and the stem ducker). It runs on a
phone with an audio interface plugged in, installs to the home screen and keeps
working offline.

The DSP is a line-by-line port of `Source/dsp` and `FxChain.cpp` to JavaScript,
running in an AudioWorklet. Nothing is approximated with stock Web Audio nodes:
the same YIN tracker, the same pitch-synchronous octave shifter, the same
Dattorro plate and the same spectral freeze run here.

## Run it

Any static file server works. The microphone needs HTTPS, except on `localhost`.

```bash
cd web && python3 -m http.server 8000
```

To use it on a phone, publish `web/` over HTTPS. GitHub Pages works: set
Pages to serve the branch, then open `/web/` and use "Add to Home Screen".

## The interface

Built for one thumb, standing up, with an instrument in the other hand.

- **The strip rail is across the top**: Bass, Mic, Keys, extra channels, and
  the master meter. Each chip has a live level meter. Tap one to put its board
  on screen; **+** adds a strip. The LIM badge on the master meter appears
  only while the safety limiter is catching something.
- **Four tabs across the bottom**, in thumb reach: Board, Loop, Tune, Foot.
- **Board**: the signal chain as a list, top to bottom. Each row shows the
  pedal's name and a one-line readout of its key settings. The big switch on
  the right turns it on or off without looking. MUTE switches are red, the
  freeze's HOLD is blue. Drag the dotted grip to reorder. Tap the row for
  its controls.
- **Editors slide up from the bottom**, one pedal at a time. Effect presets
  are at the top and the controls are full-width horizontal faders: drag
  sideways for fine moves, tap to jump, double-tap for the default. Vertical
  drags scroll the page, so you can't knock a knob while scrolling. Choice
  controls are segmented buttons. The EQ is its curve: drag the numbered
  nodes.
- **Loop**: one big status readout (state, bar, beat lights, position) and
  six pads, each tagged with the foot-switch number that does the same job.
- **Tune**: a big note name, a cents needle with a green zone, and the
  string row. BASS/GUITAR switch.
- **Foot**: learn a control by stepping on it, then give it jobs from a
  menu of the whole rig.

## What maps to what

| Desktop                                     | Phone                                                                    |
|---------------------------------------------|---------------------------------------------------------------------------|
| Effects list + parameter panel              | Board list + slide-up editor                                              |
| All 15 movable stages, Reverb, Input, Live out, Master + limiter | Same, same parameters, same factory presets (11 chain, 69 effect) |
| Drag to reorder, ADD EFFECT, remove         | Same                                                                      |
| Strips: bass, mic, keys, generic channels   | Same; each strip picks Input 1 / Input 2                                  |
| Tuner tab                                   | Tune tab (same median/slew/hysteresis filter)                             |
| Looper pane                                 | Loop tab, with the same latency compensation                              |
| FOOT tab, layouts, learn, jobs, verbs       | Foot tab; also learns **keys** so Bluetooth page-turner pedals work       |
| MIDI out from the tracker, PANIC            | Web MIDI out to a chosen port                                             |
| Keys strip with a hosted AU instrument      | Six built-in synth patches                                                |
| Autotune lane (hosted Graillon/MAutoPitch)  | Native pitch corrector: key, scale, retune speed, amount                  |
| Vocoder lane (hosted TAL-Vocoder)           | Native 20-band channel vocoder, carrier = bass after its board            |
| Amp: NAM capture                            | Amp: impulse-response capture (.wav)                                      |
| Footer: device, rate, I/O + FX latency      | Status line + Settings sheet                                              |
| settings.json / presets.json / foot.json    | Saved on the phone as you go; Export/Import in Settings                   |

## What the browser can't do

- **Neural Amp Modeler captures** (`.nam`) aren't loaded. The Amp stage takes
  impulse responses instead.
- **The Pedal stage** (any hosted AU/VST3) has no browser equivalent, so it
  is not there.
- **Web MIDI** is on Chrome (Android and desktop) but not iOS Safari. On an
  iPhone, MIDI pedals and keyboards are out. Bluetooth page-turner pedals
  still work as foot controls, and the tracked notes still play the built-in
  Keys synth.
- There is no virtual MIDI port: notes go to a hardware or OS-level port you
  pick.
- Parallel jack-fed strips aren't latency-aligned at the sum. On the web the
  boards' latencies are a millisecond or two.

## Files

- `js/dsp.js`: the DSP building blocks (biquads, compressor, tracker,
  shifter, plate, freeze, segmenter, oversampler, convolver).
- `js/engine.js`: the per-strip chain, mic lanes, keys synth, looper and
  master bus.
- `js/worklet-main.js`: the AudioWorklet shell. At start-up `app.js`
  concatenates these three into one worklet module, so it works on browsers
  without worklet `import` support.
- `js/data.js`: parameter specs and factory presets, generated from
  `Source/Parameters.cpp` and `Source/PresetStore.cpp`.
- `js/model.js`, `js/foot.js`, `js/app.js`: state, the foot engine and the
  UI.
