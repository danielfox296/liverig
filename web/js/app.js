// LiveRig for the phone: the window. Audio runs in an AudioWorklet (engine.js);
// this file owns state, MIDI, the foot controller and every screen.

import {
    SPECS, SPEC, FX, FXMAP, MOVABLE_KEYS, LANES, KEYS, isLoadable, isChannel, isMomentary, CHANNEL_IDS,
    toNorm, fromNorm, fmt, summary, loadState, saveState, defaultState, chainPresetsFor, chainPresetValues,
    effectPresetsFor, DEFAULT_ORDER,
} from './model.js';
import { FootEngine, factoryFoot, describeTrigger, KIND_NAMES, VERB_NAMES, parseAction } from './foot.js';
import { Biquad } from './dsp.js';
import { PATCHES } from './engine.js';

// ======================================================================
// state
// ======================================================================
const state = loadState ();
if (! state.foot || ! state.foot.layouts) state.foot = factoryFoot ();
if (! localStorage.getItem ('liverig.v1')) state.foot = factoryFoot ();

let ctx = null, node = null, stream = null, source = null, midi = null, wakeLock = null;
let inputChannels = 1;
let latest = null, latestAt = 0;          // last meters message from the engine
let saveTimer = 0;

const stripByKey = (k) => state.strips.find ((s) => s.key === k);
const selected = () => (state.selected === 'master' ? null : stripByKey (state.selected) || state.strips[0]);
const slotOf = (key) => (key === 'rig' || key === 'master' ? 0 : stripByKey (key)?.slot ?? -1);
const getP = (slot, id) => state.values[isChannel (id) ? slot : 0][id];

function scheduleSave ()
{
    clearTimeout (saveTimer);
    saveTimer = setTimeout (() => saveState (state), 300);
}

// Every on-screen control registers here, so a change from anywhere (a foot,
// a preset, another control) shows up wherever that value is drawn.
const bindings = new Set ();
function bind (slot, id, fn, scope)
{
    const b = { slot: isChannel (id) ? slot : 0, id, fn, scope };
    bindings.add (b);
    return b;
}
function dropScope (scope) { for (const b of bindings) if (b.scope === scope) bindings.delete (b); }

function setP (slot, id, v, { post = true, silent = false } = {})
{
    const s = isChannel (id) ? slot : 0;
    state.values[s][id] = v;
    if (post) node?.port.postMessage ({ t: 'p', c: s, id, v });
    if (! silent) for (const b of bindings) if (b.id === id && b.slot === s) b.fn (v);
    if (! isMomentary (id)) scheduleSave ();
}

function postStrips ()
{
    node?.port.postMessage ({ t: 'strips', strips: state.strips.map ((s) => ({ slot: s.slot, kind: s.kind, source: s.source, board: s.board })) });
    scheduleSave ();
}

// ======================================================================
// tiny DOM helpers
// ======================================================================
const $ = (s, r = document) => r.querySelector (s);
function h (tag, attrs = {}, ...kids)
{
    const el = document.createElement (tag);
    for (const [k, v] of Object.entries (attrs || {}))
    {
        if (v == null || v === false) continue;
        if (k === 'class') el.className = v;
        else if (k.startsWith ('on')) el.addEventListener (k.slice (2), v);
        else if (k === 'style' && typeof v === 'object') Object.assign (el.style, v);
        else if (k === 'html') el.innerHTML = v;
        else el.setAttribute (k, v === true ? '' : v);
    }
    for (const kid of kids.flat ()) if (kid != null && kid !== false) el.append (kid.nodeType ? kid : document.createTextNode (kid));
    return el;
}
function toast (msg, ms = 2200)
{
    const t = $('#toast');
    t.textContent = msg; t.hidden = false;
    clearTimeout (toast.timer);
    toast.timer = setTimeout (() => (t.hidden = true), ms);
}
const vibrate = (ms = 8) => { try { navigator.vibrate?.(ms); } catch {} };
function ask (title, value = '')
{
    const r = prompt (title, value);
    return r == null ? null : r.trim ();
}

// ======================================================================
// controls
// ======================================================================

// A horizontal fader: a wide target you can shove with one thumb. Drag
// sideways moves it relative to where it was (fine control); a quick tap
// jumps to the spot; a double tap restores the default. Vertical drags
// scroll the page instead.
function fader ({ spec, get, set, label, scope, bindTo })
{
    const fill = h ('i', { class: 'fill' + (spec.min < 0 && spec.max > 0 ? ' bi' : '') });
    const cap = h ('i', { class: 'cap' });
    const val = h ('span', { class: 'val' });
    const el = h ('div', { class: 'fader', role: 'slider', 'aria-label': label || spec.label, tabindex: 0 }, fill, cap, h ('span', { class: 'lab' }, label || spec.label), val);
    const draw = (v) =>
    {
        const n = toNorm (spec, v);
        if (spec.min < 0 && spec.max > 0)
        {
            const z = toNorm (spec, 0);
            fill.style.left = Math.min (n, z) * 100 + '%';
            fill.style.width = Math.abs (n - z) * 100 + '%';
        }
        else { fill.style.left = '0'; fill.style.width = n * 100 + '%'; }
        cap.style.left = n * 100 + '%';
        val.textContent = fmt (spec, v);
        el.setAttribute ('aria-valuetext', fmt (spec, v));
    };
    draw (get ());
    if (bindTo) bind (bindTo.slot, bindTo.id, draw, scope);

    let start = null, lastTap = 0;
    el.addEventListener ('pointerdown', (e) =>
    {
        start = { x: e.clientX, y: e.clientY, n: toNorm (spec, get ()), id: e.pointerId, t: performance.now (), mode: null, w: el.clientWidth };
    });
    el.addEventListener ('pointermove', (e) =>
    {
        if (! start || e.pointerId !== start.id) return;
        const dx = e.clientX - start.x, dy = e.clientY - start.y;
        if (! start.mode)
        {
            if (Math.abs (dx) > 6 && Math.abs (dx) > Math.abs (dy)) { start.mode = 'drag'; el.setPointerCapture (e.pointerId); el.classList.add ('active'); }
            else if (Math.abs (dy) > 8) { start = null; return; }
        }
        if (start.mode === 'drag')
        {
            e.preventDefault ();
            const v = fromNorm (spec, start.n + dx / start.w);
            if (v !== get ()) { set (v); draw (v); }
        }
    });
    const end = (e) =>
    {
        if (! start || e.pointerId !== start.id) return;
        if (! start.mode && performance.now () - start.t < 350)
        {
            const now = performance.now ();
            if (now - lastTap < 320) { set (spec.def); draw (spec.def); vibrate (); lastTap = 0; }
            else
            {
                lastTap = now;
                const r = el.getBoundingClientRect ();
                const v = fromNorm (spec, (e.clientX - r.left) / r.width);
                set (v); draw (v);
            }
        }
        el.classList.remove ('active');
        start = null;
    };
    el.addEventListener ('pointerup', end);
    el.addEventListener ('pointercancel', () => { el.classList.remove ('active'); start = null; });
    el.addEventListener ('keydown', (e) =>
    {
        const d = e.key === 'ArrowRight' || e.key === 'ArrowUp' ? 1 : e.key === 'ArrowLeft' || e.key === 'ArrowDown' ? -1 : 0;
        if (! d) return;
        e.preventDefault ();
        const v = fromNorm (spec, toNorm (spec, get ()) + d * (spec.step ? 0 : 0.01)) + (spec.step ? d * spec.step : 0);
        const c = Math.min (spec.max, Math.max (spec.min, v));
        set (c); draw (c);
    });
    return el;
}

function segmented ({ options, get, set, label, scope, bindTo })
{
    const wrap = h ('div', { class: 'seg-wrap' });
    if (label) wrap.append (h ('div', { class: 'seg-label' }, label));
    const seg = h ('div', { class: 'seg' });
    const btns = options.map (([v, text]) => h ('button', { onclick: () => { set (v); draw (v); vibrate (); } }, text));
    const draw = (cur) => btns.forEach ((b, i) => b.classList.toggle ('sel', options[i][0] === Math.round (cur)));
    seg.append (...btns);
    wrap.append (seg);
    draw (get ());
    if (bindTo) bind (bindTo.slot, bindTo.id, draw, scope);
    return wrap;
}

// The editor's control for one parameter on one strip.
function paramControl (slot, id, scope)
{
    const s = SPEC[id];
    const get = () => getP (slot, id), set = (v) => setP (slot, id, v);
    if (s.choices || s.toggle || (s.step >= 1 && s.max - s.min <= 4 && ! s.suffix))
    {
        const opts = [];
        for (let v = s.min; v <= s.max; v += s.step || 1) opts.push ([v, s.choices ? s.choices[Math.round (v - s.min)] : s.toggle ? (v ? 'On' : 'Off') : String (v)]);
        const short = opts.every (([, t]) => t.length <= 6);
        if (opts.length <= 3 || (opts.length <= 5 && short)) return segmented ({ options: opts, get, set, label: s.label, scope, bindTo: { slot, id } });
        const sel = h ('select', { class: 'select', onchange: (e) => set (Number (e.target.value)) }, opts.map (([v, t]) => h ('option', { value: v }, t)));
        sel.value = String (get ());
        bind (slot, id, (v) => (sel.value = String (v)), scope);
        return h ('div', { class: 'seg-wrap' }, h ('div', { class: 'seg-label' }, s.label), sel);
    }
    return fader ({ spec: s, get, set, scope, bindTo: { slot, id } });
}

function switchButton (slot, def, scope, extraClass = '')
{
    const kind = def.kind || 'on';
    const text = kind === 'mute' ? 'MUTE' : kind === 'hold' ? 'HOLD' : 'ON';
    const btn = h ('button', { class: 'sw ' + (kind === 'mute' ? 'mute ' : kind === 'hold' ? 'hold ' : '') + extraClass, 'aria-label': def.name + ' ' + text },
                   h ('span', {}, text));
    const draw = (v) => { btn.classList.toggle ('on', v > 0.5); btn.setAttribute ('aria-pressed', v > 0.5); };
    btn.addEventListener ('click', (e) =>
    {
        e.stopPropagation ();
        const v = getP (slot, def.on) > 0.5 ? 0 : 1;
        setP (slot, def.on, v);
        vibrate (12);
    });
    draw (getP (slot, def.on));
    bind (slot, def.on, draw, scope);
    return btn;
}

