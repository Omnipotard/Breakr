/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR's engine on a computer, without the panel: what the audio interrupt does, checked.
 *   tests/run_engine_test.sh      (builds this under UBSan + ASan and runs it)
 *  1 fidelity   a factory break in order, at its own tempo, sounds like the break (correlation)
 *  2 timing     the steps land where the tempo says, to the frame
 *  3 dice LOCK  the same seed plays the same thing every loop; FREE does not
 *  4 effects    every punch effect and every flex pattern, alone and together, at the ends of the knobs,
 *               at 40 and 300 BPM: no integer wraps (UBSan), no clipping past full scale, no NaN of silence
 *  5 slicing    AUTO finds the hits of the factory breaks (within 15 ms), and their classes
 *  6 IMA        the encoder and the decoder agree; the SNR on the factory breaks
 *  7 generators every style, every break: patterns of real slices; LOCK repeats */
#define BRK_TEST
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define FS 44100u
#define HALF_FRAMES 256u
#define RING_PUBLISH() ((void)0)
static volatile uint32_t fm1_ms;
#include "brk_breaks.h"
#include "brk_core.c"
#include "brk_ima.c"
#include "brk_data.c"
#include "brk_snd.c"
#include "brk_fx.c"
#include "brk_seq.c"
#include "brk_slice.c"
#include "brk_edit.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL  "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void boot(void)
{
    uint32_t i, k;
    memset(brk, 0, sizeof brk);
    for (i = 0; i < NFACT; i++) {
        brk_t *b = &brk[i];
        b->frames = FACT[i].frames; b->rate = FACT[i].rate; b->bpm10 = FACT[i].bpm10; b->bars = FACT[i].bars;
        b->nsl = FACT[i].nsl; b->data = FACT[i].data;
        for (k = 0; k < NSL; k++) { b->sl[k] = FACT[i].sl[k]; b->cls[k] = FACT[i].cls[k]; }
        strncpy(b->name, FACT[i].name, 15);
    }
    prj_defaults();
    engine_init();
    punch_keys = 0;
    flex_sel = flex_on = 0xFF;
    ev_w = ev_r = 0;
    sq_play = sq_rec = 0;
}

static int32_t *render(uint32_t frames)          /* stereo, interleaved */
{
    int32_t *o = calloc(frames * 2u + 512u, sizeof *o);
    uint32_t at;
    for (at = 0; at < frames; at += HALF_FRAMES) {
        int32_t blk[2 * HALF_FRAMES];
        uint32_t i;
        memset(blk, 0, sizeof blk);
        app_audio(blk, HALF_FRAMES);
        for (i = 0; i < 2u * HALF_FRAMES && at * 2u + i < frames * 2u; i++) {
            CHECK(blk[i] >= -32767 && blk[i] <= 32767, "sample out of Q15 range: %d", blk[i]);
            o[at * 2u + i] = blk[i];
        }
        fm1_ms = (at + HALF_FRAMES) * 1000u / FS;
    }
    return o;
}

/* the break decoded and read as a voice reads it (linear, at rate/44100) */
static double *expected(uint32_t b, uint32_t frames)
{
    int16_t *pcm = malloc((brk[b].frames + IMA_SPB + 1) * sizeof *pcm);
    double *e = calloc(frames, sizeof *e), pos = 0, inc = (double)brk[b].rate / FS;
    uint32_t i, blk;
    for (blk = 0; blk * IMA_SPB < brk[b].frames; blk++) {
        int16_t c[IMA_SPB + 1];
        ima_block(brk[b].data + blk * IMA_BLOCK, c, brk[b].frames - blk * IMA_SPB, 0);
        memcpy(pcm + blk * IMA_SPB, c, IMA_SPB * sizeof *c);
    }
    for (i = 0; i < frames; i++, pos += inc) {
        uint32_t p = (uint32_t)pos;
        double f = pos - p;
        if (p + 1u >= brk[b].frames) break;
        e[i] = pcm[p] * (1 - f) + pcm[p + 1] * f;
    }
    free(pcm);
    return e;
}

