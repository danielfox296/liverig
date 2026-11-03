// The LiveRig engine: strips, pedalboards, mic lanes, keys, looper and the
// master bus. A port of FxChain.cpp, AudioEngine.cpp, Looper.cpp and the
// parts of VocalChannel/InstrumentHost a browser can do natively.
//
// This file and dsp.js are concatenated into one AudioWorklet module at load
// time (see app.js), so it must not rely on anything but dsp.js.

import {
    TWO_PI, dbToGain, nextPow2, clamp, Ramp, readHermite, Biquad, OnePoleLP, AllpassOne, TptSvf, EnvFollower,
    BufferedAllpass, DCBlocker, GlueComp, CabSim, SubOctave, MasterLimiter, PitchTracker, OctaveShifter,
    PlateVerb, SpectralFreeze, NoteSegmenter, Oversampler4, Convolver
} from './dsp.js';

export const MOVABLE = ['comp', 'eq', 'filter', 'boost', 'drive', 'amp', 'cab', 'octave', 'sub', 'synth', 'phaser', 'chorus', 'delay', 'tape', 'freeze'];
const ON = { comp: 'compOn', eq: 'eqOn', filter: 'fltOn', boost: 'bstOn', drive: 'odOn', amp: 'ampOn', cab: 'cabOn', octave: 'octOn',
             sub: 'subOn', synth: 'syOn', phaser: 'phOn', chorus: 'chOn', delay: 'dlOn', tape: 'tpOn', freeze: 'frzOn', reverb: 'rvOn' };

const polyBlep = (t, dt) =>
{
    if (t < dt) { const x = t / dt; return x + x - x * x - 1; }
    if (t > 1 - dt) { const x = (t - 1) / dt; return x * x + x + x + 1; }
    return 0;
};

const choBaseMs = [8, 13, 19], choRateMult = [1, 0.87, 1.13], choDepthMult = [1, 0.8, 1.25];

// One strip's pedalboard: mono in, stereo out. Only loaded stages run; each
// one's bypass is a 15 ms crossfade so toggles never click.
export class Chain
{
    constructor (engine, slot, sr)
    {
        this.e = engine; this.slot = slot; this.sr = sr;
        const B = 128;
        this.order = [];          // loaded movable stages, in board order
        this.hasReverb = false;
        this.needTracker = false;
        this.dry = new Float32Array (B); this.carrier = new Float32Array (B);
        this.tmp = new Float32Array (B); this.tmp2 = new Float32Array (B);
        this.lastSeen = Object.create (null);
        this.primed = false;
        this.latency = 0;
        this.midiOut = [];

        this.dc = new DCBlocker (sr);
        const R = (s = 0.02) => { const r = new Ramp (0); r.reset (sr, s); return r; };
        this.inGain = R (); this.inMute = R (); this.liveOut = R ();
        this.byp = {}; for (const k of Object.keys (ON)) this.byp[k] = R (0.015);

        this.comp = new GlueComp (sr); this.makeup = R (); this.compBlend = R ();
        this.eq = [0, 1, 2, 3, 4].map (() => new Biquad ());
        this.fltEnv = new EnvFollower (); this.fltSvf = new TptSvf (); this.fltMix = R ();
        this.fltSens = 6; this.fltFrom = 120; this.fltRatio = 13.3; this.fltK = 0.3; this.fltComp = 1; this.fltCut = 120;
        this.cutCoef = 1 - Math.exp (-1 / (0.003 * sr));
        this.boostGain = R (); this.boostHair = R (); this.boostSheen = new Biquad ();
        this.os = new Oversampler4 (B); this.tone = new Biquad ();
        this.stack = R (); this.driveAmt = R (); this.driveLevel = R (); this.driveBlend = R ();
        this.biteTilt = new Biquad (); this.greenLp = new OnePoleLP (); this.goldLp = new OnePoleLP ();
        this.greenLp.setCutoff (sr * 4, 720); this.goldLp.setCutoff (sr * 4, 120);
        this.sagEnv = new EnvFollower (); this.sagEnv.setTimes (sr, 4, 160); this.sag = 0;
        this.driveVoice = 1; this.driveDC = new DCBlocker (sr); this.driveSplit = new OnePoleLP ();
        this.driveLow = new Float32Array (B); this.driveClean = new Float32Array (B);
        this.ampIn = R (); this.ampOut = R (); this.amp = new Convolver ();
        this.cab = new CabSim (sr);
        this.sub = new SubOctave (sr); this.subLevel = R (); this.subBuf = new Float32Array (B); this.subDiv = 1;
        this.tracker = new PitchTracker (sr);
        this.segmenter = new NoteSegmenter (sr);
        this.shifter = new OctaveShifter (sr); this.shifter2 = new OctaveShifter (sr);
        this.octMix = R (); this.oct2Lvl = R (); this.octBuf = new Float32Array (B);
        this.synPhase = 0; this.synSubPhase = 0; this.synPeriod = 600; this.synTarget = 600; this.synGlide = 1;
        this.synEnv = new EnvFollower (); this.synEnv.setTimes (sr, 2.5, 90); this.synSvf = new TptSvf ();
        this.synShape = R (); this.synSub = R (); this.synMix = R ();
        this.synCutBase = 350; this.synEnvAmt = 0.6; this.synK = 0.4; this.synComp = 1; this.synCut = 350;
        this.ap = Array.from ({ length: 8 }, () => new AllpassOne ());
        this.phLfo = 0; this.phFb = 0; this.phRate = 0.5; this.phDepth = 0.6; this.phFbState = 0; this.phLfoState = 0; this.phStages = 4;
        const cs = nextPow2 (Math.floor (sr * 0.05));
        this.choBuf = new Float32Array (cs); this.choMask = cs - 1; this.choWrite = cs;
        this.choPhase = [0, 1 / 3, 2 / 3]; this.choRate = 0.7;
        const choD = new Ramp (0); choD.reset (sr, 0.05); this.choDepth = choD;
        this.choVoice = [R (), R (), R ()]; this.choMix = R (); this.choTone = new OnePoleLP (); this.choLows = new Biquad ();
        const ds = nextPow2 (Math.floor (sr * 2.2));
        this.dlBuf = new Float32Array (ds); this.dlMask = ds - 1; this.dlWrite = ds;
        const dt = new Ramp (0); dt.reset (sr, 0.15); this.dlTime = dt;
        this.dlFb = R (); this.dlMix = R (); this.dlDamp = new OnePoleLP ();
        const dm = new Ramp (0); dm.reset (sr, 0.05); this.dlModDepth = dm;
        this.dlSat = R (); this.dlSmear = R ();
        this.dlNoise = 2463534242 >>> 0; this.dlWow = 0; this.dlFlut = 0; this.dlWowCoef = 0; this.dlWowAmp = 0; this.dlModRate = 0.9;
        this.dlFlutCoef = 1 - Math.exp (-TWO_PI * 6.5 / sr);
        this.smLen1 = Math.max (16, Math.floor (sr * 0.00647)); this.smLen2 = Math.max (8, Math.floor (sr * 0.00311));
        this.sm1 = new BufferedAllpass (this.smLen1 + 8); this.sm2 = new BufferedAllpass (this.smLen2 + 8);
        this.sm1.g = 0.55; this.sm2.g = 0.45;
        this.tapePre = new Biquad (); this.tapePost = new Biquad (); this.tapeBump = new Biquad ();
        this.tapeRoll = new OnePoleLP (); this.tapeDC = new DCBlocker (sr); this.tapeDrive = R (); this.tapeMix = R ();
        this.freeze = null;   // built on first load: it is the one big allocation
        this.frzLevel = R (); this.frzBuf = new Float32Array (B); this.frzHold = false;
        this.reverb = null; this.revMix = R ();
        this.revL = new Float32Array (B); this.revR = new Float32Array (B);
    }

    p (id) { return this.e.get (this.slot, id); }
    changed (id)
    {
        const v = this.p (id);
        if (! this.primed || this.lastSeen[id] !== v) { this.lastSeen[id] = v; return true; }
        return false;
    }

