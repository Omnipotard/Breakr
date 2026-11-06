# BREAKR's recorded factory breaks

These drum loops were made by **Omnipotard** for BREAKR, and are dedicated to the public domain:
**CC0 1.0 Universal** (https://creativecommons.org/publicdomain/zero/1.0/). You may copy, modify and use them
for any purpose, without asking and without credit.

| File | Break on the FM-1 | Tempo | Bars | Taken from |
|---|---|---|---|---|
| `omni142a.wav` | OMNI 142 A | 142 BPM | 2 | TD_142_Drum_loop_20, bars 3-4 |
| `omni142b.wav` | OMNI 142 B | 142 BPM | 2 | TD_142_Drum_loop_29, bars 1-2 |
| `omni72a.wav`  | OMNI 72 A  | 72 BPM  | 1 | PF_72_DRUMLOOP_02, bar 1 |
| `omni72b.wav`  | OMNI 72 B  | 72 BPM  | 1 | PF_72_DRUMLOOP_03, bar 4 |

Mono, 16-bit, 44.1 kHz, cut exactly on the bars (the originals are 4-bar 24-bit stereo loops; the bars kept
fit the firmware's flash). `tools/gen_breaks.py` reads them, finds their hits and their classes, and writes
them as 4-bit IMA ADPCM at 22.05 kHz into `firmware/gen/brk_breaks.h`.

The other four factory breaks are synthesised by the same script (no recording).

---

# Les breaks enregistrés de BREAKR

Ces boucles de batterie ont été faites par **Omnipotard** pour BREAKR et sont versées au domaine public :
**CC0 1.0 Universel**. Tu peux les copier, les modifier et les utiliser pour tout, sans demander et sans citer.
