/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: the sequencer and app_audio (audio interrupt).
 *
 * The clock: 96 ticks a quarter note, sample-accurate (a block is cut at every tick). Two cursors walk the
 * pattern: the MASTER one always at the pattern's rate (the bar, the pattern switch, the metronome, the
 * resync), and the PLAY one, which the punch effects bend (a repeat loops 4, 2 or 1 steps; 6/8 stretches its
 * steps to 4/3) and comes back to the master when they are let go, as on a Pocket Operator.
 * A step that plays goes through, in order: the flex pattern (which step of the bar, its pitch, its gate),
 * the dice (another slice, backwards, a roll, a skip, a jump of pitch; FREE or LOCKed on a seed so the
 * variation repeats), its own chance, then its triggers are scheduled (the swing, the ratchets). */

typedef struct { uint32_t t; uint8_t b, k, rev, acc; int8_t pitch; uint8_t sub, pad[2]; uint32_t skip; } trig_t;   /* acc: bit 0 accent, 1-4 quieter; skip: output frames already gone (a rest's continuation) */
#define NTRIG 24u
#define NPEND 8u
static struct {
    uint32_t tick;                              /* the current tick (since PLAY; free-running when stopped) */
    uint32_t ph, spt;                           /* Q16 frames into the tick, Q16 frames a tick */
    uint32_t m_next, m_pos, m_cnt;              /* master: tick of its next step, the step it plays then, steps so far */
    uint32_t p_next, p_pos, p_len, p_cnt, p_last; /* play cursor: the next step's time (nominal), ... */
    uint32_t p_cur;                             /* the time of the step that sounds */
    uint32_t prepped, nom_idx, nom_fx, nom_len; /* the next step, prepared half a step early (its early hits) */
    uint32_t resync;                            /* the play cursor comes back to the master at its next step */
    uint32_t rep_n, rep_base, rep_cnt;          /* repeat: steps looped (0: none) */
    uint32_t trip;
    uint32_t cur_fx;                            /* the punch effect locked on the step that plays */
    int32_t nom_bt;                             /* the DESTROY / BEAT effect the prepared step locks (-1 none) */
    uint32_t loop, pre;                         /* the pattern's loops since PLAY (conditions); the last condition's result */
    uint32_t prev_src;                          /* the pattern step the last step played came from (a rest after it rings on) */
    trig_t tr[NTRIG];
    uint32_t ntr;
    trig_t pend[NPEND];                         /* due inside the current tick: in frames from now (.t) */
    uint32_t npend;
    uint32_t last_b, last_k, last_live;         /* the last slice played (the repeats when stopped) */
    uint32_t live_next;
    int32_t tape;                               /* Q16: 1 .. 0, the tape stop */
    uint32_t flex_cnt, flex_base, flex_cur;
    uint32_t gate1_t, gate1_v, gate2_t, gate2_v; /* the flex gate: the step's two 1/32 */
    uint32_t rec_skip;
    uint32_t ext_seen, ext_frames, ext_mark;    /* MIDI clock: clocks counted, frames since the mark */
    uint32_t frames;                            /* frames rendered */
} sq;

#ifdef BRK_TEST                                 /* (tests/engine_test.c: what played, when) */
static uint32_t tlog_n;
static struct { uint32_t t; uint8_t k, rev; } tlog[4096];
#define TLOG(k, rev) do { if (tlog_n < 4096u) { tlog[tlog_n].t = sq.tick; tlog[tlog_n].k = (uint8_t)(k); tlog[tlog_n].rev = (uint8_t)(rev); tlog_n++; } } while (0)
#else
#define TLOG(k, rev) ((void)0)
#endif

static uint32_t pat_len(const pat_t *p) { return p->len < 1u ? 1u : p->len > NSTEP ? NSTEP : p->len; }
static uint32_t step_ticks(const pat_t *p) { return RATE_TICKS[p->rate < RATE_N ? p->rate : 0]; }

/* Q16 frames a tick at the tempo: 44100 x 60 / (bpm x 96) = 27562.5 / bpm = 275625 / bpm x10 */
static uint32_t spt_of(uint32_t b10) { return (1806336000u / b10) * 10u; }

