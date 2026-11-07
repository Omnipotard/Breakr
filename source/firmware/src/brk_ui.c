/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: the pages, the panel, the LEDs, MIDI, app_main.
 *
 *   HOME   PLAY     the break, its slices on the white keys (OCT: 17-32), the punch effects on the black ones
 *   SEQ    STEPS    the pattern, 16 steps on the white keys (OCT: the next 16); hold a step to edit it
 *   SEL    SLICE    the break, how it is cut (as is, a grid, its hits), its bars, its tempo
 *   EDIT   CHOP     one slice: its start, pitch, gain, backwards, class; OCT+ splits it, OCT- joins it
 *   ENV    AMP      attack, decay, length, choke, gate
 *   LFO    DICE     random: another slice, backwards, rolls, skips, jumps; LOCK; GENERATE a pattern
 *   ARP    FLEX     16 flex patterns on the white keys (half time, tape stop, gates, juggles...)
 *   FX     FX       filter, resonance, drive, crush; delay, feedback, mix, punch; BEAT: K-GLITCH's 16 programs
 *                    over the bar on the white keys, held (stutters, fills, rolls, gates, reverses, scratches...)
 *   GLO    GLOBAL   tempo, swing, the tempo mode, the metronome; clock, language, theme, volume
 *   SAVE   SAVE     4 project slots; the patterns and their chain
 * The page's button again: its next page. HOME held: the help of the page. HOME + PLAY: everything stops.
 * The knobs' bar always says what KNOB 1 (red), 2 (yellow), 3 (green) and 4 (blue) do. */

enum { P_PLAY, P_SEQ, P_SLICE, P_CHOP, P_AMP, P_DICE, P_FLEX, P_FX, P_GLOBAL, P_SAVE, P_N };
static const uint8_t PAGE_BTN[P_N] = {BTN_HOME, BTN_SEQ, BTN_SEL, BTN_EDIT, BTN_ENV, BTN_LFO, BTN_ARP, BTN_FX, BTN_GLO, BTN_SAVE};
static const uint8_t PAGE_SUBS[P_N] = {1, 1, 1, 2, 1, 3, 1, 3, 2, 1};
static const char *const PAGE_NAME[P_N] = {"PLAY", "STEPS", "SLICE", "CHOP", "AMP", "DICE", "FLEX", "FX", "GLOBAL", "SAVE"};

/* the keys: 0 = F3 .. 26 = G5; 16 white, 11 black */
static const uint8_t WHITE_KEY[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static const uint8_t BLACK_KEY[NPUNCH] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};
static int8_t key_white[27], key_black[27];      /* the white / black index of a key, -1 */

static uint8_t beat_sel;                        /* the BEAT effect KNOB 1 sets (the last key pressed) */
static uint8_t page, sub[P_N], bank16, seq_page, chop_k, brush = 1, help_on, splash = 1;
static int8_t held_step = -1;
static uint8_t held_used, gen_kept;
static uint32_t notes_prev, home_ms, chain_first = 0xFF;
static uint8_t key_live[27];                    /* the slice a key started (+1), for its release */
static char toast_s[28];
static uint32_t toast_until;
static uint8_t save_slot, save_act, save_tgt;
static uint32_t save_arm;
static uint32_t tap_ms[4], tap_n;
static uint32_t boot_ms;

typedef struct { uint8_t lang, theme, slot, pad; } settings_t;
static settings_t sets;

static void toast(const char *s)
{
    uint32_t i;
    for (i = 0; i + 1u < sizeof toast_s && s[i]; i++) toast_s[i] = s[i];
    toast_s[i] = 0;
    toast_until = fm1_ms + 1300u;
}
static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static int32_t accel(int32_t d) { int32_t a = d < 0 ? -d : d; return a >= 4 ? d * 4 : a >= 2 ? d * 2 : d; }
static void adj(uint8_t *p, int32_t d, int32_t lo, int32_t hi) { *p = (uint8_t)clampi((int32_t)*p + d, lo, hi); touch(); }

/* ---- breaks: the one of the pattern, choosing another */
static uint32_t brk_count(void) { return NFACT + nbrk_user; }
static void brk_choose(int32_t d)
{
    pat_t *p = cur_pat();
    int32_t b = (int32_t)p->brk, n = (int32_t)brk_count(), tries = NBRK;
    char t[28];
    while (tries--) {
        b = (b + d + (int32_t)NBRK) % (int32_t)NBRK;
        if (b < n && brk[b].data) break;
    }
    if ((uint32_t)b == p->brk) return;
    p->brk = (uint8_t)b;
    if (p->autog) { gen_orig(p, (uint32_t)b); p->autog = 1; }
    if (!sq_play && brk[b].bpm10) bpm10_set(brk[b].bpm10);   /* stopped: its own tempo */
    bank16 = 0;
    chop_k = 0;
    touch();
    t[0] = 0;
    str_cat(t, brk[b].name);
    toast(t);
}

/* ---- the knobs of each page */
typedef struct { const char *name; char val[14]; int32_t frac; uint8_t bip; } kv_t;   /* frac 0..1000, -1: none */
static void kv_int(kv_t *o, const char *name, int32_t v, int32_t lo, int32_t hi, const char *unit)
{
    char *p = o->val;
    o->name = name;
    o->val[0] = 0;
    p = fmt_u(p, (uint32_t)(v < 0 ? -v : v), 0);
    if (v < 0) { uint32_t i; for (i = (uint32_t)(p - o->val) + 1u; i > 0u; i--) o->val[i] = o->val[i - 1u]; o->val[0] = '-'; }
    if (unit) str_cat(o->val, unit);
    o->frac = hi > lo ? (v - lo) * 1000 / (hi - lo) : -1;
    o->bip = 0;
}
static void kv_txt(kv_t *o, const char *name, const char *v, int32_t frac)
{
    uint32_t i;
    o->name = name;
    for (i = 0; i < 13u && v[i]; i++) o->val[i] = v[i];
    o->val[i] = 0;
    o->frac = frac;
    o->bip = 0;
}
static const char *onoff(uint32_t v) { return v ? TR("OUI", "ON") : TR("NON", "OFF"); }

static void knob_info(uint32_t k, kv_t *o)
{
    params_t *p = &prj.p;
    pat_t *pt = cur_pat();
    uint32_t b = cur_brk(), s = sub[page];
    kv_txt(o, "", "", -1);
    switch (page) {
    case P_PLAY:
        if (k == 0) { kv_int(o, "PITCH", (int32_t)p->pitch - 24, -24, 24, 0); o->bip = 1; }
        if (k == 1) {
            kv_txt(o, "FILTER", p->filter < 60u ? "LOW" : p->filter > 68u ? "HIGH" : "OFF", p->filter * 1000 / 127);
            o->bip = 1;
        }
        if (k == 2) kv_int(o, "CRUSH", p->crush, 0, 15, 0);
        if (k == 3) kv_int(o, "DICE", p->d_chance, 0, 100, "%");
        break;
    case P_SEQ:
        if (held_step >= 0) {
            step_t *st = &pt->st[(uint32_t)held_step % NSTEP];
            if (k == 0) { if (st->sl) kv_int(o, TR("TRANCHE", "SLICE"), st->sl, 1, (int32_t)sl_count(b), 0); else kv_txt(o, TR("TRANCHE", "SLICE"), "--", 0); }
            if (k == 1) { kv_int(o, "PITCH", st->pitch, -24, 24, 0); o->bip = 1; }
            if (k == 2) { char t[4] = {'X', (char)('0' + ST_RAT(*st)), 0, 0}; kv_txt(o, "ROLL", t, ((int32_t)ST_RAT(*st) - 1) * 333); }
            if (k == 3) kv_int(o, TR("CHANCE", "CHANCE"), (int32_t)(8u - ST_CH(*st)) * 100 / 8, 0, 100, "%");
        } else {
            if (k == 0) kv_int(o, TR("LONG.", "LENGTH"), pat_len(pt), 1, 64, 0);
            if (k == 1) kv_txt(o, TR("PAS", "RATE"), RATE_NAME[pt->rate % RATE_N], (int32_t)pt->rate * 333);
            if (k == 2) kv_int(o, "SWING", p->swing, 50, 75, "%");
            if (k == 3) kv_int(o, TR("PINCEAU", "BRUSH"), brush, 1, (int32_t)sl_count(b), 0);
        }
        break;
    case P_SLICE: {
        slicing_t *sc = &prj.sc[b];
        if (k == 0) kv_txt(o, "MODE", SLM_NAME[sc->mode % SLM_N], sc->mode * 500);
        if (k == 1) {
            if (sc->mode == SLM_AUTO) kv_int(o, "SENS", sc->sens, 1, 10, 0);
            else if (sc->mode == SLM_GRID) kv_int(o, TR("NOMBRE", "COUNT"), sc->count, 2, 32, 0);
            else kv_int(o, TR("TRANCHES", "SLICES"), (int32_t)sl_count(b), 0, 32, 0);
        }
        if (k == 2) kv_int(o, TR("MESURES", "BARS"), sc->bars ? sc->bars : 1, 1, 8, 0);
        if (k == 3) kv_txt(o, "TEMPO", p->tmode ? "VARI" : "CHOP", p->tmode * 1000);
        break;
    }
    case P_CHOP: {
        slicing_t *sc = &prj.sc[b];
        uint32_t own = sc->nsl != 0u, kk = chop_k;
        if (s == 0u) {
            if (k == 0) { char t[14]; uint32_t ms = sl_start(b, kk) * 10u / (brk[b].rate / 100u); fmt_u(t, ms, 0); str_cat(t, "MS"); kv_txt(o, "START", t, (int32_t)(sl_start(b, kk) * 1000u / (brk[b].frames + 1u))); }
            if (k == 1) kv_txt(o, TR("FIN", "FINE"), "< >", -1);
            if (k == 2) { kv_int(o, "PITCH", own ? sc->pitch[kk % NSL] : 0, -24, 24, 0); o->bip = 1; }
            if (k == 3) kv_int(o, "GAIN", own && sc->gain[kk % NSL] ? sc->gain[kk % NSL] : 12, 0, 15, 0);
        } else {
            if (k == 0) kv_txt(o, "REV", onoff(own && (sc->rev >> kk & 1u)), own && (sc->rev >> kk & 1u) ? 1000 : 0);
            if (k == 1) kv_txt(o, TR("GENRE", "CLASS"), CLASS_NAME[sl_class(b, kk)], (int32_t)sl_class(b, kk) * 333);
            if (k == 2) kv_int(o, TR("TRANCHE", "SLICE"), (int32_t)kk + 1, 1, (int32_t)sl_count(b), 0);
            if (k == 3) kv_int(o, TR("TOTAL", "COUNT"), (int32_t)sl_count(b), 1, 32, 0);
        }
        break;
    }
    case P_AMP:
        if (k == 0) kv_int(o, "ATTACK", p->attack, 0, 15, 0);
        if (k == 1) { if (p->decay >= 15u) kv_txt(o, "DECAY", "HOLD", 1000); else kv_int(o, "DECAY", p->decay, 0, 15, 0); }
        if (k == 2) { char t[8]; fmt_u(t, p->length, 0); str_cat(t, "/16"); kv_txt(o, TR("LONG.", "LENGTH"), t, (int32_t)p->length * 1000 / 16); }
        if (k == 3) kv_txt(o, "CHOKE", p->choke ? "MONO" : "POLY", p->choke ? 1000 : 0);
        break;
    case P_DICE:
        if (s == 0u) {
            if (k == 0) kv_int(o, TR("CHANCE", "CHANCE"), p->d_chance, 0, 100, "%");
            if (k == 1) kv_int(o, "REV", p->d_rev, 0, 100, "%");
            if (k == 2) kv_int(o, "ROLL", p->d_roll, 0, 100, "%");
            if (k == 3) kv_int(o, "SKIP", p->d_skip, 0, 100, "%");
        } else if (s == 1u) {
            if (k == 0) kv_int(o, TR("SAUTS", "JUMPS"), p->d_pitch, 0, 100, "%");
            if (k == 1) kv_txt(o, "SMART", onoff(p->d_smart), p->d_smart * 1000);
            if (k == 2) {
                char t[10];
                if (!p->d_lock) kv_txt(o, "LOCK", TR("LIBRE", "FREE"), 0);
                else { fmt_u(t, p->d_lock, 0); str_cat(t, TR(" MES", " BAR")); kv_txt(o, "LOCK", t, p->d_lock * 250); }
            }
            if (k == 3) kv_int(o, "SEED", p->d_seed, 0, 99, 0);
        } else {
            if (k == 0) kv_txt(o, "STYLE", GEN_STYLE[p->g_style % GS_N], (int32_t)p->g_style * 1000 / (GS_N - 1u));
            if (k == 1) { char t[8] = "#"; fmt_u(t + 1, p->g_roll, 0); kv_txt(o, "ROLL", t, -1); }
            if (k == 2) kv_int(o, TR("DENS.", "DENS."), p->g_dens, 0, 100, "%");
            if (k == 3) kv_int(o, TR("VARIA.", "VARY"), p->g_var, 0, 100, "%");
        }
        break;
    case P_FLEX:
        if (k == 0) kv_txt(o, "MODE", FLEX_MODE[p->fl_mode % 3u], p->fl_mode * 500);
        if (k == 1) kv_txt(o, "QUANT", FLEX_QUANT[p->fl_quant % 3u], p->fl_quant * 500);
        if (k == 2) kv_int(o, TR("PROF.", "DEPTH"), p->fl_depth, 0, 100, "%");
        if (k == 3) kv_txt(o, "PITCH", onoff(p->fl_pitch), p->fl_pitch * 1000);
        break;
    case P_FX:
        if (s == 2u) {                          /* BEAT: the effect's INTENSITY, then the first page's dirt */
            if (k == 0) kv_int(o, "INTENS.", p->bt_int[beat_sel % 16u], 0, 100, "%");
            if (k == 1) { kv_txt(o, "FILTER", p->filter < 60u ? "LOW" : p->filter > 68u ? "HIGH" : "OFF", p->filter * 1000 / 127); o->bip = 1; }
            if (k == 2) kv_int(o, "DRIVE", p->drive, 0, 15, 0);
            if (k == 3) kv_int(o, "CRUSH", p->crush, 0, 15, 0);
        } else if (s == 0u) {
            if (k == 0) { kv_txt(o, "FILTER", p->filter < 60u ? "LOW" : p->filter > 68u ? "HIGH" : "OFF", p->filter * 1000 / 127); o->bip = 1; }
            if (k == 1) kv_int(o, "RESO", p->reso, 0, 15, 0);
            if (k == 2) kv_int(o, "DRIVE", p->drive, 0, 15, 0);
            if (k == 3) kv_int(o, "CRUSH", p->crush, 0, 15, 0);
        } else {
            if (k == 0) kv_txt(o, "DELAY", DLY_NAME[p->dly_time % 6u], p->dly_time * 200);
            if (k == 1) kv_int(o, TR("RETOUR", "FDBK"), p->dly_fb, 0, 15, 0);
            if (k == 2) kv_int(o, "MIX", p->dly_mix, 0, 15, 0);
            if (k == 3) kv_int(o, "PUNCH", p->punch, 0, 15, 0);
        }
        break;
    case P_GLOBAL:
        if (s == 0u) {
            if (k == 0) { char t[10]; fmt_bpm(t, bpm10()); kv_txt(o, "BPM", t, (int32_t)(bpm10() - 400u) * 1000 / 2600); }
            if (k == 1) kv_int(o, "SWING", p->swing, 50, 75, "%");
            if (k == 2) kv_txt(o, "TEMPO", p->tmode ? "VARI" : "CHOP", p->tmode * 1000);
            if (k == 3) kv_txt(o, "METRO", p->metro == 0u ? "OFF" : p->metro == 1u ? "REC" : "ON", p->metro * 500);
        } else {
            if (k == 0) kv_txt(o, TR("HORLOGE", "CLOCK"), p->clock ? "MIDI" : "INT", p->clock * 1000);
            if (k == 1) kv_txt(o, TR("LANGUE", "LANG"), lang ? "ENGLISH" : "FRANCAIS", lang * 1000);
            if (k == 2) kv_txt(o, TR("THEME", "THEME"), THEMES[theme % NTHEME].name, (int32_t)theme * 333);
            if (k == 3) kv_int(o, "VOL", p->vol, 0, 15, 0);
        }
        break;
    case P_SAVE: {
        static const char *const ACT_EN[5] = {"SAVE", "LOAD", "CLEAR PAT", "COPY PAT", "NEW"};
        static const char *const ACT_FR[5] = {"SAUVER", "CHARGER", "EFFACER", "COPIER", "NOUVEAU"};
        if (k == 0) kv_txt(o, "ACTION", lang ? ACT_EN[save_act % 5u] : ACT_FR[save_act % 5u], save_act * 250);
        if (k == 1) kv_int(o, TR("VERS", "TO PAT"), save_tgt + 1, 1, 16, 0);
        if (k == 2) { static const char *const SN[4] = {"AUTO", "A", "B", "C"}; kv_txt(o, "SLOT", SN[save_slot % 4u], save_slot * 333); }
        if (k == 3) kv_int(o, "PAT", prj.cur_pat + 1, 1, 16, 0);
        break;
    }
    default:
        break;
    }
}

