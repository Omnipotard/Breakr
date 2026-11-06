/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: looking at a break (main loop, a few blocks at a time): the waveform for the screen, and per hop of
 * 256 samples three energies in a log scale (all, the lows under ~110 Hz, the highs over ~1.7 kHz). From them:
 *   GRID   COUNT equal slices
 *   AUTO   the transients (the rise of the energy and of the highs, local peaks, at least 45 ms or half a sixteenth apart; SENS
 *          sets how small a rise counts), each found again to the sample in the audio
 * and the class of every slice from its first hops (an_class): lows leading = a kick, mids and highs = a
 * snare, highs = a hat. The dice and the generators keep a kick where a kick was. */

#define AN_HOP 128u
#define AN_MAXF 8192u                           /* hops: 47 s at 22050 Hz */
#define WAVE_W 232u                             /* columns of the waveform */
static struct {
    uint32_t b;                                 /* the break looked at (0xFF: none) */
    uint32_t busy, done;
    uint32_t blk, nblk, nf;                     /* next block, blocks, hops done */
    int32_t lp1, lp2, lp3, lp4;                 /* low-passes at ~150 Hz, ~250 Hz, ~1.5 kHz, ~5 kHz */
    int32_t c1, c2, c3, c4;                     /* their coefficients, Q8, for the break's rate */
    uint32_t ae, alo, amid, ahi, an;            /* sums over the hop */
    uint8_t le[AN_MAXF], llo[AN_MAXF], lmid[AN_MAXF], lhi[AN_MAXF], od[AN_MAXF];
    uint8_t wave[WAVE_W];                       /* the peak of every column, 0..127 */
    uint32_t gen;                               /* +1 every time a look is finished (the screen redraws) */
} an POOL;
static int16_t an_pcm[IMA_SPB + 1];

/* 8 steps an octave of a power (about 0.38 dB a step) */
static uint32_t ilog8(uint32_t x)
{
    uint32_t n = 0;
    if (!x) return 0;
    while (x >= 16u) { x >>= 1; n++; }
    return n * 8u + (x >= 8u ? (x - 8u) : 0u);
}

static int32_t an_coef(uint32_t f, uint32_t rate)  /* a one-pole's Q8 coefficient for a corner at f */
{
    uint32_t c = 1608u * f / (rate ? rate : 22050u);   /* 256 x 2 pi f / rate */
    return (int32_t)(c > 230u ? 230u : c < 1u ? 1u : c);
}

static void an_start(uint32_t b)
{
    uint32_t i;
    an.b = b;
    an.done = 0;
    an.busy = b < NBRK && brk[b].data && brk[b].frames >= 64u;
    an.blk = 0;
    an.nblk = an.busy ? (brk[b].frames + IMA_SPB - 1u) / IMA_SPB : 0;
    an.nf = 0;
    an.lp1 = an.lp2 = an.lp3 = an.lp4 = 0;
    if (an.busy) {
        an.c1 = an_coef(150, brk[b].rate);
        an.c2 = an_coef(250, brk[b].rate);
        an.c3 = an_coef(1500, brk[b].rate);
        an.c4 = an_coef(5000, brk[b].rate);
    }
    an.ae = an.alo = an.amid = an.ahi = an.an = 0;
    for (i = 0; i < WAVE_W; i++) an.wave[i] = 0;
}

static void an_block(void)
{
    const brk_t *bb = &brk[an.b];
    uint32_t first = an.blk * IMA_SPB, n = bb->frames - first, i;
    if (n > IMA_SPB) n = IMA_SPB;
    ima_block(bb->data + an.blk * IMA_BLOCK, an_pcm, n, 0);
    for (i = 0; i < n; i++) {
        int32_t x = an_pcm[i], mid, hi, a = x < 0 ? -x : x;
        uint32_t col = (uint32_t)(((first + i) >> 4) * WAVE_W / ((bb->frames >> 4) + 1u));
        an.lp1 += (x - an.lp1) * an.c1 >> 8;
        an.lp2 += (x - an.lp2) * an.c2 >> 8;
        an.lp3 += (x - an.lp3) * an.c3 >> 8;
        an.lp4 += (x - an.lp4) * an.c4 >> 8;
        mid = an.lp3 - an.lp2;                  /* ~250 Hz .. 1.5 kHz */
        hi = x - an.lp4;                        /* over ~5 kHz */
        an.ae += (uint32_t)(x * x) >> 10;
        an.alo += (uint32_t)(an.lp1 * an.lp1) >> 10;
        an.amid += (uint32_t)(mid * mid) >> 10;
        an.ahi += (uint32_t)(hi * hi) >> 10;
        if (col < WAVE_W && (uint32_t)(a >> 8) > an.wave[col]) an.wave[col] = (uint8_t)(a >> 8);
        if (++an.an == AN_HOP) {
            if (an.nf < AN_MAXF) {
                an.le[an.nf] = (uint8_t)ilog8(an.ae);
                an.llo[an.nf] = (uint8_t)ilog8(an.alo);
                an.lmid[an.nf] = (uint8_t)ilog8(an.amid);
                an.lhi[an.nf] = (uint8_t)ilog8(an.ahi);
                an.nf++;
            }
            an.ae = an.alo = an.amid = an.ahi = an.an = 0;
        }
    }
    an.blk++;
}

