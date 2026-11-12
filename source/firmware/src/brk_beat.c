/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: the two glitch banks of K-GLITCH (kg_dd.c, kg_beat.c, by Omnipotard) brought over: FX's third page
 * (BEAT) and fourth (DESTROY), 16 effects each on the white keys, held. Audio interrupt, on the whole mix, before
 * the master (brk_fx.c). Their past is a ring of the dry mix (mono, 0.74 s); a held segment is kept from the
 * ring's writer. Each effect has its own INTENSITY (0..100, KNOB 1 on its page, saved with the project). They
 * follow the sequencer's sixteenths (stopped: from the press). One plays at a time: the last key held, else the
 * one a step of the pattern locks (step FX 12..43, brk_seq.c).
 * DESTROY:
 *   LOOP DESTROY  the bar cut into 1/2 .. 1/32 (INTENSITY), looped; past 60 % each pass forward or backward, past
 *                 85 % from a random quarter of the fragment
 *   STUTTER X     2x, 3x, 4x, 8x a beat (INTENSITY), each repeat fading; past 75 % random bursts
 *   GLITCH        bit crush and sample-rate reduction, buffer corruption (a chunk of the past instead of now) and,
 *                 past 60 %, digital noise bursts
 *   SCRATCH       a hand on the record: strokes of the last attack heard, each 16th, under a crossfader;
 *                 INTENSITY: baby scratches, chirps, then tears, transformers, scribbles, drags, backward cuts
 *   REVERSE X     the last beat, 1/2 or 1/4 (INTENSITY) played backwards, segment after segment; past 70 % some
 *                 segments forwards
 *   PITCH MAD     a pitch shift that moves each 16th: a staircase up (low), random jumps (middle), a dive (high)
 *   GATE X        a pattern of silences each bar, 16ths or 32nds, more holes with INTENSITY
 *   FILTER CHAOS  a resonant low-pass jumping to a random cutoff each 16th, resonance and range with INTENSITY
 *   BEAT REPEAT   the last beat, 1/2 or 1/4 of the grid repeated in time with it, fading a little; past 70 % each
 *                 repeat half as long (a roll)
 *   BEAT SKIP     16ths dropped, or past 50 % replaced by the one before
 *   MICRO STUTTER 50 .. 10 ms grains of the last instant, a new one each 16th
 *   TIME STRETCH  the groove slowed (3/4 .. 1/4 speed) without changing its pitch (two overlapping grains)
 *   RING MOD      a sine carrier, 30 Hz .. 2 kHz
 *   FUZZ          drive (x1 .. x15) into a soft then hard clip, its level kept
 *   RANDOMIZER    one of the 14 above, a new one each 16th
 *   TOTAL CHAOS   the same, crushed, with a ring modulator on some 16ths
 * BEAT: programs over the bar; on each of its 16 sixteenths an operation on the ring (dry, repeated, the one
 * before, backwards, muted, half speed, pitched, scratched), chosen anew each bar from the bar's seed:
 *   RND STUTTER, RND BAR, RND FILL, ROLL UP, SKIP 8TH, STAMMER, GATE 8 / 16 / 3-3-2 / RAMP, REV BEAT,
 *   REV OFFBEAT, SCRATCH BAR, SCRATCH CUT, TAPE STOP, HALF TIME (details at beat_op).
 * The scratches move a "record" the way a hand does under a crossfader, read with a cubic interpolation; the sound
 * follows the record's speed as a cartridge does. Every switch fades over 64 samples. Q15 throughout. */

enum { DD_LOOP, DD_STUT, DD_GLITCH, DD_SCRATCH, DD_REV, DD_PITCH, DD_GATE, DD_FILTER,
       DD_BREP, DD_BSKIP, DD_MICRO, DD_STRETCH, DD_RING, DD_FUZZ, DD_RAND, DD_TOTAL,
       DD_RSTUT, DD_RBAR, DD_RFILL, DD_ROLL, DD_SKIP8, DD_STAMR, DD_GATE8, DD_GATE16, DD_GATE332, DD_GRAMP,
       DD_REVB, DD_REVOFF, DD_SCRBAR, DD_SCRCUT, DD_TSTOP, DD_HALFT, DD_N };
#define DD_W 2048u                               /* PITCH and STRETCH: the grain window (46 ms) */

#define BT_RN 32768u                             /* the ring: power of two, 0.74 s */
static int16_t bt_ring[BT_RN] POOL;
static uint32_t bt_w;                            /* the ring's writer (absolute index) */
static uint32_t fx_tick0, fx_ph0, fx_spt = 1u << 24;   /* the sequencer's clock at the block's start (brk_seq.c) */
static volatile int8_t bt_seq = -1;              /* the effect the playing step locks (brk_seq.c), -1 none */

enum { BO_DRY, BO_MUTE, BO_REP, BO_PREV, BO_REV, BO_HALF, BO_STOP, BO_SCR, BO_PREP, BO_RREV };
#define BO(op, arg) ((uint32_t)(op) | (uint32_t)(arg) << 4)
#define B_CONT 0x100u                            /* the operation goes on from the slot before */
#define B_CAP 0x200u                             /* the record is this slot's sound */
#define KB_OWN 10000u                            /* a record held from the ring's writer: at most */
static const uint8_t KB_DIV[10] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32};   /* REP: the repeat's length, a beat / DIV */
enum { KD1, KD2, KD3, KD4, KD6, KD8, KD12, KD16, KD24, KD32 };
/* the scratches: four strokes a 16th, each to a place on the record (eighths of the stroke's depth) with the
 * crossfader open (1) or shut (0) */
enum { SP_BABY, SP_CHIRP, SP_TEAR, SP_TRANS, SP_SCRIB, SP_DRAG, SP_BACK, SP_N };
#define SQ(t, o) ((uint8_t)((t) << 1 | (o)))
static const uint8_t KB_PAT[SP_N][4] = {
    {SQ(6, 1), SQ(0, 1), SQ(6, 1), SQ(0, 1)},    /* BABY: forward and back, open */
    {SQ(8, 1), SQ(0, 0), SQ(8, 1), SQ(0, 0)},    /* CHIRP: forward open, back shut */
    {SQ(4, 1), SQ(4, 1), SQ(8, 1), SQ(0, 1)},    /* TEAR: the forward stroke in two, a stop between */
    {SQ(4, 1), SQ(8, 0), SQ(4, 1), SQ(0, 0)},    /* TRANSFORMER: the fader cutting the strokes */
    {SQ(2, 1), SQ(0, 1), SQ(2, 1), SQ(0, 1)},    /* SCRIBBLE: tiny fast strokes */
    {SQ(2, 1), SQ(4, 1), SQ(6, 1), SQ(8, 1)},    /* DRAG: slowly forward */
    {SQ(8, 0), SQ(0, 1), SQ(8, 0), SQ(0, 1)}};   /* BACKWARD: forward shut, back open */

