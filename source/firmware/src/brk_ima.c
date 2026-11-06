/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: 4-bit IMA ADPCM in blocks of 256 bytes, as in a WAV file (mono, nBlockAlign 256):
 *   [0..1] the first sample (int16 LE), [2] the step index (0..88), [3] 0, then 252 bytes = 504 samples,
 *   the low nibble first. 505 samples a block, each block decodes on its own: a voice seeks anywhere
 *   (a slice, a reverse, a scratch) by decoding one block. 11 KB a second at 22050 Hz.
 * The decoder runs in the audio interrupt (one block when a voice enters it); the encoder is for the computer
 * (tests, the emulator's drag and drop); the editor (JavaScript) and tools/gen_breaks.py have the same one. */

static const int16_t IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};
static const int8_t IMA_IDX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

/* one block into pcm[0..504]; pcm[505] = the next block's first sample (the interpolation's last neighbour),
 * or the block's last sample when it is the last one. n: samples in the block (the last block is short). */
static void ima_block(const uint8_t *b, int16_t *pcm, uint32_t n, const uint8_t *next)
{
    int32_t pred = (int16_t)(b[0] | b[1] << 8), idx = b[2] > 88u ? 88 : b[2];
    uint32_t i;
    pcm[0] = (int16_t)pred;
    if (n > IMA_SPB) n = IMA_SPB;
    for (i = 1; i < n; i++) {
        uint32_t nib = b[4u + ((i - 1u) >> 1)];
        int32_t step = IMA_STEP[idx], diff;
        nib = (i & 1u) ? (nib & 15u) : (nib >> 4);
        diff = step >> 3;
        if (nib & 4u) diff += step;
        if (nib & 2u) diff += step >> 1;
        if (nib & 1u) diff += step >> 2;
        pred += (nib & 8u) ? -diff : diff;
        pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
        idx += IMA_IDX[nib & 7u];
        idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
        pcm[i] = (int16_t)pred;
    }
    for (; i < IMA_SPB; i++)
        pcm[i] = (int16_t)pred;
    pcm[IMA_SPB] = next ? (int16_t)(next[0] | next[1] << 8) : (int16_t)pred;
}

static uint32_t ima_bytes(uint32_t frames) { return (frames + IMA_SPB - 1u) / IMA_SPB * IMA_BLOCK; }

#if defined(DINGHY_HOST) || defined(FM1_WASM) || defined(BRK_TEST)
/* the encoder: frames samples of pcm into out (ima_bytes(frames) bytes); the step index runs on across
 * blocks, every block starts from the exact sample */
static void ima_encode(const int16_t *pcm, uint32_t frames, uint8_t *out)
{
    int32_t idx = 0;
    uint32_t at;
    for (at = 0; at < frames; at += IMA_SPB) {
        uint8_t *b = out + at / IMA_SPB * IMA_BLOCK;
        int32_t pred = pcm[at];
        uint32_t i;
        b[0] = (uint8_t)pred;
        b[1] = (uint8_t)((uint32_t)pred >> 8);
        b[2] = (uint8_t)idx;
        b[3] = 0;
        for (i = 4; i < IMA_BLOCK; i++) b[i] = 0;
        for (i = 1; i < IMA_SPB; i++) {
            int32_t s = at + i < frames ? pcm[at + i] : pred, d = s - pred, step = IMA_STEP[idx], nib = 0, diff;
            if (d < 0) { nib = 8; d = -d; }
            diff = step >> 3;
            if (d >= step) { nib |= 4; d -= step; diff += step; }
            if (d >= step >> 1) { nib |= 2; d -= step >> 1; diff += step >> 1; }
            if (d >= step >> 2) { nib |= 1; diff += step >> 2; }
            pred += (nib & 8) ? -diff : diff;
            pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
            idx += IMA_IDX[nib & 7];
            idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
            b[4u + ((i - 1u) >> 1)] |= (uint8_t)((i & 1u) ? nib : nib << 4);
        }
    }
}
#endif