    setBoard (keys)
    {
        this.order = keys.filter ((k) => MOVABLE.includes (k));
        this.hasReverb = keys.includes ('reverb');
        if (this.order.includes ('freeze') && ! this.freeze) this.freeze = new SpectralFreeze (this.sr);
        if (this.hasReverb && ! this.reverb) this.reverb = new PlateVerb (this.sr);
    }

    update ()
    {
        const sr = this.sr, p = (id) => this.p (id), ch = (id) => this.changed (id);
        this.inGain.set (dbToGain (p ('inGain')));
        this.inMute.set (p ('inMute') > 0.5 ? 0 : 1);
        const thruMuted = this.slot === 0 && p ('midiOn') > 0.5 && p ('midiThru') < 0.5;
        this.liveOut.set (thruMuted || p ('liveMute') > 0.5 ? 0 : dbToGain (p ('liveOut')));

        if (ch ('compThr')) this.comp.setThreshold (p ('compThr'));
        if (ch ('compRatio')) this.comp.setRatio (p ('compRatio'));
        if (ch ('compAtk')) this.comp.setAttack (p ('compAtk'));
        if (ch ('compRel')) this.comp.setRelease (p ('compRel'));
        if (ch ('compSc')) this.comp.setSidechain (p ('compSc'));
        this.makeup.set (dbToGain (p ('compMkup')));
        this.compBlend.set (clamp (p ('compBlend'), 0, 1));

        for (let b = 0; b < 5; ++b)
        {
            const n = b + 1, f = 'eq' + n + 'F', g = 'eq' + n + 'G', q = 'eq' + n + 'Q';
            let dirty = ch (f); dirty = ch (g) || dirty; dirty = ch (q) || dirty;
            if (dirty)
            {
                if (b === 0) this.eq[0].lowShelfQ (sr, p (f), p (q), p (g));
                else if (b === 4) this.eq[4].highShelfQ (sr, p (f), p (q), p (g));
                else this.eq[b].peak (sr, p (f), p (q), p (g));
            }
        }

        if (ch ('fltSens')) this.fltSens = 6 * Math.pow (4, (p ('fltSens') - 0.5) * 2);
        let fd = ch ('fltAtk'); fd = ch ('fltRel') || fd;
        if (fd) this.fltEnv.setTimes (sr, p ('fltAtk'), p ('fltRel'));
        let rd = ch ('fltFrom'); rd = ch ('fltTo') || rd;
        if (rd) { this.fltFrom = Math.max (20, p ('fltFrom')); this.fltRatio = Math.max (20, p ('fltTo')) / this.fltFrom; }
        if (ch ('fltRes')) { const q = Math.max (0.5, p ('fltRes')); this.fltK = 1 / q; this.fltComp = 1 / Math.sqrt (Math.max (1, q * 0.35)); }
        this.fltMix.set (p ('fltMix'));

        this.boostGain.set (dbToGain (p ('bstGain')));
        this.boostHair.set (p ('bstHair'));
        if (ch ('bstSheen')) this.boostSheen.highShelf (sr, 2800, p ('bstSheen'));

        if (ch ('odXover')) this.driveSplit.setCutoff (sr, p ('odXover'));
        this.driveBlend.set (p ('odBlend'));
        this.driveVoice = clamp (Math.round (p ('odVoice')), 1, 3);
        this.driveAmt.set (Math.max (1, p ('odDrive')));
        this.driveLevel.set (dbToGain (p ('odLevel')));
        if (ch ('odTone')) this.tone.lowpass (sr, p ('odTone'), 0.7071);
        this.stack.set (clamp (p ('odStack'), 0, 1));
        if (ch ('odBite')) this.biteTilt.peak (sr * 4, 900, 0.8, 2 + 10 * p ('odBite'));

        this.ampIn.set (dbToGain (p ('ampIn')));
        this.ampOut.set (dbToGain (p ('ampOut')));

        let cd = ch ('cabBumpF'); cd = ch ('cabBumpG') || cd; cd = ch ('cabTop') || cd; cd = ch ('cabPres') || cd;
        if (cd) this.cab.setParams (p ('cabBumpF'), p ('cabBumpG'), p ('cabTop'), p ('cabPres'));

        this.subLevel.set (p ('subLevel'));
        this.subDiv = p ('subDiv') > 0.5 ? 2 : 1;
        if (ch ('subTone')) this.sub.setTone (p ('subTone'));

        this.shifter.setSemitones (p ('octInt'));
        this.shifter2.setSemitones (p ('oct2Int'));
        this.octMix.set (p ('octMix'));
        this.oct2Lvl.set (clamp (p ('oct2Lvl'), 0, 1));

        this.synShape.set (p ('syShape')); this.synSub.set (p ('sySub')); this.synMix.set (p ('syMix'));
        if (ch ('syCut')) this.synCutBase = p ('syCut');
        if (ch ('syEnv')) this.synEnvAmt = p ('syEnv');
        if (ch ('syRes')) { const q = Math.max (0.5, p ('syRes')); this.synK = 1 / q; this.synComp = 1 / Math.sqrt (Math.max (1, q * 0.35)); }
        if (ch ('syGlide')) { const ms = p ('syGlide'); this.synGlide = ms < 0.5 ? 1 : 1 - Math.exp (-1000 / (sr * ms)); }

        this.phRate = p ('phRate'); this.phDepth = p ('phDepth');
        this.phFb = clamp (p ('phFb'), 0, 0.95); this.phStages = clamp (Math.round (p ('phStages')), 4, 8);

        this.choRate = p ('chRate');
        this.choDepth.set (p ('chDepth') * 0.0035 * sr);
        if (ch ('chVoices'))
        {
            const v = clamp (Math.round (p ('chVoices')), 1, 3);
            for (let i = 0; i < 3; ++i) this.choVoice[i].set (i < v ? 1 / v : 0);
        }
        if (ch ('chTone')) this.choTone.setCutoff (sr, p ('chTone'));
        if (ch ('chLows')) this.choLows.highpass (sr, p ('chLows'), 0.7071);
        this.choMix.set (p ('chMix'));

        this.dlTime.set (clamp (p ('dlTime') * 0.001 * sr, 4, this.dlBuf.length - 4));
        this.dlFb.set (clamp (p ('dlFb'), 0, 0.95));
        this.dlMix.set (p ('dlMix'));
        if (ch ('dlHicut')) this.dlDamp.setCutoff (sr, p ('dlHicut'));
        this.dlModRate = p ('dlModRate');
        if (ch ('dlModRate'))
        {
            this.dlWowCoef = 1 - Math.exp (-TWO_PI * (this.dlModRate * 0.6) / sr);
            this.dlWowAmp = 2.2 / Math.sqrt (Math.max (1e-6, this.dlWowCoef));
        }
        this.dlModDepth.set (p ('dlModDep') * 0.0025 * sr);
        this.dlSat.set (clamp (p ('dlSat'), 0, 1));
        this.dlSmear.set (clamp (p ('dlSmear'), 0, 1));

        this.tapeDrive.set (dbToGain (p ('tpDrive')));
        this.tapeMix.set (p ('tpMix'));
        if (ch ('tpSoften'))
        {
            const s = clamp (p ('tpSoften'), 0, 1);
            this.tapePre.highShelf (sr, 2800, 9 * s); this.tapePost.highShelf (sr, 2800, -9 * s);
            this.tapeRoll.setCutoff (sr, 16000 - 9000 * s);
        }
        if (ch ('tpBump')) this.tapeBump.lowShelf (sr, 80, 4.5 * p ('tpBump'));

        this.frzHold = p ('frzOn') > 0.5;
        if (this.freeze) { this.freeze.setFadeTimes (p ('frzIn'), p ('frzOut')); this.freeze.setMotion (p ('frzMotion')); }
        this.frzLevel.set (clamp (p ('frzLevel'), 0, 1));

        if (this.reverb) this.reverb.set (p ('rvSize'), p ('rvDamp'), p ('rvPre'), p ('rvMod'));
        this.revMix.set (p ('rvMix'));

        for (const k in ON) this.byp[k].set (p (ON[k]) > 0.5 ? 1 : 0);

        if (this.slot === 0)
        {
            let md = ch ('midiGate'); md = ch ('midiHold') || md; md = ch ('midiRel') || md; md = ch ('midiTrans') || md; md = ch ('midiBend') || md;
            if (md) this.segmenter.setParams (p ('midiGate'), p ('midiHold'), p ('midiRel'), Math.round (p ('midiTrans')), Math.round (p ('midiBend')));
        }

        let lat = 0;
        if (this.order.includes ('drive') && p ('odOn') > 0.5) lat += this.os.latency;
        if (this.order.includes ('octave') && p ('octOn') > 0.5) lat += this.shifter.lagReport;
        this.latency = lat;

        const o = this.order;
        this.needTracker = o.includes ('octave') || o.includes ('sub') || o.includes ('synth')
                           || (this.slot === 0 && p ('midiOn') > 0.5) || this.e.tunerSlot === this.slot;
        this.primed = true;
    }