// ======================================================================
// the rail
// ======================================================================
function renderRail ()
{
    const rail = $('#rail');
    rail.textContent = '';
    for (const s of state.strips)
    {
        const mini = h ('i');
        const chip = h ('button', { class: 'chip' + (state.selected === s.key ? ' sel' : ''), onclick: () => selectStrip (s.key) },
                        h ('span', { class: 'mini' }, mini), s.name);
        chip.dataset.slot = s.slot;
        chip._meter = mini;
        rail.append (chip);
    }
    rail.append (h ('button', { class: 'chip add', 'aria-label': 'Add a strip', onclick: openAddStrip }, '+'));
    $('#masterChip').classList.toggle ('sel', state.selected === 'master');
}

function selectStrip (key)
{
    state.selected = key;
    if (key !== 'master') state.loopStrip = key;
    renderRail ();
    renderBoard ();
    if (state.tab === 'tune') sendTuner ();
    if (state.tab === 'loop') renderLoop ();
    scheduleSave ();
}

function openAddStrip ()
{
    const opts = [];
    if (! stripByKey ('mic')) opts.push (['mic', 'Mic', 'Vocal strip: autotune and vocoder lanes']);
    if (! stripByKey ('keys')) opts.push (['keys', 'Keys', 'A MIDI keyboard and the tracked notes, into a built-in synth']);
    const free = [3, 4, 5, 6, 7].find ((s) => ! state.strips.some ((x) => x.slot === s));
    if (free != null) opts.push (['generic', 'Channel', 'Another input jack through its own board']);
    openSheet ('Add a strip', null, h ('div', { class: 'grid-pick' }, opts.map (([kind, name, blurb]) =>
        h ('button', { onclick: () =>
        {
            let s;
            if (kind === 'mic') s = { key: 'mic', kind, slot: 1, name: 'Mic', source: inputChannels > 1 ? 1 : 0, board: ['vocal', 'vocoder'] };
            else if (kind === 'keys') s = { key: 'keys', kind, slot: 2, name: 'Keys', source: -1, board: ['keys'] };
            else s = { key: 'ch' + (free + 1), kind: 'generic', slot: free, name: 'Ch ' + (free + 1), source: 0, board: [] };
            state.strips.push (s);
            state.strips.sort ((a, b) => a.slot - b.slot);
            if (kind === 'keys') setP (0, 'instOn', 1);
            postStrips (); closeSheet (); selectStrip (s.key);
        } }, h ('b', {}, name), h ('small', {}, blurb)))));
}

// ======================================================================
// the board
// ======================================================================
function visibleRows (strip)
{
    if (! strip) return ['master'];
    const rows = ['input'];
    for (const lane of ['vocal', 'vocoder', 'midi', 'keys']) if (LANES[lane] === strip.kind && strip.board.includes (lane)) rows.push (lane);
    for (const k of strip.board) if (FXMAP[k]?.movable) rows.push (k);
    if (strip.board.includes ('reverb')) rows.push ('reverb');
    rows.push ('out');
    return rows;
}
function availableFx (strip)
{
    return FX.filter ((d) => isLoadable (d) && ! strip.board.includes (d.key) && (! LANES[d.key] || LANES[d.key] === strip.kind));
}

function renderBoard ()
{
    const view = $('#view-board');
    dropScope ('board');
    view.textContent = '';
    const strip = selected ();
    const slot = strip ? strip.slot : 0;

    const head = h ('div', { class: 'board-head' },
        h ('div', { class: 'strip-name' }, strip ? strip.name : 'Master'),
        strip ? h ('button', { class: 'btn small', onclick: () => openStripMenu (strip) }, 'Strip ⋯') : null);
    view.append (head);

    if (strip)
    {
        const presets = chainPresetsFor (state, presetKind (strip));
        const sel = h ('select', { class: 'select', 'aria-label': 'Chain preset', onchange: (e) =>
        {
            const p = presets.find ((x) => x.name === e.target.value);
            if (p) { recallChainPreset (strip, p); toast ('Recalled ' + p.name); }
        } }, h ('option', { value: '' }, presets.length ? 'Chain preset…' : 'No chain presets yet'), presets.map ((p) => h ('option', { value: p.name }, p.name + (p.factory ? '' : ' •'))));
        view.append (h ('div', { class: 'preset-bar' }, sel,
            h ('button', { class: 'btn', onclick: () => saveChainPreset (strip) }, 'Save'),
            h ('button', { class: 'btn', 'aria-label': 'Delete a chain preset', onclick: () => deleteChainPreset (strip) }, 'Del')));
    }

    const list = h ('div', { class: 'rows' });
    const rows = visibleRows (strip);
    let n = 0;
    for (const key of rows)
    {
        const def = FXMAP[key];
        const movable = !! def.movable;
        const num = movable ? ++n : null;
        const sum = h ('div', { class: 'sum' });
        const drawSum = () => (sum.textContent = key === 'amp' ? (state.irs[slot]?.name || 'No capture — a wire') + ' · ' + summary (key, (id) => getP (slot, id))
                                                                : summary (key, (id) => getP (slot, id)));
        drawSum ();
        for (const id of def.ids.concat (def.style || [])) bind (slot, id, drawSum, 'board');
        const row = h ('div', { class: 'row', 'data-key': key });
        const drawOn = (v) => row.classList.toggle ('on', def.kind !== 'mute' && v > 0.5);
        drawOn (getP (slot, def.on));
        bind (slot, def.on, drawOn, 'board');
        const grip = movable
            ? h ('div', { class: 'grip', 'aria-label': 'Drag to reorder', html: '<svg viewBox="0 0 16 16"><circle cx="5" cy="3" r="1.5"/><circle cx="11" cy="3" r="1.5"/><circle cx="5" cy="8" r="1.5"/><circle cx="11" cy="8" r="1.5"/><circle cx="5" cy="13" r="1.5"/><circle cx="11" cy="13" r="1.5"/></svg>' })
            : h ('div', { class: 'grip none' });
        if (movable) attachDrag (grip, row, list, strip);
        const extra = h ('div', { class: 'extra' });
        if (key === 'midi')
        {
            const note = h ('span', { class: 'note-read' });
            row._note = note;
            extra.append (note, h ('button', { class: 'panic', onclick: (e) => { e.stopPropagation (); panic (); toast ('All notes off'); } }, 'PANIC'));
        }
        row.append (grip,
            h ('button', { class: 'main', onclick: () => openEditor (strip, key) },
                h ('div', { class: 'title' }, num ? h ('span', { class: 'num' }, num) : null, def.name), sum),
            extra,
            switchButton (slot, def, 'board'));
        list.append (row);
    }
    view.append (list);

    if (strip && availableFx (strip).length)
        view.append (h ('div', { style: { marginTop: '10px' } }, h ('button', { class: 'add-row', onclick: () => openAddFx (strip) }, '+ Add effect')));

    if (! strip)
    {
        view.append (h ('div', { class: 'section-label' }, 'Master'));
        view.append (h ('p', { class: 'fine' }, 'One gain over the whole mix, feeding an always-on safety limiter. When LIM shows by the master meter it is catching that many dB. Flickers on peaks are healthy; a constant large number means pull Master down.'));
        view.append (fader ({ spec: SPEC.loopLevel, get: () => getP (0, 'loopLevel'), set: (v) => setP (0, 'loopLevel', v), scope: 'board', bindTo: { slot: 0, id: 'loopLevel' } }));
    }

    view.append (h ('div', { class: 'status-line', id: 'statusLine' }));
    drawStatus ();
}

function drawStatus ()
{
    const el = $('#statusLine');
    if (! el) return;
    if (! ctx) { el.textContent = 'Audio is off — tap the dot top-left to start.'; return; }
    const io = ((ctx.baseLatency || 0) + (ctx.outputLatency || 0) + (inputLatency () || 0)) * 1000;
    const fx = latest ? latest.fxLatency / ctx.sampleRate * 1000 : 0;
    el.textContent = '';
    el.append (h ('span', {}, (ctx.sampleRate / 1000).toFixed (1) + ' kHz'), h ('span', {}, 'I/O ' + io.toFixed (1) + ' ms'),
               h ('span', {}, 'FX ' + fx.toFixed (1) + ' ms'), h ('span', {}, inputChannels + (inputChannels === 1 ? ' input' : ' inputs')));
}

// Reorder by dragging a row's grip. Order applies live as the row drops.
function attachDrag (grip, row, list, strip)
{
    grip.addEventListener ('pointerdown', (e) =>
    {
        e.preventDefault ();
        grip.setPointerCapture (e.pointerId);
        const rows = [...list.querySelectorAll ('.row')].filter ((r) => FXMAP[r.dataset.key].movable);
        const rects = rows.map ((r) => r.getBoundingClientRect ());
        const from = rows.indexOf (row);
        const startY = e.clientY, pitch = rects.length > 1 ? rects[1].top - rects[0].top : rects[0].height + 8;
        let to = from;
        row.classList.add ('dragging');
        vibrate (10);
        const move = (ev) =>
        {
            const dy = ev.clientY - startY;
            row.style.transform = `translateY(${dy}px)`;
            to = Math.max (0, Math.min (rows.length - 1, from + Math.round (dy / pitch)));
            rows.forEach ((r, i) =>
            {
                if (r === row) return;
                let shift = 0;
                if (from < to && i > from && i <= to) shift = -pitch;
                if (from > to && i < from && i >= to) shift = pitch;
                r.style.transform = shift ? `translateY(${shift}px)` : '';
            });
        };
        const up = () =>
        {
            grip.removeEventListener ('pointermove', move);
            grip.removeEventListener ('pointerup', up);
            grip.removeEventListener ('pointercancel', up);
            if (to !== from)
            {
                const movable = strip.board.filter ((k) => FXMAP[k]?.movable);
                const [k] = movable.splice (from, 1);
                movable.splice (to, 0, k);
                const others = strip.board.filter ((k) => ! FXMAP[k]?.movable);
                strip.board = others.concat (movable);
                postStrips ();
                vibrate (10);
            }
            renderBoard ();
        };
        grip.addEventListener ('pointermove', move);
        grip.addEventListener ('pointerup', up);
        grip.addEventListener ('pointercancel', up);
    });
}

function openAddFx (strip)
{
    openSheet ('Add effect', null, h ('div', { class: 'grid-pick' }, availableFx (strip).map ((d) =>
        h ('button', { onclick: () => { addFx (strip, d.key); closeSheet (); } }, h ('b', {}, d.name), h ('small', {}, d.blurb || laneBlurb (d.key))))));
}
const laneBlurb = (k) => ({ vocal: 'Pitch-corrected mic', vocoder: 'The mic makes the bass sing', midi: 'The bass as a note source', keys: 'Built-in synth for keys and tracked notes' }[k] || '');