/* the punch effects acting now: the keys and the step's lock */
static uint32_t fx_now(void) { return (punch_keys | sq.cur_fx) & ((1u << NPUNCH) - 1u); }

/* ---- a slice starts */
static void trigger(uint32_t b, uint32_t k, int32_t pitch, uint32_t rev, uint32_t acc, uint32_t key, uint32_t live, uint32_t skip)
{
    uint32_t fx = fx_now(), speed, n = sl_count(b);
    int32_t gain = 3200, p;
    const slicing_t *sc;
    if (b >= NBRK || k >= n) return;
    sc = &prj.sc[b];
    p = pitch + (int32_t)prj.p.pitch - 24 + (sc->nsl ? sc->pitch[k] : 0);
    speed = semi_q16(p);
    if (prj.p.tmode && brk[b].bpm10)            /* VARI: the slices follow the tempo (and its pitch with it) */
        speed = (speed >> 4) * ((bpm10() << 12) / brk[b].bpm10) >> 8;
    if (sc->nsl) {
        if (sc->rev >> k & 1u) rev ^= 1u;
        gain = gain * (sc->gain[k] ? sc->gain[k] : 12) / 12;
    }
    if (fx >> PX_REV & 1u) rev ^= 1u;
    TLOG(k, rev);
    if (acc & 1u) gain = gain * 3 / 2;
    if (acc >> 1) gain = gain * (int32_t)(16u - ((acc >> 1) & 15u)) / 16;   /* (a fading ratchet) */
    if (prj.p.choke) vo_choke();
    if (fx >> PX_UNI & 1u) {
        vo_start(b, k, (speed >> 8) * 65159u >> 8, rev, gain * 5 / 8, -48, 0, key, live, skip);   /* (5/8 each: as loud */
        vo_start(b, k, (speed >> 8) * 65916u >> 8, rev, gain * 5 / 8, 48, 1, key, live, skip);    /* as one, no clip) */
    } else {
        vo_start(b, k, speed, rev, gain, 0, 0, key, live, skip);
    }
    sq.last_b = b;
    sq.last_k = k;
}

static void sched(uint32_t tq, uint32_t b, uint32_t k, int32_t pitch, uint32_t rev, uint32_t acc, uint32_t skip)   /* tq: Q8 ticks */
{
    trig_t *r;
    if (sq.ntr >= NTRIG) return;
    r = &sq.tr[sq.ntr++];
    r->t = tq >> 8;
    r->sub = (uint8_t)(tq & 255u);
    r->b = (uint8_t)b;
    r->k = (uint8_t)k;
    r->pitch = (int8_t)pitch;
    r->rev = (uint8_t)rev;
    r->acc = (uint8_t)acc;
    r->skip = skip;
}

/* ---- the dice: a random number for (what, the step); LOCK: the same in every loop of 1, 2 or 4 bars */
static uint32_t dice(uint32_t what)
{
    static uint32_t free_rng = 0x2545F491u;
    if (prj.p.d_lock) {
        uint32_t win = (uint32_t)prj.p.d_lock * 16u;
        return hash3(prj.p.d_seed + 1u, sq.m_cnt % win, what);
    }
    free_rng ^= free_rng << 13;
    free_rng ^= free_rng >> 17;
    free_rng ^= free_rng << 5;
    return hash3(free_rng, what, 0);
}

/* a slice of the same class as k (SMART), or any */
static uint32_t dice_slice(uint32_t b, uint32_t k, uint32_t r)
{
    uint32_t n = sl_count(b), i, cnt = 0, want = sl_class(b, k), pick;
    if (!n) return k;
    if (!prj.p.d_smart) return r % n;
    for (i = 0; i < n; i++) cnt += sl_class(b, i) == want;
    if (!cnt) return r % n;
    pick = r % cnt;
    for (i = 0; i < n; i++)
        if (sl_class(b, i) == want && !pick--) return i;
    return k;
}