    run (st, d, n)
    {
        switch (st)
        {
            case 'comp': { const c = this.comp, m = this.makeup; for (let i = 0; i < n; ++i) d[i] = c.process (d[i]) * m.next (); break; }
            case 'eq': { const e = this.eq; for (let i = 0; i < n; ++i) d[i] = e[4].process (e[3].process (e[2].process (e[1].process (e[0].process (d[i]))))); break; }
            case 'filter': this.filter (d, n); break;
            case 'boost': this.boost (d, n); break;
            case 'drive': this.drive (d, n); break;
            case 'amp':
                for (let i = 0; i < n; ++i) d[i] *= this.ampIn.next ();
                this.amp.process (d, n);
                for (let i = 0; i < n; ++i) d[i] *= this.ampOut.next ();
                break;
            case 'cab': this.cab.process (d, n); break;
            case 'sub': this.subStage (d, n); break;
            case 'octave':
                this.octBuf.set (d.subarray (0, n));
                this.shifter.process (d, n); this.shifter2.process (this.octBuf, n);
                for (let i = 0; i < n; ++i) d[i] += this.octBuf[i] * this.oct2Lvl.next ();
                break;
            case 'synth': this.synth (d, n); break;
            case 'phaser': this.phaser (d, n); break;
            case 'chorus': this.chorus (d, n); break;
            case 'delay': this.delay (d, n); break;
            case 'tape': this.tape (d, n); break;
        }
    }

    filter (d, n)
    {
        const k = this.fltK, c = this.fltComp, sr = this.sr;
        for (let i = 0; i < n; ++i)
        {
            const e = this.fltEnv.process (d[i] * this.fltSens);
            const fc = this.fltFrom * Math.pow (this.fltRatio, e > 1 ? 1 : e);
            this.fltCut += this.cutCoef * (fc - this.fltCut);
            d[i] = this.fltSvf.lowpass (d[i], TptSvf.g (sr, this.fltCut), k) * c;
        }
    }

    boost (d, n)
    {
        for (let i = 0; i < n; ++i)
        {
            const h = this.boostHair.next ();
            let x = d[i] * this.boostGain.next ();
            const k = 1.2 + 4 * h;
            x += (Math.tanh (x * k) / k - x) * h;
            d[i] = this.boostSheen.process (x);
        }
    }

    subStage (d, n)
    {
        this.sub.process (d, this.subBuf, n, this.subDiv);
        for (let i = 0; i < n; ++i) d[i] += this.subBuf[i] * this.subLevel.next ();
    }

    drive (d, n)
    {
        const low = this.driveLow, clean = this.driveClean;
        for (let i = 0; i < n; ++i) { const l = this.driveSplit.process (d[i]); low[i] = l; d[i] -= l; clean[i] = d[i]; }
        const sag0 = Math.min (1, this.sag);
        for (let i = 0; i < n; ++i) this.sag = this.sagEnv.process (d[i] * 2.5);
        const sag1 = Math.min (1, this.sag);
        const u = this.os.upsample (d, n), upN = n * 4;
        const d0 = this.driveAmt.cur, d1 = this.driveAmt.skip (n);
        const s0 = this.stack.cur, s1 = this.stack.skip (n);
        const inv = 1 / upN, dStep = (d1 - d0) * inv, sStep = (s1 - s0) * inv, eStep = (sag1 - sag0) * inv;
        let dd = d0, s = s0, e = sag0;
        const bite = this.biteTilt;
        if (this.driveVoice === 1)
        {
            for (let i = 0; i < upN; ++i)
            {
                const x = u[i], lo = this.greenLp.process (x), hp = bite.process (x - lo);
                let y = lo + Math.tanh (hp * dd) * 0.95;
                const y2 = Math.tanh (y * (1 + 3 * s));
                y += (y2 - y) * s * 0.9;
                u[i] = y; dd += dStep; s += sStep;
            }
        }
        else if (this.driveVoice === 2)
        {
            for (let i = 0; i < upN; ++i)
            {
                const x = u[i], b = (dd - 1) / (dd + 8);
                let dirt = bite.process (x - this.goldLp.process (x));
                dirt = Math.tanh (dirt * dd * (0.8 + 1.6 * s) * 1.4 + 0.13) - 0.12927;
                u[i] = x * (1 - 0.6 * b) + dirt * b * 1.5;
                dd += dStep; s += sStep;
            }
        }
        else
        {
            for (let i = 0; i < upN; ++i)
            {
                const head = 1 - 0.3 * e;
                let v = bite.process (u[i]) * dd * 0.55;
                v = Math.tanh (v + 0.22) - 0.21652;
                const k2 = 1.7 + 6 * s;
                u[i] = Math.tanh (v * k2 / head) * head * 1.15;
                dd += dStep; s += sStep; e += eStep;
            }
        }
        this.os.downsample (d, n);
        for (let i = 0; i < n; ++i)
        {
            const driven = this.driveDC.process (this.tone.process (d[i]));
            const bl = this.driveBlend.next ();
            d[i] = (low[i] + clean[i] * (1 - bl) + driven * bl) * this.driveLevel.next ();
        }
    }

    synth (d, n)
    {
        const k = this.synK, c = this.synComp, envOct = this.synEnvAmt * 4, sr = this.sr;
        for (let i = 0; i < n; ++i)
        {
            const key = this.synEnv.process (d[i] * 2.2), keyed = key > 1 ? 1 : key;
            this.synPeriod += this.synGlide * (this.synTarget - this.synPeriod);
            const inc = 1 / (this.synPeriod < 16 ? 16 : this.synPeriod);
            this.synPhase += inc; if (this.synPhase >= 1) this.synPhase -= 1;
            this.synSubPhase += inc * 0.5; if (this.synSubPhase >= 1) this.synSubPhase -= 1;
            const shape = this.synShape.next (), sub = this.synSub.next ();
            const ph = this.synPhase, sph = this.synSubPhase;
            const saw = 2 * ph - 1 - polyBlep (ph, inc);
            let half = ph + 0.5; if (half >= 1) half -= 1;
            const sq = (ph < 0.5 ? 1 : -1) + polyBlep (ph, inc) - polyBlep (half, inc);
            let sh = sph + 0.5; if (sh >= 1) sh -= 1;
            const ssq = (sph < 0.5 ? 1 : -1) + polyBlep (sph, inc * 0.5) - polyBlep (sh, inc * 0.5);
            let v = sq + (saw - sq) * shape + ssq * sub;
            const fc = this.synCutBase * Math.pow (2, envOct * keyed);
            this.synCut += this.cutCoef * (fc - this.synCut);
            v = this.synSvf.lowpass (v, TptSvf.g (sr, this.synCut), k) * c;
            d[i] = v * keyed * 0.8;
        }
    }