static void regen(void)                         /* GENERATE: the current pattern, from the knobs */
{
    pat_t *pt = cur_pat();
    if (!gen_kept) { undo_keep(prj.cur_pat); gen_kept = 1; }
    gen_pattern(pt, pt->brk, prj.p.g_style, prj.p.g_roll, prj.p.g_dens, prj.p.g_var);
    touch();
}

static void knob_turn(uint32_t k, int32_t d)
{
    params_t *p = &prj.p;
    pat_t *pt = cur_pat();
    uint32_t b = cur_brk(), s = sub[page];
    switch (page) {
    case P_PLAY:
        if (k == 0) adj(&p->pitch, d, 0, 48);
        if (k == 1) adj(&p->filter, accel(d) * 2, 0, 127);
        if (k == 2) adj(&p->crush, d, 0, 15);
        if (k == 3) adj(&p->d_chance, accel(d) * 2, 0, 100);
        break;
    case P_SEQ:
        if (held_step >= 0) {
            step_t *st = &pt->st[(uint32_t)held_step % NSTEP];
            uint32_t n = sl_count(b);
            held_used = 1;
            if (k == 0) { st->sl = (uint8_t)clampi((int32_t)st->sl + d, 1, (int32_t)(n ? n : 1u)); brush = st->sl; }
            if (k == 1) st->pitch = (int8_t)clampi(st->pitch + d, -24, 24);
            if (k == 2) { int32_t r = clampi((int32_t)ST_RAT(*st) + d, 1, 4); st->fl = (uint8_t)((st->fl & ~0x0Cu) | (uint32_t)(r - 1) << 2); }
            if (k == 3) { int32_t c = clampi((int32_t)ST_CH(*st) - d, 0, 7); st->fl = (uint8_t)((st->fl & ~0x70u) | (uint32_t)c << 4); }
            pt->autog = 0;
            touch();
        } else {
            if (k == 0) { pt->len = (uint8_t)clampi((int32_t)pat_len(pt) + accel(d), 1, 64); pt->autog = 0; touch(); }
            if (k == 1) { pt->rate = (uint8_t)clampi((int32_t)pt->rate + d, 0, RATE_N - 1); touch(); }
            if (k == 2) adj(&p->swing, d, 50, 75);
            if (k == 3) brush = (uint8_t)clampi((int32_t)brush + d, 1, (int32_t)(sl_count(b) ? sl_count(b) : 1u));
        }
        break;
    case P_SLICE: {
        slicing_t *sc = &prj.sc[b];
        if (k == 0) { sc->mode = (uint8_t)clampi((int32_t)sc->mode + d, 0, SLM_N - 1); slice_request(b); }
        if (k == 1) {
            if (sc->mode == SLM_AUTO) { sc->sens = (uint8_t)clampi((int32_t)sc->sens + d, 1, 10); slice_request(b); }
            else if (sc->mode == SLM_GRID) { sc->count = (uint8_t)clampi((int32_t)sc->count + d, 2, 32); slice_request(b); }
        }
        if (k == 2) {
            static const uint8_t BARS[4] = {1, 2, 4, 8};
            int32_t i = sc->bars >= 8u ? 3 : sc->bars >= 4u ? 2 : sc->bars >= 2u ? 1 : 0;
            i = clampi(i + d, 0, 3);
            sc->bars = BARS[i];
            brk[b].bpm10 = brk_bpm10(b, sc->bars);
            if (!sq_play) bpm10_set(brk[b].bpm10);
            slicing_changed(b);
        }
        if (k == 3) adj(&p->tmode, d, 0, 1);
        touch();
        break;
    }
    case P_CHOP: {
        uint32_t n = sl_count(b);
        if (!n) break;
        if (chop_k >= n) chop_k = (uint8_t)(n - 1u);
        if (s == 0u) {
            if (k == 0) slice_move(b, chop_k, accel(d) * (int32_t)(brk[b].rate / 200u));   /* 5 ms a click */
            if (k == 1) slice_move(b, chop_k, d * 8);
            if (k == 2) { slicing_t *sc = slicing_own(b); sc->pitch[chop_k] = (int8_t)clampi(sc->pitch[chop_k] + d, -24, 24); touch(); }
            if (k == 3) { slicing_t *sc = slicing_own(b); sc->gain[chop_k] = (uint8_t)clampi((int32_t)(sc->gain[chop_k] ? sc->gain[chop_k] : 12) + d, 1, 15); touch(); }
        } else {
            if (k == 0) { slicing_t *sc = slicing_own(b); if (d > 0) sc->rev |= 1u << chop_k; else sc->rev &= ~(1u << chop_k); touch(); }
            if (k == 1) { slicing_t *sc = slicing_own(b); sc->cls[chop_k] = (uint8_t)clampi((int32_t)sc->cls[chop_k] + d, 0, 3); touch(); }
            if (k == 2) chop_k = (uint8_t)clampi((int32_t)chop_k + d, 0, (int32_t)n - 1);
        }
        break;
    }
    case P_AMP:
        if (k == 0) adj(&p->attack, d, 0, 15);
        if (k == 1) adj(&p->decay, d, 0, 15);
        if (k == 2) adj(&p->length, d, 1, 16);
        if (k == 3) adj(&p->choke, d, 0, 1);
        break;
    case P_DICE:
        if (s == 0u) {
            if (k == 0) adj(&p->d_chance, accel(d) * 2, 0, 100);
            if (k == 1) adj(&p->d_rev, accel(d) * 2, 0, 100);
            if (k == 2) adj(&p->d_roll, accel(d) * 2, 0, 100);
            if (k == 3) adj(&p->d_skip, accel(d) * 2, 0, 100);
        } else if (s == 1u) {
            if (k == 0) adj(&p->d_pitch, accel(d) * 2, 0, 100);
            if (k == 1) adj(&p->d_smart, d, 0, 1);
            if (k == 2) { static const uint8_t L[4] = {0, 1, 2, 4}; int32_t i = p->d_lock >= 4u ? 3 : p->d_lock >= 2u ? 2 : p->d_lock; i = clampi(i + d, 0, 3); p->d_lock = L[i]; touch(); }
            if (k == 3) adj(&p->d_seed, d, 0, 99);
        } else {
            if (k == 0) { adj(&p->g_style, d, 0, GS_N - 1); regen(); }
            if (k == 1) { p->g_roll = (uint8_t)clampi((int32_t)p->g_roll + d, 0, 255); regen(); }
            if (k == 2) { adj(&p->g_dens, accel(d) * 5, 0, 100); regen(); }
            if (k == 3) { adj(&p->g_var, accel(d) * 5, 0, 100); regen(); }
        }
        break;
    case P_FLEX:
        if (k == 0) adj(&p->fl_mode, d, 0, 2);
        if (k == 1) adj(&p->fl_quant, d, 0, 2);
        if (k == 2) adj(&p->fl_depth, accel(d) * 5, 0, 100);
        if (k == 3) adj(&p->fl_pitch, d, 0, 1);
        break;
    case P_FX:
        if (s == 2u) {
            if (k == 0) adj(&p->bt_int[beat_sel % 16u], accel(d) * 2, 0, 100);
            if (k == 1) adj(&p->filter, accel(d) * 2, 0, 127);
            if (k == 2) adj(&p->drive, d, 0, 15);
            if (k == 3) adj(&p->crush, d, 0, 15);
        } else if (s == 0u) {
            if (k == 0) adj(&p->filter, accel(d) * 2, 0, 127);
            if (k == 1) adj(&p->reso, d, 0, 15);
            if (k == 2) adj(&p->drive, d, 0, 15);
            if (k == 3) adj(&p->crush, d, 0, 15);
        } else {
            if (k == 0) adj(&p->dly_time, d, 0, 5);
            if (k == 1) adj(&p->dly_fb, d, 0, 15);
            if (k == 2) adj(&p->dly_mix, d, 0, 15);
            if (k == 3) adj(&p->punch, d, 0, 15);
        }
        break;
    case P_GLOBAL:
        if (s == 0u) {
            if (k == 0) { bpm10_set((uint32_t)clampi((int32_t)bpm10() + accel(d), 400, 3000)); touch(); }
            if (k == 1) adj(&p->swing, d, 50, 75);
            if (k == 2) adj(&p->tmode, d, 0, 1);
            if (k == 3) adj(&p->metro, d, 0, 2);
        } else {
            if (k == 0) adj(&p->clock, d, 0, 1);
            if (k == 1) { lang = (uint8_t)clampi((int32_t)lang + d, 0, 1); sets.lang = lang; settings_save(&sets, sizeof sets); }
            if (k == 2) { theme = (uint8_t)clampi((int32_t)theme + d, 0, NTHEME - 1); sets.theme = theme; settings_save(&sets, sizeof sets); g_force = 1; }
            if (k == 3) adj(&p->vol, d, 0, 15);
        }
        break;
    case P_SAVE:
        if (k == 0) { save_act = (uint8_t)clampi((int32_t)save_act + d, 0, 4); save_arm = 0; }
        if (k == 1) save_tgt = (uint8_t)clampi((int32_t)save_tgt + d, 0, NPAT - 1);
        if (k == 2) { save_slot = (uint8_t)clampi((int32_t)save_slot + d, 0, NSLOT - 1); save_arm = 0; }
        if (k == 3) { prj.cur_pat = (uint8_t)clampi((int32_t)prj.cur_pat + d, 0, NPAT - 1); }
        break;
    default:
        break;
    }
}