function addFx (strip, key)
{
    if (! strip.board.includes (key)) strip.board.push (key);
    const def = FXMAP[key];
    // A pedal joins the board lit; the freeze joins dark, its switch is the hold.
    if (! isMomentary (def.on)) setP (strip.slot, def.on, 1);
    postStrips ();
    renderBoard ();
}
function removeFx (strip, key)
{
    strip.board = strip.board.filter ((k) => k !== key);
    const def = FXMAP[key];
    setP (strip.slot, def.on, 0);
    postStrips ();
    renderBoard ();
}

function openStripMenu (strip)
{
    const items = [
        ['Rename', () => { const n = ask ('Strip name', strip.name); if (n) { strip.name = n; renderRail (); renderBoard (); scheduleSave (); } closeSheet (); }],
    ];
    if (strip.kind !== 'bass') items.push (['Remove strip', () =>
    {
        if (! confirm ('Remove the ' + strip.name + ' strip?')) return;
        if (strip.kind === 'mic') { setP (0, 'vocalOn', 0); setP (0, 'vocoderOn', 0); }
        if (strip.kind === 'keys') setP (0, 'instOn', 0);
        state.strips = state.strips.filter ((s) => s !== strip);
        if (state.loopStrip === strip.key) state.loopStrip = 'bass';
        postStrips (); closeSheet (); selectStrip ('bass');
    }, 'danger']);
    openSheet (strip.name, null, h ('div', { class: 'menu' }, items.map (([t, fn, cls]) => h ('button', { class: cls || '', onclick: fn, style: cls ? { color: 'var(--red)' } : null }, t))));
}

// ---------- chain presets ----------
const presetKind = (strip) => (strip.kind === 'generic' ? 'bass' : strip.kind);
const channelIdsFor = () => [...CHANNEL_IDS].filter ((id) => ! isMomentary (id));

function recallChainPreset (strip, preset)
{
    const vals = chainPresetValues (preset);
    const slot = strip.slot;
    for (const id of channelIdsFor ()) if (id in vals) setP (slot, id, vals[id], { post: false });
    // pedals the preset switches on join the board
    for (const k of MOVABLE_KEYS.concat ('reverb')) if (vals[FXMAP[k].on] > 0.5 && ! strip.board.includes (k)) strip.board.push (k);
    const order = preset.order || (preset.factory ? DEFAULT_ORDER : null);
    if (order)
    {
        const movable = strip.board.filter ((k) => FXMAP[k]?.movable);
        const idx = (k) => { const i = order.indexOf (k); return i < 0 ? 100 + movable.indexOf (k) : i; };
        movable.sort ((a, b) => idx (a) - idx (b));
        strip.board = strip.board.filter ((k) => ! FXMAP[k]?.movable).concat (movable);
    }
    const values = []; values[slot] = Object.fromEntries (channelIdsFor ().map ((id) => [id, getP (slot, id)]));
    node?.port.postMessage ({ t: 'values', values });
    postStrips ();
    renderBoard ();
}
function saveChainPreset (strip)
{
    const name = ask ('Name this chain preset');
    if (! name) return;
    const values = Object.fromEntries (channelIdsFor ().map ((id) => [id, getP (strip.slot, id)]));
    const order = strip.board.filter ((k) => FXMAP[k]?.movable);
    state.chainPresets = state.chainPresets.filter ((p) => ! (p.name === name && p.kind === presetKind (strip)));
    state.chainPresets.push ({ name, kind: presetKind (strip), values, order });
    scheduleSave (); renderBoard (); toast ('Saved ' + name);
}
function deleteChainPreset (strip)
{
    const mine = state.chainPresets.filter ((p) => p.kind === presetKind (strip));
    if (! mine.length) { toast ('Only presets you saved can be deleted'); return; }
    openSheet ('Delete a chain preset', null, h ('div', { class: 'menu' }, mine.map ((p) => h ('button', { onclick: () =>
    {
        state.chainPresets = state.chainPresets.filter ((x) => x !== p);
        scheduleSave (); closeSheet (); renderBoard (); toast ('Deleted ' + p.name);
    } }, p.name, h ('span', { class: 'r' }, 'Delete')))));
}

// ======================================================================
// the editor sheet
// ======================================================================
function openEditor (strip, key)
{
    const def = FXMAP[key];
    const slot = strip ? strip.slot : 0;
    const scope = 'sheet';
    const body = h ('div', { class: 'stack' });

    if (def.scope && def.key !== 'keys') body.append (effectPresetRow (slot, def));

    // extras above the faders, per row
    if (key === 'input' && strip) body.append (sourcePicker (strip));
    if (key === 'amp') body.append (ampPicker (strip));
    if (key === 'midi') body.append (midiOutPicker ());
    if (key === 'keys') body.append (patchPicker (), keysInPicker ());
    if (key === 'vocal' || key === 'vocoder') body.append (h ('p', { class: 'fine' }, key === 'vocal'
        ? 'The mic\'s own pitch tracker steers it onto the nearest note of the key below. Gain and gate are shared with the vocoder.'
        : 'The mic is the modulator; the carrier is the bass after its whole board. Drive the board brighter if the words come out muddy.'));

    if (key === 'eq') body.append (eqEditor (slot, scope));
    else for (const id of def.ids) body.append (paramControl (slot, id, scope));

    if (def.style)
    {
        body.append (h ('div', { class: 'section-label' }, key === 'vocal' ? 'Key & style' : 'Voicing'));
        for (const id of def.style) body.append (paramControl (0, id, scope));
    }
    if (key === 'freeze') body.append (h ('p', { class: 'fine' }, 'HOLD keeps what was sounding as you pressed it, for as long as it is lit. Pedals above Freeze are in the held sound; pedals below colour both. Give HOLD to a foot switch in the Foot tab.'));
    if (key === 'midi') body.append (h ('p', { class: 'fine' }, 'Aimed at swells and pads. Expression rides CC11 or CC1 while a note sustains. With Live thru off the bass leaves the mix and only the pad speaks.'));
    if (key === 'out' && strip) body.append (h ('p', { class: 'fine' }, 'This strip\'s level into the mix.'));

    if (strip && isLoadable (def))
        body.append (h ('button', { class: 'btn danger', style: { width: '100%', marginTop: '18px' }, onclick: () => { closeSheet (); removeFx (strip, key); } }, 'Remove ' + def.name + ' from board'));

    openSheet (def.name, switchButton (slot, def, scope), body);
}

function effectPresetRow (slot, def)
{
    const list = effectPresetsFor (state, def.scope);
    const sel = h ('select', { class: 'select', 'aria-label': def.name + ' preset', onchange: (e) =>
    {
        const p = list.find ((x) => x.name === e.target.value);
        if (! p) return;
        for (const [id, v] of Object.entries (p.values)) if (SPEC[id] && id !== def.on) setP (slot, id, v);
        toast (def.name + ': ' + p.name);
    } }, h ('option', { value: '' }, list.length ? 'Preset…' : 'No presets yet'), list.map ((p) => h ('option', { value: p.name }, p.name + (p.factory ? '' : ' •'))));
    const save = h ('button', { class: 'btn', onclick: () =>
    {
        const name = ask ('Name this ' + def.name + ' preset');
        if (! name) return;
        const values = Object.fromEntries (def.ids.concat (def.style || []).map ((id) => [id, getP (slot, id)]));
        state.effectPresets = state.effectPresets.filter ((p) => ! (p.effect === def.scope && p.name === name));
        state.effectPresets.push ({ effect: def.scope, name, values });
        scheduleSave ();
        sel.append (h ('option', { value: name }, name + ' •'));
        toast ('Saved ' + name);
    } }, 'Save');
    const del = h ('button', { class: 'btn', onclick: () =>
    {
        const name = sel.value;
        const p = state.effectPresets.find ((x) => x.effect === def.scope && x.name === name);
        if (! p) { toast ('Pick one of your own presets (•) to delete'); return; }
        state.effectPresets = state.effectPresets.filter ((x) => x !== p);
        scheduleSave (); sel.querySelector (`option[value="${CSS.escape (name)}"]`)?.remove (); sel.value = ''; toast ('Deleted ' + name);
    } }, 'Del');
    return h ('div', { class: 'preset-bar' }, sel, save, del);
}

function sourcePicker (strip)
{
    if (strip.kind === 'keys') return keysInPicker ();
    const opts = [[0, 'Input 1'], [1, 'Input 2'], [-1, 'None']];
    return segmented ({ label: 'Listens to', options: opts, get: () => strip.source, set: (v) =>
    {
        strip.source = v;
        postStrips ();
        if (v === 1 && inputChannels < 2) toast ('This input device has one channel — Input 2 will be silent');
    } });
}

function patchPicker ()
{
    const sel = h ('select', { class: 'select', onchange: (e) => { state.patch = e.target.value; node?.port.postMessage ({ t: 'patch', name: state.patch }); scheduleSave (); } },
                   Object.keys (PATCHES).map ((n) => h ('option', { value: n }, n)));
    sel.value = state.patch;
    return h ('div', { class: 'seg-wrap' }, h ('div', { class: 'seg-label' }, 'Instrument'), sel);
}

function midiInputs () { return midi ? [...midi.inputs.values ()] : []; }
function midiOutputs () { return midi ? [...midi.outputs.values ()] : []; }

function keysInPicker ()
{
    if (! midi) return h ('p', { class: 'fine' }, 'Web MIDI is not available in this browser, so a hardware keyboard cannot play the keys here. The tracked bass notes still can (turn on MIDI on the Bass strip).');
    const sel = h ('select', { class: 'select', onchange: (e) => { state.audio.keysIn = e.target.value; scheduleSave (); } },
                   h ('option', { value: '' }, 'Any MIDI keyboard'), midiInputs ().map ((i) => h ('option', { value: i.id }, i.name)));
    sel.value = state.audio.keysIn;
    return h ('div', { class: 'seg-wrap' }, h ('div', { class: 'seg-label' }, 'Played from'), sel);
}

function midiOutPicker ()
{
    if (! midi) return h ('p', { class: 'fine' }, 'Web MIDI is not available in this browser, so notes cannot leave the phone. They can still play the built-in Keys instrument: add a Keys strip.');
    const sel = h ('select', { class: 'select', onchange: (e) => { state.audio.midiOut = e.target.value; scheduleSave (); } },
                   h ('option', { value: '' }, 'No MIDI output'), midiOutputs ().map ((o) => h ('option', { value: o.id }, o.name)));
    sel.value = state.audio.midiOut;
    return h ('div', { class: 'seg-wrap' }, h ('div', { class: 'seg-label' }, 'Sends notes to'), sel);
}

