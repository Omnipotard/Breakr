/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: names and fixed tables: the punch effects, the flex patterns, the generator's styles. */

/* the screen's two languages: TR(french, english) */
static uint8_t lang __attribute__((unused)) = 1;                        /* 0 FR, 1 EN */
#define TR(fr, en) (lang ? (en) : (fr))

/* the punch effects, on the 11 black keys left to right (PO-33 style: held, they act; locked on a step,
 * they act on that step) */
enum { PX_RPT4, PX_RPT8, PX_RPT16, PX_ROLL, PX_TRIP, PX_REV, PX_OCTDN, PX_OCTUP, PX_UNI, PX_SCRATCH, PX_STOP };
static const char *const PUNCH_SHORT[NPUNCH] = {"1/4", "1/8", "1/16", "ROLL", "6/8", "REV", "OCT-", "OCT+", "UNI",
                                                "SCRT", "STOP"};
static const char *const PUNCH_LONG[NPUNCH] = {"REPEAT 1/4", "REPEAT 1/8", "REPEAT 1/16", "ROLL 1/32",
                                               "TRIPLET 6/8", "REVERSE", "OCTAVE DOWN", "OCTAVE UP", "UNISON",
                                               "SCRATCH", "TAPE"};

/* DESTROY and BEAT (FX, third and fourth pages): K-GLITCH's two glitch banks, 16 effects each on the white
 * keys (brk_beat.c); two lines for the screen's tiles, one for the toasts and the steps */
static const char *const BEAT_NAME[32][2] = {
    {"LOOP", "DESTROY"}, {"STUTTR", "X"}, {"GLITCH", ""}, {"SCRTCH", ""}, {"REVRSE", "X"}, {"PITCH", "MAD"},
    {"GATE", "X"}, {"FILTER", "CHAOS"}, {"BEAT", "REPEAT"}, {"BEAT", "SKIP"}, {"MICRO", "STUTTR"}, {"TIME", "STRTCH"},
    {"RING", "MOD"}, {"FUZZ", ""}, {"RANDOM", "IZER"}, {"TOTAL", "CHAOS"},
    {"RND", "STUTTR"}, {"RND", "BAR"}, {"RND", "FILL"}, {"ROLL", "UP"}, {"SKIP", "8TH"}, {"STAM-", "MER"},
    {"GATE", "8"}, {"GATE", "16"}, {"GATE", "3-3-2"}, {"GATE", "RAMP"}, {"REV", "BEAT"}, {"REV", "OFF 8"},
    {"SCRTCH", "BAR"}, {"SCRTCH", "CUT"}, {"TAPE", "STOP"}, {"HALF", "TIME"}};
static const char *const BEAT_LONG[32] = {"LOOP DESTROY", "STUTTER X", "GLITCH", "SCRATCH",
    "REVERSE X", "PITCH MAD", "GATE X", "FILTER CHAOS", "BEAT REPEAT", "BEAT SKIP", "MICRO STUT", "TIME STRETCH",
    "RING MOD", "FUZZ", "RANDOMIZER", "TOTAL CHAOS", "RND STUTTER", "RND BAR", "RND FILL", "ROLL UP", "SKIP 8TH", "STAMMER",
    "GATE 8", "GATE 16", "GATE 3-3-2", "GATE RAMP", "REV BEAT", "REV OFFBEAT", "SCRATCH BAR", "SCRATCH CUT",
    "TAPE STOP", "HALF TIME"};

/* the step's condition and its ratchet's ramp (SEQ, second page) */
static const char *const COND_NAME[CO_N] = {"ALWAYS", "1:2", "2:2", "1:3", "2:3", "3:3", "1:4", "2:4", "3:4", "4:4",
                                            "1:8", "8:8", "FIRST", "!FIRST", "PRE", "!PRE"};
static const char *const RAMP_NAME[RP_N] = {"NONE", "UP", "DOWN", "OCT UP", "OCT DN", "RANDOM", "FADE", "REV ALT"};

/* the flex patterns (Flex Beat / Gross Beat style): over a bar of 16 steps, which step plays (map), how loud
 * every 1/32 is (gate, 0..15), a pitch per step, and a speed ramp.
 * map: 0..15 the step of the bar; +16 backwards; +32 twice (a roll); +64 four times; FX_HOLD: no new slice
 * (the last one rings on); FX_REST: silence */
