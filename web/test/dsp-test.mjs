// Offline checks for the web DSP: tracker accuracy, shifter pitch, every stage
// at once (finite output, CPU), freeze, looper, mic lanes and keys.
// Run with: node web/test/dsp-test.mjs
import fs from 'fs';
const W = new URL ('../js/', import.meta.url).pathname;
const strip = (s) => s.replace (/^import[\s\S]*?from\s+['"][^'"]+['"];?/gm, '').replace (/^export\s+/gm, '');
const src = strip (fs.readFileSync (W + 'dsp.js', 'utf8')) + strip (fs.readFileSync (W + 'engine.js', 'utf8')) + '\nreturn { Engine, PitchTracker, OctaveShifter, MOVABLE, PATCHES };';
const M = new Function (src) ();
const { SPECS } = await import (W + 'data.js');
const sr = 48000;
const chanIds = new Set ();
// channel params: inGain..rvMix + liveOut/liveMute
let inRange = false;
for (const s of SPECS) { if (s.id === 'inGain') inRange = true; if (inRange) chanIds.add (s.id); if (s.id === 'rvMix') inRange = false; }
chanIds.add ('liveOut'); chanIds.add ('liveMute');
const extra = [ { id: 'tuneKey', def: 0 }, { id: 'tuneScale', def: 0 }, { id: 'tuneSpeed', def: 20 }, { id: 'tuneAmount', def: 1 }, { id: 'vocSib', def: 0.3 }, { id: 'vocRelease', def: 60 } ];
const specs = SPECS.map (s => ({ id: s.id, def: s.def, channel: chanIds.has (s.id) })).concat (extra.map (e => ({ ...e, channel: false })));

