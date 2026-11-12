/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: the voices (audio interrupt). A voice plays a slice of a break: it reads the IMA blocks where they
 * are, decodes the block it enters into its own cache (505 samples + the next block's first, for the linear
 * interpolation), and moves at a speed in Q16 frames of the break per frame of output (the break's rate over
 * 44100, the pitch, the octave and scratch effects). Its envelope: a short attack (no click), a hold, a decay,
 * then a release of 3 ms at the slice's end or its LENGTH, or when the next slice chokes it. */

typedef struct {
    uint8_t on, b, k, rel;                      /* playing; break; slice (0-based); releasing */
    uint8_t rev, twin, key, live;               /* backwards; a unison twin; the key that holds it (gate); from a key */
    int32_t pos, frac;                          /* frame in the break, Q16 fraction */
    int32_t inc;                                /* Q16 frames per output frame at speed 1, > 0 */
    int32_t lo, hi;                             /* the region played [lo, hi) */
    int32_t stop;                               /* the frame where the release starts (LENGTH) */
    int32_t amp, att, rstep;                    /* Q15 envelope; attack step; release step (per frame) */
    int32_t dcoef;                              /* Q15 decay factor per 16 frames (32768: none) */
    int32_t gl, gr;                             /* Q12 gain left / right (accent, slice gain, pan) */
    int32_t cb;                                 /* cached block (-1: none) */
    uint32_t age;
    int16_t pcm[IMA_SPB + 1];
} voice_t;
static voice_t vo[NVOICE] POOL;
static uint32_t vo_age;

static void vo_cache(voice_t *v, int32_t blk)
{
    const brk_t *bb = &brk[v->b];
    uint32_t nblk = (bb->frames + IMA_SPB - 1u) / IMA_SPB, first = (uint32_t)blk * IMA_SPB;
    ima_block(bb->data + (uint32_t)blk * IMA_BLOCK, v->pcm, bb->frames - first,
              (uint32_t)blk + 1u < nblk ? bb->data + ((uint32_t)blk + 1u) * IMA_BLOCK : 0);
    v->cb = blk;
}

/* release: the voice fades in ~3 ms and stops */
static void vo_release(voice_t *v, uint32_t frames)
{
    if (!v->on || v->rel) return;
    v->rel = 1;
    v->rstep = v->amp / (int32_t)(frames ? frames : 1u) + 1;
}

/* the voice to use: a free one, else the oldest */
static voice_t *vo_alloc(void)
{
    uint32_t i, best = 0, oldest = 0xFFFFFFFFu;
    for (i = 0; i < NVOICE; i++) {
        if (!vo[i].on) return &vo[i];
        if (vo[i].age < oldest) { oldest = vo[i].age; best = i; }
    }
    return &vo[best];
}

/* Q15 decay factor per 16 frames for DECAY 0..14 (15: none): from ~25 ms to ~2.5 s to -60 dB */
static const uint16_t DECAY_Q15[15] = {29642, 30487, 31110, 31566, 31898, 32140, 32315, 32441, 32532,
                                       32598, 32646, 32680, 32705, 32722, 32735};

/* start slice k of break b. speed: Q16 multiplier of the pitch (1 = 65536); gain Q12; pan -64..64 */
static void vo_start(uint32_t b, uint32_t k, uint32_t speed, uint32_t rev, int32_t gain, int32_t pan,
                     uint32_t twin, uint32_t key, uint32_t live, uint32_t skip)
{
    voice_t *v;
    const brk_t *bb;
    int32_t lo, hi, len16, a;
    uint32_t n = sl_count(b);
    if (b >= NBRK || !brk[b].data || k >= n || brk[b].frames < 4u) return;
    bb = &brk[b];
    lo = (int32_t)sl_start(b, k);
    hi = (int32_t)sl_end(b, k);
    if (hi - lo < 8) return;
    {                                           /* skip: output frames already gone (a rest continues the slice) */
        uint32_t inc = ((bb->rate << 10) / FS) * (speed >> 2) >> 8;
        int32_t sk = skip ? (int32_t)(((uint64_t)skip * (inc < 64u ? 64u : inc)) >> 16) : 0;
        if (skip && (sk < 0 || sk >= hi - lo - 64)) return;
        if (sk) { if (rev & 1u) hi -= sk; else lo += sk; }
    }
    v = vo_alloc();
    v->on = 0;                                  /* (the interrupt owns it: nothing reads it half set) */
    v->b = (uint8_t)b;
    v->k = (uint8_t)k;
    v->rel = 0;
    v->rev = (uint8_t)(rev & 1u);
    v->twin = (uint8_t)twin;
    v->key = (uint8_t)key;
    v->live = (uint8_t)live;
    v->lo = lo;
    v->hi = hi;
    len16 = (int32_t)(prj.p.length < 1u ? 1u : prj.p.length > 16u ? 16u : prj.p.length);
    if (len16 >= 16) v->stop = -1;
    else v->stop = (hi - lo) * len16 / 16;      /* frames from the start, either way */
    v->pos = v->rev ? hi - 2 : lo;
    v->frac = 0;
    /* Q16 frames of the break per output frame: rate / 44100 x speed */
    v->inc = (int32_t)(((bb->rate << 10) / FS) * (speed >> 2) >> 8);
    if (v->inc < 64) v->inc = 64;
    a = (int32_t)prj.p.attack;
    a = a ? a * a * 18 + 22 : 22;               /* frames: 0.5 ms .. 92 ms */
    v->amp = 0;
    v->att = 32767 / a + 1;
    v->dcoef = prj.p.decay >= 15u ? 32768 : DECAY_Q15[prj.p.decay];
    if (pan < -64) pan = -64;
    if (pan > 64) pan = 64;
    v->gl = gain * (64 - pan) >> 6;
    v->gr = gain * (64 + pan) >> 6;
    v->cb = -1;
    v->age = ++vo_age;
    v->on = 1;
}

