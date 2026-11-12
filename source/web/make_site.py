#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""BREAKR's site, from source/web/ (run from source/, after ./build.sh --release X.Y and web/make_emulator.sh):

  ../editor/index.html   the break editor (web/editor.html + web/brk.js)
  ../installer.html      the web installer (web/installer.html + ../install/webinstall.js + the package, inlined)
  ../install/breakr-X.Y.fwsc and ../install/SHA256SUMS   the package the installers use

  python3 web/make_site.py X.Y
"""
import base64
import hashlib
import json
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
TOP = SRC.parent


def main():
    ver = sys.argv[1] if len(sys.argv) > 1 else "0.4"
    pkg = SRC / "build" / f"breakr-{ver}.fwsc"
    if not pkg.exists():
        raise SystemExit(f"make_site: no {pkg} (./build.sh --release {ver} first)")
    brk = json.dumps((SRC / "web" / "brk.js").read_text()).replace("</", "<\\/")
    ed = (SRC / "web" / "editor.html").read_text().replace("__BREAKR_BRKJS__", brk)
    (TOP / "editor").mkdir(exist_ok=True)
    (TOP / "editor" / "index.html").write_text(ed)
    raw = pkg.read_bytes()
    (TOP / "install").mkdir(exist_ok=True)
    for old in (TOP / "install").glob("breakr-*.fwsc"):     # the newest one only (older ones: install/older/)
        if old.name != pkg.name:
            (TOP / "install" / "older").mkdir(exist_ok=True)
            old.rename(TOP / "install" / "older" / old.name)
    (TOP / "install" / pkg.name).write_bytes(raw)
    sums = [f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}" for p in sorted((TOP / "install").glob("*.fwsc"))]
    (TOP / "install" / "SHA256SUMS").write_text("\n".join(sums) + "\n")
    wi = (TOP / "install" / "webinstall.js").read_text().replace("</script", "<\\/script")
    inst = (SRC / "web" / "installer.html").read_text()
    inst = inst.replace("__BREAKR_WEBINSTALL__", wi).replace("__BREAKR_PKG_B64__", base64.b64encode(raw).decode())
    inst = inst.replace("__BREAKR_PKG__", pkg.name).replace("__BREAKR_VER__", ver)
    (TOP / "installer.html").write_text(inst)
    print(f"editor {len(ed)} B, installer {len(inst)} B, package {pkg.name} {len(raw)} B")


if __name__ == "__main__":
    main()