/* the rise of a band at hop h: over the least of the 3 hops before (it does not count what still rings) */
static int32_t an_rise(const uint8_t *v, uint32_t h)
{
    int32_t m = 255, k, best = 0;
    for (k = 1; k <= 3; k++) if (h >= (uint32_t)k && v[h - (uint32_t)k] < m) m = v[h - (uint32_t)k];
    if (m == 255) m = 0;
    for (k = 0; k < 3 && h + (uint32_t)k < an.nf; k++) if ((int32_t)v[h + (uint32_t)k] - m > best) best = (int32_t)v[h + (uint32_t)k] - m;
    return best;
}

/* the onset strength of every hop: the rise of the highs and the mids, and of the lows (kicks) */
static void an_onsets(void)
{
    uint32_t h;
    for (h = 0; h < an.nf; h++) {
        int32_t d = 0, m = 255, k;
        for (k = 1; k <= 2; k++) if (h >= (uint32_t)k) {
            int32_t a = an.lhi[h - (uint32_t)k], b2 = an.lmid[h - (uint32_t)k], c = an.llo[h - (uint32_t)k];
            (void)m;
            d = 0;
            if ((int32_t)an.lhi[h] - a > d) d = (int32_t)an.lhi[h] - a;
            if ((int32_t)an.lmid[h] - b2 > d) d = (int32_t)an.lmid[h] - b2;
            if ((int32_t)an.llo[h] - c > d) d = (int32_t)an.llo[h] - c;
            if (k == 1) m = d; else if (d < m) m = d;   /* risen over both hops before */
        }
        an.od[h] = (uint8_t)(h < 2u ? 0 : m < 0 ? 0 : m > 255 ? 255 : m);
    }
}

/* run the look for about `blocks` blocks; 1 when it is finished */
static uint32_t an_poll(uint32_t blocks)
{
    if (!an.busy) return an.done;
    while (blocks-- && an.blk < an.nblk) an_block();
    if (an.blk >= an.nblk) {
        an_onsets();
        an.busy = 0;
        an.done = 1;
        an.gen++;
    }
    return an.done;
}

/* the class of a slice that starts at frame s: what rose at its start (against what rang before; at most
 * 24 steps, ~9 dB, so a hit out of silence does not count more than one over a groove) and how loud each band
 * is then. Lows over mids and highs far under them: a kick; the highs and the mids rising, or mids over the
 * lows: a snare; the highs: a hat. (Tuned on the factory breaks, the recorded ones too.) */
static uint32_t an_class(uint32_t s, uint32_t e)
{
    uint32_t h = s / AN_HOP + 1u, k;
    int32_t dlo, dmid, dhi, llo = 0, lmid = 0, lhi = 0;
    (void)e;
    if (h >= an.nf) return CL_X;
    dlo = an_rise(an.llo, h); dlo = dlo > 24 ? 24 : dlo;
    dmid = an_rise(an.lmid, h); dmid = dmid > 24 ? 24 : dmid;
    dhi = an_rise(an.lhi, h); dhi = dhi > 24 ? 24 : dhi;
    for (k = h; k < h + 4u && k < an.nf; k++) {
        if (an.llo[k] > llo) llo = an.llo[k];
        if (an.lmid[k] > lmid) lmid = an.lmid[k];
        if (an.lhi[k] > lhi) lhi = an.lhi[k];
    }
    if (dlo >= 10 && llo >= lmid && lhi + 60 < llo) return CL_K;   /* the lows rose, and they lead */
    if ((dhi >= 8 && dmid >= 20) || (lmid >= llo + 5 && dmid >= 10)) return CL_S;
    if (dhi >= 2) return CL_H;
    return dlo > dhi ? CL_K : CL_H;
}