/* SAVE's action: the SAVE button again does it; LOAD, CLEAR and NEW ask a second press */
static void save_do(void)
{
    char t[28];
    uint32_t need = save_act == 1u || save_act == 2u || save_act == 4u;
    if (need && (!save_arm || fm1_ms - save_arm > 3000u)) { save_arm = fm1_ms; toast(TR("ENCORE SAVE: OUI", "SAVE AGAIN: SURE")); return; }
    save_arm = 0;
    switch (save_act) {
    case 0:
        if (sq_play) { toast(TR("STOP D'ABORD", "STOP FIRST")); return; }
        prj.bank_crc = bank_crc;
        toast(prj_save(save_slot) ? TR("ERREUR FLASH", "FLASH ERROR") : TR("SAUVE !", "SAVED !"));
        break;
    case 1:
        if (sq_play) { toast(TR("STOP D'ABORD", "STOP FIRST")); return; }
        if (prj_load(save_slot)) { toast(TR("SLOT VIDE", "EMPTY SLOT")); return; }
        if (prj.bank_crc != bank_crc) { uint32_t i; for (i = NFACT; i < NBRK; i++) prj.sc[i].nsl = 0; }
        toast(TR("CHARGE !", "LOADED !"));
        break;
    case 2: {
        pat_t *pt = cur_pat();
        undo_keep(prj.cur_pat);
        memset(pt->st, 0, sizeof pt->st);
        pt->autog = 0;
        touch();
        toast(TR("PATTERN EFFACE", "PATTERN CLEARED"));
        break;
    }
    case 3:
        undo_keep(save_tgt);
        prj.pat[save_tgt] = *cur_pat();
        touch();
        t[0] = 0;
        str_cat(t, TR("COPIE VERS ", "COPIED TO "));
        fmt_u(t + str_len(t), save_tgt + 1u, 0);
        toast(t);
        break;
    case 4:
        if (sq_play) { toast(TR("STOP D'ABORD", "STOP FIRST")); return; }
        prj_defaults();
        touch();
        toast(TR("NOUVEAU PROJET", "NEW PROJECT"));
        break;
    default:
        break;
    }
}

/* ---- drawing */
static void draw_header(void)
{
    char t[28];
    int32_t x, w;
    uint32_t b = cur_brk();
    if (!g_vis(0, 26)) return;
    g_rect(0, 0, 240, 25, C_BG);
    w = g_tw(&FONT_BM, PAGE_NAME[page]) + 14;
    g_round(3, 3, w, 19, C_ACC);
    g_text(&FONT_BM, 10, 5, PAGE_NAME[page], C_BG);
    x = 3 + w + 6;
    if (PAGE_SUBS[page] > 1u) {
        uint32_t i;
        for (i = 0; i < PAGE_SUBS[page]; i++) g_rect(x + (int32_t)i * 6, 11, 4, 4, i == sub[page] ? C_ACC2 : C_EDGE);
        x += (int32_t)PAGE_SUBS[page] * 6 + 4;
    }
    t[0] = 'P';
    fmt_u(t + 1, prj.cur_pat + 1u, 2);
    if (pat_next < NPAT) { str_cat(t, " > "); fmt_u(t + str_len(t), pat_next + 1u, 2); }
    g_text(&FONT_BS, x, 0, t, pat_next < NPAT && (fm1_ms / 200u & 1u) ? C_ACC : C_SUB);
    g_textw(&FONT_BS, x, 12, 168 - x, brk[b].name, C_DIMT);
    fmt_bpm(t, bpm10());
    g_textr(&FONT_BM, 220, 5, t, prj.p.clock ? C_ACC2 : C_TXT);
    if (sq_rec) g_dot(230, 12, 5, (fm1_ms / 300u & 1u) || !sq_play ? C_REC : RGB(120, 20, 20));
    else if (sq_play) g_play(226, 6, 13, C_PLAY);
    else g_rect(225, 7, 10, 10, C_SUB);
    g_rect(0, 25, 240, 1, C_EDGE);
}

static void draw_knobs(void)
{
    uint32_t k;
    if (!g_vis(198, 42)) return;
    g_rect(0, 197, 240, 43, C_BG);
    g_rect(0, 197, 240, 1, C_EDGE);
    for (k = 0; k < 4u; k++) {
        kv_t o;
        int32_t x = (int32_t)k * 60;
        knob_info(k, &o);
        if (!o.name[0]) continue;
        g_rect(x + 4, 201, 52, 3, C_PANEL2);
        if (o.frac >= 0) {
            if (o.bip) {
                int32_t m = x + 30, e = x + 4 + o.frac * 52 / 1000;
                if (e >= m) g_rect(m, 201, e - m + 1, 3, KNOB_C[k]);
                else g_rect(e, 201, m - e, 3, KNOB_C[k]);
                g_rect(m, 200, 1, 5, C_SUB);
            } else {
                g_rect(x + 4, 201, 2 + o.frac * 50 / 1000, 3, KNOB_C[k]);
            }
        } else {
            g_rect(x + 4, 201, 52, 3, g_mix(C_PANEL2, KNOB_C[k], 90));
        }
        g_textw(&FONT_BS, x + 4, 207, 54, o.name, g_mix(C_SUB, KNOB_C[k], 70));
        g_fit(x + 3, 222, 56, o.val, C_TXT);
        if (k < 3u) g_rect(x + 59, 206, 1, 30, C_PANEL2);
    }
}

/* the waveform of break b in [y, y+h): the slices tinted, their numbers, the playing ones lit */
static void draw_wave(int32_t y, int32_t h, uint32_t b, uint32_t numbers)
{
    uint32_t n = sl_count(b), k, col, snd = sl_sounding, fr = brk[b].frames ? brk[b].frames : 1u;
    int32_t mid = y + h / 2;
    if (!g_vis(y, h)) return;
    g_round(2, y, 236, h, C_PANEL);
    for (k = 0; k < n; k++) {                   /* the slices' backgrounds */
        int32_t x0 = 4 + (int32_t)(sl_start(b, k) / 16u * WAVE_W / (fr / 16u + 1u));
        int32_t x1 = 4 + (int32_t)(sl_end(b, k) / 16u * WAVE_W / (fr / 16u + 1u));
        uint32_t on = k < 32u && (snd >> k & 1u);
        if (on) g_blend(x0, y + 1, x1 - x0, h - 2, slice_col(k), 90);
        else if (k & 1u) g_blend(x0, y + 1, x1 - x0, h - 2, C_PANEL2, 128);
    }
    if (an.b == b) {
        for (col = 0; col < WAVE_W; col++) {
            int32_t a = an.wave[col] * (h / 2 - 3) / 127, x = 4 + (int32_t)col;
            uint32_t sk = 0;
            for (k = 0; k + 1u < n && (uint32_t)(x - 4) >= sl_start(b, k + 1u) / 16u * WAVE_W / (fr / 16u + 1u); k++) {}
            sk = k;
            g_rect(x, mid - a, 1, 2 * a + 1, (sk < 32u && (snd >> sk & 1u)) ? slice_col(sk) : g_mix(C_SUB, slice_col(sk), 70));
        }
        if (!an.done) g_textc(&FONT_BS, 120, mid - 6, "...", C_SUB);
    }
    for (k = 1; k < n; k++) {                   /* the cuts */
        int32_t x = 4 + (int32_t)(sl_start(b, k) / 16u * WAVE_W / (fr / 16u + 1u));
        g_rect(x, y + 2, 1, h - 4, g_mix(C_PANEL, C_TXT, 110));
    }
    if (numbers && n <= 32u && g_vis(y, 14)) {   /* the ruler: the slices' numbers on a dark strip */
        g_blend(3, y + 1, 234, 12, C_BG, 170);
        for (k = 0; k < n; k++) {
            int32_t x0 = 4 + (int32_t)(sl_start(b, k) / 16u * WAVE_W / (fr / 16u + 1u));
            int32_t x1 = 4 + (int32_t)(sl_end(b, k) / 16u * WAVE_W / (fr / 16u + 1u));
            char t[4];
            fmt_u(t, k + 1u, 0);
            if (x1 - x0 < g_tw(&FONT_BS, t) + 2) continue;
            g_textc(&FONT_BS, (x0 + x1) / 2, y, t, (k < 32u && (snd >> k & 1u)) ? slice_col(k) : C_SUB);
        }
    }
    {                                           /* the playheads: where the voices read */
        uint32_t j;
        for (j = 0; j < NVOICE; j++)
            if (vo[j].on && vo[j].b == b && !vo[j].twin) {
                int32_t x = 4 + (int32_t)((uint32_t)vo[j].pos / 16u * WAVE_W / (fr / 16u + 1u));
                g_rect(x, y + 1, 1, h - 2, C_TXT);
            }
    }
}

