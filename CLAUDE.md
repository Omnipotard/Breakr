# BREAKR — handoff for Claude Code

Open-source (GPL-3.0) **breakbeat slicer firmware for the M-VAVE FM-1**, by Omnipotard (GitHub: Omnipotard — repo https://github.com/Omnipotard/Breakr).
User request (FR): « découpeur de breakbeat avec génération de slices et lecture aléatoire si on le souhaite,
interface soignée, workflow accessible, import de samples, FX type PO-33 / Akai Flex Beat, page GitHub dédiée ».
Talk to the user in French.

## Platform
Built on **Dinghy / Felucca / SLOOP**, exactly like the user's own firmware **KAWA**
(https://github.com/Guizmoh/Kawa-Tracker-Fm1) — copy conventions from there (site structure, fmhub.json,
installers, release workflow `zvenson/fm1-build`, BUILDING.md). Platform files in `source/firmware/{hal,loader,src/platform.c,usb.c,ota.c,...}`
are KAWA's, unchanged except names. The JieLi toolchain is NOT reachable from the cloud sandbox: the real `.fwsc`
must be built by CI (`zvenson/fm1-build` workflow, see KAWA's `.github/workflows/release.yml`) or locally with `source/build.sh`.

Chip rules: integers only, no 64-bit division, short audio interrupt, no hex literals in MMIO ranges outside hal/
(`tools/build.py` mmio_check). Package identity: **FM-1_940** (provisional — ask the user to confirm it is free on The Firmware Hub).

## App code (source/firmware/src/, one unit: breakr.c)
brk_core (shared state) · brk_ima (4-bit IMA, 256-B blocks) · brk_data (punch FX, 16 flex patterns, styles) ·
brk_snd (voices) · brk_fx (filter/drive/crush/delay/punch) · brk_seq (96 PPQN sequencer, half-step lookahead,
signed per-step nudge, dice FREE/LOCK, punch FX, flex, app_audio) · brk_slice (waveform, onsets, K/S/H classes) ·
brk_edit (slicing, generators, undo, defaults) · brk_store (4 project slots + user bank in flash) ·
brk_sysex (bank upload "F0 7D 42 4B") · brk_gfx (bands + tile hashing, Inter AA fonts) · brk_ui (10 pages, LEDs, MIDI).
Generated: `firmware/gen/brk_breaks.h` (tools/gen_breaks.py, 8 synthesized breaks), `brk_fonts.h` (tools/gen_fonts.py).

## Tests (all pass)
    cd source
    sh tests/run_engine_test.sh   # UBSan/ASan: fidelity, timing, dice LOCK, all FX at extremes, slicing, IMA, generators
    sh tests/run_host.sh          # 19 screenshots + audio in build/host/ — LOOK at the PNGs after UI changes
    sh web/make_emulator.sh && node tests/web_e2e.js   # brk.js <-> wasm firmware: upload, list, read back

## Done
Firmware engine + UI + store + SysEx; `web/brk.js` (IMA encoder, STFT onsets, classes, bank, Web MIDI/emulator upload);
`web/emulator.html` -> `emulator/index.html` (built, JS parses, NOT yet tried in a real browser).

## To do (in order)
1. Open `emulator/index.html` in a browser, power on, check screen/audio/keys/knobs/drop-a-loop; fix.
2. `editor/index.html` — Break Editor: import several loops, trim start/end, bars/BPM, waveform with draggable
   slice markers (auto / grid / manual), classes, preview playback, capacity meter (245 760 B), 8 breaks max,
   upload to FM-1 (BRK.webMidi) or emulator, read back from FM-1, save/load `.brkbank`. FR/EN. Same visual style.
3. Root `index.html` (GitHub Pages landing, FR/EN, like KAWA's), `README.md` + `README.fr.md`, manual,
   `LICENSING.md` (credits: Dinghy/zvenson, Felucca/Hügelton, SLOOP/isod89, KAWA, Inter OFL), `fmhub.json`, `source/BUILDING.md`.
4. `.github/workflows/`: host tests + wasm + pages deploy; release via `zvenson/fm1-build` (copy KAWA's).
5. Repo https://github.com/Omnipotard/Breakr exists (pushed). Enable Pages, tag v0.1 for a first CI build.
6. After a real build: check image size/RAM from build.py output; hardware test with the user.

Known limits: on-device AUTO slicing recall ~81%, class accuracy ~65% (editor's STFT slicer is better);
factory breaks IMA SNR ~22 dB (lo-fi by design).