/* prepare step idx, due at tick t (now: the current tick, up to half a step before) */
static void fire_step(uint32_t idx, uint32_t t, uint32_t plen, uint32_t odd, uint32_t now)
{
    const pat_t *pat = &prj.pat[prj.cur_pat < NPAT ? prj.cur_pat : 0];
    uint32_t len = pat_len(pat), b = pat->brk, rat, k, rev, acc, i, n, when, end, src, cont;
    int32_t pitch, d;
    step_t s;
    if (idx >= len) idx = 0;
    sq.nom_idx = idx;
    sq.nom_fx = 0;
    s = pat->st[idx];
    src = idx;
    if (sq_erase && !sq.rep_n && flex_on >= NFLEX) {   /* SHIFT + REC held: the step going by, cleared */
        step_t *w = &((pat_t *)pat)->st[idx];
        w->sl = 0; w->pitch = 0; w->fl = 0; w->fx = 0; w->nudge = 0; w->xt = 0;
        ((pat_t *)pat)->autog = 0;
        sq.prev_src = idx;
        return;
    }
    /* the flex pattern */
    if (flex_on < NFLEX) {
        const flex_t *f = &FLEX[flex_on];
        uint32_t bp = sq.flex_cnt % 16u, code = f->map[bp], g0, g1, dep = prj.p.fl_depth;
        if (bp == 0u) sq.flex_base = idx;
        g0 = f->gate[bp] >> 4;
        g1 = f->gate[bp] & 15u;
        g0 = 15u - (15u - g0) * dep / 100u;
        g1 = 15u - (15u - g1) * dep / 100u;
        sq.gate1_t = t;
        sq.gate1_v = g0 * 2184u + (g0 == 15u ? 7u : 0u);
        sq.gate2_t = t + plen / 2u;
        sq.gate2_v = g1 * 2184u + (g1 == 15u ? 7u : 0u);
        sq.flex_cnt++;
        sq.flex_cur = flex_on;
        if (code == FX_HOLD) { sq.prev_src = 0xFFFFu; return; }
        if (code == FX_REST) { vo_choke(); sq.prev_src = 0xFFFFu; return; }
        src = (sq.flex_base + (code & 15u)) % len;
        s = pat->st[src];
        if (code & RV) s.fl ^= SF_REV;
        if (code & R4) ST_SET_RI(&s, 3u);
        else if (code & R2) ST_SET_RI(&s, 1u);
        if (prj.p.fl_pitch) s.pitch = (int8_t)(s.pitch + f->pitch[bp]);
    }
    sq.nom_fx = s.fx && s.fx <= NPUNCH ? 1u << (s.fx - 1u) : 0;
    sq.nom_bt = s.fx >= FX_GL && s.fx < FX_GL + DD_N ? (int32_t)(s.fx - FX_GL) : -1;
    if (sq_rec && sq_play && beat_keys) {       /* REC: a DESTROY / BEAT effect held is written on the step */
        int32_t w = bt_want();
        if (w >= 0) { ((pat_t *)pat)->st[idx].fx = (uint8_t)(FX_GL + (uint32_t)w); ((pat_t *)pat)->autog = 0; }
    }
    cont = sq.prev_src == 0xFFFEu || (sq.prev_src < len && src == (sq.prev_src + 1u) % len);   /* (0xFFFE: the start) */
    sq.prev_src = src;
    if (!s.sl) {                                /* a rest: the slice before it rings on. Out of its place (a repeat, a
                                                 * flex pattern), that slice is started where it would be by now */
        uint32_t h, back, out;
        if (cont) return;
        for (back = 1u; back < len; back++) if (pat->st[(src + len - back) % len].sl) break;
        if (back >= len) return;
        h = (src + len - back) % len;
        s = pat->st[h];
        k = s.sl - 1u;
        if (k >= sl_count(b)) return;
        out = (uint32_t)(((uint64_t)(back * plen * 256u - (uint32_t)((int32_t)s.nudge * (int32_t)plen)) * (sq.spt >> 8)) >> 16);
        when = (int32_t)(t - now) < 0 ? now * 256u : t * 256u;
        sched(when, b, k, s.pitch, s.fl & SF_REV ? 1u : 0u, 0, out);
        return;
    }
    if (ST_COND(s)) {                           /* the condition: on which loops of the pattern */
        static const uint8_t A[CO_N] = {0, 1, 2, 1, 2, 3, 1, 2, 3, 4, 1, 8}, B[CO_N] = {1, 2, 2, 3, 3, 3, 4, 4, 4, 4, 8, 8};
        uint32_t c = ST_COND(s), ok;
        if (c < CO_FIRST) ok = sq.loop % B[c] == (uint32_t)(A[c] - 1u);
        else if (c == CO_FIRST) ok = sq.loop == 0u;
        else if (c == CO_NFIRST) ok = sq.loop != 0u;
        else if (c == CO_PRE) ok = sq.pre;
        else ok = !sq.pre;
        if (c < CO_PRE) sq.pre = ok;
        if (!ok) return;
    }
    if (sq.rec_skip == idx) { sq.rec_skip = 0xFFFFu; return; }
    k = s.sl - 1u;
    pitch = s.pitch;
    rev = s.fl & SF_REV ? 1u : 0u;
    acc = s.fl & SF_ACC ? 1u : 0u;
    rat = ST_RAT(s);
    /* the dice */
    if (prj.p.d_skip && dice(1) % 100u < prj.p.d_skip) return;
    if (prj.p.d_chance && dice(2) % 100u < prj.p.d_chance) k = dice_slice(b, k, dice(3));
    if (prj.p.d_rev && dice(4) % 100u < prj.p.d_rev) rev ^= 1u;
    if (prj.p.d_roll && dice(5) % 100u < prj.p.d_roll) rat = 2u + dice(6) % 3u;
    if (prj.p.d_pitch && dice(7) % 100u < prj.p.d_pitch) {
        static const int8_t JUMP[6] = {12, -12, 7, -5, 5, 24};
        pitch += JUMP[dice(8) % 6u];
    }
    /* the step's own chance */
    if (ST_CH(s) && dice(9) % 8u < ST_CH(s)) return;
    if ((punch_keys | sq.nom_fx) >> PX_ROLL & 1u) rat *= 2u;
    if (rat > 16u) rat = 16u;
    n = sl_count(b);
    if (k >= n) return;
    /* when: the swing (odd steps), the nudge (the break's own groove: early or late), never before now;
     * the ratchets share what is left of the step */
    d = odd && prj.p.swing > 50u ? (int32_t)((prj.p.swing - 50u) * plen * 256u / 50u) : 0;
    d += (int32_t)s.nudge * (int32_t)plen;
    end = (t + plen) * 256u;
    when = (uint32_t)((int32_t)(t * 256u) + d);
    if ((int32_t)(when - now * 256u) < 0) when = now * 256u;
    if ((int32_t)(when - end) >= 0) when = end - 1u;
    for (i = 0; i < rat; i++) {                 /* the ratchets, and their ramp */
        int32_t p = pitch;
        uint32_t rv = rev, lvl = 0;
        switch (ST_RAMP(s)) {
        case RP_UP: p += (int32_t)i; break;
        case RP_DOWN: p -= (int32_t)i; break;
        case RP_OCTUP: p += rat > 1u ? (int32_t)(i * 12u / (rat - 1u)) : 0; break;
        case RP_OCTDN: p -= rat > 1u ? (int32_t)(i * 12u / (rat - 1u)) : 0; break;
        case RP_RAND: if (i) p += (int32_t)(dice(10 + i) % 15u) - 7; break;
        case RP_FADE: lvl = rat > 1u ? i * 13u / (rat - 1u) : 0u; break;
        case RP_REVALT: rv ^= i & 1u; break;
        default: break;
        }
        sched(when + i * (end - when) / rat, b, k, p, rv, (acc && i == 0u ? 1u : 0u) | lvl << 1, 0);
    }
}

