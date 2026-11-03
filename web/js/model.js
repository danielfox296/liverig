// The rig's state: parameter specs, the effect rows, strips, presets and
// persistence. Mirrors Parameters.cpp, the fxDefs table in MainComponent.cpp
// and PresetStore.cpp.

import { SPECS as RAW, FACTORY_EFFECT_PRESETS, FACTORY_CHAIN_PRESETS } from './data.js';

// Web-only parameters: the settings that lived inside hosted plugins' own
// windows in the desktop app (pitch corrector, vocoder) are native here.
const KEYS = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const EXTRA = [
    { id: 'tuneKey', label: 'Key', min: 0, max: 11, def: 0, skew: 0, step: 1, suffix: '', dec: 0, toggle: false, choices: KEYS },
    { id: 'tuneScale', label: 'Scale', min: 0, max: 5, def: 1, skew: 0, step: 1, suffix: '', dec: 0, toggle: false,
      choices: ['Chromatic', 'Major', 'Minor', 'Maj pent', 'Min pent', 'Blues'] },
    { id: 'tuneSpeed', label: 'Retune', min: 0, max: 200, def: 25, skew: 40, step: 0, suffix: ' ms', dec: 0, toggle: false },
    { id: 'tuneAmount', label: 'Amount', min: 0, max: 1, def: 1, skew: 0, step: 0, suffix: '', dec: 2, toggle: false },
    { id: 'vocSib', label: 'Consonants', min: 0, max: 1, def: 0.3, skew: 0, step: 0, suffix: '', dec: 2, toggle: false },
    { id: 'vocRelease', label: 'Release', min: 5, max: 300, def: 60, skew: 60, step: 0, suffix: ' ms', dec: 0, toggle: false },
];

export const SPECS = RAW.concat (EXTRA);
export const SPEC = Object.fromEntries (SPECS.map ((s) => [s.id, s]));

// Channel-scoped: one value per strip. Everything else exists once.
const ids = RAW.map ((s) => s.id);
const iIn = ids.indexOf ('inGain'), iRv = ids.indexOf ('rvMix');
export const CHANNEL_IDS = new Set (ids.slice (iIn, iRv + 1).concat (['liveOut', 'liveMute']));
export const isChannel = (id) => CHANNEL_IDS.has (id);
export const isMomentary = (id) => id === 'frzOn';

// ---------- normalised mapping (juce::NormalisableRange with skew for centre) ----------
function skewOf (s)
{
    if (! s.skew || s.skew <= s.min || s.skew >= s.max) return 1;
    return Math.log (0.5) / Math.log ((s.skew - s.min) / (s.max - s.min));
}
export function toNorm (s, v)
{
    const p = (v - s.min) / (s.max - s.min);
    return Math.pow (Math.min (1, Math.max (0, p)), skewOf (s));
}
export function fromNorm (s, n)
{
    n = Math.min (1, Math.max (0, n));
    let v = s.min + (s.max - s.min) * Math.pow (n, 1 / skewOf (s));
    if (s.step > 0) v = s.min + s.step * Math.round ((v - s.min) / s.step);
    return Math.min (s.max, Math.max (s.min, v));
}
export function fmt (s, v)
{
    if (s.choices) return s.choices[Math.round (v - s.min)] ?? String (v);
    if (s.toggle) return v > 0.5 ? 'On' : 'Off';
    if (s.suffix === ' Hz' && v >= 1000) return (v / 1000).toFixed (v >= 10000 ? 1 : 2) + ' kHz';
    if (s.suffix === ' ms' && v >= 1000) return (v / 1000).toFixed (2) + ' s';
    if (s.suffix === ' dB' && v <= -60 && s.min <= -60) return '-inf dB';
    let t = v.toFixed (s.dec);
    if (t === '-0' || /^-0\.0+$/.test (t)) t = t.slice (1);
    if ((s.suffix === ' dB' || s.suffix === ' st') && v > 0 && s.min < 0) t = '+' + t;
    return t + s.suffix;
}

