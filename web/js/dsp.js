// LiveRig DSP building blocks, ported from Source/dsp. Everything here runs on
// the audio thread inside the AudioWorklet: no allocation after construction,
// no logging, no locks. Each class mirrors its C++ namesake closely enough
// that the comments in the original still explain what it is doing.

export const TWO_PI = Math.PI * 2;

export function dbToGain (db, floor = -60) { return db <= floor ? 0 : Math.pow (10, db / 20); }
export function nextPow2 (n) { let p = 1; while (p < n) p <<= 1; return p; }
export const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

// juce::SmoothedValue<float>, linear: a ramp to the target over a fixed time.
export class Ramp
{
    constructor (v = 0) { this.cur = v; this.tgt = v; this.step = 0; this.left = 0; this.len = 1; }
    reset (sr, secs) { this.len = Math.max (1, Math.floor (sr * secs)); this.cur = this.tgt; this.left = 0; }
    set (t)
    {
        if (t === this.tgt) return;
        this.tgt = t;
        if (this.len <= 1) { this.cur = t; this.left = 0; return; }
        this.left = this.len;
        this.step = (t - this.cur) / this.len;
    }
    next ()
    {
        if (this.left > 0) { this.cur += this.step; if (--this.left === 0) this.cur = this.tgt; }
        return this.cur;
    }
    skip (n)
    {
        if (this.left > 0)
        {
            if (n >= this.left) { this.cur = this.tgt; this.left = 0; }
            else { this.cur += this.step * n; this.left -= n; }
        }
        return this.cur;
    }
    snap () { this.cur = this.tgt; this.left = 0; }
    get smoothing () { return this.left > 0; }
}

// 4-point, 3rd-order Hermite read on a masked circular buffer.
export function readHermite (buf, pos, mask)
{
    const i = Math.floor (pos);
    const f = pos - i;
    const xm1 = buf[(i - 1) & mask], x0 = buf[i & mask], x1 = buf[(i + 1) & mask], x2 = buf[(i + 2) & mask];
    const c = (x1 - xm1) * 0.5, v = x0 - x1, w = c + v;
    const a = w + v + (x2 - x0) * 0.5, b = w + a;
    return ((a * f - b) * f + c) * f + x0;
}

const clampFreq = (sr, f) => { const n = f / sr; return n < 0.0001 ? 0.0001 : n > 0.49 ? 0.49 : n; };

export class Biquad
{
    constructor () { this.b0 = 1; this.b1 = 0; this.b2 = 0; this.a1 = 0; this.a2 = 0; this.z1 = 0; this.z2 = 0; }
    process (x)
    {
        const y = this.b0 * x + this.z1;
        this.z1 = this.b1 * x - this.a1 * y + this.z2;
        this.z2 = this.b2 * x - this.a2 * y;
        return y;
    }
    reset () { this.z1 = this.z2 = 0; }
    coeffs (b0, b1, b2, a0, a1, a2)
    {
        const inv = 1 / a0;
        this.b0 = b0 * inv; this.b1 = b1 * inv; this.b2 = b2 * inv; this.a1 = a1 * inv; this.a2 = a2 * inv;
    }
    lowpass (sr, f, q)
    {
        const w0 = TWO_PI * clampFreq (sr, f), cs = Math.cos (w0), al = Math.sin (w0) / (2 * q);
        this.coeffs ((1 - cs) / 2, 1 - cs, (1 - cs) / 2, 1 + al, -2 * cs, 1 - al);
    }
    highpass (sr, f, q)
    {
        const w0 = TWO_PI * clampFreq (sr, f), cs = Math.cos (w0), al = Math.sin (w0) / (2 * q);
        this.coeffs ((1 + cs) / 2, -(1 + cs), (1 + cs) / 2, 1 + al, -2 * cs, 1 - al);
    }
    bandpass (sr, f, q)   // constant 0 dB peak
    {
        const w0 = TWO_PI * clampFreq (sr, f), cs = Math.cos (w0), al = Math.sin (w0) / (2 * q);
        this.coeffs (al, 0, -al, 1 + al, -2 * cs, 1 - al);
    }
    peak (sr, f, q, db)
    {
        const A = Math.pow (10, db / 40), w0 = TWO_PI * clampFreq (sr, f);
        const cs = Math.cos (w0), al = Math.sin (w0) / (2 * q);
        this.coeffs (1 + al * A, -2 * cs, 1 - al * A, 1 + al / A, -2 * cs, 1 - al / A);
    }
    shelfCore (sr, f, db, al, high)
    {
        const A = Math.pow (10, db / 40), w0 = TWO_PI * clampFreq (sr, f), cs = Math.cos (w0);
        const ts = 2 * Math.sqrt (A) * al;
        if (high)
            this.coeffs (A * ((A + 1) + (A - 1) * cs + ts), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - ts),
                         (A + 1) - (A - 1) * cs + ts, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - ts);
        else
            this.coeffs (A * ((A + 1) - (A - 1) * cs + ts), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - ts),
                         (A + 1) + (A - 1) * cs + ts, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - ts);
    }
    lowShelf (sr, f, db, slope = 0.9)
    {
        const A = Math.pow (10, db / 40), sn = Math.sin (TWO_PI * clampFreq (sr, f));
        this.shelfCore (sr, f, db, sn * 0.5 * Math.sqrt ((A + 1 / A) * (1 / slope - 1) + 2), false);
    }
    highShelf (sr, f, db, slope = 0.9)
    {
        const A = Math.pow (10, db / 40), sn = Math.sin (TWO_PI * clampFreq (sr, f));
        this.shelfCore (sr, f, db, sn * 0.5 * Math.sqrt ((A + 1 / A) * (1 / slope - 1) + 2), true);
    }
    lowShelfQ (sr, f, q, db)
    {
        const sn = Math.sin (TWO_PI * clampFreq (sr, f));
        this.shelfCore (sr, f, db, sn / (2 * Math.max (0.05, q)), false);
    }
    highShelfQ (sr, f, q, db)
    {
        const sn = Math.sin (TWO_PI * clampFreq (sr, f));
        this.shelfCore (sr, f, db, sn / (2 * Math.max (0.05, q)), true);
    }
    magnitudeDb (sr, hz)
    {
        const w = TWO_PI * hz / sr, c1 = Math.cos (w), s1 = Math.sin (w), c2 = Math.cos (2 * w), s2 = Math.sin (2 * w);
        const nr = this.b0 + this.b1 * c1 + this.b2 * c2, ni = this.b1 * s1 + this.b2 * s2;
        const dr = 1 + this.a1 * c1 + this.a2 * c2, di = this.a1 * s1 + this.a2 * s2;
        return 10 * Math.log10 (Math.max (1e-12, (nr * nr + ni * ni) / Math.max (1e-24, dr * dr + di * di)));
    }
}

export class OnePoleLP
{
    constructor () { this.a = 1; this.z = 0; }
    setCutoff (sr, f) { this.a = 1 - Math.exp (-TWO_PI * f / sr); }
    process (x) { this.z += this.a * (x - this.z); return this.z; }
    reset () { this.z = 0; }
}