/* ---- the transport */
static void seq_reset(void)
{
    const pat_t *pat = &prj.pat[prj.cur_pat < NPAT ? prj.cur_pat : 0];
    sq.tick = 0;
    sq.ph = 0;
    sq.m_next = sq.p_next = sq.p_cur = 0;
    sq.prepped = 0;
    sq.p_last = 0;
    sq.m_pos = sq.p_pos = 0;
    sq.m_cnt = sq.p_cnt = 0;
    sq.p_len = step_ticks(pat);
    sq.rep_n = 0;
    sq.trip = 0;
    sq.resync = 0;
    sq.ntr = 0;
    sq.npend = 0;
    sq.cur_fx = 0;
    sq.nom_bt = -1;
    sq.loop = 0;
    sq.pre = 1;
    sq.prev_src = 0xFFFEu;
    bt_seq = -1;
    sq.flex_cnt = 0;
    sq.rec_skip = 0xFFFFu;
    sq.gate1_t = sq.gate2_t = 0xFFFFFFFFu;
    mx.gate_t = 32767;
}

/* the pattern after this one: the chain, or the one asked for */
static void next_pattern(void)
{
    if (pat_next < NPAT) {
        prj.cur_pat = (uint8_t)pat_next;
        pat_next = 0xFF;
    } else if (prj.nchain > 1u) {
        uint32_t i, at = 0;
        for (i = 0; i < prj.nchain; i++)
            if (prj.chain[i] == prj.cur_pat) { at = i; break; }
        prj.cur_pat = prj.chain[(at + 1u) % prj.nchain];
    }
}