// ---------- the rows ----------
// key, display name, on/switch param, editor params, movable, preset scope
export const FX = [
    { key: 'input',   name: 'Input',      on: 'inMute',    ids: ['inGain'], kind: 'mute' },
    { key: 'vocal',   name: 'Autotune',   on: 'vocalOn',   ids: ['vocalGain', 'vocalGate', 'vocalLevel'], lane: 'mic',
      style: ['tuneKey', 'tuneScale', 'tuneSpeed', 'tuneAmount'] },
    { key: 'vocoder', name: 'Vocoder',    on: 'vocoderOn', ids: ['vocalGain', 'vocalGate', 'vocoderLvl'], lane: 'mic', style: ['vocSib', 'vocRelease'] },
    { key: 'keys',    name: 'Keys',       on: 'instOn',    ids: ['instLevel'], lane: 'keys', scope: 'keys' },
    { key: 'midi',    name: 'MIDI',       on: 'midiOn',    ids: ['midiCh', 'midiTrans', 'midiGate', 'midiHold', 'midiRel', 'midiBend', 'midiExpr', 'midiThru'], lane: 'bass', scope: 'midi' },
    { key: 'comp',    name: 'Compressor', on: 'compOn',    ids: ['compThr', 'compRatio', 'compAtk', 'compRel', 'compMkup', 'compBlend', 'compSc'], movable: true, scope: 'comp', blurb: 'FET-style, high-passed detector' },
    { key: 'eq',      name: 'EQ',         on: 'eqOn',      ids: ['eq1F', 'eq1G', 'eq1Q', 'eq2F', 'eq2G', 'eq2Q', 'eq3F', 'eq3G', 'eq3Q', 'eq4F', 'eq4G', 'eq4Q', 'eq5F', 'eq5G', 'eq5Q'], movable: true, scope: 'eq', blurb: 'Five-band parametric' },
    { key: 'filter',  name: 'Filter',     on: 'fltOn',     ids: ['fltSens', 'fltAtk', 'fltRel', 'fltFrom', 'fltTo', 'fltRes', 'fltMix'], movable: true, scope: 'filter', blurb: 'Mu-Tron-style envelope filter' },
    { key: 'boost',   name: 'Boost',      on: 'bstOn',     ids: ['bstGain', 'bstSheen', 'bstHair'], movable: true, scope: 'boost', blurb: 'Clean gain, JFET knee' },
    { key: 'drive',   name: 'Overdrive',  on: 'odOn',      ids: ['odVoice', 'odDrive', 'odTone', 'odLevel', 'odStack', 'odBite', 'odXover', 'odBlend'], movable: true, scope: 'drive', blurb: 'Three voicings, 4x oversampled' },
    { key: 'amp',     name: 'Amp',        on: 'ampOn',     ids: ['ampIn', 'ampOut'], movable: true, scope: 'amp', blurb: 'Impulse-response capture' },
    { key: 'cab',     name: 'Cab (B)',    on: 'cabOn',     ids: ['cabBumpF', 'cabBumpG', 'cabTop', 'cabPres'], movable: true, scope: 'cab', blurb: 'The filter a speaker is' },
    { key: 'octave',  name: 'Octave',     on: 'octOn',     ids: ['octInt', 'octMix', 'oct2Int', 'oct2Lvl'], movable: true, scope: 'octave', blurb: 'Pitch-synchronous shifter' },
    { key: 'sub',     name: 'Sub (B)',    on: 'subOn',     ids: ['subLevel', 'subTone', 'subDiv'], movable: true, scope: 'sub', blurb: 'Analogue octave divider' },
    { key: 'synth',   name: 'Synth',      on: 'syOn',      ids: ['syShape', 'sySub', 'syCut', 'syEnv', 'syRes', 'syGlide', 'syMix'], movable: true, scope: 'synth', blurb: 'Tracked synth voice' },
    { key: 'phaser',  name: 'Phaser',     on: 'phOn',      ids: ['phRate', 'phDepth', 'phFb', 'phStages'], movable: true, scope: 'phaser', blurb: '4 to 8 stages' },
    { key: 'chorus',  name: 'Chorus',     on: 'chOn',      ids: ['chRate', 'chDepth', 'chVoices', 'chTone', 'chLows', 'chMix'], movable: true, scope: 'chorus', blurb: 'Tri-chorus, keeps the lows' },
    { key: 'delay',   name: 'Delay',      on: 'dlOn',      ids: ['dlTime', 'dlFb', 'dlMix', 'dlHicut', 'dlModRate', 'dlModDep', 'dlSat', 'dlSmear'], movable: true, scope: 'delay', blurb: 'Tape-wobble analogue echo' },
    { key: 'tape',    name: 'Tape',       on: 'tpOn',      ids: ['tpDrive', 'tpSoften', 'tpBump', 'tpMix'], movable: true, scope: 'tape', blurb: 'Warmth, highs saturate first' },
    { key: 'freeze',  name: 'Freeze',     on: 'frzOn',     ids: ['frzLevel', 'frzIn', 'frzOut', 'frzMotion'], movable: true, scope: 'freeze', kind: 'hold', blurb: 'Sustain pedal for anything' },
    { key: 'reverb',  name: 'Reverb',     on: 'rvOn',      ids: ['rvSize', 'rvDamp', 'rvPre', 'rvMod', 'rvMix'], scope: 'reverb', blurb: 'Plate, mono in, stereo out' },
    { key: 'out',     name: 'Live out',   on: 'liveMute',  ids: ['liveOut'], kind: 'mute' },
    { key: 'master',  name: 'Master',     on: 'masterMute', ids: ['masterGain'], kind: 'mute' },
].map ((d) => ({ ...d, ids: d.ids.filter ((id) => SPEC[id]) }));
export const FXMAP = Object.fromEntries (FX.map ((d) => [d.key, d]));
export const MOVABLE_KEYS = FX.filter ((d) => d.movable).map ((d) => d.key);
export const LANES = { vocal: 'mic', vocoder: 'mic', midi: 'bass', keys: 'keys' };
export const isLoadable = (d) => d.movable || d.key === 'reverb' || !! LANES[d.key];