function ampPicker (strip)
{
    const slot = strip.slot;
    const name = h ('div', { class: 'grow', style: { fontWeight: 650 } }, state.irs[slot]?.name || 'No capture loaded — the stage is a wire');
    const file = h ('input', { type: 'file', accept: 'audio/*,.wav', hidden: true, onchange: async (e) =>
    {
        const f = e.target.files[0];
        if (! f) return;
        try
        {
            const ac = ctx || new OfflineAudioContext (1, 1, 48000);
            const buf = await ac.decodeAudioData (await f.arrayBuffer ());
            let data = buf.getChannelData (0).slice (0, 8192);
            let e2 = 0; for (const v of data) e2 += v * v;
            const g = e2 > 0 ? 1 / Math.sqrt (e2) : 1;
            data = Array.from (data, (v) => Math.round (v * g * 1e6) / 1e6);
            state.irs[slot] = { name: f.name.replace (/\.[^.]+$/, ''), data };
            node?.port.postMessage ({ t: 'ir', c: slot, data: Float32Array.from (data) });
            name.textContent = state.irs[slot].name;
            scheduleSave (); renderBoard (); toast ('Loaded ' + state.irs[slot].name);
        }
        catch { toast ('That file could not be read as audio'); }
    } });
    return h ('div', {},
        h ('div', { class: 'hrow' }, name,
            h ('button', { class: 'btn small', onclick: () => file.click () }, 'Import…'),
            h ('button', { class: 'btn small', onclick: () => { delete state.irs[slot]; node?.port.postMessage ({ t: 'ir', c: slot, data: null }); name.textContent = 'No capture loaded — the stage is a wire'; scheduleSave (); renderBoard (); } }, 'Clear')),
        file,
        h ('p', { class: 'fine' }, 'Load an amp or cab impulse response (.wav). Neural Amp Modeler .nam captures need the desktop app.'));
}

// ---------- EQ: the curve is the interface ----------
function eqEditor (slot, scope)
{
    const wrap = h ('div', { class: 'stack' });
    const canvas = h ('canvas', { class: 'eq-canvas' });
    let band = 2;
    const bandBtns = [1, 2, 3, 4, 5].map ((b) => h ('button', { onclick: () => { band = b; drawBands (); } }, ['LOW', 'MID 1', 'MID 2', 'MID 3', 'HIGH'][b - 1]));
    const bandRow = h ('div', { class: 'bands' }, bandBtns);
    const faders = h ('div', { class: 'stack' });
    const sr = 48000;
    const fToX = (f, w) => (Math.log (f / 20) / Math.log (1000)) * w;
    const xToF = (x, w) => 20 * Math.pow (1000, x / w);
    const gToY = (g, hgt) => hgt / 2 - (g / 18) * (hgt / 2);
    const yToG = (y, hgt) => ((hgt / 2 - y) / (hgt / 2)) * 18;
    const bq = [0, 1, 2, 3, 4].map (() => new Biquad ());
    function draw ()
    {
        const dpr = window.devicePixelRatio || 1, w = canvas.clientWidth, hh = canvas.clientHeight;
        if (! w) return;
        canvas.width = w * dpr; canvas.height = hh * dpr;
        const g = canvas.getContext ('2d');
        g.scale (dpr, dpr);
        g.clearRect (0, 0, w, hh);
        g.strokeStyle = '#26272d'; g.lineWidth = 1;
        for (const f of [50, 100, 200, 500, 1000, 2000, 5000, 10000]) { const x = fToX (f, w); g.beginPath (); g.moveTo (x, 0); g.lineTo (x, hh); g.stroke (); }
        for (const db of [-12, -6, 0, 6, 12]) { const y = gToY (db, hh); g.beginPath (); g.moveTo (0, y); g.lineTo (w, y); g.stroke (); }
        g.fillStyle = '#70737c'; g.font = '10px sans-serif';
        for (const [f, t] of [[100, '100'], [1000, '1k'], [10000, '10k']]) g.fillText (t, fToX (f, w) + 3, hh - 4);
        for (let b = 0; b < 5; ++b)
        {
            const n = b + 1, f = getP (slot, 'eq' + n + 'F'), gn = getP (slot, 'eq' + n + 'G'), q = getP (slot, 'eq' + n + 'Q');
            if (b === 0) bq[0].lowShelfQ (sr, f, q, gn); else if (b === 4) bq[4].highShelfQ (sr, f, q, gn); else bq[b].peak (sr, f, q, gn);
        }
        const on = getP (slot, 'eqOn') > 0.5;
        g.beginPath ();
        for (let x = 0; x <= w; x += 2)
        {
            const f = xToF (x, w);
            let db = 0; for (const b of bq) db += b.magnitudeDb (sr, f);
            const y = gToY (db, hh);
            x === 0 ? g.moveTo (x, y) : g.lineTo (x, y);
        }
        g.strokeStyle = on ? '#f0954a' : '#70737c'; g.lineWidth = 2.5; g.stroke ();
        g.lineTo (w, gToY (0, hh)); g.lineTo (0, gToY (0, hh)); g.closePath ();
        g.fillStyle = on ? 'rgba(216,123,58,.14)' : 'rgba(112,115,124,.1)'; g.fill ();
        for (let b = 1; b <= 5; ++b)
        {
            const x = fToX (getP (slot, 'eq' + b + 'F'), w), y = gToY (getP (slot, 'eq' + b + 'G'), hh);
            g.beginPath (); g.arc (x, y, b === band ? 11 : 8, 0, Math.PI * 2);
            g.fillStyle = b === band ? '#f0954a' : '#2a2c33'; g.fill ();
            g.strokeStyle = '#e8e8ea'; g.lineWidth = 1.5; g.stroke ();
            g.fillStyle = b === band ? '#17181c' : '#e8e8ea'; g.font = 'bold 10px sans-serif'; g.textAlign = 'center'; g.textBaseline = 'middle';
            g.fillText (String (b), x, y + 0.5); g.textAlign = 'start'; g.textBaseline = 'alphabetic';
        }
    }
    function drawBands ()
    {
        bandBtns.forEach ((b, i) => b.classList.toggle ('sel', i + 1 === band));
        dropScope ('eqband');
        faders.textContent = '';
        for (const p of ['F', 'G', 'Q'])
        {
            const id = 'eq' + band + p;
            faders.append (fader ({ spec: SPEC[id], get: () => getP (slot, id), set: (v) => setP (slot, id, v), label: ['LOW', 'MID 1', 'MID 2', 'MID 3', 'HIGH'][band - 1] + ' ' + SPEC[id].label.toLowerCase (), scope: 'eqband', bindTo: { slot, id } }));
        }
        draw ();
    }
    canvas.addEventListener ('pointerdown', (e) =>
    {
        const r = canvas.getBoundingClientRect ();
        const px = e.clientX - r.left, py = e.clientY - r.top;
        let best = band, bd = 1e9;
        for (let b = 1; b <= 5; ++b)
        {
            const d = Math.hypot (fToX (getP (slot, 'eq' + b + 'F'), r.width) - px, gToY (getP (slot, 'eq' + b + 'G'), r.height) - py);
            if (d < bd) { bd = d; best = b; }
        }
        if (best !== band) { band = best; drawBands (); }
        canvas.setPointerCapture (e.pointerId);
        const move = (ev) =>
        {
            const x = Math.min (r.width, Math.max (0, ev.clientX - r.left)), y = Math.min (r.height, Math.max (0, ev.clientY - r.top));
            const fs = SPEC['eq' + band + 'F'], gs = SPEC['eq' + band + 'G'];
            setP (slot, 'eq' + band + 'F', Math.min (fs.max, Math.max (fs.min, Math.round (xToF (x, r.width)))));
            setP (slot, 'eq' + band + 'G', Math.min (gs.max, Math.max (gs.min, Math.round (yToG (y, r.height) * 10) / 10)));
        };
        const up = () => { canvas.removeEventListener ('pointermove', move); canvas.removeEventListener ('pointerup', up); };
        canvas.addEventListener ('pointermove', move);
        canvas.addEventListener ('pointerup', up);
    });
    for (let b = 1; b <= 5; ++b) for (const p of ['F', 'G', 'Q']) bind (slot, 'eq' + b + p, () => draw (), scope);
    bind (slot, 'eqOn', () => draw (), scope);
    wrap.append (canvas, bandRow, faders);
    requestAnimationFrame (drawBands);
    return wrap;
}

// ======================================================================
// sheets
// ======================================================================
function openSheet (title, headRight, body, { onClose } = {})
{
    const sheet = $('#sheet');
    dropScope ('sheet'); dropScope ('eqband');
    sheet.textContent = '';
    sheet.append (h ('div', { class: 'sheet-grab' }),
        h ('div', { class: 'sheet-head' }, h ('h2', {}, title), headRight, h ('button', { class: 'close', 'aria-label': 'Close', onclick: closeSheet }, '×')),
        h ('div', { class: 'sheet-body' }, body));
    sheet.hidden = false; $('#scrim').hidden = false;
    sheet._onClose = onClose;
    // swipe the grab handle down to close
    const grab = sheet.querySelector ('.sheet-grab'), head = sheet.querySelector ('.sheet-head');
    for (const el of [grab, head])
    {
        let y0 = null;
        el.addEventListener ('pointerdown', (e) => { if (e.target.closest ('button')) return; y0 = e.clientY; });
        el.addEventListener ('pointerup', (e) => { if (y0 != null && e.clientY - y0 > 60) closeSheet (); y0 = null; });
    }
}
function closeSheet ()
{
    const sheet = $('#sheet');
    if (sheet.hidden) return;
    sheet.hidden = true; $('#scrim').hidden = true;
    dropScope ('sheet'); dropScope ('eqband');
    sheet._onClose?.();
    sheet.textContent = '';
}

