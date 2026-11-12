#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# BREAKR in a browser: compile the firmware to WebAssembly (clang, wasm32, no libc) and put it, with web/brk.js,
# into the page.
#   web/make_emulator.sh            -> build/web/breakr.wasm, ../emulator/index.html (a whole page, for GitHub Pages)
# Needs clang with the wasm32 target and wasm-ld (lld); nothing else. (As KAWA's.)
set -e
cd "$(dirname "$0")/.."
mkdir -p build/web ../emulator
clang --target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin -Wall -Wno-unused-function \
    -DFM1_WASM -DAPP_VER="\"${APP_VER:-0.4}\"" -Ifirmware/src -Ifirmware/gen \
    -Wl,--no-entry -Wl,--export-dynamic -Wl,--initial-memory=4194304 -Wl,-z,stack-size=262144 \
    -o build/web/breakr.wasm firmware/src/breakr.c
BRK_VER="${APP_VER:-0.4}" python3 - <<'PY'
import base64
import os
from pathlib import Path
b64 = base64.b64encode(Path("build/web/breakr.wasm").read_bytes()).decode()
import json
brk = json.dumps(Path("web/brk.js").read_text()).replace("</", "<\\/")
page = Path("web/emulator.html").read_text()
page = page.replace("__BREAKR_BRKJS__", brk).replace("__BREAKR_VER__", os.environ["BRK_VER"]).replace("__BREAKR_WASM__", b64)
Path("../emulator/index.html").write_text(page)
print("wasm", Path("build/web/breakr.wasm").stat().st_size, "B; page", len(page), "B")
PY
