// The foot controller: what is under the foot (controls, by name) and what
// each one does (jobs, grouped into swappable layouts). A port of
// FootControl.cpp, plus one trigger the desktop never needed: a computer key,
// which is what Bluetooth page-turner pedals send to a phone.

import { SPEC } from './model.js';

export const TRIGGER_NAMES = { program: 'Program change', controller: 'Controller', start: 'Start', stop: 'Stop', resume: 'Continue', key: 'Key' };
export const KIND_NAMES = { preset: 'Preset button', latch: 'Latching', momentary: 'Momentary', sweep: 'Expression' };
export const VERB_NAMES = { follow: 'Follows it', on: 'Turns on', off: 'Turns off', flip: 'Flips' };
export const LEARN_SETTLE_MS = 450;

export function describeTrigger (t)
{
    if (t.type === 'key') return 'Key ' + t.code.replace (/^Key|^Digit/, '');
    if (t.type === 'start' || t.type === 'stop' || t.type === 'resume') return 'MIDI ' + TRIGGER_NAMES[t.type];
    return TRIGGER_NAMES[t.type] + ' ' + t.number + ' · ' + (t.channel ? 'ch ' + t.channel : 'any channel');
}

const sameWire = (a, b) => a.type === b.type && (a.type === 'key' ? a.code === b.code
    : a.type === 'program' || a.type === 'controller' ? a.number === b.number && (! a.channel || ! b.channel || a.channel === b.channel) : true);

export function factoryFoot ()
{
    // Program changes 0/1/2: silence / autotune / vocoder, the layout the desktop rig starts with.
    const controls = [], jobs = [];
    const lanes = [[false, false], [true, false], [false, true]];
    for (let n = 0; n < 3; ++n)
    {
        const id = 'c' + (n + 1);
        controls.push ({ id, name: 'Program ' + n, trigger: { type: 'program', channel: 0, number: n }, kind: 'preset' });
        jobs.push ({ control: id, action: 'mic/vocalOn', verb: lanes[n][0] ? 'on' : 'off', lo: 0, hi: 1 });
        jobs.push ({ control: id, action: 'mic/vocoderOn', verb: lanes[n][1] ? 'on' : 'off', lo: 0, hi: 1 });
    }
    return { device: '', active: 'Default', controls, layouts: [{ name: 'Default', jobs }] };
}

export class FootEngine
{
    // rig: { get(stripKey, id), set(stripKey, id, v), command(action, ms), changed() }
    constructor (map, rig)
    {
        this.map = map; this.rig = rig;
        this.last = new Map ();         // control id -> last value
        this.learn = null;              // { state, trigger, first, sawMiddle, sawOther, at, device }
        this.lastHeard = null;
        this.onLearned = null;
        this.onHeard = null;
    }
    layout () { return this.map.layouts.find ((l) => l.name === this.map.active) || this.map.layouts[0]; }
    controlValue (id) { return this.last.get (id) ?? 0; }

    // A message from a controller. value: 0-127. Returns true if anything listened.
    hear (trigger, value, device)
    {
        this.lastHeard = { trigger, value, device, controls: [], at: performance.now () };
        if (this.learn)
        {
            const L = this.learn;
            if (L.state === 'armed')
            {
                Object.assign (L, { state: 'settling', trigger, first: value, device, at: performance.now (),
                                    sawMiddle: trigger.type === 'controller' && value !== 0 && value !== 127, sawOther: false });
                setTimeout (() => this.finishLearn (), LEARN_SETTLE_MS);
            }
            else if (L.state === 'settling' && sameWire (L.trigger, trigger))
            {
                L.sawMiddle ||= trigger.type === 'controller' && value !== 0 && value !== 127;
                L.sawOther ||= value !== L.first;
            }
            this.onHeard?.();
            return true;
        }
        if (this.map.device && device && this.map.device !== device && trigger.type !== 'key') { this.onHeard?.(); return false; }
        let any = false;
        for (const c of this.map.controls)
            if (sameWire (c.trigger, trigger) && (c.trigger.type !== 'key' || true))
            {
                if (trigger.type === 'controller' && c.trigger.channel && trigger.channel !== c.trigger.channel) continue;
                this.lastHeard.controls.push (c.id);
                this.apply (c, value);
                any = true;
            }
        this.onHeard?.();
        return any;
    }

    apply (control, midiValue)
    {
        let value = 1, pressed = true;
        const prev = this.last.has (control.id) ? this.last.get (control.id) : -1;
        switch (control.kind)
        {
            case 'latch': value = midiValue >= 64 ? 1 : 0; break;
            case 'momentary': value = midiValue >= 64 ? 1 : 0; pressed = midiValue >= 64; break;
            case 'sweep': value = Math.min (127, Math.max (0, midiValue)) / 127; pressed = value >= 0.5 && prev >= 0 && prev < 0.5; break;
            default: if (midiValue < 64 && control.trigger.type === 'key') return;   // a key's release is not a press
        }
        this.last.set (control.id, value);
        const jobs = (this.layout ()?.jobs || []).filter ((j) => j.control === control.id);
        let moved = false;
        for (const job of jobs)
        {
            if (job.action.startsWith ('do/')) { if (pressed) this.rig.command (job.action, performance.now ()); continue; }
            const t = parseAction (job.action);
            if (! t) continue;
            const s = SPEC[t.id];
            if (s.toggle)
            {
                const now = this.rig.get (t.strip, t.id) > 0.5 * (s.min + s.max);
                let want = now;
                if (job.verb === 'follow') want = value >= 0.5;
                else if (job.verb === 'flip') want = pressed ? ! now : now;
                else if (job.verb === 'on') want = pressed ? true : now;
                else if (job.verb === 'off') want = pressed ? false : now;
                if (want !== now) { this.rig.set (t.strip, t.id, want ? s.max : s.min); moved = true; }
                continue;
            }
            if (control.kind === 'preset' && ! pressed) continue;
            let target = job.lo + value * (job.hi - job.lo);
            if (control.kind === 'preset') target = job.hi;
            if (s.step > 0) target = s.min + s.step * Math.round ((target - s.min) / s.step);
            target = Math.min (s.max, Math.max (s.min, target));
            if (target !== this.rig.get (t.strip, t.id)) { this.rig.set (t.strip, t.id, target); moved = true; }
        }
        if (moved) this.rig.changed ();
    }

    beginLearn () { this.learn = { state: 'armed' }; }
    cancelLearn () { this.learn = null; }
    finishLearn ()
    {
        const L = this.learn;
        if (! L || L.state !== 'settling') return;
        this.learn = null;
        let kind = 'preset';
        if (L.trigger.type === 'controller') kind = L.sawMiddle ? 'sweep' : L.sawOther ? 'momentary' : 'latch';
        this.onLearned?.({ trigger: L.trigger, kind, device: L.device });
    }
    freshId ()
    {
        for (let n = 1; ; ++n) if (! this.map.controls.some ((c) => c.id === 'c' + n)) return 'c' + n;
    }
}

// "bass/odOn" -> { strip: 'bass', id: 'odOn' }; "rig/masterGain" for the globals.
export function parseAction (action)
{
    if (action.startsWith ('do/')) return null;
    const i = action.indexOf ('/');
    if (i < 0) return null;
    const id = action.slice (i + 1);
    if (! SPEC[id]) return null;
    return { strip: action.slice (0, i), id };
}