// ======================================================================
// the looper
// ======================================================================
const LOOP_FUNCS = [
    ['do/loopTempo', 'TAP', 'Tap tempo'], ['do/loopBars', 'BARS', 'Bars'],
    ['do/loopRecord', 'REC', 'Record / overdub'], ['do/loopStop', 'PLAY', 'Play / pause'],
    ['do/loopUndo', 'UNDO', 'Undo'], ['do/loopClear', 'CLEAR', 'Clear'],
];
const loopUi = {};
function loopSourceStrip () { return stripByKey (state.loopStrip) || state.strips[0]; }
function footNumberFor (action)
{
    const jobs = footEngine.layout ()?.jobs || [];
    const job = jobs.find ((j) => j.action === action);
    if (! job) return '–';
    const c = state.foot.controls.find ((x) => x.id === job.control);
    const m = c?.name.match (/(\d+)\s*$/);
    return m ? m[1] : '•';
}
function renderLoop ()
{
    const view = $('#view-loop');
    dropScope ('loop');
    view.textContent = '';
    const stateEl = h ('div', { class: 'loop-state' }, 'READY');
    const sub = h ('div', { class: 'loop-sub' }, '');
    const beats = h ('div', { class: 'beats' }, [0, 1, 2, 3].map (() => h ('i')));
    const prog = h ('i');
    view.append (h ('div', { class: 'card loop-status' }, stateEl, sub, beats, h ('div', { class: 'progress' }, prog)));
    const src = loopSourceStrip ();
    view.append (h ('div', { class: 'loop-meta' },
        h ('button', { class: 'btn', onclick: () =>
        {
            const opts = state.strips;
            openSheet ('Record which strip?', null, h ('div', { class: 'menu' }, opts.map ((s) => h ('button', { onclick: () => { state.loopStrip = s.key; scheduleSave (); closeSheet (); renderLoop (); } }, s.name, h ('span', { class: 'r' }, s.key === state.loopStrip ? 'Selected' : '')))));
        } }, 'Records: ', h ('b', {}, src.name)),
        h ('span', { class: 'fine', style: { alignSelf: 'center' } }, 'After its pedals')));
    const pads = h ('div', { class: 'pads' });
    loopUi.pads = {};
    for (const [action, title] of LOOP_FUNCS)
    {
        const small = h ('small');
        const label = h ('b', {}, title);
        const pad = h ('button', { class: 'pad', onpointerdown: (e) => { e.preventDefault (); vibrate (15); runCommand (action, performance.now ()); } },
                       h ('span', { class: 'foot-no', title: 'Foot switch' }, footNumberFor (action)), label, small);
        if (action === 'do/loopTempo') pad.addEventListener ('contextmenu', (e) => { e.preventDefault (); setTempoPrompt (); });
        loopUi.pads[action] = { pad, small, label };
        pads.append (pad);
    }
    view.append (pads);
    view.append (h ('div', { style: { marginTop: '12px' } }, fader ({ spec: SPEC.loopLevel, get: () => getP (0, 'loopLevel'), set: (v) => setP (0, 'loopLevel', v), scope: 'loop', bindTo: { slot: 0, id: 'loopLevel' } })));
    view.append (h ('p', { class: 'fine' }, 'Tap the tempo, tap Bars for the length, then Record: four clicks, the take, and it loops itself. Record again during the take ends it on the nearest bar line. Long-press TAP to type a tempo.'));
    Object.assign (loopUi, { stateEl, sub, beats: [...beats.children], prog });
    drawLoop ();
}
function setTempoPrompt ()
{
    const v = Number (ask ('Tempo (BPM)', String (latest?.loop?.bpm || 120)));
    if (v >= 40 && v <= 240) node?.port.postMessage ({ t: 'loop', cmd: 'tempo', v });
}
const LOOP_LABEL = { empty: 'READY', countIn: 'COUNT IN', recording: 'RECORDING', playing: 'PLAYING', overdubbing: 'OVERDUB', paused: 'PAUSED' };
const LOOP_CLASS = { recording: 'rec', overdubbing: 'dub', playing: 'play', countIn: 'count' };
function drawLoop ()
{
    if (! loopUi.stateEl || state.tab !== 'loop') return;
    const L = latest?.loop || { state: 'empty', bpm: 120, bars: 4, pos: 0, beatLen: 0, loopLen: 0, hasLayer: false, undone: false };
    loopUi.stateEl.textContent = LOOP_LABEL[L.state];
    loopUi.stateEl.className = 'loop-state ' + (LOOP_CLASS[L.state] || '');
    let pos = L.pos;
    if (ctx && latest && (L.state !== 'empty' && L.state !== 'paused'))
    {
        pos += Math.min (0.1, ctx.currentTime - latest.time) * ctx.sampleRate - (ctx.outputLatency || 0) * ctx.sampleRate;
        if ((L.state === 'playing' || L.state === 'overdubbing') && L.loopLen > 0) pos = ((pos % L.loopLen) + L.loopLen) % L.loopLen;
    }
    const beat = L.beatLen > 0 ? Math.max (0, pos / L.beatLen) : 0;
    const b = Math.floor (beat);
    const live = L.state !== 'empty' && L.beatLen > 0;
    loopUi.beats.forEach ((el, i) => { el.className = live && b % 4 === i ? 'lit' + (i === 0 ? ' one' : '') : ''; });
    if (L.state === 'countIn') loopUi.sub.textContent = 'Count ' + (Math.min (3, b) + 1) + ' of 4 · ' + L.bpm + ' BPM';
    else if (L.state === 'empty') loopUi.sub.textContent = L.bpm + ' BPM · ' + L.bars + ' bar' + (L.bars > 1 ? 's' : '');
    else loopUi.sub.textContent = 'Bar ' + (Math.floor (b / 4) + 1) + ' of ' + L.bars + ' · beat ' + ((b % 4) + 1) + ' · ' + L.bpm + ' BPM';
    loopUi.prog.style.width = (L.loopLen > 0 && L.state !== 'countIn' ? Math.min (1, pos / L.loopLen) * 100 : 0) + '%';
    const P = loopUi.pads, empty = L.state === 'empty';
    P['do/loopTempo'].small.textContent = L.bpm + ' BPM';
    P['do/loopTempo'].pad.classList.toggle ('disabled', ! empty);
    P['do/loopBars'].small.textContent = L.bars + ' bar' + (L.bars > 1 ? 's' : '');
    P['do/loopBars'].pad.classList.toggle ('disabled', ! empty);
    const rec = P['do/loopRecord'];
    rec.label.textContent = { empty: 'REC', countIn: 'CANCEL', recording: 'END TAKE', playing: 'DUB', overdubbing: 'END DUB', paused: 'DUB' }[L.state];
    rec.small.textContent = empty ? 'record' : L.state === 'recording' ? 'on the nearest bar' : L.state === 'countIn' ? 'call it off' : 'add a layer';
    rec.pad.classList.toggle ('recording', L.state === 'recording' || L.state === 'overdubbing');
    rec.pad.classList.toggle ('rec', empty);
    const play = P['do/loopStop'];
    play.label.textContent = L.state === 'paused' ? 'PLAY' : 'PAUSE';
    play.small.textContent = L.state === 'paused' ? 'from the same spot' : empty ? '' : 'hold the loop';
    play.pad.classList.toggle ('disabled', empty);
    const undo = P['do/loopUndo'];
    undo.label.textContent = L.undone ? 'REDO' : 'UNDO';
    undo.small.textContent = L.hasLayer ? 'newest layer' : '';
    undo.pad.classList.toggle ('disabled', ! L.hasLayer && L.state !== 'overdubbing');
    P['do/loopClear'].pad.classList.toggle ('disabled', empty);
}

// ======================================================================
// the tuner (TunerFilter.h)
// ======================================================================
class TunerFilter
{
    constructor () { this.reset (); }
    reset () { this.ring = []; this.shown = 0; this.farRun = 0; this.locked = -1; this.hold = 0; }
    push (hz, conf, peak)
    {
        const sounding = hz > 20 && conf > 0.5 && peak > 0.004;
        if (sounding)
        {
            const m = 69 + 12 * Math.log2 (hz / 440);
            const fresh = this.hold <= 0, far = ! fresh && Math.abs (m - this.shown) > 1.5;
            if (far && this.farRun < 2) ++this.farRun;
            else
            {
                if (fresh || far) { this.ring = []; this.shown = m; }
                this.farRun = 0;
                this.ring.push (m); if (this.ring.length > 9) this.ring.shift ();
                const s = [...this.ring].sort ((a, b) => a - b);
                this.shown += 0.2 * (s[s.length >> 1] - this.shown);
            }
            if (this.locked < 0 || Math.abs (this.shown - this.locked) > 0.6) this.locked = Math.round (this.shown);
            this.hold = 12;
        }
        else if (this.hold > 0) { --this.hold; this.farRun = 0; }
        return { live: this.hold > 0, note: this.shown, locked: this.hold > 0 ? this.locked : -1 };
    }
}
const tuner = new TunerFilter ();
const tuneUi = {};
const NOTE = ['C', 'C', 'D', 'D', 'E', 'F', 'F', 'G', 'G', 'A', 'A', 'B'], SHARP = [0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0];
const STRINGS = { bass: [23, 28, 33, 38, 43], guitar: [40, 45, 50, 55, 59, 64] };
function renderTune ()
{
    const view = $('#view-tune');
    view.textContent = '';
    const note = h ('div', { class: 'note' }, '–');
    const cents = h ('div', { class: 'cents' }, 'Play a string');
    const needle = h ('i', { class: 'needle' });
    const strings = h ('div', { class: 'strings' });
    const strip = selected () || state.strips[0];
    view.append (h ('div', { class: 'card tuner' }, note, cents,
        h ('div', { class: 'needle-wrap' }, h ('div', { class: 'needle-scale' }), h ('div', { class: 'needle-zone' }), needle),
        h ('div', { class: 'ticks' }, h ('span', {}, '−50'), h ('span', {}, '−25'), h ('span', {}, '0'), h ('span', {}, '+25'), h ('span', {}, '+50')),
        strings));
    view.append (h ('div', { style: { marginTop: '14px' } }, segmented ({ options: [[0, 'BASS'], [1, 'GUITAR']], get: () => (state.tunerMode === 'guitar' ? 1 : 0),
        set: (v) => { state.tunerMode = v ? 'guitar' : 'bass'; scheduleSave (); drawStrings (); } })));
    view.append (h ('p', { class: 'fine' }, 'Listening to ', h ('b', {}, strip.name), ' (pick another strip at the top). The reading is deliberately steadier than the tracker: a median over ~300 ms, a gliding needle and a note name that holds.'));
    Object.assign (tuneUi, { note, cents, needle, strings });
    function drawStrings ()
    {
        strings.textContent = '';
        tuneUi.stringEls = STRINGS[state.tunerMode].map ((m) => { const s = h ('span', {}, NOTE[m % 12] + (SHARP[m % 12] ? '#' : '')); s._m = m; strings.append (s); return s; });
    }
    drawStrings ();
}
function drawTune (msg)
{
    if (state.tab !== 'tune' || ! tuneUi.note) return;
    const t = msg.tuner;
    const strip = selected () || state.strips[0];
    const peak = strip.kind === 'keys' ? 0 : msg.inPeaks[Math.max (0, strip.source)] || 0;
    const r = t ? tuner.push (t.hz, t.conf, peak) : { live: false };
    if (! r.live) { tuneUi.note.textContent = '–'; tuneUi.note.className = 'note'; tuneUi.cents.textContent = 'Play a string'; tuneUi.needle.className = 'needle'; tuneUi.needle.style.left = '50%'; tuneUi.stringEls?.forEach ((s) => s.classList.remove ('hit')); return; }
    const m = r.locked, c = (r.note - m) * 100, inTune = Math.abs (c) < 3;
    tuneUi.note.innerHTML = '';
    tuneUi.note.append (NOTE[((m % 12) + 12) % 12], SHARP[((m % 12) + 12) % 12] ? h ('sup', {}, '#') : '', h ('sub', {}, String (Math.floor (m / 12) - 1)));
    tuneUi.note.className = 'note' + (inTune ? ' in' : '');
    tuneUi.cents.textContent = (c > 0 ? '+' : '') + c.toFixed (1) + ' cents';
    tuneUi.needle.style.left = (50 + Math.max (-50, Math.min (50, c))) + '%';
    tuneUi.needle.className = 'needle' + (inTune ? ' in' : '');
    tuneUi.stringEls?.forEach ((s) => s.classList.toggle ('hit', s._m === m));
}
function sendTuner ()
{
    const strip = selected () || state.strips[0];
    node?.port.postMessage ({ t: 'tuner', slot: state.tab === 'tune' ? strip.slot : -1 });
    tuner.reset ();
}