/* render n frames of every voice into L, R (added). sp0: Q16 speed multiplier at the first frame, dsp its
 * change per frame (scratch, tape stop); a negative speed plays backwards. */
static void vo_render(int32_t *L, int32_t *R, uint32_t n, int32_t sp0, int32_t dsp)
{
    uint32_t i, j;
    uint32_t sounding = 0;
    for (j = 0; j < NVOICE; j++) {
        voice_t *v = &vo[j];
        const brk_t *bb = &brk[v->b];
        int32_t sp = sp0, frames;
        if (!v->on) continue;
        if (!bb->data) { v->on = 0; continue; }
        frames = (int32_t)bb->frames;
        for (i = 0; i < n; i++, sp += dsp) {
            int32_t idx, s0, s1, s, step, einc, done;
            /* the speed of this frame: inc x sp (both Q16), direction of the voice */
            einc = (v->inc >> 4) * (sp >> 4) >> 8;
            if (v->rev) einc = -einc;
            idx = v->pos - v->cb * (int32_t)IMA_SPB;
            if (v->cb < 0 || (uint32_t)idx >= IMA_SPB) {
                if (v->pos < 0 || v->pos >= frames) { v->on = 0; break; }
                vo_cache(v, v->pos / (int32_t)IMA_SPB);
                idx = v->pos - v->cb * (int32_t)IMA_SPB;
            }
            s0 = v->pcm[idx];
            s1 = v->pcm[idx + 1];
            s = s0 + ((s1 - s0) * (v->frac >> 2) >> 14);
            /* the envelope */
            if (v->rel) {
                v->amp -= v->rstep;
                if (v->amp <= 0) { v->on = 0; break; }
            } else if (v->att) {
                v->amp += v->att;
                if (v->amp >= 32767) { v->amp = 32767; v->att = 0; }
            } else if (v->dcoef < 32768 && !(i & 15u)) {
                v->amp = v->amp * v->dcoef >> 15;
                if (v->amp < 40) { v->on = 0; break; }
            }
            s = s * v->amp >> 15;
            L[i] += s * v->gl >> 12;
            R[i] += s * v->gr >> 12;
            /* move */
            v->frac += einc;
            step = v->frac >> 16;
            v->pos += step;
            v->frac -= step * 65536;
            /* past the end of the slice: a 1.5 ms release, reading on (the break goes on there: no click);
             * at LENGTH: 3 ms */
            if (!v->rel) {
                done = einc >= 0 ? v->pos >= v->hi : v->pos < v->lo;
                if (done) vo_release(v, 64);
                else if (v->stop >= 0 && (v->rev ? (v->hi - v->pos) >= v->stop : (v->pos - v->lo) >= v->stop))
                    vo_release(v, 132);
            }
            if (v->pos < 0 || v->pos >= frames - 1) { v->on = 0; break; }
        }
        if (v->on && !v->twin && v->k < 32u) sounding |= 1u << v->k;
    }
    sl_sounding = sounding;
}

/* every voice from a key (gate mode: key k released), or all of them (k = 0xFF), fade out */
static void vo_off_key(uint32_t key)
{
    uint32_t j;
    for (j = 0; j < NVOICE; j++)
        if (vo[j].on && (key == 0xFFu || (vo[j].live && vo[j].key == key)))
            vo_release(&vo[j], 220);
}
static void vo_choke(void)                      /* the next slice cuts the last one (CHOKE on) */
{
    uint32_t j;
    for (j = 0; j < NVOICE; j++)
        if (vo[j].on) vo_release(&vo[j], 88);
}