export class AllpassOne
{
    constructor () { this.c = 0; this.z = 0; }
    setBreakFreq (sr, f)
    {
        const n = f / sr, t = Math.tan (Math.PI * (n < 0.0002 ? 0.0002 : n > 0.49 ? 0.49 : n));
        this.c = (1 - t) / (1 + t);
    }
    process (x) { const y = this.c * x + this.z; this.z = x - this.c * y; return y; }
    reset () { this.z = 0; }
}

export class TptSvf
{
    constructor () { this.ic1 = 0; this.ic2 = 0; }
    lowpass (x, g, k)
    {
        const v1 = (this.ic1 + g * (x - this.ic2)) / (1 + g * (g + k));
        const v2 = this.ic2 + g * v1;
        this.ic1 = 2 * v1 - this.ic1;
        this.ic2 = 2 * v2 - this.ic2;
        return v2;
    }
    reset () { this.ic1 = this.ic2 = 0; }
    static g (sr, f) { const n = f / sr; return Math.tan (Math.PI * (n < 0.0004 ? 0.0004 : n > 0.45 ? 0.45 : n)); }
}

export class EnvFollower
{
    constructor () { this.env = 0; this.aAtk = 1; this.aRel = 1; }
    setTimes (sr, atkMs, relMs)
    {
        this.aAtk = 1 - Math.exp (-1000 / (sr * Math.max (0.05, atkMs)));
        this.aRel = 1 - Math.exp (-1000 / (sr * Math.max (1, relMs)));
    }
    process (x) { const a = Math.abs (x); this.env += (a > this.env ? this.aAtk : this.aRel) * (a - this.env); return this.env; }
    reset () { this.env = 0; }
}

export class BufferedAllpass
{
    constructor (capacity)
    {
        const p = nextPow2 (capacity + 4);
        this.buf = new Float32Array (p); this.mask = p - 1; this.write = p; this.g = 0.5;
    }
    reset () { this.buf.fill (0); }
    process (x, delay)
    {
        const r = this.buf[(this.write - delay) & this.mask];
        const w = x + this.g * r;
        this.buf[this.write & this.mask] = w;
        if (++this.write >= this.buf.length * 2) this.write -= this.buf.length;
        return r - this.g * w;
    }
}

export class DCBlocker
{
    constructor (sr) { this.r = 1 - TWO_PI * 5 / sr; this.x1 = 0; this.y1 = 0; }
    process (x) { const y = x - this.x1 + this.r * this.y1; this.x1 = x; this.y1 = y; return y; }
    reset () { this.x1 = this.y1 = 0; }
}

// FET-style compressor with a high-passed detector and program-dependent release.
export class GlueComp
{
    constructor (sr)
    {
        this.sr = sr; this.sc = new Biquad (); this.thresh = -18; this.slope = 0.75;
        this.aAtk = 0.01; this.aRelSlow = 0.001; this.aRelFast = 0.001;
        this.avgCoef = this.coef (400); this.gr = 0; this.avg = 0;
        this.setSidechain (80); this.setAttack (3); this.setRelease (120);
    }
    coef (ms) { return 1 - Math.exp (-1 / (this.sr * 0.001 * Math.max (0.05, ms))); }
    setSidechain (hz) { this.sc.highpass (this.sr, hz, 0.7071); }
    setThreshold (db) { this.thresh = db; }
    setRatio (r) { this.slope = 1 - 1 / Math.max (1, r); }
    setAttack (ms) { this.aAtk = this.coef (ms); }
    setRelease (ms) { this.aRelSlow = this.coef (ms); this.aRelFast = this.coef (Math.min (ms, 80)); }
    reset () { this.sc.reset (); this.gr = 0; this.avg = 0; }
    process (x)
    {
        const a = Math.abs (this.sc.process (x));
        const over = 20 * Math.log10 (a > 1e-6 ? a : 1e-6) - this.thresh;
        const hk = 3;
        let target;
        if (over <= -hk) target = 0;
        else if (over >= hk) target = this.slope * over;
        else { const t = over + hk; target = this.slope * t * t / (4 * hk); }
        if (target > this.gr) this.gr += this.aAtk * (target - this.gr);
        else
        {
            const w = Math.min (1, this.avg / 6);
            this.gr += (this.aRelFast + (this.aRelSlow - this.aRelFast) * w) * (target - this.gr);
        }
        this.avg += this.avgCoef * (this.gr - this.avg);
        return x * Math.pow (10, -this.gr / 20);
    }
}

export class CabSim
{
    constructor (sr)
    {
        this.sr = sr; this.rumble = new Biquad (); this.bump = new Biquad (); this.presence = new Biquad ();
        this.top0 = new Biquad (); this.top1 = new Biquad ();
        this.rumble.highpass (sr, 42, 0.7071);
        this.setParams (90, 5, 4500, -2);
    }
    setParams (bumpF, bumpG, topHz, presDb)
    {
        this.bump.peak (this.sr, bumpF, 1.1, bumpG);
        this.presence.peak (this.sr, 2200, 0.9, presDb);
        this.top0.lowpass (this.sr, topHz, 0.5412);
        this.top1.lowpass (this.sr, topHz, 1.3066);
    }
    reset () { this.rumble.reset (); this.bump.reset (); this.presence.reset (); this.top0.reset (); this.top1.reset (); }
    process (d, n)
    {
        for (let i = 0; i < n; ++i)
        {
            let x = this.rumble.process (d[i]);
            x = this.presence.process (this.bump.process (x));
            d[i] = this.top1.process (this.top0.process (x));
        }
    }
}

// Analogue-style flip-flop octave divider.
export class SubOctave
{
    constructor (sr)
    {
        this.sr = sr;
        this.attack = 1 - Math.exp (-1 / (0.002 * sr));
        this.release = 1 - Math.exp (-1 / (0.140 * sr));
        this.t0 = new OnePoleLP (); this.t1 = new OnePoleLP (); this.tone = new OnePoleLP ();
        this.dc = new DCBlocker (sr);
        this.setTone (500); this.setPeriod (sr / 80); this.reset ();
    }
    reset () { this.t0.reset (); this.t1.reset (); this.tone.reset (); this.dc.reset (); this.env = 0; this.flip = false; this.above = false; this.counter = 0; }
    setPeriod (p)
    {
        const f0 = this.sr / Math.max (8, p);
        const c = clamp (1.4 * f0, 55, 420);
        this.t0.setCutoff (this.sr, c); this.t1.setCutoff (this.sr, c);
    }
    setTone (hz) { this.tone.setCutoff (this.sr, hz); }
    process (inp, out, n, divisor)
    {
        for (let i = 0; i < n; ++i)
        {
            const x = inp[i], r = Math.abs (x);
            this.env += (r > this.env ? this.attack : this.release) * (r - this.env);
            const key = this.t1.process (this.t0.process (x));
            const hys = 0.06 * this.env + 1e-5;
            if (! this.above && key > hys)
            {
                this.above = true;
                if (++this.counter >= divisor) { this.counter = 0; this.flip = ! this.flip; }
            }
            else if (this.above && key < -hys) this.above = false;
            out[i] = this.tone.process (this.dc.process (this.flip ? this.env : -this.env));
        }
    }
}

