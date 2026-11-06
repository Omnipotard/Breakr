/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: a breakbeat slicer for the M-VAVE FM-1.
 *
 * Eight factory breaks and eight of yours (sent from the editor over USB), cut into up to 32 slices (as they
 * came, a grid, or their hits), played from the keys, from a 64-step sequencer, or at random (the dice: another
 * slice of the same kind, backwards, rolls, skips, jumps; locked on a seed or free), with the 11 punch effects of
 * a Pocket Operator on the black keys, 16 flex patterns in the manner of Flex Beat on the white ones, and a
 * filter, drive, crush, delay and compressor on the master.
 *
 * Built on Dinghy (zvenson), on Felucca (Hugelton Instruments) and SLOOP (isod89), as KAWA: platform.c gives
 * updates, the USB rescue, USB-MIDI and audio, the panel, the screen and flash.
 *
 * One compilation unit:
 *   brk_core.c    what the interrupt and the main loop share: breaks, project, events
 *   brk_ima.c     4-bit IMA ADPCM blocks                       (audio interrupt: decode)
 *   brk_data.c    names, the punch effects, the flex patterns, the generator's styles
 *   brk_snd.c     the voices                                   (audio interrupt)
 *   brk_fx.c      the master                                   (audio interrupt)
 *   brk_seq.c     the sequencer, the dice, the flex, app_audio (audio interrupt)
 *   brk_slice.c   looking at a break: waveform, hits, classes
 *   brk_edit.c    slicing, the generators, undo, the defaults
 *   brk_store.c   the flash: projects, the bank of breaks
 *   brk_sysex.c   the bank over USB-MIDI (SysEx)
 *   brk_gfx.c     drawing in bands and tiles
 *   brk_ui.c      the pages, the panel, the LEDs, MIDI, app_main
 * Rules of the chip (Dinghy's docs/RULES.md): integers only, no 64-bit division, the audio interrupt short. */
#define APP_NAME "BREAKR"
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_940"                   /* BREAKR's identity (tools/build.py passes the same) */
#endif
#if !defined(DINGHY_HOST) && !defined(FM1_WASM)  /* (the host builds keep the store in memory) */
#ifndef STORE_LO
#define STORE_LO 0x00097000u                    /* the store: 4 project slots of 12 KiB, then the bank of breaks */
#define STORE_HI 0x000E0000u                    /* up to the update staging (0xE0000) */
#endif
#endif
#include "platform.c"
#include "brk_fonts.h"
#include "brk_breaks.h"
#include "brk_core.c"
#include "brk_ima.c"
#include "brk_data.c"
#include "brk_snd.c"
#include "brk_fx.c"
#include "brk_seq.c"
#include "brk_slice.c"
#include "brk_edit.c"
#include "brk_store.c"
#include "brk_sysex.c"
#include "brk_gfx.c"
#include "brk_ui.c"
