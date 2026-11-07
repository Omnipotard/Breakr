/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: the BEAT bank (FX, third page), K-GLITCH's (kg_beat.c, kg_dd.c, by Omnipotard) brought over: 16 programs over
 * the bar on the white keys. Audio interrupt, on the whole mix, before the master (brk_fx.c). Each effect is a program
 * over the bar: on each of its 16 sixteenths, an operation on a ring of the dry mix (mono, 0.74 s), chosen anew each
 * bar from the bar's seed (the random ones) or fixed, shaped by its INTENSITY (0..100, KNOB 1 on the page). The
 * programs are aligned to the sequencer (a key pressed in the middle of a bar starts in the middle of its program), so
 * they stay on the grid; stopped, they count from the press.
 *   RND STUTTER   stutters (32nds, triplets, 64ths) on random 8ths, more with INTENSITY
 *   RND BAR       every 16th: dry, repeated, the one before, backwards, muted, half speed or pitched (new each bar)
 *   RND FILL      the bar stays, its end (last beat, or the second half) becomes a random fill
 *   ROLL UP       the bar's downbeat repeated: a quarter, then 8ths, 16ths, 32nds (+ pitch going up with INTENSITY)
 *   SKIP 8TH      the off-8ths dropped (or, past 50 %, the 8th before again)
 *   STAMMER       3-3-2: the hits kept, what is between them the hit again (32nds past 80 %)
 *   GATE 8 / 16   a trance gate, INTENSITY its length (16: holes at random past 70 %)
 *   GATE 3-3-2    the gate on a dotted rhythm
 *   GATE RAMP     a gate that speeds up: quarters, 8ths, 16ths, 32nds
 *   REV BEAT      beats played backwards (2 and 4, or all of them past 50 %)
 *   REV OFFBEAT   each off-8th the 8th before backwards (and the last beat past 60 %)
 *   SCRATCH BAR   a beat of silence, three short scratches of the downbeat, then the downbeat backwards
 *   SCRATCH CUT   beats 1 and 3 as they are, 2 and 4 scratched with the crossfader cutting
 *   TAPE STOP     the end of the bar slows down to a stop (the last beat, or the second half)
 *   HALF TIME     each beat (or 8th past 50 %) its first half played at half speed
 * The scratches move a "record" the way a hand does (smooth strokes that stop and turn back) under a crossfader, read
 * with a cubic interpolation; the sound follows the record's speed as a cartridge does: duller and quieter when slow,
 * silent when still. Their record: the downbeat (captured on the bar), else the last attack found in the ring.
 * One effect plays at a time: the last key pressed (let go, the one still held before it). Every switch fades over
 * 64 samples. Q15 throughout. */

enum { BT_RSTUT, BT_RBAR, BT_RFILL, BT_ROLL, BT_SKIP8, BT_STAMR, BT_GATE8, BT_GATE16, BT_GATE332, BT_GRAMP,
       BT_REVB, BT_REVOFF, BT_SCRBAR, BT_SCRCUT, BT_TSTOP, BT_HALFT, BT_N };

#define BT_RN 32768u                             /* the ring: power of two, 0.74 s */
static int16_t bt_ring[BT_RN] POOL;
static uint32_t bt_w;                            /* the ring's writer (absolute index) */
static uint32_t fx_tick0, fx_ph0, fx_spt = 1u << 24;   /* the sequencer's clock at the block's start (brk_seq.c) */

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
    int8_t cur;                                  /* the effect playing (BT_*), -1 none */
    int32_t g;                                   /* its wet gain, 0..32767 */
    int32_t xf;                                  /* its fade in, 0..64 */
    uint32_t t;                                  /* samples since it started */
    uint32_t s16;                                /* the 16th last seen */
    uint32_t S, L, L0, k;                        /* a segment of the ring: start, length (L0: as captured), position */
    uint8_t own;                                 /* the segment is held from the writer */
    uint32_t ri;                                 /* a read position (ring index), its fraction (Q16), the speed (Q16) */
    int32_t rf, sp;
    int32_t gg;                                  /* the gate's gain, Q15 */
    uint32_t rnd;
    uint32_t bseed, rec, slen, sk, D, u24, du;   /* the bar's seed, the record, the 16th's length, the stroke */
    int32_t acc, lpy, ps, dl, dr, ll, lr;        /* STOP's slowdown; the cartridge's low-pass; the record's last place
                                                  * (Q15); the jump's declick and the last output */
    uint8_t op, arg, open, newop, qi, from8, to8;
} kd = {.cur = -1, .rnd = 0x51A3E1B7u};

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
static int bt_isgate(int32_t e) { return e >= BT_GATE8 && e <= BT_GRAMP; }

