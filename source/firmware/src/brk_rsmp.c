/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: RESAMPLE (SAVE > ACTION: RESAMPLE). What comes out of BREAKR (the effects, DESTROY and BEAT included) is
 * recorded for 1, 2 or 4 bars from the next bar, then becomes one of the user's breaks: no computer needed. The
 * audio interrupt takes the output after the master (rs_tap), mono, at 22050 Hz (two frames averaged), and
 * encodes it on the fly in 4-bit IMA ADPCM blocks as the bank keeps them, into a RAM buffer of 5.5 s; the main
 * loop then stops the sequencer and appends the break to the bank in flash (bank_append, brk_store.c). */

#define RS_MAX (240u * IMA_BLOCK)                /* 240 blocks: 121200 samples at 22050 Hz, 5.5 s */
#define RS_RATE 22050u
static uint8_t rs_buf[RS_MAX] POOL;
enum { RS_IDLE, RS_ARMED, RS_REC, RS_DONE };
static volatile uint8_t rs_state;
static volatile uint8_t rs_bars = 1;             /* 1, 2 or 4 */
static uint32_t rs_skip, rs_left, rs_n;          /* frames to the bar; frames (44.1 kHz) still to take; samples written */
static int32_t rs_pred, rs_idx, rs_prev;
static uint32_t rs_half;

/* frames (44.1 kHz) of n ticks at the clock of this block */
static uint32_t rs_ticks_frames(uint32_t n) { return (fx_spt >> 8) * n >> 8; }

static void rs_put(int32_t s)                    /* one sample at 22050 Hz into the IMA stream */
{
    uint32_t pos = rs_n % IMA_SPB, blk = rs_n / IMA_SPB;
    uint8_t *b = rs_buf + blk * IMA_BLOCK;
    if ((blk + 1u) * IMA_BLOCK > RS_MAX) { rs_left = 0; return; }
    s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
    if (pos == 0u) {                             /* a block: its first sample exact, the step index carried on */
        uint32_t i;
        rs_pred = s;
        b[0] = (uint8_t)s;
        b[1] = (uint8_t)((uint32_t)s >> 8);
        b[2] = (uint8_t)rs_idx;
        b[3] = 0;
        for (i = 4; i < IMA_BLOCK; i++) b[i] = 0;
    } else {
        int32_t d = s - rs_pred, step = IMA_STEP[rs_idx], nib = 0, diff = step >> 3;
        if (d < 0) { nib = 8; d = -d; }
        if (d >= step) { nib |= 4; d -= step; diff += step; }
        if (d >= step >> 1) { nib |= 2; d -= step >> 1; diff += step >> 1; }
        if (d >= step >> 2) { nib |= 1; diff += step >> 2; }
        rs_pred += (nib & 8) ? -diff : diff;
        rs_pred = rs_pred > 32767 ? 32767 : rs_pred < -32768 ? -32768 : rs_pred;
        rs_idx += IMA_IDX[nib & 7];
        rs_idx = rs_idx < 0 ? 0 : rs_idx > 88 ? 88 : rs_idx;
        b[4u + ((pos - 1u) >> 1)] |= (uint8_t)((pos & 1u) ? nib << 4 : nib);   /* (the high nibble first) */
    }
    rs_n++;
}

/* the audio interrupt, after the master: n frames of the output */
static void rs_tap(const int32_t *L, const int32_t *R, uint32_t n)
{
    uint32_t i;
    if (rs_state == RS_ARMED && sq_play) {       /* from the next bar (at once if this block starts one) */
        uint32_t bar = 4u * PPQN, into = fx_tick0 % bar;
        rs_skip = into == 0u && fx_ph0 == 0u ? 0u : rs_ticks_frames(bar - into) - (fx_ph0 >> 16);
        rs_left = rs_ticks_frames(bar * rs_bars);
        if (rs_left > 2u * (RS_MAX / IMA_BLOCK) * IMA_SPB) rs_left = 2u * (RS_MAX / IMA_BLOCK) * IMA_SPB;
        rs_n = 0;
        rs_idx = 0;
        rs_half = 0;
        rs_state = RS_REC;
    }
    if (rs_state != RS_REC) return;
    if (!sq_play) { rs_state = rs_n > RS_RATE / 10u ? RS_DONE : RS_IDLE; return; }   /* stopped: what was taken */
    for (i = 0; i < n && rs_left; i++) {
        int32_t m = (L[i] + R[i]) >> 1;
        if (rs_skip) { rs_skip--; continue; }
        if (rs_half) rs_put((rs_prev + m) >> 1);  /* two frames averaged: 22050 Hz */
        rs_prev = m;
        rs_half ^= 1u;
        rs_left--;
    }
    if (!rs_left) rs_state = RS_DONE;
}
