/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: the master (audio interrupt), in this order: DRIVE (a soft clip), CRUSH (bits and sample rate),
 * the DJ FILTER (one knob: low-pass to the left, high-pass to the right, off in the middle; the trapezoidal
 * state-variable filter below), the DELAY (tempo-synced, a low-pass in its feedback), PUNCH (a
 * compressor with its make-up gain), then the flex gate and the metronome. Integers only. */

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

typedef struct { int32_t g, k, a1, a2, a3; int32_t ic1[2], ic2[2]; } svf_t;

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
} mx;

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

/* the master on n frames of L, R (in place). Output Q15. */
static void fx_master(int32_t *L, int32_t *R, uint32_t n)
{
    uint32_t i, c;
    int32_t drv = prj.p.drive, cr = prj.p.crush, fk = prj.p.filter, dmix = prj.p.dly_mix, dfb = prj.p.dly_fb;
    int32_t pk = prj.p.punch, ftarget, q, bits_mask = 0, dt;
    /* the filter's cutoff: the knob's, 0 = off (the middle) */
    if (fk < 60) ftarget = SVF_G_LP[fk * 63 / 59];
    else if (fk > 68) ftarget = SVF_G_HP[(fk - 69) * 63 / 58 > 63 ? 63 : (fk - 69) * 63 / 58];
    else ftarget = 0;
    q = svf_damp(prj.p.reso);
    if (cr) {
        uint32_t bits = 15u - (uint32_t)cr * 11u / 15u;   /* 15 .. 4 bits */
        bits_mask = (int32_t)(0xFFFFFFFFu << (16u - bits));
        mx.hold_n = 1u + (uint32_t)cr / 2u;      /* 1 .. 8: the rate divided */
    }
    dt = fx_dly_frames();
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
        /* FILTER: low-pass left of the middle, high-pass right of it. Off in the middle; coming in, it starts where
         * the knob is, from where the sound is (no jump); moving, it glides (every 16 samples, a quarter of the way) */
        if (!ftarget) {
            mx.gcur = 0;
        } else if (!mx.gcur || mx.fhp != (fk > 68)) {   /* (or straight from one side to the other) */
            mx.fhp = fk > 68;
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
        /* DELAY (mono, mixed back in both) */
        if (!(i & 31u)) mx.dtime += (dt - mx.dtime) >> 3;
        {
            uint32_t rd = (mx.dw + DLY_N - (uint32_t)(mx.dtime > 1 ? mx.dtime : 1)) % DLY_N;
            d = dly_buf[rd];
            mx.dlp += (d - mx.dlp) >> 2;         /* darker repeats */
            m = (x[0] + x[1]) >> 1;
            m = clip15(dmix ? m + (mx.dlp * dfb * 54 >> 10) : 0);
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
