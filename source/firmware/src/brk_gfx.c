/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAKR: drawing. The screen is drawn in 8 bands of 240 x 30 (everything outside a band is clipped), each
 * band cut in 3 tiles of 80 x 30, hashed, and only the tiles that changed are sent (the panel's SPI runs at
 * 12 MHz: a whole screen is ~77 ms, a tile ~3 ms). One band a pass of the main loop, so the keys are never
 * kept waiting. Colours RGB565, anti-aliased text (gen/brk_fonts.h). After KAWA's renderer.
 * The look: a rack's panel at night. Ink black, one signal colour a theme (and a second one for what is
 * armed), boxes with two corners cut like tape labels, a narrow grotesque for what is read and pixel type for
 * the small marks that say what a control does. */
#define BAND_H 30u
#define NBAND 8u
#define TILE_W 80u
#define NTILE 3u
static uint16_t g_buf[240u * BAND_H] POOL;
static uint16_t g_tx[2][TILE_W * BAND_H] POOL;
static int32_t g_y0;
static uint32_t g_hash[NBAND][NTILE];
static uint8_t g_force = 1, g_txi, g_band;

/* ---- colours: a dark panel, the accent of the theme, a colour a knob, one a slice */
typedef struct {
    const char *name;
    uint16_t acc, acc2;
    uint8_t soft;                               /* 1: round boxes, a slate night, pastel colours */
    uint16_t bg, panel, panel2, edge, txt, sub, dimt;
    uint16_t knob[4];
} theme_t;
#define INK RGB(7, 8, 11), RGB(19, 21, 28), RGB(32, 35, 45), RGB(50, 54, 68), RGB(242, 243, 245), RGB(150, 156, 172), RGB(90, 96, 112)
#define KNOBS {RGB(255, 84, 84), RGB(255, 206, 60), RGB(80, 220, 120), RGB(70, 150, 255)}
static const theme_t THEMES[] = {
    {"WARP", RGB(232, 255, 46), RGB(255, 64, 48), 0, INK, KNOBS},        /* acid yellow, signal red */
    {"RAVE", RGB(255, 45, 149), RGB(0, 229, 255), 0, INK, KNOBS},        /* magenta, cyan */
    {"AMEN", RGB(255, 122, 26), RGB(255, 232, 196), 0, INK, KNOBS},      /* orange, cream */
    {"PHOSPHOR", RGB(57, 255, 136), RGB(190, 255, 214), 0, INK, KNOBS},  /* green on black */
    {"MONO", RGB(244, 244, 244), RGB(255, 64, 48), 0, INK, KNOBS},       /* white, a red dot */
    {"SOFT", RGB(255, 176, 150), RGB(186, 170, 255), 1,                  /* peach and lavender on slate, round */
     RGB(28, 31, 44), RGB(42, 47, 65), RGB(57, 63, 86), RGB(78, 86, 114), RGB(248, 244, 236), RGB(186, 191, 210), RGB(132, 139, 164),
     {RGB(255, 140, 140), RGB(255, 214, 130), RGB(140, 226, 170), RGB(140, 186, 255)}},
};
#define NTHEME 6u
static uint8_t theme;
#define TH (THEMES[theme % NTHEME])
#define C_BG (TH.bg)
#define C_PANEL (TH.panel)
#define C_PANEL2 (TH.panel2)
#define C_EDGE (TH.edge)
#define C_TXT (TH.txt)
#define C_SUB (TH.sub)
#define C_DIMT (TH.dimt)
#define C_ACC (TH.acc)
#define C_ACC2 (TH.acc2)
#define C_REC RGB(255, 60, 60)
#define C_PLAY RGB(70, 230, 110)
#define KNOB_C (TH.knob)
static const uint16_t CLS_C[4] = {RGB(170, 170, 185), RGB(255, 110, 80), RGB(255, 200, 70), RGB(90, 200, 255)};
static uint16_t slice_col(uint32_t k)            /* 16 hues round the wheel */
{
    static const uint16_t H[16] = {RGB(255, 99, 72), RGB(255, 150, 50), RGB(255, 204, 51), RGB(214, 230, 60),
                                   RGB(140, 230, 80), RGB(60, 220, 120), RGB(40, 220, 180), RGB(40, 200, 230),
                                   RGB(60, 160, 255), RGB(100, 120, 255), RGB(150, 100, 255), RGB(200, 90, 240),
                                   RGB(240, 80, 200), RGB(255, 80, 150), RGB(255, 90, 110), RGB(220, 220, 220)};
    return H[k & 15u];
}

static int g_vis(int32_t y, int32_t h) { return y < g_y0 + (int32_t)BAND_H && y + h > g_y0; }