static struct {
    int8_t cur, sub;                             /* the effect (DD_*, -1 none); what plays this 16th (RANDOMIZER, TOTAL) */
    int32_t g;                                   /* its wet gain, 0..32767 */
    int32_t xf;                                  /* a sub-effect's fade in, 0..64 */
    uint32_t t;                                  /* samples since it started */
    uint32_t s16;                                /* the 16th last seen */
    uint32_t S, L, L0, k;                        /* a segment of the ring: start, length (L0: as captured), position */
    uint8_t rev, own, mode;                      /* this pass backwards; held from the writer; SKIP / TOTAL: this 16th's way */
    uint32_t off;                                /* LOOP: offset into the fragment */
    int32_t rep;                                 /* BREP: this repeat's level, Q15 */
    uint32_t ri;                                 /* a read position (ring index), its fraction (Q16), the speed (Q16) */
    int32_t rf, sp;
    uint32_t ph, dph, ph2;                       /* RING's carrier; TOTAL's ring */
    uint32_t pp;                                 /* PITCH: the delay phase, Q16 over DD_W; STRETCH: the grains' phase */
    int32_t ratio;                               /* PITCH: Q16; STRETCH: the speed, Q16 */
    uint32_t ga, gb;                             /* STRETCH: the two grains' anchors */
    int8_t semi;                                 /* PITCH: the step it is on */
    uint32_t mask, i32;                          /* GATE: the bar's pattern (bit per 16th or 32nd), the slot last seen */
    int32_t gg;                                  /* GATE / SKIP / BEAT: the gain, Q15 */
    svf_t flt;                                   /* FILTER: the state-variable filter (brk_fx.c) */
    int32_t cut, cut_to, fq;                     /* .. its cutoff (note x256, gliding), its damping (Q24) */
    int32_t hl, hr;                              /* GLITCH: the held sample */
    uint32_t hn, corrupt, csrc, noise;           /* GLITCH: hold count, corrupted samples left, their source, noise left */
    uint32_t rnd;
    uint32_t bseed, rec, slen, sk, D, u24, du;   /* BEAT: the bar's seed, the record, the 16th's length, the stroke */
    int32_t acc, lpy, ps, dl, dr, ll, lr;        /* STOP's slowdown; the cartridge's low-pass (Q19); the record's last
                                                  * place (Q15); the jump's declick and the last output */
    uint8_t op, arg, open, newop, qi, from8, to8;
    uint32_t fmul;                               /* STUT: 13000 / L, Q8 (once a block: no divide a sample) */
    int32_t fz;                                  /* FUZZ: its makeup, Q8 (once a block) */
    uint8_t jump;                                /* a segment replaced mid-way: the next sample declicked */
    int32_t jl, jr, jll, jlr;                    /* .. the offsets left, the last output */
} kd = {.cur = -1, .sub = -1, .rnd = 0x51A3E1B7u};

static int32_t bclamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static uint32_t kg_rand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}
static uint32_t kb_hash(uint32_t a)
{
    a ^= a >> 16;
    a *= 0x7FEB352Du;
    a ^= a >> 15;
    a *= 0x846CA68Bu;
    return a ^ (a >> 16);
}
static int32_t kb_abs(int32_t v) { return v < 0 ? -v : v; }
static int dd_isop(int32_t e) { return e == DD_SCRATCH || (e >= DD_RSTUT && e < DD_N); }
static int dd_isgate(int32_t e) { return e >= DD_GATE8 && e <= DD_GRAMP; }

static int32_t ring_raw(uint32_t i) { return bt_ring[i & (BT_RN - 1u)]; }
static uint32_t dd_int(void) { uint32_t v = prj.p.bt_int[(uint32_t)(kd.cur >= 0 ? kd.cur : 0) % DD_N]; return v > 100u ? 100u : v; }
static uint32_t dd_beat(void) { return (fx_spt >> 4) * PPQN >> 12; }   /* frames a beat */
static uint32_t dd_barpos(void)                  /* frames into the bar (the sequencer's when it plays) */
{
    return sq_play ? ((fx_tick0 % (4u * PPQN)) * (fx_spt >> 4) >> 12) + (fx_ph0 >> 16) : kd.t;
}
static uint32_t dd_cap(uint32_t L) { return L < 64u ? 64u : L > BT_RN / 2u - 1024u ? BT_RN / 2u - 1024u : L; }
static int32_t dd_rd(uint32_t i, int32_t f)      /* the ring at i + f / 65536, linear */
{
    int32_t a = ring_raw(i), b = ring_raw(i + 1u);
    return a + (((b - a) * (f >> 1)) >> 15);
}
static int32_t dd_declick(int32_t y, uint32_t k, uint32_t L)
{
    return k < 64u ? (y * (int32_t)k) >> 6 : L - k < 64u ? (y * (int32_t)(L - k)) >> 6 : y;
}
/* the writer leaves a held segment alone once it has been written */
static int bt_owns(uint32_t w)
{
    uint32_t d = w - kd.S;
    return kd.cur >= 0 && kd.own && d >= kd.L0 && (d & (BT_RN - 1u)) < kd.L0;
}
/* 2^(s/12), Q16, s -24..24 */
static int32_t dd_semi(int32_t s)
{
    static const uint32_t R[12] = {65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715};
    int32_t o = 0;
    while (s < 0) s += 12, o--;
    while (s >= 12) s -= 12, o++;
    return o >= 0 ? (int32_t)(R[s] << o) : (int32_t)(R[s] >> -o);
}
/* the ring at i + f / 65536, cubic (Hermite): a moving record without the linear's grit */
static int32_t dd_cub(uint32_t i, int32_t f)
{
    int32_t xm = ring_raw(i - 1u), x0 = ring_raw(i), x1 = ring_raw(i + 1u), x2 = ring_raw(i + 2u), t = (f >> 4) & 4095;
    int32_t c1 = (xm * -1 + x1) / 2, c2 = xm - (5 * x0) / 2 + 2 * x1 - x2 / 2, c3 = (x2 - xm) / 2 + (3 * (x0 - x1)) / 2;
    int32_t y = ((((((c3 * t) >> 12) + c2) * t) >> 12) + c1) * t;
    return bclamp((y >> 12) + x0, -40000, 40000);
}
/* the record's sound at speed v (Q15, 32768 = normal, either way): a low-pass and a level that follow the speed */
static int32_t kb_cart(int32_t m, int32_t v)
{
    int32_t a, g;
    if (v < 0) v = -v;
    a = bclamp(v * 5 / 8, 700, 30000);
    kd.lpy += (((m * 16 - kd.lpy) >> 4) * (a >> 3)) >> 8;   /* (lpy: Q19) */
    g = v > 36000 ? 36000 : v;
    return ((kd.lpy >> 8) * (g >> 3)) >> 8;
}
/* where the hand has the record (Q15 frames from the cue): from8 to to8 (eighths of the stroke's depth D), sm of
 * the way (Q15). Exact: a stroke ends where the next one starts (no burst of speed between two) */
static int32_t kb_place(uint32_t from8, uint32_t to8, uint32_t sm)
{
    return (int32_t)(((int64_t)from8 * kd.D * 32768 + (int64_t)((int32_t)to8 - (int32_t)from8) * kd.D * (int32_t)sm) >> 3);
}
/* the last attack in the ring's last beat: where a DJ would put the needle */
static uint32_t kb_cue(uint32_t base)
{
    uint32_t span = dd_beat(), p, best, e[4] = {0, 0, 0, 0}, j = 0;
    int32_t top = 0;
    if (span > BT_RN / 2u) span = BT_RN / 2u;
    best = base - span / 2u;
    for (p = base - span; (int32_t)(base - 512u - p) > 0; p += 64u, j++) {
        uint32_t now = (uint32_t)(kb_abs(ring_raw(p)) + kb_abs(ring_raw(p + 16u)) + kb_abs(ring_raw(p + 32u)) + kb_abs(ring_raw(p + 48u)));
        int32_t rise = (int32_t)now - (int32_t)((e[0] + e[1] + e[2] + e[3]) / 4u);
        if (j >= 4u && rise > top) top = rise, best = p - 64u;
        e[j & 3u] = now;
    }
    return best;
}