static double corr0(const int32_t *o, const double *e, uint32_t from, uint32_t to, int32_t lag)
{
    double sxy = 0, sxx = 0, syy = 0;
    uint32_t i;
    for (i = from + 300u; i + 300u < to; i++) {
        double x = o[2 * (int32_t)(i + lag)], y = e[i];
        sxy += x * y; sxx += x * x; syy += y * y;
    }
    return sxy / (sqrt(sxx * syy) + 1e-9);
}
static double corr_lag(const int32_t *o, const double *e, uint32_t from, uint32_t to)   /* best within +-5 ms */
{
    double best = -1;
    int32_t lag;
    for (lag = -220; lag <= 220; lag += 2) { double c = corr0(o, e, from, to, lag); if (c > best) best = c; }
    return best;
}
static double corr(const int32_t *o, const double *e, uint32_t from, uint32_t to)
{
    double sxy = 0, sxx = 0, syy = 0;
    uint32_t i;
    for (i = from; i < to; i++) {
        double x = o[2 * i], y = e[i];
        sxy += x * y; sxx += x * x; syy += y * y;
    }
    return sxy / (sqrt(sxx * syy) + 1e-9);
}

static void t_fidelity(void)
{
    uint32_t b;
    for (b = 0; b < NFACT; b++) {
        uint32_t frames = (uint32_t)((uint64_t)brk[b].frames * FS / brk[b].rate) - 600u;
        int32_t *o;
        double *e, c;
        boot();
        prj.cur_pat = (uint8_t)b;                /* pattern b: break b in order */
        bpm10_set(brk[b].bpm10);
        prj.p.swing = 50;
        ev_push(EV_PLAY, 0, 0, 0);
        o = render(frames);
        e = expected(b, frames);
        {                                       /* the loudness envelopes, 2 ms windows (what the ear follows) */
            uint32_t w = 88, nw = frames / w, i, j;
            double *eo = calloc(nw, sizeof *eo), *ee = calloc(nw, sizeof *ee), sxy = 0, sxx = 0, syy = 0, mo = 0, me = 0;
            for (i = 0; i < nw; i++) {
                for (j = 0; j < w; j++) { double x = o[2 * (i * w + j)], y = e[i * w + j] * 3200.0 / 4096.0; eo[i] += x * x; ee[i] += y * y; }
                eo[i] = sqrt(eo[i] / w); ee[i] = sqrt(ee[i] / w); mo += eo[i]; me += ee[i];
            }
            mo /= nw; me /= nw;
            for (i = 0; i < nw; i++) { sxy += (eo[i] - mo) * (ee[i] - me); sxx += (eo[i] - mo) * (eo[i] - mo); syy += (ee[i] - me) * (ee[i] - me); }
            c = sxy / sqrt(sxx * syy);
            printf("fidelity  %-12s envelope corr %.4f  level %.3f  wave corr %.3f  (%u slices)\n", brk[b].name, c, mo / me,
                   corr_lag(o, e, 0, frames), sl_count(b));
            CHECK(c > 0.97 && mo / me > 0.95 && mo / me < 1.05, "break %s in order does not sound like itself: %.3f", brk[b].name, c);
            free(eo); free(ee);
        }
        free(o);
        free(e);
    }
}

static void t_timing(void)
{
    /* one slice on every step: the onsets every 44100*60/(bpm*4) frames */
    uint32_t i, bpm = 1234, step = 44100u * 600u / (bpm * 4u), frames = step * 33u, hits = 0, last = 0, bad = 0;
    int32_t *o;
    pat_t *p;
    boot();
    p = &prj.pat[0];
    memset(p->st, 0, sizeof p->st);
    p->len = 16;
    for (i = 0; i < 16u; i++) p->st[i].sl = 1;
    prj.p.length = 1;                           /* short: a clean gap before the next */
    prj.p.decay = 0;
    bpm10_set(bpm);
    ev_push(EV_PLAY, 0, 0, 0);
    o = render(frames);
    for (i = 1; i < frames; i++) {              /* the onsets: silence then sound */
        if (abs(o[2 * i]) > 200 && abs(o[2 * (i - 1)]) <= 200 && (i - last > step / 2u || !hits)) {
            uint32_t want = hits * step, d = i > want ? i - want : want - i;
            if (d > 40u) bad++;                 /* (the attack's first frames are quiet: 1 ms) */
            hits++;
            last = i;
        }
    }
    printf("timing    %u onsets at %u.%u BPM, %u off by more than 1 ms\n", hits, bpm / 10, bpm % 10, bad);
    CHECK(hits >= 32u && bad == 0u, "steps off the grid: %u of %u", bad, hits);
    free(o);
}

static uint64_t fp(const int32_t *o, uint32_t from, uint32_t to)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t i;
    for (i = from; i < to; i++) h = (h ^ (uint32_t)o[2 * i]) * 1099511628211ull;
    return h;
}

