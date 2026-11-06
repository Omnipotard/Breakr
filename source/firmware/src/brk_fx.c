/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: the master (audio interrupt), in this order: DRIVE (a soft clip), CRUSH (bits and sample rate),
 * the DJ FILTER (one knob: low-pass to the left, high-pass to the right, off in the middle; a Chamberlin
 * state-variable filter run twice a sample), the DELAY (tempo-synced, a low-pass in its feedback), PUNCH (a
 * compressor with its make-up gain), then the flex gate and the metronome. Integers only. */

/* filter coefficient 2 sin(pi fc / 88200) in Q14: low-pass 60 Hz .. 12 kHz, high-pass 20 Hz .. 8 kHz, 64 steps each */
static const uint16_t FLT_LP[64] = {70, 76, 83, 90, 98, 107, 116, 126, 137, 149, 162, 177, 192, 209, 227, 247, 269,
    293, 318, 346, 376, 410, 445, 485, 527, 573, 624, 678, 738, 802, 873, 949, 1033, 1123, 1222, 1329, 1445, 1572,
    1710, 1860, 2023, 2200, 2393, 2602, 2830, 3078, 3347, 3639, 3957, 4302, 4677, 5085, 5527, 6006, 6526, 7090, 7701,
    8362, 9078, 9850, 10684, 11582, 12547, 13583};
static const uint16_t FLT_HP[64] = {23, 26, 28, 31, 34, 38, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 129,
    142, 156, 172, 189, 208, 229, 252, 277, 304, 335, 368, 405, 445, 490, 538, 592, 651, 716, 788, 866, 953, 1048,
    1152, 1267, 1393, 1532, 1685, 1853, 2038, 2240, 2464, 2709, 2978, 3275, 3600, 3958, 4350, 4781, 5254, 5773, 6343,
    6966, 7649, 8396, 9211};

#define DLY_N 44100u                            /* 1 s of delay line */
static int16_t dly_buf[DLY_N] POOL;
static struct {
    int32_t lo[2], bd[2], f, q, fcur;           /* filter: low and band of each channel, coefficient, damping */
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
    mx.lo[0] = mx.lo[1] = mx.bd[0] = mx.bd[1] = 0;
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
    /* the filter's coefficient: smoothed toward the knob, 0 = off */
    if (fk < 60) ftarget = FLT_LP[fk > 59 ? 63 : fk * 63 / 59];
    else if (fk > 68) ftarget = FLT_HP[(fk - 69) * 63 / 58 > 63 ? 63 : (fk - 69) * 63 / 58];
    else ftarget = 0;
    q = 32768 - (int32_t)prj.p.reso * 1950;      /* Q14 damping: 2.0 .. 0.21 */
    if (q < 3400) q = 3400;
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
        /* FILTER: low-pass left of the middle, high-pass right of it, twice a sample */
        if (!ftarget) {
            mx.fcur = 0;
        } else if (!(i & 15u)) {
            if (!mx.fcur) mx.lo[0] = mx.lo[1] = mx.bd[0] = mx.bd[1] = 0;
            mx.fcur += (ftarget - mx.fcur) >> 2;
            if (!mx.fcur) mx.fcur = ftarget;
        }
        if (mx.fcur) {
            for (c = 0; c < 2u; c++) {
                int32_t in = clip15(x[c]), hi = 0, k;
                for (k = 0; k < 2; k++) {
                    mx.lo[c] += mx.fcur * mx.bd[c] >> 14;
                    hi = in - mx.lo[c] - (q * mx.bd[c] >> 14);
                    mx.bd[c] += mx.fcur * hi >> 14;
                    mx.bd[c] = mx.bd[c] > 98000 ? 98000 : mx.bd[c] < -98000 ? -98000 : mx.bd[c];
                    mx.lo[c] = mx.lo[c] > 98000 ? 98000 : mx.lo[c] < -98000 ? -98000 : mx.lo[c];
                }
                x[c] = fk < 64 ? mx.lo[c] : hi;
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