    chorus (d, n)
    {
        const mask = this.choMask, buf = this.choBuf, sr = this.sr;
        for (let i = 0; i < n; ++i)
        {
            const x = d[i];
            buf[this.choWrite & mask] = x;
            const depth = this.choDepth.next ();
            let wet = 0;
            for (let v = 0; v < 3; ++v)
            {
                this.choPhase[v] += this.choRate * choRateMult[v] / sr;
                if (this.choPhase[v] >= 1) this.choPhase[v] -= 1;
                const tap = choBaseMs[v] * 0.001 * sr + depth * choDepthMult[v] * Math.sin (TWO_PI * this.choPhase[v]);
                wet += readHermite (buf, this.choWrite - tap, mask) * this.choVoice[v].next ();
            }
            if (++this.choWrite >= buf.length * 2) this.choWrite -= buf.length;
            d[i] = x + this.choLows.process (this.choTone.process (wet) - x);
        }
    }

    phaser (d, n)
    {
        let i = 0;
        const st = this.phStages, sr = this.sr;
        while (i < n)
        {
            const chunk = Math.min (32, n - i);
            this.phLfo += this.phRate * chunk / sr; this.phLfo -= Math.floor (this.phLfo);
            const tri = 1 - 4 * Math.abs (this.phLfo - 0.5);
            this.phLfoState += 0.35 * (tri - this.phLfoState);
            const f = 200 * Math.pow (9, 0.5 * (1 + this.phLfoState) * this.phDepth);
            for (let a = 0; a < st; ++a) this.ap[a].setBreakFreq (sr, f);
            for (let k = 0; k < chunk; ++k)
            {
                const x = d[i + k];
                let y = x + this.phFb * this.phFbState;
                for (let a = 0; a < st; ++a) y = this.ap[a].process (y);
                this.phFbState = y;
                d[i + k] = 0.5 * (x + y);
            }
            i += chunk;
        }
        if (! Number.isFinite (this.phFbState)) { this.phFbState = 0; for (const a of this.ap) a.reset (); }
        this.phFbState = clamp (this.phFbState, -8, 8);
    }

    delay (d, n)
    {
        const mask = this.dlMask, buf = this.dlBuf, len = buf.length;
        for (let i = 0; i < n; ++i)
        {
            const x = d[i], t = this.dlTime.next ();
            this.dlNoise = (Math.imul (this.dlNoise, 1664525) + 1013904223) >>> 0;
            const r1 = (this.dlNoise | 0) * 4.6566129e-10;
            this.dlNoise = (Math.imul (this.dlNoise, 1664525) + 1013904223) >>> 0;
            const r2 = (this.dlNoise | 0) * 4.6566129e-10;
            this.dlWow += this.dlWowCoef * (r1 * this.dlWowAmp - this.dlWow);
            this.dlFlut += this.dlFlutCoef * (r2 * 14 - this.dlFlut);
            const mod = this.dlModDepth.next () * (clamp (this.dlWow, -2.2, 2.2) + 0.3 * this.dlFlut);
            const te = clamp (t + mod, 4, len - 4);
            const wet = readHermite (buf, this.dlWrite - te, mask);
            let fb = this.dlDamp.process (wet);
            const sm = this.dlSmear.next ();
            const dif = this.sm2.process (this.sm1.process (fb, this.smLen1), this.smLen2);
            fb += (dif - fb) * sm;
            const s = this.dlSat.next (), gS = 1 + s * 4;
            fb += (Math.tanh (fb * gS) / gS - fb) * s;
            buf[this.dlWrite & mask] = x + fb * this.dlFb.next ();
            if (++this.dlWrite >= len * 2) this.dlWrite -= len;
            d[i] = wet;
        }
    }

    tape (d, n)
    {
        for (let i = 0; i < n; ++i)
        {
            const g = this.tapeDrive.next ();
            let v = this.tapePre.process (d[i]) * g;
            v = Math.tanh (v + 0.13) - 0.12927;
            v = this.tapePost.process (v / g);
            v = this.tapeRoll.process (this.tapeBump.process (v));
            d[i] = this.tapeDC.process (v);
        }
    }

    process (mono, L, R, n)
    {
        this.update ();
        const ig = this.inGain, im = this.inMute;
        for (let i = 0; i < n; ++i) mono[i] = this.dc.process (mono[i]) * ig.next () * im.next ();

        if (this.needTracker)
        {
            this.tracker.process (mono, n);
            const per = this.tracker.period;
            this.shifter.setPeriod (per); this.shifter2.setPeriod (per); this.sub.setPeriod (per);
            this.synTarget = per;
        }
        if (this.slot === 0)
        {
            this.midiOut.length = 0;
            if (this.p ('midiOn') > 0.5) this.segmenter.process (mono, n, this.tracker.period, this.tracker.confidence, this.midiOut);
            else if (this.segmenter.sounding >= 0) this.segmenter.flush (this.midiOut);
        }

        const B = this.byp;
        for (let si = 0; si < this.order.length; ++si)
        {
            const st = this.order[si];
            if (st === 'freeze')
            {
                this.freeze.process (mono, this.frzBuf, n, this.frzHold);
                for (let i = 0; i < n; ++i) mono[i] += this.frzBuf[i] * this.frzLevel.next ();
                continue;
            }
            const byp = B[st];
            // A stage that is switched off and has finished its fade costs nothing;
            // the three that keep a line of history keep running so they come back clean.
            if (byp.cur === 0 && ! byp.smoothing && st !== 'delay' && st !== 'octave' && st !== 'chorus') continue;
            this.dry.set (mono.subarray (0, n));
            this.run (st, mono, n);
            const mix = st === 'comp' ? this.compBlend : st === 'filter' ? this.fltMix : st === 'octave' ? this.octMix
                      : st === 'synth' ? this.synMix : st === 'chorus' ? this.choMix : st === 'delay' ? this.dlMix
                      : st === 'tape' ? this.tapeMix : null;
            const dry = this.dry;
            if (mix) for (let i = 0; i < n; ++i) { const g = byp.next () * mix.next (); mono[i] = dry[i] + (mono[i] - dry[i]) * g; }
            else for (let i = 0; i < n; ++i) { const g = byp.next (); mono[i] = dry[i] + (mono[i] - dry[i]) * g; }
        }

        this.carrier.set (mono.subarray (0, n));

        const lo = this.liveOut;
        if (this.hasReverb)
        {
            this.reverb.process (mono, this.revL, this.revR, n);
            const rb = B.reverb, rm = this.revMix, rl = this.revL, rr = this.revR;
            for (let i = 0; i < n; ++i)
            {
                const g = rb.next () * rm.next (), out = lo.next (), m = mono[i];
                L[i] = (m + (rl[i] - m) * g) * out;
                R[i] = (m + (rr[i] - m) * g) * out;
            }
        }
        else
            for (let i = 0; i < n; ++i) { const v = mono[i] * lo.next (); L[i] = v; R[i] = v; }
    }
}

// --- The mic lanes ------------------------------------------------------
// Autotune: the mic's own tracker steers a pitch-synchronous shifter onto the
// nearest note of the chosen key and scale. Vocoder: a native channel vocoder,
// the mic as modulator and the bass after its chain as carrier.
const SCALES = [
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11],   // chromatic
    [0, 2, 4, 5, 7, 9, 11],                   // major
    [0, 2, 3, 5, 7, 8, 10],                   // minor
    [0, 2, 4, 7, 9],                          // major pentatonic
    [0, 3, 5, 7, 10],                         // minor pentatonic
    [0, 3, 5, 6, 7, 10],                      // blues
];

