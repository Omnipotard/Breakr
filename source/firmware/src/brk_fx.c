/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: the master (audio interrupt), in this order: DRIVE (a soft clip), CRUSH (bits and sample rate),
 * the FILTER (TONE's TYPE: DJ, one knob low-pass to the left, high-pass to the right, off in the middle; BAND;
 * COMB; VOWEL, two formants from A to U; WAH, a low-pass the hits open; MOVE sweeps it with the tempo or jumps it
 * at random), the TRANCE gates of two black keys, the DELAY (tempo-synced; TYPE: CLEAN, DUB a tape with its wow,
 * dark and saturated repeats that can run away; PING left and right; REVERSE; LO-FI), PUNCH (a compressor with its
 * make-up gain), LO-CUT, the WIDTH, then the flex gate and the metronome. Integers only. */

/* the filters: a state-variable filter in its trapezoidal form (Andrew Simper's "zero-delay feedback" SVF): stable at
 * any cutoff and any resonance, its cutoff where the table says, once a sample (no oversampling). g = tan(pi fc /
 * 44100) and the damping k = 1 / Q, both Q24; its coefficients a1 = 1 / (1 + g (g + k)), a2 = g a1, a3 = g a2 are
 * worked out when g or k moves (a 32-bit division and one Newton step). The states are Q15 audio; the products are
 * 32 x 32 -> 64 bits (one instruction on the FM-1's core). */
static const int32_t SVF_G_LP[64] = {   /* low-pass: 60 Hz .. 20 kHz */
    71711, 78638, 86234, 94564, 103698, 113715, 124699, 136745, 149955, 164440, 180325, 197745, 216848, 237797,
    260770, 285963, 313591, 343890, 377119, 413560, 453526, 497358, 545432, 598160, 655995, 719434, 789024, 865368,
    949127, 1041032, 1141885, 1252576, 1374083, 1507492, 1654006, 1814961, 1991845, 2186319, 2400247, 2635723,
    2895119, 3181128, 3496835, 3845795, 4232142, 4660729, 5137315, 5668824, 6263687, 6932343, 7687929, 8547314,
    9532622, 10673603, 12011404, 13604826, 15541249, 17956799, 21076468, 25301753, 31427741, 41285870, 60259871,
    114064865
};
static const int32_t SVF_G_HP[64] = {   /* high-pass: 20 Hz .. 10 kHz */
    23903, 26382, 29117, 32135, 35467, 39144, 43202, 47681, 52625, 58080, 64102, 70748, 78083, 86178, 95112, 104973,
    115857, 127868, 141126, 155757, 171906, 189730, 209402, 231114, 255078, 281528, 310722, 342944, 378510, 417767,
    461099, 508931, 561732, 620019, 684367, 755409, 833848, 920462, 1016112, 1121755, 1238454, 1367391, 1509882,
    1667397, 1841583, 2034285, 2247583, 2483829, 2745695, 3036235, 3358963, 3717955, 4117981, 4564683, 5064815,
    5626575, 6260069, 6977976, 7796517, 8736920, 9827665, 11108090, 12634384, 14490049
};

typedef struct { int32_t g, k, a1, a2, a3; int32_t ic1[2], ic2[2], bp[2]; } svf_t;

