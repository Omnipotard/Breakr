/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: what the audio interrupt and the main loop share.
 *
 *   breaks     up to 8 factory breaks (in the image) + 8 of the user's (the bank, in the store): 4-bit IMA ADPCM
 *              blocks read where they are (memory-mapped flash), each with its slices (start frames) and the
 *              class of every slice (kick, snare, hat, other: what the dice and the generators respect)
 *   project    16 patterns of up to 64 steps, the slices of every break as edited on the FM-1, the parameters
 *   engine     the transport, the punch effects held on the keys, the flex pattern, the events of the keys
 *
 * Rules of the chip (docs/RULES.md of Dinghy): integers only, no 64-bit division, the audio interrupt short.
 * The main loop writes, the interrupt reads: every shared field is one aligned word or a byte, and every
 * index the interrupt takes from shared data is bounded where it is used. */

#ifndef POOL
#if defined(DINGHY_HOST) || defined(FM1_WASM) || defined(BRK_TEST)
#define POOL
#else
#define POOL __attribute__((section(".pool")))
#endif
#endif

#define NFACT 8u                                /* factory breaks (gen/brk_breaks.h) */
#define NUSER 8u                                /* the user's breaks (the bank) */
#define NBRK (NFACT + NUSER)
#define NSL 32u                                 /* slices a break, at most */
#define NPAT 16u                                /* patterns */
#define NSTEP 64u                               /* steps a pattern, at most */
#define NVOICE 6u                               /* sample voices */
#define NPUNCH 11u                              /* punch effects (the black keys) */
#define NFLEX 16u                               /* flex patterns (the white keys on FLEX) */
#define PPQN 96u                                /* ticks a quarter note */
#define IMA_BLOCK 256u                          /* bytes an IMA block: 4 of header + 252 of nibbles */
#define IMA_SPB 505u                            /* samples a block: the header's sample + 504 nibbles */

enum { CL_X, CL_K, CL_S, CL_H };                /* the class of a slice: other, kick, snare, hat */

typedef struct {                                /* a break (the interrupt reads it) */
    const uint8_t *data;                        /* IMA blocks; 0: no break here */
    uint32_t frames;                            /* samples */
    uint32_t rate;                              /* Hz: 11025 .. 48000 */
    uint32_t bpm10;                             /* its own tempo, x10 (from its length and its bars) */
    uint8_t bars, nsl, flags, pad;
    uint32_t sl[NSL];                           /* slice starts, ascending, in frames */
    uint8_t cls[NSL];                           /* CL_* */
    char name[16];
} brk_t;
static brk_t brk[NBRK];
static volatile uint32_t nbrk_user;             /* user breaks present (the bank) */

/* a step: 6 bytes */
enum { SF_REV = 1u, SF_ACC = 2u };              /* flags: reverse, accent; bits 2-3 and 7 the ratchet (RAT_N); bits 4-6 chance */
typedef struct {
    uint8_t sl;                                 /* 0: rest; 1..32: slice */
    int8_t pitch;                               /* semitones -24..24 */
    uint8_t fl;                                 /* SF_*, ratchet, chance */
    uint8_t fx;                                 /* 0: none; 1..11: a punch effect; 12..43: a DESTROY / BEAT effect locked */
    int8_t nudge;                               /* early / late by nudge/256 of a step: the groove of a break in order */
    uint8_t xt;                                 /* bits 0-3: the condition (COND_*); bits 4-6: the ratchet's ramp (RAMP_*) */
} step_t;
static const uint8_t RAT_N[8] = {1, 2, 3, 4, 6, 8, 12, 16};   /* the ratchets: hits a step */
#define ST_RI(s) ((((s).fl >> 2) & 3u) | (((s).fl >> 5) & 4u))
#define ST_RAT(s) ((uint32_t)RAT_N[ST_RI(s)])
#define ST_SET_RI(st, i) ((st)->fl = (uint8_t)(((st)->fl & ~0x8Cu) | ((uint32_t)(i) & 3u) << 2 | ((uint32_t)(i) & 4u) << 5))
#define ST_CH(s) (((s).fl >> 4) & 7u)           /* 0 always, k: (8-k)/8 of the time */
#define ST_COND(s) ((s).xt & 15u)
#define ST_RAMP(s) (((s).xt >> 4) & 7u)
/* the conditions: A:B plays on the A-th of every B loops of the pattern; FIRST its first loop; PRE as the last
 * conditional step went (and !PRE the other way) */
enum { CO_ALWAYS, CO_1_2, CO_2_2, CO_1_3, CO_2_3, CO_3_3, CO_1_4, CO_2_4, CO_3_4, CO_4_4, CO_1_8, CO_8_8,
       CO_FIRST, CO_NFIRST, CO_PRE, CO_NPRE, CO_N };