static void draw_pads(int32_t y, uint32_t b, uint32_t base)   /* 16 slices as two rows of 8 */
{
    uint32_t i, n = sl_count(b), snd = sl_sounding;
    if (!g_vis(y, 36)) return;
    for (i = 0; i < 16u; i++) {
        uint32_t k = base + i, on = k < 32u && (snd >> k & 1u);
        int32_t x = 3 + (int32_t)(i % 8u) * 29, yy = y + (int32_t)(i / 8u) * 18;
        char t[4];
        if (k >= n) { g_outline(x, yy, 27, 16, C_PANEL2); continue; }
        g_round(x, yy, 27, 16, on ? slice_col(k) : g_mix(C_PANEL, slice_col(k), 46));
        fmt_u(t, k + 1u, 0);
        g_text(&FONT_BS, x + 3, yy + 2, t, on ? C_BG : C_TXT);
        g_textr(&FONT_BS, x + 25, yy + 2, CLASS_NAME[sl_class(b, k)], on ? C_BG : CLS_C[sl_class(b, k)]);
    }
}

static void draw_fx_chips(int32_t y)            /* the 11 punch effects, laid out as the black keys */
{
    uint32_t i, act = fx_active;
    if (!g_vis(y, 40)) return;
    for (i = 0; i < NPUNCH; i++) {
        int32_t row = i < 6u ? 0 : 1, col = i < 6u ? (int32_t)i : (int32_t)i - 6, x = 3 + col * 39 + row * 19, yy = y + row * 20;
        uint32_t on = act >> i & 1u;
        g_round(x, yy, 37, 18, on ? C_ACC : C_PANEL);
        g_textc(&FONT_BS, x + 18, yy + 3, PUNCH_SHORT[i], on ? C_BG : C_SUB);
    }
}

static void draw_play(void)
{
    uint32_t b = cur_brk(), i, act = fx_active;
    draw_wave(29, 70, b, 1);
    draw_pads(102, b, bank16 * 16u);
    draw_fx_chips(140);
    if (g_vis(182, 14)) {                       /* the state: what acts now */
        char t[40];
        t[0] = 0;
        for (i = 0; i < NPUNCH; i++) if (act >> i & 1u) { str_cat(t, PUNCH_LONG[i]); break; }
        if (!t[0] && flex_on < NFLEX) { str_cat(t, "FLEX "); str_cat(t, FLEX[flex_on].name); }
        if (!t[0] && prj.p.d_chance) { str_cat(t, TR("DES ", "DICE ")); fmt_u(t + str_len(t), prj.p.d_chance, 0); str_cat(t, "%"); }
        g_text(&FONT_BS, 5, 183, t, C_ACC2);
        if (sl_count(b) > 16u) g_textr(&FONT_BS, 236, 183, bank16 ? TR("TRANCHES 17-32", "SLICES 17-32") : TR("TRANCHES 1-16", "SLICES 1-16"), C_DIMT);
        else if (sq_rec) g_textr(&FONT_BS, 236, 183, "REC", C_REC);
    }
}

static void draw_seq(void)
{
    pat_t *pt = cur_pat();
    uint32_t b = cur_brk(), len = pat_len(pt), i, pos = sq_pos, pages = (len + 15u) / 16u;
    char t[24];
    if (seq_page >= pages) seq_page = (uint8_t)(pages ? pages - 1u : 0u);
    if (g_vis(28, 16)) {
        g_rect(0, 26, 240, 18, C_BG);
        for (i = 0; i < 4u; i++) {
            uint32_t have = i < pages;
            g_round(4 + (int32_t)i * 30, 30, 26, 10, i == seq_page ? C_ACC : have ? C_PANEL2 : C_PANEL);
            if (sq_play && pos < len && pos / 16u == i) g_rect(4 + (int32_t)i * 30 + 2 + (int32_t)(pos % 16u) * 22 / 16, 32, 2, 6, C_TXT);
        }
        t[0] = 0;
        fmt_u(t, len, 0);
        str_cat(t, TR(" PAS ", " STEPS "));
        str_cat(t, RATE_NAME[pt->rate % RATE_N]);
        g_textr(&FONT_BS, 236, 29, t, C_SUB);
    }
    for (i = 0; i < 16u; i++) {
        uint32_t s = seq_page * 16u + i;
        int32_t x = 3 + (int32_t)(i % 8u) * 29, y = 48 + (int32_t)(i / 8u) * 56;
        const step_t *st = &pt->st[s % NSTEP];
        uint32_t here = sq_play && s == pos, held = held_step >= 0 && (uint32_t)held_step == s;
        uint16_t c;
        if (!g_vis(y, 52)) continue;
        if (s >= len) { g_outline(x, y, 27, 52, C_PANEL); continue; }
        c = st->sl ? slice_col(st->sl - 1u) : C_PANEL;
        g_round(x, y, 27, 52, st->sl ? g_mix(C_PANEL, c, here ? 200 : 70) : (here ? C_PANEL2 : C_PANEL));
        if (held) g_outline(x, y, 27, 52, C_TXT);
        else if (i % 4u == 0u) g_rect(x + 2, y + 50, 23, 1, C_EDGE);
        if (st->sl) {
            fmt_u(t, st->sl, 0);
            g_textc(&FONT_BM, x + 14, y + 6, t, here ? C_BG : C_TXT);
            g_textc(&FONT_BS, x + 14, y + 23, CLASS_NAME[sl_class(b, st->sl - 1u)], here ? C_BG : CLS_C[sl_class(b, st->sl - 1u)]);
            if (ST_RAT(*st) > 1u) { uint32_t r; for (r = 0; r < ST_RAT(*st); r++) g_rect(x + 4 + (int32_t)r * 5, y + 38, 3, 3, C_ACC2); }
            if (st->fl & SF_REV) g_text(&FONT_BS, x + 3, y + 36, "<", C_ACC);
            if (st->pitch) { fmt_s(t, st->pitch); g_textc(&FONT_BS, x + 14, y + 36, t, C_ACC2); }
            if (ST_CH(*st)) g_rect(x + 3, y + 46, 21 - (int32_t)ST_CH(*st) * 3, 2, C_SUB);
            if (st->fx) g_dot(x + 22, y + 6, 2, C_ACC);
        } else {
            g_textc(&FONT_BS, x + 14, y + 20, "-", C_DIMT);
        }
    }
    if (g_vis(162, 34)) {
        if (held_step >= 0) {
            const step_t *st = &pt->st[(uint32_t)held_step % NSTEP];
            t[0] = 0;
            str_cat(t, TR("PAS ", "STEP "));
            fmt_u(t + str_len(t), (uint32_t)held_step + 1u, 0);
            g_text(&FONT_BM, 6, 168, t, C_TXT);
            g_text(&FONT_BS, 80, 171, st->fx ? PUNCH_LONG[(st->fx - 1u) % NPUNCH] : TR("NOIRES: EFFET", "BLACK: FX LOCK"), st->fx ? C_ACC : C_DIMT);
        } else {
            g_text(&FONT_BS, 6, 166, TR("BLANCHES: POSER / OTER", "WHITE: PUT / CLEAR"), C_DIMT);
            g_text(&FONT_BS, 6, 180, TR("TENIR UN PAS: L'EDITER", "HOLD A STEP: EDIT IT"), C_DIMT);
            fmt_u(t, brush, 0);
            g_round(196, 165, 40, 26, slice_col(brush - 1u));
            g_textc(&FONT_BM, 216, 170, t, C_BG);
        }
    }
}

static void draw_slice(void)
{
    uint32_t b = cur_brk();
    slicing_t *sc = &prj.sc[b];
    char t[28];
    draw_wave(29, 96, b, 1);
    if (g_vis(130, 66)) {
        uint32_t ms = brk[b].frames / (brk[b].rate / 100u) * 10u;
        t[0] = 0;
        fmt_u(t, ms / 1000u, 0);
        str_cat(t, ".");
        fmt_u(t + str_len(t), ms / 100u % 10u, 0);
        str_cat(t, " S   ");
        fmt_u(t + str_len(t), brk[b].rate, 0);
        str_cat(t, " HZ");
        g_text(&FONT_BS, 6, 132, t, C_SUB);
        t[0] = 0;
        fmt_bpm(t, brk[b].bpm10);
        str_cat(t, " BPM");
        g_textr(&FONT_BM, 236, 130, t, C_ACC2);
        g_text(&FONT_BM, 6, 150, brk[b].name, C_TXT);
        t[0] = 0;
        fmt_u(t, sl_count(b), 0);
        str_cat(t, TR(" TRANCHES", " SLICES"));
        g_textr(&FONT_BS, 236, 153, t, C_SUB);
        g_text(&FONT_BS, 6, 172, sc->mode == SLM_AUTO ? TR("AUTO: CHAQUE COUP", "AUTO: EVERY HIT") :
                                 sc->mode == SLM_GRID ? TR("GRILLE: PARTS EGALES", "GRID: EQUAL PARTS") :
                                 TR("TEL QUEL: SES COUPES", "AS IS: ITS OWN CUTS"), C_DIMT);
        if (slice_want_b < NBRK || an.busy) g_textr(&FONT_BS, 236, 172, TR("ANALYSE...", "LOOKING..."), C_ACC);
        g_text(&FONT_BS, 6, 184, TR("SELECT: CHANGER DE BREAK", "SELECT: ANOTHER BREAK"), C_DIMT);
    }
}

/* CHOP: a zoom on the slice's start (its own peaks, made when the slice changes) */
static uint8_t zoom_pk[232];
static uint32_t zoom_key = 0xFFFFFFFFu;
static void zoom_make(uint32_t b, uint32_t k)
{
    uint32_t s = sl_start(b, k), span = brk[b].rate / 6u, from = s > span / 4u ? s - span / 4u : 0, col, blk = 0xFFFFFFFFu;
    uint32_t key = b << 24 ^ k << 16 ^ s;
    if (key == zoom_key) return;
    zoom_key = key;
    for (col = 0; col < 232u; col++) {
        uint32_t f0 = from + col * span / 232u, f1 = from + (col + 1u) * span / 232u, f, pk = 0;
        for (f = f0; f < f1 && f < brk[b].frames; f++) {
            int32_t x;
            if (f / IMA_SPB != blk) { blk = f / IMA_SPB; ima_block(brk[b].data + blk * IMA_BLOCK, an_pcm, brk[b].frames - blk * IMA_SPB, 0); }
            x = an_pcm[f - blk * IMA_SPB];
            if ((uint32_t)(x < 0 ? -x : x) > pk) pk = (uint32_t)(x < 0 ? -x : x);
        }
        zoom_pk[col] = (uint8_t)(pk >> 8);
    }
}

static void draw_chop(void)
{
    uint32_t b = cur_brk(), n = sl_count(b), k = chop_k < n ? chop_k : 0, col;
    char t[28];
    if (g_vis(29, 92)) {
        uint32_t s = sl_start(b, k), span = brk[b].rate / 6u, from = s > span / 4u ? s - span / 4u : 0;
        int32_t mid = 29 + 46, mk = (int32_t)((s - from) * 232u / span) + 4;
        g_round(2, 29, 236, 92, C_PANEL);
        for (col = 0; col < 232u; col++) {
            int32_t a = zoom_pk[col] * 42 / 127;
            g_rect(4 + (int32_t)col, mid - a, 1, 2 * a + 1, 4 + (int32_t)col >= mk ? slice_col(k) : C_DIMT);
        }
        g_rect(mk, 31, 2, 88, C_TXT);
        g_text(&FONT_BS, 6, 31, "10 MS", C_DIMT);
        g_rect(6, 44, (int32_t)(brk[b].rate / 100u * 232u / span), 1, C_DIMT);
    }
    draw_wave(125, 34, b, 0);
    if (g_vis(125, 34) && n) {                  /* the chosen slice, outlined in the overview */
        uint32_t fr = brk[b].frames / 16u + 1u;
        int32_t x0 = 4 + (int32_t)(sl_start(b, k) / 16u * WAVE_W / fr), x1 = 4 + (int32_t)(sl_end(b, k) / 16u * WAVE_W / fr);
        g_outline(x0, 125, x1 - x0 + 1 < 3 ? 3 : x1 - x0 + 1, 34, C_TXT);
    }
    if (g_vis(162, 34)) {
        t[0] = 0;
        str_cat(t, TR("TRANCHE ", "SLICE "));
        fmt_u(t + str_len(t), k + 1u, 0);
        str_cat(t, " / ");
        fmt_u(t + str_len(t), n, 0);
        g_text(&FONT_BM, 6, 164, t, slice_col(k));
        g_textr(&FONT_BS, 236, 166, CLASS_NAME[sl_class(b, k)][0] == '-' ? TR("AUTRE", "OTHER") :
                sl_class(b, k) == CL_K ? "KICK" : sl_class(b, k) == CL_S ? "SNARE" : "HAT", CLS_C[sl_class(b, k)]);
        g_text(&FONT_BS, 6, 184, TR("OCT+ COUPER   OCT- JOINDRE", "OCT+ SPLIT   OCT- JOIN"), C_DIMT);
    }
}