// A row's one-line summary under its name, so the board reads without opening anything.
const SUMMARY = {
    input: ['inGain'], vocal: ['tuneKey', 'tuneScale'], vocoder: ['vocoderLvl'], keys: ['instLevel'], midi: ['midiTrans', 'midiCh'],
    comp: ['compThr', 'compRatio'], eq: [], filter: ['fltFrom', 'fltTo'], boost: ['bstGain'], drive: ['odVoice', 'odDrive'],
    amp: ['ampIn'], cab: ['cabBumpF', 'cabTop'], octave: ['octInt', 'octMix'], sub: ['subDiv', 'subLevel'], synth: ['syShape', 'syMix'],
    phaser: ['phRate', 'phDepth'], chorus: ['chVoices', 'chMix'], delay: ['dlTime', 'dlFb'], tape: ['tpDrive', 'tpMix'],
    freeze: ['frzLevel', 'frzOut'], reverb: ['rvSize', 'rvMix'], out: ['liveOut'], master: ['masterGain'],
};
export function summary (key, get)
{
    if (key === 'eq')
    {
        const parts = [];
        for (let b = 1; b <= 5; ++b) { const g = get ('eq' + b + 'G'); if (Math.abs (g) >= 0.5) parts.push (fmt (SPEC['eq' + b + 'F'], get ('eq' + b + 'F')) + ' ' + fmt (SPEC['eq1G'], g)); }
        return parts.length ? parts.join (' · ') : 'Flat';
    }
    if (key === 'chorus') return get ('chVoices') + ' voice' + (get ('chVoices') > 1 ? 's' : '') + ' · mix ' + fmt (SPEC.chMix, get ('chMix'));
    return (SUMMARY[key] || []).map ((id) => (SPEC[id].choices ? '' : SPEC[id].label + ' ') + fmt (SPEC[id], get (id))).join (' · ');
}