// ======================================================================
// the foot controller
// ======================================================================
const rig = {
    get: (strip, id) => { const s = slotOf (strip); return s < 0 ? (isChannel (id) ? undefined : getP (0, id)) : getP (s, id); },
    set: (stripKey, id, v) =>
    {
        const strip = stripByKey (stripKey);
        const s = strip ? strip.slot : stripKey === 'rig' ? 0 : -1;
        if (s < 0 && isChannel (id)) return;
        // a foot that switches on a pedal not on its board puts it there
        const def = FX.find ((d) => d.on === id && isLoadable (d));
        if (strip && def && v > 0.5 && ! strip.board.includes (def.key) && (! LANES[def.key] || LANES[def.key] === strip.kind)) { strip.board.push (def.key); postStrips (); if (state.tab === 'board') renderBoard (); }
        setP (Math.max (0, s), id, v);
    },
    command: (a, ms) => runCommand (a, ms),
    changed: () => {},
};
const footEngine = new FootEngine (state.foot, rig);
footEngine.onHeard = () => { drawLastHeard (); drawLamps (); };

function runCommand (action, ms = performance.now ())
{
    const src = loopSourceStrip ();
    const loop = (cmd, extra = {}) => node?.port.postMessage ({ t: 'loop', cmd, ms, ...extra });
    if (! node && action.startsWith ('do/loop')) { toast ('Start audio first'); return; }
    if (action === 'do/loopTempo') loop ('tap');
    else if (action === 'do/loopBars') loop ('bars');
    else if (action === 'do/loopRecord') loop ('record', { arg: src.slot });
    else if (action === 'do/loopStop') loop ('play');
    else if (action === 'do/loopUndo') loop ('undo');
    else if (action === 'do/loopClear') loop ('clear');
    else if (action === 'do/panic') panic ();
    else if (action.startsWith ('do/layout:'))
    {
        state.foot.active = action.slice (10);
        scheduleSave ();
        toast ('Foot layout: ' + state.foot.active);
        if (state.tab === 'foot') renderFoot ();
    }
    else if (action.startsWith ('do/preset:'))
    {
        const [, key, ...rest] = action.slice (3).split (':');
        const name = rest.join (':'), strip = stripByKey (key);
        const p = strip && chainPresetsFor (state, presetKind (strip)).find ((x) => x.name === name);
        if (p) { recallChainPreset (strip, p); toast (strip.name + ': ' + p.name); }
    }
}

function describeAction (action)
{
    const dot = ' · ';
    const loopF = LOOP_FUNCS.find ((f) => f[0] === action);
    if (loopF) return 'Loop' + dot + loopF[2];
    if (action === 'do/panic') return 'MIDI panic';
    if (action.startsWith ('do/layout:')) return 'Switch foot layout' + dot + action.slice (10);
    if (action.startsWith ('do/preset:')) { const [, key, ...n] = action.slice (3).split (':'); return (stripByKey (key)?.name || key) + dot + 'Preset' + dot + n.join (':'); }
    const t = parseAction (action);
    if (! t) return action;
    const strip = t.strip === 'rig' ? null : stripByKey (t.strip);
    const def = FX.find ((d) => d.on === t.id) || FX.find ((d) => d.ids.includes (t.id) || d.style?.includes (t.id));
    const sw = def && def.on === t.id;
    const missing = { mic: 'Mic', keys: 'Keys' }[t.strip] || t.strip;
    return [strip ? strip.name : t.strip === 'rig' ? 'Rig' : missing + ' (add the strip)', def?.name, sw ? (def.kind === 'hold' ? 'Hold' : def.kind === 'mute' ? 'Mute' : 'On') : SPEC[t.id].label].filter (Boolean).join (dot);
}

const footUi = {};
function renderFoot ()
{
    const view = $('#view-foot');
    dropScope ('foot');
    view.textContent = '';
    const map = state.foot;

    if (! midi) view.append (h ('div', { class: 'card', style: { padding: '12px', marginBottom: '10px' } },
        h ('b', {}, navigator.requestMIDIAccess ? (ctx ? 'MIDI not available' : 'MIDI not connected yet') : 'No Web MIDI in this browser'),
        h ('p', { class: 'fine', style: { margin: '4px 0 0' } }, navigator.requestMIDIAccess
            ? (ctx ? 'MIDI access was not granted. Bluetooth page-turner pedals still work: they arrive as keys.' : 'Start audio to connect MIDI controllers (Chrome on Android supports USB MIDI pedals).')
            : 'MIDI pedals need Chrome on Android or a desktop browser. Bluetooth page-turner pedals work everywhere: they arrive as keys.')));

    const layoutSel = h ('select', { class: 'select grow', 'aria-label': 'Foot layout', onchange: (e) => { map.active = e.target.value; scheduleSave (); renderFoot (); } },
                         map.layouts.map ((l) => h ('option', { value: l.name }, l.name)));
    layoutSel.value = map.active;
    view.append (h ('div', { class: 'section-label' }, 'Layout'));
    layoutSel.style.width = '100%';
    view.append (layoutSel, h ('div', { class: 'layout-btns' },
        h ('button', { class: 'btn small', onclick: () =>
        {
            const base = footEngine.layout ();
            let name = base.name + ' 2', i = 2;
            while (map.layouts.some ((l) => l.name === name)) name = base.name + ' ' + ++i;
            map.layouts.push ({ name, jobs: base.jobs.map ((j) => ({ ...j })) });
            map.active = name; scheduleSave (); renderFoot ();
        } }, 'Copy'),
        h ('button', { class: 'btn small', onclick: () =>
        {
            const l = footEngine.layout (), name = ask ('Layout name', l.name);
            if (! name || map.layouts.some ((x) => x.name === name)) return;
            for (const lay of map.layouts) for (const j of lay.jobs) if (j.action === 'do/layout:' + l.name) j.action = 'do/layout:' + name;
            l.name = name; map.active = name; scheduleSave (); renderFoot ();
        } }, 'Rename'),
        h ('button', { class: 'btn small', onclick: () =>
        {
            if (map.layouts.length < 2) { toast ('The last layout stays'); return; }
            const l = footEngine.layout ();
            if (! confirm ('Delete layout ' + l.name + '?')) return;
            map.layouts = map.layouts.filter ((x) => x !== l);
            for (const lay of map.layouts) lay.jobs = lay.jobs.filter ((j) => j.action !== 'do/layout:' + l.name);
            map.active = map.layouts[0].name; scheduleSave (); renderFoot ();
        } }, 'Del')));

    const devSel = h ('select', { class: 'select grow', style: { width: '100%' }, onchange: (e) => { map.device = e.target.value; scheduleSave (); } },
                      h ('option', { value: '' }, 'Any controller'), midiInputs ().map ((i) => h ('option', { value: i.name }, i.name)));
    devSel.value = map.device;
    footUi.heard = h ('div', { class: 'fine', style: { marginTop: '8px' } });
    view.append (h ('div', { class: 'section-label' }, 'Listening to'), devSel, footUi.heard);
    drawLastHeard ();

    view.append (h ('button', { class: 'btn primary', style: { width: '100%', marginTop: '14px', height: '52px' }, onclick: () => startLearn () }, '+ Add a foot control'));

    view.append (h ('div', { class: 'section-label' }, 'Controls'));
    footUi.lamps = new Map ();
    if (! map.controls.length) view.append (h ('p', { class: 'fine' }, 'Tap Add, then step on a switch or pedal. A MIDI controller, an expression pedal, or a Bluetooth page-turner all work.'));
    for (const c of map.controls) view.append (controlCard (c));
}

function controlCard (c)
{
    const map = state.foot;
    const lamp = h ('i', { class: 'lamp' });
    footUi.lamps.set (c.id, lamp);
    const kindSel = h ('select', { class: 'select', style: { height: '36px', fontSize: '13px' }, onchange: (e) => { c.kind = e.target.value; scheduleSave (); renderFoot (); } },
                       Object.entries (KIND_NAMES).map (([k, n]) => h ('option', { value: k }, n)));
    kindSel.value = c.kind;
    const card = h ('div', { class: 'card fcard' },
        h ('div', { class: 'fcard-head' }, lamp,
            h ('button', { class: 'fcard-name', onclick: () => { const n = ask ('Name this control', c.name); if (n) { c.name = n; scheduleSave (); renderFoot (); } } }, c.name),
            kindSel,
            h ('button', { class: 'x', 'aria-label': 'Control menu', onclick: () => openSheet (c.name, null, h ('div', { class: 'menu' },
                h ('button', { onclick: () => { closeSheet (); startLearn (c); } }, 'Learn again'),
                h ('button', { style: { color: 'var(--red)' }, onclick: () =>
                {
                    map.controls = map.controls.filter ((x) => x !== c);
                    for (const l of map.layouts) l.jobs = l.jobs.filter ((j) => j.control !== c.id);
                    scheduleSave (); closeSheet (); renderFoot ();
                } }, 'Remove control'))) }, '⋯')),
        h ('div', { class: 'fcard-wire' }, describeTrigger (c.trigger)));
    const jobs = (footEngine.layout ()?.jobs || []).filter ((j) => j.control === c.id);
    for (const job of jobs) card.append (jobRow (c, job));
    card.append (h ('button', { class: 'btn small', style: { marginTop: '10px', width: '100%' }, onclick: () => openFunctionPicker (c) }, '+ Give it a job'));
    drawLamps ();
    return card;
}