#define FX_HOLD 0xFEu
#define FX_REST 0xFFu
#define RV 16u
#define R2 32u
#define R4 64u
enum { RAMP_NONE, RAMP_STOP, RAMP_SPIN, RAMP_BACK };
typedef struct {
    const char *name;
    uint8_t map[16];
    uint8_t gate[16];                           /* two 1/32 a byte: the high nibble first */
    int8_t pitch[16];
    uint8_t ramp;
} flex_t;
#define G_FULL {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
#define M_ID {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}
#define P_0 {0}
static const flex_t FLEX[NFLEX] = {
    {"HALF", {0, FX_HOLD, 1, FX_HOLD, 2, FX_HOLD, 3, FX_HOLD, 4, FX_HOLD, 5, FX_HOLD, 6, FX_HOLD, 7, FX_HOLD},
     G_FULL, P_0, RAMP_NONE},
    {"DOUBLE", {0, 2, 4, 6, 8, 10, 12, 14, 0, 2, 4, 6, 8, 10, 12, 14}, G_FULL, P_0, RAMP_NONE},
    {"TAPE", M_ID, G_FULL, P_0, RAMP_STOP},
    {"REWIND", {15 + RV, 14 + RV, 13 + RV, 12 + RV, 11 + RV, 10 + RV, 9 + RV, 8 + RV, 7 + RV, 6 + RV, 5 + RV, 4 + RV,
                3 + RV, 2 + RV, 1 + RV, 0 + RV},
     G_FULL, {2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5}, RAMP_NONE},
    {"BUILD", {0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 8 + R2, 8 + R2, 12 + R2, 12 + R2, 12 + R4, 12 + R4}, G_FULL,
     {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 3, 4, 5}, RAMP_NONE},
    {"GATE 8", M_ID, {0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0},
     P_0, RAMP_NONE},
    {"GATE 16", M_ID, {0xF0, 0x00, 0xF0, 0x00, 0xF0, 0x00, 0xF0, 0x00, 0xF0, 0x00, 0xF0, 0x00, 0xF0, 0x00, 0xF0, 0x00},
     P_0, RAMP_NONE},
    {"TRIPLET", M_ID, {0xFF, 0x0F, 0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F, 0xF0, 0xFF},
     P_0, RAMP_NONE},
    {"JUGGLE", {0, 1, 2, 3, 0, 1, 2, 3, 8, 9, 10, 11, 8, 9, 10, 11}, G_FULL, P_0, RAMP_NONE},
    {"ROLL", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 + R2, 12 + R2, 12 + R4, 12 + R4}, G_FULL,
     {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 4, 7}, RAMP_NONE},
    {"ECHO", {0, 1, 2, 3, 0, 1, 2, 3, 8, 9, 10, 11, 8, 9, 10, 11},
     {0xFF, 0xFF, 0xFF, 0xFF, 0x99, 0x88, 0x66, 0x44, 0xFF, 0xFF, 0xFF, 0xFF, 0x99, 0x88, 0x66, 0x44}, P_0, RAMP_NONE},
    {"SWAP", {0, 1, 3, 2, 4, 5, 7, 6, 8, 9, 11, 10, 12, 13, 15, 14}, G_FULL, P_0, RAMP_NONE},
    {"SPIN", {0, 1, 2, 3, 4, 5, 6, 7, 7 + RV, 6 + RV, 5 + RV, 4 + RV, 3 + RV, 2 + RV, 1 + RV, 0 + RV},
     {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xEE, 0xDD, 0xBB, 0x99, 0x77, 0x44, 0x22}, P_0, RAMP_BACK},
    {"DROP", {FX_REST, FX_REST, FX_REST, FX_REST, 4, 5, 6, 7, FX_REST, FX_REST, 10, 11, 12, 13, 14, 15},
     {0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, P_0, RAMP_NONE},
    {"CHOP", {0, 3, 6, 1, 4, 7, 10, 13, 8, 11, 14, 1, 12, 15, 2, 5}, G_FULL, P_0, RAMP_NONE},
    {"SLOW", M_ID, G_FULL, {0, 0, 0, 0, 0, 0, 0, 0, -1, -2, -3, -4, -6, -8, -10, -12}, RAMP_SPIN},
};
static const char *const FLEX_MODE[3] = {"LATCH", "HOLD", "1 BAR"};
static const char *const FLEX_QUANT[3] = {"STEP", "BEAT", "BAR"};

/* the generator's styles (DICE, third page) */
enum { GS_ORIG, GS_SHUFFLE, GS_JUNGLE, GS_HALF, GS_BOOM, GS_2STEP, GS_GLITCH, GS_BUILD, GS_DRILL, GS_CORE, GS_IDM,
       GS_N };
static const char *const GEN_STYLE[GS_N] = {"ORIGINAL", "SHUFFLE", "JUNGLE", "HALF", "BOOM BAP", "2-STEP",
                                            "GLITCH", "BUILD UP", "DRILL", "BREAKCORE", "IDM"};
static const char *const CLASS_NAME[4] = {"-", "K", "S", "H"};
static const char *const RATE_NAME[RATE_N] = {"1/16", "1/8", "1/32", "1/16T"};
static const char *const SLM_NAME[SLM_N] = {"AS IS", "GRID", "AUTO"};
static const char *const DLY_NAME[6] = {"1/16", "1/8", "3/16", "1/4", "3/8", "1/2"};