static void tick(uint32_t t)
{
    const pat_t *pat = &prj.pat[prj.cur_pat < NPAT ? prj.cur_pat : 0];
    uint32_t L = step_ticks(pat), len = pat_len(pat), fx = fx_now(), want_rep, i, nn;
    /* the flex gate */
    if (t == sq.gate1_t) { mx.gate_t = (int32_t)sq.gate1_v; sq.gate1_t = 0xFFFFFFFFu; }
    if (t == sq.gate2_t) { mx.gate_t = (int32_t)sq.gate2_v; sq.gate2_t = 0xFFFFFFFFu; }
    if (flex_on >= NFLEX && sq.flex_cur != 0xFFu) { mx.gate_t = 32767; sq.flex_cur = 0xFF; }
    if (!sq_play) {                             /* stopped: the repeats replay the last slice (live) */
        uint32_t every = fx >> PX_RPT16 & 1u ? L : fx >> PX_RPT8 & 1u ? 2u * L : fx >> PX_RPT4 & 1u ? 4u * L :
                         fx >> PX_ROLL & 1u ? L / 2u : 0u;
        if (every && sq.last_live) {
            if ((int32_t)(t - sq.live_next) >= 0) {
                trigger(sq.last_b, sq.last_k, 0, 0, 0, 0xFF, 1, 0);
                sq.live_next = t + every;
            }
        } else {
            sq.live_next = t;
        }
        goto fire;
    }
    /* the master: its step */
    if (t == sq.m_next) {
        sq.m_pos = (sq.m_pos + 1u) % len;
        sq.m_cnt++;
        sq_bar_step = sq.m_cnt;
        sq.m_next += L;
    }
    if (t % PPQN == 0u && (prj.p.metro == 2u || (prj.p.metro == 1u && sq_rec)))
        fx_click(t % (4u * PPQN) == 0u);
    /* 6/8 let go: the play cursor comes back on the master's next step */
    if (sq.trip && !(fx >> PX_TRIP & 1u)) {
        sq.trip = 0;
        sq.p_len = L;
        sq.resync = 1;
    }
    /* the next step, prepared half a step before its time (an early hit is scheduled before it) */
    nn = sq.resync && !sq.prepped ? sq.m_next : sq.p_next;
    if (!sq.prepped && (int32_t)(t + sq.p_len / 2u - nn) >= 0) {
        uint32_t idx;
        if (sq.resync) { sq.p_next = sq.m_next; sq.p_pos = sq.m_pos; sq.resync = 0; }
        if (sq.p_next == sq.m_next && sq.m_pos == 0u && sq.m_cnt) {   /* the pattern ends: the next, the chain */
            uint32_t was = prj.cur_pat;
            next_pattern();
            sq.loop = prj.cur_pat != was ? 0u : sq.loop + 1u;
            if (prj.cur_pat != was) {
                sq.prev_src = 0xFFFEu;
                pat = &prj.pat[prj.cur_pat];
                len = pat_len(pat);
                L = step_ticks(pat);
                sq.p_pos = 0;
                sq.rep_n = 0;
                if (!sq.trip) sq.p_len = L;
            }
        }
        want_rep = fx >> PX_RPT16 & 1u ? 1u : fx >> PX_RPT8 & 1u ? 2u : fx >> PX_RPT4 & 1u ? 4u : 0u;
        if (want_rep != sq.rep_n) {
            if (want_rep && !sq.rep_n) { sq.rep_base = sq.p_pos; sq.rep_cnt = 0; }
            if (!want_rep && sq.p_next == sq.m_next) sq.p_pos = sq.m_pos;
            sq.rep_n = want_rep;
        }
        if (!sq.trip && fx >> PX_TRIP & 1u) { sq.trip = 1; sq.p_len = L * 4u / 3u; }
        if (!sq.trip) sq.p_len = L;
        if (flex_sel != flex_on) {              /* a flex pattern starts on a step, a beat or a bar */
            uint32_t q = prj.p.fl_quant, at = sq.p_next;
            uint32_t ok = q == 0u || (q == 1u && at % PPQN == 0u) || (q == 2u && at % (4u * PPQN) == 0u);
            if (flex_sel >= NFLEX) ok = 1;
            if (ok) { flex_on = flex_sel; sq.flex_cnt = 0; }
        }
        if (flex_on < NFLEX && prj.p.fl_mode == 2u && sq.flex_cnt >= 16u) flex_on = flex_sel = 0xFF;   /* 1 BAR: done */
        if (sq.rep_n) {
            idx = (sq.rep_base + sq.rep_cnt % sq.rep_n) % len;
            sq.rep_cnt++;
        } else {
            idx = sq.p_pos % len;
            sq.p_pos = (idx + 1u) % len;
        }
        fire_step(idx, sq.p_next, sq.p_len, sq.p_cnt & 1u, t);
        sq.nom_len = sq.p_len;
        sq.prepped = 1;
    }
    /* the prepared step's time: it is the one that sounds */
    if (sq.prepped && (int32_t)(t - sq.p_next) >= 0) {
        sq_pos = sq.p_last = sq.nom_idx;
        sq.cur_fx = sq.nom_fx;
        bt_seq = (int8_t)sq.nom_bt;
        sq.p_cur = sq.p_next;
        sq.p_next += sq.nom_len;
        sq.p_cnt++;
        sq.prepped = 0;
    }
fire:
    for (i = 0; i < sq.ntr;) {                  /* the triggers due in this tick: now, or a part of it later */
        if ((int32_t)(t - sq.tr[i].t) >= 0) {
            trig_t r = sq.tr[i];
            sq.tr[i] = sq.tr[--sq.ntr];
            if (!r.sub || (int32_t)(t - r.t) > 0) trigger(r.b, r.k, r.pitch, r.rev, r.acc, 0xFF, 0, r.skip);
            else if (sq.npend < NPEND) {
                r.t = (uint32_t)r.sub * (sq.spt >> 8) >> 16;   /* frames into the tick */
                sq.pend[sq.npend++] = r;
            }
        } else {
            i++;
        }
    }
}