function bass (f, secs, amp = 0.3) {
  const n = Math.floor (secs * sr), x = new Float32Array (n);
  for (let i = 0; i < n; ++i) { const t = i / sr; const env = Math.min (1, t * 50) * Math.exp (-t * 0.8);
    x[i] = amp * env * (Math.sin (2*Math.PI*f*t) + 0.5*Math.sin (4*Math.PI*f*t + 0.3) + 0.25*Math.sin (6*Math.PI*f*t+1)); }
  return x;
}
function estimateHz (x, start, len) { // zero-crossing-free: autocorrelation
  const cs = [];
  for (let lag = 20; lag < 2000; ++lag) { let s = 0, e1 = 0, e2 = 0;
    for (let i = start; i < start + len; ++i) { s += x[i] * x[i + lag]; e1 += x[i]*x[i]; e2 += x[i+lag]*x[i+lag]; }
    cs[lag] = s / Math.sqrt (e1 * e2 + 1e-12); }
  const mx = Math.max (...cs.slice (20));
  for (let lag = 21; lag < 1999; ++lag) if (cs[lag] > 0.9 * mx && cs[lag] >= cs[lag-1] && cs[lag] >= cs[lag+1]) return sr / lag;
  return 0;
}
function run (engine, input) {
  const n = input.length, L = new Float32Array (n), R = new Float32Array (n);
  const bl = new Float32Array (128), br = new Float32Array (128);
  let worst = 0, t0 = performance.now ();
  for (let o = 0; o + 128 <= n; o += 128) {
    const tb = performance.now ();
    engine.process ([input.subarray (o, o + 128), input.subarray (o, o + 128)], bl, br, 128);
    if (o > sr) worst = Math.max (worst, performance.now () - tb);
    L.set (bl, o); R.set (br, o);
  }
  return { L, R, ms: performance.now () - t0, worst };
}
// 1. tracker accuracy across the bass register
for (const f of [30.87, 41.2, 55, 98, 196, 392]) {
  const tr = new M.PitchTracker (sr); const x = bass (f, 0.6);
  for (let o = 0; o + 128 <= x.length; o += 128) tr.process (x.subarray (o, o + 128), 128);
  const cents = 1200 * Math.log2 (tr.frequency / f);
  console.log (`tracker ${f} Hz -> ${tr.frequency.toFixed (2)} Hz  (${cents.toFixed (2)} c, conf ${tr.confidence.toFixed (2)})`);
}
// 2. octave shifter output pitch through the engine
function makeEngine (board, vals = {}) {
  const e = new M.Engine (sr); e.init (specs, null);
  for (const k in vals) e.set (0, k, vals[k]);
  e.setStrips ([{ slot: 0, kind: 'bass', source: 0, board }]); return e;
}
for (const [semi, f] of [[12, 55], [12, 82.4], [-12, 110], [7, 98]]) {
  const e = makeEngine (['octave'], { octOn: 1, octInt: semi, octMix: 1 });
  const r = run (e, bass (f, 1.0));
  const hz = estimateHz (r.L, 24000, 4096); const want = f * Math.pow (2, semi / 12);
  console.log (`octave ${semi > 0 ? '+' : ''}${semi} on ${f} Hz -> ${hz.toFixed (1)} Hz (want ${want.toFixed (1)}, ${(1200*Math.log2 (hz/want)).toFixed (0)} c)`);
}
// 3. every stage on, NaN and CPU
const all = [...M.MOVABLE, 'reverb'];
const on = {}; for (const k of ['compOn','eqOn','fltOn','bstOn','odOn','ampOn','cabOn','octOn','subOn','syOn','phOn','chOn','dlOn','tpOn','rvOn']) on[k] = 1;
on.eq1G = 4; on.eq3G = -3; on.frzOn = 0;
const e = makeEngine (all, on);
const x = bass (55, 3.0, 0.5);
let r = run (e, x);
let nan = 0, peak = 0; for (const v of r.L) { if (!Number.isFinite (v)) nan++; peak = Math.max (peak, Math.abs (v)); }
console.log (`all stages: ${nan} non-finite, peak ${peak.toFixed (3)}, ${(r.ms / (x.length / sr) / 10).toFixed (1)}% of one core (avg), worst block ${r.worst.toFixed (2)} ms of ${(128/sr*1000).toFixed (2)} ms`);
// freeze: hold mid-note, check sustained output after input silence
const ef = makeEngine (['freeze'], { frzLevel: 1 });
const xin = new Float32Array (sr * 3); xin.set (bass (82.4, 1.0, 0.4));
const outF = new Float32Array (xin.length); const bl = new Float32Array (128), br = new Float32Array (128);
for (let o = 0; o + 128 <= xin.length; o += 128) { if (o === 0.5 * sr - (0.5*sr)%128) ef.set (0, 'frzOn', 1); ef.process ([xin.subarray (o, o+128)], bl, br, 128); outF.set (bl, o); }
const rms = (a, s, l) => { let q = 0; for (let i = s; i < s + l; ++i) q += a[i]*a[i]; return Math.sqrt (q / l); };
console.log (`freeze: rms at 0.6s ${rms (outF, 0.6*sr, 4800).toFixed (4)}, at 2.0s ${rms (outF, 2*sr, 4800).toFixed (4)}, at 2.8s ${rms (outF, 2.8*sr, 4800).toFixed (4)}; pitch held ${estimateHz (outF, 2*sr, 4096).toFixed (1)} Hz`);
// looper: record 1 bar at 120, check it plays back
const el = makeEngine ([]);
const lin = new Float32Array (sr * 6); for (let i = 0; i < lin.length; ++i) lin[i] = 0.2 * Math.sin (2*Math.PI*220*i/sr);
el.looper.press ('record', 0); el.looper.setBars (1);
el.looper.bars = 1;
const lo = new Float32Array (lin.length);
for (let o = 0; o + 128 <= lin.length; o += 128) { const inp = o < 4.0*sr ? lin.subarray (o, o+128) : new Float32Array (128); el.process ([inp], bl, br, 128); lo.set (bl, o); }
console.log (`looper: state ${el.looper.state}, loop ${(el.looper.loopLen/sr).toFixed (2)} s, playback rms at 5s (input silent) ${rms (lo, 5*sr, 4800).toFixed (3)}`);
// mic lanes: autotune + vocoder with a mic strip
const ev = new M.Engine (sr); ev.init (specs, null);
ev.set (0, 'vocalOn', 1); ev.set (0, 'vocoderOn', 1); ev.set (0, 'vocalGate', -70);
ev.setStrips ([{ slot: 0, kind: 'bass', source: 0, board: [] }, { slot: 1, kind: 'mic', source: 1, board: ['vocal','vocoder'] }]);
const voice = bass (220 * Math.pow (2, 0.3/12), 1.5, 0.3);
const ov = new Float32Array (voice.length);
let worstV = 0, totV = 0; const bb = bass (55, 2).subarray (0, 128);
for (let o = 0; o + 128 <= voice.length; o += 128) { const t0 = performance.now (); ev.process ([bb, voice.subarray (o, o+128)], bl, br, 128); const dt = performance.now () - t0; if (o > sr / 2) { worstV = Math.max (worstV, dt); totV += dt; } ov.set (bl, o); }
console.log ('mic lanes avg % of a core', (totV / ((voice.length - sr/2) / sr) / 10).toFixed (1));
let nanV = 0; for (const v of ov) if (!Number.isFinite (v)) nanV++;
console.log (`mic lanes: ${nanV} non-finite, worst block ${worstV.toFixed (2)} ms, vocal peak ${ev.vocal.peak.toFixed (3)}`);
// keys
const ek = new M.Engine (sr); ek.init (specs, null); ek.set (0, 'instOn', 1);
ek.setStrips ([{ slot: 0, kind: 'bass', source: 0, board: [] }, { slot: 2, kind: 'keys', source: -1, board: ['keys'] }]);
for (const name of Object.keys (M.PATCHES)) { ek.instrument.setPatch (name); ek.note (true, 60, 100); ek.note (true, 64, 100); let pk = 0, bad = 0;
  for (let b = 0; b < 200; ++b) { ek.process ([new Float32Array (128)], bl, br, 128); for (const v of bl) { if (!Number.isFinite (v)) bad++; pk = Math.max (pk, Math.abs (v)); } }
  ek.note (false, 60, 0); ek.note (false, 64, 0); for (let b = 0; b < 400; ++b) ek.process ([new Float32Array (128)], bl, br, 128);
  console.log (`patch ${name}: peak ${pk.toFixed (3)}, bad ${bad}`); }
