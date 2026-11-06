# BREAKR — breakbeat slicer firmware for the M-VAVE FM-1 (work in progress)

Open-source (GPL-3.0) firmware on the Dinghy / Felucca platform (same base as KAWA).

**Done:** 8 synthesized factory breaks (IMA 4-bit), slicing (as-is / grid / auto transients + kick/snare/hat classes),
64-step sequencer with groove-preserving micro-timing, dice (random slice / reverse / roll / skip / pitch, FREE or LOCKed),
pattern generators (8 styles), 11 PO-33-style punch FX on the black keys, 16 Flex-Beat-style patterns, master FX
(filter, drive, crush, delay, punch), 10-page anti-aliased UI, project slots, user break bank over USB SysEx,
host tests (UBSan/ASan) and a WebAssembly build.

**To do:** emulator page, break editor page (web/brk.js is ready), docs/site, CI release (zvenson/fm1-build), hardware test.

    cd source
    sh tests/run_engine_test.sh      # engine checks
    sh tests/run_host.sh             # screens + audio in build/host/
    sh web/make_emulator.sh          # wasm build
    ./build.sh                       # device .fwsc (JieLi toolchain, see BUILDING.md of KAWA)