export class VocalLanes
{
    constructor (engine, sr)
    {
        this.e = engine; this.sr = sr;
        const B = 128;
        const R = (s = 0.02) => { const r = new Ramp (0); r.reset (sr, s); return r; };
        this.gain = R (); this.tuneLevel = R (); this.vocLevel = R ();
        this.tuneRamp = R (0.015); this.vocRamp = R (0.015);
        this.gateEnv = new EnvFollower (); this.gateEnv.setTimes (sr, 1, 60);
        this.gateState = 0; this.gateCoef = 1 - Math.exp (-1 / (0.008 * sr)); this.gateOpen = false;
        this.gated = new Float32Array (B); this.tuned = new Float32Array (B); this.voc = new Float32Array (B);
        this.tracker = new PitchTracker (sr, { decRateTarget: 8000, fMin: 70, fMax: 1000, window: 256 });
        this.shifter = new OctaveShifter (sr);
        this.shift = 0;
        // vocoder bank
        this.nb = 20;
        const lo = 120, hi = 7000;
        this.bands = [];
        for (let b = 0; b < this.nb; ++b)
        {
            const f = lo * Math.pow (hi / lo, b / (this.nb - 1));
            const mk = () => { const q = new Biquad (); q.bandpass (sr, f, 7); return q; };
            this.bands.push ({ m1: mk (), m2: mk (), c1: mk (), c2: mk (), me: 0, ce: 0 });
        }
        this.sib = new Biquad (); this.sib.highpass (sr, 5000, 0.7071);
        this.envAtk = 1 - Math.exp (-1 / (0.003 * sr));
        this.envRel = 1 - Math.exp (-1 / (0.06 * sr));
        this.lastRel = -1;
    }

    process (mic, carrier, out, n)
    {
        const e = this.e, g = (id) => e.get (0, id);
        this.gain.set (dbToGain (g ('vocalGain')));
        this.tuneLevel.set (dbToGain (g ('vocalLevel')));
        this.vocLevel.set (dbToGain (g ('vocoderLvl')));
        const tuneOn = g ('vocalOn') > 0.5, vocOn = g ('vocoderOn') > 0.5;
        this.tuneRamp.set (tuneOn ? 1 : 0); this.vocRamp.set (vocOn ? 1 : 0);

        // gate: open at Gate, shut 6 dB under it; the detector listens ahead of the fader
        const openAt = dbToGain (g ('vocalGate'), -200), closeAt = openAt * 0.5;
        const gd = this.gated;
        let peak = 0;
        for (let i = 0; i < n; ++i)
        {
            const x = mic[i] * this.gain.next ();
            const env = this.gateEnv.process (mic[i]);
            if (! this.gateOpen && env > openAt) this.gateOpen = true;
            else if (this.gateOpen && env < closeAt) this.gateOpen = false;
            this.gateState += this.gateCoef * ((this.gateOpen ? 1 : 0) - this.gateState);
            gd[i] = x * this.gateState;
            const a = Math.abs (x); if (a > peak) peak = a;
        }
        this.peak = peak;
        out.fill (0, 0, n);

        if (tuneOn || this.tuneRamp.smoothing)
        {
            const t = this.tuned;
            t.set (gd.subarray (0, n));
            this.tracker.process (t, n);
            if (this.tracker.confidence > 0.6)
            {
                const midi = 69 + 12 * Math.log2 (this.tracker.frequency / 440);
                const key = Math.round (g ('tuneKey')), scale = SCALES[clamp (Math.round (g ('tuneScale')), 0, SCALES.length - 1)];
                let best = midi, bestD = 1e9;
                const base = Math.floor (midi) - 12;
                for (let m = base; m <= base + 24; ++m)
                {
                    if (! scale.includes (((m - key) % 12 + 12) % 12)) continue;
                    const dd = Math.abs (m - midi);
                    if (dd < bestD) { bestD = dd; best = m; }
                }
                const wanted = (best - midi) * g ('tuneAmount');
                const ms = g ('tuneSpeed');
                const coef = ms < 1 ? 1 : 1 - Math.exp (-n * 1000 / (this.sr * ms));
                this.shift += coef * (wanted - this.shift);
            }
            this.shifter.setPeriod (this.tracker.period);
            this.shifter.setSemitones (this.shift);
            this.shifter.process (t, n);
            for (let i = 0; i < n; ++i) out[i] += t[i] * this.tuneLevel.next () * this.tuneRamp.next ();
        }

        if (vocOn || this.vocRamp.smoothing)
        {
            const rel = g ('vocRelease');
            if (rel !== this.lastRel) { this.lastRel = rel; this.envRel = 1 - Math.exp (-1 / (Math.max (5, rel) * 0.001 * this.sr)); }
            const v = this.voc, bands = this.bands, aA = this.envAtk, aR = this.envRel, sib = g ('vocSib');
            for (let i = 0; i < n; ++i)
            {
                const m = gd[i], c = carrier[i];
                let acc = 0;
                for (let b = 0; b < this.nb; ++b)
                {
                    const bd = bands[b];
                    const mb = bd.m2.process (bd.m1.process (m));
                    const cb = bd.c2.process (bd.c1.process (c));
                    const ma = Math.abs (mb), ca = Math.abs (cb);
                    bd.me += (ma > bd.me ? aA : aR) * (ma - bd.me);
                    bd.ce += (ca > bd.ce ? aA : 0.002) * (ca - bd.ce);
                    acc += cb * Math.min (8, bd.me / (bd.ce + 1e-4));
                }
                v[i] = acc * 0.35 + this.sib.process (m) * sib;
            }
            for (let i = 0; i < n; ++i) out[i] += v[i] * this.vocLevel.next () * this.vocRamp.next ();
        }
    }
}

// --- Keys: a small built-in polysynth -----------------------------------
// The original hosts an Audio Unit; a browser cannot, so the keys lane plays
// one of a handful of built-in patches instead.
export const PATCHES = {
    'Warm pad':  { osc: 'saw2',  cutoff: 1400, res: 0.6, atk: 0.35, dec: 0.4, sus: 0.85, rel: 0.9, env: 1.2, gain: 0.32 },
    'Strings':   { osc: 'saw3',  cutoff: 2600, res: 0.4, atk: 0.18, dec: 0.3, sus: 0.9,  rel: 0.6, env: 0.6, gain: 0.26 },
    'Organ':     { osc: 'organ', cutoff: 6000, res: 0.2, atk: 0.006, dec: 0.1, sus: 1,  rel: 0.06, env: 0,  gain: 0.28 },
    'E-piano':   { osc: 'fm',    cutoff: 5000, res: 0.2, atk: 0.003, dec: 1.4, sus: 0.25, rel: 0.4, env: 0, gain: 0.42 },
    'Soft lead': { osc: 'sq',    cutoff: 1800, res: 1.2, atk: 0.02, dec: 0.3, sus: 0.8,  rel: 0.25, env: 1.5, gain: 0.25 },
    'Synth bass':{ osc: 'sawsub',cutoff: 500,  res: 2.0, atk: 0.004, dec: 0.25, sus: 0.5, rel: 0.12, env: 2.5, gain: 0.4 },
};