/* the sample where the rise of hop h starts: the first sample over a quarter of the hop's peak */
static uint32_t an_refine(uint32_t h)
{
    const brk_t *bb = &brk[an.b];
    uint32_t from = h ? (h - 1u) * AN_HOP : 0, to = (h + 1u) * AN_HOP, f, peak = 0, blk = 0xFFFFFFFFu;
    if (to > bb->frames) to = bb->frames;
    for (f = from; f < to; f++) {
        int32_t x;
        if (f / IMA_SPB != blk) {
            blk = f / IMA_SPB;
            ima_block(bb->data + blk * IMA_BLOCK, an_pcm, bb->frames - blk * IMA_SPB, 0);
        }
        x = an_pcm[f - blk * IMA_SPB];
        if ((uint32_t)(x < 0 ? -x : x) > peak) peak = (uint32_t)(x < 0 ? -x : x);
    }
    blk = 0xFFFFFFFFu;
    for (f = from; f < to; f++) {
        int32_t x;
        if (f / IMA_SPB != blk) {
            blk = f / IMA_SPB;
            ima_block(bb->data + blk * IMA_BLOCK, an_pcm, bb->frames - blk * IMA_SPB, 0);
        }
        x = an_pcm[f - blk * IMA_SPB];
        if ((uint32_t)(x < 0 ? -x : x) * 4u > peak) break;
    }
    f = f > from + 24u ? f - 24u : from;        /* ~1 ms before: the attack whole */
    return f;
}

/* slices into sl / cls (ascending, the first at 0); returns the count */
static uint32_t an_slices(uint32_t mode, uint32_t count, uint32_t sens, uint32_t *sl, uint8_t *cls)
{
    const brk_t *bb = &brk[an.b];
    uint32_t n = 0, i, k;
    if (!an.done || an.b >= NBRK || !bb->data) return 0;
    if (mode == SLM_GRID) {
        if (count < 2u) count = 2u;
        if (count > NSL) count = NSL;
        for (i = 0; i < count; i++) sl[i] = bb->frames * i / count;
        n = count;
    } else {
        uint32_t nf = an.nf, mx = 0, thr, gap, cand = 0, last = 0;
        uint32_t pos[NSL], str[NSL];
        if (sens < 1u) sens = 1u;
        if (sens > 10u) sens = 10u;
        thr = 24u - 2u * sens;                  /* SENS 1: a rise of 8 dB .. SENS 10: 1.5 dB */
        gap = bb->rate / 22u / AN_HOP + 1u;     /* ~45 ms, or half a sixteenth if longer (slow breaks ring) */
        if (bb->bpm10 >= 300u) {
            uint32_t half16 = bb->rate * 75u / bb->bpm10 / AN_HOP;   /* rate x 60 / (bpm x 8) */
            if (half16 > gap) gap = half16;
        }
        for (i = 0; i < nf; i++) if (an.le[i] > mx) mx = an.le[i];
        pos[0] = 0;
        str[0] = 255;
        cand = 1;
        for (i = 2; i + 1u < nf; i++) {
            uint32_t o = an.od[i];
            if (o < thr || o < an.od[i - 1] || o < an.od[i + 1] || an.le[i] + 60u < mx) continue;
            if (i - last < gap) {               /* too close: the stronger of the two */
                if (cand > 1u && o > str[cand - 1u]) { pos[cand - 1u] = i; str[cand - 1u] = o; last = i; }
                continue;
            }
            if (cand < NSL) {
                pos[cand] = i;
                str[cand] = o;
                cand++;
            } else {                            /* full: replace the weakest if this one is stronger */
                uint32_t w = 1;
                for (k = 2; k < NSL; k++) if (str[k] < str[w]) w = k;
                if (o <= str[w]) continue;
                for (k = w; k + 1u < NSL; k++) { pos[k] = pos[k + 1u]; str[k] = str[k + 1u]; }
                pos[NSL - 1u] = i;
                str[NSL - 1u] = o;
            }
            last = i;
        }
        sl[0] = 0;
        n = 1;
        for (i = 1; i < cand; i++) {
            uint32_t f = an_refine(pos[i]);
            if (f > sl[n - 1u] + bb->rate / 40u) sl[n++] = f;   /* 25 ms apart at least */
        }
    }
    for (i = 0; i < n; i++)
        cls[i] = (uint8_t)an_class(sl[i], i + 1u < n ? sl[i + 1u] : bb->frames);
    return n;
}

/* the tempo of a break of `bars` bars, x10 */
static uint32_t brk_bpm10(uint32_t b, uint32_t bars)
{
    if (b >= NBRK || !brk[b].frames) return 1200;
    return 2400u * bars * (brk[b].rate / 10u) / (brk[b].frames / 10u + 1u);
}