static void draw_amp(void)
{
    params_t *p = &prj.p;
    if (g_vis(29, 120)) {
        int32_t x0 = 12, base = 140, top = 44, xa, xh, i;
        g_round(2, 29, 236, 120, C_PANEL);
        xa = x0 + (int32_t)p->attack * 4 + 2;
        xh = 30 + (int32_t)p->length * 12;
        if (xh < xa + 4) xh = xa + 4;
        for (i = x0; i < 228; i++) {
            int32_t v;
            if (i < xa) v = (i - x0) * (base - top) / (xa - x0);
            else if (p->decay >= 15u) v = i < xh ? base - top : 0;
            else {
                int32_t d = (i - xa), tau = 6 + (int32_t)p->decay * 9;
                v = (base - top) * tau / (tau + d * 2);
                if (i >= xh) v = 0;
            }
            if (i >= xh && i < xh + 4) v = (base - top) * (xh + 4 - i) / 8;
            if (v > 0) g_rect(i, base - v, 1, v, g_mix(C_PANEL, C_ACC, 70));
            if (v > 0) g_rect(i, base - v, 1, 2, C_ACC);
        }
        g_rect(x0, base, 216, 1, C_EDGE);
    }
    if (g_vis(152, 44)) {
        uint32_t j, used = 0;
        for (j = 0; j < NVOICE; j++) used += vo[j].on;
        g_text(&FONT_BM, 6, 156, p->choke ? TR("MONO: COUPE", "MONO: CHOPS") : TR("POLY: RESONNE", "POLY: RINGS"), C_TXT);
        for (j = 0; j < NVOICE; j++) g_round(160 + (int32_t)j * 13, 157, 10, 12, j < used ? C_ACC : C_PANEL2);
        g_text(&FONT_BS, 6, 178, p->gate ? TR("GATE: TANT QUE TENUE", "GATE: WHILE HELD") : TR("AMP ENCORE: GATE", "AMP AGAIN: GATE"), C_DIMT);
    }
}

static void draw_dice(void)
{
    params_t *p = &prj.p;
    uint32_t s = sub[P_DICE], i;
    char t[24];
    if (s < 2u) {
        static const char *const N0[4] = {"CHANCE", "REV", "ROLL", "SKIP"};
        static const char *const N1[4] = {"JUMPS", "SMART", "LOCK", "SEED"};
        uint32_t v[4];
        if (s == 0u) { v[0] = p->d_chance; v[1] = p->d_rev; v[2] = p->d_roll; v[3] = p->d_skip; }
        else { v[0] = p->d_pitch; v[1] = p->d_smart * 100u; v[2] = p->d_lock * 25u; v[3] = p->d_seed; }
        for (i = 0; i < 4u; i++) {
            int32_t x = 6 + (int32_t)i * 58, h = (int32_t)v[i] * 96 / 100;
            if (!g_vis(30, 130)) break;
            g_round(x, 32, 50, 104, C_PANEL);
            if (h > 0) g_round(x + 2, 134 - h, 46, h, g_mix(C_PANEL, KNOB_C[i], 150));
            g_textc(&FONT_BS, x + 25, 140, s ? N1[i] : N0[i], C_SUB);
        }
        if (g_vis(156, 40)) {
            g_text(&FONT_BS, 6, 160, s == 0u ? TR("LE HASARD, A CHAQUE PAS", "RANDOM, ON EVERY STEP") :
                                       TR("LOCK: LE HASARD SE REPETE", "LOCK: THE RANDOM REPEATS"), C_DIMT);
            g_text(&FONT_BS, 6, 174, TR("LFO ENCORE: PAGE SUIVANTE", "LFO AGAIN: NEXT PAGE"), C_DIMT);
            if (s == 0u && sq_play) { g_dot(226, 166, 6, sq_bar_step & 1u ? C_ACC : C_ACC2); }
        }
    } else {
        pat_t *pt = cur_pat();
        uint32_t len = pat_len(pt) < 32u ? pat_len(pt) : 32u, b = cur_brk();
        if (g_vis(30, 40)) {
            g_text(&FONT_BM, 6, 32, GEN_STYLE[p->g_style % GS_N], C_ACC);
            t[0] = '#';
            fmt_u(t + 1, p->g_roll, 0);
            g_textr(&FONT_BL, 236, 30, t + 1, C_TXT);
        }
        for (i = 0; i < len; i++) {             /* the pattern it made, as cells */
            int32_t x = 4 + (int32_t)(i % 16u) * 14, y = 84 + (int32_t)(i / 16u) * 34;
            const step_t *st = &pt->st[i];
            if (!g_vis(y, 30)) continue;
            if (st->sl) {
                g_round(x, y, 13, 30, g_mix(C_PANEL, CLS_C[sl_class(b, st->sl - 1u)], sq_play && i == sq_pos ? 255 : 120));
                if (ST_RAT(*st) > 1u) g_rect(x + 3, y + 25, 7, 2, C_BG);
            } else {
                g_round(x, y, 13, 30, C_PANEL);
            }
        }
        if (g_vis(156, 40)) {
            g_text(&FONT_BS, 6, 160, TR("CHAQUE CRAN: UN NOUVEAU", "EVERY CLICK: A NEW ONE"), C_DIMT);
            g_text(&FONT_BS, 6, 174, TR("OCT-: ANNULER", "OCT-: UNDO"), C_DIMT);
            g_text(&FONT_BS, 140, 174, "K S H", C_SUB);
        }
    }
}

static void draw_flex(void)
{
    uint32_t i, on = flex_on, sel = flex_sel;
    for (i = 0; i < NFLEX; i++) {
        int32_t x = 3 + (int32_t)(i % 4u) * 59, y = 29 + (int32_t)(i / 4u) * 30;
        uint32_t a = i == on, q = i == sel && i != on;
        const char *nm = FLEX[i].name;
        if (!g_vis(y, 28)) continue;
        g_round(x, y, 56, 27, a ? C_ACC : q && (fm1_ms / 150u & 1u) ? C_ACC2 : C_PANEL);
        {
            char t[4];
            fmt_u(t, i + 1u, 0);
            g_text(&FONT_BS, x + 4, y + 1, t, a ? C_BG : C_DIMT);
        }
        g_textw(&FONT_BS, x + 4, y + 13, 50, nm, a ? C_BG : C_TXT);
    }
    if (g_vis(150, 46)) {                       /* the gate of the one playing (or chosen): 32 bars */
        uint32_t f = on < NFLEX ? on : sel < NFLEX ? sel : 0;
        g_round(2, 150, 236, 44, C_PANEL);
        for (i = 0; i < 32u; i++) {
            uint32_t lv = (i & 1u) ? FLEX[f].gate[i / 2u] & 15u : FLEX[f].gate[i / 2u] >> 4;
            int32_t h = (int32_t)lv * 30 / 15, x = 6 + (int32_t)i * 7;
            uint32_t cur = on < NFLEX && ((sq_bar_step + 15u) % 16u) == i / 2u;
            g_rect(x, 186 - h, 5, h, cur ? C_TXT : g_mix(C_PANEL, C_ACC, on < NFLEX ? 200 : 90));
        }
        if (on >= NFLEX && sel >= NFLEX) g_textc(&FONT_BS, 120, 160, TR("BLANCHES: 16 FLEX", "WHITE KEYS: 16 FLEX"), C_SUB);
    }
}

/* BEAT: the 16 programs as the white keys, 4 rows of 4 (left to right, then down); lit while held, the one KNOB 1
 * sets outlined, each one's INTENSITY a bar under it */
static void draw_beat(void)
{
    uint32_t i, on = beat_keys;
    if (g_vis(29, 152)) {
        g_rect(0, 29, 240, 152, C_BG);
        for (i = 0; i < 16u; i++) {
            int32_t x = 2 + (int32_t)(i % 4u) * 59, y = 31 + (int32_t)(i / 4u) * 37;
            uint32_t lit = on >> i & 1u, it = prj.p.bt_int[i] > 100u ? 100u : prj.p.bt_int[i];
            if (i == beat_sel && !lit) g_round(x - 1, y - 1, 59, 37, C_ACC2);
            g_round(x, y, 57, 35, lit ? C_ACC : C_PANEL);
            g_textw(&FONT_BS, x + 4, y + 2, 50, BEAT_NAME[i][0], lit ? C_BG : C_TXT);
            g_textw(&FONT_BS, x + 4, y + 15, 50, BEAT_NAME[i][1], lit ? C_BG : C_SUB);
            g_rect(x + 4, y + 30, 49 * (int32_t)it / 100, 2, lit ? C_BG : C_ACC2);
        }
    }
    if (g_vis(182, 14)) {
        g_rect(0, 182, 240, 14, C_BG);
        g_textc(&FONT_BS, 120, 182, TR("BEAT: TIENS UNE BLANCHE", "BEAT: HOLD A WHITE KEY"), C_DIMT);
    }
}

static void draw_fx(void)
{
    params_t *p = &prj.p;
    uint32_t s = sub[P_FX], i;
    if (s == 2u) { draw_beat(); return; }
    if (g_vis(29, 110)) {
        g_round(2, 29, 236, 110, C_PANEL);
        if (s == 0u) {                          /* the filter: its response, the cut-off and the resonance's bump */
            int32_t f = p->filter, x, c = -1, lp = f < 60, hp = f > 68, top = 54, bot = 134;
            if (lp) c = 8 + f * 220 / 60;
            if (hp) c = 8 + (f - 69) * 220 / 58;
            for (x = 6; x < 234; x++) {
                int32_t y = top, d;
                if (c >= 0) {
                    d = lp ? x - c : c - x;     /* octaves past the cut-off, roughly */
                    if (d > 0) y = top + d * 3;
                    else if (d > -14) y = top - (int32_t)p->reso * (14 + d) / 5;
                }
                if (y > bot) y = bot;
                g_rect(x, y, 1, bot - y, g_mix(C_PANEL, C_ACC, 55));
                g_rect(x, y, 1, 2, C_ACC);
            }
            if (c >= 0) g_rect(c, 40, 1, 96, C_SUB);
            g_text(&FONT_BS, 8, 33, c < 0 ? "OFF" : lp ? "LOW-PASS" : "HIGH-PASS", C_SUB);
        } else {                                /* the delay's echoes */
            int32_t lv = 90, x = 14, step = 12 + (int32_t)p->dly_time * 7;
            g_rect(x, 134 - lv, 6, lv, C_TXT);
            for (i = 0; i < 12u && lv > 2 && p->dly_mix; i++) {
                x += step;
                if (x > 228) break;
                lv = (i == 0u ? lv * (int32_t)p->dly_mix / 15 : lv * (int32_t)p->dly_fb * 54 / 1024);
                if (lv > 0) g_rect(x, 134 - lv, 6, lv, g_mix(C_PANEL, C_ACC, 220));
            }
        }
    }
    if (g_vis(142, 54)) {
        int32_t lv = live_level * 228 / 32767;
        g_text(&FONT_BS, 6, 144, s == 0u ? TR("DRIVE, CRUSH: LA SALETE", "DRIVE, CRUSH: THE DIRT") : TR("PUNCH: COMPRESSEUR", "PUNCH: THE COMPRESSOR"), C_DIMT);
        g_round(6, 162, 228, 10, C_PANEL);
        g_rect(6, 164, lv, 6, lv > 210 ? C_REC : C_PLAY);
        g_text(&FONT_BS, 6, 178, TR("FX ENCORE: PAGE SUIVANTE", "FX AGAIN: NEXT PAGE"), C_DIMT);
    }
}