/* the operation of the effect e on the bar's 16th s */
static uint32_t beat_op(int32_t e, uint32_t s, uint32_t I)
{
    uint32_t h = kb_hash(kd.bseed ^ (s * 0x9E3779B9u)), r = h % 100u, b = s >> 2, q = s & 3u;
    switch (e) {
    case DD_SCRATCH: {                           /* (DESTROY) */
        static const uint8_t SM[4] = {SP_CHIRP, SP_TEAR, SP_TRANS, SP_BACK};
        uint32_t p = I < 25u ? SP_BABY : I < 50u ? (s & 1u ? SP_BABY : SP_CHIRP) : I < 75u ? SM[(h >> 8) % 4u] : (h >> 8) % SP_N;
        return BO(BO_SCR, p);
    }
    case DD_RSTUT: {                             /* decided per 8th */
        static const uint8_t ST[6] = {KD32, KD16, KD16, KD12, KD24, KD8};
        uint32_t hp = kb_hash(kd.bseed ^ ((s >> 1) * 0x85EBCA6Bu));
        if (s < 2u || hp % 100u >= 15u + I * 6u / 10u) return BO(BO_DRY, 0);
        return BO(BO_REP, ST[(hp >> 8) % 6u]) | (s & 1u ? B_CONT : 0u);
    }
    case DD_RBAR: {
        uint32_t dry = 70u - I * 6u / 10u;
        if (r < dry || (s == 0u && I < 50u)) return BO(BO_DRY, 0);
        switch ((h >> 8) % 8u) {
        case 0: return BO(BO_REP, KD32);
        case 1: return BO(BO_REP, KD16);
        case 2: return BO(BO_PREV, 1);
        case 3: return BO(BO_REV, 1);
        case 4: return BO(BO_MUTE, 0);
        case 5: return BO(BO_HALF, 1);
        case 6: return BO(BO_REP, KD24);
        default: return BO(BO_PREP, (h >> 12) % 2u ? 7 : 12);
        }
    }
    case DD_RFILL:
        if (s < (I < 50u ? 12u : 8u)) return BO(BO_DRY, 0);
        if (s == 15u) return (h >> 8) % 2u ? BO(BO_REP, KD32) : BO(BO_MUTE, 0);
        switch ((h >> 8) % 5u) {
        case 0: return BO(BO_REP, KD16);
        case 1: return BO(BO_REP, KD8);
        case 2: return BO(BO_REV, 1);
        case 3: return BO(BO_PREV, 1);
        default: return BO(BO_PREP, 5u + (h >> 12) % 8u);
        }
    case DD_ROLL:                                /* the downbeat: a quarter, 8ths, 16ths, 32nds */
        if (b == 0u) return BO(BO_DRY, 0) | (s == 0u ? B_CAP : 0u);
        return BO(BO_PREP, b) | (q ? B_CONT : 0u);
    case DD_SKIP8:
        if (q < 2u) return BO(BO_DRY, 0);
        return I < 50u ? BO(BO_MUTE, 0) : BO(BO_PREV, 2);
    case DD_STAMR: {                             /* hits on 0, 3, 6 of each half bar */
        static const uint8_t BACK[8] = {0, 1, 2, 0, 1, 2, 0, 1};
        uint32_t k = s & 7u;
        if (!BACK[k] || (I < 40u && k != 1u && k != 4u && k != 7u)) return BO(BO_DRY, 0);
        return I > 80u ? BO(BO_REP, KD32) : BO(BO_PREV, BACK[k]);
    }
    case DD_REVB:
        if (I < 50u && !(b & 1u)) return BO(BO_DRY, 0);
        return BO(BO_REV, 4) | (q ? B_CONT : 0u);
    case DD_REVOFF:
        if (I >= 60u && b == 3u) return BO(BO_REV, 4) | (q ? B_CONT : 0u);
        if (q < 2u) return BO(BO_DRY, 0);
        return BO(BO_REV, 2) | (q == 3u ? B_CONT : 0u);
    case DD_SCRBAR: {                            /* silence, three short scratches, the downbeat backwards */
        uint32_t mute = I > 66u ? 2u : 4u, n = I > 66u ? 5u : 3u, k;
        static const uint8_t SB[5] = {SP_CHIRP, SP_TEAR, SP_TRANS, SP_BABY, SP_BACK};
        if (s < mute) return BO(BO_MUTE, 0) | (s == 0u ? B_CAP : 0u);
        if (s >= 12u) return BO(BO_RREV, 0) | (s > 12u ? B_CONT : 0u);
        k = (s - mute) / 2u;
        if ((s - mute) & 1u || k >= n) return BO(BO_MUTE, 0);
        return BO(BO_SCR, I < 34u ? SP_CHIRP : SB[k % 5u]);
    }
    case DD_SCRCUT:
        if (!(b & 1u)) return BO(BO_DRY, 0) | (q == 0u ? B_CAP : 0u);
        return BO(BO_SCR, I < 50u ? (q & 1u ? SP_BACK : SP_TRANS) : (h >> 8) % 2u ? SP_SCRIB : SP_TRANS);
    case DD_TSTOP: {
        uint32_t st = I < 50u ? 12u : 8u;
        if (s < st) return BO(BO_DRY, 0);
        return BO(BO_STOP, 16u - st) | (s > st ? B_CONT : 0u);
    }
    case DD_HALFT:
        if (I < 50u) return BO(BO_HALF, 4) | (q ? B_CONT : 0u);
        return BO(BO_HALF, 2) | (q & 1u ? B_CONT : 0u);
    default:                                     /* the gates: the sound as it is, gated (beat_gate) */
        return BO(BO_DRY, 0);
    }
}

/* the gates, from the place in the bar: open or shut */
static uint32_t beat_gate(int32_t e, uint32_t p, uint32_t I)
{
    uint32_t beat = dd_beat() + 1u, duty = 25u + I * 65u / 100u, cyc, ph;
    switch (e) {
    case DD_GATE8:
        cyc = beat / 2u;
        break;
    case DD_GATE16:
        cyc = beat / 4u;
        if (I > 70u && kb_hash(kd.bseed ^ (p / (cyc + 1u)) * 0x27D4EB2Fu) % 100u < I - 70u) return 0;
        break;
    case DD_GATE332: {                           /* 3 + 3 + 2 sixteenths each half bar */
        static const uint8_t ST[8] = {0, 0, 0, 3, 3, 3, 6, 6}, LN[8] = {3, 3, 3, 3, 3, 3, 2, 2};
        uint32_t sl = beat / 4u + 1u, hb = p % (beat * 2u), k = hb / sl;
        if (k > 7u) k = 7u;
        return hb - ST[k] * sl < LN[k] * sl * duty / 100u;
    }
    default:                                     /* GATE RAMP: quarters, 8ths, 16ths, 32nds */
        cyc = beat >> ((p / beat) & 3u);
        duty = 30u + I * 40u / 100u;
        break;
    }
    if (!cyc) cyc = 1;
    ph = p % cyc;
    return ph < cyc * duty / 100u;
}

/* a new 16th of the bar (s), the ring's index base: the operation set up */
static void beat_slot(uint32_t s, uint32_t base)
{
    uint32_t I = dd_int(), code, op, arg, beat = dd_beat(), slen = beat / 4u;
    if (s == 0u) kd.bseed = kg_rand(&kd.rnd);   /* a new bar: new random ones */
    code = beat_op(kd.sub, s, I);
    op = code & 15u;
    arg = code >> 4 & 15u;
    kd.slen = slen;
    kd.sk = 0;
    if (code & B_CAP) {                          /* the record: this 16th on (held from the writer) */
        kd.rec = base;
        kd.S = base;
        kd.L0 = kd.sub == DD_ROLL ? (beat / 2u < KB_OWN ? beat / 2u : KB_OWN) : (beat < KB_OWN ? beat : KB_OWN);
        kd.own = 1;
    }
    if ((code & B_CONT) && op == kd.op) return;
    kd.op = (uint8_t)op;
    kd.arg = (uint8_t)arg;
    kd.newop = 1;                                /* (the jump declicked) */
    kd.open = op != BO_MUTE;
    kd.k = 0;
    switch (op) {
    case BO_REP:
        kd.S = base;                             /* (the first pass is the live sound) */
        kd.L = dd_cap(beat / KB_DIV[arg % 10u]);
        if (kd.sub != DD_ROLL && kd.sub != DD_SCRBAR) kd.own = 0;
        break;
    case BO_PREV:
        kd.S = base - arg * slen;
        kd.L = dd_cap(slen);
        break;
    case BO_REV:
        kd.L = dd_cap(arg * slen);
        kd.S = base - kd.L;
        break;
    case BO_RREV:
        kd.S = kd.rec;
        kd.L = kd.L0 > 64u ? kd.L0 : 64u;
        break;
    case BO_HALF:
        kd.ri = base, kd.rf = 0, kd.sp = 32768;
        break;
    case BO_STOP:                                /* to a standstill over 3/4 of the span */
        kd.ri = base, kd.rf = 0, kd.sp = 65536;
        kd.acc = (int32_t)(65536u / (arg * slen * 3u / 4u + 1u)) + 1;
        kd.lpy = 0;
        break;
    case BO_PREP: {                              /* a repeat of the record (ROLL) or of the 16th before, pitched */
        int32_t semi = kd.sub == DD_ROLL ? (I > 50u ? (int32_t)(arg * (I > 80u ? 4u : 2u)) : 0) : (int32_t)arg;
        if (kd.sub == DD_ROLL) {
            kd.S = kd.rec;
            kd.L = dd_cap(beat >> arg);
            if (kd.L > kd.L0 && kd.own) kd.L = kd.L0;
        } else {
            kd.S = base - slen;
            kd.L = dd_cap(slen);
        }
        kd.ri = kd.S, kd.rf = 0;
        kd.sp = dd_semi(semi);
        break;
    }
    case BO_SCR:
        kd.D = slen / 2u;
        kd.qi = 0;
        kd.u24 = 0;
        kd.du = (1u << 24) / (slen / 4u + 1u);
        kd.from8 = kd.to8;                       /* (from where the last stroke left the record) */
        kd.to8 = (uint8_t)(KB_PAT[arg % SP_N][0] >> 1);
        kd.open = KB_PAT[arg % SP_N][0] & 1u;
        kd.ps = kb_place(kd.from8, kd.from8, 0);   /* (the hand starts still: no burst of speed) */
        break;
    default:
        break;
    }
}