static void svf_coef(svf_t *f, int32_t g, int32_t k)
{
    int32_t d, x, e;
    if (g == f->g && k == f->k) return;
    f->g = g;
    f->k = k;
    d = (1 << 24) + (int32_t)(((int64_t)g * (g + k)) >> 24);   /* 1 + g (g + k), Q24 (<= ~61) */
    x = (int32_t)(0x80000000u / (uint32_t)(d >> 8)) << 9;     /* 1 / d to 15 bits, then Q24 */
    e = (1 << 24) - (int32_t)(((int64_t)d * x) >> 24);        /* one Newton step: x (2 - d x) */
    f->a1 = x + (int32_t)(((int64_t)x * e) >> 24);
    f->a2 = (int32_t)(((int64_t)g * f->a1) >> 24);
    f->a3 = (int32_t)(((int64_t)g * f->a2) >> 24);
}
static void svf_clear(svf_t *f) { f->ic1[0] = f->ic1[1] = f->ic2[0] = f->ic2[1] = 0; f->g = -1; }
/* one sample of channel c: the low-pass out, the high-pass in *hi */
static int32_t svf_run(svf_t *f, uint32_t c, int32_t v0, int32_t *hi)
{
    int32_t v3 = v0 - f->ic2[c], v1, v2;
    v1 = (int32_t)(((int64_t)f->a1 * f->ic1[c] + (int64_t)f->a2 * v3) >> 24);
    v2 = f->ic2[c] + (int32_t)(((int64_t)f->a2 * f->ic1[c] + (int64_t)f->a3 * v3) >> 24);
    f->bp[c] = v1;
    f->ic1[c] = 2 * v1 - f->ic1[c];
    f->ic2[c] = 2 * v2 - f->ic2[c];
    f->ic1[c] = f->ic1[c] > 1048575 ? 1048575 : f->ic1[c] < -1048575 ? -1048575 : f->ic1[c];   /* (never: it is stable) */
    f->ic2[c] = f->ic2[c] > 1048575 ? 1048575 : f->ic2[c] < -1048575 ? -1048575 : f->ic2[c];
    if (hi) *hi = v0 - (int32_t)(((int64_t)f->k * v1) >> 24) - v2;
    return v2;
}
/* the damping for RESO 0..15: 2.0 (no bump) .. 0.2 (a resonance of 5), Q24 */
static int32_t svf_damp(uint32_t reso) { return (int32_t)(32768u - (reso > 15u ? 15u : reso) * 1966u) << 10; }

#define DLY_N 44100u                            /* 1 s of delay line */
static int16_t dly_buf[DLY_N] POOL;
static struct {
    svf_t flt;                                  /* the DJ filter */
    int32_t gcur;                               /* its cutoff now (g, Q24; 0: off), gliding to the knob's */
    uint32_t fhp;                               /* 1: it runs as the high-pass */
    uint32_t hold_n, hold_c;                    /* crush: sample-rate hold */
    int32_t hold_v[2];
    uint32_t dw;                                /* delay write */
    int32_t dlp, dtime;                         /* delay: feedback low-pass state, current time (smoothed) */
    int32_t env, cgain;                         /* compressor: envelope, gain Q12 */
    int32_t gate, gate_t;                       /* flex gate: current and target (Q15) */
    int32_t click_ph, click_n, click_f;         /* metronome */
    /* 0.4 */
    svf_t f2;                                   /* VOWEL: the second formant */
    uint32_t ftype;                             /* the type running (a change: from silence) */
    int32_t fq4, fq4_to;                        /* BAND, WAH: where the filter is (index Q4 of SVF_G_LP), gliding */
    int32_t wenv;                               /* WAH: the hits' envelope */
    int32_t cd, cd_to;                          /* COMB: its delay (Q4), gliding */
    uint32_t cw;                                /* .. its write */
    uint32_t mv_slot;                           /* MOVE: the 16th or 8th of its last random */
    int32_t mv_rnd;
    uint32_t pos0, beat, play;                  /* the bar at the block's start (frames into it), a beat, playing */
    uint32_t tg;                                /* TRANCE: the gates the black keys ask (bit 0: 16THS, bit 1: PATTERN) */
    int32_t tgg;                                /* .. their gain, Q15 */
    uint32_t tph, tlen, tstep, tfree;           /* .. where they are in the 16th, its length, the 16th of the bar */
    int32_t lcx[2], lcy[2];                     /* LO-CUT */
    uint32_t wph;                               /* DUB: its wow */
    int32_t dhp;                                /* .. its high-pass */
    uint32_t rk, rbase;                         /* REVERSE: the chunk read backwards */
    int32_t lofi_v;                             /* LO-FI: the held sample */
    uint32_t lofi_c;
} mx;
static int16_t comb_buf[2][1200];

static const uint16_t SINQ[65] = {0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039,
    11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273,
    30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767};
/* a sine of a 16-bit phase, Q15 (the quarter table, read between its points) */
static int32_t sin16(uint32_t ph)
{
    uint32_t q = (ph >> 14) & 3u, x = ph & 0x3FFFu, i, f;
    int32_t v;
    if (q & 1u) x = 0x4000u - x;
    i = x >> 8;
    f = x & 255u;
    v = (int32_t)SINQ[i] + (i < 64u ? ((int32_t)(SINQ[i + 1u] - SINQ[i]) * (int32_t)f >> 8) : 0);
    return q & 2u ? -v : v;
}
static int32_t clip15(int32_t x) { return x > 32767 ? 32767 : x < -32767 ? -32767 : x; }