/* the ratchet's ramp: the pitch up or down a semitone a hit, up or down an octave over the step, random,
 * fading, or every other hit backwards */
enum { RP_NONE, RP_UP, RP_DOWN, RP_OCTUP, RP_OCTDN, RP_RAND, RP_FADE, RP_REVALT, RP_N };
#define FX_GL 12u                               /* step fx 12..43: DESTROY 0..15, BEAT 16..31 */
typedef struct {
    step_t st[NSTEP];
    uint8_t len;                                /* 1..64 */
    uint8_t brk;                                /* the break it plays */
    uint8_t rate;                               /* RATE_*: steps of 1/16, 1/8, 1/32, 1/16 triplet */
    uint8_t autog;                              /* 1: still the break in order (follows a new slicing) */
} pat_t;
enum { RATE_16, RATE_8, RATE_32, RATE_16T, RATE_N };
static const uint8_t RATE_TICKS[RATE_N] = {24, 48, 12, 16};

/* slicing kept per break (what the SLICE and CHOP pages did): replaces the break's own slices */
typedef struct {
    uint8_t mode;                               /* SLM_ORIG: as it came; SLM_GRID: count equal; SLM_AUTO: transients */
    uint8_t count;                              /* grid: 4..32 */
    uint8_t sens;                               /* auto: 1..10 */
    uint8_t bars;                               /* 1, 2, 4, 8: gives its tempo */
    uint8_t edited, nsl, pad[2];                /* edited: the CHOP page moved a slice */
    uint32_t sl[NSL];
    uint8_t cls[NSL];
    int8_t pitch[NSL];                          /* a slice's own pitch (CHOP), semitones */
    uint8_t gain[NSL];                          /* a slice's gain, 0..15 (12 = 0 dB) */
    uint32_t rev;                               /* a slice played backwards: bit per slice */
} slicing_t;
enum { SLM_ORIG, SLM_GRID, SLM_AUTO, SLM_N };

/* the parameters (one byte each, the knobs) */
typedef struct {
    uint8_t pitch;          /* 0..48: -24..+24 semitones, 24 = none */
    uint8_t filter;         /* 0..64..127: low-pass .. off .. high-pass */
    uint8_t reso;           /* 0..15 */
    uint8_t drive;          /* 0..15 */
    uint8_t crush;          /* 0..15 */
    uint8_t dly_time;       /* 0..5: 1/16, 1/8, 3/16, 1/4, 3/8, 1/2 */
    uint8_t dly_fb;         /* 0..15 */
    uint8_t dly_mix;        /* 0..15 */
    uint8_t punch;          /* 0..15 compressor */
    uint8_t attack;         /* 0..15 */
    uint8_t decay;          /* 0..15 (15 = hold) */
    uint8_t length;         /* 1..16: sixteenths of the slice played */
    uint8_t choke;          /* 1: one voice (classic chop), 0: tails ring */
    uint8_t gate;           /* 1: a key plays while held; 0: one shot */
    uint8_t swing;          /* 50..75 */
    uint8_t tmode;          /* 0 CHOP: slices keep their pitch; 1 VARI: they follow the tempo */
    uint8_t d_chance, d_rev, d_roll, d_skip;    /* the dice, % */
    uint8_t d_pitch, d_smart, d_lock, d_seed;   /* d_lock: 0 free, 1/2/4 bars */
    uint8_t g_style, g_dens, g_var, g_roll;     /* the generator */
    uint8_t fl_mode, fl_quant, fl_depth, fl_pitch;   /* flex: latch/hold/one bar; step/beat/bar; depth %; pitch on/off */
    uint8_t bpm_hi, bpm_lo;                     /* bpm x10 */
    uint8_t metro;          /* 0 off, 1 when recording, 2 always */
    uint8_t clock;          /* 0 internal, 1 MIDI */
    uint8_t vol;            /* 0..15 */
    uint8_t bt_int[32];     /* DESTROY (0..15) and BEAT (16..31): each effect's INTENSITY, 0..100 */
    uint8_t pad[3];
} params_t;

typedef struct {
    uint32_t magic, version;
    params_t p;
    pat_t pat[NPAT];
    uint8_t chain[16], nchain, cur_pat, cur_brk, pad;
    slicing_t sc[NBRK];
    uint32_t bank_crc;                          /* the bank the user slicings were made for */
    char name[12];
} project_t;
#define PRJ_MAGIC 0x4B524252u                   /* "BRBK" */
#define PRJ_VERSION 1u
static project_t prj POOL;