static void draw_global(void)
{
    char t[16];
    if (g_vis(29, 110)) {
        g_round(2, 29, 236, 110, C_PANEL);
        if (sub[P_GLOBAL] == 0u) {
            fmt_bpm(t, bpm10());
            g_textc(&FONT_BL, 120, 52, t, C_TXT);
            g_textc(&FONT_BS, 120, 92, prj.p.clock ? "MIDI CLOCK" : "BPM", C_SUB);
            if (sq_play) { uint32_t i, beat = (sq_bar_step / 4u) % 4u; for (i = 0; i < 4u; i++) g_dot(84 + (int32_t)i * 24, 118, 5, i == beat ? C_ACC : C_PANEL2); }
        } else {
            {
                int32_t x0 = 120 - (int32_t)LOGO_W / 2, y0 = 50, gy, gx;
                uint32_t bpr = (LOGO_W + 1u) / 2u;
                for (gy = 0; gy < (int32_t)LOGO_H; gy++) {
                    if (!g_vis(y0 + gy, 1)) continue;
                    for (gx = 0; gx < (int32_t)LOGO_W; gx++) {
                        uint32_t v = LOGO[(uint32_t)gy * bpr + (uint32_t)gx / 2u];
                        v = (gx & 1) ? (v & 15u) : (v >> 4);
                        if (v) g_pxb(x0 + gx, y0 + gy, C_ACC, v * 17u);
                    }
                }
            }
            g_textc(&FONT_BS, 120, 96, "BREAKBEAT SLICER  " APP_VER, C_SUB);
            g_textc(&FONT_BS, 120, 112, "GPL-3.0  BY OMNIPOTARD", C_DIMT);
        }
    }
    if (g_vis(142, 54)) {
        g_text(&FONT_BS, 6, 146, TR("ALGO: LE TEMPO PARTOUT", "ALGO: THE TEMPO, ANYWHERE"), C_DIMT);
        g_text(&FONT_BS, 6, 160, TR("OCT+: TAPER LE TEMPO", "OCT+: TAP THE TEMPO"), C_DIMT);
        g_text(&FONT_BS, 6, 174, TR("GLO ENCORE: SYSTEME", "GLO AGAIN: SYSTEM"), C_DIMT);
    }
}

static void draw_save(void)
{
    uint32_t i;
    static const char *const SN[4] = {"AUTO", "A", "B", "C"};
    for (i = 0; i < NSLOT; i++) {
        int32_t x = 3 + (int32_t)i * 59;
        if (!g_vis(29, 34)) break;
        g_round(x, 29, 56, 32, i == save_slot ? C_ACC : C_PANEL);
        g_textc(&FONT_BM, x + 28, 32, SN[i], i == save_slot ? C_BG : C_TXT);
        g_textc(&FONT_BS, x + 28, 48, slot_used[i] ? TR("PLEIN", "SAVED") : TR("VIDE", "EMPTY"), i == save_slot ? C_BG : C_DIMT);
    }
    for (i = 0; i < NPAT; i++) {
        int32_t x = 3 + (int32_t)(i % 8u) * 29, y = 68 + (int32_t)(i / 8u) * 26;
        const pat_t *pt = &prj.pat[i];
        uint32_t used = 0, k, inch = 0;
        char t[4];
        if (!g_vis(y, 24)) continue;
        for (k = 0; k < pat_len(pt) && !used; k++) used = pt->st[k].sl != 0;
        for (k = 0; k < prj.nchain; k++) inch |= prj.chain[k] == i;
        g_round(x, y, 27, 23, i == prj.cur_pat ? C_ACC : used ? C_PANEL2 : C_PANEL);
        fmt_u(t, i + 1u, 0);
        g_textc(&FONT_BS, x + 13, y + 5, t, i == prj.cur_pat ? C_BG : used ? C_TXT : C_DIMT);
        if (inch) g_rect(x + 4, y + 19, 19, 2, i == prj.cur_pat ? C_BG : C_ACC2);
    }
    if (g_vis(124, 72)) {
        char t[40];
        t[0] = 0;
        if (prj.nchain > 1u) {
            str_cat(t, TR("CHAINE ", "CHAIN "));
            for (i = 0; i < prj.nchain && i < 10u; i++) { fmt_u(t + str_len(t), prj.chain[i] + 1u, 0); if (i + 1u < prj.nchain) str_cat(t, "-"); }
        } else {
            str_cat(t, TR("TENIR + AUTRES: CHAINE", "HOLD + OTHERS: CHAIN"));
        }
        g_text(&FONT_BS, 6, 128, t, prj.nchain > 1u ? C_ACC2 : C_DIMT);
        g_text(&FONT_BS, 6, 146, TR("SAVE ENCORE: FAIRE L'ACTION", "SAVE AGAIN: DO THE ACTION"), C_SUB);
        g_text(&FONT_BS, 6, 160, TR("OCT-: ANNULER  OCT+: CHAINE OFF", "OCT-: UNDO  OCT+: NO CHAIN"), C_DIMT);
        if (pat_dirty) g_text(&FONT_BS, 6, 176, TR("* NON SAUVE", "* NOT SAVED"), C_ACC);
    }
}

/* HOME held: what this page does */
static const char *const HELP_EN[P_N][6] = {
    {"WHITE KEYS  PLAY SLICES 1-16", "OCT- / OCT+  SLICES 17-32", "BLACK KEYS  PUNCH EFFECTS", "SELECT  BREAK   ALGO  TEMPO", "PRESETS  PATTERN   REC  RECORD", "HOME + PLAY  STOP EVERYTHING"},
    {"WHITE KEYS  STEPS: PUT / CLEAR", "HOLD A STEP + KNOBS  EDIT IT", "HOLD A STEP + BLACK  FX LOCK", "OCT- / OCT+  STEPS 17-64", "SELECT  THE SLICE TO PUT", "PLAY + REC  RECORD LIVE"},
    {"SELECT  CHOOSE THE BREAK", "MODE  AS IS / GRID / AUTO HITS", "COUNT  GRID SLICES  SENS  AUTO", "BARS  GIVE THE BREAK ITS TEMPO", "TEMPO  CHOP KEEPS THE PITCH", "        VARI FOLLOWS THE TEMPO"},
    {"SELECT OR A KEY  A SLICE", "START / FINE  MOVE ITS START", "PITCH / GAIN  OF THIS SLICE", "OCT+  SPLIT IT IN TWO", "OCT-  JOIN IT TO THE ONE BEFORE", "EDIT AGAIN  REVERSE, CLASS"},
    {"ATTACK  FADE IN OF EVERY SLICE", "DECAY  HOLD, OR SHORTER HITS", "LENGTH  HOW MUCH OF THE SLICE", "CHOKE  MONO CUTS, POLY RINGS", "", ""},
    {"CHANCE  ANOTHER SLICE (SAME KIND)", "REV ROLL SKIP  HOW OFTEN", "LFO AGAIN  JUMPS, SMART, LOCK", "LOCK  THE RANDOM REPEATS", "LFO AGAIN  GENERATE A PATTERN", "OCT-  UNDO"},
    {"WHITE KEYS  16 FLEX PATTERNS", "THE SAME KEY AGAIN  OFF", "MODE  LATCH / HOLD / ONE BAR", "QUANT  STARTS ON STEP BEAT BAR", "DEPTH  HOW DEEP THE GATE", "BLACK KEYS  PUNCH EFFECTS"},
    {"FILTER  LEFT LOW, RIGHT HIGH", "RESO DRIVE CRUSH  THE DIRT", "FX AGAIN  DELAY AND PUNCH", "FX 3RD TIME  BEAT: 16 ON WHITE", "BEAT  KNOB 1: INTENSITY", "BLACK KEYS  PUNCH EFFECTS"},
    {"BPM SWING  (ALGO: TEMPO TOO)", "TEMPO  CHOP OR VARI", "METRONOME  OFF / REC / ON", "OCT+  TAP THE TEMPO", "GLO AGAIN  CLOCK LANGUAGE THEME", ""},
    {"SELECT  SLOT   KNOB 1  ACTION", "SAVE AGAIN  DO IT", "WHITE KEYS  PATTERN 1-16", "HOLD ONE + OTHERS  A CHAIN", "OCT-  UNDO   OCT+  NO CHAIN", "AUTO SAVES ITSELF WHEN STOPPED"},
};
static const char *const HELP_FR[P_N][6] = {
    {"BLANCHES  TRANCHES 1-16", "OCT- / OCT+  TRANCHES 17-32", "NOIRES  EFFETS PUNCH", "SELECT  BREAK   ALGO  TEMPO", "PRESETS  PATTERN   REC  ENREG.", "HOME + PLAY  TOUT ARRETER"},
    {"BLANCHES  PAS: POSER / OTER", "TENIR UN PAS + KNOBS  L'EDITER", "TENIR UN PAS + NOIRE  EFFET", "OCT- / OCT+  PAS 17-64", "SELECT  LA TRANCHE A POSER", "PLAY + REC  ENREGISTRER"},
    {"SELECT  CHOISIR LE BREAK", "MODE  TEL QUEL / GRILLE / AUTO", "NOMBRE  GRILLE  SENS  AUTO", "MESURES  DONNENT SON TEMPO", "TEMPO  CHOP GARDE LA HAUTEUR", "        VARI SUIT LE TEMPO"},
    {"SELECT OU UNE TOUCHE  TRANCHE", "START / FIN  DEPLACER LE DEBUT", "PITCH / GAIN  DE CETTE TRANCHE", "OCT+  LA COUPER EN DEUX", "OCT-  LA JOINDRE A LA PRECEDENTE", "EDIT ENCORE  ENVERS, GENRE"},
    {"ATTACK  ENTREE DE CHAQUE TRANCHE", "DECAY  TENUE, OU PLUS COURT", "LONGUEUR  QUELLE PART JOUER", "CHOKE  MONO COUPE, POLY RESONNE", "", ""},
    {"CHANCE  UNE AUTRE TRANCHE", "REV ROLL SKIP  A QUELLE FREQUENCE", "LFO ENCORE  SAUTS, SMART, LOCK", "LOCK  LE HASARD SE REPETE", "LFO ENCORE  GENERER UN PATTERN", "OCT-  ANNULER"},
    {"BLANCHES  16 FLEX PATTERNS", "LA MEME TOUCHE  ARRET", "MODE  LATCH / HOLD / 1 MESURE", "QUANT  DEPART: PAS TEMPS MESURE", "PROFONDEUR  DU GATE", "NOIRES  EFFETS PUNCH"},
    {"FILTER  GAUCHE BAS, DROITE HAUT", "RESO DRIVE CRUSH  LA SALETE", "FX ENCORE  DELAY ET PUNCH", "FX 3E FOIS  BEAT: 16 BLANCHES", "BEAT  KNOB 1: INTENSITE", "NOIRES  EFFETS PUNCH"},
    {"BPM SWING  (ALGO: TEMPO AUSSI)", "TEMPO  CHOP OU VARI", "METRONOME  OFF / REC / ON", "OCT+  TAPER LE TEMPO", "GLO ENCORE  HORLOGE LANGUE THEME", ""},
    {"SELECT  SLOT   KNOB 1  ACTION", "SAVE ENCORE  LE FAIRE", "BLANCHES  PATTERN 1-16", "TENIR + AUTRES  UNE CHAINE", "OCT-  ANNULER  OCT+  SANS CHAINE", "SAUVE TOUT SEUL A L'ARRET"},
};