export class MasterLimiter
{
    constructor (sr) { this.rel = Math.exp (-1 / (0.12 * sr)); this.env = 0; this.lastGain = 1; }
    static ceiling = 0.891;
    process (out, i, L, R)
    {
        const l = L[i], r = R[i];
        const peak = Math.max (Math.abs (l), Math.abs (r));
        this.env = peak > this.env ? peak : peak + (this.env - peak) * this.rel;
        const g = this.env > 0.891 ? 0.891 / this.env : 1;
        this.lastGain = g;
        L[i] = l * g; R[i] = r * g;
        return g;
    }
}

// YIN pitch tracker with full-rate refinement. The range is configurable so the
// same code serves the bass (26-500 Hz) and the voice (70-1000 Hz).
export class PitchTracker
{
    constructor (sr, { decRateTarget = 3000, fMin = 26, fMax = 500, window = 256 } = {})
    {
        this.sr = sr;
        this.decim = Math.max (1, Math.round (sr / decRateTarget));
        this.decRate = sr / this.decim;
        const aaFreq = Math.min (fMax * 1.4, this.decRate * 0.24);
        this.aa = [new Biquad (), new Biquad (), new Biquad ()];
        this.aa[0].lowpass (sr, aaFreq, 0.51764); this.aa[1].lowpass (sr, aaFreq, 0.70711); this.aa[2].lowpass (sr, aaFreq, 1.93185);
        this.tauMin = Math.max (3, Math.floor (this.decRate / fMax));
        this.tauMax = Math.min (400, Math.ceil (this.decRate / fMin));
        this.window = window;
        this.hop = 32;
        const decN = nextPow2 (window + this.tauMax + 8);
        this.decBuf = new Float32Array (decN); this.decMask = decN - 1;
        this.maxPeriod = (this.tauMax + 1) * this.decim;
        const frN = nextPow2 (Math.max (4096, 2048 + 2 * this.maxPeriod));
        this.frBuf = new Float32Array (frN); this.frMask = frN - 1;
        this.scratch = new Float32Array (window + this.tauMax + 4);
        this.frScratch = new Float32Array (frN);
        this.diff = new Float32Array (this.tauMax + 2);
        this.cmnd = new Float32Array (this.tauMax + 2);
        this.diffFull = new Float32Array (2 * this.decim + 4);
        this.reset ();
    }
    reset ()
    {
        for (const f of this.aa) f.reset ();
        this.decBuf.fill (0); this.frBuf.fill (0);
        this.decCounter = this.decWrite = this.frWrite = this.since = 0;
        this.period = this.sr / 80; this.confidence = 0;
    }
    get frequency () { return this.sr / this.period; }
    process (inp, n)
    {
        const aa0 = this.aa[0], aa1 = this.aa[1], aa2 = this.aa[2];
        for (let i = 0; i < n; ++i)
        {
            const x = inp[i];
            this.frBuf[this.frWrite & this.frMask] = x; ++this.frWrite;
            const f = aa2.process (aa1.process (aa0.process (x)));
            if (++this.decCounter >= this.decim)
            {
                this.decCounter = 0;
                this.decBuf[this.decWrite & this.decMask] = f; ++this.decWrite;
                if (++this.since >= this.hop) { this.since = 0; this.analyse (); }
            }
        }
        if (this.frWrite > 1e9) { this.frWrite &= this.frMask; this.frWrite += this.frBuf.length; }
        if (this.decWrite > 1e9) { this.decWrite &= this.decMask; this.decWrite += this.decBuf.length; }
    }
    analyse ()
    {
        const W = this.window, tMax = this.tauMax, tMin = this.tauMin, total = W + tMax;
        const s = this.scratch, db = this.decBuf, m = this.decMask, dw = this.decWrite;
        for (let i = 0; i < total; ++i) s[i] = db[(dw - total + i) & m];
        let energy = 0;
        for (let j = 0; j < W; ++j) { const v = s[tMax + j]; energy += v * v; }
        if (Math.sqrt (energy / W) < 0.0007) { this.confidence = 0; return; }
        const diff = this.diff, cmnd = this.cmnd;
        for (let tau = tMin; tau <= tMax; ++tau)
        {
            let sum = 0;
            for (let j = 0; j < W; ++j) { const d = s[tMax + j] - s[tMax + j - tau]; sum += d * d; }
            diff[tau] = sum;
        }
        let running = 0;
        for (let tau = tMin; tau <= tMax; ++tau)
        {
            running += diff[tau];
            cmnd[tau] = running > 1e-12 ? diff[tau] * (tau - tMin + 1) / running : 1;
        }
        let best = -1;
        for (let tau = tMin; tau <= tMax - 1; ++tau)
            if (cmnd[tau] < 0.15)
            {
                let t = tau;
                while (t + 1 <= tMax && cmnd[t + 1] < cmnd[t]) ++t;
                best = t; break;
            }
        if (best < 0)
        {
            best = tMin;
            for (let tau = tMin + 1; tau <= tMax; ++tau) if (cmnd[tau] < cmnd[best]) best = tau;
        }
        if (2 * best + 2 <= tMax && cmnd[best] > 0.02)
        {
            let doubled = 2 * best;
            for (let tau = 2 * best - 2; tau <= 2 * best + 2; ++tau) if (cmnd[tau] < cmnd[doubled]) doubled = tau;
            if (cmnd[doubled] < 0.5 * cmnd[best] && cmnd[doubled] < 0.03) best = doubled;
        }
        const quality = cmnd[best];
        if (quality > 0.4) { this.confidence = 0; return; }
        let tauEst = best;
        if (best > tMin && best < tMax)
        {
            const s0 = cmnd[best - 1], s1 = cmnd[best], s2 = cmnd[best + 1], den = s0 + s2 - 2 * s1;
            if (Math.abs (den) > 1e-9) tauEst = best + 0.5 * (s0 - s2) / den;
        }
        const conf = clamp (1 - quality, 0, 1);
        const refined = this.refine (tauEst * this.decim);
        const delta = Math.abs (refined - this.period);
        if (conf < 0.7 && delta > 0.05 * this.period && this.confidence > 0.5) { this.confidence = conf * 0.5; return; }
        this.period = refined; this.confidence = conf;
    }
    refine (coarse)
    {
        const p0 = Math.round (coarse), search = Math.max (1, this.decim);
        const lo = Math.max (8, p0 - search), hi = p0 + search;
        const Wf = clamp (p0, 256, 1024);
        if (Wf + hi + 4 >= this.frBuf.length) return coarse;
        const total = Wf + hi, fs = this.frScratch, fb = this.frBuf, m = this.frMask, fw = this.frWrite;
        for (let i = 0; i < total; ++i) fs[i] = fb[(fw - total + i) & m];
        let energy = 0;
        for (let j = 0; j < Wf; ++j) { const v = fs[hi + j]; energy += v * v; }
        if (energy < 1e-12) return coarse;
        let bestLag = p0, bestVal = -2;
        const df = this.diffFull;
        for (let lag = lo; lag <= hi; ++lag)
        {
            let num = 0, le = 0;
            for (let j = 0; j < Wf; ++j) { const a = fs[hi + j], b = fs[hi + j - lag]; num += a * b; le += b * b; }
            const score = num / (Math.sqrt (energy * le) + 1e-18);
            df[lag - lo] = score;
            if (score > bestVal) { bestVal = score; bestLag = lag; }
        }
        let result = bestLag;
        if (bestLag > lo && bestLag < hi)
        {
            const s0 = df[bestLag - 1 - lo], s1 = df[bestLag - lo], s2 = df[bestLag + 1 - lo], den = s0 + s2 - 2 * s1;
            if (Math.abs (den) > 1e-12) result = bestLag - 0.5 * (s2 - s0) / den;
        }
        return clamp (result, 8, this.maxPeriod);
    }
}

