/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: the user's breaks over USB-MIDI, from the editor's page (Web MIDI) or install/breakr_bank.py.
 * As KAWA's samples (Felucca's way, hardware-checked): one request, one reply; the bank's header sector is
 * erased first and its header programmed last, after the CRCs of what reached the flash are checked.
 *
 * Frames: F0 7D 42 4B cmd pack7(args) F7 ("BK"; 7D: the non-commercial id; pack7: 7 bits at a time, the low
 * ones first, as the updater's). Every reply: the same cmd, pack7(rc, ...). Commands:
 *   1 INFO                         -> rc, "BRKR", version (4 chars), breaks max, capacity (3 B LE), breaks now
 *   2 BEGIN                        -> rc            (stopped only: the sound holds until END)
 *   3 WRITE off(3 B LE) data       -> rc, off(3)    (the sound: off from 0, a multiple of 256, in order; <= 256 B)
 *   4 ENTRY i entry(224 B)         -> rc, i         (the directory entry of break i)
 *   5 END header(32 B)             -> rc            (magic, version 1, count, data length, CRC data, CRC entries)
 *   6 ERASE                        -> rc            (an empty bank)
 *   7 LIST i                       -> rc, i, used, name(12), rate(2), frames(3), bars, slices
 *   8 READ off(3 B LE) n(<=96)     -> rc, off(3), data   (the bank as it is in flash: header sector + sound)
 * Codes: 0 ok, 1 bad arguments, 2 busy (playing), 3 flash, 4 CRC, 5 no session / out of order. */
enum { BK_INFO = 1, BK_BEGIN, BK_WRITE, BK_ENTRY, BK_END, BK_ERASE, BK_LIST, BK_READ };
enum { BK_OK, BK_EARGS, BK_EBUSY, BK_EFLASH, BK_ECRC, BK_ESTATE };

static uint32_t bk_pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 8u;
        while (nb >= 7u) { out[o++] = (uint8_t)(acc & 0x7Fu); acc >>= 7; nb -= 7u; }
    }
    if (nb) out[o++] = (uint8_t)(acc & 0x7Fu);
    return o;
}
static uint32_t bk_unpack7(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t max)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)(*in++ & 0x7Fu) << nb;
        nb += 7u;
        if (nb >= 8u) {
            if (o < max) out[o] = (uint8_t)acc;
            o++;
            acc >>= 8;
            nb -= 8u;
        }
    }
    return o;
}

#if defined(DINGHY_HOST)
static void bk_send(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    printf("%6u ms  sysex <", fm1_ms);
    for (i = 0; i < n; i++) printf(" %02X", p[i]);
    printf("\n");
}
#elif defined(FM1_WASM)
static uint8_t w_reply[160];
static uint32_t w_reply_n;
static void bk_send(const uint8_t *p, uint32_t n) { if (n <= sizeof w_reply) { memcpy(w_reply, p, n); w_reply_n = n; } }
#else
static void bk_send(const uint8_t *p, uint32_t n) { ota_wire_send(p, n); }
#endif

static uint8_t bk_on;                           /* a session (BEGIN .. END) */
static uint32_t bk_next, bk_ms;
static uint8_t bk_dec[300], bk_wire[8 + 140];
static uint8_t bk_last_rc;
static uint32_t bk_toast;                       /* the screen says it: 1 received, 2 erased */

static void bk_reply(uint32_t cmd, const uint8_t *b, uint32_t n)   /* n <= 110 */
{
    uint32_t w = 5;
    bk_wire[0] = 0xF0;
    bk_wire[1] = 0x7D;
    bk_wire[2] = 0x42;
    bk_wire[3] = 0x4B;
    bk_wire[4] = (uint8_t)cmd;
    w += bk_pack7(b, n, bk_wire + 5);
    bk_wire[w++] = 0xF7;
    bk_send(bk_wire, w);
}

static void bk_end_session(void) { bk_on = 0; sound_free(); }

static void bk_take(void)                       /* nothing reads the bank from here on */
{
    uint32_t i;
    sound_hold();
    for (i = NFACT; i < NBRK; i++) brk[i].data = 0;
    for (i = 0; i < NVOICE; i++) if (vo[i].b >= NFACT) vo[i].on = 0;
    nbrk_user = 0;
}