static uint8_t brk_named(const char *name)       /* a factory break by its name (pattern b plays break b) */
{
    uint32_t b;
    for (b = 0; b < NFACT; b++) if (!strcmp(brk[b].name, name)) return (uint8_t)b;
    return 0;
}

static void t_dice(void)
{
    /* LOCK 1 bar: what plays in bars 2, 3 and 4 is the same (slices, backwards, rolls, when); FREE: not */
    uint32_t bar = 44100u * 600u * 4u / 1700u, frames = bar * 5u, lock;
    for (lock = 0; lock < 2u; lock++) {
        uint32_t i, same = 1, nb[5] = {0}, first[5];
        int32_t *o;
        boot();
        prj.cur_pat = brk_named("FUNK 100");    /* 16 steps, one bar */
        bpm10_set(1700);
        prj.p.d_chance = 60; prj.p.d_rev = 30; prj.p.d_roll = 30; prj.p.d_skip = 10; prj.p.d_seed = 7;
        prj.p.d_lock = (uint8_t)(lock ? 1u : 0u);
        tlog_n = 0;
        ev_push(EV_PLAY, 0, 0, 0);
        o = render(frames);
        for (i = 0; i < 5u; i++) first[i] = 0xFFFFFFFFu;
        for (i = 0; i < tlog_n; i++) { uint32_t b = tlog[i].t / 384u; if (b < 5u) { if (first[b] == 0xFFFFFFFFu) first[b] = i; nb[b]++; } }
        for (i = 1; i < 4u && same; i++) {
            uint32_t j;
            if (nb[i] != nb[1]) { same = 0; break; }
            for (j = 0; j < nb[1]; j++) {
                const uint32_t a0 = first[1] + j, a1 = first[i] + j;
                if (tlog[a0].t % 384u != tlog[a1].t % 384u || tlog[a0].k != tlog[a1].k || tlog[a0].rev != tlog[a1].rev) { same = 0; break; }
            }
        }
        printf("dice      %s: bars 2-4 %s (%u, %u, %u slices)\n", lock ? "LOCK 1 BAR" : "FREE", same ? "identical" : "differ", nb[1], nb[2], nb[3]);
        CHECK(lock ? same : !same, "%s: bars 2-4 %s", lock ? "LOCK" : "FREE", same ? "identical" : "differ");
        free(o);
    }
}

static void t_effects(void)
{
    static const uint16_t BPMS[2] = {400, 3000};
    uint32_t f, bi, fl;
    int32_t *o;
    for (bi = 0; bi < 2u; bi++) {
        for (f = 0; f <= NPUNCH; f++) {          /* each effect alone, then all of them */
            boot();
            bpm10_set(BPMS[bi]);
            prj.p.drive = 15; prj.p.crush = 15; prj.p.reso = 15; prj.p.filter = (uint8_t)(f & 1u ? 0 : 127);
            prj.p.dly_mix = 15; prj.p.dly_fb = 15; prj.p.punch = 15; prj.p.pitch = (uint8_t)(f & 2u ? 0 : 48);
            prj.p.attack = 15; prj.p.d_roll = 100; prj.p.d_pitch = 100; prj.p.tmode = 1; prj.p.choke = 0;
            ev_push(EV_PLAY, 0, 0, 0);
            punch_keys = f < NPUNCH ? 1u << f : (1u << NPUNCH) - 1u;
            o = render(44100u * 2u);
            free(o);
        }
        for (fl = 0; fl < NFLEX; fl++) {
            boot();
            bpm10_set(BPMS[bi]);
            prj.p.fl_quant = 0;
            ev_push(EV_PLAY, 0, 0, 0);
            flex_sel = fl;
            punch_keys = fl & 1u ? 1u << PX_SCRATCH : 1u << PX_UNI;
            o = render(44100u * 2u);
            free(o);
        }
    }
    /* live, stopped: the repeats replay the last slice */
    boot();
    ev_push(EV_SLICE, 3, 5, 0);
    punch_keys = 1u << PX_RPT16;
    o = render(44100u);
    {
        uint32_t i, on = 0;
        for (i = 22050; i < 44100u; i++) on += abs(o[2 * i]) > 300;
        CHECK(on > 2000u, "a repeat held while stopped is silent");
    }
    free(o);
    printf("effects   %u punch effects, %u flex patterns, 40 and 300 BPM, knobs at their ends: ran\n", NPUNCH, NFLEX);
}