// ---------- strips ----------
export const STRIP_KINDS = {
    bass: { slot: 0, name: 'Bass' },
    mic:  { slot: 1, name: 'Mic' },
    keys: { slot: 2, name: 'Keys' },
};

export function defaultState ()
{
    const values = Array.from ({ length: 8 }, () => ({}));
    for (const s of SPECS) for (let c = 0; c < 8; ++c) if (c === 0 || isChannel (s.id)) values[c][s.id] = s.def;
    return {
        version: 1,
        values,
        strips: [{ key: 'bass', kind: 'bass', slot: 0, name: 'Bass', source: 0, board: ['comp', 'eq', 'drive', 'octave', 'chorus', 'delay', 'reverb'] }],
        selected: 'bass',
        tab: 'board',
        tunerMode: 'bass',
        patch: 'Warm pad',
        loopStrip: 'bass',
        chainPresets: [],                    // user-saved: { name, kind, values, order }
        effectPresets: [],                   // user-saved: { effect, name, values }
        foot: { device: '', active: 'Default', controls: [], layouts: [{ name: 'Default', jobs: [] }] },
        audio: { inputId: '', midiOut: '', keysIn: '' },
        irs: {},                             // slot -> { name, data: number[] }
        wake: true,
    };
}

const STORE_KEY = 'liverig.v1';
export function loadState ()
{
    const d = defaultState ();
    try
    {
        const raw = localStorage.getItem (STORE_KEY);
        if (! raw) return d;
        const s = JSON.parse (raw);
        const out = { ...d, ...s, audio: { ...d.audio, ...(s.audio || {}) }, foot: s.foot || d.foot };
        // fill any parameter a newer build added
        out.values = d.values.map ((def, c) => ({ ...def, ...((s.values || [])[c] || {}) }));
        for (const c of out.values) c.frzOn = 0;   // a gesture, never restored
        if (! out.strips?.length) out.strips = d.strips;
        return out;
    }
    catch { return d; }
}
export function saveState (state)
{
    try
    {
        const copy = { ...state, values: state.values.map ((c) => { const o = { ...c }; delete o.frzOn; return o; }) };
        localStorage.setItem (STORE_KEY, JSON.stringify (copy));
    }
    catch { /* storage full or blocked: the rig still plays */ }
}

// ---------- presets ----------
export const DEFAULT_ORDER = 'comp,eq,filter,boost,drive,amp,octave,synth,phaser,chorus,delay,tape'.split (',');

export function chainPresetsFor (state, kind)
{
    const user = state.chainPresets.filter ((p) => p.kind === kind || (! p.kind && kind === 'bass'));
    const factory = kind === 'mic' || kind === 'keys' ? [] : FACTORY_CHAIN_PRESETS.map ((f) => ({ ...f, factory: true }));
    const names = new Set (user.map ((p) => p.name));
    return factory.filter ((f) => ! names.has (f.name)).concat (user);
}

// What a chain preset holds for a factory sound: every channel knob at its
// default, then the preset's own settings over the top.
export function chainPresetValues (preset)
{
    if (! preset.factory) return preset.values;
    const v = {};
    for (const id of CHANNEL_IDS) if (! isMomentary (id)) v[id] = SPEC[id].def;
    return { ...v, ...preset.values };
}

export function effectPresetsFor (state, effect)
{
    const user = state.effectPresets.filter ((p) => p.effect === effect);
    const names = new Set (user.map ((p) => p.name));
    return FACTORY_EFFECT_PRESETS.filter ((p) => p.effect === effect && ! names.has (p.name)).map ((p) => ({ ...p, factory: true })).concat (user);
}

export { FACTORY_EFFECT_PRESETS, FACTORY_CHAIN_PRESETS, KEYS };