static void fx_reset(void)
{
    uint32_t i;
    for (i = 0; i < DLY_N; i++) dly_buf[i] = 0;
    svf_clear(&mx.flt);
    mx.gcur = 0;
    mx.dlp = 0;
    mx.env = 0;
    mx.cgain = 4096;
    mx.gate = mx.gate_t = 32767;
    svf_clear(&mx.f2);
    mx.ftype = 0xFF;
    mx.tgg = 32767;
    for (i = 0; i < 1200u; i++) comb_buf[0][i] = comb_buf[1][i] = 0;
}

static void fx_click(uint32_t accent) { mx.click_n = 1400; mx.click_ph = 0; mx.click_f = accent ? 3567 : 2378; }   /* 2400 / 1600 Hz, Q16 a frame */

/* the delay time in frames for DLY_TIME 0..5 (1/16, 1/8, 3/16, 1/4, 3/8, 1/2) at the tempo */
static int32_t fx_dly_frames(void)
{
    static const uint8_t SIXTEENTHS[6] = {1, 2, 3, 4, 6, 8};
    uint32_t t = prj.p.dly_time > 5u ? 5u : prj.p.dly_time;
    uint32_t f = 6615000u / bpm10() * SIXTEENTHS[t];  /* a sixteenth: 44100 x 60 / 4 / bpm = 6615000 / bpm x10 */
    if (f >= DLY_N - 2u) f = DLY_N - 2u;
    return (int32_t)f;
}

static const uint16_t COMB_D[128] = {17640, 17105, 16586, 16083, 15595, 15122, 14663, 14219, 13787, 13369, 12963, 12570,
    12189, 11819, 11461, 11113, 10776, 10449, 10132, 9825, 9527, 9238, 8958, 8686, 8422, 8167, 7919, 7679, 7446, 7220,
    7001, 6789, 6583, 6383, 6190, 6002, 5820, 5643, 5472, 5306, 5145, 4989, 4838, 4691, 4549, 4411, 4277, 4147, 4021,
    3899, 3781, 3666, 3555, 3447, 3343, 3241, 3143, 3048, 2955, 2866, 2779, 2694, 2613, 2533, 2457, 2382, 2310, 2240,
    2172, 2106, 2042, 1980, 1920, 1862, 1805, 1751, 1697, 1646, 1596, 1548, 1501, 1455, 1411, 1368, 1327, 1286, 1247,
    1210, 1173, 1137, 1103, 1069, 1037, 1005, 975, 945, 917, 889, 862, 836, 810, 786, 762, 739, 716, 695, 674, 653,
    633, 614, 596, 578, 560, 543, 527, 511, 495, 480, 466, 451, 438, 424, 412, 399, 387, 375, 364, 353};   /* Q4: 40 Hz .. 2 kHz */
static const uint16_t VOW_I[5][2] = {{434, 503}, {378, 594}, {261, 632}, {391, 458}, {279, 464}};   /* A E I O U, Q4 */
static const uint16_t LOCUT_A[16] = {32675, 32654, 32629, 32598, 32561, 32516, 32460, 32392, 32310, 32209, 32087, 31939,
                                     31758, 31539, 31274, 30953};   /* 20 .. 400 Hz */
static const uint8_t TG_PAT[16] = {1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 0};   /* TRANCE PATTERN: 3-3-2 */
static const char *const FT_NAME[FT_N] = {"DJ", "BAND", "COMB", "VOWEL", "WAH"};
static const char *const MOVE_NAME[8] = {"OFF", "1/16", "1/8", "1/4", "1/2", "1 BAR", "RND 16", "RND 8"};
static const char *const DT_NAME[DT_N] = {"CLEAN", "DUB", "PING", "REVERSE", "LO-FI"};