static uint16_t g_mix(uint16_t a, uint16_t b, uint32_t t)   /* a .. b, t = 0..256 */
{
    uint32_t ar = a >> 11, ag = (a >> 5) & 63u, ab = a & 31u, br = b >> 11, bg = (b >> 5) & 63u, bb = b & 31u;
    ar = (ar * (256u - t) + br * t) >> 8;
    ag = (ag * (256u - t) + bg * t) >> 8;
    ab = (ab * (256u - t) + bb * t) >> 8;
    return (uint16_t)(ar << 11 | ag << 5 | ab);
}

static void g_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t x1 = x + w, y1 = y + h, i;
    if (x < 0) x = 0;
    if (x1 > 240) x1 = 240;
    if (y < g_y0) y = g_y0;
    if (y1 > g_y0 + (int32_t)BAND_H) y1 = g_y0 + (int32_t)BAND_H;
    for (; y < y1; y++) {
        uint16_t *p = &g_buf[(uint32_t)(y - g_y0) * 240u];
        for (i = x; i < x1; i++) p[i] = c;
    }
}

static void g_blend(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c, uint32_t t)
{
    int32_t x1 = x + w, y1 = y + h, i;
    if (x < 0) x = 0;
    if (x1 > 240) x1 = 240;
    if (y < g_y0) y = g_y0;
    if (y1 > g_y0 + (int32_t)BAND_H) y1 = g_y0 + (int32_t)BAND_H;
    for (; y < y1; y++) {
        uint16_t *p = &g_buf[(uint32_t)(y - g_y0) * 240u];
        for (i = x; i < x1; i++) p[i] = g_mix(p[i], c, t);
    }
}

static void g_px(int32_t x, int32_t y, uint16_t c)
{
    y -= g_y0;
    if ((uint32_t)x < 240u && (uint32_t)y < BAND_H) g_buf[(uint32_t)y * 240u + (uint32_t)x] = c;
}
static void g_pxb(int32_t x, int32_t y, uint16_t c, uint32_t t)
{
    y -= g_y0;
    if ((uint32_t)x < 240u && (uint32_t)y < BAND_H) {
        uint16_t *p = &g_buf[(uint32_t)y * 240u + (uint32_t)x];
        *p = g_mix(*p, c, t);
    }
}