static void pend_fire(uint32_t frames)          /* the frame-precise triggers: frames passed */
{
    uint32_t i;
    for (i = 0; i < sq.npend;) {
        if (sq.pend[i].t <= frames) {
            trig_t r = sq.pend[i];
            sq.pend[i] = sq.pend[--sq.npend];
            trigger(r.b, r.k, r.pitch, r.rev, r.acc, 0xFF, 0, r.skip);
        } else {
            sq.pend[i].t -= frames;
            i++;
        }
    }
}

/* a key played while recording: the step it is nearest */
static void rec_key(uint32_t k)
{
    pat_t *pat = &prj.pat[prj.cur_pat < NPAT ? prj.cur_pat : 0];
    uint32_t len = pat_len(pat), el = sq.tick - sq.p_cur, idx, i;
    uint32_t fx = punch_keys, f;
    step_t *s;
    if (el * 2u < sq.nom_len || !sq.nom_len) {
        idx = sq.p_last;                        /* this step: it plays from the next loop */
    } else {                                    /* the next one: it plays now, from the key */
        idx = sq.prepped ? sq.nom_idx : (sq.rep_n ? sq.p_last : sq.p_pos % len);
        if (sq.prepped) {                       /* what was prepared for it is dropped */
            for (i = 0; i < sq.ntr;) {
                if ((int32_t)(sq.tr[i].t - sq.p_next) >= 0 || (int32_t)(sq.tr[i].t + sq.nom_len / 2u - sq.p_next) >= 0) sq.tr[i] = sq.tr[--sq.ntr];
                else i++;
            }
        } else {
            sq.rec_skip = idx;
        }
    }
    if (idx >= len) return;
    s = &pat->st[idx];
    s->sl = (uint8_t)(k + 1u);
    s->pitch = 0;
    s->fl = 0;
    for (f = 0; f < NPUNCH && !(fx >> f & 1u); f++) {}
    s->fx = (uint8_t)(f < NPUNCH ? f + 1u : 0u);
    s->nudge = 0;
    pat->autog = 0;
}