/* the effect's sample: the dry x, y; idx the ring index of this sample */
static void beat_sample(int32_t x, int32_t y, uint32_t idx, int32_t *wl, int32_t *wr)
{
    int32_t m = 0;
    kd.sk++;
    switch (kd.op) {
    case BO_DRY:
        *wl = x, *wr = y;
        goto gate;
    case BO_MUTE:
        break;
    case BO_REP:
    case BO_PREV:
        m = dd_declick(ring_raw(kd.S + kd.k), kd.k, kd.L);
        if (++kd.k >= kd.L) kd.k = 0;
        break;
    case BO_REV:
    case BO_RREV:
        m = dd_declick(ring_raw(kd.S + kd.L - 1u - kd.k), kd.k, kd.L);
        if (++kd.k >= kd.L) {
            kd.k = 0;
            if (kd.op == BO_REV) kd.S = idx + 1u - kd.L;   /* (on: the next stretch, just played, backwards) */
        }
        break;
    case BO_HALF: {
        int32_t f = kd.rf + kd.sp;
        m = dd_cub(kd.ri, kd.rf);
        kd.ri += (uint32_t)(f >> 16), kd.rf = f & 0xFFFF;
        if (idx - kd.ri > BT_RN - 4096u) kd.ri = idx - kd.slen;
        break;
    }
    case BO_PREP: {
        int32_t f = kd.rf + kd.sp;
        uint32_t o = kd.ri - kd.S;
        m = dd_declick(dd_rd(kd.ri, kd.rf), o < kd.L ? o : 0u, kd.L);
        kd.ri += (uint32_t)(f >> 16), kd.rf = f & 0xFFFF;
        if (kd.ri - kd.S >= kd.L) kd.ri -= kd.L;
        if ((int32_t)(idx - kd.ri) < 4) kd.ri = kd.S;   /* (never ahead of the writer) */
        break;
    }
    case BO_STOP: {
        int32_t f = kd.rf + kd.sp;
        m = kb_cart(dd_cub(kd.ri, kd.rf), kd.sp >> 1);
        kd.ri += (uint32_t)(f >> 16), kd.rf = f & 0xFFFF;
        kd.sp = kd.sp > kd.acc ? kd.sp - kd.acc : 0;
        break;
    }
    case BO_SCR: {                               /* the hand: a smooth stroke to the next place each quarter */
        uint32_t u = kd.u24 >> 9, sm = (((u * u) >> 15) * ((3u << 15) - 2u * u)) >> 15;   /* smoothstep, Q15 */
        int32_t off, v;
        uint32_t ri;
        off = kb_place(kd.from8, kd.to8, sm);
        v = off - kd.ps;
        kd.ps = off;
        ri = kd.rec + (uint32_t)(off >> 15);
        if ((int32_t)(idx - ri) < 8) ri = idx - 8u;   /* (the record ends at now) */
        m = kb_cart(dd_cub(ri, (off & 0x7FFF) << 1), v);
        kd.u24 += kd.du;
        if (kd.u24 >= 1u << 24) {                /* the next stroke */
            uint8_t p;
            kd.u24 = 0;
            kd.from8 = kd.to8;
            if (kd.qi < 3u) kd.qi++;
            p = KB_PAT[kd.arg % SP_N][kd.qi];
            kd.to8 = (uint8_t)(p >> 1);
            kd.open = p & 1u;
        }
        break;
    }
    default:
        break;
    }
    *wl = *wr = m;
gate:
    *wl = (*wl * (kd.gg >> 3)) >> 12;
    *wr = (*wr * (kd.gg >> 3)) >> 12;
    if (kd.newop) {                              /* a new operation: from where the last one was, quickly */
        kd.newop = 0;
        kd.dl = kd.ll - *wl, kd.dr = kd.lr - *wr;
    }
    *wl += kd.dl, *wr += kd.dr;
    kd.dl -= kd.dl >> 5, kd.dr -= kd.dr >> 5;
    kd.ll = *wl, kd.lr = *wr;
}

/* a sine, Q15, of a 32-bit phase (two parabolas) */
static int32_t sine_q(uint32_t ph)
{
    int32_t x = ((int32_t)(ph >> 17) - 16384) * 2, s;   /* -1..1, Q15 */
    s = x * 4 - ((x * (x < 0 ? -x : x)) >> 13);
    return s > 32767 ? -32767 : s < -32767 ? 32767 : -s;   /* (the crests clamped on the same side: no flip) */
}

/* the BEAT program (or SCRATCH) e taken over at ring index base: its record (the last attack: kb_cue) and seed */
static void beat_engage(int32_t e, uint32_t base)
{
    uint32_t beat = dd_beat(), L;
    kd.bseed = kg_rand(&kd.rnd);
    kd.op = BO_DRY;
    kd.open = 1;
    kd.newop = 0;
    kd.dl = kd.dr = 0;
    kd.ll = kd.lr = 0;
    kd.lpy = 0;
    kd.ps = 0;
    kd.from8 = kd.to8 = 0;
    kd.rec = kb_cue(base);
    L = e == DD_SCRATCH ? beat / 4u : beat;      /* (SCRATCH: the stroke's depth and some) */
    kd.S = kd.rec;
    kd.L0 = L < KB_OWN ? L : KB_OWN;
    kd.own = e == DD_SCRATCH || e == DD_SCRBAR || e == DD_SCRCUT || e == DD_ROLL;
}