// Pitch-synchronous two-tap delay-line shifter: taps relocate by exactly one
// detected period, so every crossfade is between waveform-aligned copies.
export class OctaveShifter
{
    constructor (sr)
    {
        this.sr = sr;
        let size = 1; while (size < sr * 0.35) size <<= 1;
        this.buf = new Float32Array (size); this.mask = size - 1;
        this.alpha = 2; this.period = sr / 80; this.jumpPeriod = this.period;
        this.xfadeLen = 128; this.minLag = 8;
        this.pos = new Float64Array (2);
        this.reset ();
    }
    reset ()
    {
        this.buf.fill (0);
        this.writePos = this.buf.length;
        this.pos[0] = this.writePos - (this.minLag + 0.75 * this.period); this.pos[1] = this.pos[0];
        this.gain = this.gainTarget = this.gainStep = 0; this.fading = false;
        this.lagReport = this.minLag + 0.75 * this.period;
    }
    setSemitones (s) { this.alpha = Math.pow (2, clamp (s, -24, 24) / 12); }
    setPeriod (p) { this.period = clamp (p, 24, this.buf.length / 4); }
    startJump (delta)
    {
        const active = this.gainTarget > 0.5 ? 1 : 0, other = 1 - active;
        this.pos[other] = this.pos[active] + delta;
        this.gainTarget = other === 1 ? 1 : 0;
        this.gainStep = (this.gainTarget - this.gain) / this.xfadeLen;
        this.fading = true;
    }
    process (d, n)
    {
        const a = this.alpha, buf = this.buf, mask = this.mask, pos = this.pos;
        for (let k = 0; k < n; ++k)
        {
            buf[this.writePos & mask] = d[k];
            const s0 = readHermite (buf, pos[0], mask), s1 = readHermite (buf, pos[1], mask);
            d[k] = s0 + (s1 - s0) * this.gain;
            pos[0] += a; pos[1] += a; ++this.writePos;
            if (this.fading)
            {
                this.gain += this.gainStep;
                if ((this.gainStep > 0 && this.gain >= this.gainTarget) || (this.gainStep <= 0 && this.gain <= this.gainTarget))
                { this.gain = this.gainTarget; this.fading = false; }
            }
            else
            {
                const active = this.gainTarget > 0.5 ? 1 : 0;
                const lag = this.writePos - pos[active];
                this.lagReport += 0.0002 * (lag - this.lagReport);
                if (Math.abs (a - 1) > 1e-9)
                {
                    this.jumpPeriod = this.period;
                    this.xfadeLen = clamp ((0.25 * this.jumpPeriod) | 0, 32, 512);
                    const fade = this.xfadeLen, lagHi = this.minLag + this.jumpPeriod + fade;
                    if (a > 1) { if (lag < this.minLag + fade * (a - 1) + 1) this.startJump (-this.jumpPeriod); }
                    else if (lag > lagHi) this.startJump (this.jumpPeriod);
                }
            }
        }
        if (this.writePos > 2 ** 40)
        {
            const shift = Math.floor (2 ** 40 / buf.length) * buf.length;
            this.writePos -= shift; pos[0] -= shift; pos[1] -= shift;
        }
    }
}

// Dattorro plate with a modulated tank.
class Line
{
    constructor (cap) { const p = nextPow2 (cap + 8); this.buf = new Float32Array (p); this.mask = p - 1; this.write = p; }
    clear () { this.buf.fill (0); }
    push (x) { this.buf[this.write & this.mask] = x; if (++this.write >= this.buf.length * 2) this.write -= this.buf.length; }
    read (d)
    {
        const hi = this.buf.length - 8;
        return readHermite (this.buf, this.write - (d < 4 ? 4 : d > hi ? hi : d), this.mask);
    }
}
class VerbAllpass
{
    constructor (cap, g = 0.5) { this.line = new Line (cap); this.g = g; }
    process (x, d) { const r = this.line.read (d), w = x + this.g * r; this.line.push (w); return r - this.g * w; }
}
const kInDiffLen = [142, 107, 379, 277], kInDiffG = [0.75, 0.75, 0.625, 0.625];
const kModApA = 672, kModApB = 908, kDelayA1 = 4453, kDelayB1 = 4217, kApA2 = 1800, kApB2 = 2656, kDelayA2 = 3720, kDelayB2 = 3163;
const kExc = 16, kCycle = kModApA + kDelayA1 + kApA2 + kDelayA2 + kModApB + kDelayB1 + kApB2 + kDelayB2;
const kMinScale = 0.35, kMaxScale = 1.10;