static void handle_ev(const ev_t *e)
{
    const pat_t *pat = &prj.pat[prj.cur_pat < NPAT ? prj.cur_pat : 0];
    switch (e->t) {
    case EV_SLICE:                              /* a: slice, b: key */
        trigger(pat->brk, e->a, 0, 0, 0, e->b, 1, 0);
        sq.last_live = 1;
        sq.live_next = sq.tick + step_ticks(pat) * 4u;   /* (the repeats when stopped start after it) */
        if (sq_rec && sq_play) rec_key(e->a);
        break;
    case EV_SLICE_OFF:
        if (prj.p.gate) vo_off_key(e->a);
        break;
    case EV_PLAY:
        seq_reset();
        sq.ext_seen = ext_clocks;
        sq_play = 1;
        tick(0);
        break;
    case EV_CONT:
        sq_play = 1;
        break;
    case EV_STOP:
        sq_play = 0;
        sq_rec = 0;
        sq.ntr = 0;
        sq.npend = 0;
        sq.cur_fx = 0;
        bt_seq = -1;
        sq_pos = 0xFF;
        mx.gate_t = 32767;
        if (flex_on < NFLEX) flex_on = 0xFF;
        break;
    case EV_PANIC:
        sq_play = 0;
        sq_rec = 0;
        sq.ntr = 0;
        bt_seq = -1;
        vo_off_key(0xFF);
        break;
    default:
        break;
    }
}

/* the speed multiplier (Q16) the effects ask at this moment: the octaves, scratch, tape stop, the flex ramp */
static int32_t speed_now(uint32_t t, uint32_t ph)
{
    uint32_t fx = fx_now();
    int32_t sp = 65536;
    if (fx >> PX_OCTDN & 1u) sp >>= 1;
    if (fx >> PX_OCTUP & 1u) sp <<= 1;
    if (flex_on < NFLEX && FLEX[flex_on].ramp != RAMP_NONE) {
        uint32_t into = (sq.flex_cnt ? sq.flex_cnt - 1u : 0u) % 16u;   /* 0..15: the step of the bar */
        int32_t r = (int32_t)into * 4096;       /* Q16 fraction of the bar */
        switch (FLEX[flex_on].ramp) {
        case RAMP_STOP: sp = (int32_t)((int64_t)sp * (65536 - r) >> 16); break;
        case RAMP_SPIN: sp = (int32_t)((int64_t)sp * (65536 - r / 3) >> 16); break;
        case RAMP_BACK: if (into >= 8u) sp = sp * 3 / 2; break;
        default: break;
        }
    }
    return (int32_t)(((int64_t)sp * sq.tape) >> 16);
}