export class Instrument
{
    constructor (sr)
    {
        this.sr = sr; this.patch = PATCHES['Warm pad'];
        this.voices = Array.from ({ length: 8 }, () => ({ note: -1, vel: 0, ph: [0, 0, 0, 0], env: 0, stage: 0, age: 0, svf: new TptSvf (), mod: 0, held: false }));
        this.bend = 0; this.expr = 1; this.exprTarget = 1; this.age = 0; this.out = new Float32Array (128);
    }
    setPatch (name) { if (PATCHES[name]) this.patch = PATCHES[name]; }
    noteOn (note, vel)
    {
        let v = this.voices.find ((x) => x.note === note) || this.voices.find ((x) => x.stage === 0);
        if (! v) v = this.voices.reduce ((a, b) => (a.age < b.age ? a : b));
        v.note = note; v.vel = vel / 127; v.stage = 1; v.age = ++this.age; v.held = true;
        if (v.env === 0) { v.ph.fill (0); v.svf.reset (); v.mod = 1; }
    }
    noteOff (note) { for (const v of this.voices) if (v.note === note && v.stage > 0 && v.stage < 4) v.stage = 4; }
    panic () { for (const v of this.voices) { v.stage = 0; v.env = 0; v.note = -1; } }
    process (out, n, gain)
    {
        const P = this.patch, sr = this.sr;
        const aStep = 1 / Math.max (1, P.atk * sr), dCoef = 1 - Math.exp (-1 / (Math.max (0.001, P.dec) * sr * 0.3));
        const rCoef = 1 - Math.exp (-1 / (Math.max (0.001, P.rel) * sr * 0.3));
        this.expr += 0.05 * (this.exprTarget - this.expr);
        const k = 1 / Math.max (0.5, P.res + 0.5);
        out.fill (0, 0, n);
        for (const v of this.voices)
        {
            if (v.stage === 0) continue;
            const hz = 440 * Math.pow (2, (v.note - 69 + this.bend) / 12), inc = hz / sr;
            for (let i = 0; i < n; ++i)
            {
                if (v.stage === 1) { v.env += aStep; if (v.env >= 1) { v.env = 1; v.stage = 2; } }
                else if (v.stage === 2) { v.env += dCoef * (P.sus - v.env); }
                else if (v.stage === 4) { v.env -= rCoef * v.env; if (v.env < 1e-4) { v.env = 0; v.stage = 0; v.note = -1; break; } }
                let s = 0;
                const ph = v.ph;
                switch (P.osc)
                {
                    case 'saw2':
                        ph[0] = (ph[0] + inc * 1.004) % 1; ph[1] = (ph[1] + inc * 0.996) % 1;
                        s = (2 * ph[0] - 1 - polyBlep (ph[0], inc)) + (2 * ph[1] - 1 - polyBlep (ph[1], inc)); s *= 0.5; break;
                    case 'saw3':
                        ph[0] = (ph[0] + inc * 1.006) % 1; ph[1] = (ph[1] + inc) % 1; ph[2] = (ph[2] + inc * 0.994) % 1;
                        s = (2 * ph[0] - 1 - polyBlep (ph[0], inc)) + (2 * ph[1] - 1 - polyBlep (ph[1], inc)) + (2 * ph[2] - 1 - polyBlep (ph[2], inc)); s *= 0.4; break;
                    case 'organ':
                        ph[0] = (ph[0] + inc) % 1;
                        s = Math.sin (TWO_PI * ph[0]) + 0.6 * Math.sin (2 * TWO_PI * ph[0]) + 0.35 * Math.sin (3 * TWO_PI * ph[0]) + 0.2 * Math.sin (4 * TWO_PI * ph[0]); s *= 0.45; break;
                    case 'fm':
                        ph[0] = (ph[0] + inc) % 1; ph[1] = (ph[1] + inc * 14) % 1;
                        v.mod *= 0.99985;
                        s = Math.sin (TWO_PI * ph[0] + 1.2 * v.mod * v.vel * Math.sin (TWO_PI * ph[1]) + 0.8 * Math.sin (TWO_PI * ph[0])); break;
                    case 'sq':
                    {
                        ph[0] = (ph[0] + inc) % 1; let h = ph[0] + 0.5; if (h >= 1) h -= 1;
                        s = (ph[0] < 0.5 ? 1 : -1) + polyBlep (ph[0], inc) - polyBlep (h, inc); s *= 0.6; break;
                    }
                    case 'sawsub':
                    {
                        ph[0] = (ph[0] + inc) % 1; ph[1] = (ph[1] + inc * 0.5) % 1; let h = ph[1] + 0.5; if (h >= 1) h -= 1;
                        s = (2 * ph[0] - 1 - polyBlep (ph[0], inc)) * 0.6 + ((ph[1] < 0.5 ? 1 : -1) + polyBlep (ph[1], inc * 0.5) - polyBlep (h, inc * 0.5)) * 0.5; break;
                    }
                }
                const fc = Math.min (sr * 0.45, P.cutoff * Math.pow (2, P.env * v.env * v.vel));
                s = v.svf.lowpass (s, TptSvf.g (sr, fc), k);
                out[i] += s * v.env * (0.35 + 0.65 * v.vel);
            }
        }
        const gg = gain * this.expr * P.gain;
        for (let i = 0; i < n; ++i) out[i] *= gg;
    }
}

