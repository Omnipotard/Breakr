// SPDX-License-Identifier: GPL-3.0-only
// web/brk.js against the real firmware compiled to WebAssembly (build/web/breakr.wasm, web/make_emulator.sh):
// a bank of breaks built in JavaScript is sent over the SysEx protocol, read back, and must be identical;
// then LIST, the store (what the page keeps) and ERASE.   node tests/web_e2e.js
"use strict";
const fs = require("fs");
const path = require("path");
const BRK = require("../web/brk.js");

function fail(m) { console.error("FAIL " + m); process.exit(1); }
function eq(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }

(async () => {
  const wasm = fs.readFileSync(path.join(__dirname, "..", "build", "web", "breakr.wasm"));
  const {instance} = await WebAssembly.instantiate(wasm, {});
  const ex = instance.exports;
  ex.breakr_init();
  for (let i = 0; i < 8; i++) { ex.breakr_audio(); ex.breakr_step(); }
  const tr = BRK.emulatorTransport(ex);

  const info = await BRK.info(tr);
  if (info.app !== "BRKR") fail("INFO app " + info.app);
  console.log("info      " + info.app + " " + info.version + ", " + info.max + " breaks, " + info.cap + " B");

  // three breaks: a synthetic drum-ish loop each, IMA, slices on a grid and from the onsets
  const rate = 22050, breaks = [];
  for (let k = 0; k < 3; k++) {
    const frames = rate * (k + 1), x = new Float32Array(frames);
    const hit = Math.floor(rate / (4 + k));
    for (let i = 0; i < frames; i++) {
      const t = i % hit;
      x[i] = Math.exp(-t / (300 + 200 * k)) * Math.sin(2 * Math.PI * (60 + 40 * k) * t / rate) + 0.05 * Math.sin(i * 0.37 * (k + 1));
    }
    const pcm = BRK.toInt16(x);
    const sl = k === 1 ? BRK.onsets(pcm, rate, 5) : BRK.grid(frames, 8 * (k + 1));
    const cls = k === 1 ? BRK.classify(sl, BRK.analyse(pcm, rate)) : sl.map((_, i) => i & 3);
    breaks.push({name: "TEST " + (k + 1), rate, frames, data: BRK.imaEncode(pcm), bars: k + 1, bpm10: 1200, sl, cls});
  }
  const bank = BRK.buildBank(breaks);
  let last = 0;
  await BRK.upload(tr, bank, (p) => { last = p; });
  if (last !== 1) fail("progress did not reach 1");
  console.log("upload    " + breaks.length + " breaks, " + bank.data.length + " B");

  const list = await BRK.list(tr);
  if (list.length !== 3) fail("LIST: " + list.length + " breaks");
  list.forEach((l, i) => {
    if (l.name !== breaks[i].name.slice(0, 12) || l.frames !== breaks[i].frames || l.slices !== Math.min(32, breaks[i].sl.length))
      fail("LIST " + i + " " + JSON.stringify(l));
  });
  console.log("list      " + list.map((l) => l.name + " (" + l.slices + ")").join(", "));

  const back = await BRK.readBank(tr);
  if (back.length !== breaks.length) fail("READ: " + back.length + " breaks");
  back.forEach((b, i) => {
    const a = breaks[i];
    if (b.name !== a.name || b.rate !== a.rate || b.frames !== a.frames || b.bars !== a.bars || b.bpm10 !== a.bpm10) fail("READ entry " + i);
    if (!eq(b.data, a.data)) fail("READ data " + i);
    if (!eq(b.sl, a.sl.slice(0, 32)) || !eq(b.cls, a.cls.slice(0, 32).map((c) => c & 3))) fail("READ slices " + i);
  });
  console.log("read back identical");

  // the bank file round trip
  const f = BRK.bankFile(bank), rb = BRK.readBankFile(f);
  if (rb.length !== 3 || !eq(rb[2].data, breaks[2].data)) fail("bank file");

  // the store the page keeps: copy it, a fresh instance, put it back, the breaks are there
  const st = new Uint8Array(ex.memory.buffer, ex.breakr_store(), ex.breakr_store_size()).slice();
  const {instance: i2} = await WebAssembly.instantiate(wasm, {});
  const e2 = i2.exports;
  e2.breakr_init();
  new Uint8Array(e2.memory.buffer, e2.breakr_store(), e2.breakr_store_size()).set(st);
  e2.breakr_store_loaded();
  for (let i = 0; i < 4; i++) { e2.breakr_audio(); e2.breakr_step(); }
  const l2 = await BRK.list(BRK.emulatorTransport(e2));
  if (l2.length !== 3) fail("store reload: " + l2.length + " breaks");
  console.log("store     kept across a reload");

  await BRK.erase(tr);
  if ((await BRK.list(tr)).length !== 0) fail("ERASE");
  if ((await BRK.info(tr)).count !== 0) fail("ERASE count");
  console.log("erase     ok");

  // a bad frame and a write out of order are refused, not crashing
  if (ex.breakr_sysex(0) !== 0) fail("empty frame answered");
  const d = BRK.parse(await tr.request(BRK.frame(BRK.CMD.WRITE, [0, 1, 0, 1, 2, 3])));
  if (!d || d.data[0] === 0) fail("write without a session accepted");

  console.log("\nWEB E2E PASSED");
})().catch((e) => fail(e.stack || e));
