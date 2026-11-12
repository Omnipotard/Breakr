/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: editing (main loop): the slicing of a break, the generators, undo, the defaults. */

static uint32_t pat_dirty;                      /* the project changed since it was saved */
static uint32_t dirty_ms;

static void touch(void) { pat_dirty = 1; dirty_ms = fm1_ms; }

static pat_t *cur_pat(void) { return &prj.pat[prj.cur_pat < NPAT ? prj.cur_pat : 0]; }
static uint32_t cur_brk(void) { uint32_t b = cur_pat()->brk; return b < NBRK && brk[b].data ? b : 0; }

static int32_t clampn(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- generators */
static uint32_t g_rng;
static uint32_t grand(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static uint32_t gpct(uint32_t pct) { return grand() % 100u < pct; }

/* a slice of class c of break b (random among them), else `fallback` */
static uint32_t pick_class(uint32_t b, uint32_t c, uint32_t fallback)
{
    uint32_t n = sl_count(b), i, cnt = 0, r;
    for (i = 0; i < n; i++) cnt += sl_class(b, i) == c;
    if (!cnt) return fallback < n ? fallback : 0;
    r = grand() % cnt;
    for (i = 0; i < n; i++)
        if (sl_class(b, i) == c && !r--) return i;
    return fallback;
}

/* the break in order: slice k on the step its start falls on */
static void gen_orig(pat_t *p, uint32_t b)
{
    uint32_t n = sl_count(b), len, k, bars = brk[b].bars ? brk[b].bars : 1u;
    if (prj.sc[b].bars) bars = prj.sc[b].bars;
    len = 16u * bars;
    if (len > NSTEP) len = NSTEP;
    memset(p->st, 0, sizeof p->st);
    p->len = (uint8_t)len;
    p->brk = (uint8_t)b;
    p->rate = RATE_16;
    for (k = 0; k < n; k++) {                   /* the step it falls in, and how late in it (its groove) */
        uint32_t at = sl_start(b, k) * len, pos = (at + brk[b].frames / 2u) / brk[b].frames;   /* the nearest step */
        int32_t nud = ((int32_t)at - (int32_t)(pos * brk[b].frames)) * 256 / (int32_t)brk[b].frames;   /* (|.| <= frames/2) */
        if (pos >= len) pos = len - 1u;
        if (!p->st[pos].sl) { p->st[pos].sl = (uint8_t)(k + 1u); p->st[pos].nudge = (int8_t)clampn(nud, -127, 127); }
    }
    if (!p->st[0].sl && n) p->st[0].sl = 1;
}

static void put(pat_t *p, uint32_t at, uint32_t k, int32_t pitch, uint32_t fl, uint32_t fx)
{
    if (at >= NSTEP) return;
    p->st[at].sl = (uint8_t)(k + 1u);
    p->st[at].pitch = (int8_t)pitch;
    p->st[at].fl = (uint8_t)fl;
    p->st[at].fx = (uint8_t)fx;
    p->st[at].nudge = 0;
    p->st[at].xt = 0;
}
#define RAT(n) ((uint32_t)((n) - 1u) << 2)
static void put_rat(pat_t *p, uint32_t at, uint32_t ri, uint32_t ramp)   /* a ratchet (RAT_N index) and its ramp */
{
    if (at >= NSTEP || !p->st[at].sl) return;
    ST_SET_RI(&p->st[at], ri);
    p->st[at].xt = (uint8_t)((p->st[at].xt & 15u) | (ramp & 7u) << 4);
}
static uint32_t euclid(uint32_t k, uint32_t n, uint32_t i) { return n && (i * k) % n < k; }   /* k hits over n */

/* write pattern p for break b in a style; roll: which roll of the dice (0: the same as ORIG for most) */
static void gen_pattern(pat_t *p, uint32_t b, uint32_t style, uint32_t roll, uint32_t dens, uint32_t var)
{
    uint32_t len, i, bar, n = sl_count(b);
    uint32_t K, S, H;
    pat_t o;
    if (!n) return;
    g_rng = hash3(roll + 1u, style * 977u + b, 0x5EEDu) | 1u;
    gen_orig(&o, b);
    len = o.len;
    *p = o;
    p->autog = style == GS_ORIG && roll == 0u;
    if (style == GS_ORIG) {
        if (roll) for (i = 0; i < len; i++)     /* a few variations */
            if (p->st[i].sl && gpct(var / 3u)) p->st[i].sl = (uint8_t)(pick_class(b, sl_class(b, p->st[i].sl - 1u), p->st[i].sl - 1u) + 1u);
        return;
    }
    K = pick_class(b, CL_K, 0);
    S = pick_class(b, CL_S, n > 1u ? 1u : 0u);
    H = pick_class(b, CL_H, n > 2u ? 2u : 0u);
    switch (style) {
    case GS_SHUFFLE:
        for (i = 0; i < len; i++)
            if (p->st[i].sl && gpct(30u + var / 2u))
                p->st[i].sl = (uint8_t)(pick_class(b, sl_class(b, p->st[i].sl - 1u), grand() % n) + 1u);
        break;
    case GS_JUNGLE:
        for (bar = 0; bar * 16u < len; bar++) {
            uint32_t o16 = bar * 16u, beat = grand() % 4u;
            if (gpct(50u + var / 2u)) {          /* one beat: a snare edit */
                put(p, o16 + beat * 4u, S, 0, 0, 0);
                put(p, o16 + beat * 4u + 2u, S, 0, RAT(2), 0);
                if (gpct(dens)) put(p, o16 + beat * 4u + 3u, pick_class(b, CL_S, S), 2, 0, 0);
            }
            if (gpct(40u + var / 3u)) put(p, o16 + 10u, K, 0, 0, 0);   /* the kick on the and of three */
            if (bar & 1u || len <= 16u) {        /* the end of the phrase: a roll */
                put(p, o16 + 14u, S, 0, RAT(2), 0);
                put(p, o16 + 15u, S, gpct(var) ? 2 : 0, gpct(var / 2u) ? RAT(4) : RAT(2), 0);
            }
            if (gpct(var / 2u)) put(p, o16 + 6u + (grand() % 2u) * 2u, pick_class(b, CL_S, S), 0, SF_REV, 0);
        }
        break;
    case GS_HALF:
        for (i = 0; i < len; i++) p->st[i].sl = 0;
        for (bar = 0; bar * 16u < len; bar++) {
            uint32_t o16 = bar * 16u;
            put(p, o16, K, 0, SF_ACC, 0);
            put(p, o16 + 8u, S, 0, SF_ACC, 0);
            if (gpct(50u + var / 2u)) put(p, o16 + 10u + (grand() % 2u), pick_class(b, CL_K, K), 0, 0, 0);
            for (i = 2; i < 16u; i += 2u)
                if (!p->st[o16 + i].sl && gpct(dens)) put(p, o16 + i, pick_class(b, CL_H, H), 0, 0, 0);
            if (gpct(var)) put(p, o16 + 15u, pick_class(b, CL_S, S), 0, 0, 0);
        }
        break;
    case GS_BOOM:
        for (i = 0; i < len; i++) p->st[i].sl = 0;
        for (bar = 0; bar * 16u < len; bar++) {
            uint32_t o16 = bar * 16u;
            put(p, o16, K, 0, SF_ACC, 0);
            put(p, o16 + 4u, S, 0, SF_ACC, 0);
            put(p, o16 + 10u, K, 0, 0, 0);
            put(p, o16 + 12u, S, 0, SF_ACC, 0);
            if (gpct(40u + var / 2u)) put(p, o16 + 7u, pick_class(b, CL_K, K), 0, 0, 0);
            for (i = 2; i < 16u; i += 4u)
                if (gpct(dens + 30u)) put(p, o16 + i, pick_class(b, CL_H, H), 0, 0, 0);
            if (gpct(var / 2u)) put(p, o16 + 15u, pick_class(b, CL_S, S), 0, 0, 0);
        }
        break;
    case GS_2STEP:
        for (i = 0; i < len; i++) p->st[i].sl = 0;
        for (bar = 0; bar * 16u < len; bar++) {
            uint32_t o16 = bar * 16u;
            put(p, o16, K, 0, SF_ACC, 0);
            put(p, o16 + 4u, S, 0, SF_ACC, 0);
            put(p, o16 + 10u, K, 0, 0, 0);
            put(p, o16 + 12u, S, 0, SF_ACC, 0);
            for (i = 2; i < 16u; i += 4u) put(p, o16 + i, pick_class(b, CL_H, H), 0, 0, 0);
            if (gpct(dens)) put(p, o16 + 7u, pick_class(b, CL_S, S), 0, 0, 0);
            if (gpct(var)) put(p, o16 + 14u, pick_class(b, CL_S, S), 0, RAT(2), 0);
        }
        break;
    case GS_GLITCH:
        for (i = 0; i < len; i++) {
            if (!p->st[i].sl) { if (gpct(dens / 2u)) put(p, i, grand() % n, 0, 0, 0); continue; }
            if (gpct(var / 2u)) p->st[i].fl = (uint8_t)(p->st[i].fl | RAT(2u + grand() % 3u));
            if (gpct(var / 3u)) p->st[i].fl ^= SF_REV;
            if (gpct(var / 4u)) p->st[i].pitch = (int8_t)((grand() & 1u) ? 12 : -12);
            if (gpct(var / 5u)) p->st[i].fx = (uint8_t)(1u + PX_RPT16 + (grand() % 2u) * (PX_ROLL - PX_RPT16));
            if (gpct(var / 4u)) p->st[i].sl = 0;
        }
        break;
    case GS_DRILL:                              /* drill'n'bass: the break's hits, drilled (ratchets that climb or fall) */
        for (i = 0; i < len; i++) p->st[i].sl = 0;
        for (bar = 0; bar * 16u < len; bar++) {
            uint32_t o16 = bar * 16u, db = 1u + (grand() % 2u) * 2u, end = (bar & 1u) || len <= 16u;
            static const uint8_t RMP[4] = {RP_UP, RP_DOWN, RP_OCTUP, RP_FADE};
            put(p, o16, K, 0, SF_ACC, 0);
            put(p, o16 + 4u, S, 0, SF_ACC, 0);
            put(p, o16 + 12u, S, 0, SF_ACC, 0);
            if (gpct(60u)) put(p, o16 + 10u, pick_class(b, CL_K, K), 0, 0, 0);
            if (gpct(30u + var / 4u)) put(p, o16 + 7u, pick_class(b, CL_K, K), 0, 0, 0);
            for (i = 1; i < 16u; i++) {          /* the gaps: hats and ghosts, some drilled */
                uint32_t at = o16 + i;
                if (p->st[at].sl || !gpct(dens)) continue;
                put(p, at, gpct(60u) ? pick_class(b, CL_H, H) : grand() % n, gpct(var / 4u) ? ((grand() & 1u) ? 12 : -12) : 0, 0, 0);
                if (gpct(var / 2u)) put_rat(p, at, 1u + grand() % 5u, RMP[grand() % 4u]);
            }
            for (i = 1; i < 4u; i++)             /* a snare drill on one beat */
                if (gpct(55u + var / 3u)) { put(p, o16 + db * 4u + i, pick_class(b, CL_S, S), 0, 0, 0); put_rat(p, o16 + db * 4u + i, 2u + grand() % 4u, RP_UP); }
            if (end) {                           /* the phrase's end: 12 and 16 hits up an octave */
                put(p, o16 + 14u, S, 0, SF_ACC, 0);
                put_rat(p, o16 + 14u, 6u, RP_OCTUP);
                put(p, o16 + 15u, pick_class(b, CL_S, S), gpct(var) ? 5 : 0, 0, 0);
                put_rat(p, o16 + 15u, 7u, gpct(50u) ? RP_OCTUP : RP_DOWN);
            }
        }
        break;
    case GS_CORE:                               /* breakcore: four on the floor under amen edits, rolls, glitches */
        for (i = 0; i < len; i++) {
            uint32_t q = i % 16u;
            if (q % 4u == 0u) { put(p, i, K, q == 0u ? 0 : -3, SF_ACC, 0); continue; }
            if (q == 4u || q == 12u) { put(p, i, S, 0, SF_ACC, 0); continue; }
            if (!gpct(40u + dens / 2u)) { p->st[i].sl = 0; continue; }
            put(p, i, grand() % n, gpct(var / 5u) ? 7 : 0, gpct(var / 3u) ? SF_REV : 0, 0);
            if (gpct(20u + var / 3u)) put_rat(p, i, 3u + grand() % 5u, gpct(50u) ? RP_FADE : RP_DOWN);
            if (gpct(var / 6u)) p->st[i].fx = (uint8_t)(FX_GL + (gpct(50u) ? 16u + 0u : 1u));   /* RND STUTTER / STUTTER X */
        }
        for (bar = 0; bar * 16u < len; bar++)
            if ((bar & 1u) || len <= 16u) {      /* the end of the phrase: a scratch, a roll */
                p->st[bar * 16u + 13u].fx = (uint8_t)(FX_GL + 3u);   /* SCRATCH */
                put(p, bar * 16u + 15u, S, 0, SF_ACC, 0);
                put_rat(p, bar * 16u + 15u, 7u, RP_OCTUP);
            }
        break;
    case GS_IDM: {                              /* IDM: an odd length against the bar, euclidean hits, conditions */
        uint32_t L = len - 1u - grand() % 3u, kk = 3u + grand() % 3u, ss = 2u + grand() % 2u, rot = grand() % 4u;
        for (i = 0; i < len; i++) p->st[i].sl = 0;
        p->len = (uint8_t)L;
        for (i = 0; i < L; i++) {
            if (euclid(kk, L, i)) put(p, i, pick_class(b, CL_K, K), 0, i == 0u ? SF_ACC : 0, 0);
            else if (euclid(ss, L, (i + L - rot - 4u) % L)) put(p, i, pick_class(b, CL_S, S), 0, 0, 0);
            else if (gpct(dens)) put(p, i, gpct(50u) ? pick_class(b, CL_H, H) : grand() % n, gpct(var / 4u) ? -5 : 0, 0, 0);
            else continue;
            p->st[i].nudge = (int8_t)((int32_t)(grand() % 61u) - 30);
            if (gpct(var / 3u)) p->st[i].xt = (uint8_t)(CO_1_2 + grand() % 6u);
            if (gpct(var / 3u)) put_rat(p, i, 2u + grand() % 2u, RP_REVALT);
            if (gpct(var / 8u)) p->st[i].fx = (uint8_t)(FX_GL + (gpct(50u) ? 2u : 10u));   /* GLITCH / MICRO STUTTER */
        }
        break;
    }
    case GS_BUILD:
        for (i = 0; i < len; i++) {
            uint32_t q = i % 16u;
            if (q >= 8u && q < 12u) put(p, i, S, 0, q & 1u ? 0 : SF_ACC, 0);
            else if (q >= 12u) put(p, i, S, (int32_t)(q - 11u) * (var > 50u ? 2 : 1), RAT(q >= 14u ? 4u : 2u), 0);
        }
        break;
    default:
        break;
    }
}

/* ---- undo: the last 16 edits (a pattern, or a break's slicing), and redo. An edit is marked before it is made:
 * the target as it was is kept. The same edit going on (the same knob, the same step, within 1.5 s) stays one entry.
 * Undo swaps the kept state with the live one, so the entry then holds what redo puts back. */
enum { UK_PAT, UK_SC };
enum { U_STEP, U_REC, U_ERASE, U_GEN, U_CLEAR, U_COPY, U_LEN, U_RATE, U_ROT, U_HUM, U_RAT, U_REV, U_CHOP, U_SLICE,
       U_BREAK, U_N };
static const char *const U_NAME_EN[U_N] = {"STEP", "RECORDING", "ERASE", "GENERATE", "CLEAR", "COPY", "LENGTH", "RATE",
                                            "ROTATE", "HUMANIZE", "RATCHETS", "REVERSE", "CHOP", "SLICING", "BREAK"};
static const char *const U_NAME_FR[U_N] = {"PAS", "ENREG.", "GOMME", "GENERER", "EFFACER", "COPIE", "LONGUEUR", "VITESSE",
                                            "DECALER", "HUMANISER", "RATCHETS", "INVERSER", "CHOP", "DECOUPE", "BREAK"};
#define NUNDO 16u
typedef struct {
    uint8_t kind, idx, what, pad;
    uint16_t tag, pad2;
    uint32_t ms;
    union { pat_t pat; slicing_t sc; } u;
} undo_t;
static undo_t undo_buf[NUNDO];
static uint32_t u_base, u_cur, u_top;           /* [u_base, u_cur): undo; [u_cur, u_top): redo */
static uint16_t u_seq;                          /* (a tag no other edit has: a press that is its own edit) */

static void undo_mark(uint32_t kind, uint32_t idx, uint32_t what, uint32_t tag)
{
    undo_t *u;
    if (kind == UK_PAT ? idx >= NPAT : idx >= NBRK) return;
    if (u_cur > u_base && u_cur == u_top) {
        u = &undo_buf[(u_cur - 1u) % NUNDO];
        if (u->kind == kind && u->idx == idx && u->what == what && u->tag == (uint16_t)tag && u->ms && fm1_ms - u->ms < 1500u) {
            u->ms = fm1_ms ? fm1_ms : 1u;
            return;
        }
    }
    if (u_cur - u_base >= NUNDO) u_base = u_cur - NUNDO + 1u;   /* full: the oldest goes */
    u = &undo_buf[u_cur % NUNDO];
    u->kind = (uint8_t)kind;
    u->idx = (uint8_t)idx;
    u->what = (uint8_t)what;
    u->tag = (uint16_t)tag;
    u->ms = fm1_ms ? fm1_ms : 1u;
    if (kind == UK_PAT) u->u.pat = prj.pat[idx];
    else u->u.sc = prj.sc[idx];
    u_cur++;
    u_top = u_cur;
}
static uint32_t undo_tag(void) { return ++u_seq; }
static int undo_can(int redo) { return redo ? u_cur != u_top : u_cur != u_base; }
static uint32_t undo_next(int redo) { return undo_buf[(redo ? u_cur : u_cur - 1u) % NUNDO].what; }   /* (if undo_can) */

static void slicing_changed(uint32_t b);
static uint32_t slice_want_b;
/* a slicing put back: the interrupt reads the break's own slices meanwhile */
static void sc_put(uint32_t b, const slicing_t *src)
{
    slicing_t *sc = &prj.sc[b];
    uint8_t n = src->nsl;
    sc->nsl = 0;
    RING_PUBLISH();
    sc->mode = src->mode; sc->count = src->count; sc->sens = src->sens; sc->bars = src->bars; sc->edited = src->edited;
    memcpy(sc->sl, src->sl, sizeof sc->sl);
    memcpy(sc->cls, src->cls, sizeof sc->cls);
    memcpy(sc->pitch, src->pitch, sizeof sc->pitch);
    memcpy(sc->gain, src->gain, sizeof sc->gain);
    sc->rev = src->rev;
    RING_PUBLISH();
    sc->nsl = n;
}
static uint32_t brk_bpm10(uint32_t b, uint32_t bars);
/* undo (redo): the edit it undid, U_*; -1 nothing to undo */
static int32_t undo_swap(int redo)
{
    undo_t *u;
    if (!undo_can(redo)) return -1;
    u = redo ? &undo_buf[u_cur++ % NUNDO] : &undo_buf[--u_cur % NUNDO];
    if (u->kind == UK_PAT) {
        pat_t t = prj.pat[u->idx];
        prj.pat[u->idx] = u->u.pat;
        u->u.pat = t;
    } else {
        slicing_t t = prj.sc[u->idx];
        if (slice_want_b == u->idx) slice_want_b = 0xFF;   /* (a look still going would undo the undo) */
        sc_put(u->idx, &u->u.sc);
        u->u.sc = t;
        if (prj.sc[u->idx].bars && brk[u->idx].data) brk[u->idx].bpm10 = brk_bpm10(u->idx, prj.sc[u->idx].bars);
        slicing_changed(u->idx);
    }
    u->ms = 0;                                  /* (nothing joins an entry that has been swapped) */
    touch();
    return u->what;
}
static void undo_reset(void) { u_base = u_cur = u_top = 0; }

/* ---- slicing: apply what SLICE asks for break b once the look at it is done */
static uint32_t slice_want_b = 0xFF;
static void slice_request(uint32_t b) { slice_want_b = b; if (an.b != b || (!an.busy && !an.done)) an_start(b); }

static void slicing_changed(uint32_t b)          /* the patterns still in order follow it */
{
    uint32_t i;
    for (i = 0; i < NPAT; i++)
        if (prj.pat[i].autog && prj.pat[i].brk == b) {
            gen_orig(&prj.pat[i], b);
            prj.pat[i].autog = 1;
        }
    touch();
}

static void slice_poll(void)
{
    uint32_t b = slice_want_b, n, i;
    slicing_t *sc;
    static uint32_t tmp_sl[NSL];
    static uint8_t tmp_cls[NSL];
    if (b >= NBRK) return;
    if (an.b != b) an_start(b);
    if (!an_poll(16)) return;
    slice_want_b = 0xFF;
    sc = &prj.sc[b];
    if (sc->mode == SLM_ORIG) {
        sc->nsl = 0;                            /* the break's own slices */
        sc->edited = 0;
        slicing_changed(b);
        return;
    }
    n = an_slices(sc->mode, sc->count, sc->sens, tmp_sl, tmp_cls);
    if (n < 1u) return;
    sc->nsl = 0;                                /* (the interrupt reads the break's own meanwhile) */
    for (i = 0; i < n; i++) {
        sc->sl[i] = tmp_sl[i];
        sc->cls[i] = tmp_cls[i];
        sc->pitch[i] = 0;
        sc->gain[i] = 12;
    }
    sc->rev = 0;
    sc->edited = 0;
    RING_PUBLISH();
    sc->nsl = (uint8_t)n;
    slicing_changed(b);
}

/* the CHOP page edits slices: make the slicing the project's own first */
static slicing_t *slicing_own(uint32_t b)
{
    slicing_t *sc = &prj.sc[b];
    uint32_t i, n;
    if (sc->nsl) return sc;
    n = brk[b].nsl;
    for (i = 0; i < n; i++) {
        sc->sl[i] = brk[b].sl[i];
        sc->cls[i] = brk[b].cls[i];
        sc->pitch[i] = 0;
        sc->gain[i] = 12;
    }
    sc->rev = 0;
    RING_PUBLISH();
    sc->nsl = (uint8_t)n;
    return sc;
}

/* move slice k's start by d frames (k > 0: the first stays at 0) */
static void slice_move(uint32_t b, uint32_t k, int32_t d)
{
    slicing_t *sc = slicing_own(b);
    int32_t lo, hi, v;
    if (k == 0u || k >= sc->nsl) return;
    lo = (int32_t)sc->sl[k - 1u] + 200;
    hi = (k + 1u < sc->nsl ? (int32_t)sc->sl[k + 1u] : (int32_t)brk[b].frames) - 200;
    v = (int32_t)sc->sl[k] + d;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    if (lo > hi) return;
    sc->sl[k] = (uint32_t)v;
    sc->edited = 1;
    touch();
}

/* split slice k in two halves (OCT+), or join it to the one before (OCT-) */
static int slice_split(uint32_t b, uint32_t k)
{
    slicing_t *sc = slicing_own(b);
    uint32_t i, s, e;
    if (k >= sc->nsl || sc->nsl >= NSL) return 0;
    s = sc->sl[k];
    e = k + 1u < sc->nsl ? sc->sl[k + 1u] : brk[b].frames;
    if (e - s < 800u) return 0;
    for (i = sc->nsl; i > k + 1u; i--) {
        sc->sl[i] = sc->sl[i - 1u];
        sc->cls[i] = sc->cls[i - 1u];
        sc->pitch[i] = sc->pitch[i - 1u];
        sc->gain[i] = sc->gain[i - 1u];
    }
    sc->sl[k + 1u] = s + (e - s) / 2u;
    sc->cls[k + 1u] = CL_X;
    sc->pitch[k + 1u] = 0;
    sc->gain[k + 1u] = 12;
    sc->nsl++;
    sc->edited = 1;
    slicing_changed(b);
    return 1;
}
static int slice_join(uint32_t b, uint32_t k)
{
    slicing_t *sc = slicing_own(b);
    uint32_t i;
    if (k == 0u || k >= sc->nsl || sc->nsl <= 2u) return 0;
    for (i = k; i + 1u < sc->nsl; i++) {
        sc->sl[i] = sc->sl[i + 1u];
        sc->cls[i] = sc->cls[i + 1u];
        sc->pitch[i] = sc->pitch[i + 1u];
        sc->gain[i] = sc->gain[i + 1u];
    }
    sc->nsl--;
    sc->edited = 1;
    slicing_changed(b);
    return 1;
}

/* ---- a new project: the eight breaks in order on 1..8, eight rolls of the dice on 9..16 */
static void p2_defaults(void)
{
    uint32_t i;
    memset(&prj.p2, 0, sizeof prj.p2);
    prj.p2.width = 16;
    prj.p2.pn_speed = 1;
    prj.p2.pn_edge = 4;
    for (i = 0; i < NPAN; i++) prj.p2.pn_int[i] = 70;
}
static void prj_defaults(void)
{
    static const uint8_t GEN_B[8] = {0, 1, 4, 3, 0, 2, 5, 6};
    static const uint8_t GEN_S[8] = {GS_JUNGLE, GS_SHUFFLE, GS_GLITCH, GS_BOOM, GS_2STEP, GS_HALF, GS_JUNGLE, GS_BUILD};
    uint32_t i;
    memset(&prj, 0, sizeof prj);
    prj.magic = PRJ_MAGIC;
    prj.version = PRJ_VERSION;
    prj.p.pitch = 24;
    prj.p.filter = 64;
    prj.p.dly_time = 2;
    prj.p.dly_fb = 6;
    prj.p.decay = 15;
    prj.p.length = 16;
    prj.p.choke = 1;
    prj.p.swing = 50;
    prj.p.d_smart = 1;
    prj.p.d_seed = 1;
    prj.p.g_dens = 50;
    prj.p.g_var = 50;
    prj.p.fl_depth = 100;
    prj.p.fl_pitch = 1;
    prj.p.vol = 12;
    for (i = 0; i < 32u; i++) prj.p.bt_int[i] = 50;
    for (i = 0; i < NBRK; i++) { prj.sc[i].count = 16; prj.sc[i].sens = 5; prj.sc[i].bars = brk[i].bars; }
    for (i = 0; i < 8u; i++) {
        uint32_t b = i < NFACT && brk[i].data ? i : 0;
        gen_orig(&prj.pat[i], b);
        prj.pat[i].autog = 1;
        gen_pattern(&prj.pat[8u + i], GEN_B[i], GEN_S[i], i + 1u, 50, 50);
        prj.pat[8u + i].autog = 0;
    }
    prj.cur_pat = 0;
    bpm10_set(brk[0].bpm10 ? brk[0].bpm10 : 1700u);
    memcpy(prj.name, "BREAKR", 7);
    p2_defaults();
}

/* everything from flash goes through here before the interrupt sees it */
static void prj_sanitize(void)
{
    uint32_t i, k;
    params_t *p = &prj.p;
    if (p->pitch > 48u) p->pitch = 24;
    if (p->filter > 127u) p->filter = 64;
    if (p->reso > 15u) p->reso = 15;
    if (p->drive > 15u) p->drive = 15;
    if (p->crush > 15u) p->crush = 15;
    if (p->dly_time > 5u) p->dly_time = 2;
    if (p->dly_fb > 15u) p->dly_fb = 15;
    if (p->dly_mix > 15u) p->dly_mix = 15;
    if (p->punch > 15u) p->punch = 15;
    if (p->attack > 15u) p->attack = 15;
    if (p->decay > 15u) p->decay = 15;
    if (p->length < 1u || p->length > 16u) p->length = 16;
    p->choke &= 1u;
    p->gate &= 1u;
    if (p->swing < 50u || p->swing > 75u) p->swing = 50;
    p->tmode &= 1u;
    if (p->d_chance > 100u) p->d_chance = 100;
    if (p->d_rev > 100u) p->d_rev = 100;
    if (p->d_roll > 100u) p->d_roll = 100;
    if (p->d_skip > 100u) p->d_skip = 100;
    if (p->d_pitch > 100u) p->d_pitch = 100;
    p->d_smart &= 1u;
    if (p->d_lock != 0u && p->d_lock != 1u && p->d_lock != 2u && p->d_lock != 4u) p->d_lock = 0;
    if (p->g_style >= GS_N) p->g_style = 0;
    if (p->g_dens > 100u) p->g_dens = 50;
    if (p->g_var > 100u) p->g_var = 50;
    if (p->fl_mode > 2u) p->fl_mode = 0;
    if (p->fl_quant > 2u) p->fl_quant = 1;
    if (p->fl_depth > 100u) p->fl_depth = 100;
    p->fl_pitch &= 1u;
    bpm10_set(bpm10());
    if (p->metro > 2u) p->metro = 0;
    if (p->clock > 1u) p->clock = 0;
    if (p->vol > 15u) p->vol = 12;
    for (i = 0; i < 32u; i++) if (p->bt_int[i] > 100u) p->bt_int[i] = 50;
    for (i = 0; i < NPAT; i++) {
        pat_t *pt = &prj.pat[i];
        if (pt->len < 1u || pt->len > NSTEP) pt->len = 16;
        if (pt->brk >= NBRK || !brk[pt->brk].data) pt->brk = 0;
        if (pt->rate >= RATE_N) pt->rate = RATE_16;
        pt->autog &= 1u;
        for (k = 0; k < NSTEP; k++) {
            step_t *s = &pt->st[k];
            if (s->sl > NSL) s->sl = 0;
            if (s->pitch < -24 || s->pitch > 24) s->pitch = 0;
            if (s->fx > NPUNCH && (s->fx < FX_GL || s->fx >= FX_GL + 32u)) s->fx = 0;   /* (punch 1..11, DESTROY / BEAT 12..43) */
        }
    }
    if (prj.nchain > 16u) prj.nchain = 0;
    for (i = 0; i < 16u; i++) if (prj.chain[i] >= NPAT) prj.chain[i] = 0;
    if (prj.cur_pat >= NPAT) prj.cur_pat = 0;
    for (i = 0; i < NBRK; i++) {
        slicing_t *sc = &prj.sc[i];
        if (sc->mode >= SLM_N) sc->mode = SLM_ORIG;
        if (sc->count < 2u || sc->count > NSL) sc->count = 16;
        if (sc->sens < 1u || sc->sens > 10u) sc->sens = 5;
        if (sc->bars != 1u && sc->bars != 2u && sc->bars != 4u && sc->bars != 8u) sc->bars = brk[i].bars ? brk[i].bars : 1u;
        if (sc->nsl > NSL || !brk[i].data) sc->nsl = 0;
        for (k = 0; k < sc->nsl; k++) {         /* ascending, inside the break */
            if (sc->sl[k] >= brk[i].frames || (k && sc->sl[k] <= sc->sl[k - 1u])) { sc->nsl = 0; break; }
            sc->cls[k] &= 3u;
            if (sc->pitch[k] < -24 || sc->pitch[k] > 24) sc->pitch[k] = 0;
            if (sc->gain[k] > 15u) sc->gain[k] = 12;
        }
        if (sc->nsl) sc->sl[0] = 0;
    }
    prj.name[11] = 0;
    {
        params2_t *q = &prj.p2;
        if (q->flt_type >= FT_N) q->flt_type = FT_DJ;
        if (q->flt_move > 7u) q->flt_move = 0;
        if (q->dly_type >= DT_N) q->dly_type = DT_CLEAN;
        if (q->locut > 15u) q->locut = 0;
        if (q->width > 32u) q->width = 16;
        if (q->pn_speed > 2u) q->pn_speed = 1;
        if (q->pn_edge > 15u) q->pn_edge = 4;
        for (i = 0; i < NPAN; i++) if (q->pn_int[i] > 100u) q->pn_int[i] = 70;
    }
}
