/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: the flash store, [STORE_LO, STORE_HI) = [0x97000, 0xE0000), the part an update never erases:
 *   4 project slots of 12 KiB (slot 0: the autosave, then A, B, C)
 *   the bank: the user's breaks (a header sector: its header, then 8 directory entries; then the sound, up to
 *   240 KiB = 22 s at 22050 Hz). Written whole by the editor over USB (brk_sysex.c), its header last: a cut
 *   upload leaves an empty bank, never a broken one. The audio interrupt reads the sound where it is.
 * A write holds the sound first (the flash stalls the chip while it erases): only when nothing plays. */

#define SLOT_SIZE 0x3000u
#define NSLOT 4u
#define SLOT_AT(k) (STORE_LO + (uint32_t)(k) * SLOT_SIZE)
#define BANK_AT (STORE_LO + NSLOT * SLOT_SIZE)
#define BANK_DATA (BANK_AT + 0x1000u)
#define BANK_CAP (STORE_HI - BANK_DATA)
#define BANK_MAGIC 0x424B5242u                  /* "BRKB" */
#define SLOT_MAGIC 0x534B5242u                  /* "BRKS" */
typedef char prj_fits_a_slot[sizeof(project_t) + 16u <= SLOT_SIZE ? 1 : -1];
typedef char store_fits[BANK_DATA + 65536u <= STORE_HI ? 1 : -1];

typedef struct {
    char name[16];
    uint32_t rate, frames, off, len, bpm10, crc;
    uint8_t bars, nsl, flags, pad;
    uint32_t sl[NSL];
    uint8_t cls[NSL];
    uint8_t rsv[20];
} bank_ent_t;                                   /* 224 B */
typedef struct { uint32_t magic, version, n, data_len, crc_data, crc_dir, rsv[2]; } bank_hdr_t;
typedef char bank_ent_224[sizeof(bank_ent_t) == 224u ? 1 : -1];
#define ENT_AT(i) (BANK_AT + 64u + (uint32_t)(i) * 224u)