// --- The looper: port of Looper.cpp -------------------------------------
export class Looper
{
    static beatsPerBar = 4; static countIn = 4; static maxBars = 16; static maxSeconds = 64;
    constructor (sr)
    {
        this.rate = sr;
        this.bpm = 120; this.bars = 4; this.taps = [];
        this.capacity = Math.ceil (64 * sr) + 1;
        this.allocated = false;
        this.fadeLen = Math.max (8, Math.round (0.008 * sr));
        this.preRoll = [new Float32Array (this.fadeLen), new Float32Array (this.fadeLen)];
        this.queue = [];
        const R = (s, v) => { const r = new Ramp (v); r.reset (sr, s); return r; };
        this.playGain = R (0.01, 0); this.levelGain = R (0.02, 1); this.dubGain = R (0.005, 0);
        this.layerGain = [R (0.01, 1), R (0.01, 1)];
        this.job = ['none', 'none']; this.jobPos = [0, 0]; this.jobLen = [0, 0];
        this.clickLen = Math.floor (0.06 * sr); this.clickDecay = Math.exp (-1 / (0.012 * sr));
        this.toInitial ();
    }
    toInitial ()
    {
        this.state = 'empty'; this.loopLen = 0; this.countLen = 0; this.totalBeats = 0; this.beatLen = 0;
        this.clock = 0; this.nextBeat = 0; this.capturing = false; this.latchLatency = false; this.latency = 0;
        this.source = -1; this.playPos = 0; this.pausedAt = 0; this.newest = -1; this.undone = false;
        this.dubbing = false; this.wantLayer = false; this.dubTail = 0; this.clickAge = -1;
    }
    ensure ()
    {
        if (this.allocated) return true;
        this.base = [new Float32Array (this.capacity), new Float32Array (this.capacity)];
        this.layers = [[new Float32Array (this.capacity), new Float32Array (this.capacity)],
                       [new Float32Array (this.capacity), new Float32Array (this.capacity)]];
        this.allocated = true;
        return true;
    }
    static maxBarsAt (bpm)
    {
        const bar = 4 * 60 / Math.max (1, bpm);
        return clamp (Math.floor (64 / bar + 1e-6), 1, 16);
    }
    tapTempo (nowMs)
    {
        if (this.state !== 'empty') return;
        const t = this.taps;
        if (t.length > 0 && nowMs - t[t.length - 1] > 2500) t.length = 0;
        if (t.length === 5) t.shift ();
        t.push (nowMs);
        if (t.length < 2) return;
        const span = t[t.length - 1] - t[0];
        if (span <= 0) return;
        this.bpm = clamp (Math.round (60000 * (t.length - 1) / span), 40, 240);
        this.bars = Math.min (this.bars, Looper.maxBarsAt (this.bpm));
    }
    tapBars ()
    {
        if (this.state !== 'empty') return;
        const next = this.bars + 1;
        this.bars = next > Looper.maxBarsAt (this.bpm) ? 1 : next;
    }
    setTempo (bpm) { if (this.state === 'empty') { this.bpm = clamp (Math.round (bpm), 40, 240); this.bars = Math.min (this.bars, Looper.maxBarsAt (this.bpm)); } }
    setBars (b) { if (this.state === 'empty') this.bars = clamp (Math.round (b), 1, Looper.maxBarsAt (this.bpm)); }
    press (cmd, arg) { if (cmd === 'record') this.ensure (); this.queue.push ([cmd, arg]); }
    beginBlock ()
    {
        if (! this.allocated) { this.queue.length = 0; return -1; }
        while (this.queue.length) { const [c, a] = this.queue.shift (); this.handle (c, a); }
        return this.state !== 'empty' ? this.source : -1;
    }
    handle (cmd, arg)
    {
        const st = this.state;
        if (cmd === 'record')
        {
            if (st === 'empty')
            {
                const length = Math.min (this.bars, Looper.maxBarsAt (this.bpm));
                const beat = this.rate * 60 / this.bpm;
                const samples = Math.round (beat * 4 * length);
                if (samples > this.capacity || samples <= 4 * this.fadeLen) return;
                this.beatLen = beat; this.countLen = Math.round (beat * 4); this.loopLen = samples;
                this.totalBeats = 4 + 4 * length; this.clock = 0; this.nextBeat = 0; this.playPos = 0;
                this.capturing = true; this.latchLatency = true; this.source = arg;
                this.playGain.set (0); this.playGain.snap (); this.state = 'countIn';
            }
            else if (st === 'countIn') this.toEmpty ();
            else if (st === 'recording') this.closeTake ();
            else if (st === 'paused' || st === 'playing')
            {
                if (st === 'paused') { this.playPos = this.pausedAt; this.playGain.set (1); this.state = 'playing'; }
                this.source = arg; this.wantLayer = true; this.latchLatency = ! this.capturing;
            }
            else if (st === 'overdubbing') this.endLayer (true);
        }
        else if (cmd === 'play')
        {
            if (st === 'countIn' || st === 'recording') this.toEmpty ();
            else if (st === 'playing' || st === 'overdubbing')
            {
                this.endLayer (false); this.wantLayer = false; this.pausedAt = this.playPos;
                this.state = 'paused'; this.playGain.set (0);
            }
            else if (st === 'paused') { this.playPos = this.pausedAt; this.playGain.set (1); this.state = 'playing'; }
        }
        else if (cmd === 'undo')
        {
            if (st === 'empty' || st === 'countIn' || st === 'recording') return;
            if (this.wantLayer) { this.wantLayer = false; return; }
            if (this.dubbing) { this.endLayer (false); this.undone = true; }
            else if (this.newest >= 0) this.undone = ! this.undone;
            if (this.newest >= 0) this.layerGain[this.newest].set (this.undone ? 0 : 1);
        }
        else if (cmd === 'clear') this.toEmpty ();
    }
    toEmpty ()
    {
        if (this.state !== 'countIn' && this.state !== 'recording')
            for (let b = 0; b < 2; ++b)
                if (b === this.newest || this.job[b] !== 'none')
                {
                    this.jobLen[b] = this.job[b] !== 'none' ? Math.max (this.jobLen[b], this.loopLen) : this.loopLen;
                    this.job[b] = 'wipe'; this.jobPos[b] = 0;
                }
        this.newest = -1; this.undone = this.dubbing = this.wantLayer = this.capturing = false;
        this.dubTail = 0; this.clickAge = -1; this.source = -1; this.state = 'empty';
        this.playGain.set (0);
    }
    closeTake ()
    {
        const barLen = this.beatLen * 4, planned = (this.totalBeats - 4) / 4;
        const pos = this.clock - this.countLen;
        const kept = clamp (Math.round (pos / barLen), 1, planned);
        if (kept === planned) return;
        const length = Math.round (barLen * kept), written = this.clock - this.latency - this.countLen;
        for (let k = 0; k < this.fadeLen; ++k)
        {
            const idx = length - this.fadeLen + k;
            if (idx >= written) break;
            const w = (k + 1) / (this.fadeLen + 1);
            for (let c = 0; c < 2; ++c) this.base[c][idx] += w * (this.preRoll[c][k] - this.base[c][idx]);
        }
        this.loopLen = length; this.totalBeats = 4 + 4 * kept; this.bars = kept;
        if (written >= length) this.capturing = false;
        if (pos >= length)
        {
            this.state = 'playing'; this.playPos = pos - length;
            this.playGain.set (0); this.playGain.snap (); this.playGain.set (1);
        }
    }
    tryStartLayer ()
    {
        let target = -1;
        if (this.newest >= 0)
        {
            target = 1 - this.newest;
            if (this.job[target] !== 'none') return;
            this.job[this.newest] = this.undone ? 'wipe' : 'fold'; this.jobPos[this.newest] = 0; this.jobLen[this.newest] = this.loopLen;
        }
        else
        {
            target = this.job[0] === 'none' ? 0 : this.job[1] === 'none' ? 1 : -1;
            if (target < 0) return;
        }
        this.newest = target; this.undone = false;
        this.layerGain[target].set (1); this.layerGain[target].snap ();
        this.dubbing = true; this.dubTail = 0; this.wantLayer = false; this.state = 'overdubbing';
    }
    endLayer (keepTail)
    {
        if (this.dubbing) this.dubTail = keepTail ? this.latency : 0;
        this.dubbing = false;
        if (this.state === 'overdubbing') this.state = 'playing';
    }
    runJobs (budget)
    {
        for (let b = 0; b < 2 && budget > 0; ++b)
        {
            if (this.job[b] === 'none') continue;
            if (this.state === 'empty' ? this.playGain.smoothing : this.layerGain[b].smoothing) continue;
            const count = Math.min (budget, this.jobLen[b] - this.jobPos[b]);
            for (let c = 0; c < 2; ++c)
            {
                const layer = this.layers[b][c], base = this.base[c], p0 = this.jobPos[b];
                if (this.job[b] === 'fold') for (let i = 0; i < count; ++i) base[p0 + i] += layer[p0 + i];
                layer.fill (0, p0, p0 + count);
            }
            this.jobPos[b] += count; budget -= count;
            if (this.jobPos[b] >= this.jobLen[b]) this.job[b] = 'none';
        }
    }
    process (srcL, srcR, outL, outR, n, latencySamples, level)
    {
        if (! this.allocated) return;
        if (this.latchLatency)
        {
            this.latency = clamp (latencySamples, 0, Math.floor (Math.min (this.countLen, this.loopLen) / 2));
            this.latchLatency = false;
        }
        this.runJobs (n * 64);
        if (this.wantLayer && this.state === 'playing') this.tryStartLayer ();
        this.levelGain.set (level);
        const [bL, bR] = this.base, l0 = this.layers[0], l1 = this.layers[1];
        for (let i = 0; i < n; ++i)
        {
            const inL = srcL[i], inR = srcR[i];
            let addL = 0, addR = 0;
            if (this.state === 'countIn' && this.clock >= this.countLen) this.state = 'recording';
            if (this.state === 'recording' && this.clock >= this.countLen + this.loopLen)
            { this.state = 'playing'; this.playPos = 0; this.playGain.set (1); this.playGain.snap (); }
            const counting = this.state === 'countIn' || this.state === 'recording';
            if (counting && this.nextBeat < this.totalBeats && this.clock >= Math.round (this.nextBeat * this.beatLen))
            {
                const accent = this.nextBeat % 4 === 0;
                this.clickAge = 0; this.clickPhase = 0;
                this.clickStep = TWO_PI * (accent ? 1500 : 1000) / this.rate;
                this.clickEnv = accent ? 0.4 : 0.28;
                ++this.nextBeat;
            }
            if (this.capturing)
            {
                const g = this.clock - this.latency - this.countLen;
                if (g >= -this.fadeLen && g < 0) { this.preRoll[0][g + this.fadeLen] = inL; this.preRoll[1][g + this.fadeLen] = inR; }
                else if (g >= 0 && g < this.loopLen)
                {
                    let vL = inL, vR = inR;
                    const tail = g - (this.loopLen - this.fadeLen);
                    if (tail >= 0)
                    {
                        const w = (tail + 1) / (this.fadeLen + 1);
                        vL += w * (this.preRoll[0][tail] - vL); vR += w * (this.preRoll[1][tail] - vR);
                    }
                    bL[g] = vL; bR[g] = vR;
                    if (g === this.loopLen - 1) this.capturing = false;
                }
            }
            if (counting || this.capturing) ++this.clock;
            const lg = this.levelGain.next ();
            if (this.state === 'playing' || this.state === 'overdubbing' || this.playGain.smoothing)
            {
                const pg = this.playGain.next () * lg, g0 = this.layerGain[0].next (), g1 = this.layerGain[1].next ();
                const p = this.playPos;
                addL = (bL[p] + l0[0][p] * g0 + l1[0][p] * g1) * pg;
                addR = (bR[p] + l0[1][p] * g0 + l1[1][p] * g1) * pg;
                const feeding = this.dubbing || this.dubTail > 0;
                if (this.newest >= 0 && (feeding || this.dubGain.smoothing || this.dubGain.cur > 0))
                {
                    if (! this.dubbing && this.dubTail > 0) --this.dubTail;
                    this.dubGain.set (feeding ? 1 : 0);
                    const dg = this.dubGain.next ();
                    let w = p - this.latency; if (w < 0) w += this.loopLen;
                    this.layers[this.newest][0][w] += inL * dg; this.layers[this.newest][1][w] += inR * dg;
                }
                if (++this.playPos >= this.loopLen) this.playPos = 0;
            }
            if (this.clickAge >= 0)
            {
                const c = Math.sin (this.clickPhase) * this.clickEnv;
                this.clickPhase += this.clickStep; this.clickEnv *= this.clickDecay;
                addL += c; addR += c;
                if (++this.clickAge >= this.clickLen) this.clickAge = -1;
            }
            outL[i] += addL; outR[i] += addR;
        }
    }
    snapshot ()
    {
        const counting = this.state === 'countIn' || this.state === 'recording';
        const pos = counting ? (this.state === 'countIn' ? this.clock : this.clock - this.countLen) : this.playPos;
        return { state: this.state, bpm: this.bpm, bars: this.bars, source: this.source, pos, beatLen: this.beatLen,
                 loopLen: this.loopLen, hasLayer: this.newest >= 0, undone: this.undone };
    }
}