/* SOFT: a box with its four corners round (radius 6, or 3 when small), their edge anti-aliased */
static void g_roundsoft(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    static const uint16_t IN6[6] = {922, 520, 289, 140, 49, 5}, IN3[3] = {343, 102, 11};   /* the inset, Q8, a row */
    const uint16_t *in = w >= 16 && h >= 16 ? IN6 : IN3;
    int32_t r = w >= 16 && h >= 16 ? 6 : 3, j;
    if (w < 7 || h < 7) { g_rect(x, y, w, h, c); return; }
    for (j = 0; j < r; j++) {
        int32_t q = in[j], full = (q + 255) >> 8, part = q >> 8, a = 256 - (q & 255);
        int32_t yt = y + j, yb = y + h - 1 - j;
        g_rect(x + full, yt, w - 2 * full, 1, c);
        g_rect(x + full, yb, w - 2 * full, 1, c);
        if (q & 255) {
            g_pxb(x + part, yt, c, (uint32_t)a); g_pxb(x + w - 1 - part, yt, c, (uint32_t)a);
            g_pxb(x + part, yb, c, (uint32_t)a); g_pxb(x + w - 1 - part, yb, c, (uint32_t)a);
        }
    }
    g_rect(x, y + r, w, h - 2 * r, c);
}
/* a box with its top-left and bottom-right corners cut (3 px, the diagonal blended): a tape label */
static void g_round(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t r;
    if (!g_vis(y, h)) return;
    if (TH.soft) { g_roundsoft(x, y, w, h, c); return; }
    if (w < 8 || h < 8) { g_rect(x, y, w, h, c); return; }
    for (r = 0; r < 3; r++) {
        g_rect(x + 3 - r, y + r, w - 3 + r, 1, c);
        g_pxb(x + 2 - r, y + r, c, 110);
        g_rect(x, y + h - 1 - r, w - 3 + r, 1, c);
        g_pxb(x + w - 3 + r, y + h - 1 - r, c, 110);
    }
    g_rect(x, y + 3, w, h - 6, c);
}
/* the rounded box of before (radius 3), for the round things */
static void g_soft(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    if (!g_vis(y, h) || w < 6 || h < 6) { if (g_vis(y, h)) g_rect(x, y, w, h, c); return; }
    g_rect(x + 3, y, w - 6, h, c);
    g_rect(x, y + 3, 3, h - 6, c);
    g_rect(x + w - 3, y + 3, 3, h - 6, c);
    g_rect(x + 1, y + 1, 2, 2, c);
    g_rect(x + w - 3, y + 1, 2, 2, c);
    g_rect(x + 1, y + h - 3, 2, 2, c);
    g_rect(x + w - 3, y + h - 3, 2, 2, c);
    g_rect(x + 2, y, 1, 1, c); g_rect(x, y + 2, 1, 1, c);
    g_pxb(x + 1, y, c, 140); g_pxb(x, y + 1, c, 140);
    g_rect(x + w - 3, y, 1, 1, c); g_rect(x + w - 1, y + 2, 1, 1, c);
    g_pxb(x + w - 2, y, c, 140); g_pxb(x + w - 1, y + 1, c, 140);
    g_rect(x + 2, y + h - 1, 1, 1, c); g_rect(x, y + h - 3, 1, 1, c);
    g_pxb(x + 1, y + h - 1, c, 140); g_pxb(x, y + h - 2, c, 140);
    g_rect(x + w - 3, y + h - 1, 1, 1, c); g_rect(x + w - 1, y + h - 3, 1, 1, c);
    g_pxb(x + w - 2, y + h - 1, c, 140); g_pxb(x + w - 1, y + h - 2, c, 140);
}
static void g_outline(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)   /* (the label's edge) */
{
    if (!g_vis(y, h)) return;
    if (TH.soft) {                              /* (round: the box, its inside put back) */
        g_rect(x + 4, y, w - 8, 1, c); g_rect(x + 4, y + h - 1, w - 8, 1, c);
        g_rect(x, y + 4, 1, h - 8, c); g_rect(x + w - 1, y + 4, 1, h - 8, c);
        g_pxb(x + 1, y + 2, c, 200); g_pxb(x + 2, y + 1, c, 200); g_px(x + 3, y + 1, c); g_px(x + 1, y + 3, c);
        g_pxb(x + w - 2, y + 2, c, 200); g_pxb(x + w - 3, y + 1, c, 200); g_px(x + w - 4, y + 1, c); g_px(x + w - 2, y + 3, c);
        g_pxb(x + 1, y + h - 3, c, 200); g_pxb(x + 2, y + h - 2, c, 200); g_px(x + 3, y + h - 2, c); g_px(x + 1, y + h - 4, c);
        g_pxb(x + w - 2, y + h - 3, c, 200); g_pxb(x + w - 3, y + h - 2, c, 200); g_px(x + w - 4, y + h - 2, c); g_px(x + w - 2, y + h - 4, c);
        return;
    }
    g_rect(x + 3, y, w - 3, 1, c);
    g_rect(x, y + h - 1, w - 3, 1, c);
    g_rect(x, y + 3, 1, h - 3, c);
    g_rect(x + w - 1, y, 1, h - 3, c);
    g_px(x + 2, y + 1, c); g_px(x + 1, y + 2, c);
    g_px(x + w - 2, y + h - 2, c); g_px(x + w - 3, y + h - 1 - 0, c); g_px(x + w - 1, y + h - 3, c);
}
/* diagonal hatching (armed, recording, held): 2 lines of 6 */
static void g_hatch(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c, uint32_t t, uint32_t ph)
{
    int32_t i, j, y1 = y + h;
    if (y < g_y0) y = g_y0;
    if (y1 > g_y0 + (int32_t)BAND_H) y1 = g_y0 + (int32_t)BAND_H;
    for (j = y; j < y1; j++)
        for (i = x < 0 ? 0 : x; i < x + w && i < 240; i++)
            if ((uint32_t)(i + j + (int32_t)ph) % 6u < 2u) g_pxb(i, j, c, t);
}