/* the effect e takes over from the sample at ring index base (the 16th's part in dd_tick) */
static void dd_engage(int32_t e, uint32_t base)
{
    uint32_t beat = dd_beat(), I = dd_int(), pos = dd_barpos(), L;
    kd.sub = (int8_t)e;
    kd.xf = 0;
    kd.own = 0;
    kd.rev = 0;
    kd.off = 0;
    kd.rep = 32767;
    kd.gg = 32767;
    switch (e) {
    case DD_LOOP: {
        static const uint8_t DIV[5] = {2, 4, 8, 16, 32};   /* of a bar */
        L = dd_cap(beat * 4u / DIV[I * 5u / 101u]);
        kd.k = pos % L;
        kd.S = base - kd.k;                      /* the last division: the first pass is the live sound */
        kd.L = kd.L0 = L;
        kd.own = 1;
        break;
    }
    case DD_STUT: {
        static const uint8_t DIV[4] = {2, 3, 4, 8};   /* repeats a beat */
        L = dd_cap(beat / DIV[I * 4u / 101u]);
        kd.k = pos % L;
        kd.S = base - kd.k;
        kd.L = kd.L0 = L;
        kd.own = 1;
        break;
    }
    case DD_BREP:
        L = dd_cap(beat >> (I * 3u / 101u));
        kd.k = pos % L;
        kd.S = base - kd.k - L;                  /* what just played, again: the last whole segment of the grid */
        kd.L = kd.L0 = L;
        kd.own = 1;
        break;
    case DD_REV:
        L = dd_cap(beat >> (I * 3u / 101u));
        kd.k = pos % L;
        kd.S = base - kd.k - L;                  /* the segment before, backwards */
        kd.L = kd.L0 = L;
        kd.rev = 1;
        break;
    case DD_PITCH:
        kd.pp = 0;
        kd.semi = 0;
        kd.ratio = 65536;
        break;
    case DD_STRETCH:
        kd.ri = base - DD_W;
        kd.rf = 0;
        kd.pp = 0;
        kd.ga = kd.ri - DD_W / 2u;
        kd.gb = kd.ri - DD_W / 2u;
        kd.ratio = I < 50u ? 49152 - (int32_t)I * 16384 / 50 : 32768 - ((int32_t)I - 50) * 16384 / 50;   /* 3/4 .. 1/4 */
        break;
    case DD_GATE:
        kd.i32 = 0xFFFFFFFFu;
        break;
    case DD_FILTER:
        svf_clear(&kd.flt);
        kd.cut = kd.cut_to = 100 << 8;
        kd.fq = (30000 - (int32_t)I * 230) << 10;   /* Q24 damping: 1.8 .. 0.43 */
        break;
    case DD_RING:
        kd.ph = 0;
        kd.dph = (uint32_t)(30u << (I * 6u / 100u)) * (0xFFFFFFFFu / FS);   /* 30 Hz .. ~2 kHz */
        break;
    case DD_GLITCH:
        kd.hn = 0;
        kd.corrupt = kd.noise = 0;
        break;
    default:
        if (dd_isop(e)) beat_engage(e, base);
        break;
    }
}

/* each 16th: what changes on the grid (s16: the sequencer's 16th, or since the effect started) */
static void dd_tick(uint32_t base, uint32_t s16)
{
    uint32_t I = dd_int(), r = kg_rand(&kd.rnd), step = dd_beat() / 4u;
    kd.jump = 1;
    if (kd.cur == DD_RAND || kd.cur == DD_TOTAL) {   /* a new effect each 16th */
        dd_engage((int32_t)(r % 14u), base);     /* (any of the first 14) */
        kd.mode = (uint8_t)(kd.cur == DD_TOTAL && (r >> 8) % 3u == 0u);   /* TOTAL: a ring modulator on top */
        r = kg_rand(&kd.rnd);
    }
    switch (kd.sub) {
    case DD_BSKIP:                               /* drop it, repeat the one before, or play it */
        kd.mode = (uint8_t)(r % 100u < I * 6u / 10u ? (I > 50u && (r >> 8) % 2u ? 2u : 1u) : 0u);
        if (kd.mode == 2u) {
            kd.S = base - step;
            kd.k = 0;
            kd.L = kd.L0 = dd_cap(step);
        }
        break;
    case DD_FILTER:
        kd.cut_to = (int32_t)(36u + r % (40u + I * 50u / 100u)) << 8;
        break;
    case DD_PITCH:
        if (I < 34u) {                           /* a staircase up an octave */
            static const int8_t ST[8] = {0, 2, 4, 5, 7, 9, 11, 12};
            kd.semi = ST[(kd.t / (step + 1u)) % 8u];
        } else if (I < 67u) {                    /* random jumps */
            static const int8_t J[7] = {-12, -7, -5, 0, 5, 7, 12};
            kd.semi = J[r % 7u];
        } else {                                 /* a dive: an octave up down to two down, again */
            kd.semi = (int8_t)(kd.semi <= -24 ? 12 : kd.semi - 3);
        }
        kd.ratio = dd_semi(kd.semi);
        break;
    case DD_MICRO: {
        uint32_t G = 2205u - I * 1764u / 100u;   /* 50 .. 10 ms */
        kd.S = base - G;
        kd.k = 0;
        kd.L = kd.L0 = G;
        kd.own = 1;
        break;
    }
    case DD_GLITCH:
        if (r % 100u < 20u + I / 2u) {           /* a chunk of the past instead of now */
            kd.corrupt = 256u + (r >> 8) % (256u + I * 16u);
            kd.csrc = base - 2048u - (r >> 16) % (BT_RN / 2u);
        }
        if (I > 60u && (r >> 4) % 100u < I - 60u) kd.noise = 64u + (r >> 12) % 256u;
        break;
    default:
        break;
    }
    if (dd_isop(kd.sub)) beat_slot(s16 % 16u, base);   /* BEAT, SCRATCH: the 16th's operation */
}