static int32_t g_of(int32_t q4)                 /* SVF_G_LP between its entries (index Q4, 0 .. 63) */
{
    int32_t i = q4 >> 4, f = q4 & 15;
    if (i < 0) return SVF_G_LP[0];
    if (i >= 63) return SVF_G_LP[63];
    return SVF_G_LP[i] + (int32_t)(((int64_t)(SVF_G_LP[i + 1] - SVF_G_LP[i]) * f) >> 4);
}
/* MOVE: the knob's offset now (-32 .. 32, knob units), from where the bar is */
static int32_t fx_move(void)
{
    uint32_t m = prj.p2.flt_move, beat = mx.beat ? mx.beat : 22050u, per, ph, p0 = mx.play ? mx.pos0 : mx.tfree;
    static const uint8_t QN[5] = {1, 2, 4, 8, 16};      /* sixteenths: 1/16, 1/8, 1/4, 1/2, a bar */
    if (!m) return 0;
    if (m >= 6u) {                                       /* RND: a new place each 16th / 8th */
        uint32_t slot = p0 / (beat / (m == 6u ? 4u : 2u) + 1u);
        if (slot != mx.mv_slot) {
            mx.mv_slot = slot;
            mx.mv_rnd = (int32_t)(hash3(slot, m, 77u) % 65u) - 32;
        }
        return mx.mv_rnd;
    }
    per = beat / 4u * QN[m - 1u];
    if (!per) return 0;
    ph = (p0 % per) * 256u / per;                         /* 0..255: a triangle */
    return (int32_t)(ph < 128u ? ph : 255u - ph) / 2 - 32;
}