function jobRow (control, job)
{
    const layout = footEngine.layout ();
    const t = parseAction (job.action);
    const el = h ('div', { class: 'job' },
        h ('div', { class: 'job-head' }, h ('div', { class: 'job-name' }, describeAction (job.action)),
            h ('button', { class: 'x', 'aria-label': 'Remove job', onclick: () => { layout.jobs = layout.jobs.filter ((j) => j !== job); scheduleSave (); renderFoot (); } }, '×')));
    if (! t) return el;
    const s = SPEC[t.id];
    if (s.toggle)
    {
        const verbs = control.kind === 'preset' ? ['on', 'off', 'flip'] : ['follow', 'on', 'off', 'flip'];
        if (! verbs.includes (job.verb)) job.verb = verbs[0];
        el.append (h ('div', { style: { marginTop: '8px' } }, segmented ({ options: verbs.map ((v, i) => [i, VERB_NAMES[v]]), get: () => verbs.indexOf (job.verb), set: (i) => { job.verb = verbs[i]; scheduleSave (); } })));
    }
    else
    {
        if (job.lo == null || job.lo === 0 && job.hi === 1 && s.max !== 1) { job.lo = s.min; job.hi = s.max; }
        const box = h ('div', { style: { display: 'flex', flexDirection: 'column', gap: '8px', marginTop: '8px' } });
        if (control.kind === 'preset')
            box.append (fader ({ spec: s, label: 'Sends to', get: () => job.hi, set: (v) => { job.hi = v; scheduleSave (); } }));
        else
        {
            box.append (fader ({ spec: s, label: 'Pedal up', get: () => job.lo, set: (v) => { job.lo = v; scheduleSave (); } }),
                        fader ({ spec: s, label: 'Pedal down', get: () => job.hi, set: (v) => { job.hi = v; scheduleSave (); } }),
                        h ('button', { class: 'btn small', onclick: () => { [job.lo, job.hi] = [job.hi, job.lo]; scheduleSave (); renderFoot (); } }, 'Swap ends'));
        }
        el.append (box);
    }
    return el;
}

function addJob (control, action)
{
    const layout = footEngine.layout ();
    const t = parseAction (action);
    const s = t && SPEC[t.id];
    const job = { control: control.id, action, verb: control.kind === 'preset' ? 'flip' : 'follow', lo: s ? s.min : 0, hi: s ? (s.toggle ? 1 : getP (Math.max (0, slotOf (t.strip)), t.id)) : 1 };
    if (s && ! s.toggle && control.kind !== 'preset') { job.lo = s.min; job.hi = s.max; }
    layout.jobs.push (job);
    scheduleSave ();
    closeSheet ();
    renderFoot ();
}

// The rig as a menu: every strip, every pedal on its board, each pedal's
// switch and each of its knobs, a strip's presets, the looper, the master,
// MIDI panic and the layouts.
function openFunctionPicker (control)
{
    const choose = (action) => addJob (control, action);
    const page = (title, items, back) =>
    {
        const body = h ('div', {},
            back ? h ('button', { class: 'btn small', style: { marginBottom: '10px' }, onclick: back }, '‹ Back') : null,
            h ('div', { class: 'menu' }, items.map (([label, fn, right]) => h ('button', { onclick: fn }, label, h ('span', { class: 'r' }, right || '›')))));
        openSheet (title, null, body);
    };
    const root = () => page ('Give ' + control.name + ' a job', [
        ...state.strips.map ((s) => [s.name, () => stripPage (s)]),
        ['Looper', () => page ('Looper', LOOP_FUNCS.map ((f) => [f[2], () => choose (f[0]), 'press']).concat ([['Loop level', () => choose ('rig/loopLevel'), 'knob']]), root)],
        ['Master', () => page ('Master', [['Gain', () => choose ('rig/masterGain'), 'knob'], ['Mute', () => choose ('rig/masterMute'), 'switch']], root)],
        ['MIDI panic', () => choose ('do/panic'), 'press'],
        ['Switch foot layout', () => page ('Switch foot layout', state.foot.layouts.map ((l) => [l.name, () => choose ('do/layout:' + l.name), 'press']), root)],
    ]);
    const stripPage = (s) => page (s.name, [
        ...visibleRows (s).map ((k) => [FXMAP[k].name, () => fxPage (s, k)]),
        ['Presets', () => page (s.name + ' presets', chainPresetsFor (state, presetKind (s)).map ((p) => [p.name, () => choose ('do/preset:' + s.key + ':' + p.name), 'recall']), () => stripPage (s))],
    ], root);
    const fxPage = (s, k) =>
    {
        const d = FXMAP[k];
        const sw = d.kind === 'hold' ? 'Hold' : d.kind === 'mute' ? 'Mute' : 'On / off';
        page (s.name + ' · ' + d.name, [[sw, () => choose (s.key + '/' + d.on), 'switch'], ...d.ids.concat (d.style || []).map ((id) => [SPEC[id].label, () => choose (s.key + '/' + id), SPEC[id].choices ? 'choice' : 'knob'])], () => stripPage (s));
    };
    root ();
}

let learnFor = null;
function startLearn (relearn = null)
{
    learnFor = relearn;
    const overlay = h ('div', { class: 'learn', id: 'learn' }, h ('div', {}, h ('div', { class: 'pulse' }), h ('h2', {}, 'Step on it'),
        h ('p', { class: 'muted' }, 'Press the switch, rock the pedal, or tap the page-turner. The rig listens instead of obeying.'),
        h ('button', { class: 'btn', style: { marginTop: '16px' }, onclick: cancelLearn }, 'Cancel')));
    document.body.append (overlay);
    footEngine.beginLearn ();
}
function cancelLearn () { footEngine.cancelLearn (); $('#learn')?.remove (); }
footEngine.onLearned = ({ trigger, kind }) =>
{
    $('#learn')?.remove ();
    const map = state.foot;
    if (learnFor) { learnFor.trigger = trigger; learnFor.kind = kind; }
    else
    {
        const id = footEngine.freshId ();
        const n = map.controls.length + 1;
        map.controls.push ({ id, name: trigger.type === 'key' ? 'Pedal ' + n : 'Button ' + n, trigger, kind });
    }
    vibrate (30);
    scheduleSave ();
    renderFoot ();
    toast ('Learned: ' + describeTrigger (trigger) + ' (' + KIND_NAMES[kind] + ')');
};
function drawLastHeard ()
{
    if (! footUi.heard) return;
    const L = footEngine.lastHeard;
    footUi.heard.textContent = L ? 'Last heard: ' + describeTrigger (L.trigger) + (L.trigger.type === 'controller' ? ' = ' + L.value : '') + (L.device ? ' from ' + L.device : '') : 'Last heard: nothing yet';
}
function drawLamps ()
{
    if (! footUi.lamps) return;
    for (const [id, lamp] of footUi.lamps)
    {
        const c = state.foot.controls.find ((x) => x.id === id);
        const lit = c && (c.kind === 'preset' ? footEngine.lastHeard?.controls.includes (id) && performance.now () - footEngine.lastHeard.at < 300 : footEngine.controlValue (id) >= 0.5);
        lamp.classList.toggle ('lit', !! lit);
    }
}

// keys from a Bluetooth page-turner, or a keyboard
const typing = () => /^(INPUT|SELECT|TEXTAREA)$/.test (document.activeElement?.tagName);
window.addEventListener ('keydown', (e) =>
{
    if (e.repeat || typing () || e.metaKey || e.ctrlKey) return;
    if (footEngine.learn || state.foot.controls.some ((c) => c.trigger.type === 'key' && c.trigger.code === e.code))
    {
        e.preventDefault ();
        footEngine.hear ({ type: 'key', code: e.code }, 127, '');
    }
});
window.addEventListener ('keyup', (e) =>
{
    if (typing ()) return;
    if (state.foot.controls.some ((c) => c.trigger.type === 'key' && c.trigger.code === e.code)) footEngine.hear ({ type: 'key', code: e.code }, 0, '');
});

// ======================================================================
// MIDI
// ======================================================================
async function startMidi ()
{
    if (! navigator.requestMIDIAccess) return;
    try { midi = await navigator.requestMIDIAccess ({ sysex: false }); }
    catch { midi = null; return; }
    const wire = () => { for (const input of midi.inputs.values ()) input.onmidimessage = (m) => onMidi (input, m.data); };
    wire ();
    midi.onstatechange = () => { wire (); if (state.tab === 'foot') renderFoot (); };
}
function onMidi (input, d)
{
    const st = d[0] & 0xf0, ch = (d[0] & 0x0f) + 1;
    if (d[0] === 0xfa) return footEngine.hear ({ type: 'start' }, 127, input.name);
    if (d[0] === 0xfc) return footEngine.hear ({ type: 'stop' }, 127, input.name);
    if (d[0] === 0xfb) return footEngine.hear ({ type: 'resume' }, 127, input.name);
    if (st === 0xc0) return footEngine.hear ({ type: 'program', channel: ch, number: d[1] }, 127, input.name);
    if (st === 0xb0) return footEngine.hear ({ type: 'controller', channel: ch, number: d[1] }, d[2], input.name);
    if (st === 0x90 || st === 0x80)
    {
        if (! stripByKey ('keys')) return;
        if (state.audio.keysIn && state.audio.keysIn !== input.id) return;
        node?.port.postMessage ({ t: 'note', on: st === 0x90 && d[2] > 0, note: d[1], vel: d[2] });
    }
}
function midiOut () { return state.audio.midiOut && midi ? midi.outputs.get (state.audio.midiOut) : null; }
function sendTrackerMidi (ev)
{
    const out = midiOut ();
    if (! out) return;
    const ch = (Math.round (getP (0, 'midiCh')) - 1) & 15, expr = Math.round (getP (0, 'midiExpr'));
    for (let i = 0; i < ev.length; i += 3)
    {
        const t = ev[i], a = ev[i + 1], b = ev[i + 2];
        if (t === 0x90) out.send ([0x90 | ch, a, b]);
        else if (t === 0x80) out.send ([0x80 | ch, a, 0]);
        else if (t === 0xe0) out.send ([0xe0 | ch, a & 127, (a >> 7) & 127]);
        else if (t === 0xb0 && expr) out.send ([0xb0 | ch, expr === 1 ? 1 : 11, a]);
    }
}
function panic ()
{
    node?.port.postMessage ({ t: 'panic' });
    const out = midiOut ();
    if (out) for (let c = 0; c < 16; ++c) { out.send ([0xb0 | c, 123, 0]); out.send ([0xb0 | c, 120, 0]); out.send ([0xe0 | c, 0, 64]); }
}