static int32_t ring_raw(uint32_t i) { return bt_ring[i & (BT_RN - 1u)]; }
static uint32_t dd_int(void) { uint32_t v = prj.p.bt_int[(uint32_t)(kd.cur >= 0 ? kd.cur : 0) % BT_N]; return v > 100u ? 100u : v; }
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
    case BT_RSTUT: {                             /* decided per 8th */
        static const uint8_t ST[6] = {KD32, KD16, KD16, KD12, KD24, KD8};
        uint32_t hp = kb_hash(kd.bseed ^ ((s >> 1) * 0x85EBCA6Bu));
        if (s < 2u || hp % 100u >= 15u + I * 6u / 10u) return BO(BO_DRY, 0);
        return BO(BO_REP, ST[(hp >> 8) % 6u]) | (s & 1u ? B_CONT : 0u);
    }
    case BT_RBAR: {
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
    case BT_RFILL:
        if (s < (I < 50u ? 12u : 8u)) return BO(BO_DRY, 0);
        if (s == 15u) return (h >> 8) % 2u ? BO(BO_REP, KD32) : BO(BO_MUTE, 0);
        switch ((h >> 8) % 5u) {
        case 0: return BO(BO_REP, KD16);
        case 1: return BO(BO_REP, KD8);
        case 2: return BO(BO_REV, 1);
        case 3: return BO(BO_PREV, 1);
        default: return BO(BO_PREP, 5u + (h >> 12) % 8u);
        }
    case BT_ROLL:                                /* the downbeat: a quarter, 8ths, 16ths, 32nds */
        if (b == 0u) return BO(BO_DRY, 0) | (s == 0u ? B_CAP : 0u);
        return BO(BO_PREP, b) | (q ? B_CONT : 0u);
    case BT_SKIP8:
        if (q < 2u) return BO(BO_DRY, 0);
        return I < 50u ? BO(BO_MUTE, 0) : BO(BO_PREV, 2);
    case BT_STAMR: {                             /* hits on 0, 3, 6 of each half bar */
        static const uint8_t BACK[8] = {0, 1, 2, 0, 1, 2, 0, 1};
        uint32_t k = s & 7u;
        if (!BACK[k] || (I < 40u && k != 1u && k != 4u && k != 7u)) return BO(BO_DRY, 0);
        return I > 80u ? BO(BO_REP, KD32) : BO(BO_PREV, BACK[k]);
    }
    case BT_REVB:
        if (I < 50u && !(b & 1u)) return BO(BO_DRY, 0);
        return BO(BO_REV, 4) | (q ? B_CONT : 0u);
    case BT_REVOFF:
        if (I >= 60u && b == 3u) return BO(BO_REV, 4) | (q ? B_CONT : 0u);
        if (q < 2u) return BO(BO_DRY, 0);
        return BO(BO_REV, 2) | (q == 3u ? B_CONT : 0u);
    case BT_SCRBAR: {                            /* silence, three short scratches, the downbeat backwards */
        uint32_t mute = I > 66u ? 2u : 4u, n = I > 66u ? 5u : 3u, k;
        static const uint8_t SB[5] = {SP_CHIRP, SP_TEAR, SP_TRANS, SP_BABY, SP_BACK};
        if (s < mute) return BO(BO_MUTE, 0) | (s == 0u ? B_CAP : 0u);
        if (s >= 12u) return BO(BO_RREV, 0) | (s > 12u ? B_CONT : 0u);
        k = (s - mute) / 2u;
        if ((s - mute) & 1u || k >= n) return BO(BO_MUTE, 0);
        return BO(BO_SCR, I < 34u ? SP_CHIRP : SB[k % 5u]);
    }
    case BT_SCRCUT:
        if (!(b & 1u)) return BO(BO_DRY, 0) | (q == 0u ? B_CAP : 0u);
        return BO(BO_SCR, I < 50u ? (q & 1u ? SP_BACK : SP_TRANS) : (h >> 8) % 2u ? SP_SCRIB : SP_TRANS);
    case BT_TSTOP: {
        uint32_t st = I < 50u ? 12u : 8u;
        if (s < st) return BO(BO_DRY, 0);
        return BO(BO_STOP, 16u - st) | (s > st ? B_CONT : 0u);
    }
    case BT_HALFT:
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
    case BT_GATE8:
        cyc = beat / 2u;
        break;
    case BT_GATE16:
        cyc = beat / 4u;
        if (I > 70u && kb_hash(kd.bseed ^ (p / (cyc + 1u)) * 0x27D4EB2Fu) % 100u < I - 70u) return 0;
        break;
    case BT_GATE332: {                           /* 3 + 3 + 2 sixteenths each half bar */
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
    code = beat_op(kd.cur, s, I);
    op = code & 15u;
    arg = code >> 4 & 15u;
    kd.slen = slen;
    kd.sk = 0;
    if (code & B_CAP) {                          /* the record: this 16th on (held from the writer) */
        kd.rec = base;
        kd.S = base;
        kd.L0 = kd.cur == BT_ROLL ? (beat / 2u < KB_OWN ? beat / 2u : KB_OWN) : (beat < KB_OWN ? beat : KB_OWN);
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
        if (kd.cur != BT_ROLL && kd.cur != BT_SCRBAR) kd.own = 0;
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
        int32_t semi = kd.cur == BT_ROLL ? (I > 50u ? (int32_t)(arg * (I > 80u ? 4u : 2u)) : 0) : (int32_t)arg;
        if (kd.cur == BT_ROLL) {
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
        int32_t a = (int32_t)(kd.from8 * kd.D / 8u), d = ((int32_t)kd.to8 - (int32_t)kd.from8) * (int32_t)kd.D / 8, off, v;
        uint32_t ri;
        off = a * 32768 + d * (int32_t)sm;
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

/* the effect e takes over at ring index base: its record (the last attack: kb_cue) and seed */
static void beat_engage(int32_t e, uint32_t base)
{
    uint32_t beat = dd_beat(), L;
    kd.cur = (int8_t)e;
    kd.g = 0;
    kd.t = 0;
    kd.xf = 0;
    kd.s16 = 0xFFFFFFFFu;
    kd.gg = 32767;
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
    L = beat;
    kd.S = kd.rec;
    kd.L0 = L < KB_OWN ? L : KB_OWN;
    kd.own = e == BT_SCRBAR || e == BT_SCRCUT || e == BT_ROLL;
}

/* the effect the keys ask for now: the last pressed of those held (bt_order: the press order), -1 none */
static volatile uint8_t bt_order[BT_N];
static volatile uint8_t bt_norder;
static int32_t bt_want(void)
{
    uint32_t keys = beat_keys, i;
    for (i = bt_norder; i-- > 0u;)
        if (keys >> bt_order[i] & 1u) return bt_order[i];
    return -1;
}

/* the block: the dry mix into the ring, then the effect over it (in place, Q15) */
static void bt_process(int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i, base, s16, I;
    int32_t want = bt_want();
    for (i = 0; i < n; i++, bt_w++)              /* the ring: the dry mix, mono (a held record kept) */
        if (!bt_owns(bt_w)) bt_ring[bt_w & (BT_RN - 1u)] = (int16_t)bclamp((l[i] + r[i]) >> 1, -32767, 32767);
    base = bt_w - n;
    if (kd.cur < 0 && want < 0) return;
    if (kd.cur < 0) beat_engage(want, base);    /* a new one */
    else if (want >= 0 && want != kd.cur && kd.g == 0) beat_engage(want, base);
    s16 = sq_play ? fx_tick0 / (PPQN / 4u) : kd.t / (dd_beat() / 4u + 1u);
    if (s16 != kd.s16) {
        kd.s16 = s16;
        beat_slot(s16 % 16u, base);
    }
    I = dd_int();
    if (bt_isgate(kd.cur)) kd.open = (uint8_t)beat_gate(kd.cur, dd_barpos(), I);
    for (i = 0; i < n; i++) {
        int32_t x = l[i], y = r[i], wl, wr, target = want == kd.cur ? 32767 : 0, gt = kd.open ? 32767 : 0;
        if (kd.g < target) kd.g = kd.g + 512 > target ? target : kd.g + 512;
        else if (kd.g > target) kd.g = kd.g - 512 < target ? target : kd.g - 512;
        kd.gg += gt > kd.gg ? (gt - kd.gg > 512 ? 512 : gt - kd.gg) : -(kd.gg - gt > 512 ? 512 : kd.gg - gt);
        beat_sample(x, y, base + i, &wl, &wr);
        if (kd.xf < 64) {                        /* it just came in: from the dry sound */
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
}

/* a key of the page pressed / let go (main loop): the press order kept */
static void bt_key(uint32_t e, uint32_t down)
{
    uint32_t i, j = 0;
    uint8_t o[BT_N];
    for (i = 0; i < bt_norder; i++) if (bt_order[i] != e) o[j++] = bt_order[i];
    if (down && j < BT_N) o[j++] = (uint8_t)e;
    for (i = 0; i < j; i++) bt_order[i] = o[i];
    bt_norder = (uint8_t)j;
    if (down) beat_keys |= 1u << e;
    else beat_keys &= ~(1u << e);
}
