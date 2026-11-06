/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR's web library (the editor and the emulator share it): audio in, slices, 4-bit IMA ADPCM, the bank
 * of breaks, and the upload over SysEx, to an FM-1 (Web MIDI) or to the emulator. The same formats as the
 * firmware: firmware/src/brk_ima.c, brk_store.c, brk_sysex.c. Plain JavaScript, no build step. */
(function (root) {
  "use strict";
  const BRK = {};

  /* ---- 4-bit IMA ADPCM, blocks of 256 bytes = 505 samples (brk_ima.c) */
  const STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767];
  const IDX = [-1, -1, -1, -1, 2, 4, 6, 8];
  const SPB = 505, BLOCK = 256;
  function qn(pred, idx, s) {
    let d = s - pred, step = STEP[idx], nib = 0;
    if (d < 0) { nib = 8; d = -d; }
    if (d >= step) { nib |= 4; d -= step; }
    if (d >= step >> 1) { nib |= 2; d -= step >> 1; }
    if (d >= step >> 2) nib |= 1;
    return nib;
  }
  function ap(st, nib) {                         /* st = [pred, idx] */
    const step = STEP[st[1]];
    let diff = step >> 3;
    if (nib & 4) diff += step;
    if (nib & 2) diff += step >> 1;
    if (nib & 1) diff += step >> 2;
    let p = st[0] + ((nib & 8) ? -diff : diff);
    st[0] = p > 32767 ? 32767 : p < -32768 ? -32768 : p;
    let i = st[1] + IDX[nib & 7];
    st[1] = i < 0 ? 0 : i > 88 ? 88 : i;
  }
  BRK.imaBytes = (frames) => Math.ceil(frames / SPB) * BLOCK;
  /* each nibble the best of 16 over it and the next `look` samples (+1.5 dB on drums; the decoder is standard) */
  BRK.imaEncode = function (pcm, look) {
    look = look === undefined ? 2 : look;
    const n = pcm.length, out = new Uint8Array(BRK.imaBytes(n)), st = [0, 0], t = [0, 0];
    let idx = 0;
    for (let at = 0; at < n; at += SPB) {
      const b = (at / SPB) * BLOCK, end = Math.min(n, at + SPB);
      st[0] = pcm[at]; st[1] = idx;
      out[b] = pcm[at] & 255; out[b + 1] = (pcm[at] >> 8) & 255; out[b + 2] = idx; out[b + 3] = 0;
      for (let i = 1; i < SPB; i++) {
        let nib = 0;
        if (at + i < n) {
          let best = Infinity;
          for (let c = 0; c < 16; c++) {
            t[0] = st[0]; t[1] = st[1];
            ap(t, c);
            let e = (t[0] - pcm[at + i]) ** 2;
            for (let j = 1; j <= look && at + i + j < end; j++) {
              ap(t, qn(t[0], t[1], pcm[at + i + j]));
              e += (t[0] - pcm[at + i + j]) ** 2;
              if (e >= best) break;
            }
            if (e < best) { best = e; nib = c; }
          }
        }
        ap(st, nib);
        out[b + 4 + ((i - 1) >> 1)] |= (i & 1) ? nib : nib << 4;
      }
      idx = st[1];
    }
    return out;
  };
  BRK.imaDecode = function (data, frames) {
    const out = new Int16Array(frames), st = [0, 0];
    for (let blk = 0; blk * SPB < frames; blk++) {
      const b = blk * BLOCK, base = blk * SPB;
      st[0] = (data[b] | data[b + 1] << 8) << 16 >> 16; st[1] = Math.min(88, data[b + 2]);
      out[base] = st[0];
      for (let i = 1; i < SPB && base + i < frames; i++) {
        const v = data[b + 4 + ((i - 1) >> 1)];
        ap(st, (i & 1) ? v & 15 : v >> 4);
        out[base + i] = st[0];
      }
    }
    return out;
  };

  /* ---- audio in: any file the browser decodes, to mono at `rate` (a gentle low-pass first) */
  BRK.decodeFile = async function (file, ctx) {
    const buf = await file.arrayBuffer();
    const ab = await ctx.decodeAudioData(buf.slice(0));
    const n = ab.length, mono = new Float32Array(n);
    for (let c = 0; c < ab.numberOfChannels; c++) {
      const d = ab.getChannelData(c);
      for (let i = 0; i < n; i++) mono[i] += d[i] / ab.numberOfChannels;
    }
    return {pcm: mono, rate: ab.sampleRate, name: file.name.replace(/\.[^.]+$/, "")};
  };
  BRK.resample = function (x, from, to) {
    if (from === to) return Float32Array.from(x);
    const ratio = from / to, n = Math.floor(x.length / ratio), y = new Float32Array(n);
    let src = x;
    if (to < from) {                             /* two one-pole passes at ~0.42 of the new rate */
      const a = Math.exp(-2 * Math.PI * 0.42 * to / from);
      src = new Float32Array(x.length);
      let s1 = 0, s2 = 0;
      for (let i = 0; i < x.length; i++) { s1 = (1 - a) * x[i] + a * s1; s2 = (1 - a) * s1 + a * s2; src[i] = s2; }
      let r1 = 0, r2 = 0;                        /* (and backwards: no delay) */
      for (let i = x.length - 1; i >= 0; i--) { r1 = (1 - a) * src[i] + a * r1; r2 = (1 - a) * r1 + a * r2; src[i] = r2; }
    }
    for (let i = 0; i < n; i++) {
      const p = i * ratio, k = Math.floor(p), f = p - k;
      y[i] = (src[k] || 0) * (1 - f) + (src[k + 1] || 0) * f;
    }
    return y;
  };
  BRK.toInt16 = function (x, gain) {             /* normalised to -1 dBFS unless a gain is given */
    let peak = 1e-9;
    for (let i = 0; i < x.length; i++) peak = Math.max(peak, Math.abs(x[i]));
    const g = gain || 0.89 / peak, y = new Int16Array(x.length);
    for (let i = 0; i < x.length; i++) y[i] = Math.max(-32768, Math.min(32767, Math.round(x[i] * g * 32767)));
    return y;
  };

  /* ---- the tempo of a loop: the bars (1, 2, 4, 8) that put it nearest 120 BPM, inside 70..185 */
  BRK.guessBars = function (seconds) {
    let best = 1, bestd = Infinity;
    for (const bars of [1, 2, 4, 8]) {
      const bpm = bars * 240 / seconds;
      const d = bpm < 70 || bpm > 185 ? 1000 + Math.abs(bpm - 120) : Math.abs(bpm - 125);
      if (d < bestd) { bestd = d; best = bars; }
    }
    return best;
  };

  /* ---- slices: an STFT (1024, hop 128), the spectral flux in three bands; peaks over a moving mean; each
   * onset found again to the sample. The class from what rose: lows (kick), mids and highs (snare), highs (hat) */
  function fft(re, im) {
    const n = re.length;
    for (let i = 1, j = 0; i < n; i++) {
      let bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
    }
    for (let len = 2; len <= n; len <<= 1) {
      const ang = -2 * Math.PI / len, wr = Math.cos(ang), wi = Math.sin(ang);
      for (let i = 0; i < n; i += len) {
        let cr = 1, ci = 0;
        for (let k = 0; k < len / 2; k++) {
          const a = i + k, b = a + len / 2, tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
          re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
          const ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr;
        }
      }
    }
  }
  BRK.analyse = function (pcm, rate) {           /* pcm: Float32Array or Int16Array */
    const N = 1024, H = 128, nf = Math.max(1, Math.floor((pcm.length - N) / H) + 1);
    const scale = pcm instanceof Int16Array ? 1 / 32768 : 1;
    const win = new Float32Array(N);
    for (let i = 0; i < N; i++) win[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / N);
    const bin = (f) => Math.max(1, Math.min(N / 2 - 1, Math.round(f * N / rate)));
    const b150 = bin(150), b300 = bin(300), b2k = bin(2000), b5k = bin(5000);
    const lo = new Float32Array(nf), mid = new Float32Array(nf), hi = new Float32Array(nf), flux = new Float32Array(nf), en = new Float32Array(nf);
    let prev = new Float32Array(N / 2);
    const re = new Float32Array(N), im = new Float32Array(N);
    for (let f = 0; f < nf; f++) {
      const o = f * H;
      for (let i = 0; i < N; i++) { re[i] = (pcm[o + i] || 0) * scale * win[i]; im[i] = 0; }
      fft(re, im);
      const mag = new Float32Array(N / 2);
      let fl = 0, l = 0, m = 0, h = 0, e = 0;
      for (let k = 1; k < N / 2; k++) {
        const v = Math.log1p(10 * Math.hypot(re[k], im[k]));
        mag[k] = v;
        const d = v - prev[k];
        if (d > 0) fl += d * (k > b2k ? 1.4 : 1);
        if (k <= b150) l += v; else if (k >= b300 && k <= b2k) m += v; else if (k >= b5k) h += v;
        e += v;
      }
      prev = mag;
      flux[f] = fl; lo[f] = l / b150; mid[f] = m / (b2k - b300 + 1); hi[f] = h / (N / 2 - b5k); en[f] = e;
    }
    return {flux, lo, mid, hi, en, hop: H, n: N, rate};
  };
  BRK.onsets = function (pcm, rate, sens, an) {  /* sens 1..10: onset frame positions (samples), the first at 0 */
    an = an || BRK.analyse(pcm, rate);
    const f = an.flux, nf = f.length, out = [0], strength = [Infinity];
    const k = 1.9 - 0.13 * sens, gap = Math.round(0.045 * rate / an.hop);
    let last = -1e9;
    for (let i = 2; i < nf - 1; i++) {
      let mean = 0, c = 0;
      for (let j = Math.max(0, i - 12); j <= Math.min(nf - 1, i + 4); j++) { mean += f[j]; c++; }
      mean /= c;
      if (f[i] < f[i - 1] || f[i] < f[i + 1] || f[i] < mean * k + 0.5) continue;
      const at = i * an.hop + an.n / 2 - an.hop;   /* the window's middle, a hop early */
      if (i - last < gap) { if (f[i] > strength[strength.length - 1] && out.length > 1) { out[out.length - 1] = at; strength[strength.length - 1] = f[i]; last = i; } continue; }
      out.push(at); strength.push(f[i]); last = i;
    }
    /* each one to the sample: the first sample over a quarter of the peak around it, 1 ms before */
    const scale = pcm instanceof Int16Array ? 1 / 32768 : 1, res = [0];
    for (let j = 1; j < out.length; j++) {
      const a = Math.max(0, out[j] - an.n / 2), b = Math.min(pcm.length, out[j] + an.n / 2);
      let peak = 0;
      for (let i = a; i < b; i++) peak = Math.max(peak, Math.abs(pcm[i] * scale));
      let i = a;
      while (i < b && Math.abs(pcm[i] * scale) * 4 < peak) i++;
      const s = Math.max(0, i - Math.round(rate / 1000));
      if (s > res[res.length - 1] + rate / 40) res.push(s);
    }
    /* at most 32: the strongest */
    if (res.length > 32) {
      const idx = res.map((s, i) => [s, i === 0 ? Infinity : (an.flux[Math.min(an.flux.length - 1, Math.round(s / an.hop))] || 0)]);
      idx.sort((x, y) => y[1] - x[1]);
      return idx.slice(0, 32).map((x) => x[0]).sort((x, y) => x - y);
    }
    return res;
  };
  BRK.classify = function (starts, an) {         /* 0 other, 1 kick, 2 snare, 3 hat */
    const rise = (v, h) => {
      let m = Infinity, best = 0;
      for (let k = 1; k <= 3; k++) if (h - k >= 0) m = Math.min(m, v[h - k]);
      if (m === Infinity) m = 0;
      for (let k = 0; k < 4 && h + k < v.length; k++) best = Math.max(best, v[h + k] - m);
      return best;
    };
    return starts.map((s) => {
      const h = Math.max(0, Math.round(s / an.hop) - 2);
      const l = rise(an.lo, h), m = rise(an.mid, h), hh = rise(an.hi, h);
      if (l > 0.35 && l >= hh * 0.9) return 1;
      if (hh > 0.25 && m > 0.25) return 2;
      if (hh > 0.12) return 3;
      return l > hh ? 1 : 0;
    });
  };
  BRK.grid = (frames, count) => Array.from({length: count}, (_, i) => Math.floor(frames * i / count));

  /* ---- the bank (brk_store.c): header sector = header (32 B) + 8 entries of 224 B from byte 64; then the sound */
  const BANK_MAGIC = 0x424B5242;
  BRK.BANK_CAP = 0xE0000 - (0x97000 + 4 * 0x3000 + 0x1000);   /* 245760 B */
  BRK.MAX_BREAKS = 8;
  function crc32(bytes, c) {
    c = c === undefined ? 0xFFFFFFFF : c;
    for (let i = 0; i < bytes.length; i++) {
      c ^= bytes[i];
      for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0xEDB88320 : c >>> 1;
    }
    return c >>> 0;
  }
  BRK.crc32 = (b) => (~crc32(b)) >>> 0;
  /* breaks: [{name, rate, frames, data (IMA), bars, bpm10, sl: [..], cls: [..]}] */
  BRK.buildBank = function (breaks) {
    if (breaks.length > BRK.MAX_BREAKS) throw new Error("8 breaks at most");
    let total = 0;
    for (const b of breaks) total += b.data.length;
    if (total > BRK.BANK_CAP) throw new Error("the bank is " + total + " B, " + BRK.BANK_CAP + " B at most");
    const data = new Uint8Array(total), ents = new Uint8Array(breaks.length * 224), dv = new DataView(ents.buffer);
    let off = 0;
    breaks.forEach((b, i) => {
      const e = i * 224;
      const nm = (b.name || "BREAK").toUpperCase().replace(/[^ -_]/g, "").slice(0, 15);
      for (let k = 0; k < nm.length; k++) ents[e + k] = nm.charCodeAt(k);
      dv.setUint32(e + 16, b.rate, true);
      dv.setUint32(e + 20, b.frames, true);
      dv.setUint32(e + 24, off, true);
      dv.setUint32(e + 28, b.data.length, true);
      dv.setUint32(e + 32, b.bpm10 || 0, true);
      dv.setUint32(e + 36, BRK.crc32(b.data), true);
      ents[e + 40] = b.bars || 1;
      ents[e + 41] = Math.min(32, b.sl.length);
      for (let k = 0; k < 32; k++) dv.setUint32(e + 44 + 4 * k, k < b.sl.length ? b.sl[k] : 0, true);
      for (let k = 0; k < 32; k++) ents[e + 172 + k] = k < b.cls.length ? b.cls[k] & 3 : 0;
      data.set(b.data, off);
      off += b.data.length;
    });
    const hdr = new Uint8Array(32), hv = new DataView(hdr.buffer);
    hv.setUint32(0, BANK_MAGIC, true);
    hv.setUint32(4, 1, true);
    hv.setUint32(8, breaks.length, true);
    hv.setUint32(12, total, true);
    hv.setUint32(16, BRK.crc32(data), true);
    hv.setUint32(20, BRK.crc32(ents), true);
    return {hdr, ents, data};
  };
  /* a bank file (.brkbank): "BRKF", then the header, the entries, the sound */
  BRK.bankFile = function (bank) {
    const f = new Uint8Array(8 + 32 + bank.ents.length + bank.data.length);
    f.set([0x42, 0x52, 0x4B, 0x46, 1, 0, 0, 0]);
    f.set(bank.hdr, 8); f.set(bank.ents, 40); f.set(bank.data, 40 + bank.ents.length);
    return f;
  };
  BRK.readBankFile = function (f) {
    if (f[0] !== 0x42 || f[1] !== 0x52 || f[2] !== 0x4B || f[3] !== 0x46) throw new Error("not a BREAKR bank");
    const hdr = f.slice(8, 40), hv = new DataView(hdr.buffer), n = hv.getUint32(8, true), len = hv.getUint32(12, true);
    const ents = f.slice(40, 40 + n * 224), data = f.slice(40 + n * 224, 40 + n * 224 + len);
    return BRK.bankBreaks({hdr, ents, data});
  };
  BRK.bankBreaks = function (bank) {             /* a bank back into breaks (to edit them) */
    const dv = new DataView(bank.ents.buffer, bank.ents.byteOffset), out = [];
    for (let i = 0; i < bank.ents.length / 224; i++) {
      const e = i * 224;
      let name = "";
      for (let k = 0; k < 16 && bank.ents[e + k]; k++) name += String.fromCharCode(bank.ents[e + k]);
      const off = dv.getUint32(e + 24, true), len = dv.getUint32(e + 28, true), nsl = bank.ents[e + 41];
      const b = {name, rate: dv.getUint32(e + 16, true), frames: dv.getUint32(e + 20, true), bpm10: dv.getUint32(e + 32, true),
        bars: bank.ents[e + 40], data: bank.data.slice(off, off + len), sl: [], cls: []};
      for (let k = 0; k < nsl; k++) { b.sl.push(dv.getUint32(e + 44 + 4 * k, true)); b.cls.push(bank.ents[e + 172 + k]); }
      out.push(b);
    }
    return out;
  };

  /* ---- the SysEx protocol (brk_sysex.c): F0 7D 42 4B cmd pack7(args) F7 */
  const CMD = {INFO: 1, BEGIN: 2, WRITE: 3, ENTRY: 4, END: 5, ERASE: 6, LIST: 7, READ: 8};
  BRK.CMD = CMD;
  const RC = ["ok", "bad arguments", "busy: stop the FM-1 first", "flash error", "CRC mismatch", "out of order"];
  function pack7(b) {
    const o = [];
    let acc = 0, nb = 0;
    for (const x of b) { acc |= x << nb; nb += 8; while (nb >= 7) { o.push(acc & 0x7F); acc >>>= 7; nb -= 7; } }
    if (nb) o.push(acc & 0x7F);
    return o;
  }
  function unpack7(b) {
    const o = [];
    let acc = 0, nb = 0;
    for (const x of b) { acc |= (x & 0x7F) << nb; nb += 7; if (nb >= 8) { o.push(acc & 0xFF); acc >>>= 8; nb -= 8; } }
    return new Uint8Array(o);
  }
  BRK.frame = (cmd, args) => new Uint8Array([0xF0, 0x7D, 0x42, 0x4B, cmd, ...pack7(args || []), 0xF7]);
  BRK.parse = function (f) {                     /* a reply: {cmd, data} or null */
    if (!f || f.length < 6 || f[0] !== 0xF0 || f[1] !== 0x7D || f[2] !== 0x42 || f[3] !== 0x4B || f[f.length - 1] !== 0xF7) return null;
    return {cmd: f[4], data: unpack7(f.subarray(5, f.length - 1))};
  };

  /* a transport: request(frame) -> Promise(reply frame). The emulator's, or Web MIDI's */
  BRK.emulatorTransport = function (ex) {        /* ex: the wasm exports (breakr_sysex_buf / breakr_sysex / breakr_reply) */
    return {
      name: "emulator",
      request(f) {
        const mem = () => new Uint8Array(ex.memory.buffer);
        mem().set(f, ex.breakr_sysex_buf());
        const n = ex.breakr_sysex(f.length);
        return Promise.resolve(n ? mem().slice(ex.breakr_reply(), ex.breakr_reply() + n) : null);
      }
    };
  };
  BRK.webMidi = async function () {              /* the FM-1 over USB-MIDI (Chrome, Edge, Opera: SysEx allowed) */
    if (!navigator.requestMIDIAccess) throw new Error("this browser has no Web MIDI (use Chrome or Edge)");
    const acc = await navigator.requestMIDIAccess({sysex: true});
    const pick = (map) => {
      let first = null;
      for (const p of map.values()) { if (/fm-?1|m-?vave|breakr|felucca|dinghy/i.test(p.name || "")) return p; first = first || p; }
      return first;
    };
    const inp = pick(acc.inputs), out = pick(acc.outputs);
    if (!inp || !out) throw new Error("no FM-1 found: a USB data cable, the FM-1 on, no other MIDI program open");
    let waiting = null;
    inp.onmidimessage = (ev) => {
      const d = ev.data;
      if (d[0] === 0xF0 && d[1] === 0x7D && d[2] === 0x42 && d[3] === 0x4B && waiting) { const w = waiting; waiting = null; w(d); }
    };
    return {
      name: out.name,
      request(f, ms) {
        return new Promise((res) => {
          const t = setTimeout(() => { waiting = null; res(null); }, ms || 1500);
          waiting = (d) => { clearTimeout(t); res(d); };
          out.send(Array.from(f));
        });
      },
      close() { inp.onmidimessage = null; }
    };
  };

  async function ask(tr, cmd, args, tries) {
    for (let t = 0; t < (tries || 4); t++) {
      const r = BRK.parse(await tr.request(BRK.frame(cmd, args)));
      if (r && r.cmd === cmd) return r.data;
    }
    throw new Error("the FM-1 does not answer (is BREAKR running on it?)");
  }
  BRK.info = async function (tr) {
    const d = await ask(tr, CMD.INFO, []);
    const s = (a, b) => String.fromCharCode(...d.slice(a, b));
    return {rc: d[0], app: s(1, 5), version: s(5, 9).trim(), max: d[9], cap: d[10] | d[11] << 8 | d[12] << 16, count: d[13]};
  };
  BRK.list = async function (tr) {
    const out = [];
    for (let i = 0; i < 8; i++) {
      const d = await ask(tr, CMD.LIST, [i]);
      if (!d[2]) continue;
      let name = "";
      for (let k = 0; k < 12 && d[3 + k]; k++) name += String.fromCharCode(d[3 + k]);
      out.push({slot: i, name, rate: d[15] | d[16] << 8, frames: d[17] | d[18] << 8 | d[19] << 16, bars: d[20], slices: d[21]});
    }
    return out;
  };
  /* send a bank: BEGIN, the sound by 256 B, the entries, END; progress(0..1) */
  BRK.upload = async function (tr, bank, progress) {
    const check = (d, what) => { if (d[0] !== 0) throw new Error(what + ": " + (RC[d[0]] || "error " + d[0])); };
    const info = await BRK.info(tr);
    if (info.app !== "BRKR") throw new Error("the FM-1 does not run BREAKR");
    if (bank.data.length > info.cap) throw new Error("too big for this FM-1");
    check(await ask(tr, CMD.BEGIN, []), "begin");
    const n = bank.data.length, chunks = Math.ceil(n / 256);
    for (let c = 0; c < chunks; c++) {
      const off = c * 256, part = bank.data.subarray(off, Math.min(n, off + 256));
      check(await ask(tr, CMD.WRITE, [off & 255, off >> 8 & 255, off >> 16 & 255, ...part]), "write");
      if (progress && (c & 7) === 0) progress(c / chunks);
    }
    for (let i = 0; i < bank.ents.length / 224; i++)
      check(await ask(tr, CMD.ENTRY, [i, ...bank.ents.subarray(i * 224, i * 224 + 224)]), "entry");
    check(await ask(tr, CMD.END, [...bank.hdr]), "end");
    if (progress) progress(1);
  };
  BRK.erase = async function (tr) { const d = await ask(tr, CMD.ERASE, []); if (d[0]) throw new Error(RC[d[0]]); };
  BRK.readBank = async function (tr, progress) { /* the bank as it is on the FM-1 (READ): its breaks */
    const rd = async (off, n) => { const d = await ask(tr, CMD.READ, [off & 255, off >> 8 & 255, off >> 16 & 255, n]); return d.slice(4, 4 + n); };
    const head = new Uint8Array(4096);
    for (let o = 0; o < 64 + 8 * 224; o += 96) head.set(await rd(o, Math.min(96, 64 + 8 * 224 - o)), o);
    const hv = new DataView(head.buffer);
    if (hv.getUint32(0, true) !== BANK_MAGIC) return [];
    const count = hv.getUint32(8, true), len = hv.getUint32(12, true), data = new Uint8Array(len);
    for (let o = 0; o < len; o += 96) { data.set(await rd(4096 + o, Math.min(96, len - o)), o); if (progress && (o / 96 & 15) === 0) progress(o / len); }
    return BRK.bankBreaks({hdr: head.slice(0, 32), ents: head.slice(64, 64 + count * 224), data});
  };

  root.BRK = BRK;
  if (typeof module !== "undefined") module.exports = BRK;
})(typeof window !== "undefined" ? window : globalThis);