// --- The engine: everything summed --------------------------------------
export class Engine
{
    constructor (sr)
    {
        this.sr = sr;
        this.isChannel = Object.create (null);
        this.P = Array.from ({ length: 8 }, () => Object.create (null));
        this.chains = Array.from ({ length: 8 }, (_, s) => new Chain (this, s, sr));
        this.strips = [];
        this.vocal = new VocalLanes (this, sr);
        this.instrument = new Instrument (sr);
        this.looper = new Looper (sr);
        this.limiter = new MasterLimiter (sr);
        this.masterGain = new Ramp (1); this.masterGain.reset (sr, 0.02);
        this.instGain = 0;
        this.tunerSlot = -1;
        this.roundTrip = 0; this.keysRoundTrip = 0;
        const B = 128;
        this.mono = new Float32Array (B); this.L = new Float32Array (B); this.R = new Float32Array (B);
        this.liveL = new Float32Array (B); this.liveR = new Float32Array (B);
        this.tapL = new Float32Array (B); this.tapR = new Float32Array (B);
        this.zero = new Float32Array (B); this.keysBuf = new Float32Array (B); this.laneBuf = new Float32Array (B);
        this.notes = [];
        this.midiEvents = [];
        this.peaks = new Float32Array (8); this.inPeaks = new Float32Array (2);
        this.outPeak = [0, 0]; this.grDb = 0;
    }
    get (slot, id) { const P = this.P; return this.isChannel[id] ? P[slot][id] : P[0][id]; }
    set (slot, id, v) { this.P[this.isChannel[id] ? slot : 0][id] = v; }
    init (specs, values)
    {
        for (const s of specs) { this.isChannel[s.id] = s.channel; for (let c = 0; c < 8; ++c) this.P[c][s.id] = s.def; }
        if (values) this.load (values);
    }
    load (values) { values.forEach ((obj, c) => { if (obj) for (const id in obj) this.P[c][id] = obj[id]; }); }
    setStrips (strips)
    {
        this.strips = strips.slice ().sort ((a, b) => a.slot - b.slot);
        for (const s of this.strips) this.chains[s.slot].setBoard (s.board);
        for (let c = 0; c < 8; ++c) if (! this.strips.find ((s) => s.slot === c)) this.chains[c].setBoard ([]);
    }
    note (on, note, vel) { this.notes.push (on ? 0x90 : 0x80, note, vel); }

    process (inputs, outL, outR, n)
    {
        const in0 = inputs[0] || this.zero, in1 = inputs[1] || in0;
        const ins = [in0, in1];
        for (let c = 0; c < 2; ++c)
        {
            let p = this.inPeaks[c] * 0.97; const x = ins[c];
            for (let i = 0; i < n; ++i) { const a = Math.abs (x[i]); if (a > p) p = a; }
            this.inPeaks[c] = p;
        }
        const live = [this.liveL, this.liveR];
        this.liveL.fill (0); this.liveR.fill (0);
        const loopSource = this.looper.beginBlock ();
        this.tapL.fill (0); this.tapR.fill (0);
        const mono = this.mono, L = this.L, R = this.R;
        let loopLatency = 0;

        const addStrip = (slot) =>
        {
            const ch = this.chains[slot];
            let p = this.peaks[slot] * 0.97;
            for (let i = 0; i < n; ++i)
            {
                this.liveL[i] += L[i]; this.liveR[i] += R[i];
                const a = Math.max (Math.abs (L[i]), Math.abs (R[i])); if (a > p) p = a;
            }
            this.peaks[slot] = p;
            if (loopSource === slot) { this.tapL.set (L.subarray (0, n)); this.tapR.set (R.subarray (0, n)); loopLatency = ch.latency; }
        };

        let micStrip = null, keysStrip = false;
        for (const s of this.strips)
        {
            if (s.kind === 'mic') { micStrip = s; continue; }
            if (s.kind === 'keys') { keysStrip = true; continue; }
            if (s.kind === 'master') continue;
            const src = s.source >= 0 && s.source < 2 ? ins[s.source] : this.zero;
            mono.set (src.subarray (0, n));
            this.chains[s.slot].process (mono, L, R, n);
            addStrip (s.slot);
        }

        // tracker notes and the keys player's hands, one stream
        const ev = this.chains[0].midiOut;
        const merged = this.midiEvents;
        merged.length = 0;
        for (let i = 0; i < ev.length; ++i) merged.push (ev[i]);
        const inst = this.instrument;
        const exprOn = this.get (0, 'midiExpr') > 0.5, bendRange = this.get (0, 'midiBend');
        for (let i = 0; i < ev.length; i += 3)
        {
            const t = ev[i];
            if (t === 0x90) inst.noteOn (ev[i + 1], ev[i + 2]);
            else if (t === 0x80) inst.noteOff (ev[i + 1]);
            else if (t === 0xE0) inst.bend = bendRange * (ev[i + 1] - 8192) / 8192;
            else if (t === 0xB0 && exprOn) inst.exprTarget = ev[i + 1] / 127;
        }
        for (let i = 0; i < this.notes.length; i += 3)
        {
            if (this.notes[i] === 0x90) { inst.exprTarget = 1; inst.noteOn (this.notes[i + 1], this.notes[i + 2]); }
            else inst.noteOff (this.notes[i + 1]);
        }
        this.notes.length = 0;

        if (keysStrip && this.get (0, 'instOn') > 0.5)
        {
            const target = dbToGain (this.get (0, 'instLevel'));
            this.instGain += 0.25 * (target - this.instGain);
            inst.process (mono, n, this.instGain);
            this.chains[2].process (mono, L, R, n);
            addStrip (2);
        }
        else if (keysStrip) this.peaks[2] *= 0.9;

        if (micStrip)
        {
            const src = micStrip.source >= 0 && micStrip.source < 2 ? ins[micStrip.source] : this.zero;
            this.vocal.process (src, this.chains[0].carrier, mono, n);
            this.chains[1].process (mono, L, R, n);
            addStrip (1);
        }

        const rt = loopSource === 2 ? this.keysRoundTrip : this.roundTrip;
        this.looper.process (this.tapL, this.tapR, this.liveL, this.liveR, n, rt + Math.round (loopLatency),
                             dbToGain (this.get (0, 'loopLevel')));

        this.masterGain.set (this.get (0, 'masterMute') > 0.5 ? 0 : dbToGain (this.get (0, 'masterGain')));
        let worst = 1, pl = this.outPeak[0] * 0.97, pr = this.outPeak[1] * 0.97;
        for (let i = 0; i < n; ++i)
        {
            const g = this.masterGain.next ();
            outL[i] = this.liveL[i] * g; outR[i] = this.liveR[i] * g;
            const lg = this.limiter.process (null, i, outL, outR);
            if (lg < worst) worst = lg;
            const al = Math.abs (outL[i]), ar = Math.abs (outR[i]);
            if (al > pl) pl = al; if (ar > pr) pr = ar;
        }
        this.outPeak[0] = pl; this.outPeak[1] = pr;
        const gr = worst < 1 ? -20 * Math.log10 (worst) : 0;
        this.grDb = Math.max (gr, this.grDb * 0.9);
    }
}
