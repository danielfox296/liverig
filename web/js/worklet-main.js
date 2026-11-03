// The AudioWorkletProcessor shell around Engine. app.js concatenates dsp.js,
// engine.js and this file into one module, so imports are stripped there.
import { Engine } from './engine.js';

class LiveRigProcessor extends AudioWorkletProcessor
{
    constructor (options)
    {
        super ();
        const o = options.processorOptions || {};
        this.engine = new Engine (sampleRate);
        this.engine.init (o.specs, o.values);
        if (o.strips) this.engine.setStrips (o.strips);
        if (o.patch) this.engine.instrument.setPatch (o.patch);
        this.blocks = 0;
        this.port.onmessage = (e) => this.onMessage (e.data);
    }
    onMessage (m)
    {
        const e = this.engine;
        switch (m.t)
        {
            case 'p': e.set (m.c, m.id, m.v); break;
            case 'values': e.load (m.values); break;
            case 'strips': e.setStrips (m.strips); break;
            case 'loop':
                if (m.cmd === 'tap') e.looper.tapTempo (m.ms);
                else if (m.cmd === 'bars') e.looper.tapBars ();
                else if (m.cmd === 'tempo') e.looper.setTempo (m.v);
                else if (m.cmd === 'setBars') e.looper.setBars (m.v);
                else e.looper.press (m.cmd, m.arg);
                break;
            case 'note': e.note (m.on, m.note, m.vel); break;
            case 'patch': e.instrument.setPatch (m.name); break;
            case 'panic': e.instrument.panic (); e.chains[0].segmenter.reset (); break;
            case 'ir': e.chains[m.c].amp.load (m.data); break;
            case 'tuner': e.tunerSlot = m.slot; break;
            case 'latency': e.roundTrip = m.roundTrip; e.keysRoundTrip = m.keys; break;
        }
    }
    process (inputs, outputs)
    {
        const out = outputs[0];
        if (! out || out.length === 0) return true;
        const n = out[0].length;
        const inp = inputs[0] || [];
        const L = out[0], R = out[1] || out[0];
        this.engine.process (inp, L, R, n);
        const ev = this.engine.midiEvents;
        if (ev.length) this.port.postMessage ({ t: 'midi', ev: ev.slice () });
        if (++this.blocks % 12 === 0)
        {
            const e = this.engine, tr = e.tunerSlot >= 0 ? e.chains[e.tunerSlot].tracker : null;
            this.port.postMessage ({
                t: 'meters',
                peaks: Array.from (e.peaks), inPeaks: Array.from (e.inPeaks), out: e.outPeak.slice (), gr: e.grDb,
                vocal: e.vocal.peak || 0,
                tuner: tr ? { hz: tr.frequency, conf: tr.confidence, peak: e.peaks[e.tunerSlot] } : null,
                loop: e.looper.snapshot (),
                fxLatency: e.chains[0].latency,
                note: e.chains[0].segmenter.sounding,
                time: currentTime,
            });
        }
        return true;
    }
}

registerProcessor ('liverig', LiveRigProcessor);