export class PlateVerb
{
    constructor (sr)
    {
        this.sr = sr; this.srScale = sr / 29761;
        const cap = (l) => Math.ceil (l * this.srScale * kMaxScale + kExc * this.srScale) + 16;
        this.pre = new Line (Math.ceil (0.130 * sr) + 16);
        this.inDiff = kInDiffLen.map ((l, i) => new VerbAllpass (Math.ceil (l * this.srScale) + 16, kInDiffG[i]));
        this.modApA = new VerbAllpass (cap (kModApA), -0.7); this.modApB = new VerbAllpass (cap (kModApB), -0.7);
        this.apA2 = new VerbAllpass (cap (kApA2)); this.apB2 = new VerbAllpass (cap (kApB2));
        this.dA1 = new Line (cap (kDelayA1)); this.dB1 = new Line (cap (kDelayB1));
        this.dA2 = new Line (cap (kDelayA2)); this.dB2 = new Line (cap (kDelayB2));
        this.bandwidth = new OnePoleLP (); this.bandwidth.setCutoff (sr, 10500);
        this.dampA = new OnePoleLP (); this.dampB = new OnePoleLP ();
        this.lowA = new OnePoleLP (); this.lowB = new OnePoleLP (); this.lowA.setCutoff (sr, 90); this.lowB.setCutoff (sr, 90);
        const sm = (s) => 1 - Math.exp (-1 / (s * sr));
        this.scaleCoef = sm (0.08); this.decayCoef = sm (0.03); this.excCoef = sm (0.05); this.preCoef = sm (0.1);
        this.sizeT = 0.5; this.dampT = 0.5; this.preT = 12; this.modT = 0.5;
        this.last = [-1, -1, -1, -1];
        this.scaleGoal = 0.725; this.decayGoal = 0.6; this.excGoal = 0; this.preGoal = 1;
        this.dn = 1e-20;
        this.updateTargets ();
        this.reset ();
    }
    reset ()
    {
        this.pre.clear (); for (const d of this.inDiff) d.line.clear ();
        for (const a of [this.modApA, this.modApB, this.apA2, this.apB2]) a.line.clear ();
        for (const l of [this.dA1, this.dB1, this.dA2, this.dB2]) l.clear ();
        this.bandwidth.reset (); this.dampA.reset (); this.dampB.reset (); this.lowA.reset (); this.lowB.reset ();
        this.phA = 0; this.phB = 0.5; this.snap = true;
    }
    set (size, damp, preMs, mod) { this.sizeT = clamp (size, 0, 1); this.dampT = clamp (damp, 0, 1); this.preT = Math.max (0, preMs); this.modT = clamp (mod, 0, 1); }
    updateTargets ()
    {
        const L = this.last;
        if (this.sizeT !== L[0])
        {
            L[0] = this.sizeT;
            this.scaleGoal = kMinScale + (kMaxScale - kMinScale) * this.sizeT;
            const rt60 = 0.5 * Math.exp (2.7726 * this.sizeT);
            const cyc = kCycle * this.scaleGoal / 29761;
            this.decayGoal = Math.min (0.9995, Math.pow (10, -3 * cyc / (4 * rt60)));
            const g2 = clamp (this.decayGoal + 0.15, 0.25, 0.5);
            this.apA2.g = g2; this.apB2.g = g2;
        }
        if (this.dampT !== L[1])
        {
            L[1] = this.dampT;
            const fc = 18000 * Math.exp (this.dampT * Math.log (1000 / 18000));
            this.dampA.setCutoff (this.sr, fc); this.dampB.setCutoff (this.sr, fc);
        }
        if (this.preT !== L[2])
        {
            L[2] = this.preT;
            const cap = this.pre.buf.length - 12, s = this.preT * 0.001 * this.sr;
            this.preGoal = s < 1 ? 1 : s > cap ? cap : s;
        }
        if (this.modT !== L[3]) { L[3] = this.modT; this.excGoal = this.modT * kExc * this.srScale; }
    }
    process (inp, left, right, n)
    {
        this.updateTargets ();
        if (this.snap)
        {
            this.snap = false;
            this.scale = this.scaleGoal; this.decay = this.decayGoal; this.exc = this.excGoal; this.preS = this.preGoal;
        }
        const incA = 0.87 / this.sr, incB = 1.09 / this.sr, ss = this.srScale;
        const dA1 = this.dA1, dB1 = this.dB1, dA2 = this.dA2, dB2 = this.dB2, apA2 = this.apA2, apB2 = this.apB2;
        for (let i = 0; i < n; ++i)
        {
            this.scale += this.scaleCoef * (this.scaleGoal - this.scale);
            this.decay += this.decayCoef * (this.decayGoal - this.decay);
            this.exc += this.excCoef * (this.excGoal - this.exc);
            this.preS += this.preCoef * (this.preGoal - this.preS);
            const k = ss * this.scale;
            this.phA += incA; if (this.phA >= 1) this.phA -= 1;
            this.phB += incB; if (this.phB >= 1) this.phB -= 1;
            const excA = this.exc * Math.sin (TWO_PI * this.phA), excB = this.exc * Math.sin (TWO_PI * this.phB);
            const fbA = dA2.read (kDelayA2 * k), fbB = dB2.read (kDelayB2 * k);
            let x = this.pre.read (this.preS);
            this.pre.push (inp[i]);
            x = this.bandwidth.process (x);
            for (let d = 0; d < 4; ++d) x = this.inDiff[d].process (x, kInDiffLen[d] * ss);
            let a = x + this.decay * fbB + this.dn;
            this.dn = -this.dn;
            a = this.modApA.process (a, kModApA * k + excA);
            let vA = dA1.read (kDelayA1 * k); dA1.push (a);
            vA = this.dampA.process (vA); vA -= this.lowA.process (vA); vA *= this.decay;
            vA = apA2.process (vA, kApA2 * k); dA2.push (vA);
            let b = x + this.decay * fbA + this.dn;
            b = this.modApB.process (b, kModApB * k + excB);
            let vB = dB1.read (kDelayB1 * k); dB1.push (b);
            vB = this.dampB.process (vB); vB -= this.lowB.process (vB); vB *= this.decay;
            vB = apB2.process (vB, kApB2 * k); dB2.push (vB);
            const yL = dB1.read (266 * k) + dB1.read (2974 * k) - apB2.line.read (1913 * k) + dB2.read (1996 * k)
                     - dA1.read (1990 * k) - apA2.line.read (187 * k) - dA2.read (1066 * k);
            const yR = dA1.read (353 * k) + dA1.read (3627 * k) - apA2.line.read (1228 * k) + dA2.read (2673 * k)
                     - dB1.read (2111 * k) - apB2.line.read (335 * k) - dB2.read (121 * k);
            left[i] = 0.6 * yL; right[i] = 0.6 * yR;
        }
    }
}