/* text in a font (gen/brk_fonts.h), the glyph's levels blended into what is under it; returns the end x */
static int32_t g_text(const felucca_font_t *f, int32_t x, int32_t y, const char *s, uint16_t c)
{
    if (!g_vis(y, f->h)) {
        for (; *s; s++) x += f->adv[glyph(f, (uint8_t)*s)];
        return x;
    }
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), w = f->bw[gi], bpr = (w + 1u) / 2u, gx, gy;
        const uint8_t *gd = f->data + f->off[gi];
        for (gy = 0; gy < f->h; gy++) {
            int32_t py = y + (int32_t)gy - g_y0;
            uint16_t *row;
            if ((uint32_t)py >= BAND_H) continue;
            row = &g_buf[(uint32_t)py * 240u];
            for (gx = 0; gx < w; gx++) {
                uint32_t v = gd[gy * bpr + gx / 2u];
                int32_t px = x - (int32_t)f->pad + (int32_t)gx;
                v = (gx & 1u) ? (v & 15u) : (v >> 4);
                if (v && (uint32_t)px < 240u) row[px] = v >= 15u ? c : g_mix(row[px], c, v * 17u);
            }
        }
        x += f->adv[gi];
    }
    return x;
}
static int32_t g_tw(const felucca_font_t *f, const char *s)
{
    int32_t w = 0;
    for (; *s; s++) w += f->adv[glyph(f, (uint8_t)*s)];
    return w;
}
/* text cut to a width (the glyphs that fit) */
static void g_textw(const felucca_font_t *f, int32_t x, int32_t y, int32_t w, const char *s, uint16_t c)
{
    char t[24];
    uint32_t n = 0;
    int32_t used = 0;
    while (s[n] && n < sizeof t - 1u) {
        int32_t a = f->adv[glyph(f, (uint8_t)s[n])];
        if (used + a > w + 1) break;
        used += a;
        t[n] = s[n];
        n++;
    }
    t[n] = 0;
    g_text(f, x, y, t, c);
}
/* a value in a box: the big font if it fits, else the small one, else cut */
static void g_fit(int32_t x, int32_t y, int32_t w, const char *s, uint16_t c)
{
    if (g_tw(&FONT_BM, s) <= w) g_text(&FONT_BM, x, y, s, c);
    else g_textw(&FONT_BS, x, y + 2, w, s, c);
}
static void g_textc(const felucca_font_t *f, int32_t cx, int32_t y, const char *s, uint16_t c) { g_text(f, cx - g_tw(f, s) / 2, y, s, c); }
static void g_textr(const felucca_font_t *f, int32_t rx, int32_t y, const char *s, uint16_t c) { g_text(f, rx - g_tw(f, s), y, s, c); }

/* the transport's icons */
static void g_play(int32_t x, int32_t y, int32_t s, uint16_t c)   /* a triangle s high */
{
    int32_t r;
    for (r = 0; r < s; r++) {
        int32_t d = r < s / 2 ? r : s - 1 - r;
        g_rect(x, y + r, d + 1, 1, c);
        g_pxb(x + d + 1, y + r, c, 110);
    }
}
static void g_dot(int32_t cx, int32_t cy, int32_t r, uint16_t c)   /* a filled disc, its edge blended */
{
    int32_t dy, dx;
    if (!g_vis(cy - r, 2 * r + 1)) return;
    for (dy = -r; dy <= r; dy++)
        for (dx = -r; dx <= r; dx++) {
            int32_t d = dx * dx + dy * dy, r2 = r * r;
            if (d <= r2 - r) g_px(cx + dx, cy + dy, c);
            else if (d <= r2 + r) g_pxb(cx + dx, cy + dy, c, (uint32_t)(256 * (r2 + r - d) / (2 * r + 1)));
        }
}

/* numbers into text (no printf on the chip) */
static char *fmt_u(char *p, uint32_t v, uint32_t digits)
{
    char t[12];
    uint32_t n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 11u);
    while (n < digits) t[n++] = '0';
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}
static char *fmt_s(char *p, int32_t v) { if (v) *p++ = v < 0 ? '-' : '+'; return fmt_u(p, (uint32_t)(v < 0 ? -v : v), 0); }
static char *str_cat(char *p, const char *s) { while (*p) p++; while (*s) *p++ = *s++; *p = 0; return p; }
static void fmt_bpm(char *p, uint32_t b10) { p = fmt_u(p, b10 / 10u, 0); *p++ = '.'; *p++ = (char)('0' + b10 % 10u); *p = 0; }

/* one band: draw it, send the tiles that changed. Returns 1 when it was the last band of the frame. */
static uint32_t g_band_send(void (*draw)(void))
{
    uint32_t b = g_band, t;
    g_y0 = (int32_t)(b * BAND_H);
    draw();
    for (t = 0; t < NTILE; t++) {
        uint32_t h = 2166136261u, r, i, x0 = t * TILE_W;
        uint16_t *tx;
        for (r = 0; r < BAND_H; r++) {
            const uint16_t *p = &g_buf[r * 240u + x0];
            for (i = 0; i < TILE_W; i++) h = (h ^ p[i]) * 16777619u;
        }
        if (h == g_hash[b][t] && !g_force) continue;
        g_hash[b][t] = h;
        tx = g_tx[g_txi];
        g_txi ^= 1u;
        lcd_sync();                             /* (the buffer two transfers ago is free; wait for the last) */
        for (r = 0; r < BAND_H; r++) {
            const uint16_t *p = &g_buf[r * 240u + x0];
            uint16_t *q = &tx[r * TILE_W];
            for (i = 0; i < TILE_W; i++) q[i] = (uint16_t)(p[i] >> 8 | p[i] << 8);
        }
        lcd_blit(x0, b * BAND_H, TILE_W, BAND_H, tx);
    }
    g_band = (uint8_t)((b + 1u) % NBAND);
    if (g_band == 0u) { g_force = 0; return 1; }
    return 0;
}