/* the wet sample of the effect playing, for the dry x, y; idx the ring index of this sample */
static void dd_sample(int32_t x, int32_t y, uint32_t idx, int32_t *wl, int32_t *wr)
{
    uint32_t I = dd_int();
    int32_t m;
    switch (kd.sub) {
    case DD_LOOP: {
        uint32_t p = kd.off + kd.k;
        if (p >= kd.L) p -= kd.L;
        m = dd_declick(ring_raw(kd.S + (kd.rev ? kd.L - 1u - p : p)), kd.k, kd.L);
        if (kd.off) {                            /* (where the fragment wraps inside the pass: faded there too) */
            uint32_t wp = kd.L - kd.off, dist = kd.k >= wp ? kd.k - wp : wp - kd.k;
            if (dist < 64u) m = (m * (int32_t)dist) >> 6;
        }
        if (++kd.k >= kd.L) {
            uint32_t r = kg_rand(&kd.rnd);
            kd.k = 0;
            kd.rev = (uint8_t)(I > 60u && r % 2u);
            kd.off = I > 85u ? ((r >> 8) % 4u) * (kd.L / 4u) : 0u;
        }
        *wl = *wr = m;
        return;
    }
    case DD_STUT:
    case DD_BREP:
    case DD_MICRO:
        m = dd_declick(ring_raw(kd.S + kd.k), kd.k, kd.L);
        if (kd.sub == DD_STUT)                   /* each repeat fades a little */
            m = (m * ((65536 - (int32_t)((kd.k * kd.fmul) >> 8)) >> 4)) >> 12;
        else if (kd.sub == DD_BREP)
            m = (m * kd.rep) >> 15;
        if (++kd.k >= kd.L) {
            kd.k = 0;
            if (kd.sub == DD_STUT && I > 75u) {  /* bursts */
                uint32_t r = kg_rand(&kd.rnd) % 3u;
                kd.L = dd_cap(kd.L0 >> r);
            } else if (kd.sub == DD_BREP) {
                kd.rep = kd.rep * (int32_t)(1000u - I) / 1000;
                if (kd.rep < 24576) kd.rep = 24576;   /* (never below 3/4: a repeat stays a beat) */
                if (I > 70u && kd.L > kd.L0 / 8u) kd.L = dd_cap(kd.L / 2u);
            }
        }
        *wl = *wr = m;
        return;
    case DD_REV:
        m = dd_declick(ring_raw(kd.S + (kd.rev ? kd.L - 1u - kd.k : kd.k)), kd.k, kd.L);
        if (++kd.k >= kd.L) {
            kd.k = 0;
            kd.S += kd.L;                        /* the next segment (already written: one behind) */
            kd.rev = (uint8_t)(I <= 70u || kg_rand(&kd.rnd) % 3u != 0u);
        }
        *wl = *wr = m;
        return;
    case DD_PITCH: {                             /* two taps sliding through a delay, crossfaded */
        uint32_t W16 = DD_W << 16, pa = kd.pp, pb = (kd.pp + W16 / 2u) % W16;
        int32_t a = dd_rd(idx - 65u - (pa >> 16), (int32_t)(65535u - (pa & 0xFFFFu))), b = dd_rd(idx - 65u - (pb >> 16), (int32_t)(65535u - (pb & 0xFFFFu)));
        int32_t wa = (int32_t)((pa >> 16) < DD_W / 2u ? (pa >> 16) : DD_W - (pa >> 16));
        m = (a * wa + b * ((int32_t)DD_W / 2 - wa)) >> 10;   /* / (DD_W / 2) */
        kd.pp = (uint32_t)(((int32_t)kd.pp + (65536 - kd.ratio)) % (int32_t)W16 + (int32_t)W16) % W16;
        *wl = *wr = m;
        return;
    }
    case DD_STRETCH: {                           /* two grains at normal speed from a point that moves slower */
        uint32_t q = kd.pp, qb = (q + DD_W / 2u) % DD_W, back;
        int32_t wa = (int32_t)(q < DD_W / 2u ? q : DD_W - q), f;
        if (q == 0u) kd.ga = kd.ri - DD_W / 2u;
        if (qb == 0u) kd.gb = kd.ri - DD_W / 2u;
        m = (ring_raw(kd.ga + q) * wa + ring_raw(kd.gb + qb) * ((int32_t)DD_W / 2 - wa)) >> 10;
        kd.pp = (q + 1u) % DD_W;
        f = kd.rf + kd.ratio;
        kd.ri += (uint32_t)(f >> 16);
        kd.rf = f & 0xFFFF;
        back = idx - kd.ri;
        if (back > BT_RN - 4u * DD_W) kd.ri = idx - 2u * DD_W;   /* too far behind: back near now */
        *wl = *wr = m;
        return;
    }
    case DD_GATE:
    case DD_BSKIP:
        if (kd.sub == DD_BSKIP && kd.mode == 2u) {   /* the 16th before, again */
            m = dd_declick(ring_raw(kd.S + kd.k), kd.k, kd.L);
            if (++kd.k >= kd.L) kd.k = 0;
            *wl = *wr = m;
            return;
        }
        *wl = (x * (kd.gg >> 3)) >> 12;
        *wr = (y * (kd.gg >> 3)) >> 12;
        return;
    case DD_FILTER: {                            /* the state-variable low-pass (brk_fx.c), its cutoff per block */
        *wl = svf_run(&kd.flt, 0, bclamp(x, -32767, 32767), 0);
        *wr = svf_run(&kd.flt, 1, bclamp(y, -32767, 32767), 0);
        return;
    }
    case DD_RING: {
        int32_t s = sine_q(kd.ph);
        kd.ph += kd.dph;
        *wl = (x * (s >> 3)) >> 12;
        *wr = (y * (s >> 3)) >> 12;
        return;
    }
    case DD_FUZZ: {                              /* x1 .. x15 into a soft then a hard clip, about the dry level */
        int32_t d = 16 + (int32_t)I * 224 / 100, v[2] = {x, y}, j;
        for (j = 0; j < 2; j++) {
            int32_t u = (bclamp(v[j], -65535, 65535) * d) >> 4;
            if (u > 12000) u = 12000 + ((u - 12000) >> 3);
            if (u < -12000) u = -12000 + ((u + 12000) >> 3);
            v[j] = (bclamp(u, -18000, 18000) * kd.fz) >> 8;
        }
        *wl = v[0], *wr = v[1];
        return;
    }
    case DD_GLITCH: {
        int32_t a = x, b = y, mk = -(1 << (1 + (int32_t)I / 10));   /* the low bits gone: 1 .. 11 */
        uint32_t hold = 1u + I / 6u;
        if (kd.corrupt) {                        /* a chunk of the past */
            a = b = ring_raw(kd.csrc++);
            kd.corrupt--;
        }
        if (kd.noise) {
            kd.noise--;
            a = b = (int32_t)(kg_rand(&kd.rnd) >> 18) - 8192;   /* (a quarter of full scale) */
        }
        if (kd.hn++ % hold == 0u) kd.hl = a, kd.hr = b;
        *wl = kd.hl & mk;
        *wr = kd.hr & mk;
        return;
    }
    default:
        if (dd_isop(kd.sub)) { beat_sample(x, y, idx, wl, wr); return; }
        *wl = x, *wr = y;
        return;
    }
}

/* the effect the keys ask for (the last pressed of those held), else the step's lock; -1 none */
static volatile uint8_t bt_order[DD_N];
static volatile uint8_t bt_norder;
static int32_t bt_want(void)
{
    uint32_t keys = beat_keys, i;
    for (i = bt_norder; i-- > 0u;)
        if (keys >> bt_order[i] & 1u) return bt_order[i];
    return bt_seq;
}

/* the block: the dry mix into the ring, then the effect over it (in place, Q15) */
static void bt_process(int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i, base, s16, I;
    int32_t want = bt_want();
    base = bt_w;                                 /* (this block's first sample, about to be written) */
    if (kd.cur >= 0 || want >= 0) {
        if (kd.cur < 0) {                        /* a new one */
            kd.cur = (int8_t)want;
            kd.g = 0;
            kd.t = 0;
            kd.jl = kd.jr = 0;
            kd.jll = l[0], kd.jlr = r[0];
            kd.s16 = 0xFFFFFFFFu;
            dd_engage(want == DD_RAND || want == DD_TOTAL ? DD_STUT : want, base);
        }
        s16 = sq_play ? fx_tick0 / (PPQN / 4u) : kd.t / (dd_beat() / 4u + 1u);
        if (s16 != kd.s16) {
            kd.s16 = s16;
            dd_tick(base, s16);
        }
    }
    /* the ring: the dry mix, mono, written once the 16th is set up: a record taken now holds this block's sound,
     * not what an older record kept the writer from (a held record is kept) */
    for (i = 0; i < n; i++, bt_w++)
        if (!bt_owns(bt_w)) bt_ring[bt_w & (BT_RN - 1u)] = (int16_t)bclamp((l[i] + r[i]) >> 1, -32767, 32767);
    if (kd.cur < 0) return;
    I = dd_int();
    kd.fmul = kd.L ? (13000u << 8) / kd.L : 0u;
    kd.fz = (int32_t)(320u * 220u / (220u + 16u + I * 224u / 100u));   /* x1: 1.17, x15: 0.6 */
    if (kd.sub == DD_GATE) {                     /* the slot of the bar: open or shut */
        uint32_t per = I > 50u ? 32u : 16u, slot = dd_barpos() * per / (dd_beat() * 4u + 1u);
        if (slot < kd.i32 || kd.i32 == 0xFFFFFFFFu) {   /* a new bar: a new pattern */
            uint32_t j;
            kd.mask = 0;
            for (j = 0; j < per; j++)
                if ((j % (per / 4u)) == 0u || kg_rand(&kd.rnd) % 100u >= 20u + I * 6u / 10u) kd.mask |= 1u << j;
        }
        kd.i32 = slot;
    }
    if (dd_isgate(kd.sub)) kd.open = (uint8_t)beat_gate(kd.sub, dd_barpos(), I);
    if (kd.sub == DD_FILTER) {                   /* the cutoff glides to its 16th's, per block */
        int32_t ix;
        kd.cut += (kd.cut_to - kd.cut) / 8;
        ix = ((kd.cut >> 8) - 36) * 63 / 90;
        svf_coef(&kd.flt, SVF_G_LP[ix < 0 ? 0 : ix > 63 ? 63 : ix], kd.fq);
    }
    for (i = 0; i < n; i++) {
        int32_t x = l[i], y = r[i], wl, wr, target = want == kd.cur ? 32767 : 0;
        if (kd.g < target) kd.g = kd.g + 512 > target ? target : kd.g + 512;
        else if (kd.g > target) kd.g = kd.g - 512 < target ? target : kd.g - 512;
        if (kd.sub == DD_GATE || kd.sub == DD_BSKIP || dd_isop(kd.sub)) {   /* the gate's gain glides (64 samples) */
            int32_t open = kd.sub == DD_GATE ? (kd.i32 < 32u && (kd.mask >> kd.i32 & 1u)) : dd_isop(kd.sub) ? kd.open : kd.mode != 1u;
            int32_t gt = open ? 32767 : 0;
            kd.gg += gt > kd.gg ? (gt - kd.gg > 512 ? 512 : gt - kd.gg) : -(kd.gg - gt > 512 ? 512 : kd.gg - gt);
        }
        dd_sample(x, y, base + i, &wl, &wr);
        if (!dd_isop(kd.sub)) {                  /* (the programs declick their own switches) */
            if (kd.jump) { kd.jump = 0; kd.jl = kd.jll - wl; kd.jr = kd.jlr - wr; }
            wl += kd.jl, wr += kd.jr;
            kd.jl -= kd.jl >> 5, kd.jr -= kd.jr >> 5;
            kd.jll = wl, kd.jlr = wr;
        }
        if (kd.cur == DD_TOTAL) {                /* TOTAL: crushed, sometimes ring modulated */
            int32_t mk = -(1 << (3 + (int32_t)I / 14));
            wl &= mk, wr &= mk;
            if (kd.mode) {
                int32_t s = sine_q(kd.ph2);
                kd.ph2 += 600u * (0xFFFFFFFFu / FS);   /* (600 Hz) */
                wl = (wl * (s >> 3)) >> 12, wr = (wr * (s >> 3)) >> 12;
            }
        }
        if (kd.xf < 64) {                        /* a sub-effect just came in: from the dry sound */
            wl = x + (((wl - x) * kd.xf) >> 6);
            wr = y + (((wr - y) * kd.xf) >> 6);
            kd.xf++;
        }
        wl = bclamp(wl - x, -65536, 65536), wr = bclamp(wr - y, -65536, 65536);
        l[i] = bclamp(x + ((wl * (kd.g >> 3)) >> 12), -65536, 65535);
        r[i] = bclamp(y + ((wr * (kd.g >> 3)) >> 12), -65536, 65535);
        kd.t++;
    }
    if (kd.g == 0 && want != kd.cur) {           /* faded out: the next one starts on the next block */
        kd.cur = -1;
        kd.own = 0;
    }
}