static void bk_handle(uint32_t cmd, const uint8_t *a, uint32_t n)
{
    uint8_t r[112];
    uint32_t i;
    r[0] = BK_OK;
    bk_ms = fm1_ms;
    switch (cmd) {
    case BK_INFO: {
        static const char V[] = APP_VER "    ";
        r[1] = 'B'; r[2] = 'R'; r[3] = 'K'; r[4] = 'R';
        for (i = 0; i < 4u; i++) r[5 + i] = (uint8_t)V[i];
        r[9] = NUSER;
        r[10] = (uint8_t)BANK_CAP; r[11] = (uint8_t)(BANK_CAP >> 8); r[12] = (uint8_t)(BANK_CAP >> 16);
        r[13] = (uint8_t)nbrk_user;
        bk_reply(cmd, r, 14);
        return;
    }
    case BK_BEGIN:
        if (sq_play || rs_state != RS_IDLE) { r[0] = BK_EBUSY; break; }   /* (playing, or a RESAMPLE on its way) */
        bk_take();
        if (st_erase(BANK_AT)) { r[0] = BK_EFLASH; bk_end_session(); break; }   /* the header first: empty */
        bk_on = 1;
        bk_next = 0;
        break;
    case BK_WRITE: {
        uint32_t off = n >= 3u ? (uint32_t)a[0] | (uint32_t)a[1] << 8 | (uint32_t)a[2] << 16 : 0xFFFFFFFFu, len = n - 3u;
        r[1] = n >= 3u ? a[0] : 0; r[2] = n >= 3u ? a[1] : 0; r[3] = n >= 3u ? a[2] : 0;
        if (n < 4u || len > 256u || (off & 0xFFu) || off + len > BANK_CAP) { r[0] = BK_EARGS; bk_reply(cmd, r, 4); return; }
        if (!bk_on) { r[0] = BK_ESTATE; bk_reply(cmd, r, 4); return; }
        if (off + 256u == bk_next) { r[0] = bk_last_rc; bk_reply(cmd, r, 4); return; }   /* its reply was lost */
        if (off != bk_next) { r[0] = BK_ESTATE; bk_reply(cmd, r, 4); return; }
        {
            uint32_t at = BANK_DATA + off;
            if ((at & 0xFFFu) == 0u && st_erase(at)) r[0] = BK_EFLASH;
            else if (st_prog(at, a + 3, len)) r[0] = BK_EFLASH;
        }
        if (r[0] == BK_OK) bk_next = off + 256u;
        bk_last_rc = r[0];
        bk_reply(cmd, r, 4);
        return;
    }
    case BK_ENTRY:
        r[1] = n ? a[0] : 0;
        if (n != 1u + 224u || a[0] >= NUSER) { r[0] = BK_EARGS; bk_reply(cmd, r, 2); return; }
        if (!bk_on) { r[0] = BK_ESTATE; bk_reply(cmd, r, 2); return; }
        if (st_prog(ENT_AT(a[0]), a + 1, 224u)) r[0] = BK_EFLASH;
        bk_reply(cmd, r, 2);
        return;
    case BK_END: {
        bank_hdr_t h;
        if (n != sizeof h) { r[0] = BK_EARGS; break; }
        if (!bk_on) { r[0] = BK_ESTATE; break; }
        memcpy(&h, a, sizeof h);
        if (h.magic != BANK_MAGIC || h.version != 1u || h.n > NUSER || h.data_len > bk_next ||
            crc_of(st_xip(BANK_DATA), h.data_len) != h.crc_data || crc_of(st_xip(ENT_AT(0)), h.n * 224u) != h.crc_dir) {
            r[0] = BK_ECRC;
            bk_end_session();
            bank_boot();
            break;
        }
        if (st_prog(BANK_AT, &h, sizeof h)) r[0] = BK_EFLASH;
        bk_end_session();
        bank_boot();
        bk_toast = 1;
        break;
    }
    case BK_ERASE:
        if (sq_play) { r[0] = BK_EBUSY; break; }
        bk_take();
        if (st_erase(BANK_AT)) r[0] = BK_EFLASH;
        bk_end_session();
        bank_boot();
        bk_toast = 2;
        break;
    case BK_LIST: {
        uint32_t k = n ? a[0] : 0xFFu;
        const brk_t *b = &brk[NFACT + (k < NUSER ? k : 0u)];
        uint32_t used = k < NUSER && b->data != 0;
        r[1] = (uint8_t)k;
        r[2] = (uint8_t)used;
        for (i = 0; i < 12u; i++) r[3 + i] = (uint8_t)(used ? b->name[i] : 0);
        r[15] = (uint8_t)(used ? b->rate : 0); r[16] = (uint8_t)(used ? b->rate >> 8 : 0);
        r[17] = (uint8_t)(used ? b->frames : 0); r[18] = (uint8_t)(used ? b->frames >> 8 : 0); r[19] = (uint8_t)(used ? b->frames >> 16 : 0);
        r[20] = (uint8_t)(used ? b->bars : 0);
        r[21] = (uint8_t)(used ? b->nsl : 0);
        bk_reply(cmd, r, 22);
        return;
    }
    case BK_READ: {
        uint32_t off = n >= 4u ? (uint32_t)a[0] | (uint32_t)a[1] << 8 | (uint32_t)a[2] << 16 : 0xFFFFFFFFu, len = n >= 4u ? a[3] : 0;
        r[1] = n >= 4u ? a[0] : 0; r[2] = n >= 4u ? a[1] : 0; r[3] = n >= 4u ? a[2] : 0;
        if (len > 96u || off + len > 0x1000u + BANK_CAP) { r[0] = BK_EARGS; bk_reply(cmd, r, 4); return; }
        memcpy(r + 4, st_xip(BANK_AT + off), len);
        bk_reply(cmd, r, 4 + len);
        return;
    }
    default:
        r[0] = BK_EARGS;
        break;
    }
    bk_reply(cmd, r, 1);
}

/* a frame from the computer (the bytes between F0 and F7): ours (1), or the updater's (0: ota_service takes it) */
static int app_sysex(const uint8_t *p, uint32_t n)
{
    uint32_t d;
    if (n < 4u || p[0] != 0x7Du || p[1] != 0x42u || p[2] != 0x4Bu)
        return 0;
    d = bk_unpack7(p + 4, n - 4u, bk_dec, sizeof bk_dec);
    if (d <= sizeof bk_dec)
        bk_handle(p[3], bk_dec, d);
    return 1;
}

static void bk_poll(void)                       /* a session left hanging gives the sound back */
{
    if (bk_on && fm1_ms - bk_ms > 10000u) {
        bk_end_session();
        bank_boot();
    }
}