/* ---- the audio interrupt: n stereo frames, L R L R, Q15; out is zero */
static int32_t mixL[HALF_FRAMES], mixR[HALF_FRAMES];
static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i, off = 0, ext = prj.p.clock == 1u;
    if (audio_hold || n > HALF_FRAMES) { audio_blocks++; return; }
    while (ev_r != ev_w) {
        ev_t e = evq[ev_r % NEV];
        handle_ev(&e);
        ev_r++;
    }
    /* the tempo: the BPM, or the MIDI clock's (measured over a beat) */
    if (!ext) {
        sq.spt = spt_of(bpm10());
    } else {
        uint32_t clocks = ext_clocks;
        sq.ext_frames += n;
        if (clocks - sq.ext_mark >= 24u) {      /* a beat of clocks: the tempo they mean */
            uint32_t f = sq.ext_frames;
            if (f > 4000u && f < 200000u) sq.spt = (f << 10) / 96u << 6;
            sq.ext_mark = clocks;
            sq.ext_frames = 0;
        }
        if (!sq.spt) sq.spt = spt_of(1200u);
    }
    /* tape stop: down over a beat while held, back up over a sixteenth */
    {
        int32_t beat = (int32_t)((sq.spt >> 16) * PPQN), d;
        if (beat < 1000) beat = 1000;
        d = (int32_t)((n << 16) / (uint32_t)beat);
        sq.tape = sq.tape + 4 * d < 65536 ? sq.tape + 4 * d : 65536;   /* (the tape stop is BEAT's and FLEX's now) */
    }
    fx_tick0 = sq.tick;                         /* (the BEAT bank: where the bar is) */
    fx_ph0 = sq.ph;
    fx_spt = sq.spt;
    for (i = 0; i < n; i++) mixL[i] = mixR[i] = 0;
    while (off < n) {
        uint32_t left = n - off, chunk = (sq.spt - sq.ph + 65535u) >> 16, run = 1;
        int32_t sp0, sp1;
        if (ext && sq_play) {                   /* the MIDI clock leads: never more than a clock ahead of it */
            uint32_t allowed = (ext_clocks - sq.ext_seen) * 4u;
            if (sq.tick + 1u > allowed + 4u) run = 0;
            else if (sq.tick + 8u < allowed) { sq.ph = sq.spt; chunk = 1; }   /* behind: catch up, a tick a frame */
        }
        if (!chunk) chunk = 1;
        if (chunk > left || !run) chunk = left;
        {
            uint32_t j;
            for (j = 0; j < sq.npend; j++) if (sq.pend[j].t && sq.pend[j].t < chunk) chunk = sq.pend[j].t;
        }
        if (sq.npend) { pend_fire(0); }
        sp0 = speed_now(sq.tick, sq.ph);
        sp1 = run ? speed_now(sq.tick, sq.ph + (chunk << 16) < sq.spt ? sq.ph + (chunk << 16) : sq.spt - 1u) : sp0;
        vo_render(mixL + off, mixR + off, chunk, sp0, (sp1 - sp0) / (int32_t)chunk);
        off += chunk;
        if (sq.npend) pend_fire(chunk);
        if (!run) break;
        sq.ph += chunk << 16;
        if (sq.ph >= sq.spt) {
            sq.ph -= sq.spt;
            sq.tick++;
            tick(sq.tick);
        }
    }
    bt_process(mixL, mixR, n);                 /* the BEAT bank, then the master, then PAN */
    mx.tg = (fx_now() >> PX_TG16) & 3u;
    mx.play = sq_play;
    mx.beat = dd_beat();
    mx.pos0 = dd_barpos();
    fx_master(mixL, mixR, n);
    pn_process(mixL, mixR, n);
    rs_tap(mixL, mixR, n);                      /* RESAMPLE: what comes out */
    for (i = 0; i < n; i++) {
        out[2u * i] = mixL[i];
        out[2u * i + 1u] = mixR[i];
    }
    fx_active = fx_now() | (flex_on < NFLEX ? 1u << 16 : 0u);
    sq.frames += n;
    audio_blocks++;
}

static void engine_init(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++) vo[i].on = 0;
    fx_reset();
    bt_reset();
    seq_reset();
    sq.tape = 65536;
    sq.flex_cur = 0xFF;
    sq.spt = spt_of(bpm10());
    sq.last_b = 0;
    sq.last_k = 0;
}