static void draw_help(void)
{
    uint32_t i;
    g_rect(0, g_y0, 240, BAND_H, C_BG);
    if (g_vis(16, 30)) {
        g_round(12, 18, 216, 26, C_ACC);
        g_textc(&FONT_BM, 120, 23, PAGE_NAME[page], C_BG);
    }
    for (i = 0; i < 6u; i++) {
        const char *s = lang ? HELP_EN[page][i] : HELP_FR[page][i];
        if (s[0]) g_text(&FONT_BS, 14, 58 + (int32_t)i * 22, s, i % 2u ? C_SUB : C_TXT);
    }
    g_textc(&FONT_BS, 120, 196, TR("BREAKR  " APP_VER "  AIDE", "BREAKR  " APP_VER "  HELP"), C_DIMT);
}

static void draw_toast(void)
{
    int32_t w;
    if (!toast_until || (int32_t)(fm1_ms - toast_until) > 0 || !g_vis(94, 34)) return;
    w = g_tw(&FONT_BM, toast_s) + 30;
    if (w > 232) w = 232;
    g_round(120 - w / 2, 96, w, 30, C_ACC);
    g_round(120 - w / 2 + 2, 98, w - 4, 26, C_PANEL);
    g_textc(&FONT_BM, 120, 103, toast_s, C_TXT);
}

static void draw_splash(void)
{
    int32_t x0 = 120 - (int32_t)LOGO_W / 2, y0 = 82, gy, gx;
    uint32_t bpr = (LOGO_W + 1u) / 2u, t = fm1_ms - boot_ms;
    g_rect(0, g_y0, 240, BAND_H, C_BG);
    for (gy = 0; gy < (int32_t)LOGO_H; gy++) {
        if (!g_vis(y0 + gy, 1)) continue;
        for (gx = 0; gx < (int32_t)LOGO_W; gx++) {
            uint32_t v = LOGO[(uint32_t)gy * bpr + (uint32_t)gx / 2u];
            v = (gx & 1) ? (v & 15u) : (v >> 4);
            if (v) g_pxb(x0 + gx, y0 + gy, gx * 1000 / (int32_t)LOGO_W < (int32_t)t ? C_ACC : C_PANEL2, v * 17u);
        }
    }
    if (g_vis(126, 40)) {
        int32_t i;
        for (i = 0; i < 16; i++) g_round(8 + i * 14, 132, 12, 12, (int32_t)t / 60 > i ? slice_col((uint32_t)i) : C_PANEL);
        g_textc(&FONT_BS, 120, 152, "BREAKBEAT SLICER FOR THE FM-1", C_SUB);
    }
}

static void draw_screen(void)
{
    if (splash) { draw_splash(); return; }
    g_rect(0, g_y0, 240, BAND_H, C_BG);
    draw_header();
    switch (page) {
    case P_PLAY: draw_play(); break;
    case P_SEQ: draw_seq(); break;
    case P_SLICE: draw_slice(); break;
    case P_CHOP: draw_chop(); break;
    case P_AMP: draw_amp(); break;
    case P_DICE: draw_dice(); break;
    case P_FLEX: draw_flex(); break;
    case P_FX: draw_fx(); break;
    case P_GLOBAL: draw_global(); break;
    case P_SAVE: draw_save(); break;
    default: break;
    }
    draw_knobs();
    if (help_on) draw_help();
    draw_toast();
}

/* ---- the LEDs: 0 off, 1 the glow, 2 lit */
static uint8_t led_want[41];
static void led(uint32_t id, uint32_t lvl) { if (id < 41u) led_want[id] = (uint8_t)lvl; }
#if defined(DINGHY_HOST) || defined(FM1_WASM)
static void leds_flush(void) { host_leds(led_want); }
#else
static void leds_flush(void)
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++) {
        uint8_t on = 0, dim = 0;
        for (r = 1; r < 5u; r++) {
            int8_t id = FM1_KEYMAP[r][p];
            if (id < 0 || id > 40) continue;
            if (led_want[id] == 2u) on |= (uint8_t)(1u << r);
            else if (led_want[id] == 1u) dim |= (uint8_t)(1u << r);
        }
        fm1_led[p] = on;
        fm1_led_dim[p] = dim;
    }
}
#endif

static void leds_update(void)
{
    uint32_t i, b = cur_brk(), n = sl_count(b), snd = sl_sounding, act = fx_active, blink = fm1_ms / 125u & 1u;
    pat_t *pt = cur_pat();
    for (i = 0; i < 41u; i++) led_want[i] = 0;
    for (i = 0; i < P_N; i++) led(PAGE_BTN[i], i == page ? 2u : 1u);
    led(BTN_PLAY, sq_play ? ((sq_bar_step & 3u) == 1u ? 1u : 2u) : 1u);
    led(BTN_REC, sq_rec ? (blink ? 2u : 1u) : 1u);
    led(BTN_OCT_DOWN, (page == P_PLAY || page == P_SLICE) ? (bank16 ? 1u : 0u) : 1u);
    led(BTN_OCT_UP, (page == P_PLAY || page == P_SLICE) ? (bank16 ? 2u : n > 16u ? 1u : 0u) : 1u);
    for (i = 0; i < NPUNCH; i++) led(KEY_LED(BLACK_KEY[i]), act >> i & 1u ? 2u : 1u);
    for (i = 0; i < 16u; i++) {
        uint32_t id = KEY_LED(WHITE_KEY[i]), lv = 0;
        switch (page) {
        case P_SEQ: {
            uint32_t s = seq_page * 16u + i, has = s < pat_len(pt) && pt->st[s].sl;
            lv = has ? 2u : s < pat_len(pt) ? 1u : 0u;
            if (sq_play && s == sq_pos) lv = has ? 1u : 2u;
            if (held_step >= 0 && (uint32_t)held_step == s) lv = blink ? 2u : 0u;
            break;
        }
        case P_FLEX:
            lv = i == flex_on ? 2u : i == flex_sel ? (blink ? 2u : 1u) : 1u;
            break;
        case P_FX:
            lv = sub[P_FX] == 2u ? (beat_keys >> i & 1u ? 2u : i == beat_sel ? (blink ? 2u : 1u) : 1u) : 0u;
            break;
        case P_SAVE: {
            uint32_t k, inch = 0;
            for (k = 0; k < prj.nchain; k++) inch |= prj.chain[k] == i;
            lv = i == prj.cur_pat ? 2u : inch ? (blink ? 2u : 1u) : 1u;
            break;
        }
        default: {
            uint32_t k = bank16 * 16u + i;
            lv = k < n ? ((k < 32u && (snd >> k & 1u)) ? 2u : 1u) : 0u;
            if (page == P_CHOP && k == chop_k) lv = blink ? 2u : 1u;
            break;
        }
        }
        led(id, lv);
    }
    leds_flush();
}

/* ---- the panel */
static void page_set(uint32_t p)
{
    if (p == page) sub[p] = (uint8_t)((sub[p] + 1u) % PAGE_SUBS[p]);
    page = (uint8_t)p;
    if (page != P_FX || sub[P_FX] != 2u) { uint32_t e; for (e = 0; e < 16u; e++) if (beat_keys >> e & 1u) bt_key(e, 0); }   /* (let go with the page) */
    held_step = -1;
    gen_kept = 0;
    save_arm = 0;
}

static void transport(uint32_t play)
{
    if (play && !sq_play) { ev_push(EV_PLAY, 0, 0, 0); midi_send(0xFA, 0, 0); }
    if (!play && sq_play) { ev_push(EV_STOP, 0, 0, 0); midi_send(0xFC, 0, 0); }
}

static void key_down(uint32_t key)
{
    int32_t w = key_white[key], bk = key_black[key];
    pat_t *pt = cur_pat();
    uint32_t b = cur_brk(), n = sl_count(b);
    if (bk >= 0) {                              /* a black key: a punch effect (or the held step's lock) */
        if (page == P_SEQ && held_step >= 0) {
            step_t *st = &pt->st[(uint32_t)held_step % NSTEP];
            st->fx = st->fx == (uint8_t)(bk + 1) ? 0 : (uint8_t)(bk + 1);
            held_used = 1;
            pt->autog = 0;
            touch();
            return;
        }
        punch_keys |= 1u << bk;
        return;
    }
    if (w < 0) return;
    if (page == P_FX && sub[P_FX] == 2u) {      /* BEAT: held, it plays (the last pressed) */
        beat_sel = (uint8_t)w;
        bt_key((uint32_t)w, 1);
        if (!sq_play) toast(BEAT_LONG[w]);
        return;
    }
    switch (page) {
    case P_SEQ: {
        uint32_t s = seq_page * 16u + (uint32_t)w;
        if (s >= pat_len(pt)) return;
        held_step = (int8_t)s;
        held_used = 0;
        if (!pt->st[s].sl) {                    /* an empty step: the brush, at once */
            pt->st[s].sl = (uint8_t)(brush <= n ? brush : 1u);
            pt->st[s].fl = 0;
            pt->st[s].pitch = 0;
            pt->st[s].fx = 0;
            pt->st[s].nudge = 0;
            held_used = 1;
            pt->autog = 0;
            touch();
            if (!sq_play) ev_push(EV_SLICE, pt->st[s].sl - 1u, key, 0);
        }
        return;
    }
    case P_FLEX: {
        uint32_t f = (uint32_t)w;
        if (prj.p.fl_mode == 0u && (flex_sel == f || (flex_on == f && flex_sel >= NFLEX))) flex_sel = 0xFF;   /* LATCH: again = off */
        else flex_sel = f;
        if (!sq_play) toast(FLEX[f].name);
        return;
    }
    case P_SAVE: {
        uint32_t k = (uint32_t)w;
        if (chain_first < NPAT && chain_first != k) {   /* another pattern while one is held: a chain */
            if (prj.nchain < 16u) prj.chain[prj.nchain++] = (uint8_t)k;
            touch();
            return;
        }
        chain_first = k;
        prj.nchain = 1;
        prj.chain[0] = (uint8_t)k;
        if (sq_play) pat_next = k;
        else prj.cur_pat = (uint8_t)k;
        return;
    }
    default: {
        uint32_t k = bank16 * 16u + (uint32_t)w;
        if (k >= n) return;
        if (page == P_CHOP) chop_k = (uint8_t)k;
        brush = (uint8_t)(k + 1u);
        key_live[key] = (uint8_t)(k + 1u);
        ev_push(EV_SLICE, k, key, 0);
        return;
    }
    }
}

static void key_up(uint32_t key)
{
    int32_t w = key_white[key], bk = key_black[key];
    pat_t *pt = cur_pat();
    if (bk >= 0) { punch_keys &= ~(1u << bk); return; }
    if (w < 0) return;
    if (beat_keys >> w & 1u) bt_key((uint32_t)w, 0);
    if (page == P_SEQ && held_step >= 0 && (uint32_t)held_step == seq_page * 16u + (uint32_t)w) {
        if (!held_used) {                       /* a tap on a full step: cleared */
            pt->st[(uint32_t)held_step % NSTEP].sl = 0;
            pt->autog = 0;
            touch();
        }
        held_step = -1;
    }
    if (page == P_FLEX && prj.p.fl_mode == 1u && flex_sel == (uint32_t)w) flex_sel = 0xFF;   /* HOLD */
    if (page == P_SAVE && chain_first == (uint32_t)w) {
        chain_first = 0xFF;
        if (prj.nchain <= 1u) prj.nchain = 0;
    }
    if (key_live[key]) { ev_push(EV_SLICE_OFF, key, 0, 0); key_live[key] = 0; }
}

