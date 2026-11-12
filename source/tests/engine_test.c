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
#include "brk_beat.c"
#include "brk_rsmp.c"
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

/* DESTROY and BEAT (FX, pages 3 and 4; K-GLITCH's banks): every effect at 3 tempos and 3 intensities against the same groove
 * without it: it changes the sound while held, stays in range, lets go cleanly; the gates cut on the bar */
static void t_beat(void)
{
    static const uint16_t BPMS[3] = {700, 1200, 2400};
    static const uint8_t INT[3] = {0, 50, 100};
    uint32_t bi, e, ii, i, changed = 0, runs = 0;
    for (bi = 0; bi < 3u; bi++) {
        uint32_t bar = (uint32_t)(44100.0 * 60.0 * 4.0 * 10.0 / BPMS[bi]), pre = (bar + 255u) & ~255u, len = (2u * bar + 255u) & ~255u, tot = pre + len + 22016u;   /* (whole blocks: render() keeps the time) */
        int32_t *dry;
        boot();
        bpm10_set(BPMS[bi]);
        ev_push(EV_PLAY, 0, 0, 0);
        dry = render(tot);
        for (e = 0; e < DD_N; e++) {
            for (ii = 0; ii < 3u; ii++) {
                int32_t *a, *b, *c;
                double de = 0, dd = 0;
                uint32_t quiet = 0, win = 0;
                boot();
                bpm10_set(BPMS[bi]);
                for (i = 0; i < DD_N; i++) prj.p.bt_int[i] = INT[ii];
                ev_push(EV_PLAY, 0, 0, 0);
                a = render(pre);                 /* a bar of the groove, then the program held 2 bars, then let go */
                bt_key(e, 1);
                b = render(len);
                bt_key(e, 0);
                c = render(22016u);
                for (i = 0; i < 2u * len; i++) {
                    double w = b[i], d0 = dry[2u * pre + i];
                    de += (w - d0) * (w - d0);
                    dd += d0 * d0;
                    CHECK(b[i] >= -32767 && b[i] <= 32767, "BEAT %u: out of range", e);
                }
                for (i = 0; i + 441u <= len; i += 441u, win++) {   /* 10 ms windows */
                    uint32_t j, pk = 0;
                    for (j = 0; j < 441u; j++) { int32_t v = b[2u * (i + j)]; v = v < 0 ? -v : v; if ((uint32_t)v > pk) pk = (uint32_t)v; }
                    quiet += pk < 200u;
                }
                if ((e == DD_GATE8 || e == DD_GATE16) && ii == 1u)
                    CHECK(quiet * 100u >= win * 25u && quiet * 100u <= win * 75u, "BEAT gate %u at %u: %u of %u windows silent", e, BPMS[bi], quiet, win);
                runs++;
                changed += de > dd * 0.01;
                if (de <= dd * 0.01 && ii) printf("glitch    %-12s %3u%% at %u: no change?\n", BEAT_LONG[e], INT[ii], BPMS[bi] / 10u);
                for (i = 2u * 11025u; i < 2u * 22050u; i++)   /* let go: it fades (no stuck effect) */
                    CHECK(kd.cur < 0, "BEAT %u still playing after its key", e);
                free(a); free(b); free(c);
            }
        }
        free(dry);
    }
    printf("glitch    32 effects (DESTROY, BEAT) x 3 tempos x 3 intensities: %u of %u change the groove\n", changed, runs);
    CHECK(changed * 100u >= runs * 85u, "DESTROY / BEAT: too few effects change the sound");
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

/* clicks: a one-sample jump much bigger than the slope around it (channel L of an interleaved render) */
static uint32_t clicks(const int32_t *o, uint32_t a, uint32_t b)
{
    uint32_t n, c = 0;
    for (n = a + 20u; n + 20u < b; n++) {
        int32_t d = abs(o[2u * n] - o[2u * n - 2u]), j;
        double m = 0;
        if (d < 2500) continue;
        for (j = 2; j <= 16; j++) m += abs(o[2u * (n - j)] - o[2u * (n - j - 1)]) + abs(o[2u * (n + j)] - o[2u * (n + j - 1)]);
        if (d > 7.0 * m / 30.0 + 600.0) { c++; n += 32u; }
    }
    return c;
}

/* the glitch effects that are not meant to be digital (all but GLITCH, FUZZ, RANDOMIZER, TOTAL CHAOS): no click,
 * pressed and let go off the grid; a repeat over a rest of the pattern plays the slice that rings there */
static void t_clean(void)
{
    static const uint8_t INT[3] = {10, 50, 90};
    uint32_t e, ii, i, worst = 0, bad = 0, bar = 44100u * 60u * 4u / 140u, pre = (bar + 3001u) & ~255u, len = (2u * bar + 1777u) & ~255u;
    for (e = 0; e < DD_N; e++) {
        if (e == DD_GLITCH || e == DD_FUZZ || e == DD_RAND || e == DD_TOTAL) continue;
        for (ii = 0; ii < 3u; ii++) {
            int32_t *a, *b, *c, *o;
            uint32_t n;
            boot();
            bpm10_set(1400);
            for (i = 0; i < DD_N; i++) prj.p.bt_int[i] = INT[ii];
            ev_push(EV_PLAY, 0, 0, 0);
            a = render(pre);
            bt_key(e, 1);
            b = render(len);
            bt_key(e, 0);
            c = render(22016u);
            o = malloc(sizeof *o * 2u * (pre + len + 22016u));
            memcpy(o, a, sizeof *o * 2u * pre);
            memcpy(o + 2u * pre, b, sizeof *o * 2u * len);
            memcpy(o + 2u * (pre + len), c, sizeof *o * 2u * 22016u);
            n = clicks(o, pre - 2000u, pre + len + 4000u);
            if (n > worst) worst = n;
            if (n > 1u) { bad++; printf("click     %-12s %2u%%: %u\n", BEAT_LONG[e], INT[ii], n); }   /* (one: a sharp hit can pass for one) */
            free(a); free(b); free(c); free(o);
        }
    }
    CHECK(bad == 0, "%u glitch effect settings click", bad);
    {   /* REPEAT 1/16 over a rest: the slice that rings there, again (not silence) */
        const pat_t *pt;
        uint32_t step = 44100u * 60u / 1420u / 4u, r = 0, k;
        double wet = 0, dry = 0;
        int32_t *d, *w;
        boot();
        bpm10_set(1420);
        pt = cur_pat();
        for (k = 2; k < pat_len(pt); k++) if (!pt->st[k].sl && pt->st[k - 1u].sl) { r = k; break; }
        CHECK(r != 0, "no rest in the break in order");
        ev_push(EV_PLAY, 0, 0, 0);
        d = render(step * (r + 6u));
        boot();
        bpm10_set(1420);
        ev_push(EV_PLAY, 0, 0, 0);
        w = render(step * r - step / 2u);
        punch_keys = 1u << PX_RPT16;
        free(w);
        w = render(step * 6u);
        punch_keys = 0;
        for (i = step / 2u; i < step * 6u; i++) wet += (double)w[2u * i] * w[2u * i];
        for (i = step * r; i < step * (r + 1u); i++) dry += (double)d[2u * i] * d[2u * i] * 5.5;
        printf("clean     glitch effects held off the grid: %u with clicks (worst %u); REPEAT 1/16 over a rest: %.0f%% of its step\n", bad, worst, 100.0 * sqrt(wet / (dry + 1.0)));
        CHECK(wet > dry * 0.25, "REPEAT 1/16 over a rest is near silence");
        free(d); free(w);
    }
}

/* undo: 16 edits back, redo, one entry for an edit going on, a slicing put back */
static void t_undo(void)
{
    uint32_t i, ok = 1;
    pat_t *p;
    boot();
    undo_reset();
    p = &prj.pat[0];
    fm1_ms = 1000;
    undo_mark(UK_PAT, 0, U_STEP, 3); p->st[3].sl = 9;
    fm1_ms = 1200;
    undo_mark(UK_PAT, 0, U_STEP, 3); p->st[3].pitch = 5;            /* (the same step, held: one entry) */
    fm1_ms = 5000;
    undo_mark(UK_PAT, 0, U_CLEAR, undo_tag()); memset(p->st, 0, sizeof p->st);
    CHECK(undo_swap(0) == U_CLEAR && p->st[3].sl == 9 && p->st[3].pitch == 5, "undo: the clear is not undone");
    CHECK(undo_swap(0) == U_STEP && p->st[3].pitch == 0, "undo: the step edit is not one entry");
    CHECK(undo_swap(0) == -1, "undo: more than was done");
    CHECK(undo_swap(1) == U_STEP && p->st[3].sl == 9 && p->st[3].pitch == 5, "redo: the step");
    CHECK(undo_swap(1) == U_CLEAR && p->st[3].sl == 0, "redo: the clear");
    CHECK(undo_swap(1) == -1, "redo: more than was undone");
    undo_reset();
    for (i = 0; i < 20u; i++) { fm1_ms = 10000u + i * 2000u; undo_mark(UK_PAT, 0, U_LEN, 0); p->len = (uint8_t)(i + 1u); }
    for (i = 0; i < 16u; i++) ok &= undo_swap(0) == U_LEN;
    CHECK(ok && p->len == 4u && undo_swap(0) == -1, "undo: 16 back, the oldest dropped (len %u)", p->len);
    undo_reset();
    {   /* a slicing: moved, then put back */
        uint32_t b = 0, was;
        slicing_t *sc = slicing_own(b);
        was = sc->sl[2];
        undo_mark(UK_SC, b, U_CHOP, undo_tag());
        slice_move(b, 2, 300);
        CHECK(sc->sl[2] != was, "undo: the slice did not move");
        CHECK(undo_swap(0) == U_CHOP && prj.sc[b].sl[2] == was, "undo: the slice is not back");
    }
    printf("undo      16 edits back, redo, an edit going on is one, a slicing put back: %s\n", fails ? "see above" : "ok");
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

/* the steps' conditions and ratchets: 1:2 plays every other loop, 3:4 once in four, FIRST once; x16 with its ramp */
static void t_steps(void)
{
    uint32_t bar, i, c1[6] = {0}, c3[6] = {0}, cf[6] = {0}, rk[6] = {0};
    int32_t *o;
    pat_t *p;
    boot();
    bpm10_set(1200);
    p = &prj.pat[0];
    prj.cur_pat = 0;
    memset(p->st, 0, sizeof p->st);
    p->len = 16; p->rate = RATE_16; p->autog = 0;
    p->st[0].sl = 1; p->st[0].xt = CO_1_2;
    p->st[4].sl = 2; p->st[4].xt = CO_3_4;
    p->st[8].sl = 3; p->st[8].xt = CO_FIRST;
    p->st[12].sl = 4; ST_SET_RI(&p->st[12], 7u); p->st[12].xt = (uint8_t)(RP_OCTUP << 4);
    tlog_n = 0;
    ev_push(EV_PLAY, 0, 0, 0);
    o = render(44100u * 12u + 4096u);          /* 6 bars at 120 */
    for (i = 0; i < tlog_n; i++) {
        bar = tlog[i].t / 384u;
        if (bar >= 6u) continue;
        if (tlog[i].k == 0u) c1[bar]++;
        if (tlog[i].k == 1u) c3[bar]++;
        if (tlog[i].k == 2u) cf[bar]++;
        if (tlog[i].k == 3u) rk[bar]++;
    }
    printf("steps     1:2 %u%u%u%u%u%u  3:4 %u%u%u%u%u%u  FIRST %u%u%u%u%u%u  x16 %u %u\n", c1[0], c1[1], c1[2], c1[3], c1[4], c1[5],
           c3[0], c3[1], c3[2], c3[3], c3[4], c3[5], cf[0], cf[1], cf[2], cf[3], cf[4], cf[5], rk[0], rk[1]);
    CHECK(c1[0] == 1u && c1[1] == 0u && c1[2] == 1u && c1[3] == 0u && c1[4] == 1u, "condition 1:2");
    CHECK(c3[2] == 1u && c3[0] + c3[1] + c3[3] == 0u && c3[5] == 0u, "condition 3:4");
    CHECK(cf[0] == 1u && cf[1] + cf[2] + cf[3] + cf[4] + cf[5] == 0u, "condition FIRST");
    CHECK(rk[0] == 16u && rk[1] == 16u, "ratchet x16: %u, %u hits", rk[0], rk[1]);
    p->st[2].sl = 1; p->st[2].fx = (uint8_t)(FX_GL + 31u); p->st[3].sl = 1; p->st[3].fx = 5; p->st[5].fx = 200;
    prj_sanitize();                             /* (what a project from flash goes through) */
    CHECK(p->st[2].fx == FX_GL + 31u && p->st[3].fx == 5u && p->st[5].fx == 0u && ST_COND(p->st[0]) == CO_1_2 && ST_RAMP(p->st[12]) == RP_OCTUP,
          "a loaded project keeps its locks, conditions and ramps");
    free(o);
}

/* the DJ filter: every position of the knob at three resonances, on white noise. A filter takes away; at the
 * lowest resonance it never adds (no ringing near the top of the low-pass), at the highest it rings but stays
 * bounded; and the knob passing the middle back and forth stays clean */
static void t_filter(void)
{
    static const uint8_t RESO[3] = {0, 8, 15};
    static int32_t L[4096], R[4096];
    uint32_t ri, f, i, rs = 12345u, bad = 0;
    double worst[3] = {0, 0, 0};
    int worst_at[3] = {0, 0, 0};
    for (ri = 0; ri < 3u; ri++)
        for (f = 0; f < 128u; f++) {
            double ein = 0, eout = 0;
            boot();
            fx_reset();
            prj.p.drive = 0; prj.p.crush = 0; prj.p.dly_mix = 0; prj.p.punch = 0;
            prj.p.reso = RESO[ri];
            prj.p.filter = (uint8_t)f;
            for (i = 0; i < 4096u; i++) { rs = rs * 1664525u + 1013904223u; L[i] = R[i] = (int32_t)(rs >> 18) - 8192; }
            for (i = 0; i < 4096u; i += 256u) fx_master(L + i, R + i, 256u);
            for (i = 0; i < 4096u; i++) { rs = rs * 1664525u + 1013904223u; L[i] = R[i] = (int32_t)(rs >> 18) - 8192; }
            for (i = 0; i < 4096u; i++) ein += (double)L[i] * L[i];
            for (i = 0; i < 4096u; i += 256u) fx_master(L + i, R + i, 256u);
            for (i = 0; i < 4096u; i++) eout += (double)L[i] * L[i];
            if (sqrt(eout / ein) > worst[ri]) { worst[ri] = sqrt(eout / ein); worst_at[ri] = (int)f; }
            if (ri == 0u && eout > ein * 1.10 * 1.10) bad++;
            if (eout > ein * 3.0 * 3.0) bad++;
        }
    printf("filter    loudest out / in: reso 0 %.2f (knob %d), reso 8 %.2f (knob %d), reso 15 %.2f (knob %d)\n",
           worst[0], worst_at[0], worst[1], worst_at[1], worst[2], worst_at[2]);
    CHECK(!bad, "the filter adds level (%u knob positions)", bad);
    {                                           /* the knob through the middle, back and forth, on a sine */
        int32_t pk = 0;
        boot();
        fx_reset();
        prj.p.drive = 0; prj.p.crush = 0; prj.p.dly_mix = 0; prj.p.punch = 0; prj.p.reso = 0;
        for (f = 0; f < 400u; f++) {
            uint32_t pos = f % 80u < 40u ? 40u + f % 80u : 120u - f % 80u;   /* 40 .. 80 .. 40 */
            prj.p.filter = (uint8_t)pos;
            for (i = 0; i < 256u; i++) { int32_t v = (int32_t)(16000.0 * sin((f * 256u + i) * 0.0712)); L[i] = R[i] = v; }
            fx_master(L, R, 256u);
            for (i = 0; i < 256u; i++) { int32_t a = L[i] < 0 ? -L[i] : L[i]; if (a > pk) pk = a; }
        }
        printf("filter    a 500 Hz sine at 16000, the knob swept through the middle: peak %d\n", pk);
        CHECK(pk < 18000, "the filter rings through the middle (peak %d)", pk);
    }
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
                    if (p.st[i].fx >= FX_GL + DD_N || ST_COND(p.st[i]) >= CO_N) bad++;
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
    t_beat();
    t_effects();
    t_slicing();
    t_filter();
    t_clean();
    t_undo();
    t_steps();
    t_generators();
    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL ENGINE CHECKS PASSED\n", fails);
    return fails != 0;
}
