// SPDX-License-Identifier: GPL-3.0-only
// web/brk.js against the firmware compiled to WebAssembly: IMA, onsets, the bank, the SysEx upload, read back.
// sh web/make_emulator.sh first, then: node tests/web_e2e.js
const fs = require('fs');
const BRK = require(__dirname + '/../web/brk.js');
const bytes = fs.readFileSync(__dirname + '/../build/web/breakr.wasm');
const ex = new WebAssembly.Instance(new WebAssembly.Module(bytes), {}).exports;
ex.breakr_init();
for (let i = 0; i < 50; i++) { ex.breakr_audio(); ex.breakr_step(); }
// a synthetic break: noise bursts every 1/8 at 120 BPM, 2 s
const rate = 22050, n = rate * 2, x = new Float32Array(n);
for (let k = 0; k < 16; k++) { const at = Math.floor(k * n / 16); for (let i = 0; i < 2000; i++) x[at + i] += (Math.random() * 2 - 1) * Math.exp(-i / 400) * (k % 4 === 0 ? 1 : 0.5); }
const pcm = BRK.toInt16(x);
const data = BRK.imaEncode(pcm);
const dec = BRK.imaDecode(data, n);
let s = 0, e = 0; for (let i = 0; i < n; i++) { s += pcm[i] ** 2; e += (pcm[i] - dec[i]) ** 2; }
console.log('IMA SNR', (10 * Math.log10(s / e)).toFixed(1), 'dB');
const an = BRK.analyse(pcm, rate), on = BRK.onsets(pcm, rate, 6, an);
console.log('onsets', on.length, on.slice(0, 6));
const bank = BRK.buildBank([{name: 'Test break', rate, frames: n, data, bars: 1, bpm10: 1200, sl: on, cls: BRK.classify(on, an)}]);
const tr = BRK.emulatorTransport(ex);
(async () => {
  console.log('info', await BRK.info(tr));
  await BRK.upload(tr, bank, () => {});
  console.log('list', await BRK.list(tr));
  const back = await BRK.readBank(tr);
  console.log('readback', back.length, back[0].name, back[0].frames, 'same data', back[0].data.every((v, i) => v === data[i]));
  const again = BRK.readBankFile(BRK.bankFile(bank));
  console.log('file roundtrip', again[0].sl.length === on.length);
})().catch(e => { console.error('FAIL', e); process.exit(1); });