static void input(void)
{
    uint32_t rel, pr = fm1_input_edges(&rel), notes = fm1_in.notes & 0x7FFFFFFu, ne = fm1_input_note_edges();
    uint32_t i, down = notes & ~notes_prev, up = notes_prev & ~notes;
    int32_t d;
    if (splash) {                               /* any key: on */
        if (pr || ne || fm1_ms - boot_ms > 1600u) { splash = 0; g_force = 1; }
        notes_prev = notes;
        return;
    }
    down |= ne & ~notes_prev;                   /* a tap between two passes: down and up */
    for (i = 0; i < 27u; i++) {
        if (down >> i & 1u) key_down(i);
        if ((up >> i & 1u) || ((ne >> i & 1u) && !(notes >> i & 1u))) key_up(i);
    }
    notes_prev = notes;
    /* buttons */
    if (pr >> BTN_HOME & 1u) home_ms = fm1_ms;
    if ((fm1_in.buttons >> BTN_HOME & 1u) && fm1_ms - home_ms > 380u) help_on = 1;
    if (rel >> BTN_HOME & 1u) {
        if (!help_on) page_set(P_PLAY);
        help_on = 0;
    }
    if ((pr >> BTN_PLAY & 1u) && (fm1_in.buttons >> BTN_HOME & 1u)) {   /* HOME + PLAY: panic */
        ev_push(EV_PANIC, 0, 0, 0);
        midi_send(0xFC, 0, 0);
        punch_keys = 0;
        flex_sel = 0xFF;
        help_on = 0;
        home_ms = fm1_ms + 100000u;
        toast(TR("TOUT ARRETE", "ALL STOPPED"));
        pr &= ~(1u << BTN_PLAY);
    }
    for (i = 1; i < P_N; i++) if (pr >> PAGE_BTN[i] & 1u) {
        if (i == P_SAVE && page == P_SAVE) save_do();
        else page_set(i);
    }
    if (pr >> BTN_PLAY & 1u) {
        if (sq_play) { transport(0); sq_rec = 0; }
        else transport(1);
    }
    if (pr >> BTN_REC & 1u) {
        if (!sq_play) { sq_rec = 1; transport(1); }
        else sq_rec = !sq_rec;
        if (sq_rec) toast(TR("ENREGISTRE", "RECORDING"));
    }
    if (pr >> BTN_OCT_DOWN & 1u) {
        switch (page) {
        case P_SEQ: if (seq_page) seq_page--; break;
        case P_CHOP: if (slice_join(cur_brk(), chop_k)) { if (chop_k) chop_k--; toast(TR("JOINT", "JOINED")); } break;
        case P_DICE: case P_SAVE: toast(undo_do() ? TR("ANNULE", "UNDONE") : TR("RIEN A ANNULER", "NOTHING TO UNDO")); break;
        default: bank16 = 0; break;
        }
    }
    if (pr >> BTN_OCT_UP & 1u) {
        switch (page) {
        case P_SEQ: if ((seq_page + 1u) * 16u < pat_len(cur_pat())) seq_page++; break;
        case P_CHOP: if (slice_split(cur_brk(), chop_k)) toast(TR("COUPE", "SPLIT")); break;
        case P_SAVE: prj.nchain = 0; toast(TR("SANS CHAINE", "NO CHAIN")); break;
        case P_GLOBAL: {                        /* tap tempo */
            if (tap_n && fm1_ms - tap_ms[(tap_n - 1u) & 3u] > 2000u) tap_n = 0;
            tap_ms[tap_n & 3u] = fm1_ms;
            tap_n++;
            if (tap_n >= 2u) {
                uint32_t k = tap_n > 4u ? 4u : tap_n, span = tap_ms[(tap_n - 1u) & 3u] - tap_ms[(tap_n - k) & 3u];
                if (span) { bpm10_set(600000u * (k - 1u) / span); touch(); }
            }
            break;
        }
        default: if (sl_count(cur_brk()) > 16u) bank16 = 1; break;
        }
    }
    /* encoders */
    if ((d = fm1_enc_take(ENC_SELECT)) != 0) {
        switch (page) {
        case P_SEQ: brush = (uint8_t)clampi((int32_t)brush + d, 1, (int32_t)(sl_count(cur_brk()) ? sl_count(cur_brk()) : 1u)); break;
        case P_CHOP: chop_k = (uint8_t)clampi((int32_t)chop_k + d, 0, (int32_t)sl_count(cur_brk()) - 1); break;
        case P_SAVE: save_slot = (uint8_t)clampi((int32_t)save_slot + d, 0, NSLOT - 1); save_arm = 0; break;
        case P_FLEX: flex_sel = (uint32_t)clampi(flex_sel < NFLEX ? (int32_t)flex_sel + d : 0, 0, NFLEX - 1); break;
        default: brk_choose(d > 0 ? 1 : -1); break;
        }
    }
    if ((d = fm1_enc_take(ENC_ALGORITHM)) != 0) { bpm10_set((uint32_t)clampi((int32_t)bpm10() + accel(d) * 10, 400, 3000)); touch(); }
    if ((d = fm1_enc_take(ENC_PRESETS)) != 0) {
        int32_t base = pat_next < NPAT ? (int32_t)pat_next : (int32_t)prj.cur_pat, np = clampi(base + d, 0, NPAT - 1);
        char t[16];
        if (sq_play) pat_next = (uint32_t)np;
        else prj.cur_pat = (uint8_t)np;
        prj.nchain = 0;
        t[0] = 0;
        str_cat(t, "PATTERN ");
        fmt_u(t + str_len(t), (uint32_t)np + 1u, 0);
        toast(t);
    }
    for (i = 0; i < 4u; i++)
        if ((d = fm1_enc_take(ENC_KNOB1 + i)) != 0) knob_turn(i, d);
}

/* ---- MIDI in: notes 36.. play the slices, CC, the clock; out: the clock */
static uint32_t clk_sent;
static void app_poll(void)
{
    uint8_t st, d1, d2;
    while (midi_get(&st, &d1, &d2)) {
        uint32_t s = st & 0xF0u;
        if (st == 0xF8u) { if (prj.p.clock) ext_clocks++; }
        else if (st == 0xFAu) { if (prj.p.clock) ev_push(EV_PLAY, 0, 0, 0); }
        else if (st == 0xFBu) { if (prj.p.clock) ev_push(EV_CONT, 0, 0, 0); }
        else if (st == 0xFCu) { if (prj.p.clock) ev_push(EV_STOP, 0, 0, 0); }
        else if (s == 0x90u && d2 && d1 >= 36u && d1 < 68u) ev_push(EV_SLICE, d1 - 36u, 0xF0u, 0);
        else if ((s == 0x80u || (s == 0x90u && !d2)) && d1 >= 36u && d1 < 68u) ev_push(EV_SLICE_OFF, 0xF0u, 0, 0);
        else if (s == 0xB0u) {
            if (d1 == 1u) prj.p.d_chance = (uint8_t)(d2 * 100u / 127u);
            if (d1 == 74u) prj.p.filter = d2;
            if (d1 == 71u) prj.p.reso = (uint8_t)(d2 >> 3);
            if (d1 == 75u) prj.p.crush = (uint8_t)(d2 >> 3);
        }
    }
    if (!prj.p.clock && sq_play) {              /* our clock out: a clock every 4 ticks */
        uint32_t t = sq.tick / 4u;
        if (t + 0u < clk_sent || t > clk_sent + 48u) clk_sent = t;
        while (clk_sent < t) { midi_send(0xF8, 0, 0); clk_sent++; }
    } else {
        clk_sent = 0;
    }
    bk_poll();
}

static void set_lang(uint32_t l) { lang = l ? 1u : 0u; }   /* (the host's and the emulator's: 0 FR, 1 EN) */

/* ---- start, and a pass of the main loop */
static void app_init(void)
{
    uint32_t i;
    for (i = 0; i < 27u; i++) { key_white[i] = -1; key_black[i] = -1; }
    for (i = 0; i < 16u; i++) key_white[WHITE_KEY[i]] = (int8_t)i;
    for (i = 0; i < NPUNCH; i++) key_black[BLACK_KEY[i]] = (int8_t)i;
    st_boot();
    fact_boot();
    bank_boot();
    an.b = 0xFF;
    for (i = 0; i < NSLOT; i++) slot_used[i] = (uint8_t)slot_check(i);
    if (prj_load(0)) prj_defaults();
    if (prj.bank_crc != bank_crc) for (i = NFACT; i < NBRK; i++) prj.sc[i].nsl = 0;
    if (!settings_load(&sets, sizeof sets)) { lang = sets.lang & 1u; theme = (uint8_t)(sets.theme % NTHEME); }
    else { sets.lang = lang; sets.theme = theme; }
    engine_init();
    pat_dirty = 0;
    boot_ms = fm1_ms;
}

static void app_step(void)
{
    uint32_t b = cur_brk();
    static uint32_t led_ms;
    input();
    slice_poll();
    if (slice_want_b >= NBRK) {                 /* the current break looked at (the waveform) */
        if (an.b != b) an_start(b);
        an_poll(6);
    }
    if (page == P_CHOP && sl_count(b)) zoom_make(b, chop_k < sl_count(b) ? chop_k : 0);
    if (bk_toast) { toast(bk_toast == 1u ? TR("BREAKS RECUS !", "BREAKS IN !") : TR("BANQUE VIDE", "BANK EMPTY")); bk_toast = 0; an.b = 0xFF; }
    if (pat_dirty && !sq_play && !bk_on && fm1_ms - dirty_ms > 6000u) {   /* the autosave, when it is quiet */
        uint32_t j, busy = 0;
        for (j = 0; j < NVOICE; j++) busy |= vo[j].on;
        if (!busy) { prj.bank_crc = bank_crc; prj_save(0); }
    }
    if (fm1_ms - led_ms >= 20u) { led_ms = fm1_ms; leds_update(); }
    g_band_send(draw_screen);
    live_level = live_level * 7 / 8;
}

static void app_main(void)
{
    app_init();
    for (;;) {
        platform_poll();
        app_step();
    }
}

#if defined(FM1_WASM)
/* the emulator's page: what the knobs do now (their labels under the knobs), the page, the theme; the store
 * (projects and the bank) that the page keeps between visits; the SysEx of the editor, as over USB */
static char w_lab[4][24];
WASM_EXPORT("breakr_page") uint32_t breakr_page(void) { return page; }
WASM_EXPORT("breakr_theme") uint32_t breakr_theme(void) { return theme; }
WASM_EXPORT("breakr_knob_label") const char *breakr_knob_label(uint32_t i)
{
    kv_t o;
    uint32_t n;
    if (i >= 4u) return "";
    knob_info(i, &o);
    for (n = 0; n < 23u && o.name[n]; n++) w_lab[i][n] = o.name[n];
    w_lab[i][n] = 0;
    return w_lab[i];
}
WASM_EXPORT("breakr_store") uint8_t *breakr_store(void) { return host_store; }
WASM_EXPORT("breakr_store_size") uint32_t breakr_store_size(void) { return STORE_HI - STORE_LO; }
WASM_EXPORT("breakr_store_gen") uint32_t breakr_store_gen(void) { return store_gen; }
WASM_EXPORT("breakr_store_loaded") void breakr_store_loaded(void)   /* the page put the store back: read it again */
{
    uint32_t i;
    host_store_ok = 1;
    bank_boot();
    for (i = 0; i < NSLOT; i++) slot_used[i] = (uint8_t)slot_check(i);
    if (!prj_load(0) && prj.bank_crc != bank_crc) for (i = NFACT; i < NBRK; i++) prj.sc[i].nsl = 0;
    an.b = 0xFF;
    g_force = 1;
}
static uint8_t w_sx[320];
WASM_EXPORT("breakr_sysex_buf") uint8_t *breakr_sysex_buf(void) { return w_sx; }
WASM_EXPORT("breakr_sysex") uint32_t breakr_sysex(uint32_t n)   /* a frame F0..F7 in the buffer; the reply's length */
{
    w_reply_n = 0;
    if (n >= 2u && n <= sizeof w_sx && w_sx[0] == 0xF0u && w_sx[n - 1u] == 0xF7u) app_sysex(w_sx + 1, n - 2u);
    return w_reply_n;
}
WASM_EXPORT("breakr_reply") uint8_t *breakr_reply(void) { return w_reply; }
#endif