/* the master on n frames of L, R (in place). Output Q15. */
static void fx_master(int32_t *L, int32_t *R, uint32_t n)
{
    uint32_t i, c, type = prj.p2.flt_type < FT_N ? prj.p2.flt_type : 0u, dtp = prj.p2.dly_type < DT_N ? prj.p2.dly_type : 0u;
    int32_t drv = prj.p.drive, cr = prj.p.crush, fk = prj.p.filter, dmix = prj.p.dly_mix, dfb = prj.p.dly_fb;
    int32_t pk = prj.p.punch, ftarget = 0, q, bits_mask = 0, dt, mv = fx_move(), lca = 0, wid = prj.p2.width;
    int32_t ck = 0, cfb = 0, vk = 0, vf1 = 0, vf2 = 0;
    if (type != mx.ftype) {                      /* another type: its state from nothing */
        svf_clear(&mx.flt);
        svf_clear(&mx.f2);
        mx.gcur = 0;
        mx.ftype = type;
        mx.fq4 = mx.fq4_to = fk * 8;
        mx.cd = mx.cd_to = COMB_D[fk & 127];
        mx.wenv = 0;
    }
    q = svf_damp(prj.p.reso);
    if (type == FT_DJ) {                         /* the knob's side, MOVE inside it; 0 = off (the middle) */
        if (fk < 60) { fk += mv; fk = fk < 0 ? 0 : fk > 59 ? 59 : fk; ftarget = SVF_G_LP[fk * 63 / 59]; }
        else if (fk > 68) { fk += mv; fk = fk < 69 ? 69 : fk > 127 ? 127 : fk; ftarget = SVF_G_HP[(fk - 69) * 63 / 58 > 63 ? 63 : (fk - 69) * 63 / 58]; }
    } else {
        int32_t k2 = fk + mv;
        k2 = k2 < 0 ? 0 : k2 > 127 ? 127 : k2;
        if (type == FT_COMB) {
            mx.cd_to = COMB_D[k2];
            cfb = 9830 + (int32_t)prj.p.reso * 1420;      /* 0.3 .. 0.95, Q15 */
        } else if (type == FT_VOWEL) {          /* A E I O U across the knob: the two formants */
            int32_t seg = k2 * 4 / 128, f = (k2 * 4) % 128;
            vf1 = VOW_I[seg][0] + ((VOW_I[seg + 1][0] - VOW_I[seg][0]) * f >> 7);
            vf2 = VOW_I[seg][1] + ((VOW_I[seg + 1][1] - VOW_I[seg][1]) * f >> 7);
            vk = (int32_t)(10000 - prj.p.reso * 560) << 10;   /* Q24: 0.6 .. 0.1 */
        } else {
            mx.fq4_to = k2 * 8;                  /* BAND, WAH: the knob over the 64 places */
            ck = type == FT_BAND ? (int32_t)(23000 - prj.p.reso * 1400) << 10 : q;
        }
    }
    if (cr) {
        uint32_t bits = 15u - (uint32_t)cr * 11u / 15u;   /* 15 .. 4 bits */
        bits_mask = (int32_t)(0xFFFFFFFFu << (16u - bits));
        mx.hold_n = 1u + (uint32_t)cr / 2u;      /* 1 .. 8: the rate divided */
    }
    if (prj.p2.locut) lca = LOCUT_A[prj.p2.locut & 15u];
    dt = fx_dly_frames();
    if (dtp == DT_PING && dt > (int32_t)(DLY_N / 2u) - 2) dt = (int32_t)(DLY_N / 2u) - 2;   /* (two lines of half a second) */
    {                                            /* TRANCE: where the 16th is */
        uint32_t len = mx.beat / 4u ? mx.beat / 4u : 5512u, p0 = mx.play ? mx.pos0 : mx.tfree;
        mx.tlen = len;
        mx.tstep = (p0 / len) & 15u;
        mx.tph = p0 % len;
        mx.tfree += n;
    }
    for (i = 0; i < n; i++) {
        int32_t x[2], d, m;
        x[0] = L[i];
        x[1] = R[i];
        for (c = 0; c < 2u; c++) {
            int32_t v = x[c];
            if (drv) {                           /* DRIVE: gain then 1.5 v - 0.5 v^3 */
                v = v * (8 + drv * 3) >> 3;
                v = v > 32767 ? 32767 : v < -32767 ? -32767 : v;
                v = (3 * v - ((v * v >> 15) * v >> 15)) >> 1;
            }
            if (cr) {                            /* CRUSH */
                if (c == 0u && ++mx.hold_c >= mx.hold_n) mx.hold_c = 0;
                if (mx.hold_c == 0u) mx.hold_v[c] = clip15(v) & bits_mask;
                v = mx.hold_v[c];
            }
            x[c] = v;
        }
        if (type == FT_DJ) {
            /* DJ: low-pass left of the middle, high-pass right of it. Off in the middle; coming in, it starts where
             * the knob is, from where the sound is (no jump); moving, it glides (every 16 samples, a quarter) */
            if (!ftarget) {
                mx.gcur = 0;
            } else if (!mx.gcur || mx.fhp != (uint32_t)(prj.p.filter > 68)) {   /* (or straight from one side to the other) */
                mx.fhp = prj.p.filter > 68;
                svf_clear(&mx.flt);
                if (!mx.fhp) { mx.flt.ic2[0] = clip15(x[0]); mx.flt.ic2[1] = clip15(x[1]); }   /* (the low-pass from where the sound is) */
                mx.gcur = ftarget;
                svf_coef(&mx.flt, mx.gcur, q);
            } else if (!(i & 15u)) {
                int32_t step = (ftarget - mx.gcur) / 4;
                mx.gcur = step ? mx.gcur + step : ftarget;
                svf_coef(&mx.flt, mx.gcur, q);
            }
            if (mx.gcur) {
                for (c = 0; c < 2u; c++) {
                    int32_t hi, lo = svf_run(&mx.flt, c, clip15(x[c]), &hi);
                    x[c] = mx.fhp ? hi : lo;
                }
            }
        } else if (type == FT_COMB) {           /* COMB: a delay of 1/f fed back, its pitch on the knob */
            uint32_t rd;
            int32_t a, b, fr;
            if (!(i & 15u)) mx.cd += (mx.cd_to - mx.cd) / 8;
            for (c = 0; c < 2u; c++) {
                rd = (mx.cw + 1200u * 2u - (uint32_t)(mx.cd >> 4) - 1u) % 1200u;
                fr = mx.cd & 15;
                a = comb_buf[c][rd];
                b = comb_buf[c][(rd + 1u) % 1200u];
                d = b + (((a - b) * fr) >> 4);
                m = clip15(x[c] + (d * cfb >> 15));
                comb_buf[c][mx.cw] = (int16_t)m;
                x[c] = (m * (32768 - cfb * 5 / 8)) >> 15;
            }
            mx.cw = mx.cw + 1u >= 1200u ? 0 : mx.cw + 1u;
        } else if (type == FT_VOWEL) {          /* VOWEL: two band-passes on the formants, a little of the sound */
            if (!(i & 15u)) { svf_coef(&mx.flt, g_of(vf1), vk); svf_coef(&mx.f2, g_of(vf2), vk); }
            {                                    /* (one voice: the formants of the middle, the sides kept dry) */
                int32_t v = clip15((x[0] + x[1]) >> 1), f;
                svf_run(&mx.flt, 0, v, 0);
                svf_run(&mx.f2, 0, v, 0);
                f = (int32_t)(((int64_t)mx.flt.bp[0] * vk) >> 24) + (int32_t)(((int64_t)mx.f2.bp[0] * vk) >> 25);
                f = f * (12 + (int32_t)prj.p.reso) / 10;   /* (narrower: louder, to keep the level) */
                x[0] = f + clip15(x[0]) / 5;
                x[1] = f + clip15(x[1]) / 5;
            }
        } else {                                 /* BAND: a band-pass on the knob; WAH: a low-pass the hits open */
            if (!(i & 15u)) {
                int32_t at = mx.fq4_to;
                if (type == FT_WAH) at += (mx.wenv >> 6) > 400 ? 400 : (mx.wenv >> 6);
                at = at > 1008 ? 1008 : at;
                mx.fq4 += (at - mx.fq4) / 4;
                svf_coef(&mx.flt, g_of(mx.fq4), ck);
            }
            if (type == FT_WAH) {
                int32_t a = x[0] < 0 ? -x[0] : x[0];
                if (a > 32767) a = 32767;
                mx.wenv += (a << 4) > mx.wenv ? ((a << 4) - mx.wenv) >> 4 : -(mx.wenv >> 11);
            }
            for (c = 0; c < 2u; c++) {
                int32_t lo = svf_run(&mx.flt, c, clip15(x[c]), 0);
                x[c] = type == FT_BAND ? (int32_t)(((int64_t)mx.flt.bp[c] * ck) >> 24) * (256 + (int32_t)prj.p.reso * 64) >> 8 : lo;
            }
        }
        /* TRANCE: the gates of two black keys (16ths, or a 3-3-2 pattern), soft edges (1.5 ms) */
        if (mx.tg || mx.tgg != 32767) {
            uint32_t open = 1;
            if (mx.tg & 1u) open = mx.tph < mx.tlen * 9u / 16u;
            if (mx.tg & 2u) open = open && TG_PAT[mx.tstep] && mx.tph < mx.tlen * 3u / 4u;
            mx.tgg += ((open ? 32767 : 0) - mx.tgg) >> 6;
            if (!mx.tg && mx.tgg > 32700) mx.tgg = 32767;
            x[0] = (int32_t)(((int64_t)x[0] * mx.tgg) >> 15);
            x[1] = (int32_t)(((int64_t)x[1] * mx.tgg) >> 15);
        }
        if (++mx.tph >= mx.tlen) { mx.tph = 0; mx.tstep = (mx.tstep + 1u) & 15u; }
        /* DELAY */
        if (!(i & 31u)) mx.dtime += (dt - mx.dtime) >> 3;
        m = (x[0] + x[1]) >> 1;
        if (dtp == DT_PING) {                    /* PING: two lines, each one's repeats into the other */
            uint32_t H = DLY_N / 2u, w = mx.dw % H, rd = (w + H - (uint32_t)(mx.dtime > 1 ? mx.dtime : 1)) % H;
            int32_t dl = dly_buf[rd], dr = dly_buf[H + rd], f = dfb * 54;
            dly_buf[w] = (int16_t)clip15(dmix ? m + (dr * f >> 10) : 0);
            dly_buf[H + w] = (int16_t)clip15(dmix ? dl * f >> 10 : 0);
            mx.dw = w + 1u >= H ? 0 : w + 1u;
            if (dmix) { x[0] += dl * dmix >> 4; x[1] += dr * dmix >> 4; }
        } else {
            uint32_t back = (uint32_t)(mx.dtime > 1 ? mx.dtime : 1), rd;
            int32_t fbk;
            if (dtp == DT_REV) {                 /* REVERSE: each chunk of the time, read backwards */
                uint32_t T = back < 128u ? 128u : back;
                if (++mx.rk >= T) { mx.rk = 0; mx.rbase = mx.dw; }
                rd = (mx.rbase + DLY_N * 2u - 1u - mx.rk) % DLY_N;
                d = dly_buf[rd];
                if (mx.rk < 64u) d = d * (int32_t)mx.rk >> 6;
                else if (T - mx.rk < 64u) d = d * (int32_t)(T - mx.rk) >> 6;
            } else if (dtp == DT_DUB) {          /* DUB: the tape's wow, read between samples */
                int32_t w = sin16(mx.wph >> 4), pos;
                uint32_t r1;
                mx.wph += 14u;                   /* (~0.6 Hz) */
                pos = (int32_t)back * 16 + (w >> 10);     /* Q4: +-2 samples */
                rd = (mx.dw + DLY_N * 2u - (uint32_t)(pos >> 4) - 1u) % DLY_N;
                r1 = rd + 1u >= DLY_N ? 0 : rd + 1u;
                d = dly_buf[r1] + (((dly_buf[rd] - dly_buf[r1]) * (pos & 15)) >> 4);
            } else {
                rd = (mx.dw + DLY_N - back) % DLY_N;
                d = dly_buf[rd];
            }
            if (dtp == DT_DUB) {                 /* darker, no mud, saturated, can run away */
                mx.dlp += (d - mx.dlp) >> 3;
                mx.dhp += (mx.dlp - mx.dhp) >> 8;
                fbk = (mx.dlp - mx.dhp) * dfb * 74 >> 10;     /* (up to 1.08) */
                fbk = fbk > 32767 ? 32767 : fbk < -32767 ? -32767 : fbk;
                fbk = (3 * fbk - ((fbk * fbk >> 15) * fbk >> 15)) >> 1;
                d = (mx.dlp - mx.dhp) * 3 / 4;
            } else if (dtp == DT_LOFI) {         /* LO-FI: a phone line, held samples, fewer bits */
                if (++mx.lofi_c >= 4u) { mx.lofi_c = 0; mx.lofi_v = d & ~0xFF; }
                mx.dlp += (mx.lofi_v - mx.dlp) >> 2;
                mx.dhp += (mx.dlp - mx.dhp) >> 5;
                d = mx.dlp - mx.dhp;
                fbk = d * dfb * 58 >> 10;
            } else {
                mx.dlp += (d - mx.dlp) >> 2;     /* darker repeats */
                fbk = mx.dlp * dfb * 54 >> 10;
            }
            m = clip15(dmix ? m + fbk : 0);
            dly_buf[mx.dw] = (int16_t)m;
            mx.dw = mx.dw + 1u >= DLY_N ? 0 : mx.dw + 1u;
            if (dmix) {
                d = d * dmix >> 4;
                x[0] += d;
                x[1] += d;
            }
        }
        /* PUNCH: follow the peak, pull it down above the threshold, add the make-up */
        if (pk) {
            int32_t a = x[0] < 0 ? -x[0] : x[0], b = x[1] < 0 ? -x[1] : x[1], thr;
            if (b > a) a = b;
            mx.env += a > mx.env ? (a - mx.env) >> 4 : (a - mx.env) >> 12;
            if (!(i & 15u)) {
                thr = 26000 - pk * 1350;
                mx.cgain = mx.env > thr ? (int32_t)(((uint32_t)thr << 12) / (uint32_t)mx.env) : 4096;
                mx.cgain = mx.cgain * (16 + pk) >> 4;
            }
            x[0] = x[0] * mx.cgain >> 12;
            x[1] = x[1] * mx.cgain >> 12;
        }
        /* LO-CUT: a high-pass on what comes out */
        if (lca) {
            for (c = 0; c < 2u; c++) {
                int32_t y = (int32_t)(((int64_t)(mx.lcy[c] + x[c] - mx.lcx[c]) * lca) >> 15);
                mx.lcx[c] = x[c];
                mx.lcy[c] = y;
                x[c] = y;
            }
        }
        /* WIDTH: the sides narrowed (0: mono) or widened (32: twice) */
        if (wid != 16) {
            int32_t md = (x[0] + x[1]) >> 1, sd = ((x[0] - x[1]) >> 1) * wid >> 4;
            x[0] = md + sd;
            x[1] = md - sd;
        }
        /* the flex gate (smoothed: ~1 ms) */
        if (mx.gate != 32767 || mx.gate_t != 32767) {
            mx.gate += (mx.gate_t - mx.gate) >> 5;
            if (mx.gate_t == 32767 && mx.gate > 32700) mx.gate = 32767;
            x[0] = x[0] * mx.gate >> 15;
            x[1] = x[1] * mx.gate >> 15;
        }
        /* the metronome: a short triangle blip */
        if (mx.click_n > 0) {
            int32_t ph = mx.click_ph & 0xFFFF, tri = ph < 32768 ? ph - 16384 : 49151 - ph, a = mx.click_n * 9;
            mx.click_ph += mx.click_f;
            tri = (tri * a) >> 15;
            x[0] += tri;
            x[1] += tri;
            mx.click_n--;
        }
        L[i] = clip15(x[0]);
        R[i] = clip15(x[1]);
        {
            int32_t a = L[i] < 0 ? -L[i] : L[i];
            if (a > live_level) live_level = a;
        }
    }
}
