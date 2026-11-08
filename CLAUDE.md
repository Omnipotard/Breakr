# BREAKR — notes for Claude Code

Open-source (GPL-3.0) **breakbeat slicer firmware for the M-VAVE FM-1**, by Omnipotard.
Repo https://github.com/Omnipotard/Breakr · site (GitHub Pages, from `main`, root) https://omnipotard.github.io/Breakr/.
Talk to the user in French. Breakbeat, breakcore, IDM (Squarepusher-style drill'n'bass) is the target.

## Platform
Built on **Dinghy / Felucca / SLOOP**, like the user's firmware **KAWA** (https://github.com/Guizmoh/Kawa-Tracker-Fm1).
Platform files in `source/firmware/{hal,loader,src/platform.c,usb.c,ota.c,...}` are KAWA's, unchanged except names.
Package identity **FM-1_2002** (confirmed by the user). Chip rules: integers only, no 64-bit division (32×32→64
multiplies are native), short audio interrupt (256-frame blocks, 44.1 kHz, Q15), no hex literals in MMIO ranges
outside hal/ (`tools/build.py` mmio_check).

Real build: `tools/get_toolchain.sh` (JieLi toolchain) + the AC79 SDK (gitee, tag AC79NN_SDK_V1.2.1_2023-12-13);
the cloud environment must allow pkgman.jieliapp.com, gitee.com and jl-update.oss-cn-shenzhen.aliyuncs.com.
`build.py` prints the budgets (0.2: image ~380 KB of 581 KB, RAM ~23 KB of 96 KB, pool ~320 KB of 336 KB).

## App code (source/firmware/src/, one unit: breakr.c)
brk_core (shared state) · brk_ima (4-bit IMA, 256-B blocks) · brk_data (punch FX + their names, 16 flex patterns,
styles, labels) · brk_snd (voices) · brk_fx (master: TPT state-variable DJ filter, drive, crush, delay, punch) ·
brk_beat (K-GLITCH's BEAT bank, FX ×3, effects 16..31, and DESTROY bank, FX ×4, effects 0..15, one engine) ·
brk_seq (96 PPQN sequencer, ratchets ×1..×16 with ramps, conditions, nudge, dice FREE/LOCK, flex, step FX locks) ·
brk_slice (waveform, onsets, K/S/H classes) · brk_edit (slicing, generators incl. DRILL/BREAKCORE/IDM, undo) ·
brk_rsmp (RESAMPLE to the user bank) · brk_store (4 project slots + user bank in flash) · brk_sysex (bank upload
"F0 7D 42 4B") · brk_gfx (bands, tile hashing, Barlow Condensed + Silkscreen fonts) · brk_ui (pages, hint strip,
help, punch window on FX pages, LEDs, MIDI).
Generated: `firmware/gen/brk_breaks.h` (tools/gen_breaks.py: 4 loops played by Omnipotard, CC0, + 4 synthesized),
`brk_fonts.h` (tools/gen_fonts.py).

## Tests (all pass at 0.2)
    cd source
    sh tests/run_engine_test.sh            # UBSan/ASan: fidelity, timing, dice, filter sweep, glitch, steps, generators
    APP_VER=0.2 sh tests/run_host.sh       # tests/tour.txt -> build/host/breakr-*.png + audio. LOOK at the PNGs.
    APP_VER=0.2 sh web/make_emulator.sh && node tests/web_e2e.js
    HOME=/home/user ./build.sh --release 0.2
    cp build/breakr-0.2.fwsc build/breakr.fwsc && sh tests/run_loader_tests.sh
    python3 web/make_site.py 0.2           # editor/index.html, installer.html, install/

## Site
`index.html`, `manual.html`, `site.css` by hand; `emulator/index.html` (web/emulator.html), `editor/index.html`
(web/editor.html + brk.js), `installer.html` (web/installer.html + install/webinstall.js + the package) generated.
`docs/img/*.png` are copies of build/host/breakr-<name>.png (names in tests/tour.txt) + `card.png` (four screens and the
logo, 1200×630, PIL). `fmhub.json` for The Firmware Hub. CI: `.github/workflows/build.yml` (branches `test/X.Y…`).

## Next
1. Hardware test with the user (sound of the filter and the glitch banks, timing, flash writes, RESAMPLE).
2. A release (tag + the `.fwsc`) once it has been played on a real FM-1.

Known limits: on-device AUTO slicing recall ~77 %, class accuracy ~60 % (engine test; the editor's STFT slicer is better);
factory breaks IMA SNR ~22 dB (lo-fi by design).