// In-place complex radix-2 FFT on separate re/im arrays.
export class FFT
{
    constructor (n)
    {
        this.n = n;
        this.rev = new Uint32Array (n);
        const bits = Math.log2 (n);
        for (let i = 0; i < n; ++i)
        {
            let r = 0;
            for (let b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            this.rev[i] = r;
        }
        this.cos = new Float64Array (n / 2); this.sin = new Float64Array (n / 2);
        for (let i = 0; i < n / 2; ++i) { this.cos[i] = Math.cos (TWO_PI * i / n); this.sin[i] = Math.sin (TWO_PI * i / n); }
    }
    // inverse: conjugate twiddles, no scaling (callers scale).
    transform (re, im, inverse)
    {
        const n = this.n, rev = this.rev;
        for (let i = 0; i < n; ++i)
        {
            const j = rev[i];
            if (j > i) { let t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
        }
        const sgn = inverse ? 1 : -1;
        for (let size = 2; size <= n; size <<= 1)
        {
            const half = size >> 1, step = n / size;
            for (let start = 0; start < n; start += size)
                for (let k = 0; k < half; ++k)
                {
                    const wr = this.cos[k * step], wi = sgn * this.sin[k * step];
                    const a = start + k, b = a + half;
                    const xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - xr; im[b] = im[a] - xi;
                    re[a] += xr; im[a] += xi;
                }
        }
    }
}

// The freeze: reads the spectrum of the moment, finds each partial, and keeps
// sounding those partials as steady tones. Two voices so a new press can start
// while the last hold is still fading. Port of SpectralFreeze.cpp.
const motionBaseMs = [9, 14], motionRateHz = [0.29, 0.41];
export class SpectralFreeze
{
    constructor (sr)
    {
        this.sr = sr;
        const order = sr > 50000 ? 14 : 13;
        const N = this.size = 1 << order;
        this.hop = N / 4; this.bins = N / 2 + 1;
        this.fft = new FFT (N);
        this.window = new Float64Array (N); this.synthWindow = new Float64Array (N);
        for (let i = 0; i < N; ++i) { const w = 0.5 - 0.5 * Math.cos (TWO_PI * i / N); this.window[i] = w; this.synthWindow[i] = w / 1.5; }
        this.wRe = new Float64Array (N); this.wIm = new Float64Array (N);
        this.eRe = new Float64Array (N); this.eIm = new Float64Array (N);
        this.power = new Float64Array (this.bins); this.shapes = new Float64Array (this.bins);
        this.history = new Float32Array (N * 2); this.historyMask = N * 2 - 1;
        this.voices = [0, 1].map (() => ({
            sRe: new Float64Array (this.bins), sIm: new Float64Array (this.bins),
            tRe: new Float64Array (this.bins), tIm: new Float64Array (this.bins),
            ola: new Float32Array (N), read: 0, untilFrame: 0, gain: 0, held: false, active: false }));
        let ms = 1; while (ms < sr * 0.03) ms <<= 1;
        this.motionBuf = new Float32Array (ms); this.motionMask = ms - 1;
        this.motionLows = [new Biquad (), new Biquad ()];
        for (const s of this.motionLows) s.highpass (sr, 150, 0.7071);
        this.motionTarget = 0; this.motion = 0;
        this.setFadeTimes (100, 150);
        this.reset ();
    }
    reset ()
    {
        this.history.fill (0); this.historyWrite = 0;
        for (const v of this.voices) { v.ola.fill (0); v.read = 0; v.untilFrame = 0; v.gain = 0; v.held = false; v.active = false; }
        this.wasHolding = false; this.untilCapture = -1;
        this.motionBuf.fill (0); this.motionWrite = this.motionBuf.length;
        this.motionPhase = [0, 0.25]; this.motion = this.motionTarget;
        for (const s of this.motionLows) s.reset ();
    }
    setFadeTimes (inMs, outMs)
    {
        this.inStep = 1000 / (this.sr * Math.max (4, inMs));
        this.outStep = 1000 / (this.sr * Math.max (4, outMs));
    }
    setMotion (a) { this.motionTarget = clamp (a, 0, 1); }
    get sounding () { return this.voices[0].active || this.voices[1].active; }
    analyse (re, im, samplesAgo)
    {
        const N = this.size, start = this.historyWrite - samplesAgo - N, h = this.history, m = this.historyMask, w = this.window;
        for (let i = 0; i < N; ++i) { re[i] = h[(start + i) & m] * w[i]; im[i] = 0; }
        this.fft.transform (re, im, false);
    }
    turn (v)
    {
        for (let k = 0; k < this.bins; ++k)
        {
            const re = v.sRe[k], im = v.sIm[k], tr = v.tRe[k], ti = v.tIm[k];
            v.sRe[k] = re * tr - im * ti; v.sIm[k] = re * ti + im * tr;
        }
    }
    addFrame (v, skip)
    {
        const N = this.size, re = this.wRe, im = this.wIm, B = this.bins;
        for (let k = 0; k < B; ++k) { re[k] = v.sRe[k]; im[k] = v.sIm[k]; }
        for (let k = B; k < N; ++k) { re[k] = v.sRe[N - k]; im[k] = -v.sIm[N - k]; }
        this.fft.transform (re, im, true);
        const mask = N - 1, inv = 1 / N, sw = this.synthWindow;
        for (let i = skip; i < N; ++i) v.ola[(v.read + i - skip) & mask] += re[i] * inv * sw[i];
    }
    holdRegion (v, from, to, peak, floor)
    {
        let cr = 0, ci = 0, energy = 0;
        const eRe = this.eRe, eIm = this.eIm, wRe = this.wRe, wIm = this.wIm;
        for (let k = from; k < to; ++k)
        {
            const ar = eRe[k], ai = eIm[k], br = wRe[k], bi = wIm[k];
            cr += br * ar + bi * ai; ci += bi * ar - br * ai; energy += br * br + bi * bi;
        }
        if (energy <= floor)
        {
            for (let k = from; k < to; ++k) { v.sRe[k] = v.sIm[k] = v.tRe[k] = v.tIm[k] = 0; }
            return;
        }
        let offset = 0;
        if (cr * cr + ci * ci > 1e-30)
        {
            const beyond = Math.atan2 (ci, cr) - 0.5 * Math.PI * peak;
            offset = clamp ((beyond - TWO_PI * Math.round (beyond / TWO_PI)) * 2 / Math.PI, -1, 1);
        }
        const centre = peak + offset;
        const turnRe = Math.cos (0.5 * Math.PI * centre), turnIm = Math.sin (0.5 * Math.PI * centre);
        const lobe = -Math.sin (Math.PI * offset) / Math.PI;
        let shapeEnergy = 0;
        const shapes = this.shapes;
        for (let k = from; k < to; ++k)
        {
            const d = k - centre, parity = ((k - peak) & 1) !== 0 ? -1 : 1;
            const shape = Math.abs (d) < 1e-4 ? parity : Math.abs (Math.abs (d) - 1) < 1e-4 ? 0.5 * parity : lobe / (d * (1 - d * d));
            shapes[k] = shape; shapeEnergy += shape * shape;
        }
        const pr = wRe[peak], pi = wIm[peak], pm = Math.sqrt (pr * pr + pi * pi);
        const scale = shapeEnergy > 1e-30 && pm > 1e-30 ? Math.sqrt (energy / shapeEnergy) / pm : 0;
        for (let k = from; k < to; ++k)
        {
            v.sRe[k] = scale * pr * shapes[k]; v.sIm[k] = scale * pi * shapes[k];
            v.tRe[k] = turnRe; v.tIm[k] = turnIm;
        }
    }
    capture (v)
    {
        this.analyse (this.eRe, this.eIm, this.hop);
        this.analyse (this.wRe, this.wIm, 0);
        const B = this.bins, power = this.power;
        let total = 0;
        for (let k = 0; k < B; ++k) { const p = this.wRe[k] ** 2 + this.wIm[k] ** 2; power[k] = p; total += p; }
        const floor = total * 1e-8;
        const at = (k) => (k >= 0 && k < B ? power[k] : -1);
        let regionStart = 0, lastPeak = -1;
        for (let k = 0; k < B; ++k)
        {
            const p = at (k);
            if (! (p > at (k - 1) && p > at (k - 2) && p >= at (k + 1) && p >= at (k + 2))) continue;
            if (lastPeak >= 0)
            {
                let valley = lastPeak + 1;
                for (let j = lastPeak + 1; j < k; ++j) if (power[j] < power[valley]) valley = j;
                this.holdRegion (v, regionStart, valley, lastPeak, floor);
                regionStart = valley;
            }
            lastPeak = k;
        }
        if (lastPeak >= 0) this.holdRegion (v, regionStart, B, lastPeak, floor);
        else { v.sRe.fill (0); v.sIm.fill (0); v.tRe.fill (0); v.tIm.fill (0); }
        if (! v.active)
        {
            v.ola.fill (0); v.read = 0; v.gain = 0;
            for (let ahead = 1; ahead <= 4; ++ahead) { this.turn (v); this.addFrame (v, (4 - ahead) * this.hop); }
        }
        else
        {
            for (let ahead = 1; ahead <= 4; ++ahead) this.turn (v);
            this.addFrame (v, 0);
        }
        v.untilFrame = this.hop; v.held = true; v.active = true;
    }
    render (v, layer, n)
    {
        const mask = this.size - 1;
        let done = 0;
        while (done < n)
        {
            if (v.untilFrame === 0) { this.turn (v); this.addFrame (v, 0); v.untilFrame = this.hop; }
            const run = Math.min (n - done, v.untilFrame);
            const target = v.held ? 1 : 0, step = v.held ? this.inStep : this.outStep;
            for (let i = 0; i < run; ++i)
            {
                const at = (v.read + i) & mask, s = v.ola[at];
                v.ola[at] = 0;
                if (v.gain < target) v.gain = Math.min (target, v.gain + step);
                else if (v.gain > target) v.gain = Math.max (target, v.gain - step);
                layer[done + i] += s * v.gain;
            }
            v.read = (v.read + run) & mask; v.untilFrame -= run; done += run;
            if (! v.held && v.gain <= 0) { v.active = false; return; }
        }
    }
    addMotion (layer, n)
    {
        const mask = this.motionMask, buf = this.motionBuf, depth = 2.5 * 0.001 * this.sr;
        for (let i = 0; i < n; ++i)
        {
            this.motion += 0.0005 * (this.motionTarget - this.motion);
            const x = layer[i];
            buf[this.motionWrite & mask] = x;
            let wet = 0;
            for (let t = 0; t < 2; ++t)
            {
                this.motionPhase[t] += motionRateHz[t] / this.sr;
                if (this.motionPhase[t] >= 1) this.motionPhase[t] -= 1;
                const tap = motionBaseMs[t] * 0.001 * this.sr + depth * this.motion * Math.sin (TWO_PI * this.motionPhase[t]);
                wet += 0.5 * readHermite (buf, this.motionWrite - tap, mask);
            }
            if (++this.motionWrite >= buf.length * 2) this.motionWrite -= buf.length;
            const ang = this.motion * 0.7853982;
            const moved = x * Math.cos (ang) + wet * Math.sin (ang) * 1.4142136;
            layer[i] = x + this.motionLows[1].process (this.motionLows[0].process (moved - x));
        }
    }
    process (inp, layer, n, hold)
    {
        if (hold && ! this.wasHolding) { for (const v of this.voices) v.held = false; this.untilCapture = this.hop; }
        else if (! hold && this.wasHolding) { for (const v of this.voices) v.held = false; this.untilCapture = -1; }
        if (this.untilCapture >= 0 && (this.untilCapture -= n) < 0)
        {
            this.untilCapture = -1;
            const [v0, v1] = this.voices;
            const next = ! v0.active ? v0 : ! v1.active ? v1 : v0.gain <= v1.gain ? v0 : v1;
            if (! this.sounding) { this.motionBuf.fill (0); for (const s of this.motionLows) s.reset (); }
            this.capture (next);
        }
        this.wasHolding = hold;
        const h = this.history, m = this.historyMask;
        for (let i = 0; i < n; ++i) h[(this.historyWrite + i) & m] = inp[i];
        this.historyWrite = (this.historyWrite + n) & m;
        layer.fill (0, 0, n);
        if (! this.sounding) return;
        for (const v of this.voices) if (v.active) this.render (v, layer, n);
        this.addMotion (layer, n);
    }
}

// The tracker as a note source, aimed at swells and pads.
export class NoteSegmenter
{
    constructor (sr)
    {
        this.sr = sr;
        this.attack = 1 - Math.exp (-1 / (0.004 * sr));
        this.release = 1 - Math.exp (-1 / (0.090 * sr));
        this.maxNote = sr * 30;
        this.setParams (-42, 45, 180, 12, 2);
        this.reset ();
    }
    reset ()
    {
        this.env = 0; this.sounding = -1; this.candidate = -1;
        this.candidateSamples = this.quietSamples = this.graceSamples = this.noteSamples = 0;
        this.lastBend = 8192; this.lastExpr = -1;
    }
    setParams (gateDb, holdMs, relMs, transpose, bendRange)
    {
        this.gate = gateDb; this.hold = holdMs * 0.001 * this.sr; this.rel = relMs * 0.001 * this.sr;
        this.transpose = transpose; this.bendRange = bendRange;
    }
    velocity (levelDb)
    {
        const span = Math.max (6, -this.gate), norm = clamp ((levelDb - this.gate) / span, 0, 1);
        return clamp (Math.round (127 * Math.pow (norm, 0.6)), 1, 127);
    }
    flush (out)
    {
        if (this.sounding < 0) return;
        out.push (0x80, this.sounding, 0);
        this.sounding = -1; this.candidate = -1; this.candidateSamples = 0;
    }
    // out: a flat array of triples (type, a, b): 0x90 note on, 0x80 off, 0xE0 bend, 0xB0 expression
    process (inp, n, period, confidence, out)
    {
        for (let i = 0; i < n; ++i) { const r = Math.abs (inp[i]); this.env += (r > this.env ? this.attack : this.release) * (r - this.env); }
        const levelDb = 20 * Math.log10 (this.env + 1e-9);
        const live = confidence >= 0.5 && levelDb > this.gate;
        const hz = this.sr / Math.max (8, period);
        const exact = 69 + 12 * Math.log2 (hz / 440) + this.transpose;
        const nearest = Math.round (exact);
        if (live && nearest >= 0 && nearest <= 127)
        {
            this.quietSamples = 0; this.graceSamples = 0;
            if (this.candidate >= 0 && Math.abs (exact - this.candidate) <= 0.65) this.candidateSamples += n;
            else { this.candidate = nearest; this.candidateSamples = n; }
        }
        else
        {
            this.quietSamples += n; this.graceSamples += n;
            if (this.graceSamples > this.hold / 3) { this.candidate = -1; this.candidateSamples = 0; }
        }
        const settled = this.candidate >= 0 && this.candidateSamples >= this.hold;
        const elsewhere = this.sounding >= 0 && Math.abs (exact - this.sounding) > 0.65;
        const move = this.candidate >= 0 && this.candidateSamples >= this.hold * 2 && elsewhere;
        if (this.sounding < 0)
        {
            if (settled)
            {
                out.push (0xE0, 8192, 0, 0x90, this.candidate, this.velocity (levelDb));
                this.sounding = this.candidate; this.noteSamples = 0; this.lastBend = 8192; this.lastExpr = -1;
            }
        }
        else
        {
            this.noteSamples += n;
            if (move && this.candidate !== this.sounding)
            {
                out.push (0xE0, 8192, 0, 0x90, this.candidate, this.velocity (levelDb), 0x80, this.sounding, 0);
                this.sounding = this.candidate; this.noteSamples = 0; this.lastBend = 8192; this.lastExpr = -1;
            }
            else if (this.quietSamples >= this.rel || this.noteSamples >= this.maxNote)
            {
                out.push (0x80, this.sounding, 0);
                this.sounding = -1;
            }
        }
        if (this.sounding >= 0 && live)
        {
            if (this.bendRange > 0)
            {
                const off = exact - this.sounding;
                if (Math.abs (off) <= this.bendRange)
                {
                    const v = clamp (Math.round (8192 + 8191 * off / this.bendRange), 0, 16383);
                    if (Math.abs (v - this.lastBend) >= 32) { this.lastBend = v; out.push (0xE0, v, 0); }
                }
            }
            const e = this.velocity (levelDb);
            if (this.lastExpr < 0 || Math.abs (e - this.lastExpr) >= 2) { this.lastExpr = e; out.push (0xB0, e, 0); }
        }
    }
}

// 4x oversampler: two windowed-sinc FIR stages (zero-stuff up, decimate down).
export class Oversampler4
{
    constructor (maxBlock = 128, taps = 96)
    {
        this.taps = taps;
        const h = new Float64Array (taps), fc = 0.1, beta = 8;
        const bessel0 = (x) => { let s = 1, t = 1; for (let k = 1; k < 30; ++k) { t *= (x / (2 * k)) ** 2; s += t; } return s; };
        let sum = 0;
        for (let i = 0; i < taps; ++i)
        {
            const m = i - (taps - 1) / 2;
            const sinc = m === 0 ? 2 * fc : Math.sin (TWO_PI * fc * m) / (Math.PI * m);
            const r = 2 * i / (taps - 1) - 1;
            h[i] = sinc * bessel0 (beta * Math.sqrt (Math.max (0, 1 - r * r))) / bessel0 (beta);
            sum += h[i];
        }
        for (let i = 0; i < taps; ++i) h[i] /= sum;
        this.h = Float32Array.from (h);
        // polyphase for the up stage: phase p uses taps p, p+4, ... (gain 4)
        this.phases = [0, 1, 2, 3].map ((p) => { const a = []; for (let i = p; i < taps; i += 4) a.push (h[i] * 4); return Float32Array.from (a); });
        this.upHist = new Float32Array (64 + maxBlock); this.upLen = Math.ceil (taps / 4);
        this.downHist = new Float32Array (taps + maxBlock * 4);
        this.up = new Float32Array (maxBlock * 4);
        this.latency = (taps - 1) / 4;   // base-rate samples, both stages together
    }
    reset () { this.upHist.fill (0); this.downHist.fill (0); }
    upsample (x, n)
    {
        const L = this.upLen, hist = this.upHist, out = this.up;
        // hist holds the last L-1 inputs followed by this block
        for (let i = 0; i < n; ++i) hist[L - 1 + i] = x[i];
        for (let i = 0; i < n; ++i)
            for (let p = 0; p < 4; ++p)
            {
                const ph = this.phases[p];
                let acc = 0;
                for (let j = 0; j < ph.length; ++j) acc += ph[j] * hist[L - 1 + i - j];
                out[i * 4 + p] = acc;
            }
        for (let i = 0; i < L - 1; ++i) hist[i] = hist[n + i];
        return out;
    }
    downsample (x, n)
    {
        const T = this.taps, hist = this.downHist, u = this.up, h = this.h, N4 = n * 4;
        for (let i = 0; i < N4; ++i) hist[T - 1 + i] = u[i];
        for (let i = 0; i < n; ++i)
        {
            const end = T - 1 + i * 4 + 3;
            let acc = 0;
            for (let j = 0; j < T; ++j) acc += h[j] * hist[end - j];
            x[i] = acc;
        }
        for (let i = 0; i < T - 1; ++i) hist[i] = hist[N4 + i];
    }
}

// Uniformly partitioned convolution (partition = 128, the render quantum) for
// the Amp stage's impulse response.
export class Convolver
{
    constructor ()
    {
        this.B = 128; this.N = 256; this.fft = new FFT (256);
        this.parts = 0; this.irRe = []; this.irIm = []; this.fdlRe = []; this.fdlIm = []; this.pos = 0;
        this.inBuf = new Float64Array (256); this.re = new Float64Array (256); this.im = new Float64Array (256);
        this.accRe = new Float64Array (256); this.accIm = new Float64Array (256);
    }
    get loaded () { return this.parts > 0; }
    load (ir)
    {
        if (! ir || ir.length === 0) { this.parts = 0; return; }
        const B = this.B, N = this.N, parts = Math.min (64, Math.ceil (ir.length / B));
        this.irRe = []; this.irIm = []; this.fdlRe = []; this.fdlIm = [];
        for (let p = 0; p < parts; ++p)
        {
            const re = new Float64Array (N), im = new Float64Array (N);
            for (let i = 0; i < B; ++i) re[i] = ir[p * B + i] || 0;
            this.fft.transform (re, im, false);
            this.irRe.push (re); this.irIm.push (im);
            this.fdlRe.push (new Float64Array (N)); this.fdlIm.push (new Float64Array (N));
        }
        this.parts = parts; this.pos = 0; this.inBuf.fill (0);
    }
    process (d, n)
    {
        if (this.parts === 0 || n !== this.B) return;
        const B = this.B, N = this.N, re = this.re, im = this.im, inBuf = this.inBuf;
        for (let i = 0; i < B; ++i) { inBuf[i] = inBuf[B + i]; inBuf[B + i] = d[i]; }
        for (let i = 0; i < N; ++i) { re[i] = inBuf[i]; im[i] = 0; }
        this.fft.transform (re, im, false);
        this.pos = (this.pos + this.parts - 1) % this.parts;
        this.fdlRe[this.pos].set (re); this.fdlIm[this.pos].set (im);
        const aR = this.accRe, aI = this.accIm;
        aR.fill (0); aI.fill (0);
        for (let p = 0; p < this.parts; ++p)
        {
            const fr = this.fdlRe[(this.pos + p) % this.parts], fi = this.fdlIm[(this.pos + p) % this.parts];
            const hr = this.irRe[p], hi = this.irIm[p];
            for (let k = 0; k < N; ++k) { aR[k] += fr[k] * hr[k] - fi[k] * hi[k]; aI[k] += fr[k] * hi[k] + fi[k] * hr[k]; }
        }
        this.fft.transform (aR, aI, true);
        for (let i = 0; i < B; ++i) d[i] = aR[B + i] / N;
    }
}