/* ---- the engine's live state (the interrupt owns most of it) */
enum { EV_NONE, EV_SLICE, EV_SLICE_OFF, EV_PLAY, EV_STOP, EV_CONT, EV_PANIC, EV_CLOCK };
typedef struct { uint8_t t, a, b, c; } ev_t;
#define NEV 64u
static ev_t evq[NEV];
static volatile uint32_t ev_w, ev_r;            /* main writes ev_w, the interrupt ev_r */

static volatile uint32_t punch_keys;            /* bit k: punch effect k held (the black keys) */
static volatile uint32_t beat_keys;             /* bit k: DESTROY (0..15) / BEAT (16..31) effect k held (FX's pages 3, 4) */
static volatile uint32_t flex_sel = 0xFF;       /* the flex pattern asked for (0..15), 0xFF none */
static volatile uint32_t flex_on = 0xFF;        /* the one playing (the interrupt sets it at the launch) */
static volatile uint32_t audio_hold;            /* 1: the interrupt is silent and touches nothing (flash writes) */
static volatile uint32_t audio_blocks;          /* blocks rendered */
static volatile uint32_t sq_play;               /* transport running */
static volatile uint32_t sq_rec;                /* recording the keys into the pattern */
static volatile uint32_t sq_erase;              /* SHIFT + REC held: the steps going by are cleared */
static volatile uint32_t sq_pos = 0xFF;         /* the pattern step that played last (for the screen) */
static volatile uint32_t sq_bar_step;           /* master step since play (the screen's bar counter) */
static volatile uint32_t pat_next = 0xFF;       /* a pattern to switch to at the next bar */
static volatile uint32_t sl_sounding;           /* bit k: slice k+1 of the current break sounds (the screen, LEDs) */
static volatile uint32_t fx_active;             /* bit k: punch effect k acts now (held or locked on a step) */
static volatile uint32_t ext_clocks;            /* MIDI clocks received (main -> interrupt) */
static volatile int32_t live_level;             /* output peak, for the meter */

static void ev_push(uint32_t t, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t w = ev_w;
    if (w - ev_r >= NEV)
        return;                                 /* full: the key is dropped, never the interrupt delayed */
    evq[w % NEV].t = (uint8_t)t;
    evq[w % NEV].a = (uint8_t)a;
    evq[w % NEV].b = (uint8_t)b;
    evq[w % NEV].c = (uint8_t)c;
    RING_PUBLISH();
    ev_w = w + 1u;
}

static uint32_t bpm10(void) { uint32_t v = (uint32_t)prj.p.bpm_hi << 8 | prj.p.bpm_lo; return v < 400u ? 400u : v > 3000u ? 3000u : v; }
static void bpm10_set(uint32_t v) { if (v < 400u) v = 400u; if (v > 3000u) v = 3000u; prj.p.bpm_hi = (uint8_t)(v >> 8); prj.p.bpm_lo = (uint8_t)v; }

/* the slices the interrupt plays for break b: the project's slicing if there is one, else the break's own */
static uint32_t sl_count(uint32_t b)
{
    if (b >= NBRK || !brk[b].data) return 0;
    return prj.sc[b].nsl ? (prj.sc[b].nsl > NSL ? NSL : prj.sc[b].nsl) : brk[b].nsl;
}
static uint32_t sl_start(uint32_t b, uint32_t k)
{
    uint32_t v;
    if (b >= NBRK || k >= NSL) return 0;
    v = prj.sc[b].nsl ? prj.sc[b].sl[k] : brk[b].sl[k];
    return v < brk[b].frames ? v : 0;
}
static uint32_t sl_end(uint32_t b, uint32_t k)
{
    uint32_t n = sl_count(b), e;
    if (k + 1u >= n) return brk[b].frames;
    e = sl_start(b, k + 1u);
    return e > sl_start(b, k) ? e : brk[b].frames;
}
static uint32_t sl_class(uint32_t b, uint32_t k)
{
    if (b >= NBRK || k >= NSL) return CL_X;
    return (prj.sc[b].nsl ? prj.sc[b].cls[k] : brk[b].cls[k]) & 3u;
}

/* a hash for the dice: the same (seed, a, b) gives the same number (LOCK) */
static uint32_t hash3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

/* 2^(n/12) in Q16 for n = -24..24: the pitch of a slice */
static const uint32_t SEMI_Q16[49] = {
    16384, 17358, 18390, 19484, 20643, 21870, 23170, 24548, 26008, 27554, 29193, 30929,
    32768, 34716, 36781, 38968, 41285, 43740, 46341, 49097, 52016, 55109, 58386, 61858,
    65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715,
    131072, 138866, 147123, 155872, 165140, 174960, 185364, 196386, 208064, 220436, 233544, 247431,
    262144};
static uint32_t semi_q16(int32_t n) { if (n < -24) n = -24; if (n > 24) n = 24; return SEMI_Q16[n + 24]; }