/* ---- CRC-32 (the usual one), a nibble at a time */
static uint32_t crc_step(uint32_t c, const uint8_t *p, uint32_t n)
{
    static const uint32_t T[16] = {0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
                                   0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
                                   0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    while (n--) {
        c ^= *p++;
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return c;
}
static uint32_t crc_of(const uint8_t *p, uint32_t n) { return ~crc_step(0xFFFFFFFFu, p, n); }

/* ---- the flash, or a stand-in for it on a computer / in a browser */
#if defined(DINGHY_HOST) || defined(FM1_WASM)
static uint8_t host_store[STORE_HI - STORE_LO];
static uint8_t host_store_ok;
static uint32_t store_gen;                      /* +1 at every write (the emulator's page keeps the store) */
static void st_boot(void) { if (!host_store_ok) { memset(host_store, 0xFF, sizeof host_store); host_store_ok = 1; } }
static int st_erase(uint32_t off) { memset(host_store + off - STORE_LO, 0xFF, 0x1000u); store_gen++; return 0; }
static int st_prog(uint32_t off, const void *p, uint32_t n)
{
    const uint8_t *q = p;
    uint32_t i;
    for (i = 0; i < n; i++) host_store[off - STORE_LO + i] &= q[i];   /* NOR: programming only clears bits */
    store_gen++;
    return 0;
}
static int st_rd(uint32_t off, void *p, uint32_t n) { memcpy(p, host_store + off - STORE_LO, n); return 0; }
static const uint8_t *st_xip(uint32_t off) { return host_store + off - STORE_LO; }
static void hold_wait(void) {}
static int store_ok(void) { return 1; }
#else
static void st_boot(void) { if (flash_ok) fl_plain_window_init(); }   /* the data area reads as written */
static int st_erase(uint32_t off) { uint32_t took; fm1_wdt_feed(); return fl_erase4k(off, &took); }
static int st_prog(uint32_t off, const void *p, uint32_t n) { return fl_write(off, p, n); }
static int st_rd(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }
static const uint8_t *st_xip(uint32_t off) { return (const uint8_t *)FM1_XIP(off); }
static void hold_wait(void)                     /* the interrupt sees the hold: it plays zeros */
{
    uint32_t b = audio_blocks, t0 = fm1_ms;
    while (audio_blocks - b < 3u && fm1_ms - t0 < 60u)
        fm1_wdt_feed();
}
static int store_ok(void) { return flash_ok; }
#endif

static void sound_hold(void) { audio_hold = 1; hold_wait(); }
static void sound_free(void) { audio_hold = 0; }

/* ---- the project slots */
static uint8_t slot_used[NSLOT];
static uint32_t slot_check(uint32_t k)          /* 1: slot k holds a project */
{
    uint32_t h[4];
    if (k >= NSLOT || st_rd(SLOT_AT(k), h, sizeof h) || h[0] != SLOT_MAGIC || h[1] != sizeof(project_t))
        return 0;
    return crc_of(st_xip(SLOT_AT(k) + 16u), sizeof(project_t)) == h[2];
}

static int prj_save(uint32_t k)
{
    uint32_t h[4], i;
    int rc = 0;
    if (k >= NSLOT || !store_ok()) return -1;
    h[0] = SLOT_MAGIC;
    h[1] = sizeof(project_t);
    h[2] = crc_of((const uint8_t *)&prj, sizeof(project_t));
    h[3] = 0;
    sound_hold();
    for (i = 0; i < SLOT_SIZE && !rc; i += 0x1000u) rc = st_erase(SLOT_AT(k) + i);
    if (!rc) rc = st_prog(SLOT_AT(k) + 16u, &prj, sizeof(project_t));
    if (!rc) rc = st_prog(SLOT_AT(k), h, sizeof h);   /* the header last: a cut save stays invalid */
    sound_free();
    if (!rc && !slot_check(k)) rc = -2;
    slot_used[k] = (uint8_t)(rc == 0);
    if (!rc && k == 0u) pat_dirty = 0;
    return rc;
}

static int prj_load(uint32_t k)
{
    if (!slot_check(k)) return -1;
    sound_hold();
    st_rd(SLOT_AT(k) + 16u, &prj, sizeof(project_t));
    prj_sanitize();
    sound_free();
    return 0;
}

/* ---- the bank: the user's breaks into brk[NFACT..] */
static uint32_t bank_crc;                       /* the bank's (0: none) */
static void bank_boot(void)
{
    bank_hdr_t h;
    uint32_t i, k;
    nbrk_user = 0;
    for (i = NFACT; i < NBRK; i++) brk[i].data = 0;
    bank_crc = 0;
    if (!store_ok() || st_rd(BANK_AT, &h, sizeof h) || h.magic != BANK_MAGIC || h.version != 1u || h.n > NUSER ||
        h.data_len > BANK_CAP)
        return;
    if (crc_of(st_xip(ENT_AT(0)), h.n * 224u) != h.crc_dir || crc_of(st_xip(BANK_DATA), h.data_len) != h.crc_data)
        return;
    for (i = 0; i < h.n; i++) {
        bank_ent_t e;
        brk_t *b = &brk[NFACT + i];
        st_rd(ENT_AT(i), &e, sizeof e);
        if (e.rate < 8000u || e.rate > 48000u || e.frames < 256u || e.off + e.len > h.data_len ||
            e.len < ima_bytes(e.frames) || e.nsl > NSL)
            continue;
        b->frames = e.frames;
        b->rate = e.rate;
        b->bars = e.bars >= 1u && e.bars <= 8u ? e.bars : 1u;
        b->bpm10 = e.bpm10 >= 400u && e.bpm10 <= 3000u ? e.bpm10 : 2400u * b->bars * (e.rate / 10u) / (e.frames / 10u + 1u);
        b->nsl = e.nsl ? e.nsl : 1u;
        for (k = 0; k < NSL; k++) {
            b->sl[k] = k < e.nsl && e.sl[k] < e.frames ? e.sl[k] : 0;
            b->cls[k] = (uint8_t)(e.cls[k] & 3u);
            if (k && k < b->nsl && b->sl[k] <= b->sl[k - 1u]) { b->nsl = (uint8_t)k; }
        }
        b->sl[0] = 0;
        for (k = 0; k < 15u && e.name[k]; k++) b->name[k] = e.name[k] >= 'a' && e.name[k] <= 'z' ? (char)(e.name[k] - 32) : e.name[k];
        b->name[k] = 0;
        RING_PUBLISH();
        b->data = st_xip(BANK_DATA + e.off);
        nbrk_user = i + 1u;
    }
    bank_crc = h.crc_data ^ h.crc_dir;
}

/* a break appended to the bank (RESAMPLE): its sound from the next sector after the bank's, its entry, then the
 * header rewritten last (a cut write leaves the old bank's header gone: the bank then reads empty, as after a cut
 * upload). 0: done, its index in the bank in *at; -1 no store, -2 the bank is full, -3 no room, -4 flash. */
static uint8_t ent_tmp[NUSER * 224u];
static int bank_append(const uint8_t *data, uint32_t len, const bank_ent_t *e0, uint32_t *at)
{
    bank_hdr_t h;
    bank_ent_t e = *e0;
    uint32_t n = 0, off = 0, i;
    int rc = 0;
    if (!store_ok()) return -1;
    if (!st_rd(BANK_AT, &h, sizeof h) && h.magic == BANK_MAGIC && h.version == 1u && h.n <= NUSER && h.data_len <= BANK_CAP &&
        bank_crc) {                             /* (a bank that bank_boot took: kept) */
        n = h.n;
        off = (h.data_len + 0xFFFu) & ~0xFFFu;
        st_rd(ENT_AT(0), ent_tmp, n * 224u);
    }
    if (n >= NUSER) return -2;
    if (off + len > BANK_CAP) return -3;
    e.off = off;
    e.len = len;
    e.crc = crc_of(data, len);
    memcpy(ent_tmp + n * 224u, &e, 224u);
    sound_hold();
    for (i = 0; i < len && !rc; i += 0x1000u) rc = st_erase(BANK_DATA + off + i);
    if (!rc) rc = st_prog(BANK_DATA + off, data, len);
    if (!rc) {
        h.magic = BANK_MAGIC;
        h.version = 1u;
        h.n = n + 1u;
        h.data_len = off + len;
        h.crc_data = crc_of(st_xip(BANK_DATA), h.data_len);
        h.crc_dir = crc_of(ent_tmp, h.n * 224u);
        h.rsv[0] = h.rsv[1] = 0;
        rc = st_erase(BANK_AT);
        if (!rc) rc = st_prog(ENT_AT(0), ent_tmp, h.n * 224u);
        if (!rc) rc = st_prog(BANK_AT, &h, sizeof h);
    }
    sound_free();
    bank_boot();
    if (rc) return -4;
    *at = n;
    return 0;
}

/* the factory breaks into brk[0..] */
static void fact_boot(void)
{
    uint32_t i, k;
    for (i = 0; i < NFACT; i++) {
        brk_t *b = &brk[i];
        const fact_t *f = &FACT[i];
        b->frames = f->frames;
        b->rate = f->rate;
        b->bpm10 = f->bpm10;
        b->bars = f->bars;
        b->nsl = f->nsl;
        for (k = 0; k < NSL; k++) { b->sl[k] = f->sl[k]; b->cls[k] = f->cls[k]; }
        for (k = 0; k < 15u && f->name[k]; k++) b->name[k] = f->name[k];
        b->name[k] = 0;
        b->data = f->data;
    }
}