static void bt_reset(void)
{
    uint32_t i;
    for (i = 0; i < BT_RN; i++) bt_ring[i] = 0;
    kd.cur = -1;
    kd.own = 0;
    bt_norder = 0;
    bt_seq = -1;
}

/* a key of the DESTROY / BEAT pages pressed or let go (main loop): e = 0..31, the press order kept */
static void bt_key(uint32_t e, uint32_t down)
{
    uint32_t i, j = 0;
    uint8_t o[DD_N];
    if (e >= DD_N) return;
    for (i = 0; i < bt_norder; i++) if (bt_order[i] != e) o[j++] = bt_order[i];
    if (down && j < DD_N) o[j++] = (uint8_t)e;
    for (i = 0; i < j; i++) bt_order[i] = o[i];
    bt_norder = (uint8_t)j;
    if (down) beat_keys |= 1u << e;
    else beat_keys &= ~(1u << e);
}

/* ---- PAN (ARP's second page): 16 effects on the white keys, held (SHIFT: latched), on the whole output after the
 * master. The first six move the sound with the tempo; the others take it apart across the two sides, glitchy.
 * Each has its INTENSITY (KNOB 1 on its page); SPEED plays them at half, the, or double speed; EDGE makes their
 * moves smooth or hard (to clicks). One plays at a time: the last key held. A ring of what came out (mono, 8192
 * samples) feeds STUT SIDE, REV SIDE and HAAS JUMP. */
enum { PN_AUTO4, PN_AUTO8, PN_AUTO16, PN_PING, PN_TRIP, PN_CIRCLE, PN_RAND, PN_CLICK, PN_STUT, PN_REVS, PN_HAAS,
       PN_SPLIT, PN_FLIP, PN_GLITCH, PN_DRUNK, PN_CHAOS };
static const char *const PAN_NAME[NPAN][2] = {
    {"AUTO", "1/4"}, {"AUTO", "1/8"}, {"AUTO", "1/16"}, {"PING", "8TH"}, {"TRIP", "L C R"}, {"CIRCLE", "BAR"},
    {"RANDOM", "16TH"}, {"CLICK", "PAN"}, {"STUT", "SIDE"}, {"REV", "SIDE"}, {"HAAS", "JUMP"}, {"SPLIT", "HITS"},
    {"FLIP", "L/R"}, {"GLITCH", "PAN"}, {"DRUNK", ""}, {"PAN", "CHAOS"}};
static const char *const PAN_LONG[NPAN] = {"AUTO 1/4", "AUTO 1/8", "AUTO 1/16", "PING 8TH", "TRIPLET L C R", "CIRCLE",
    "RANDOM 16TH", "CLICK PAN", "STUT SIDE", "REV SIDE", "HAAS JUMP", "SPLIT HITS", "FLIP L/R", "GLITCH PAN", "DRUNK",
    "PAN CHAOS"};
#define PN_RN 8192u
static int16_t pn_ring[PN_RN];
static volatile uint32_t pan_keys;               /* bit e: PAN effect e held or latched (main loop) */
static volatile uint8_t pn_order[NPAN], pn_norder;
static struct {
    int8_t cur, sub;                             /* the effect playing (-1 none); CHAOS: the one this 16th */
    int32_t g;                                   /* its wet gain, Q15 */
    uint32_t w;                                  /* the ring's writer */
    uint32_t i16, ph16, s16;                     /* the 16th (of the bar, at its speed), where in it, its length */
    int32_t pos, to;                             /* where the sound is, -32767 (left) .. 32767 (right), and where it goes */
    int32_t amp;                                 /* CIRCLE: its level at the back */
    uint32_t seg, segL, k;                       /* STUT, REV: the segment (ring index), its length, the place in it */
    uint32_t side;                               /* STUT, REV, HAAS: the side that plays it (0 left, 1 right) */
    uint32_t hd;                                 /* HAAS: the delay */
    uint32_t mask, flip, mute, rnd, free;
    int32_t gl, gr;                              /* the gains now */
} pn = {.cur = -1, .sub = -1, .rnd = 0x7A11C0DEu};