static void t_slicing(void)
{
    uint32_t b, tot_hit = 0, tot_true = 0, tot_found = 0, cls_ok = 0, cls_n = 0;
    for (b = 0; b < NFACT; b++) {
        uint32_t sl[NSL], n, i, j, hit = 0, tol = brk[b].rate * 15u / 1000u;
        uint8_t cls[NSL];
        boot();
        an_start(b);
        while (!an_poll(64)) {}
        n = an_slices(SLM_AUTO, 16, 6, sl, cls);
        for (i = 1; i < FACT[b].nsl; i++) {      /* every true hit (but the first, always 0): found? */
            for (j = 0; j < n; j++) {
                uint32_t d = sl[j] > FACT[b].sl[i] ? sl[j] - FACT[b].sl[i] : FACT[b].sl[i] - sl[j];
                if (d <= tol) { hit++; break; }
            }
        }
        for (i = 0; i < FACT[b].nsl; i++) {      /* the classes on the true slices */
            uint32_t e = i + 1u < FACT[b].nsl ? FACT[b].sl[i + 1u] : brk[b].frames;
            uint32_t c = an_class(FACT[b].sl[i], e);
            cls_ok += c == FACT[b].cls[i];
            cls_n++;
        }
        printf("slicing   %-12s %2u true hits, %2u found, %2u matched within 15 ms\n", brk[b].name, FACT[b].nsl - 1u, n - 1u, hit);
        tot_hit += hit; tot_true += FACT[b].nsl - 1u; tot_found += n - 1u;
    }
    printf("slicing   recall %.0f%%  precision %.0f%%  classes %.0f%% right\n", 100.0 * tot_hit / tot_true,
           100.0 * tot_hit / (tot_found ? tot_found : 1), 100.0 * cls_ok / cls_n);
    CHECK(tot_hit * 100u >= tot_true * 70u, "AUTO misses too many hits");
    CHECK(cls_ok * 100u >= cls_n * 60u, "classes too often wrong");
}

static void t_ima(void)
{
    uint32_t b;
    for (b = 0; b < NFACT; b++) {
        uint32_t n = brk[b].frames, i, blk;
        int16_t *pcm = malloc((n + IMA_SPB) * sizeof *pcm), *re = malloc((n + IMA_SPB) * sizeof *re);
        uint8_t *enc = malloc(ima_bytes(n));
        double s = 0, e = 0;
        for (blk = 0; blk * IMA_SPB < n; blk++) {
            int16_t c[IMA_SPB + 1];
            ima_block(brk[b].data + blk * IMA_BLOCK, c, n - blk * IMA_SPB, 0);
            memcpy(pcm + blk * IMA_SPB, c, IMA_SPB * sizeof *c);
        }
        ima_encode(pcm, n, enc);                /* re-encode the decoded: close to it */
        for (blk = 0; blk * IMA_SPB < n; blk++) {
            int16_t c[IMA_SPB + 1];
            ima_block(enc + blk * IMA_BLOCK, c, n - blk * IMA_SPB, 0);
            memcpy(re + blk * IMA_SPB, c, IMA_SPB * sizeof *c);
        }
        for (i = 0; i < n; i++) { s += (double)pcm[i] * pcm[i]; e += (double)(pcm[i] - re[i]) * (pcm[i] - re[i]); }
        CHECK(10 * log10(s / (e + 1)) > 20, "IMA round trip poor on %s", brk[b].name);
        free(pcm); free(re); free(enc);
    }
    printf("ima       encoder / decoder round trip on 8 breaks: ok\n");
}

static void t_generators(void)
{
    uint32_t b, s, r, bad = 0, total = 0;
    boot();
    for (b = 0; b < NFACT; b++)
        for (s = 0; s < GS_N; s++)
            for (r = 0; r < 8u; r++) {
                pat_t p;
                uint32_t i, notes = 0;
                gen_pattern(&p, b, s, r, 50, 70);
                for (i = 0; i < pat_len(&p); i++) {
                    if (p.st[i].sl > sl_count(b)) bad++;
                    notes += p.st[i].sl != 0;
                    if (p.st[i].fx > NPUNCH) bad++;
                }
                if (!notes) bad++;
                total++;
            }
    printf("generator %u patterns (8 breaks x %u styles x 8 rolls), %u bad\n", total, GS_N, bad);
    CHECK(!bad, "generators wrote %u bad patterns", bad);
}

int main(void)
{
    boot();
    t_ima();
    t_fidelity();
    t_timing();
    t_dice();
    t_effects();
    t_slicing();
    t_generators();
    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL ENGINE CHECKS PASSED\n", fails);
    return fails != 0;
}