// ======================================================================
// audio
// ======================================================================
const stripImports = (s) => s.replace (/^import[\s\S]*?from\s+['"][^'"]+['"];?/gm, '').replace (/^export\s+/gm, '');
async function workletUrl ()
{
    const files = ['dsp.js', 'engine.js', 'worklet-main.js'];
    const texts = await Promise.all (files.map ((f) => fetch (new URL (f, import.meta.url)).then ((r) => r.text ())));
    return URL.createObjectURL (new Blob ([texts.map (stripImports).join ('\n')], { type: 'text/javascript' }));
}
function inputLatency () { try { return stream?.getAudioTracks ()[0]?.getSettings ().latency || 0; } catch { return 0; } }
async function openInput ()
{
    const constraints = { audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false, channelCount: { ideal: 2 }, latency: { ideal: 0 } } };
    if (state.audio.inputId) constraints.audio.deviceId = { exact: state.audio.inputId };
    try { stream = await navigator.mediaDevices.getUserMedia (constraints); }
    catch (e)
    {
        if (state.audio.inputId) { state.audio.inputId = ''; delete constraints.audio.deviceId; stream = await navigator.mediaDevices.getUserMedia (constraints); }
        else throw e;
    }
    inputChannels = stream.getAudioTracks ()[0]?.getSettings ().channelCount || 1;
    source = ctx.createMediaStreamSource (stream);
    source.connect (node);
    sendLatency ();
}
function closeInput ()
{
    try { source?.disconnect (); } catch {}
    stream?.getTracks ().forEach ((t) => t.stop ());
    source = null; stream = null;
}
function sendLatency ()
{
    if (! ctx) return;
    const sr = ctx.sampleRate, out = (ctx.outputLatency || 0) + (ctx.baseLatency || 0);
    node?.port.postMessage ({ t: 'latency', roundTrip: Math.round ((out + inputLatency ()) * sr), keys: Math.round (out * sr + 64) });
}

async function startAudio ()
{
    if (ctx) return;
    const btn = $('#startBtn');
    btn.textContent = 'Starting…'; btn.disabled = true;
    try
    {
        ctx = new (window.AudioContext || window.webkitAudioContext) ({ latencyHint: 'interactive' });
        await ctx.audioWorklet.addModule (await workletUrl ());
        const specs = SPECS.map ((s) => ({ id: s.id, def: s.def, channel: isChannel (s.id) }));
        node = new AudioWorkletNode (ctx, 'liverig', {
            numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2], channelCount: 2, channelCountMode: 'explicit', channelInterpretation: 'discrete',
            processorOptions: { specs, values: state.values, strips: state.strips.map ((s) => ({ slot: s.slot, kind: s.kind, source: s.source, board: s.board })), patch: state.patch },
        });
        node.port.onmessage = (e) => onEngine (e.data);
        node.connect (ctx.destination);
        for (const [slot, ir] of Object.entries (state.irs)) node.port.postMessage ({ t: 'ir', c: Number (slot), data: Float32Array.from (ir.data) });
        await openInput ();
        await ctx.resume ();
        $('#start').hidden = true;
        $('#power').classList.add ('on');
        startMidi ().then (() => { if (state.tab === 'foot') renderFoot (); });
        keepAwake ();
        ctx.onstatechange = () => $('#power').classList.toggle ('on', ctx.state === 'running');
        renderBoard ();
        sendTuner ();
    }
    catch (err)
    {
        console.error (err);
        try { ctx?.close (); } catch {}
        ctx = null; node = null;
        btn.textContent = 'Start audio'; btn.disabled = false;
        const msg = err?.name === 'NotAllowedError' ? 'Microphone permission was refused. Allow it in the browser\'s site settings, then try again.'
                  : ! window.AudioWorkletNode ? 'This browser has no AudioWorklet. Use a current Chrome, Safari or Firefox.'
                  : 'Audio could not start: ' + (err?.message || err);
        $('.start-card p').textContent = msg;
    }
}

async function keepAwake ()
{
    if (! state.wake || ! navigator.wakeLock) return;
    try { wakeLock = await navigator.wakeLock.request ('screen'); } catch {}
}
document.addEventListener ('visibilitychange', () =>
{
    if (document.visibilityState === 'visible') { if (ctx && state.wake) keepAwake (); ctx?.resume (); }
    else saveState (state);
});

function onEngine (m)
{
    if (m.t === 'midi') { sendTrackerMidi (m.ev); return; }
    if (m.t !== 'meters') return;
    latest = m; latestAt = performance.now ();
    drawTune (m);
}

// meters, loop pane and other live readouts on the display's clock
function frame ()
{
    requestAnimationFrame (frame);
    if (! latest) return;
    const m = latest;
    const pct = (p) => Math.min (100, Math.max (0, (20 * Math.log10 (p + 1e-6) + 60) / 60 * 100)) + '%';
    $('#mL').style.setProperty ('--v', pct (m.out[0]));
    $('#mR').style.setProperty ('--v', pct (m.out[1]));
    const lim = $('#lim');
    lim.hidden = m.gr < 0.1;
    if (m.gr >= 0.1) lim.textContent = m.gr.toFixed (1);
    for (const chip of document.querySelectorAll ('#rail .chip[data-slot]')) chip._meter.style.height = pct (m.peaks[Number (chip.dataset.slot)]);
    const midiRow = document.querySelector ('.row[data-key="midi"]');
    if (midiRow?._note) midiRow._note.textContent = m.note >= 0 ? NOTE[m.note % 12] + (SHARP[m.note % 12] ? '#' : '') + (Math.floor (m.note / 12) - 1) : '';
    drawLoop ();
    if (performance.now () - (frame.status || 0) > 1000) { frame.status = performance.now (); drawStatus (); drawLamps (); }
}

// ======================================================================
// settings
// ======================================================================
async function openSettings ()
{
    const body = h ('div');
    let inputs = [];
    try { inputs = (await navigator.mediaDevices.enumerateDevices ()).filter ((d) => d.kind === 'audioinput'); } catch {}
    const inSel = h ('select', { class: 'select', style: { width: '100%' }, onchange: async (e) =>
    {
        state.audio.inputId = e.target.value; scheduleSave ();
        if (ctx) { closeInput (); try { await openInput (); toast ('Input: ' + (e.target.selectedOptions[0]?.textContent || 'default')); renderBoard (); } catch { toast ('That input could not be opened'); } }
    } }, h ('option', { value: '' }, 'System default'), inputs.map ((d, i) => h ('option', { value: d.deviceId }, d.label || 'Input ' + (i + 1))));
    inSel.value = state.audio.inputId;
    body.append (h ('div', { class: 'section-label' }, 'Audio input'), inSel,
        h ('p', { class: 'fine' }, ctx ? `${inputChannels} channel${inputChannels > 1 ? 's' : ''} open · ${(ctx.sampleRate / 1000).toFixed (1)} kHz · output ${(((ctx.outputLatency || 0) + (ctx.baseLatency || 0)) * 1000).toFixed (1)} ms · input ${(inputLatency () * 1000).toFixed (1)} ms`
                                     : 'Start audio to see the device figures. Device names appear once the microphone is allowed.'));
    body.append (h ('div', { class: 'section-label' }, 'MIDI'));
    if (midi) body.append (midiOutPicker (), h ('p', { class: 'fine' }, midiInputs ().length + ' MIDI input' + (midiInputs ().length === 1 ? '' : 's') + ' connected: ' + (midiInputs ().map ((i) => i.name).join (', ') || 'none')));
    else body.append (h ('p', { class: 'fine' }, ! navigator.requestMIDIAccess ? 'This browser has no Web MIDI. Bluetooth page-turner pedals still work as foot controls.'
                                                  : ctx ? 'MIDI access was not granted. Allow MIDI for this site in the browser settings, then reload.' : 'Connects when audio starts.'));
    body.append (h ('div', { class: 'section-label' }, 'Stage'),
        segmented ({ label: 'Keep the screen awake', options: [[1, 'On'], [0, 'Off']], get: () => (state.wake ? 1 : 0), set: (v) => { state.wake = !! v; scheduleSave (); if (v) keepAwake (); else { wakeLock?.release?.(); wakeLock = null; } } }));
    const imp = h ('input', { type: 'file', accept: 'application/json,.json', hidden: true, onchange: async (e) =>
    {
        try
        {
            const s = JSON.parse (await e.target.files[0].text ());
            if (! s.values || ! s.strips) throw new Error ();
            localStorage.setItem ('liverig.v1', JSON.stringify (s));
            location.reload ();
        }
        catch { toast ('That is not a LiveRig settings file'); }
    } });
    body.append (h ('div', { class: 'section-label' }, 'Settings & presets'),
        h ('div', { class: 'hrow' },
            h ('button', { class: 'btn grow', onclick: () =>
            {
                saveState (state);
                const a = h ('a', { href: URL.createObjectURL (new Blob ([localStorage.getItem ('liverig.v1')], { type: 'application/json' })), download: 'liverig-settings.json' });
                document.body.append (a); a.click (); a.remove ();
            } }, 'Export'),
            h ('button', { class: 'btn grow', onclick: () => imp.click () }, 'Import'),
            h ('button', { class: 'btn grow danger', onclick: () => { if (confirm ('Reset every setting, preset and foot map?')) { localStorage.removeItem ('liverig.v1'); location.reload (); } } }, 'Reset')), imp,
        h ('p', { class: 'fine' }, 'Everything is saved on this phone as you go: knobs, boards, strips, presets and the foot map.'));
    openSheet ('Settings', null, body);
}

// ======================================================================
// tabs and start
// ======================================================================
function showTab (tab)
{
    state.tab = tab;
    for (const b of document.querySelectorAll ('#tabs button')) b.classList.toggle ('sel', b.dataset.tab === tab);
    for (const v of ['board', 'loop', 'tune', 'foot']) $('#view-' + v).hidden = v !== tab;
    if (tab === 'board') renderBoard ();
    if (tab === 'loop') renderLoop ();
    if (tab === 'tune') renderTune ();
    if (tab === 'foot') renderFoot ();
    sendTuner ();
    scheduleSave ();
}

for (const b of document.querySelectorAll ('#tabs button')) b.addEventListener ('click', () => showTab (b.dataset.tab));
$('#scrim').addEventListener ('click', closeSheet);
$('#settingsBtn').addEventListener ('click', openSettings);
$('#masterChip').addEventListener ('click', () => selectStrip ('master'));
$('#startBtn').addEventListener ('click', startAudio);
$('#skipBtn').addEventListener ('click', () => ($('#start').hidden = true));
$('#power').addEventListener ('click', async () =>
{
    if (! ctx) { $('#start').hidden = false; return; }
    if (ctx.state === 'running') { await ctx.suspend (); toast ('Audio paused'); }
    else { await ctx.resume (); toast ('Audio on'); }
    $('#power').classList.toggle ('on', ctx.state === 'running');
});
document.addEventListener ('keydown', (e) => { if (e.key === 'Escape') closeSheet (); });

renderRail ();
showTab (state.tab || 'board');
requestAnimationFrame (frame);
if ('serviceWorker' in navigator && location.protocol === 'https:') navigator.serviceWorker.register ('sw.js').catch (() => {});

// For tests and the curious.
window.liverig = { state, runCommand, setP, getP, get latest () { return latest; }, get node () { return node; } };