static int32_t pn_want(void)
{
    uint32_t keys = pan_keys, i;
    for (i = pn_norder; i-- > 0u;) if (keys >> pn_order[i] & 1u) return pn_order[i];
    return -1;
}
static void pn_key(uint32_t e, uint32_t down)    /* (main loop) */
{
    uint32_t i, j = 0;
    uint8_t o[NPAN];
    if (e >= NPAN) return;
    for (i = 0; i < pn_norder; i++) if (pn_order[i] != e) o[j++] = pn_order[i];
    if (down && j < NPAN) o[j++] = (uint8_t)e;
    for (i = 0; i < j; i++) pn_order[i] = o[i];
    pn_norder = (uint8_t)j;
    if (down) pan_keys |= 1u << e;
    else pan_keys &= ~(1u << e);
}
/* a new 16th: what the glitchy ones do over it */
static void pn_tick(uint32_t I)
{
    uint32_t r = kg_rand(&pn.rnd), s = pn.i16 & 15u;
    int32_t dep = (int32_t)(I * 327u);           /* the depth: 0 .. 32700 */
    if (pn.cur == PN_CHAOS) pn.sub = (int8_t)(PN_RAND + r % 8u);   /* (a glitchy one each 16th) */
    switch (pn.sub) {
    case PN_RAND: pn.to = (int32_t)(r >> 16) % (2 * dep + 1) - dep; break;
    case PN_STUT:                                /* the last 16th, again, on one side (the other each beat) */
        if ((s & 3u) == 0u) { pn.side ^= 1u; pn.segL = pn.s16 < PN_RN - 64u ? pn.s16 : PN_RN - 64u; pn.seg = pn.w - pn.segL; pn.k = 0; }
        break;
    case PN_REVS:                                /* the 16th before, backwards, on one side (the other each bar) */
        if (s == 0u) pn.side ^= 1u;
        pn.segL = pn.s16 < PN_RN - 64u ? pn.s16 : PN_RN - 64u;
        pn.seg = pn.w - pn.segL;
        pn.k = 0;
        break;
    case PN_HAAS: pn.side = r & 1u; pn.hd = 44u + (r >> 8) % (44u + I * 11u); break;   /* 1 .. 26 ms */
    case PN_SPLIT:
        if (s == 0u) pn.mask = kg_rand(&pn.rnd);
        pn.to = (pn.mask >> s & 1u) ? dep : -dep;
        break;
    case PN_FLIP: pn.flip = (r % 100u) < 30u + I * 6u / 10u ? pn.flip ^ 1u : pn.flip; break;
    case PN_DRUNK:                               /* a stagger: a step a 16th, half the width at most, off the walls */
        pn.to += (int32_t)((r >> 16) % (uint32_t)(dep + 1)) - dep / 2;
        if (pn.to > dep) pn.to = 2 * dep - pn.to;
        if (pn.to < -dep) pn.to = -2 * dep - pn.to;
        pn.to = bclamp(pn.to, -dep, dep);
        break;
    default: break;
    }
}
static void pn_process(int32_t *L, int32_t *R, uint32_t n)
{
    uint32_t i, I, sp = prj.p2.pn_speed, edge = prj.p2.pn_edge > 15u ? 15u : prj.p2.pn_edge, beat = dd_beat(), p0, sh;
    int32_t want = pn_want();
    for (i = 0; i < n; i++)                      /* the ring: what comes out, mono */
        pn_ring[(pn.w + i) & (PN_RN - 1u)] = (int16_t)bclamp((L[i] + R[i]) >> 1, -32767, 32767);
    if (pn.cur < 0 && want < 0) { pn.w += n; return; }
    if (pn.cur < 0) {                            /* a new one */
        pn.cur = (int8_t)want;
        pn.sub = (int8_t)want;
        pn.g = 0;
        pn.pos = pn.to = 0;
        pn.i16 = 0xFFFFFFFFu;
        pn.flip = pn.mute = 0;
        pn.gl = pn.gr = 23170;
    }
    I = prj.p2.pn_int[(uint32_t)pn.cur % NPAN];
    if (I > 100u) I = 100u;
    /* where the bar is, at the effect's speed: its 16th, the place in it */
    pn.s16 = (beat / 4u) * 2u / (1u << sp);      /* (half, the, double speed) */
    if (pn.s16 < 64u) pn.s16 = 64u;
    p0 = sq_play ? dd_barpos() * (1u << sp) / 2u : pn.free;
    pn.free += n;
    pn.ph16 = p0 % pn.s16;
    if (p0 / pn.s16 != pn.i16) { pn.i16 = p0 / pn.s16; pn_tick(I); }
    sh = (15u - edge) * 10u / 15u;               /* the moves: 10 (smooth) .. 0 (at once) */
    for (i = 0; i < n; i++) {
        int32_t x = L[i], y = R[i], m = (x + y) >> 1, sd = (x - y) >> 1, wl, wr, gl, gr, target = want == pn.cur ? 32767 : 0;
        int32_t dep = (int32_t)(I * 327u);
        uint32_t ph = pn.ph16, len = pn.s16, wi = (pn.w + i) & (PN_RN - 1u);
        int32_t e = pn.sub;
        if (pn.g < target) pn.g = pn.g + 128 > target ? target : pn.g + 128;
        else if (pn.g > target) pn.g = pn.g - 128 < target ? target : pn.g - 128;
        pn.amp = 32767;
        switch (e) {                             /* the place to go */
        case PN_AUTO4: case PN_AUTO8: case PN_AUTO16: {
            uint32_t per = e == PN_AUTO4 ? len * 4u : e == PN_AUTO8 ? len * 2u : len, at = ((pn.i16 % (per / len)) * len + ph) % per;
            uint32_t inv = 0xFFFFFFFFu / per;    /* (no 64-bit division: a 32-bit one, then a multiply) */
            pn.to = (int32_t)(((int64_t)sin16((uint32_t)(((uint64_t)at * inv) >> 16)) * dep) >> 15);
            break;
        }
        case PN_PING: pn.to = ((pn.i16 >> 1) & 1u) ? dep : -dep; break;
        case PN_TRIP: { uint32_t t3 = ((pn.i16 & 3u) * len + ph) * 3u / (len * 4u); pn.to = (int32_t)(t3 > 2u ? 2u : t3) * dep - dep; break; }
        case PN_CIRCLE: {                        /* round over a bar: left, front, right, the back (quieter, darker) */
            uint32_t at = ((pn.i16 & 15u) * len + ph) * 4096u / (len * 16u);
            pn.to = (int32_t)(((int64_t)sin16(at << 4) * dep) >> 15);
            pn.amp = 32767 - (32767 - sin16((at << 4) + 16384u)) * (int32_t)I / 400;
            break;
        }
        case PN_CLICK: {                         /* hard from side to side each 32nd (64th past 50 %), no fade */
            uint32_t div = I > 50u ? 4u : 2u;
            pn.to = ((pn.i16 * div + ph * div / len) & 1u) ? 32767 : -32767;
            pn.pos = pn.to;
            break;
        }
        case PN_GLITCH: {                        /* each 32nd: hard left, hard right, the middle, or nothing */
            uint32_t h = hash3(pn.i16, ph * 2u / len, pn.rnd) % 100u;
            pn.mute = h < I / 3u;
            pn.to = h < 40u + I / 3u ? (h & 1u ? 32767 : -32767) : 0;
            pn.pos = pn.to;
            break;
        }
        default: break;
        }
        if (e != PN_CLICK && e != PN_GLITCH) {   /* the move: smooth, or hard (EDGE) */
            if (sh) pn.pos += (pn.to - pn.pos) >> sh;
            else pn.pos = pn.to;
        }
        if (!(i & 3u) || e == PN_CLICK || e == PN_GLITCH) {   /* where it is: the two gains (equal power), every 4 samples */
            uint32_t a = (uint32_t)(pn.pos + 32768) >> 2;   /* 0 .. 16383: a quarter turn */
            pn.gl = sin16(16384u - a > 16383u ? 16383u : 16384u - a);
            pn.gr = sin16(a > 16383u ? 16383u : a);
        }
        gl = pn.gl, gr = pn.gr;
        wl = (int32_t)(((int64_t)m * gl) >> 15) * 19661 >> 14;   /* (x 1.2: the middle a little quieter, a side */
        wr = (int32_t)(((int64_t)m * gr) >> 15) * 19661 >> 14;   /* a little louder, the peaks kept) */
        wl += sd, wr -= sd;
        switch (e) {                             /* the ones that change the sound of one side */
        case PN_STUT: case PN_REVS: {
            uint32_t L0 = pn.segL ? pn.segL : 64u, k = pn.k % L0, idx = e == PN_STUT ? pn.seg + k : pn.seg + L0 - 1u - k;
            int32_t v = pn_ring[idx & (PN_RN - 1u)], f = k < 64u ? (int32_t)k : L0 - k < 64u ? (int32_t)(L0 - k) : 64;
            v = (v * f >> 6) * (int32_t)I / 100 + m * (100 - (int32_t)I) / 100;
            pn.k++;
            if (pn.side) wr = v; else wl = v;
            break;
        }
        case PN_HAAS: { int32_t v = pn_ring[(wi - pn.hd) & (PN_RN - 1u)]; if (pn.side) wr = v; else wl = v; break; }
        case PN_FLIP: {                          /* left and right swapped, one side upside down each 16th */
            if (pn.flip) { int32_t t = wl; wl = wr; wr = t; }
            if (pn.i16 & 1u && I > 50u) wr = -wr;
            break;
        }
        default: break;
        }
        if (pn.mute) wl = wr = 0;
        if (pn.amp != 32767) { wl = (int32_t)(((int64_t)wl * pn.amp) >> 15); wr = (int32_t)(((int64_t)wr * pn.amp) >> 15); }
        L[i] = bclamp(x + (int32_t)(((int64_t)(wl - x) * pn.g) >> 15), -32767, 32767);
        R[i] = bclamp(y + (int32_t)(((int64_t)(wr - y) * pn.g) >> 15), -32767, 32767);
        if (++pn.ph16 >= pn.s16) {               /* the next 16th */
            pn.ph16 = 0;
            pn.i16++;
            pn_tick(I);
        }
    }
    pn.w += n;
    if (pn.g == 0 && want != pn.cur) pn.cur = pn.sub = -1;
    else if (want >= 0 && want != pn.cur && pn.g == 0) pn.cur = -1;
}
