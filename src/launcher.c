/*
 * kqjc launcher - custom home UI prototype for the MI air monitor (KQJCY02QP).
 * Static musl armhf build. Renders 800x480 landscape into a RAM buffer and
 * writes it rotated into /dev/fb0 (panel is 480x800).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "status_icons.h"
#include "m3_palette.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <termios.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <ctype.h>
#include <dirent.h>
#include "sky_stops.h"
#include "ui_i18n.h"

static void persist_spawn(const char *path, const char *text);
static void config_save(void);
static void config_load(void);
static int config_mount_ready(void);
static void org_commit(void);
static char org_input[80];
static int bl_write(int v);
static int bl_level, bl_dim;
static long bl_idle_ms;
static int config_state;
static void net_connect(void);
static void net_spawn(const char *sh);
static void upd_time(void);
static void apply_launcher_timezone(void);
static char wifi_pw[64];
static int wx_state;
static long wx_ok_ms;
static int wx_refresh_min;
static void wx_spawn_fetch(void);
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/wireless.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>

#define LW 800
#define LH 480
#define MARGIN 24
#define STATUS_H 36
#define DOCK_H 108
#define DOCK_Y (LH - 24 - DOCK_H)
#define DOTS_Y_ICONS 452
#define DOTS_Y_SENSORS 452
#define GRID_Y0 58
#define ICON 72

/* ------------------------------------------------------------------ atlas */
/* glyph record on disk: <IHhhHHII> = 22 bytes, no padding */
#define GLYPH_SZ 22
struct glyph { uint32_t cp; int16_t adv, bx, by; uint16_t w, h; uint32_t off, len; };
struct face { const uint8_t *body; int n, lh, asc; };
struct sprite { uint32_t off; uint16_t w, h; };

static uint8_t *atlas;
static size_t atlas_sz;
static uint32_t blob_off;
static struct face faces[80];
static struct sprite icons[18], masks[4];

static struct glyph gread(const uint8_t *r)
{
    uint32_t cp = (uint32_t)r[0] | (uint32_t)r[1] << 8 | (uint32_t)r[2] << 16 | (uint32_t)r[3] << 24;
    uint16_t adv = (uint16_t)(r[4] | r[5] << 8);
    uint16_t bx = (uint16_t)(r[6] | r[7] << 8);
    uint16_t by = (uint16_t)(r[8] | r[9] << 8);
    uint16_t w = (uint16_t)(r[10] | r[11] << 8);
    uint16_t h = (uint16_t)(r[12] | r[13] << 8);
    uint32_t off = (uint32_t)r[14] | (uint32_t)r[15] << 8 | (uint32_t)r[16] << 16 | (uint32_t)r[17] << 24;
    uint32_t len = (uint32_t)r[18] | (uint32_t)r[19] << 8 | (uint32_t)r[20] << 16 | (uint32_t)r[21] << 24;
    struct glyph g;
    g.cp = cp; g.adv = (int16_t)adv; g.bx = (int16_t)bx; g.by = (int16_t)by;
    g.w = w; g.h = h; g.off = off; g.len = len;
    return g;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int atlas_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("atlas: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    atlas = malloc((size_t)sz);
    if (!atlas || fread(atlas, 1, (size_t)sz, f) != (size_t)sz) { printf("atlas: read fail\n"); return -1; }
    fclose(f);
    atlas_sz = (size_t)sz;
    if (rd32(atlas) != 0x314C5441u) { printf("atlas: bad magic\n"); return -1; }
    uint32_t face_dir = rd32(atlas + 4);
    int nf = rd16(atlas + 8), ni = rd16(atlas + 10), nm = rd16(atlas + 12);
    uint32_t icon_dir = rd32(atlas + 16), mask_dir = rd32(atlas + 20);
    blob_off = rd32(atlas + 24);
    if (blob_off >= atlas_sz) { printf("atlas: bad blob offset\n"); return -1; }
    for (int i = 0; i < nf && i < 80; i++) {
        const uint8_t *r = atlas + face_dir + i * 12;
        uint32_t body = rd32(r);
        faces[i].n = (int)rd32(r + 4);
        faces[i].lh = rd16(r + 8);
        faces[i].asc = (int16_t)rd16(r + 10);
        if (body + (size_t)faces[i].n * GLYPH_SZ > atlas_sz) { printf("atlas: face %d out of range\n", i); return -1; }
        faces[i].body = atlas + body;
    }
    for (int i = 0; i < ni && i < 18; i++) {
        const uint8_t *r = atlas + icon_dir + i * 12;
        icons[i].off = rd32(r); icons[i].w = rd16(r + 4); icons[i].h = rd16(r + 6);
    }
    for (int i = 0; i < nm && i < 4; i++) {
        const uint8_t *r = atlas + mask_dir + i * 12;
        masks[i].off = rd32(r); masks[i].w = rd16(r + 4); masks[i].h = rd16(r + 6);
    }
    printf("atlas ok: faces=%d icons=%d masks=%d blob=%u size=%u\n", nf, ni, nm, blob_off, (unsigned)atlas_sz);
    return 0;
}

/* -------------------------------------------------------------------- gfx */
static uint32_t land[LW * LH];

static inline uint32_t blend(uint32_t dst, uint32_t src, int a)
{
    if (a <= 0) return dst;
    if (a >= 255) return 0xFF000000u | (src & 0x00FFFFFFu);
    int dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    int sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    return 0xFF000000u | (uint32_t)(dr + (((sr - dr) * a) >> 8)) << 16
         | (uint32_t)(dg + (((sg - dg) * a) >> 8)) << 8
         | (uint32_t)(db + (((sb - db) * a) >> 8));
}

static void fill(int x0, int y0, int x1, int y1, uint32_t c, int a)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LW) x1 = LW;
    if (y1 > LH) y1 = LH;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = land + (size_t)y * LW;
        for (int x = x0; x < x1; x++) row[x] = blend(row[x], c, a);
    }
}

/* rounded rect with signed-distance antialiasing; stroke<0 -> filled, else ring of `stroke` px */
static void rrect(int x0, int y0, int x1, int y1, float r, uint32_t c, int a, float stroke)
{
    if (r < 0.5f) r = 0.5f;
    float fx0 = (float)x0, fy0 = (float)y0, fx1 = (float)x1, fy1 = (float)y1;
    float hw = (fx1 - fx0) * 0.5f, hh = (fy1 - fy0) * 0.5f;
    float cx = fx0 + hw, cy = fy0 + hh;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    int ix0 = x0 - 1, iy0 = y0 - 1, ix1 = x1 + 1, iy1 = y1 + 1;
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > LW) ix1 = LW;
    if (iy1 > LH) iy1 = LH;
    for (int y = iy0; y < iy1; y++) {
        uint32_t *row = land + (size_t)y * LW;
        float py = (float)y + 0.5f - cy;
        for (int x = ix0; x < ix1; x++) {
            float px = (float)x + 0.5f - cx;
            float qx = fabsf(px) - (hw - r), qy = fabsf(py) - (hh - r);
            float ax = qx > 0 ? qx : 0, ay = qy > 0 ? qy : 0;
            float d = sqrtf(ax * ax + ay * ay) + (qx > qy ? (qx > 0 ? 0 : qx) : (qy > 0 ? 0 : qy)) - r;
            float cov;
            if (stroke < 0) cov = 0.5f - d;
            else {
                float dd = fabsf(d + stroke * 0.5f) - stroke * 0.5f;
                cov = 0.5f - dd;
            }
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            row[x] = blend(row[x], c, (int)(cov * (float)a));
        }
    }
}

static void sprite_rgba(const struct sprite *s, int dx, int dy, int alpha)
{
    const uint8_t *src = atlas + blob_off + s->off;
    for (int y = 0; y < s->h; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= LH) continue;
        uint32_t *row = land + (size_t)ty * LW;
        for (int x = 0; x < s->w; x++) {
            int tx = dx + x;
            if (tx < 0 || tx >= LW) continue;
            const uint8_t *p = src + ((size_t)y * s->w + x) * 4;
            int a = p[3] * alpha / 255;
            if (!a) continue;
            row[tx] = blend(row[tx], (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2], a);
        }
    }
}

static void sprite_mask(const struct sprite *s, int dx, int dy, uint32_t rgb, int alpha)
{
    const uint8_t *src = atlas + blob_off + s->off;
    for (int y = 0; y < s->h; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= LH) continue;
        uint32_t *row = land + (size_t)ty * LW;
        for (int x = 0; x < s->w; x++) {
            int tx = dx + x;
            if (tx < 0 || tx >= LW) continue;
            int a = src[(size_t)y * s->w + x] * alpha / 255;
            if (!a) continue;
            row[tx] = blend(row[tx], rgb, a);
        }
    }
}

static const struct glyph *find_glyph_static(int face, uint32_t cp)
{
    static struct glyph out;
    const struct face *f = &faces[face];
    /* records are not sorted on disk, so scan linearly (<=197 entries) */
    for (int i = 0; i < f->n; i++) {
        struct glyph g = gread(f->body + (size_t)i * GLYPH_SZ);
        if (g.cp == cp) { out = g; return &out; }
    }
    return NULL;
}

#define find_glyph find_glyph_static

static uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p;
    if (c < 0x80) { *s += 1; return c; }
    if ((c & 0xE0) == 0xC0) { c = ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu); *s += 2; return c; }
    if ((c & 0xF0) == 0xE0) { c = ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); *s += 3; return c; }
    if ((c & 0xF8) == 0xF0) { c = ((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu); *s += 4; return c; }
    *s += 1;
    return '?';
}

static int textw(const char *s, int face)
{
    s = ui_tr(s);
    int w = 0;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        const struct glyph *g = find_glyph(face, cp);
        w += g ? g->adv : 9;
    }
    return w;
}

static void text(int x, int ybase, const char *s, int face, uint32_t rgb, int alpha)
{
    s = ui_tr(s);
    int pen = x;
    const struct face *f = &faces[face];
    while (*s) {
        uint32_t cp = utf8_next(&s);
        const struct glyph *g = find_glyph(face, cp);
        if (!g) { pen += 9; continue; }
        if (g->w && g->h) {
            const uint8_t *src = atlas + blob_off + g->off;
            int gx = pen + g->bx, gy = ybase + g->by;
            for (int y = 0; y < g->h; y++) {
                int ty = gy + y;
                if (ty < 0 || ty >= LH) continue;
                uint32_t *row = land + (size_t)ty * LW;
                for (int x2 = 0; x2 < g->w; x2++) {
                    int tx = gx + x2;
                    if (tx < 0 || tx >= LW) continue;
                    int a = src[(size_t)y * g->w + x2] * alpha / 255;
                    if (!a) continue;
                    row[tx] = blend(row[tx], rgb, a);
                }
            }
        }
        pen += g->adv;
    }
    (void)f;
}

static int textcenter(int cx, int ybase, const char *s, int face, uint32_t rgb, int alpha)
{
    int w = textw(s, face);
    text(cx - w / 2, ybase, s, face, rgb, alpha);
    return w;
}

/* visible ink extent of a string, measured relative to the baseline */
static void ink_box(const char *s, int face, int *top, int *bot)
{
    s = ui_tr(s);
    int t = 0, b = 0, first = 1;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        const struct glyph *g = find_glyph(face, cp);
        if (!g || !g->w || !g->h) continue;
        int gt = g->by, gb = g->by + (int)g->h;
        if (first) { t = gt; b = gb; first = 0; }
        else { if (gt < t) t = gt; if (gb > b) b = gb; }
    }
    if (first) { t = -1; b = 1; }
    *top = t;
    *bot = b;
}

/* baseline that optically centres the ink of s on cy; centring the em box
   instead leaves CJK text sitting visibly low inside pills and rows */
static int vbase(int cy, const char *s, int face)
{
    int t, b;
    ink_box(s, face, &t, &b);
    return cy - (t + b) / 2;
}

/* --------------------------------------------------------------------- fb */
static int fbfd = -1;
static uint32_t *fb;
static size_t fb_bytes, stride_px;
static struct fb_var_screeninfo vi;

static int fb_ready;

static int fb_open(void)
{
    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) { perror("open /dev/fb0"); return -1; }
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vi)) { perror("FBIOGET_VSCREENINFO"); return -1; }
    stride_px = vi.xres_virtual;
    fb_bytes = stride_px * 4 * vi.yres_virtual;
    fb = mmap(NULL, fb_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fb == MAP_FAILED) { perror("mmap fb"); return -1; }
    printf("fb: %ux%u virt %ux%u bpp %u yoff %u\n", vi.xres, vi.yres, vi.xres_virtual,
           vi.yres_virtual, vi.bits_per_pixel, vi.yoffset);
    if (vi.xres != 480 || vi.yres != 800 || vi.bits_per_pixel != 32) {
        printf("fb: unexpected geometry\n");
        return -1;
    }
    return 0;
}

/* landscape (LW x LH) -> panel: px = (LH-1) - vy, py = vx.
 * Cross-checked against the vendor UI capture: rotating the panel image 90 deg
 * counter-clockwise gives (vx,vy) = (py, 479-px), exactly this inverse. */
static void blit_rot(uint32_t *dst)
{
    for (int by = 0; by < LH; by += 16) {
        int byEnd = by + 16 > LH ? LH : by + 16;
        for (int bx = 0; bx < LW; bx += 16) {
            int bxEnd = bx + 16 > LW ? LW : bx + 16;
            for (int vx = bx; vx < bxEnd; vx++) {
                const uint32_t *srow = land + (size_t)vx;
                uint32_t *drow = dst + (size_t)vx * stride_px + (LH - 1 - by);
                for (int vy = by; vy < byEnd; vy++) {
                    *drow = srow[(size_t)vy * LW];
                    drow--;
                }
            }
        }
    }
}

static void present(void)
{
    if (!fb_ready) return;
    uint32_t *base = (uint32_t *)fb;
    const size_t frame_px = (size_t)vi.yres * stride_px;   /* one panel frame */
    blit_rot(base);
    /* the driver alternates the two halves by itself; if we only painted one,
       the other one (still holding the vendor's last frame) flashed through */
    if (vi.yres * 2 <= vi.yres_virtual)
        blit_rot(base + frame_px);
}

/* ------------------------------------------------------------------ theme */
struct theme {
    int is_light;
    uint32_t on, on2, on3;
    uint32_t card, card_line, dock, dock_line, pill, pill_line;
    uint32_t accent, accent_ink, chip, chip_ink;
    uint32_t menu, menu_line, menu_ink;
    uint32_t dot_on, dot_off, scrim, wall_top, wall_bot, glow;
    int card_alpha, dock_alpha;
};

static struct theme TH_DARK = {
    .is_light = 0,
    .on = 0xE9EEF4, .on2 = 0x9AA2AE, .on3 = 0x78808C,
    .card = 0xFFFFFF, .card_line = 0xFFFFFF, .dock = 0xFFFFFF, .dock_line = 0xFFFFFF,
    .pill = 0xFFFFFF, .pill_line = 0xFFFFFF,
    .accent = 0x5ED6BA, .accent_ink = 0x142C26, .chip = 0x1A685A, .chip_ink = 0xBEF6E6,
    .menu = 0x262A32, .menu_line = 0xFFFFFF, .menu_ink = 0xE6EBF1,
    .dot_on = 0xFFFFFF, .dot_off = 0xFFFFFF, .scrim = 0x06080C,
    .wall_top = 0x161B24, .wall_bot = 0x0B0D11, .glow = 0x2C9E8A,
    .card_alpha = 16, .dock_alpha = 22
};

static struct theme TH_LIGHT = {
    .is_light = 1,
    .on = 0x181B21, .on2 = 0x5C6470, .on3 = 0x7A828E,
    .card = 0xFFFFFF, .card_line = 0x000000, .dock = 0xFFFFFF, .dock_line = 0x000000,
    .pill = 0xFFFFFF, .pill_line = 0x000000,
    .accent = 0xFF6900, .accent_ink = 0xFFFFFF, .chip = 0xFFE9D8, .chip_ink = 0xB24200,
    .menu = 0xFFFFFF, .menu_line = 0x000000, .menu_ink = 0x1C2028,
    .dot_on = 0x1C2028, .dot_off = 0x1C2028, .scrim = 0x12161E,
    .wall_top = 0xE4EAF2, .wall_bot = 0xD2DAE6, .glow = 0xFFD6B4,
    .card_alpha = 250, .dock_alpha = 235
};

static struct theme *TH = &TH_DARK;

/* ------------------------------------------------------------ M3 colour roles
 * Everything below is derived from one seed colour (the teal #2C9E8A) by
 * gen_m3.py, which walks the CIELAB lightness axis at the seed's hue/chroma and
 * gamut-maps back into sRGB. That gives the tonal palettes Material builds its
 * 29 roles from; the two schemes below just pick tones out of them. */
enum { T0, T10, T20, T30, T40, T50, T60, T70, T80, T90, T95, T99, T100 };

static void theme_init(void)
{
    /* dark scheme: tone 80 accents on tone 10-20 surfaces */
    TH_DARK.on       = NEUTRAL[T90];
    TH_DARK.on2      = NEUTRAL_VARIANT[T70];
    TH_DARK.on3      = NEUTRAL_VARIANT[T50];
    TH_DARK.card     = NEUTRAL[T20];
    TH_DARK.card_line = NEUTRAL_VARIANT[T30];
    TH_DARK.accent   = PRIMARY[T80];
    TH_DARK.accent_ink = PRIMARY[T10];
    TH_DARK.chip     = PRIMARY[T30];
    TH_DARK.chip_ink = PRIMARY[T90];
    TH_DARK.menu     = NEUTRAL[T20];
    TH_DARK.menu_line = NEUTRAL_VARIANT[T30];
    TH_DARK.menu_ink = NEUTRAL[T90];
    TH_DARK.dot_on   = PRIMARY[T80];
    TH_DARK.dot_off  = NEUTRAL_VARIANT[T40];
    TH_DARK.scrim    = 0x000000;
    TH_DARK.wall_top = 0x15191A;
    TH_DARK.wall_bot = 0x0A0C0D;
    TH_DARK.glow     = PRIMARY[T50];
    TH_DARK.card_alpha = 255;
    TH_DARK.dock_alpha = 255;

    /* light scheme: tone 40 accents on tone 95-100 surfaces */
    TH_LIGHT.on      = NEUTRAL[T10];
    TH_LIGHT.on2     = NEUTRAL_VARIANT[T30];
    TH_LIGHT.on3     = NEUTRAL_VARIANT[T50];
    TH_LIGHT.card    = 0xFFFFFF;
    TH_LIGHT.card_line = NEUTRAL_VARIANT[T80];
    TH_LIGHT.accent  = PRIMARY[T40];
    TH_LIGHT.accent_ink = 0xFFFFFF;
    TH_LIGHT.chip    = PRIMARY[T90];
    TH_LIGHT.chip_ink = PRIMARY[T10];
    TH_LIGHT.menu    = 0xFFFFFF;
    TH_LIGHT.menu_line = NEUTRAL_VARIANT[T70];
    TH_LIGHT.menu_ink = NEUTRAL[T10];
    TH_LIGHT.dot_on  = PRIMARY[T40];
    TH_LIGHT.dot_off = NEUTRAL_VARIANT[T60];
    TH_LIGHT.scrim   = NEUTRAL[T10];
    TH_LIGHT.wall_top = NEUTRAL[T95];
    TH_LIGHT.wall_bot = NEUTRAL[T90];
    TH_LIGHT.glow    = PRIMARY[T90];
    TH_LIGHT.card_alpha = 255;
    TH_LIGHT.dock_alpha = 255;
}


static int wall_dim;          /* wallpaper tone: 0 normal, 1 darker */
static uint32_t wall_cache[LW * LH];
static const struct theme *wall_cache_theme;
static int wall_cache_dim = -1;

static void wallpaper(void)
{
    if (wall_cache_theme == TH && wall_cache_dim == wall_dim) {
        memcpy(land, wall_cache, sizeof land);
        return;
    }
    uint32_t t0 = TH->wall_top, t1 = TH->wall_bot;
    if (wall_dim) {
        t0 = (((t0 >> 16 & 0xFF) * 7 / 10) << 16) | (((t0 >> 8 & 0xFF) * 7 / 10) << 8) | ((t0 & 0xFF) * 7 / 10);
        t1 = (((t1 >> 16 & 0xFF) * 7 / 10) << 16) | (((t1 >> 8 & 0xFF) * 7 / 10) << 8) | ((t1 & 0xFF) * 7 / 10);
    }
    for (int y = 0; y < LH; y++) {
        float t = powf((float)y / (float)(LH - 1), 0.9f);
        int r = (int)(((t0 >> 16) & 0xFF) * (1 - t) + ((t1 >> 16) & 0xFF) * t);
        int g = (int)(((t0 >> 8) & 0xFF) * (1 - t) + ((t1 >> 8) & 0xFF) * t);
        int b = (int)((t0 & 0xFF) * (1 - t) + (t1 & 0xFF) * t);
        uint32_t c = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        uint32_t *row = land + (size_t)y * LW;
        for (int x = 0; x < LW; x++) row[x] = c;
    }
    int amp = TH->is_light ? 44 : 56;
    for (int y = 0; y < LH; y++) {
        uint32_t *row = land + (size_t)y * LW;
        for (int x = 0; x < LW; x++) {
            float d1 = sqrtf((float)(x + 100) * (x + 100) + (float)(y + 300) * (y + 300)) / 470.0f;
            float d2 = sqrtf((float)(x - 980) * (x - 980) + (float)(y - 620) * (y - 620)) / 520.0f;
            float a1 = d1 < 1 ? (1 - d1) * (1 - d1) : 0;
            float a2 = d2 < 1 ? (1 - d2) * (1 - d2) : 0;
            int a = (int)((a1 * 0.62f + a2 * 0.42f) * (float)amp);
            if (a > 0) row[x] = blend(row[x], TH->glow, a);
        }
    }
    memcpy(wall_cache, land, sizeof land);
    wall_cache_theme = TH;
    wall_cache_dim = wall_dim;
}

/* --------------------------------------------------------------------- ui */
struct card { char name[20]; char value[12]; char unit[10]; char tag[32]; uint32_t color; };
static struct card SENS[6];

static const char *APP_NAMES[7] = {"时钟", "天气", "设置",
                                    "", "日历", "便签", "文件"};
static const int HOME_APPS[6] = {0, 1, 2, 4, 5, 6};
static const int DOCK_APPS[5] = {0, 1, 2, 5, 6};
#define N_HOME_APPS 6
#define N_APP_IDS 7

static int page;              /* 0 sensors, 1 icons */
static int info_open;         /* about-this-device overlay */
static int detail_open;       /* per-card sensor detail popup */
static int detail_idx;
static int history_open, history_idx, history_span, history_page;
/* tentative declarations so the popup can see the compensated values */
static long now_ms(void);
static int gas_read_baseline(char *out, size_t n);
static double raw_tc, raw_rh, comp_tc, comp_rh;
enum { CAL_IDLE = 0, CAL_RUN, CAL_OK, CAL_FAIL, CAL_SAVE };
static int cal_state;
static long cal_start_ms;
static long cal_done_at;
static char cal_reason[48];
static long cal_save_started;
static char cal_pending_base[64];

#define GAS_CAL_MS (20L * 60L * 1000L)
#define BASELINE_DIR "/mnt/UDISK/launcher"
#define BASELINE_FILE BASELINE_DIR "/iaq_baseline.txt"
#define BASELINE_TMP "/tmp/iaq_baseline.txt"
#define SGP_DIR "/sys/bus/i2c/devices/2-0058"
static int menu_open, menu_x, menu_y, menu_icon;
static int theme_idx;
static int chooser_open = 1;
static char clock_s[8] = "--:--", date_s[24] = "";
static int batt_pct = -1;
static char batt_s[8] = "--";
static int wifi_up;
static int batt_charging, batt_full, batt_discharging, batt_not_charging;
static int batt_usb_online = -1, batt_ac_online = -1, batt_current_ua = -1;
static int batt_voltage_uv = -1, batt_low_voltage_hits, batt_external_hits;
static int dirty = 1;
static int press_active, press_x, press_y;
static int press_pending, land_valid;

/* family -> weights actually shipped; combo = running index over families */
static const int FAMILY_W[3] = { 3, 3, 4 };
static const int FAMILY_BASE[3] = { 0, 3, 6 };
static int font_family = 0;   /* 0 yahei  1 mi  2 rubik */
static int font_weight = 2;   /* index inside the family */
static int font_combo = 2;
/* forward declarations: the theme file already stores these */
static int clock_style, clk_opt_open;
static int clock_family[2], clock_size_idx[2], clock_weight[2];
static int font_user;          /* set once the user picks a font explicitly */
#define FACE(sz) (font_combo * 8 + (sz))
#define FS16 0
#define FS18 1
#define FS20 2
#define FS26 3
#define FS38 4
#define FS72 5
#define FS96 6
#define FS120 7


static int sky_mode;   /* 1 while the sky-gradient clock style owns the frame */

static int pm25_to_aqi(double ug);
static const char *aqi_level(int aqi, uint32_t *col);
#include "history_data.h"

static void status_mask(const uint8_t mask[384], int x, int y, uint32_t color, int alpha)
{
    for (int py = 0; py < 16; py++) for (int px = 0; px < 24; px++) {
        int a = mask[py * 24 + px] * alpha / 255;
        if (a)
            fill(x + px, y + py, x + px + 1, y + py + 1,
                 color, a);
    }
}

enum { BAT_NORMAL, BAT_LOW, BAT_CHARGING, BAT_FULL, BAT_EXTERNAL };

static int battery_external_online(void)
{
    /* AXP22 reports data-capable hosts as USB and ordinary power adapters as
       AC. Its AC bit can also linger off-cable; Discharging rules that out. */
    return batt_usb_online == 1 ||
           (batt_ac_online == 1 &&
            (batt_charging || batt_full || batt_not_charging));
}

static int battery_voltage_low(void)
{
    return !battery_external_online() && batt_low_voltage_hits >= 2;
}

static int battery_ui_state(void)
{
    /* USB-off readings take priority over stale Charging/Full driver states. */
    if (battery_voltage_low()) return BAT_LOW;
    /* AXP22 may report Charging with only 1-2 mA flowing. The lightning
       means measurable battery charge, not merely an attached USB cable. */
    if (battery_external_online()) {
        if (batt_charging && batt_current_ua > 20000) return BAT_CHARGING;
        if (batt_pct == 100 && batt_full) return BAT_FULL;
        return BAT_EXTERNAL;
    }
    if ((batt_pct >= 0 && batt_pct < 25 &&
         (batt_discharging || !battery_external_online())))
        return BAT_LOW;
    return BAT_NORMAL;
}

static int status_text_base(int cy, const char *s, int face)
{
    int top, bot;
    ink_box(s, face, &top, &bot);
    int base = cy - (top + bot) / 2;
    if (base + top < 2) base = 2 - top;
    if (base + bot > STATUS_H - 2) base = STATUS_H - 2 - bot;
    return base;
}

static void draw_status(void)
{
    /* every element shares this optical centre line */
    const int cy = STATUS_H / 2;
    const int gap = 8;

    /* over the sky the bar sits on an arbitrary photo-like gradient, so it
       always uses light ink on a soft scrim instead of theme colours */
    uint32_t ink1 = sky_mode ? 0xFFFFFF : TH->on;
    uint32_t ink2 = sky_mode ? 0xE8EFF7 : TH->on2;
    uint32_t ink3 = sky_mode ? 0xCFD9E4 : TH->on3;
    if (sky_mode) {
        /* fade the scrim out instead of a hard-edged strip */
        for (int y = 0; y < 54; y++) {
            int a = 96 - y * 96 / 54;
            fill(0, y, LW, y + 1, 0x000000, a);
        }
    }

    /* the sky style shows the time in the middle of the screen, so the bar
       drops its own clock and date there */
    if (!sky_mode) {
        int x = MARGIN;
        /* Numerals have a shorter visible body than the square CJK glyphs.
           A 20/18 atlas pairing gives the two strings matching optical size. */
        int time_face = FACE(FS20), date_face = FACE(FS18);
        text(x, status_text_base(cy, clock_s, time_face), clock_s, time_face, ink1, 255);
        x += textw(clock_s, time_face) + gap;
        text(x, status_text_base(cy, date_s, date_face), date_s, date_face, ink3, 255);
    }

    /* Right cluster uses one optical centre and symbolic marks. */
    int right = LW - MARGIN;
    int pctw = textw(batt_s, FACE(FS16));
    int state = battery_ui_state();
    uint32_t bink = state == BAT_LOW ? 0xD05B54 : ink1;
    uint32_t pctink = state == BAT_LOW ? 0xD05B54 : ink2;
    text(right - pctw, status_text_base(cy, batt_s, FACE(FS16)),
         batt_s, FACE(FS16), pctink, 255);
    int bx = right - pctw - gap - 24;
    int by = cy - 8;
    int band = batt_pct < 25 ? 0 : batt_pct < 75 ? 1 : 2;
    status_mask(status_battery_shell, bx, by, bink, 255);
    if (state == BAT_FULL)
        status_mask(status_battery_full, bx, by, bink, 255);
    else if (batt_pct >= 0)
        status_mask(band == 0 ? status_battery_low :
                    band == 1 ? status_battery_mid : status_battery_high,
                    bx, by, bink, 255);
    int px = bx - gap - 24;
    if (state == BAT_CHARGING)
        status_mask(status_power_charging, px, by, ink1, 255);
    else if (battery_external_online())
        status_mask(status_power_plug, px, by, ink1, 255);
    status_mask(wifi_up ? status_wifi_on : status_wifi_off,
                px - gap - 24, cy - 8, wifi_up ? ink1 : ink3, 255);
}

static void draw_low_voltage_notice(void)
{
    if (!battery_voltage_low()) return;
    const char *msg = ui_lang ? "Low voltage · connect USB" : "电压偏低 · 请接电源";
    int f = FACE(FS16), w = textw(msg, f) + 44;
    if (w < 270) w = 270;
    int x = (LW - w) / 2, y = LH - 72;
    rrect(x, y, x + w, y + 40, 14, 0xAD403B, 245, -1);
    textcenter(LW / 2, vbase(y + 20, msg, f), msg, f, 0xFFFFFF, 255);
}

static void draw_dots(int active, int total, int y)
{
    int gap = 14, dot = 8, pill = 26, total_w = 0;
    for (int i = 0; i < total; i++) total_w += (i == active ? pill : dot);
    total_w += gap * (total - 1);
    int x = (LW - total_w) / 2;
    for (int i = 0; i < total; i++) {
        int w = (i == active ? pill : dot);
        rrect(x, y - 4, x + w, y + 4, 4, i == active ? TH->dot_on : TH->dot_off,
              i == active ? 245 : 78, -1);
        x += w + gap;
    }
}

static uint32_t shade(uint32_t c, float k)
{
    int r = (int)(((c >> 16) & 255) * k);
    int g = (int)(((c >> 8) & 255) * k);
    int b = (int)((c & 255) * k);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void draw_sensors(void)
{
    /* 26px headline over a dim 16px subtitle, 12px between the two ink boxes */
    text(MARGIN, 48 + faces[FACE(FS26)].asc, "室内空气", FACE(FS26), TH->on, 255);

    int cw = (LW - 2 * MARGIN - 2 * 16) / 3, ch = 118, gx = 16, gy = 20, y0 = 108;
    for (int i = 0; i < 6; i++) {
        int r = i / 3, c = i % 3;
        int bx = MARGIN + c * (cw + gx), by = y0 + r * (ch + gy);
        rrect(bx, by, bx + cw, by + ch, 20, TH->card, TH->card_alpha, -1);
        rrect(bx, by, bx + cw, by + ch, 20, TH->card_line, TH->is_light ? 16 : 22, 1.2f);

        const char *tag = SENS[i].tag;
        if (ui_lang && i == 3 && !strcmp(tag, "轻度污染")) tag = "Sensitive";
        int namew = textw(SENS[i].name, FACE(FS16));
        int tw = textw(tag, FACE(FS16)) + 20;
        if (tw > cw - 36 && ui_lang) { tag = "Alert"; tw = textw(tag, FACE(FS16)) + 20; }
        int px0 = bx + cw - 18 - tw;
        int py0 = bx + 22 + namew + 12 > px0 ? by + 40 : by + 16;
        text(bx + 22, by + 36, SENS[i].name, FACE(FS16), TH->on2, 255);
        rrect(px0, py0, px0 + tw, py0 + 22, 11, SENS[i].color, TH->is_light ? 40 : 56, -1);
        text(px0 + 10, vbase(py0 + 11, tag, FACE(FS16)), tag, FACE(FS16),
             TH->is_light ? shade(SENS[i].color, 0.55f) : SENS[i].color, 255);

        /* value baseline sits 18px above the card bottom edge */
        int vb = by + ch - 24;
        text(bx + 22, vb, SENS[i].value, FACE(FS38), TH->on, 255);
        int vw = textw(SENS[i].value, FACE(FS38));
        text(bx + 34 + vw, vb - 1, SENS[i].unit, FACE(FS16), TH->on3, 255);
    }
}

static void draw_icons(void)
{
    int cell = (LW - 2 * MARGIN) / 5;
    int asc18 = faces[FACE(FS18)].asc;
    for (int i = 0; i < N_HOME_APPS; i++) {
        int app = HOME_APPS[i], r = i / 5, c = i % 5;
        int cx = MARGIN + cell * c + cell / 2;
        int iy = 100 + r * 128;
        sprite_rgba(&icons[app], cx - ICON / 2, iy, 255);
        int tw = textw(APP_NAMES[app], FACE(FS18));
        text(cx - tw / 2, iy + ICON + 8 + asc18, APP_NAMES[app], FACE(FS18),
             TH->is_light ? 0x262A32 : 0xDEE4EC, 255);
    }
}

static const char *MENU_ITEMS[3] = {"更换主题", "壁纸明暗", "关于本机"};
#define MENU_W 252
#define MENU_ROW 46
#define MENU_H (MENU_TOP * 2 + MENU_ROW * 3)
#define MENU_TOP 22

static void menu_rect(int *mx, int *my)
{
    int x = menu_x - MENU_W / 2, y = menu_y;
    if (x < MARGIN) x = MARGIN;
    if (x + MENU_W > LW - MARGIN) x = LW - MARGIN - MENU_W;
    if (y < STATUS_H + 8) y = STATUS_H + 8;
    if (y + MENU_H > LH - 16) y = LH - 16 - MENU_H;
    *mx = x;
    *my = y;
}

static void fmt_ago(char *out, size_t n)
{
    if (cal_done_at <= 0) {
        snprintf(out, n, "已保存基线");
        return;
    }
    long d = (long)time(NULL) - cal_done_at;
    if (d < 0) d = 0;
    if (d < 3600) snprintf(out, n, "%ld 分钟前", d / 60);
    else if (d < 86400) snprintf(out, n, "%ld 小时前", d / 3600);
    else snprintf(out, n, "%ld 天前", d / 86400);
}

static void detail_box(int *px, int *py, int *pw, int *ph)
{
    int w = 470, h = detail_idx == 4 ? 368 : detail_idx == 3 ? 352 : 288;
    *px = (LW - w) / 2;
    *py = (LH - h) / 2;
    *pw = w;
    *ph = h;
}

static void detail_history_btn(int *bx0, int *by0, int *bx1, int *by1)
{
    int x, y, w, h;
    detail_box(&x, &y, &w, &h);
    *bx0 = x + 28; *bx1 = x + w - 28;
    *by0 = y + h - 68; *by1 = y + h - 20;
}

static void detail_btn(int *bx0, int *by0, int *bx1, int *by1)
{
    int x, y, w, h;
    detail_box(&x, &y, &w, &h);
    int ly = y + 62 + 40 + 40 + 52;
    *bx0 = x + 28;
    *bx1 = x + w - 28;
    *by0 = ly - 18;
    *by1 = ly + 20;
}

static void draw_detail(void)
{
    if (!detail_open) return;
    fill(0, 0, LW, LH, TH->scrim, TH->is_light ? 100 : 150);
    int x, y, w, h;
    detail_box(&x, &y, &w, &h);
    rrect(x, y, x + w, y + h, 24, TH->menu, 252, -1);
    rrect(x, y, x + w, y + h, 24, TH->menu_line, 26, 1.2f);

    text(x + 28, vbase(y + 30, SENS[detail_idx].name, FACE(FS20)), SENS[detail_idx].name,
         FACE(FS20), TH->menu_ink, 255);

    int ly = y + 62;
    char buf[96];
    if (detail_idx == 1 || detail_idx == 2) {
        double raw = detail_idx == 1 ? raw_tc : raw_rh;
        double cmpv = detail_idx == 1 ? comp_tc : comp_rh;
        int lx = x + 28, lv = x + 205;
        const char *l1 = detail_idx == 1 ? "原始（含板热）" : "原始（未修正）";
        const char *l2 = detail_idx == 1 ? "已补偿" : "已补偿";
        text(lx, vbase(ly, l1, FACE(FS16)), l1, FACE(FS16), TH->on3, 255);
        snprintf(buf, sizeof buf, detail_idx == 1 ? "%.2f °C" : "%.2f %%", raw);
        text(lv, vbase(ly, buf, FACE(FS16)), buf, FACE(FS16), TH->on2, 255);
        ly += 44;
        text(lx, vbase(ly, l2, FACE(FS16)), l2, FACE(FS16), TH->on3, 255);
        snprintf(buf, sizeof buf, detail_idx == 1 ? "%.2f °C" : "%.2f %%", cmpv);
        text(lv, vbase(ly, buf, FACE(FS16)), buf, FACE(FS16), TH->menu_ink, 255);
        ly += 44;
        text(lx, vbase(ly, "补偿量", FACE(FS16)), "补偿量", FACE(FS16), TH->on3, 255);
        snprintf(buf, sizeof buf, detail_idx == 1 ? "%+.1f K" : "%+.1f %%", cmpv - raw);
        text(lv, vbase(ly, buf, FACE(FS16)), buf, FACE(FS16), TH->on2, 255);
    } else if (detail_idx == 3) {
        int lx = x + 28, lv = x + 205;
        uint32_t acol = TH->on2;
        int aqi = atoi(SENS[3].value);
        const char *lvl = SENS[3].value[0] == '-' ? "暂无数据" : aqi_level(aqi, &acol);
        text(lx, vbase(ly, "质量等级", FACE(FS16)), "质量等级", FACE(FS16), TH->on3, 255);
        text(lv, vbase(ly, lvl, FACE(FS16)), lvl, FACE(FS16), acol, 255);
        ly += 40;
        text(lx, vbase(ly, "主导污染物", FACE(FS16)), "主导污染物", FACE(FS16), TH->on3, 255);
        text(lv, vbase(ly, "PM2.5", FACE(FS16)), "PM2.5", FACE(FS16), TH->menu_ink, 255);
        ly += 40;
        text(lx, vbase(ly, "PM2.5 当前", FACE(FS16)), "PM2.5 当前", FACE(FS16), TH->on3, 255);
        char pb[32];
        snprintf(pb, sizeof pb, "%s μg/m³", SENS[0].value);
        text(lv, vbase(ly, pb, FACE(FS16)), pb, FACE(FS16), TH->on2, 255);
        ly += 40;
        text(lx, vbase(ly, "依据", FACE(FS16)), "依据", FACE(FS16), TH->on3, 255);
        text(lv, vbase(ly, "1分钟均值按24h档位估算", FACE(FS16)), "1分钟均值按24h档位估算", FACE(FS16), TH->on2, 255);
    } else if (detail_idx == 4) {
        int lx = x + 28, lv = x + 205;
        text(lx, vbase(ly, "当前读数", FACE(FS16)), "当前读数", FACE(FS16), TH->on3, 255);
        snprintf(buf, sizeof buf, "%s %s", SENS[detail_idx].value, SENS[detail_idx].unit);
        text(lv, vbase(ly, buf, FACE(FS16)), buf, FACE(FS16), TH->menu_ink, 255);
        ly += 40;
        text(lx, vbase(ly, "基线学习", FACE(FS16)), "基线学习", FACE(FS16), TH->on3, 255);
        if (cal_state == CAL_RUN) {
            long left = (GAS_CAL_MS - (now_ms() - cal_start_ms)) / 1000;
            if (left < 0) left = 0;
            snprintf(buf, sizeof buf, "剩余 %ld:%02ld", left / 60, left % 60);
        } else if (cal_state == CAL_OK) {
            char ago[32];
            fmt_ago(ago, sizeof ago);
            snprintf(buf, sizeof buf, "已完成 · %s", ago);
        } else if (cal_state == CAL_SAVE) {
            snprintf(buf, sizeof buf, "保存并校验中…");
        } else if (cal_state == CAL_FAIL) {
            snprintf(buf, sizeof buf, "失败 · %s", cal_reason);
        } else {
            snprintf(buf, sizeof buf, "未学习");
        }
        text(lv, vbase(ly, buf, FACE(FS16)), buf, FACE(FS16), TH->on2, 255);
        ly += 40;
        text(lx, vbase(ly, "基线值", FACE(FS16)), "基线值", FACE(FS16), TH->on3, 255);
        char base[64] = "-";
        gas_read_baseline(base, sizeof base);
        text(lv, vbase(ly, base, FACE(FS16)), base, FACE(FS16), TH->on2, 255);
        ly += 52;

        const char *act;
        uint32_t btn_bg, btn_ink;
        if (cal_state == CAL_RUN) {
            act = "学习中";
            btn_bg = TH->on3;
            btn_ink = TH->on;
        } else if (cal_state == CAL_SAVE) {
            act = "正在保存";
            btn_bg = TH->on3;
            btn_ink = TH->on;
        } else if (cal_state == CAL_FAIL) {
            act = "重新学习基线";
            btn_bg = 0xE5484D;
            btn_ink = 0xFFFFFF;
        } else if (cal_state == CAL_OK) {
            act = "基线已保存";
            btn_bg = TH->accent;
            btn_ink = TH->accent_ink;
        } else {
            act = "开始基线学习";
            btn_bg = TH->accent;
            btn_ink = TH->accent_ink;
        }
        rrect(x + 28, ly - 17, x + w - 28, ly + 17, 17, btn_bg,
              cal_state == CAL_RUN || cal_state == CAL_SAVE ? 90 : 255, -1);
        int tw = textw(act, FACE(FS16));
        text(x + (w - tw) / 2, vbase(ly, act, FACE(FS16)), act, FACE(FS16), btn_ink, 255);

        const char *cap;
        if (cal_state == CAL_RUN)
            cap = "请保持环境安静，不要触碰设备";
        else if (cal_state == CAL_SAVE)
            cap = "正在确认基线已写入设备存储";
        else if (cal_state == CAL_OK)
            cap = "轻点可重新学习基线";
        else if (cal_state == CAL_FAIL)
            cap = "请检查传感器后重试";
        else
            cap = "需要静置 20 分钟";
        int cw2 = textw(cap, FACE(FS16));
        if (cw2 > w - 56) cw2 = w - 56;
        text(x + (w - cw2) / 2, ly + 38, cap, FACE(FS16), TH->on3, 255);
    } else {
        int lx = x + 28, lv = x + 205;
        text(lx, vbase(ly, "来源", FACE(FS16)), "来源", FACE(FS16), TH->on3, 255);
        const char *src = detail_idx == 0 ? "串口粒子计数（ttyS3）" : "光感 I2C 1-0023";
        text(lv, vbase(ly, src, FACE(FS16)), src, FACE(FS16), TH->on2, 255);
        ly += 44;
        text(lx, vbase(ly, "当前读数", FACE(FS16)), "当前读数", FACE(FS16), TH->on3, 255);
        snprintf(buf, sizeof buf, "%s %s", SENS[detail_idx].value, SENS[detail_idx].unit);
        text(lv, vbase(ly, buf, FACE(FS16)), buf, FACE(FS16), TH->menu_ink, 255);
    }

    int hx0, hy0, hx1, hy1;
    detail_history_btn(&hx0, &hy0, &hx1, &hy1);
    rrect(hx0, hy0, hx1, hy1, 20, TH->chip, 255, -1);
    textcenter(x + w / 2, vbase((hy0 + hy1) / 2, "History", FACE(FS18)),
               "History", FACE(FS18), TH->chip_ink, 255);
}

static void draw_menu(void)
{
    if (!menu_open) return;
    int x, y;
    menu_rect(&x, &y);
    rrect(x, y + 3, x + MENU_W, y + MENU_H + 6, 24, 0x101820, 40, -1);
    rrect(x, y, x + MENU_W, y + MENU_H, 24, TH->menu, TH->is_light ? 252 : 250, -1);
    rrect(x, y, x + MENU_W, y + MENU_H, 24, TH->menu_line, TH->is_light ? 18 : 30, 1.2f);
    for (int i = 0; i < (int)(sizeof MENU_ITEMS / sizeof MENU_ITEMS[0]); i++)
        text(x + 26, vbase(y + MENU_TOP + MENU_ROW * i + MENU_ROW / 2, MENU_ITEMS[i], FACE(FS20)),
             MENU_ITEMS[i], FACE(FS20), TH->menu_ink, 255);
}

static void draw_info(void)
{
    if (!info_open) return;
    fill(0, 0, LW, LH, TH->scrim, TH->is_light ? 118 : 168);
    int w = 520, h = 236, x = (LW - w) / 2, y = (LH - h) / 2;
    rrect(x, y + 3, x + w, y + h + 6, 24, 0x101820, 40, -1);
    rrect(x, y, x + w, y + h, 24, TH->menu, TH->is_light ? 252 : 250, -1);
    rrect(x, y, x + w, y + h, 24, TH->menu_line, TH->is_light ? 18 : 30, 1.2f);
    text(x + 28, vbase(y + 34, "关于本机", FACE(FS20)), "关于本机", FACE(FS20), TH->on, 255);
    char up[64], line[128];
    FILE *f = fopen("/proc/uptime", "r");
    double secs = 0;
    if (f) { if (fscanf(f, "%lf", &secs) != 1) secs = 0; fclose(f); }
    snprintf(up, sizeof up, "%.0f 小时 %.0f 分", secs / 3600.0, (double)((long)secs % 3600) / 60.0);
    const char *rows[4][2] = {
        {"型号", "米家空气检测仪 KQJCY02QP"},
        {"系统", "TinaLinux 3.4.39 armv7l"},
        {"界面", "自建 Launcher 1.0"},
        {"运行", up},
    };
    for (int i = 0; i < 4; i++) {
        int ry = y + 78 + i * 34;
        text(x + 28, vbase(ry, rows[i][0], FACE(FS16)), rows[i][0], FACE(FS16), TH->on3, 255);
        text(x + 118, vbase(ry, rows[i][1], FACE(FS16)), rows[i][1], FACE(FS16), TH->on2, 255);
    }
    snprintf(line, sizeof line, "点任意位置关闭");
    int tw = textw(line, FACE(FS16));
    text(x + (w - tw) / 2, y + h - 22, line, FACE(FS16), TH->on2, 255);
}

#define THEME_FILE "/tmp/launcher_theme.txt"

static void theme_save(void)
{
    FILE *f = fopen(THEME_FILE, "w");
    if (!f) return;
    fprintf(f, "%d %d %d %d %d %d %d %d %d\n", theme_idx, font_combo, clock_style,
            clock_family[0], clock_size_idx[0], clock_weight[0],
            clock_family[1], clock_size_idx[1], clock_weight[1]);
    fclose(f);
    config_save();
}

static void theme_load(void)
{
    FILE *f = fopen(THEME_FILE, "r");
    if (!f) return;
    int t = -1, fc = -1, cs = clock_style, a = -1, b = -1, c = -1, d = -1, e = -1, g = -1;
    int got = fscanf(f, "%d %d %d %d %d %d %d %d %d", &t, &fc, &cs, &a, &b, &c, &d, &e, &g);
    if (got >= 2 && t >= 0 && t <= 1) {
        if (got >= 9) {
            if (cs >= 0 && cs <= 1) clock_style = cs;
            if (a >= 0 && a <= 2 && d >= 0 && d <= 2) { clock_family[0] = a; clock_family[1] = d; }
            if (b >= 0 && b <= 2 && e >= 0 && e <= 2) { clock_size_idx[0] = b; clock_size_idx[1] = e; }
            if (c >= 0 && c <= 3 && g >= 0 && g <= 3) { clock_weight[0] = c; clock_weight[1] = g; }
        } else if (got >= 5) {
            if (cs >= 0 && cs <= 1) clock_style = cs;
            if (a >= 0 && a <= 2) { clock_size_idx[0] = a; clock_size_idx[1] = a; }
            if (b >= 0 && b <= 3) { clock_weight[0] = b; clock_weight[1] = b; }
            if (fc >= 0 && fc < 10) {
                int fam = fc >= 6 ? 2 : fc >= 3 ? 1 : 0;
                clock_family[0] = clock_family[1] = fam;
            }
        }
        theme_idx = t;
        TH = t ? &TH_LIGHT : &TH_DARK;
        if (fc >= 0) {
            font_combo = fc;
            font_user = 1;
        }
        chooser_open = 0;
    }
    fclose(f);
}

static void draw_chooser(void)
{
    if (!chooser_open) return;
    fill(0, 0, LW, LH, TH->scrim, 190);
    int w = 420, h = 240, x = (LW - w) / 2, y = (LH - h) / 2;
    rrect(x, y, x + w, y + h, 28, TH->is_light ? 0xFFFFFF : 0x22262E, 255, -1);
    textcenter(LW / 2, y + 34 + faces[FACE(FS26)].asc, "选择主题", FACE(FS26), TH->on, 255);
    int by = y + 96;
    for (int i = 0; i < 2; i++) {
        int bx = x + 32 + i * 190;
        int sel = (i == theme_idx);
        rrect(bx, by, bx + 160, by + 96, 20, i == 0 ? 0x151A22 : 0xF2F5FA, 255, -1);
        rrect(bx, by, bx + 160, by + 96, 20, sel ? TH->accent : TH->menu_line, sel ? 255 : 20, sel ? 2.5f : 1.2f);
        textcenter(bx + 80, vbase(by + 32, i == 0 ? "深色" : "浅色", FACE(FS20)), i == 0 ? "深色" : "浅色",
                   FACE(FS20), i == 0 ? 0xE9EEF4 : 0x181B21, 255);
        textcenter(bx + 80, vbase(by + 64, i == 0 ? "Material You" : "MI_LanTing", FACE(FS16)),
                   i == 0 ? "Material You" : "MI_LanTing", FACE(FS16), i == 0 ? 0x9AA2AE : 0x68707E, 255);
    }
}

static const char *dump_path;

static void dump_land(void)
{
    if (!dump_path) return;
    FILE *f = fopen(dump_path, "wb");
    if (f) {
        fwrite(land, 4, (size_t)LW * LH, f);
        fclose(f);
        fprintf(stderr, "shot %dx%d -> %s\n", LW, LH, dump_path);
    } else {
        fprintf(stderr, "shot: cannot write %s\n", dump_path);
    }
    dump_path = NULL;
}


/* --------------------------------------------------------------------- apps */
static int app_open = -1;      /* -1 = home, otherwise the app index */
static int clock_style;        /* 0 = big digits, 1 = sunrise / sunset card */
static int clock_family[2] = {0, 0};
static int clock_size_idx[2] = {1, 1};   /* 0 = 72, 1 = 96, 2 = 120 px */
static int clock_weight[2] = {2, 2};     /* each style owns its face */
static int clk_opt_open;

static int clock_face(void)
{
    static const int sz_idx[3] = {FS72, FS96, FS120};
    static const int nw[3] = {3, 3, 4};
    int fam = clock_family[clock_style ? 1 : 0];
    int w = clock_weight[clock_style ? 1 : 0];
    if (w > nw[fam] - 1) w = nw[fam] - 1;
    if (w < 0) w = 0;
    int size = clock_size_idx[clock_style ? 1 : 0];
    return (FAMILY_BASE[fam] + w) * 8 + sz_idx[size < 0 ? 0 : (size > 2 ? 2 : size)];
}

/* left-pointing chevron, used by the app header's back button */
static void tri_left(int cx, int cy, int size, uint32_t col, int alpha)
{
    int h = size, w = size / 2;
    int right = cx + w / 2;
    for (int dy = -h / 2; dy <= h / 2; dy++) {
        int d = dy < 0 ? -dy : dy;
        int span = (h / 2 - d) * w / (h / 2 ? h / 2 : 1);
        int y = cy + dy;
        if (y < 0 || y >= LH) continue;
        for (int dx = 0; dx < span; dx++) {
            int x = right - span + dx;
            if (x < 0 || x >= LW) continue;
            uint32_t *px = land + (size_t)y * LW + x;
            *px = blend(*px, col, alpha);
        }
    }
}

static void draw_app_header(const char *title)
{
    int cy = STATUS_H + 26;
    rrect(20, cy - 18, 56, cy + 18, 18, sky_mode ? 0x16212C : TH->on3, 255, -1);
    tri_left(40, cy, 14, sky_mode ? 0xFFFFFF : (TH->is_light ? 0xFFFFFF : 0x101311), 255);
    if (title && title[0])
        text(70, vbase(cy, title, FACE(FS20)), title, FACE(FS20), sky_mode ? 0xFFFFFF : TH->on, 255);
}

/* big-digits clock */
static void draw_clock_big(void)
{
    int cf = clock_face();
    int asc = faces[cf].asc;
    int tw = textw(clock_s, cf);
    int base = 196 + asc / 2;
    text((LW - tw) / 2, base + 2, clock_s, cf, 0x000000, 60);
    text((LW - tw) / 2, base, clock_s, cf, TH->on, 255);

    int tw2 = textw(date_s, FACE(FS20));
    text((LW - tw2) / 2, base + 62, date_s, FACE(FS20), TH->on2, 255);

    const char *hint = "轻点切换样式";
    int tw3 = textw(hint, FACE(FS16));
    text((LW - tw3) / 2, LH - 42, hint, FACE(FS16), TH->on3, 255);
}

/* vertical gradient sky taken from the macOS Solar Gradients set: 16 frames
   spread evenly over the day, thirteen colour stops interpolated per row */
static uint32_t lerp888(uint32_t a, uint32_t b, float t)
{
    int ar = (int)((a >> 16) & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF), bg = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
    int r = (int)((float)ar + (float)(br - ar) * t + 0.5f);
    int g = (int)((float)ag + (float)(bg - ag) * t + 0.5f);
    int bl = (int)((float)ab + (float)(bb - ab) * t + 0.5f);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

static void sky_at(int minutes, uint32_t stops[SKY_N])
{
    float fp = (float)(minutes % 1440) / 90.0f;
    int i0 = ((int)fp) % 16;
    int i1 = (i0 + 1) % 16;
    float t = fp - (float)(int)fp;
    for (int k = 0; k < SKY_N; k++)
        stops[k] = lerp888(SKY_STOPS[i0][k], SKY_STOPS[i1][k], t);
}

static uint32_t sky_row(const uint32_t stops[SKY_N], int y, int sky_h)
{
    float v = (float)y / (float)(sky_h - 1) * (float)(SKY_N - 1);
    int k = (int)v;
    if (k > SKY_N - 2) k = SKY_N - 2;
    if (k < 0) k = 0;
    return lerp888(stops[k], stops[k + 1], v - (float)k);
}

static int preview_hour = -1;

static void draw_clock_sun(void)
{
    int hour = 12, minute = 0;
    if (clock_s[0] >= '0' && clock_s[4] >= '0') {
        hour = (clock_s[0] - '0') * 10 + (clock_s[1] - '0');
        minute = (clock_s[3] - '0') * 10 + (clock_s[4] - '0');
    }
    if (preview_hour >= 0) { hour = preview_hour; minute = 0; }
    int minutes = hour * 60 + minute;

    uint32_t stops[SKY_N];
    sky_at(minutes, stops);

    /* the wallpaper alone: a smooth vertical gradient, nothing else */
    sky_mode = 1;
    const int sky_h = 322;
    for (int y = 0; y < LH; y++)
        fill(0, y, LW, y + 1, sky_row(stops, y < sky_h ? y : sky_h - 1, sky_h), 255);

    /* the digits are always white, whatever the sky behind them looks like */
    int base = LH - 30;
    int cf = clock_face();
    int tw = textw(clock_s, cf);
    text(30, base + 2, clock_s, cf, 0x000000, 130);
    text(30, base, clock_s, cf, 0xFFFFFF, 255);
    char buf[32];
    snprintf(buf, sizeof buf, "%s%s", SENS[1].value[0] ? SENS[1].value : "--", SENS[1].unit);
    text(30 + tw + 26, base - 12, buf, FACE(FS20), 0x000000, 130);
    text(30 + tw + 26, base - 14, buf, FACE(FS20), 0xFFFFFF, 240);
}


/* --------------------------------------------------------------- keyboard */
static int kb_open, kb_shift;
static char kb_text[80];
static int kb_len;
static char *kb_target;              /* where OK commits the typed text */
static char city[48] = "";           /* used later by the weather page */
static char apikey[64] = "";         /* weather API key (ASCII, keyboard friendly) */
static int wx_src;                   /* 0 = open-meteo, 1 = domestic API */

#define KB_H 246

static void kb_panel(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = 12;
    *x1 = LW - 12;
    *y1 = LH - 10;
    *y0 = *y1 - KB_H;
}

static void kb_slot(int row, int col, int span, int *x0, int *y0, int *x1, int *y1)
{
    int px0, py0, px1, py1;
    kb_panel(&px0, &py0, &px1, &py1);
    int gw = (px1 - px0 - 20) / 10, gap = 5;
    int rx = px0 + 10 + col * gw;
    *x0 = rx;
    *x1 = rx + gw * span - gap;
    *y0 = py0 + 44 + row * 40;
    *y1 = *y0 + 36;
}

static const char *kb_row_text(int row)
{
    static const char *r[4] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    return r[row];
}

static void kb_insert(char c)
{
    int limit = kb_target == org_input ? 72 : (int)sizeof kb_text - 1;
    if (kb_len < limit) {
        kb_text[kb_len++] = c;
        kb_text[kb_len] = 0;
    }
}

static void draw_key(int row, int col, int span, const char *label, int accent)
{
    int x0, y0, x1, y1;
    kb_slot(row, col, span, &x0, &y0, &x1, &y1);
    uint32_t bg = accent ? TH->accent : (TH->is_light ? 0xE7ECF3 : 0x333A46);
    uint32_t ink = accent ? TH->accent_ink : TH->on;
    rrect(x0, y0, x1, y1, 9, bg, 255, -1);
    int tw = textw(label, FACE(FS20));
    textcenter((x0 + x1) / 2, vbase((y0 + y1) / 2, label, FACE(FS20)), label, FACE(FS20), ink, 255);
    (void)tw;
}

/* ------------------------------------------------------ clock options sheet */
#define CLK_OPT_W 520
#define CLK_OPT_H 360
#define CLK_LBL_H 24

static void clk_opt_rect(int *x, int *y)
{
    *x = (LW - CLK_OPT_W) / 2;
    *y = (LH - CLK_OPT_H) / 2;
}

static int clk_opt_cols(int row)
{
    if (row == 0) return 2;
    if (row == 1 || row == 2) return 3;
    return clock_family[clock_style] == 2 ? 4 : 3;
}

static void clk_opt_chip(int row, int col, int cols, int *x0, int *y0, int *x1, int *y1)
{
    int bx, by;
    clk_opt_rect(&bx, &by);
    int pad = 22, gap = 10;
    int cw = (CLK_OPT_W - pad * 2 - gap * (cols - 1)) / cols;
    *x0 = bx + pad + col * (cw + gap);
    *x1 = *x0 + cw;
    *y0 = by + 52 + row * 70 + CLK_LBL_H;
    *y1 = *y0 + 38;
}

static void clk_opt_draw(void)
{
    if (!clk_opt_open) return;
    fill(0, 0, LW, LH, TH->scrim, TH->is_light ? 110 : 160);
    int bx, by;
    clk_opt_rect(&bx, &by);
    rrect(bx, by, bx + CLK_OPT_W, by + CLK_OPT_H, 26, TH->menu, TH->is_light ? 252 : 250, -1);
    rrect(bx, by, bx + CLK_OPT_W, by + CLK_OPT_H, 26, TH->menu_line, TH->is_light ? 16 : 26, 1.2f);
    text(bx + 22, vbase(by + 30, "时钟设置", FACE(FS20)), "时钟设置", FACE(FS20), TH->menu_ink, 255);

    static const char *LBL[4] = {"样式", "字体", "字号", "字重"};
    static const char *STYLE[2] = {"大字", "渐变"};
    static const char *FAMILY[3] = {"雅黑", "MI", "Rubik"};
    static const char *SZ[3] = {"72", "96", "120"};
    static const char *WT[4] = {"细", "常规", "中等", "粗"};
    uint32_t off_bg = TH->is_light ? 0xE7ECF3 : 0x333A46;

    for (int r = 0; r < 4; r++) {
        text(bx + 22, vbase(by + 52 + r * 70 + CLK_LBL_H / 2, LBL[r], FACE(FS16)),
             LBL[r], FACE(FS16), TH->on3, 255);
        int n = clk_opt_cols(r);
        for (int c = 0; c < n; c++) {
            int x0, y0, x1, y1;
            clk_opt_chip(r, c, n, &x0, &y0, &x1, &y1);
            int on = r == 0 ? (clock_style == c)
                   : r == 1 ? (clock_family[clock_style] == c)
                   : r == 2 ? (clock_size_idx[clock_style] == c)
                            : (clock_weight[clock_style] == c);
            const char *lb = r == 0 ? STYLE[c] : r == 1 ? FAMILY[c] : r == 2 ? SZ[c] : WT[c];
            rrect(x0, y0, x1, y1, 12, on ? TH->accent : off_bg, 255, -1);
            textcenter((x0 + x1) / 2, vbase((y0 + y1) / 2, lb, FACE(FS20)), lb, FACE(FS20),
                       on ? TH->accent_ink : TH->on, 255);
        }
    }
}

static int clk_opt_hit(int x, int y)
{
    if (!clk_opt_open) return 0;
    int bx, by;
    clk_opt_rect(&bx, &by);
    if (x < bx || x > bx + CLK_OPT_W || y < by || y > by + CLK_OPT_H) {
        clk_opt_open = 0;
        dirty = 1;
        return 1;
    }
    for (int r = 0; r < 4; r++) {
        int n = clk_opt_cols(r);
        for (int c = 0; c < n; c++) {
            int x0, y0, x1, y1;
            clk_opt_chip(r, c, n, &x0, &y0, &x1, &y1);
            if (x < x0 || x > x1 || y < y0 || y > y1) continue;
            if (r == 0) clock_style = c;
            else if (r == 1) {
                clock_family[clock_style] = c;
                if (c != 2 && clock_weight[clock_style] > 2) clock_weight[clock_style] = 2;
            }
            else if (r == 2) clock_size_idx[clock_style] = c;
            else clock_weight[clock_style] = c;
            theme_save();
            dirty = 1;
            return 1;
        }
    }
    return 1;
}

static void draw_keyboard(void)
{
    if (!kb_open) return;
    int x0, y0, x1, y1;
    kb_panel(&x0, &y0, &x1, &y1);
    rrect(x0, y0 - 6, x1, y1 + 10, 26, TH->menu, TH->is_light ? 252 : 250, -1);
    rrect(x0, y0 - 6, x1, y1 + 10, 26, TH->menu_line, TH->is_light ? 16 : 26, 1.2f);
    /* the value being edited */
    char masked[sizeof kb_text];
    if ((kb_target == wifi_pw || kb_target == apikey) && kb_len) {
        int n = kb_len < (int)sizeof masked - 1 ? kb_len : (int)sizeof masked - 1;
        memset(masked, '*', (size_t)n);
        masked[n] = 0;
    }
    const char *shown = kb_len ? ((kb_target == wifi_pw || kb_target == apikey) ? masked : kb_text) : "输入";
    if (kb_target == org_input && kb_len > 0) {
        while (textw(shown, FACE(FS20)) > x1 - x0 - 50 && *shown) shown++;
    }
    text(x0 + 22, vbase(y0 + 22, shown, FACE(FS20)), shown, FACE(FS20),
         kb_len ? TH->on : TH->on3, 255);
    rrect(x0 + 14, y0 + 30, x1 - 14, y0 + 31, 0, TH->accent, 255, -1);

    for (int row = 0; row < 4; row++) {
        const char *rt = kb_row_text(row);
        int n = (int)strlen(rt);
        for (int i = 0; i < n; i++) {
            char lb[2] = {rt[i], 0};
            if (rt[i] >= 'a' && rt[i] <= 'z' && kb_shift) lb[0] = (char)(lb[0] - 32);
            draw_key(row, row == 3 ? i + 1 : i, 1, lb, 0);
        }
        if (row == 2) draw_key(2, 9, 1, "DEL", 0);
        if (row == 3) {
            draw_key(3, 0, 1, "SFT", kb_shift);
            draw_key(3, 8, 1, ".", 0);
            draw_key(3, 9, 1, "_", 0);
        }
    }
    draw_key(4, 0, 2, "@", 0);
    draw_key(4, 2, 5, "SPACE", 0);
    draw_key(4, 7, 1, "-", 0);
    draw_key(4, 8, 2, "OK", 1);
}

static void kb_hit(int x, int y)
{
    int x0, y0, x1, y1;
    for (int row = 0; row < 5; row++) {
        int total = row == 4 ? 4 : 10;
        for (int i = 0; i < total; i++) {
            int col = i, span = 1;
            if (row == 4) {
                col = i == 0 ? 0 : i == 1 ? 2 : i == 2 ? 7 : 8;
                span = i == 1 ? 5 : i == 2 ? 1 : 2;
            }
            kb_slot(row, col, span, &x0, &y0, &x1, &y1);
            if (x < x0 || x > x1 || y < y0 || y > y1) continue;
            if (row == 2 && i == 9) {
                if (kb_len > 0) kb_text[--kb_len] = 0;
                dirty = 1;
                return;
            }
            if (row == 3 && i == 0) {
                kb_shift ^= 1;
                dirty = 1;
                return;
            }
            if (row == 3 && i == 8) { kb_insert('.'); dirty = 1; return; }
            if (row == 3 && i == 9) { kb_insert('_'); dirty = 1; return; }
            if (row == 4) {
                if (i == 0) { kb_insert('@'); }
                else if (i == 1) { kb_insert(' '); }
                else if (i == 2) { kb_insert('-'); }
                else {
                    if (kb_target) {
                        size_t target_size = kb_target == city ? sizeof city
                                           : kb_target == wifi_pw ? sizeof wifi_pw
                                           : kb_target == org_input ? sizeof org_input : sizeof apikey;
                        snprintf(kb_target, target_size, "%s", kb_text);
                        if (kb_target == wifi_pw) net_connect();
                        else if (kb_target == org_input) org_commit();
                        else { config_save(); wx_state = 0; if (wifi_up) wx_spawn_fetch(); }
                    }
                    kb_open = 0;
                }
                dirty = 1;
                return;
            }
            const char *rt = kb_row_text(row);
            char c = rt[row == 3 ? i - 1 : i];
            if (c >= 'a' && c <= 'z' && kb_shift) c = (char)(c - 32);
            kb_insert(c);
            dirty = 1;
            return;
        }
    }
    /* a tap outside the panel closes it */
    if (y < y0) { kb_open = 0; dirty = 1; }
}

/* -------------------------------------------------------------- time sync */
static int tsync_state;             /* 0 idle, 1 running, 2 done, 3 failed */
static int64_t tsync_t0, tsync_last_try, ntp_last_try;
static long last_wxauto, last_auto;
static pid_t tsync_pid;
static time_t tsync_last_success;
static char tsync_source[12];
static int tsync_no_network;
#define NTP_HOST "ntp.aliyun.com"
#define HTTPS_TIME_URL "https://www.baidu.com/"
static int tz_offset_min = 480; /* local clock only; UTC+08:00 */
static int sync_interval_hours = 12;

static int64_t sync_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void apply_launcher_timezone(void)
{
    char zone[32];
    int minutes = tz_offset_min < 0 ? -tz_offset_min : tz_offset_min;
    /* POSIX TZ signs are the inverse of the displayed UTC offset. */
    snprintf(zone, sizeof zone, "UTC%c%d:%02d",
             tz_offset_min >= 0 ? '-' : '+', minutes / 60, minutes % 60);
    setenv("TZ", zone, 1);
    tzset();
}

static void timezone_label(char *out, size_t cap)
{
    int minutes = tz_offset_min < 0 ? -tz_offset_min : tz_offset_min;
    snprintf(out, cap, "UTC%c%02d:%02d", tz_offset_min >= 0 ? '+' : '-',
             minutes / 60, minutes % 60);
}

static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097L + (long)doe - 719468L;
}

static int month_from_name(const char *v)
{
    static const char *M[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; i++)
        if (!strncmp(v, M[i], 3)) return i + 1;
    return 0;
}

/* "Wed, 27 Jan 2026 08:39:00 GMT" -> epoch seconds */
static long parse_http_date(const char *v)
{
    while (*v == ' ') v++;
    const char *comma = strchr(v, ',');
    if (!comma) return 0;
    v = comma + 1;
    while (*v == ' ') v++;
    int day = atoi(v);
    while (*v && *v != ' ') v++;
    while (*v == ' ') v++;
    int mon = month_from_name(v);
    while (*v && *v != ' ') v++;
    while (*v == ' ') v++;
    int year = atoi(v);
    while (*v && *v != ' ') v++;
    while (*v == ' ') v++;
    int hh = atoi(v);
    while (*v && *v != ':') v++;
    if (*v == ':') v++;
    int mm = atoi(v);
    while (*v && *v != ':') v++;
    if (*v == ':') v++;
    int ss = atoi(v);
    if (mon == 0 || year < 2020) return 0;
    long days = days_from_civil(year, mon, day);
    return days * 86400L + hh * 3600L + mm * 60L + ss;
}

static long read_http_epoch(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    long epoch = 0;
    while (fgets(line, sizeof line, f)) {
        char low[8] = {0};
        for (int i = 0; i < 5 && line[i]; i++)
            low[i] = (char)tolower((unsigned char)line[i]);
        if (!strncmp(low, "date:", 5)) {
            epoch = parse_http_date(line + 5);
            break;
        }
    }
    fclose(f);
    return epoch;
}

static double ntp_wire_time(const unsigned char *p)
{
    uint32_t sec, frac;
    memcpy(&sec, p, 4); memcpy(&frac, p + 4, 4);
    uint64_t whole = ntohl(sec);
    /* NTP's 32-bit seconds field rolls over in 2036. */
    if (whole < 2208988800ULL) whole += 4294967296ULL;
    return (double)(whole - 2208988800ULL) + (double)ntohl(frac) / 4294967296.0;
}

static int ntp_query(struct timeval *out)
{
    struct addrinfo hint = {0}, *list = NULL;
    hint.ai_family = AF_INET;
    hint.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(NTP_HOST, "123", &hint, &list)) return -1;
    int result = -1;
    for (struct addrinfo *it = list; it; it = it->ai_next) {
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        struct timeval timeout = {3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        if (connect(fd, it->ai_addr, it->ai_addrlen)) { close(fd); continue; }
        unsigned char req[48] = {0}, reply[128];
        req[0] = 0x23; /* NTPv4 client mode */
        req[2] = 6;    /* 64-second poll interval */
        struct timeval t1, t4;
        gettimeofday(&t1, NULL);
        uint32_t sec = htonl((uint32_t)((uint64_t)t1.tv_sec + 2208988800ULL));
        uint32_t frac = htonl((uint32_t)((uint64_t)t1.tv_usec * 4294967296ULL / 1000000ULL));
        memcpy(req + 40, &sec, 4); memcpy(req + 44, &frac, 4);
        ssize_t n = send(fd, req, sizeof req, 0) == (ssize_t)sizeof req
                  ? recv(fd, reply, sizeof reply, 0) : -1;
        gettimeofday(&t4, NULL);
        close(fd);
        if (n < 48 || (reply[0] >> 6) == 3 || (reply[0] & 7) != 4 ||
            reply[1] == 0 || reply[1] > 15 ||
            memcmp(reply + 24, req + 40, 8)) continue;
        double server_rx = ntp_wire_time(reply + 32);
        double server_tx = ntp_wire_time(reply + 40);
        if (server_rx < 1600000000.0 || server_tx < 1600000000.0 ||
            server_tx > 4102444800.0) continue;
        double client_tx = (double)t1.tv_sec + t1.tv_usec / 1e6;
        double client_rx = (double)t4.tv_sec + t4.tv_usec / 1e6;
        double offset = ((server_rx - client_tx) + (server_tx - client_rx)) / 2.0;
        double corrected = client_rx + offset;
        out->tv_sec = (time_t)corrected;
        out->tv_usec = (suseconds_t)((corrected - out->tv_sec) * 1000000.0);
        result = 0;
        break;
    }
    freeaddrinfo(list);
    return result;
}

static void time_sync_spawn(void)
{
    if (tsync_state == 1) return;
    if (!wifi_up) {
        tsync_state = 3;
        tsync_no_network = 1;
        dirty = 1;
        return;
    }
    tsync_state = 1;
    tsync_no_network = 0;
    dirty = 1;
    unlink("/tmp/time_sync_result");
    tsync_t0 = sync_mono_ms();
    tsync_last_try = tsync_t0;
    int try_ntp = ntp_last_try == 0 || tsync_t0 - ntp_last_try >= 60000;
    if (try_ntp) ntp_last_try = tsync_t0;
    pid_t pid = fork();
    if (pid < 0) { tsync_state = 3; return; }
    if (pid == 0) {
        alarm(20);
        struct timeval tv = {0, 0};
        const char *source = "NTP";
        int ok = try_ntp && ntp_query(&tv) == 0;
        if (!ok) {
            source = "HTTPS";
            unlink("/tmp/http_head.txt.part");
            system("curl -sI -m 8 -k " HTTPS_TIME_URL
                   " > /tmp/http_head.txt.part 2>/dev/null");
            rename("/tmp/http_head.txt.part", "/tmp/http_head.txt");
            long epoch = read_http_epoch("/tmp/http_head.txt");
            if (epoch >= 1600000000L) { tv.tv_sec = epoch; tv.tv_usec = 0; ok = 1; }
        }
        int fd = open("/tmp/time_sync_result.part", O_WRONLY|O_CREAT|O_TRUNC, 0600);
        if (fd >= 0) {
            if (ok) dprintf(fd, "%ld %ld %s\n", (long)tv.tv_sec, (long)tv.tv_usec, source);
            else dprintf(fd, "0 0 failed\n");
            close(fd);
            rename("/tmp/time_sync_result.part", "/tmp/time_sync_result");
        }
        _exit(0);
    }
    tsync_pid = pid;
}

static void time_try_apply(void)
{
    FILE *f = fopen("/tmp/time_sync_result", "r");
    if (!f) return;
    long epoch = 0, usec = 0;
    char source[12] = {0};
    int got = fscanf(f, "%ld %ld %11s", &epoch, &usec, source);
    fclose(f);
    unlink("/tmp/time_sync_result");
    if (tsync_pid) { waitpid(tsync_pid, NULL, WNOHANG); tsync_pid = 0; }
    if (got != 3 || epoch < 1600000000L || usec < 0 || usec >= 1000000L) {
        tsync_state = 3; dirty = 1; return;
    }
    struct timeval tv;
    tv.tv_sec = (time_t)epoch;
    tv.tv_usec = (suseconds_t)usec;
    if (settimeofday(&tv, NULL) == 0) {
        printf("time synced to %ld via %s\n", epoch, source);
        fflush(stdout);
        tsync_state = 2;
        tsync_last_success = (time_t)epoch;
        snprintf(tsync_source, sizeof tsync_source, "%s", source);
        upd_time();
        dirty = 1;
    } else {
        tsync_state = 3;
        dirty = 1;
    }
}

static void net_autopilot(void)
{
    long t = now_ms();
    int64_t sync_now = sync_mono_ms();
    if (!wifi_up) {
        static long last_dhcp_try;
        if (t - last_dhcp_try > 30000) {
            last_dhcp_try = t;
            net_spawn("wpa_cli -p /etc/wifi/sockets -i wlan0 status | grep -q '^wpa_state=COMPLETED' && "
                      "! ip -4 addr show wlan0 | grep -q 'inet ' && "
                      "udhcpc -i wlan0 -q -n -t 5 -T 2 >/dev/null 2>&1");
        }
        return;
    }
    /* The retained RTC can be wrong by hours while still reporting a plausible
       year. Sync after every boot/connect and periodically, not only before 2020. */
    if (tsync_state == 1) {
        if (access("/tmp/time_sync_result", F_OK) == 0) time_try_apply();
        else if (sync_now - tsync_t0 > 22000) {
            if (tsync_pid) { kill(tsync_pid, SIGKILL); waitpid(tsync_pid, NULL, WNOHANG); tsync_pid = 0; }
            tsync_state = 3; dirty = 1;
        }
    } else if (tsync_last_try == 0 ||
               sync_now - tsync_last_try >
                 (tsync_state == 2 ? (int64_t)sync_interval_hours * 3600000 : 60000)) {
        time_sync_spawn();
    }
    if (wx_state == 0 || wx_state == 3 ||
        (wx_state == 2 && t - wx_ok_ms > wx_refresh_min * 60000L)) {
        if (t - last_wxauto > 60000) { last_wxauto = t; wx_spawn_fetch(); }
    }
}

/* ------------------------------------------------------------------ wifi */
#define WPA_P "/etc/wifi/sockets"

static int net_sub;                 /* 0 = settings list, 1 = network page */
static int settings_page;           /* 0 = list, 1 = status, 2 = controls, 3 = weather */
static int status_detail;           /* 0 = overview, 1 = power, 2 = network, 3 = sensors */
static void draw_status_detail(void);
static int net_state;               /* 0 idle, 1 scanning, 2 list, 3 connecting, 4 ok, 5 connect fail, 6 scan fail */
static long net_t0;
static char net_ssid[40];
static char net_link[64];           /* ssid + ip, shown at the top */
static int net_nap;
static char net_ap[8][40];
static int net_rssi[8];

static void net_spawn(const char *sh)
{
    int pid = fork();
    if (pid != 0) return;
    setsid();
    execl("/bin/sh", "sh", "-c", sh, (char *)NULL);
    _exit(127);
}

static void net_scan(void)
{
    unlink("/tmp/net_scan.txt");
    net_state = 1;
    net_t0 = now_ms();
    dirty = 1;
    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "wpa_cli -p %s -i wlan0 scan >/dev/null 2>&1; sleep 3; "
             "wpa_cli -p %s -i wlan0 scan_results > /tmp/net_scan.txt 2>/dev/null", WPA_P, WPA_P);
    net_spawn(cmd);
}

static void net_parse_scan(void)
{
    FILE *f = fopen("/tmp/net_scan.txt", "r");
    if (!f) { net_state = 2; return; }
    char line[256];
    net_nap = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'b' && line[1] == 's') continue;   /* header */
        char *last = NULL;
        for (char *q = line; *q; q++)
            if (*q == '\t') last = q;
        if (!last) continue;
        char *ssid = last + 1;
        size_t l = strlen(ssid);
        while (l && (ssid[l - 1] == '\n' || ssid[l - 1] == '\r')) ssid[--l] = 0;
        if (!l) continue;
        int sig = 0;
        {
            int tabs = 0;
            char *q = line;
            while (*q && tabs < 2) { if (*q == '\t') tabs++; q++; }
            sig = atoi(q);
        }
        if (net_nap < 8) {
            snprintf(net_ap[net_nap], sizeof net_ap[0], "%s", ssid);
            net_rssi[net_nap] = sig;
            net_nap++;
        }
    }
    fclose(f);
    net_state = 2;
    dirty = 1;
}

static void net_connect(void)
{
    unlink("/tmp/net_done");
    unlink("/tmp/net_status.txt");
    unlink("/tmp/net_saved");
    net_state = 3;
    net_t0 = now_ms();
    dirty = 1;
    /* keep the credentials out of the shell quoting rules by writing a script */
    char ssid_q[80], pw_q[140];
    size_t o = 0;
    for (const char *q = net_ssid; *q && o < sizeof ssid_q - 5; q++) {
        if (*q == '\'') { ssid_q[o++] = '\''; ssid_q[o++] = '\\'; ssid_q[o++] = '\''; ssid_q[o++] = '\''; }
        else ssid_q[o++] = *q;
    }
    ssid_q[o] = 0;
    o = 0;
    for (const char *q = wifi_pw; *q && o < sizeof pw_q - 5; q++) {
        if (*q == '\'') { pw_q[o++] = '\''; pw_q[o++] = '\\'; pw_q[o++] = '\''; pw_q[o++] = '\''; }
        else pw_q[o++] = *q;
    }
    pw_q[o] = 0;

    FILE *f = fopen("/tmp/net_conn.sh", "w");
    if (!f) { net_state = 5; dirty = 1; return; }
    fchmod(fileno(f), 0600);
    fprintf(f,
            "#!/bin/sh\n"
            "P=%s\n"
            "ID=`wpa_cli -p $P -i wlan0 add_network | tail -1`\n"
            "wpa_cli -p $P -i wlan0 set_network $ID ssid '\"%s\"' >/dev/null\n"
            "wpa_cli -p $P -i wlan0 set_network $ID psk '\"%s\"' >/dev/null\n"
            "wpa_cli -p $P -i wlan0 set_network $ID priority 20 >/dev/null\n"
            "wpa_cli -p $P -i wlan0 enable_network $ID >/dev/null\n"
            "wpa_cli -p $P -i wlan0 select_network $ID >/dev/null\n"
            "sleep 4\n"
            "udhcpc -i wlan0 -q -n -t 5 -T 2 >/dev/null 2>&1\n"
            "( wpa_cli -p $P -i wlan0 status; ip -4 addr show wlan0 ) > /tmp/net_status.txt 2>&1\n"
            "if grep -q '^wpa_state=COMPLETED' /tmp/net_status.txt && grep -q 'inet ' /tmp/net_status.txt; then\n"
            "  wpa_cli -p $P -i wlan0 enable_network all >/dev/null\n"
            "  if wpa_cli -p $P -i wlan0 save_config | grep -q '^OK'; then\n"
            "    chmod 600 /etc/wifi/wpa_supplicant.conf\n"
            "    sync\n"
            "    echo ok > /tmp/net_saved\n"
            "  else\n"
            "    echo failed > /tmp/net_saved\n"
            "  fi\n"
            "fi\n"
            "echo done > /tmp/net_done\n"
            "rm -f /tmp/net_conn.sh\n",
            WPA_P, ssid_q, pw_q);
    fclose(f);
    net_spawn("/bin/sh /tmp/net_conn.sh");
}

static void net_poll(void)
{
    if (net_state == 1) {
        if (access("/tmp/net_scan.txt", F_OK) == 0) net_parse_scan();
        else if (now_ms() - net_t0 > 12000) { net_state = 6; dirty = 1; }
    } else if (net_state == 3) {
        if (access("/tmp/net_done", F_OK) == 0) {
            FILE *f = fopen("/tmp/net_status.txt", "r");
            net_link[0] = 0;
            if (f) {
                char line[160];
                char ssid[40] = "";
                char ip[40] = "";
                while (fgets(line, sizeof line, f)) {
                    if (!strncmp(line, "ssid=", 5)) {
                        snprintf(ssid, sizeof ssid, "%s", line + 5);
                        ssid[strcspn(ssid, "\r\n")] = 0;
                    }
                    const char *q = strstr(line, "inet ");
                    if (q) { q += 5; size_t k = 0; while (*q && *q != ' ' && k < sizeof ip - 1) ip[k++] = *q++; ip[k] = 0; }
                }
                fclose(f);
                if (ssid[0] && ip[0]) snprintf(net_link, sizeof net_link, "%s %s", ssid, ip);
            }
            net_state = net_link[0] ? 4 : 5;
            if (net_state == 4) {
                FILE *sf = fopen("/tmp/net_saved", "r");
                char saved[16] = {0};
                if (sf) { fgets(saved, sizeof saved, sf); fclose(sf); }
                printf("network profile save: %s\n",
                       !strncmp(saved, "ok", 2) ? "ok" : "failed");
                fflush(stdout);
            }
            dirty = 1;
        } else if (now_ms() - net_t0 > 40000) { net_state = 5; dirty = 1; }
    }
}

static void draw_network(void)
{
    char buf[80];
    int y = STATUS_H + 58;
    rrect(MARGIN, y, LW - MARGIN, y + 66, 16, TH->card, TH->card_alpha, -1);
    const char *st = net_state == 3 ? "连接中…" : wifi_up ? net_link
                     : net_state == 5 ? "连接失败，请重试" : "未连接";
    text(MARGIN + 20, vbase(y + 33, st, FACE(FS18)), st, FACE(FS18), TH->on, 255);

    int by = y + 82;
    const char *bt = net_state == 1 ? "扫描中…" : net_state == 6 ? "扫描失败 · 重试" : "扫描附近 WiFi";
    rrect(MARGIN, by, LW - MARGIN, by + 52, 14, TH->accent, 255, -1);
    int tw = textw(bt, FACE(FS18));
    text(MARGIN + ((LW - 2 * MARGIN) - tw) / 2, vbase(by + 26, bt, FACE(FS18)), bt, FACE(FS18),
         TH->accent_ink, 255);

    for (int i = 0; i < net_nap && i < 5; i++) {
        int ry = by + 62 + i * 48;
        if (ry > LH - 30) break;
        rrect(MARGIN, ry, LW - MARGIN, ry + 44, 14, TH->card, TH->card_alpha, -1);
        text(MARGIN + 18, vbase(ry + 22, net_ap[i], FACE(FS18)), net_ap[i], FACE(FS18), TH->on, 255);
        snprintf(buf, sizeof buf, "%d", net_rssi[i]);
        int w = textw(buf, FACE(FS16));
        text(LW - MARGIN - 16 - w, vbase(ry + 22, buf, FACE(FS16)), buf, FACE(FS16), TH->on3, 255);
    }
}

static int network_hit(int x, int y)
{
    int y0 = STATUS_H + 58;
    if (x >= MARGIN && x <= LW - MARGIN && y >= y0 + 82 && y <= y0 + 134) {
        net_scan();
        return 1;
    }
    for (int i = 0; i < net_nap && i < 5; i++) {
        int ry = y0 + 82 + 62 + i * 48;
        if (x >= MARGIN && x <= LW - MARGIN && y >= ry && y <= ry + 44) {
            snprintf(net_ssid, sizeof net_ssid, "%s", net_ap[i]);
            net_link[0] = 0;
            kb_target = wifi_pw;
            wifi_pw[0] = 0;
            snprintf(kb_text, sizeof kb_text, "%s", "");
            kb_len = 0;
            kb_shift = 0;
            kb_open = 1;
            dirty = 1;
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- weather */
#define WX_JSON "/tmp/wx.json"
#define WX_LAT "39.90"
#define WX_LON "116.40"

static long wx_started_ms, wx_ok_ms;
static double wx_cur_t, wx_feel, wx_hum, wx_wind;
static int wx_cur_code = -1;
static int wx_refresh_min = 30;
static double wx_tmax[4], wx_tmin[4];
static int wx_dcode[4];
static int wx_cn_ready;
static char wx_cn_city[64], wx_cn_city_en[64], wx_cn_phrase[64], wx_cn_alert[128], wx_cn_error[32];
static double wx_cn_temp, wx_cn_feel, wx_cn_hum, wx_cn_wind;
static char wx_cn_code[12];
static int wx_cn_daylight = 1, wx_page;
static struct { double low, high; char phrase[64], code[12]; } wx_cn_day[5];
static struct { int hour, pop, daylight; double temp; char code[12]; } wx_cn_hour[12];

static int wx_supported(const char *s, int face)
{
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (!find_glyph(face, cp)) return 0;
    }
    return 1;
}

static void wx_cn_load(void)
{
    const char *preview = getenv("WXCN_PREVIEW_FILE");
    FILE *f = fopen(preview && *preview ? preview : "/tmp/wxcn.txt", "r");
    if (!f) return;
    char line[256];
    int temp = 0, days = 0, hours = 0;
    while (fgets(line, sizeof line, f)) {
        char *v = strchr(line, '=');
        if (!v) continue;
        *v++ = 0;
        v[strcspn(v, "\r\n")] = 0;
        int i;
        if (!strcmp(line, "city")) snprintf(wx_cn_city, sizeof wx_cn_city, "%s", v);
        else if (!strcmp(line, "city_en")) snprintf(wx_cn_city_en, sizeof wx_cn_city_en, "%s", v);
        else if (!strcmp(line, "temp")) { wx_cn_temp = atof(v); temp = 1; }
        else if (!strcmp(line, "feel")) wx_cn_feel = atof(v);
        else if (!strcmp(line, "humidity")) wx_cn_hum = atof(v);
        else if (!strcmp(line, "wind")) wx_cn_wind = atof(v);
        else if (!strcmp(line, "phrase")) snprintf(wx_cn_phrase, sizeof wx_cn_phrase, "%s", v);
        else if (!strcmp(line, "code")) snprintf(wx_cn_code, sizeof wx_cn_code, "%s", v);
        else if (!strcmp(line, "daylight")) wx_cn_daylight = strcmp(v, "false") != 0;
        else if (!strcmp(line, "alert")) snprintf(wx_cn_alert, sizeof wx_cn_alert, "%s", v);
        else if (sscanf(line, "day%d", &i) == 1 && i >= 0 && i < 5) {
            char *a = strchr(v, '|'), *b = a ? strchr(a + 1, '|') : NULL;
            char *c = b ? strchr(b + 1, '|') : NULL;
            if (a && b) {
                *a++ = 0; *b++ = 0;
                if (c) *c++ = 0;
                wx_cn_day[i].low = atof(v); wx_cn_day[i].high = atof(a);
                snprintf(wx_cn_day[i].phrase, sizeof wx_cn_day[i].phrase, "%s", b);
                if (c) snprintf(wx_cn_day[i].code, sizeof wx_cn_day[i].code, "%s", c);
                days++;
            }
        } else if (sscanf(line, "hour%d", &i) == 1 && i >= 0 && i < 12) {
            char *a = strchr(v, '|'), *b = a ? strchr(a + 1, '|') : NULL;
            char *c = b ? strchr(b + 1, '|') : NULL;
            char *d = c ? strchr(c + 1, '|') : NULL;
            if (a && b) {
                *a++ = 0; *b++ = 0;
                if (c) *c++ = 0;
                if (d) *d++ = 0;
                wx_cn_hour[i].hour = atoi(v); wx_cn_hour[i].temp = atof(a);
                wx_cn_hour[i].pop = atoi(b); hours++;
                if (c) snprintf(wx_cn_hour[i].code, sizeof wx_cn_hour[i].code, "%s", c);
                wx_cn_hour[i].daylight = !d || strcmp(d, "false") != 0;
            }
        }
    }
    fclose(f);
    if (!wx_supported(wx_cn_city, FACE(FS18)))
        snprintf(wx_cn_city, sizeof wx_cn_city, "%s", wx_cn_city_en[0] ? wx_cn_city_en : "自动定位");
    if (!wx_supported(wx_cn_phrase, FACE(FS20)))
        snprintf(wx_cn_phrase, sizeof wx_cn_phrase, "天气信息");
    if (!wx_supported(wx_cn_alert, FACE(FS20)))
        snprintf(wx_cn_alert, sizeof wx_cn_alert, "生效预警");
    for (int i = 0; i < 5; i++)
        if (!wx_supported(wx_cn_day[i].phrase, FACE(FS16)))
            snprintf(wx_cn_day[i].phrase, sizeof wx_cn_day[i].phrase, "天气预报");
    if (temp && days == 5 && hours == 12) {
        wx_cn_ready = 1;
        wx_state = 2; wx_ok_ms = now_ms();
        wx_cn_error[0] = 0;
    } else { wx_state = 3; snprintf(wx_cn_error, sizeof wx_cn_error, "数据格式异常"); }
}

static const char *wx_desc(int code, char *buf, int cap)
{
    const char *s2 = "-";
    if (code == 0) s2 = "晴";
    else if (code == 1) s2 = "少云";
    else if (code == 2) s2 = "多云";
    else if (code == 3) s2 = "阴";
    else if (code == 45 || code == 48) s2 = "雾";
    else if (code >= 51 && code <= 57) s2 = "毛毛雨";
    else if (code >= 61 && code <= 65) s2 = "雨";
    else if (code == 66 || code == 67) s2 = "冻雨";
    else if (code >= 71 && code <= 77) s2 = "雪";
    else if (code >= 80 && code <= 82) s2 = "阵雨";
    else if (code == 85 || code == 86) s2 = "阵雪";
    else if (code >= 95) s2 = "雷雨";
    snprintf(buf, cap, "%s", s2);
    return buf;
}

static int wx_find(const char *s, const char *key, const char **at)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *q = strstr(s, pat);
    if (!q) return 0;
    q = strchr(q + strlen(pat), ':');
    if (!q) return 0;
    *at = q + 1;
    return 1;
}

static double wx_num(const char *s, const char *key, double dflt)
{
    const char *at;
    if (!wx_find(s, key, &at)) return dflt;
    return strtod(at, NULL);
}

static void wx_arr(const char *s, const char *key, double *out, int n)
{
    const char *at;
    if (!wx_find(s, key, &at)) return;
    const char *p2 = at;
    for (int i = 0; i < n; i++) {
        while (*p2 && *p2 != '-' && *p2 != ']' && !(*p2 >= '0' && *p2 <= '9')) p2++;
        if (*p2 == ']' || !*p2) return;
        out[i] = strtod(p2, (char **)&p2);
    }
}

static void wx_parse(const char *txt)
{
    const char *cur = strstr(txt, "\"current\"");
    if (cur) {
        wx_cur_t = wx_num(cur, "temperature_2m", 0);
        wx_feel = wx_num(cur, "apparent_temperature", wx_cur_t);
        wx_hum = wx_num(cur, "relative_humidity_2m", 0);
        wx_wind = wx_num(cur, "wind_speed_10m", 0);
        wx_cur_code = (int)wx_num(cur, "weather_code", -1);
    }
    double codes[4], mx[4], mn[4];
    for (int i = 0; i < 4; i++) { codes[i] = -1; mx[i] = mn[i] = 0; }
    const char *dl = strstr(txt, "\"daily\"");
    const char *src = dl ? dl : txt;
    wx_arr(src, "weather_code", codes, 4);
    wx_arr(src, "temperature_2m_max", mx, 4);
    wx_arr(src, "temperature_2m_min", mn, 4);
    for (int i = 0; i < 4; i++) {
        wx_dcode[i] = (int)codes[i];
        wx_tmax[i] = mx[i];
        wx_tmin[i] = mn[i];
    }
}

static void wx_try_load(void)
{
    if (apikey[0]) { wx_cn_load(); return; }
    FILE *f = fopen(WX_JSON, "r");
    if (!f) return;
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    if (n < 40) return;
    wx_parse(buf);
    wx_state = wx_cur_code >= 0 ? 2 : 3;
    wx_ok_ms = now_ms();
}

static void wx_spawn_fetch(void)
{
    if (wx_state == 1) return;
    if (apikey[0]) {
        unlink("/tmp/wxcn.ready"); unlink("/tmp/wxcn.err");
        wx_state = 1; wx_started_ms = now_ms(); dirty = 1;
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            setenv("WEATHER_KEY", apikey, 1);
            setenv("WEATHER_CITY", city, 1);
            execl("/bin/sh", "sh", BASELINE_DIR "/weathercn_fetch.sh", (char *)NULL);
            _exit(127);
        }
        if (pid < 0) wx_state = 3;
        return;
    }
    unlink("/tmp/wx.json.new");
    wx_state = 1;
    wx_started_ms = now_ms();
    dirty = 1;
    int pid = fork();
    if (pid != 0) return;
    setsid();
    char url[384];
    snprintf(url, sizeof url,
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
             "weather_code,wind_speed_10m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min"
             "&timezone=Asia%%2FShanghai&forecast_days=4", WX_LAT, WX_LON);
    execl("/usr/bin/curl", "curl", "-s", "-m", "12", "-k", "-o", "/tmp/wx.json.new", url,
          (char *)NULL);
    _exit(127);
}

static void wx_poll(void)
{
    if (apikey[0] && wx_state == 1) {
        if (access("/tmp/wxcn.ready", F_OK) == 0) {
            rename("/tmp/wxcn.ready", "/tmp/wxcn.txt");
            wx_cn_load(); dirty = 1;
        } else if (access("/tmp/wxcn.err", F_OK) == 0) {
            FILE *f = fopen("/tmp/wxcn.err", "r");
            if (f) { (void)fgets(wx_cn_error, sizeof wx_cn_error, f); fclose(f); }
            wx_cn_error[strcspn(wx_cn_error, "\r\n")] = 0;
            wx_state = 3; dirty = 1;
        } else if (now_ms() - wx_started_ms > 90000) {
            snprintf(wx_cn_error, sizeof wx_cn_error, "请求超时");
            wx_state = 3; dirty = 1;
        }
        return;
    }
    if (wx_state == 1) {
        if (access("/tmp/wx.json.new", F_OK) == 0) {
            rename("/tmp/wx.json.new", WX_JSON);
            wx_try_load();
            dirty = 1;
        } else if (now_ms() - wx_started_ms > 20000) {
            wx_state = 3;
            dirty = 1;
        }
    }
}

static void wx_fit(char *out, size_t cap, const char *src, int face, int maxw)
{
    char translated[256];
    snprintf(translated, sizeof translated, "%s", ui_tr(src));
    src = translated;
    size_t n = 0;
    out[0] = 0;
    while (*src && n + 5 < cap) {
        unsigned char c = (unsigned char)*src;
        int bytes = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (n + (size_t)bytes + 4 >= cap) break;
        memcpy(out + n, src, (size_t)bytes);
        n += (size_t)bytes; out[n] = 0;
        if (textw(out, face) > maxw) {
            n -= (size_t)bytes;
            memcpy(out + n, "…", 4);
            return;
        }
        src += bytes;
    }
}

static void wx_line(int x0, int y0, int x1, int y1, uint32_t col, int thick)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        fill(x0 - thick / 2, y0 - thick / 2, x0 + (thick + 1) / 2,
             y0 + (thick + 1) / 2, col, 255);
        if (x0 == x1 && y0 == y1) break;
        int e = 2 * err;
        if (e >= dy) { err += dy; x0 += sx; }
        if (e <= dx) { err += dx; y0 += sy; }
    }
}

/* WeatherCN's LocalSource.WeatherCode follows its published China code table.
   A phrase fallback keeps older cached summaries usable. */
static int wx_kind(const char *code, const char *phrase)
{
    if (code && *code) {
        int n = atoi(code);
        if (n == 0) return 0;             /* clear */
        if (n == 1) return 1;             /* partly cloudy */
        if (n == 2) return 2;             /* overcast */
        if (n == 4 || n == 5) return 4;   /* thunder */
        if ((n >= 13 && n <= 17) || (n >= 26 && n <= 28) || n == 302) return 5;
        if ((n >= 18 && n <= 20) || (n >= 29 && n <= 33) ||
            (n >= 49 && n <= 58)) return 6;
        return 3;                         /* rain */
    }
    if (phrase && strstr(phrase, "雷")) return 4;
    if (phrase && strstr(phrase, "雪")) return 5;
    if (phrase && strstr(phrase, "雨")) return 3;
    if (phrase && (strstr(phrase, "雾") || strstr(phrase, "霾"))) return 6;
    if (phrase && strstr(phrase, "云")) return 1;
    if (phrase && strstr(phrase, "阴")) return 2;
    return 0;
}

static void wx_icon(int x, int y, int s, int kind, int daylight, uint32_t bg)
{
    uint32_t sun = 0xE9B75B, cloud = TH->is_light ? 0x788996 : 0xC2CDD6;
    uint32_t rain = 0x609CE2;
    int thick = s >= 48 ? 3 : 2;
    if (kind == 0 || kind == 1) {
        int cx = x + (kind == 1 ? s * 3 / 8 : s / 2);
        int cy = y + (kind == 1 ? s * 3 / 8 : s / 2);
        int r = s / 5;
        if (daylight) {
            rrect(cx - r, cy - r, cx + r, cy + r, r, sun, 255, -1);
            for (int k = 0; k < 8; k++) {
                static const int vx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
                static const int vy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
                wx_line(cx + vx[k] * (r + 4), cy + vy[k] * (r + 4),
                        cx + vx[k] * (r + s / 7 + 4),
                        cy + vy[k] * (r + s / 7 + 4), sun, thick);
            }
        } else {
            rrect(cx - r, cy - r, cx + r, cy + r, r, sun, 255, -1);
            rrect(cx - r / 2, cy - r - 2, cx + r + 2, cy + r / 2,
                  r, bg, 255, -1);
        }
    }
    if (kind >= 1 && kind <= 5) {
        int cy = y + s * 3 / 5;
        rrect(x + s / 7, cy - s / 10, x + s * 6 / 7, cy + s / 7,
              s / 10, cloud, 255, -1);
        rrect(x + s / 4, cy - s / 4, x + s * 3 / 5, cy + s / 12,
              s / 6, cloud, 255, -1);
        rrect(x + s / 2, cy - s / 5, x + s * 4 / 5, cy + s / 10,
              s / 7, cloud, 255, -1);
    }
    if (kind == 3 || kind == 4) {
        for (int k = 0; k < 3; k++) {
            int rx = x + s / 3 + k * s / 6;
            wx_line(rx, y + s * 4 / 5, rx - s / 14, y + s * 9 / 10,
                    rain, thick);
        }
    }
    if (kind == 4) {
        wx_line(x + s / 2, y + s * 2 / 3, x + s * 2 / 5,
                y + s * 4 / 5, sun, thick);
        wx_line(x + s * 2 / 5, y + s * 4 / 5, x + s * 3 / 5,
                y + s * 4 / 5, sun, thick);
    }
    if (kind == 5) {
        for (int k = 0; k < 3; k++) {
            int xx = x + s / 3 + k * s / 6, yy = y + s * 5 / 6;
            wx_line(xx - 3, yy, xx + 3, yy, rain, thick);
            wx_line(xx, yy - 3, xx, yy + 3, rain, thick);
        }
    }
    if (kind == 6) {
        for (int k = 0; k < 3; k++)
            wx_line(x + s / 5, y + s / 3 + k * s / 5,
                    x + s * 4 / 5, y + s / 3 + k * s / 5, cloud, thick);
    }
}

static void wx_tabs(void)
{
    static const char *labels[3] = {"现在", "5日", "12小时"};
    for (int i = 0; i < 3; i++) {
        int x = 300 + i * 106, y = STATUS_H + 8;
        rrect(x, y, x + 100, y + 40, 20,
              i == wx_page ? TH->accent : TH->chip, 255, -1);
        textcenter(x + 50, vbase(y + 20, labels[i], FACE(FS18)), labels[i],
                   FACE(FS18), i == wx_page ? TH->accent_ink : TH->chip_ink, 255);
    }
}

static void wx_cn_draw(void)
{
    char buf[128], fit[128];
    if (!wx_cn_ready) {
        const char *reason = !strcmp(wx_cn_error, "ip") ? "IP 定位失败，可在设置中填写城市" :
                             !strcmp(wx_cn_error, "location") ? "城市解析失败，请检查城市名称" :
                             !strcmp(wx_cn_error, "no_ca") ? "证书文件缺失" :
                             !strcmp(wx_cn_error, "no_key") || !strcmp(wx_cn_error, "bad_key") ? "请检查 API Key" :
                             !strcmp(wx_cn_error, "current") || !strcmp(wx_cn_error, "daily") ||
                             !strcmp(wx_cn_error, "hourly") || !strcmp(wx_cn_error, "alerts") ?
                             "接口请求失败，请检查密钥与网络" : wx_cn_error;
        text(MARGIN, 150, wx_state == 1 ? "正在获取华风爱科天气…" :
             wx_state == 3 ? "天气获取失败" : "等待天气数据", FACE(FS26), TH->on, 255);
        text(MARGIN, 200, wx_state == 3 ? reason : "位置会按网络 IP 粗略定位", FACE(FS18), TH->on3, 255);
        draw_app_header("天气");
        return;
    }
    if (wx_page == 0) {
        rrect(24, 105, 776, 322, 24, TH->card, TH->card_alpha, -1);
        snprintf(buf, sizeof buf, "%s", ui_lang && wx_cn_city_en[0] ? wx_cn_city_en :
                 wx_cn_city[0] ? wx_cn_city : "自动定位");
        wx_fit(fit, sizeof fit, buf, FACE(FS18), 300);
        text(52, 145, fit, FACE(FS18), TH->on2, 255);
        snprintf(buf, sizeof buf, "%.0f°", wx_cn_temp);
        text(48, 273, buf, FACE(FS120), TH->on, 255);
        wx_icon(594, 129, 115, wx_kind(wx_cn_code, wx_cn_phrase),
                wx_cn_daylight, TH->card);
        wx_fit(fit, sizeof fit, wx_cn_phrase, FACE(FS20), 170);
        textcenter(652, vbase(285, fit, FACE(FS20)), fit, FACE(FS20), TH->on, 255);
        snprintf(buf, sizeof buf, "体感 %.0f°", wx_cn_feel);
        text(330, 173, buf, FACE(FS20), TH->on2, 255);
        snprintf(buf, sizeof buf, "湿度 %.0f%%", wx_cn_hum);
        text(330, 223, buf, FACE(FS20), TH->on2, 255);
        snprintf(buf, sizeof buf, "风 %.0f km/h", wx_cn_wind);
        text(330, 273, buf, FACE(FS20), TH->on2, 255);
        if (wx_cn_alert[0]) {
            rrect(24, 342, 776, 440, 20, TH->card, TH->card_alpha, -1);
            text(50, 380, "生效预警", FACE(FS16), 0xE6984A, 255);
            wx_fit(fit, sizeof fit, wx_cn_alert, FACE(FS20), 680);
            text(50, 416, fit, FACE(FS20), TH->on, 255);
        } else {
            text(44, 385, "暂无生效预警", FACE(FS18), TH->on3, 255);
        }
    } else if (wx_page == 1) {
        static const char *dayname[5] = {"今天", "明天", "后天", "第4天", "第5天"};
        const int cardw = 144, gap = 8;
        for (int i = 0; i < 5; i++) {
            int x = MARGIN + i * (cardw + gap);
            rrect(x, 123, x + cardw, 416, 20, TH->card, TH->card_alpha, -1);
            textcenter(x + cardw / 2, vbase(158, dayname[i], FACE(FS20)),
                       dayname[i], FACE(FS20), TH->on2, 255);
            wx_icon(x + 40, 179, 64, wx_kind(wx_cn_day[i].code, wx_cn_day[i].phrase),
                    1, TH->card);
            snprintf(buf, sizeof buf, "%.0f°", wx_cn_day[i].high);
            textcenter(x + cardw / 2, vbase(294, buf, FACE(FS26)), buf,
                       FACE(FS26), TH->on, 255);
            snprintf(buf, sizeof buf, "%.0f°", wx_cn_day[i].low);
            textcenter(x + cardw / 2, vbase(337, buf, FACE(FS20)), buf,
                       FACE(FS20), TH->on3, 255);
            wx_fit(fit, sizeof fit, wx_cn_day[i].phrase, FACE(FS18), cardw - 20);
            textcenter(x + cardw / 2, vbase(383, fit, FACE(FS18)), fit,
                       FACE(FS18), TH->on2, 255);
        }
    } else {
        int px[12], py[12];
        double lo = wx_cn_hour[0].temp, hi = lo;
        for (int i = 1; i < 12; i++) {
            if (wx_cn_hour[i].temp < lo) lo = wx_cn_hour[i].temp;
            if (wx_cn_hour[i].temp > hi) hi = wx_cn_hour[i].temp;
        }
        if (hi - lo < 3) { hi += 1.5; lo -= 1.5; }
        for (int i = 0; i < 12; i++) {
            px[i] = 57 + i * 62;
            py[i] = 340 - (int)((wx_cn_hour[i].temp - lo) / (hi - lo) * 80.0);
            wx_icon(px[i] - 17, 133, 34, wx_kind(wx_cn_hour[i].code, NULL),
                    wx_cn_hour[i].daylight, TH->wall_top);
            snprintf(buf, sizeof buf, "%.0f°", wx_cn_hour[i].temp);
            textcenter(px[i], vbase(236, buf, FACE(FS16)), buf,
                       FACE(FS16), TH->on2, 255);
        }
        wx_line(43, 355, 753, 355, TH->on3, 1);
        for (int i = 1; i < 12; i++)
            wx_line(px[i - 1], py[i - 1], px[i], py[i], TH->accent, 3);
        for (int i = 0; i < 12; i++) {
            rrect(px[i] - 4, py[i] - 4, px[i] + 4, py[i] + 4,
                  4, TH->accent, 255, -1);
            int pop = wx_cn_hour[i].pop;
            if (pop < 0) pop = 0;
            if (pop > 100) pop = 100;
            if (pop > 0) rrect(px[i] - 9, 406 - pop * 34 / 100,
                               px[i] + 9, 406, 5, 0x609CE2, 255, -1);
            snprintf(buf, sizeof buf, ui_lang ? "%02d:00" : "%02d时", wx_cn_hour[i].hour);
            textcenter(px[i], vbase(435, buf, FACE(FS16)), buf,
                       FACE(FS16), TH->on2, 255);
        }
    }
    draw_app_header("天气");
}

static void draw_weather(void)
{
    const char *title = "天气 · 北京 (Open-Meteo)";
    int y = STATUS_H + 64;
    char buf[64], d1[24];
    const int refresh_x0 = 628, refresh_y0 = STATUS_H + 8;
    const int refresh_x1 = LW - MARGIN, refresh_y1 = STATUS_H + 48;
    const char *refresh = wx_state == 1 ? "获取中…" : wx_state == 3 ? "重试"
                          : now_ms() - wx_ok_ms < 3000 && wx_state == 2 ? "已更新" : "刷新";
    rrect(refresh_x0, refresh_y0, refresh_x1, refresh_y1, 20,
          TH->chip, 255, -1);
    textcenter((refresh_x0 + refresh_x1) / 2,
               vbase((refresh_y0 + refresh_y1) / 2, refresh, FACE(FS18)),
               refresh, FACE(FS18), TH->chip_ink, 255);

    if (apikey[0]) { wx_cn_draw(); wx_tabs(); return; }

    if (wx_cur_code < 0) {
        text(MARGIN, y + 10, wx_state == 1 ? "正在获取…" : wx_state == 3 ? "获取失败" : "尚未获取", FACE(FS26), TH->on, 255);
        text(MARGIN, y + 54, wifi_up ? "轻点刷新重试" : "需要先连上 WiFi", FACE(FS18), TH->on3, 255);
        draw_app_header(title);
        return;
    }

    /* current conditions */
    snprintf(buf, sizeof buf, "%.0f", wx_cur_t);
    int tw = textw(buf, FACE(FS120));
    text(MARGIN, y + 92, buf, FACE(FS120), TH->on, 255);
    snprintf(buf, sizeof buf, "°C");
    text(MARGIN + tw + 8, y + 60, buf, FACE(FS26), TH->on2, 255);

    wx_desc(wx_cur_code, d1, sizeof d1);
    text(MARGIN, y + 128, d1, FACE(FS26), TH->on, 255);

    snprintf(buf, sizeof buf, "体感 %.0f°  湿度 %.0f%%  风 %.0fkm/h", wx_feel, wx_hum, wx_wind);
    text(MARGIN, y + 168, buf, FACE(FS18), TH->on2, 255);

    /* Three compact forecast cards use the landscape width as three columns. */
    const char *dl[4] = {"今天", "明天", "后天", 0};
    const int gap = 12, card_y = 304, card_h = 138;
    const int card_w = (LW - 2 * MARGIN - 2 * gap) / 3;
    for (int i = 0; i < 3; i++) {
        int x = MARGIN + i * (card_w + gap);
        rrect(x, card_y, x + card_w, card_y + card_h, 20, TH->card, TH->card_alpha, -1);
        rrect(x, card_y, x + card_w, card_y + card_h, 20, TH->card_line,
              TH->is_light ? 14 : 22, 1.0f);
        text(x + 20, card_y + 34, dl[i], FACE(FS18), TH->on2, 255);
        wx_desc(wx_dcode[i], d1, sizeof d1);
        text(x + 20, card_y + 79, d1, FACE(FS26), TH->on, 255);
        snprintf(buf, sizeof buf, "%.0f° / %.0f°", wx_tmax[i], wx_tmin[i]);
        text(x + 20, card_y + 117, buf, FACE(FS20), TH->on2, 255);
    }
    draw_app_header(title);
}

/* --------------------------------------------------------------- settings */
static const char *FAM_NAME[3] = {"雅黑", "MI", "Rubik"};
static const char *WT_CJK[3] = {"细", "常规", "粗"};
static const char *WT_RUB[4] = {"细", "常规", "中等", "粗"};

static void font_label(int combo, char *out, int cap)
{
    int fam = combo >= 6 ? 2 : (combo >= 3 ? 1 : 0);
    int w = combo - FAMILY_BASE[fam];
    const char *weight = fam == 2 ? WT_RUB[w < 0 ? 0 : (w > 3 ? 3 : w)] :
                         WT_CJK[w < 0 ? 0 : (w > 2 ? 2 : w)];
    snprintf(out, cap, "%s · %s", ui_tr(FAM_NAME[fam]), ui_tr(weight));
}

#define SET_X0 24
#define SET_Y0 (STATUS_H + 52)
#define SET_H 54
#define SET_GAP 10

static void set_row_rect(int i, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = SET_X0;
    *x1 = LW - SET_X0;
    *y0 = SET_Y0 + i * (SET_H + SET_GAP);
    *y1 = *y0 + SET_H;
}

static void draw_set_row(int i, const char *label, const char *value)
{
    int x0, y0, x1, y1;
    set_row_rect(i, &x0, &y0, &x1, &y1);
    rrect(x0, y0, x1, y1, 16, TH->card, TH->card_alpha, -1);
    rrect(x0, y0, x1, y1, 16, TH->card_line, TH->is_light ? 14 : 22, 1.0f);
    int cy = (y0 + y1) / 2;
    text(x0 + 22, vbase(cy, label, FACE(FS20)), label, FACE(FS20), TH->on, 255);
    char fit[128];
    wx_fit(fit, sizeof fit, value, FACE(FS18), 320);
    int vw = textw(fit, FACE(FS18));
    text(x1 - 22 - vw, vbase(cy, fit, FACE(FS18)), fit, FACE(FS18), TH->on2, 255);
}

static void draw_settings(void)
{
    char buf[64];
    if (settings_page == 4) {
        int x0, y0, x1, y1;
        set_row_rect(0, &x0, &y0, &x1, &y1);
        draw_set_row(0, "UTC 时差", "");
        int cy = (y0 + y1) / 2;
        rrect(x1 - 286, y0 + 7, x1 - 238, y1 - 7, 12, TH->card_line, 70, -1);
        rrect(x1 - 70, y0 + 7, x1 - 22, y1 - 7, 12, TH->card_line, 70, -1);
        textcenter(x1 - 262, vbase(cy, "-", FACE(FS20)), "-", FACE(FS20), TH->on, 255);
        textcenter(x1 - 46, vbase(cy, "+", FACE(FS20)), "+", FACE(FS20), TH->on, 255);
        timezone_label(buf, sizeof buf);
        textcenter(x1 - 154, vbase(cy, buf, FACE(FS18)), buf, FACE(FS18), TH->on2, 255);
        draw_set_row(1, "网络校时", tsync_state == 1 ? "校时中…" :
                     tsync_no_network ? "请连接 Wi-Fi" :
                     tsync_state == 3 ? "校时失败 · 重试" :
                     tsync_state == 2 ? "已校时 · 重校" : "轻点校时");
        draw_set_row(2, "NTP", NTP_HOST ":123");
        draw_set_row(3, "备用时间源", HTTPS_TIME_URL);
        snprintf(buf, sizeof buf, "%d %s", sync_interval_hours < 24 ? sync_interval_hours :
                 sync_interval_hours / 24, sync_interval_hours < 24 ? "小时" : "天");
        draw_set_row(4, "自动间隔", buf);
        if (tsync_last_success) {
            struct tm saved;
            localtime_r(&tsync_last_success, &saved);
            snprintf(buf, sizeof buf, "%02d:%02d · %s", saved.tm_hour,
                     saved.tm_min, tsync_source);
        } else snprintf(buf, sizeof buf, "—");
        draw_set_row(5, "上次校时", buf);
        return;
    }
    if (settings_page == 1) {
        if (status_detail) { draw_status_detail(); return; }
        double secs = 0;
        FILE *f = fopen("/proc/uptime", "r");
        if (f) { (void)fscanf(f, "%lf", &secs); fclose(f); }
        draw_set_row(0, "型号", "KQJCY02QP");
        draw_set_row(1, "系统", "TinaLinux 3.4.39");
        snprintf(buf, sizeof buf, "%.0f 小时", secs / 3600.0);
        draw_set_row(2, "运行时间", buf);
        int bs = battery_ui_state();
        snprintf(buf, sizeof buf, "%d%% · %s", batt_pct,
                 bs == BAT_CHARGING ? "充电" : bs == BAT_FULL ? "已满" :
                 bs == BAT_EXTERNAL ? "USB 供电" :
                 bs == BAT_LOW ? (battery_voltage_low() ? "电压偏低" : "电量低") :
                 "电池供电");
        draw_set_row(3, "电源", buf);
        draw_set_row(4, "Wi-Fi", wifi_up ? "已连接" : "未连接");
        snprintf(buf, sizeof buf, "PM %s · TVOC %s", SENS[0].value, SENS[4].value);
        draw_set_row(5, "传感器", buf);
        return;
    }
    if (settings_page == 2) {
        snprintf(buf, sizeof buf, "%d / 100", bl_level);
        draw_set_row(0, "屏幕亮度", buf);
        snprintf(buf, sizeof buf, "%ld 秒", bl_idle_ms / 1000);
        draw_set_row(1, "闲置后调暗", buf);
        snprintf(buf, sizeof buf, "%s", bl_dim > 0 ? (bl_dim == 20 ? "低" : "中") : "关闭");
        draw_set_row(2, "调暗亮度", buf);
        snprintf(buf, sizeof buf, "%d 分钟", wx_refresh_min);
        draw_set_row(3, "天气刷新", buf);
        draw_set_row(4, "语言", ui_lang ? "English" : "简体中文");
        return;
    }
    if (settings_page == 3) {
        draw_set_row(0, "定位", city[0] ? "手动城市" : "自动 / IP");
        draw_set_row(1, "城市", city[0] ? city : "轻点输入");
        draw_set_row(2, "华风爱科 API Key", apikey[0] ? "已设置" : "未设置");
        snprintf(buf, sizeof buf, "%d 分钟", wx_refresh_min);
        draw_set_row(3, "刷新间隔", buf);
        draw_set_row(4, "数据源", apikey[0] ? "华风爱科" : "Open-Meteo");
        return;
    }
    if (settings_page == 5) {
        draw_set_row(0, "主题", theme_idx ? "浅色" : "深色");
        font_label(font_combo, buf, sizeof buf);
        draw_set_row(1, "字体", buf);
        return;
    }
    draw_set_row(0, "网络", wifi_up ? net_link : "未连接");
    draw_set_row(1, "主题 / 字体", theme_idx ? "浅色" : "深色");
    draw_set_row(2, "天气", city[0] ? city : "自动定位");
    draw_set_row(3, "设备状态", "传感器 / 电源");
    draw_set_row(4, "显示与刷新", "亮度 / 息屏");
    draw_set_row(5, "时间", "UTC / 网络校时");
}

static int settings_hit(int x, int y)
{
    for (int i = 0; i < (settings_page == 2 || settings_page == 3 ? 5 :
                         settings_page == 5 ? 2 : 6); i++) {
        int x0, y0, x1, y1;
        set_row_rect(i, &x0, &y0, &x1, &y1);
        if (x < x0 || x > x1 || y < y0 || y > y1) continue;
        if (settings_page == 1) {
            if (!status_detail && i >= 3 && i <= 5) status_detail = i - 2;
            dirty = 1;
            return 1;
        }
        if (settings_page == 2) {
            if (i == 0) { bl_level = bl_level == 40 ? 60 : bl_level == 60 ? 80 : bl_level == 80 ? 100 : 40; bl_write(bl_level); }
            else if (i == 1) bl_idle_ms = bl_idle_ms == 30000 ? 60000 : bl_idle_ms == 60000 ? 90000 : bl_idle_ms == 90000 ? 180000 : 30000;
            else if (i == 2) bl_dim = bl_dim == 0 ? 20 : bl_dim == 20 ? 40 : 0;
            else if (i == 3) wx_refresh_min = wx_refresh_min == 15 ? 30 : wx_refresh_min == 30 ? 60 : wx_refresh_min == 60 ? 120 : 15;
            else { ui_lang ^= 1; upd_time(); }
            config_save(); dirty = 1; return 1;
        }
        if (settings_page == 3) {
            if (i == 0) { city[0] = 0; config_save(); wx_state = 0; if (wifi_up) wx_spawn_fetch(); }
            else if (i == 1 || i == 2) {
                kb_target = i == 1 ? city : apikey;
                snprintf(kb_text, sizeof kb_text, "%s", kb_target);
                kb_len = (int)strlen(kb_text); kb_shift = 0; kb_open = 1;
            } else if (i == 3) {
                wx_refresh_min = wx_refresh_min == 15 ? 30 : wx_refresh_min == 30 ? 60 : wx_refresh_min == 60 ? 120 : 15;
                config_save();
            }
            dirty = 1; return 1;
        }
        if (settings_page == 4) {
            if (i == 0) {
                if (x >= x1 - 286 && x <= x1 - 238 && tz_offset_min > -720)
                    tz_offset_min -= 30;
                else if (x >= x1 - 70 && x <= x1 - 22 && tz_offset_min < 840)
                    tz_offset_min += 30;
                else return 1;
                apply_launcher_timezone();
                upd_time();
                config_save();
            } else if (i == 1) time_sync_spawn();
            else if (i == 4) {
                sync_interval_hours = sync_interval_hours == 3 ? 12 :
                                      sync_interval_hours == 12 ? 24 :
                                      sync_interval_hours == 24 ? 72 :
                                      sync_interval_hours == 72 ? 168 : 3;
                config_save();
            }
            dirty = 1; return 1;
        }
        if (settings_page == 5) {
            if (i == 0) {
                theme_idx ^= 1;
                TH = theme_idx ? &TH_LIGHT : &TH_DARK;
                if (!font_user) font_combo = FAMILY_BASE[theme_idx ? 1 : 0] + 1;
            } else {
                font_combo = (font_combo + 1) % 10;
                font_user = 1;
            }
            theme_save(); dirty = 1; return 1;
        }
        if (i == 0) {
            net_sub = 1;
            if (net_state == 0) net_scan();
        } else settings_page = i == 1 ? 5 : i == 2 ? 3 :
                               i == 3 ? 1 : i == 4 ? 2 : 4;
        dirty = 1;
        return 1;
    }
    return 0;
}

#include "file_app.h"
#include "organizer_app.h"
#include "history_ui.h"

static void draw_app(void)
{
    const char *title = app_open >= 0 && app_open < N_APP_IDS ? APP_NAMES[app_open] : "";

    if (app_open == 0) {
        if (clock_style == 0) draw_clock_big();
        else draw_clock_sun();
    } else if (app_open == 1) {
        draw_weather();
        return;
    } else if (app_open == 2) {
        if (net_sub) { draw_network(); title = "网络"; }
        else {
            draw_settings();
            title = settings_page == 1 ?
                    (status_detail == 1 ? "电源" : status_detail == 2 ? "网络状态" :
                     status_detail == 3 ? "传感器状态" : "设备状态") :
                    settings_page == 2 ? "显示与刷新" :
                    settings_page == 3 ? "天气设置" :
                    settings_page == 4 ? "时间" :
                    settings_page == 5 ? "主题 / 字体" : "设置";
        }
    } else if (app_open == 6) {
        file_draw();
    } else if (app_open == 4) {
        org_draw_calendar();
    } else if (app_open == 5) {
        org_draw_notes();
    } else {
        int cx = LW / 2, cy = 250;
        if (app_open >= 0 && app_open < N_APP_IDS)
            sprite_rgba(&icons[app_open], cx - ICON / 2, cy - ICON, 255);
        const char *msg = "开发中";
        int tw = textw(msg, FACE(FS20));
        text(cx - tw / 2, cy + 60, msg, FACE(FS20), TH->on2, 255);
    }
    draw_app_header(title);
    if (app_open == 2 && !net_sub && config_state) {
        const char *msg = config_state == 1 ? "保存中…" : config_state == 2 ? "已保存" : "保存失败";
        text(LW - MARGIN - textw(msg, FACE(FS16)),
             vbase(STATUS_H + 26, msg, FACE(FS16)), msg, FACE(FS16),
             config_state == 3 ? 0xE5484D : TH->on3, 255);
    }
}

static void draw_pressed(void)
{
    if (!press_active) return;
    rrect(press_x - 20, press_y - 20, press_x + 20, press_y + 20,
          20, TH->accent, 82, -1);
}

static void render(void)
{
    sky_mode = 0;
    wallpaper();
    if (history_open) {
        hist_draw();
        draw_status();
        draw_low_voltage_notice();
        draw_pressed();
        dump_land();
        present();
        land_valid = 1;
        dirty = 0;
        return;
    }
    if (app_open >= 0) {
        draw_app();
        draw_status();
        draw_low_voltage_notice();
        draw_menu();
        draw_detail();
        draw_info();
        draw_keyboard();
        clk_opt_draw();
        draw_pressed();
        dump_land();
        present();
        land_valid = 1;
        dirty = 0;
        return;
    }
    draw_status();
    if (page == 0) {
        draw_sensors();
        draw_dots(0, 2, DOTS_Y_SENSORS);
        draw_detail();
    } else {
        draw_icons();
        draw_dots(1, 2, DOTS_Y_ICONS);
    }
    draw_low_voltage_notice();
    draw_menu();
    draw_chooser();
    draw_info();
    draw_keyboard();
    clk_opt_draw();
    draw_pressed();
    dump_land();
    present();
    land_valid = 1;
    dirty = 0;
}

/* ------------------------------------------------------------------- time */
static void upd_time(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(clock_s, sizeof clock_s, "%02d:%02d", tm.tm_hour, tm.tm_min);
    const char *wd[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    if (ui_lang) {
        static const char *en_wd[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        snprintf(date_s, sizeof date_s, "%02d/%02d %s", tm.tm_mon + 1, tm.tm_mday, en_wd[tm.tm_wday]);
    } else snprintf(date_s, sizeof date_s, "%d月%d日 %s", tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday]);
}

/* ---------------------------------------------------------------- sensors */
static int read_int_file(const char *p, int *out)
{
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    if (v >= 0) *out = v;
    return v;
}

static uint8_t crc8(const uint8_t *d, int n)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

static int i2c_claim(int fd, int addr)
{
    if (ioctl(fd, I2C_SLAVE, addr) == 0) return 0;
    int e1 = errno;
    if (ioctl(fd, I2C_SLAVE_FORCE, addr) == 0) return 0;
    printf("i2c claim 0x%02x: I2C_SLAVE errno=%d, FORCE errno=%d\n", addr, e1, errno);
    return -1;
}

/* SHTC3 / SHT3x style: 2-byte cmd, 6-byte reply with crc per word */
static int i2c_probe_temp_humi(int bus, int addr, double *t_c, double *rh)
{
    char path[32];
    snprintf(path, sizeof path, "/dev/i2c-%d", bus);
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    if (i2c_claim(fd, addr) < 0) { close(fd); return -2; }
    uint8_t cmd[2] = {0x7C, 0xA2};                 /* SHTC3 measure T first, clock stretching */
    struct i2c_msg msgs[2];
    uint8_t buf[8] = {0};
    msgs[0].addr = addr; msgs[0].flags = 0; msgs[0].len = 2; msgs[0].buf = cmd;
    msgs[1].addr = addr; msgs[1].flags = I2C_M_RD; msgs[1].len = 6; msgs[1].buf = buf;
    ssize_t n = 0;
    struct i2c_rdwr_ioctl_data io = {.msgs = msgs, .nmsgs = 2};
    for (int retry = 0; retry < 3 && n <= 0; retry++) {
        n = ioctl(fd, I2C_RDWR, &io);
        if (n <= 0) usleep(20000);
    }
    close(fd);
    if (n <= 0) return -3;
    if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) return -4;
    uint16_t tr = (uint16_t)(buf[0] << 8 | buf[1]);
    uint16_t hr = (uint16_t)(buf[3] << 8 | buf[4]);
    *t_c = -45.0 + 175.0 * tr / 65536.0;
    *rh = 100.0 * hr / 65536.0;
    return 0;
}

/* BH1721FVC on i2c-1 addr 0x23: one time H-res measurement -> 2 bytes */
static int i2c_probe_light(int bus, int addr, double *lux)
{
    char path[32];
    snprintf(path, sizeof path, "/dev/i2c-%d", bus);
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    if (i2c_claim(fd, addr) < 0) { close(fd); return -2; }
    uint8_t cmd = 0x20;
    if (write(fd, &cmd, 1) != 1) { close(fd); return -3; }
    usleep(180000);
    uint8_t b[2] = {0};
    if (read(fd, b, 2) != 2) { close(fd); return -4; }
    close(fd);
    int raw = b[0] << 8 | b[1];
    *lux = raw / 1.2;
    return 0;
}

/* SGP30 style gas sensor on i2c-2 addr 0x58 (TVOC / eCO2) */
static int i2c_probe_gas(int bus, int addr, int *co2eq, int *tvoc)
{
    char path[32];
    snprintf(path, sizeof path, "/dev/i2c-%d", bus);
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    if (i2c_claim(fd, addr) < 0) { close(fd); return -2; }
    uint8_t cmd[2] = {0x20, 0x08};
    if (write(fd, cmd, 2) != 2) { close(fd); return -3; }
    usleep(25000);
    uint8_t b[6] = {0};
    if (read(fd, b, 6) != 6) { close(fd); return -4; }
    close(fd);
    if (crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) return -5;
    *co2eq = b[0] << 8 | b[1];
    *tvoc = b[3] << 8 | b[4];
    return 0;
}

/* The factory UI receives unsolicited 60-byte PM frames on /dev/ttyS3 about
   every two seconds. Sending an 8-byte request makes the module reply with
   AT+error, so consume the stream without writing or flushing the input. */
static int pm25_ugm3(double *out)
{
    static double cal = 0;
    if (cal <= 0) {
        cal = 1.30691;
        FILE *f = fopen("/usr/bin/data/etc/pm25_calibrate", "r");
        if (f) {
            double v = 0;
            if (fscanf(f, "%lf", &v) == 1 && v > 0.5 && v < 3.0) cal = v;
            fclose(f);
        }
    }
    int fd = open("/dev/ttyS3", O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    /* raw mode: the vendor app may leave the line in an odd state, and we must
       not wait for a newline that never comes */
    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &tio);
    }
    uint8_t buf[240];
    int n = 0, done = 0;
    for (int wait = 0; wait < 40 && !done; wait++) {
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, 100) <= 0) continue;
        ssize_t r = read(fd, buf + n, sizeof buf - n);
        if (r <= 0) continue;
        n += (int)r;
        for (int i = 0; i + 59 < n; i++)
            if (buf[i] == 0x5A && buf[i + 1] == 0xA5 && buf[i + 2] == 0x34
                && buf[i + 3] == 0x00) { done = 1; break; }
        if (n >= (int)sizeof buf - 60 && !done) {
            memmove(buf, buf + n - 60, 60);
            n = 60;
        }
    }
    close(fd);
    for (int i = 0; i + 59 < n; i++) {
        if (buf[i] != 0x5A || buf[i + 1] != 0xA5) continue;
        if (buf[i + 2] != 0x34 || buf[i + 3] != 0x00) continue;
        uint32_t w1 = (uint32_t)buf[i + 8] | ((uint32_t)buf[i + 9] << 8)
                    | ((uint32_t)buf[i + 10] << 16) | ((uint32_t)buf[i + 11] << 24);
        if (w1 < 1000000u) {
            *out = (double)w1 / 100.0 * cal;
            return 0;
        }
    }
    return -3;
}

static void set_tag(struct card *c, const char *tag, uint32_t col)
{
    snprintf(c->tag, sizeof c->tag, "%s", tag);
    c->color = col;
}

/* saturation vapour pressure in hPa (Magnus) */
static double es_hpa(double t)
{
    return 6.112 * exp(17.62 * t / (243.12 + t));
}

/* The SHTC3 reads board heat. Two adjacent BranOS comparisons (2026-11-03
   and 2026-11-04) suggest a junction-dependent self-heating offset. This is
   empirical agreement with the vendor UI, not a reference calibration. */
#define TH_COMP_OFFSET 9.1
#define TH_COMP_GAIN 0.263

static double raw_tc, raw_rh, comp_tc, comp_rh;

/* SGP30 wants the absolute humidity it is breathing so it can compensate the
   moisture effect on the metal-oxide layer (mg/m3). */
static int sysfs_write(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = strlen(val);
    ssize_t w = write(fd, val, n);
    close(fd);
    return w == (ssize_t)n ? 0 : -1;
}

static int gas_read_baseline(char *out, size_t n)
{
    FILE *f = fopen(SGP_DIR "/iaq_baseline", "r");
    if (!f) return 0;
    if (!fgets(out, (int)n, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    size_t l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = 0;
    return l > 0;
}

/* A hung writer must never block the UI, so every persistence goes through a
   short-lived child process: if it stalls in D state it is simply abandoned. */
static void persist_spawn(const char *path, const char *text)
{
    pid_t p = fork();
    if (p != 0) return;
    mkdir(BASELINE_DIR, 0755);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        size_t n = strlen(text);
        if (write(fd, text, n) == (ssize_t)n) fsync(fd);
        close(fd);
    }
    _exit(0);
}

static void gas_finish_cal(void)
{
    char base[64];
    if (!gas_read_baseline(base, sizeof base) || strlen(base) != 8 ||
        strspn(base, "0123456789abcdefABCDEF") != 8) {
        cal_state = CAL_FAIL;
        snprintf(cal_reason, sizeof cal_reason, "基线值无效");
        persist_spawn("/tmp/cal_breadcrumb.txt", "failed\n");
        dirty = 1;
        return;
    }
    if (!config_mount_ready()) {
        cal_state = CAL_FAIL;
        snprintf(cal_reason, sizeof cal_reason, "UDISK 未挂载");
        dirty = 1;
        return;
    }
    snprintf(cal_pending_base, sizeof cal_pending_base, "%s", base);
    unlink("/tmp/cal_save_result.txt");
    cal_state = CAL_SAVE;
    cal_save_started = now_ms();
    pid_t pid = fork();
    if (pid == 0) {
        int ok = 0;
        char payload[96], tmp[128], check[64] = {0};
        long when = time(NULL);
        snprintf(payload, sizeof payload, "%s\n%ld\n", base, when);
        mkdir(BASELINE_DIR, 0700);
        snprintf(tmp, sizeof tmp, BASELINE_DIR "/baseline.%ld.tmp", (long)getpid());
        int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            size_t n = strlen(payload);
            ok = write(fd, payload, n) == (ssize_t)n && fsync(fd) == 0;
            close(fd);
            if (ok) ok = rename(tmp, BASELINE_FILE) == 0;
            if (!ok) unlink(tmp);
        }
        if (ok) {
            FILE *rf = fopen(BASELINE_FILE, "r");
            if (rf) { ok = fscanf(rf, "%63s", check) == 1 && !strcmp(check, base); fclose(rf); }
            else ok = 0;
        }
        if (ok) {
            int tf = open(BASELINE_TMP, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (tf >= 0) { (void)write(tf, payload, strlen(payload)); close(tf); }
        }
        int res = open("/tmp/cal_save_result.txt", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (res >= 0) { (void)write(res, ok ? "ok\n" : "failed\n", ok ? 3 : 7); close(res); }
        _exit(ok ? 0 : 1);
    }
    if (pid < 0) { cal_state = CAL_FAIL; snprintf(cal_reason, sizeof cal_reason, "无法启动保存"); }
    dirty = 1;
}

static void gas_restore_baseline(void)
{
    const char *paths[2] = { BASELINE_FILE, BASELINE_TMP };
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        char base[64] = {0};
        long when = 0;
        int got = fscanf(f, "%63s %ld", base, &when);
        fclose(f);
        if (got < 1 || strlen(base) != 8 || strspn(base, "0123456789abcdefABCDEF") != 8) continue;
        char v[80];
        snprintf(v, sizeof v, "%s\n", base);
        if (sysfs_write(SGP_DIR "/iaq_baseline", v)) {
            printf("gas baseline restore failed from %s\n", paths[i]);
            fflush(stdout);
            continue;
        }
        char verify[64] = {0};
        if (!gas_read_baseline(verify, sizeof verify) || strcmp(verify, base)) {
            printf("gas baseline restore readback mismatch from %s (requested=%s readback=%s)\n",
                   paths[i], base, verify);
            fflush(stdout);
            continue;
        }
        if (got >= 2) cal_done_at = when;
        cal_state = CAL_OK;
        printf("gas baseline restored from %s (requested=%s readback=%s)\n", paths[i], base, verify);
        fflush(stdout);
        return;
    }
}

/* twenty quiet minutes average the baseline of the room */
static void gas_cal_tick(long now)
{
    if (cal_state == CAL_RUN && now - cal_start_ms >= GAS_CAL_MS) gas_finish_cal();
    if (cal_state != CAL_SAVE) return;
    FILE *f = fopen("/tmp/cal_save_result.txt", "r");
    if (f) {
        char result[16] = {0};
        (void)fgets(result, sizeof result, f); fclose(f);
        unlink("/tmp/cal_save_result.txt");
        if (!strncmp(result, "ok", 2)) {
            cal_done_at = time(NULL); cal_state = CAL_OK; cal_reason[0] = 0;
            persist_spawn("/tmp/cal_breadcrumb.txt", "done\n");
            printf("gas baseline saved and verified: %s\n", cal_pending_base);
        } else {
            cal_state = CAL_FAIL;
            snprintf(cal_reason, sizeof cal_reason, "基线写入失败");
            persist_spawn("/tmp/cal_breadcrumb.txt", "failed\n");
        }
        fflush(stdout); dirty = 1;
    } else if (now - cal_save_started > 15000) {
        cal_state = CAL_FAIL; snprintf(cal_reason, sizeof cal_reason, "基线保存超时");
        dirty = 1;
    }
}

/* TVOC in ppb.
   The vendor driver's iaq_measure is authoritative: it parses the SGP30 reply
   itself and reports tvoc / co2_eq separately. Word order in the raw reply is
   (co2_eq, tvoc) on this part, so falling back to raw must use word1. */
static double gas_tvoc(void)
{
    double tv = -1;
    FILE *gf = fopen("/sys/bus/i2c/devices/2-0058/iaq_measure", "r");
    if (gf) {
        char k1[32] = {0}, k2[32] = {0};
        double ce = 0;
        if (fscanf(gf, "%31s %lf %31s %lf", k1, &tv, k2, &ce) == 4 && tv >= 0) {
            fclose(gf);
            return tv;
        }
        fclose(gf);
    }
    int w0 = -1, w1 = -1;
    if (i2c_probe_gas(2, 0x58, &w0, &w1) == 0 && w1 >= 0) return (double)w1;
    return -1;
}

/* the SGP30 expects to be read once per second; sampling it more slowly makes
   the driver hand back its power-on defaults (tvoc 0 / co2 400) */
static int gas_ok;
static void sense_gas(void)
{
    double tv = gas_tvoc();
    if (tv >= 0) {
        snprintf(SENS[4].value, sizeof SENS[4].value, "%.0f", tv);
        set_tag(&SENS[4], tv >= 60000 ? "超量程" : tv > 1000 ? "偏高" : "良好",
                tv > 1000 ? 0xDE6E6E : 0x3FB98A);
        gas_ok = 1;
    } else if (!gas_ok) {
        snprintf(SENS[4].value, sizeof SENS[4].value, "-");
        set_tag(&SENS[4], "无响应", 0x78808C);
    }
}
/* China HJ 633-2012 breakpoints for PM2.5 (24 h average) */
static int pm25_to_aqi(double ug)
{
    static const double bp[6] = {35, 75, 115, 150, 250, 350};
    static const int iaqi[6] = {50, 100, 150, 200, 300, 400};
    if (ug <= 0) return 0;
    double lo_c = 0, lo_i = 0;
    for (int i = 0; i < 6; i++) {
        if (ug <= bp[i]) {
            return (int)((iaqi[i] - lo_i) / (bp[i] - lo_c) * (ug - lo_c) + lo_i + 0.5);
        }
        lo_c = bp[i];
        lo_i = iaqi[i];
    }
    return 500;
}

static const char *aqi_level(int aqi, uint32_t *col)
{
    if (aqi <= 50) { *col = 0x3FB98A; return "优"; }
    if (aqi <= 100) { *col = 0xE6B84A; return "良"; }
    if (aqi <= 150) { *col = 0xE6984A; return "轻度污染"; }
    if (aqi <= 200) { *col = 0xDE6E6E; return "中度污染"; }
    if (aqi <= 300) { *col = 0xB45CC8; return "重度污染"; }
    *col = 0x8C4A4A;
    return "严重污染";
}

static int pm_async;

static long aqi_window_started;
static double aqi_window_sum;
static int aqi_window_count, aqi_has_value;

static void aqi_publish(double pm)
{
    if (pm < 0) {
        snprintf(SENS[3].value, sizeof SENS[3].value, "-");
        set_tag(&SENS[3], "无响应", 0x78808C);
        aqi_has_value = 0;
    } else {
        int aqi = pm25_to_aqi(pm);
        uint32_t acol;
        const char *lvl = aqi_level(aqi, &acol);
        snprintf(SENS[3].value, sizeof SENS[3].value, "%d", aqi);
        set_tag(&SENS[3], lvl, acol);
        aqi_has_value = 1;
    }
}

static void pm_apply(double pm)
{
    long now = now_ms();
    if (pm >= 0 && pm < 1000) {
        hist_sample(0, pm);
        snprintf(SENS[0].value, sizeof SENS[0].value, "%.1f", pm);
        set_tag(&SENS[0], pm > 75 ? "偏高" : pm > 35 ? "偏中" : "良好", 0x3FB98A);
        if (!aqi_has_value) aqi_publish(pm);
        aqi_window_sum += pm;
        aqi_window_count++;
    } else {
        snprintf(SENS[0].value, sizeof SENS[0].value, "-");
        set_tag(&SENS[0], "无响应", 0x78808C);
    }
    if (!aqi_window_started) aqi_window_started = now;
    if (now - aqi_window_started >= 60000) {
        aqi_publish(aqi_window_count ? aqi_window_sum / aqi_window_count : -1);
        aqi_window_sum = 0;
        aqi_window_count = 0;
        aqi_window_started = now;
    }
    if ((app_open < 0 && page == 0) || detail_open ||
        (app_open == 2 && settings_page == 1 && status_detail == 3)) dirty = 1;
}

static int pm_pipe = -1;
static pid_t pm_pid;
static long pm_started;

static void pm_start(long now)
{
    if (pm_pid || pm_pipe >= 0) return;
    int fds[2];
    if (pipe(fds)) return;
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        double pm = -1;
        if (pm25_ugm3(&pm) || pm < 0 || pm >= 1000) pm = -1;
        (void)write(fds[1], &pm, sizeof pm);
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); return; }
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    pm_pipe = fds[0];
    pm_pid = pid;
    pm_started = now;
}

static void pm_poll(long now)
{
    if (pm_pipe < 0) return;
    double pm;
    ssize_t n = read(pm_pipe, &pm, sizeof pm);
    if (n == (ssize_t)sizeof pm) {
        pm_apply(pm);
        close(pm_pipe); pm_pipe = -1; pm_pid = 0;
    } else if (n == 0 || now - pm_started > 10000) {
        if (pm_pid) kill(pm_pid, SIGKILL);
        pm_apply(-1);
        close(pm_pipe); pm_pipe = -1; pm_pid = 0;
    }
}

static void sense_all(void)
{
    /* battery */
    int cap;
    char st[32] = {0};
    FILE *sf = fopen("/sys/class/power_supply/battery/status", "r");
    if (sf) {
        if (fscanf(sf, "%31s", st) != 1) st[0] = 0;
        fclose(sf);
    }
    batt_charging = strstr(st, "Charg") != NULL;
    batt_full = strstr(st, "Full") != NULL;
    batt_discharging = strstr(st, "Discharg") != NULL;
    batt_not_charging = !strcmp(st, "Not");
    batt_usb_online = -1;
    batt_ac_online = -1;
    batt_current_ua = -1;
    batt_voltage_uv = -1;
    (void)read_int_file("/sys/class/power_supply/usb/online", &batt_usb_online);
    (void)read_int_file("/sys/class/power_supply/ac/online", &batt_ac_online);
    if (battery_external_online())
        (void)read_int_file("/sys/class/power_supply/battery/current_now", &batt_current_ua);
    if (battery_external_online() && batt_current_ua >= 0 && batt_current_ua <= 10000) {
        if (batt_external_hits < 3) batt_external_hits++;
    } else if (!battery_external_online() || batt_current_ua > 20000) {
        batt_external_hits = 0;
    }
    if (!battery_external_online() &&
        read_int_file("/sys/class/power_supply/battery/voltage_now", &batt_voltage_uv) >= 0) {
        /* Warn before the pack reaches the board's 3.30 V design minimum.
           A prior battery run fell from 3.57 V to 3.14 V in ~4 minutes while
           the fuel gauge still reported 78%, so capacity alone is unsafe. */
        if (batt_voltage_uv < 3700000 && batt_low_voltage_hits < 2)
            batt_low_voltage_hits++;
        else if (batt_voltage_uv > 3850000)
            batt_low_voltage_hits = 0;
    } else batt_low_voltage_hits = 0;
    wifi_up = 0;
    FILE *wf = fopen("/sys/class/net/wlan0/operstate", "r");
    int link_up = 0;
    if (wf) {
        char ws[16] = {0};
        if (fscanf(wf, "%15s", ws) == 1 && !strcmp(ws, "up")) link_up = 1;
        fclose(wf);
    }
    /* The settings row and status icon must use the same live connection
       snapshot. A link alone can be up while DHCP has not completed. */
    net_link[0] = 0;
    if (link_up) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof ifr);
            snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "wlan0");
            if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
                char ip[INET_ADDRSTRLEN] = {0};
                struct sockaddr_in *sa = (struct sockaddr_in *)&ifr.ifr_addr;
                if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip) &&
                    strcmp(ip, "0.0.0.0")) {
                    wifi_up = 1;
                    struct iwreq wrq;
                    char ssid[40] = {0};
                    memset(&wrq, 0, sizeof wrq);
                    snprintf(wrq.ifr_name, sizeof wrq.ifr_name, "wlan0");
                    wrq.u.essid.pointer = ssid;
                    wrq.u.essid.length = sizeof ssid - 1;
                    if (ioctl(sock, SIOCGIWESSID, &wrq) == 0 && ssid[0])
                        snprintf(net_link, sizeof net_link, "%s %s", ssid, ip);
                    else
                        snprintf(net_link, sizeof net_link, "已连接 %s", ip);
                }
            }
            close(sock);
        }
    }
    if (read_int_file("/sys/class/power_supply/battery/capacity", &cap) >= 0) {
        batt_pct = cap;
        snprintf(batt_s, sizeof batt_s, "%d%%", cap);
    }
    /* environment: prefer the vendor kernel drivers' calibrated sysfs values
       (read-only files), fall back to raw i2c transactions when one is absent */
    double tc = 0, rh = 0;
    int got_th = 0;
    FILE *tf = fopen("/sys/bus/i2c/devices/2-0070/shtc3_temper_humi", "r");
    if (tf) {
        int t100 = -1000, h100 = -1000;
        if (fscanf(tf, "temperature=%d,humidity=%d", &t100, &h100) == 2 && t100 > -4000 && h100 >= 0) {
            tc = t100 / 100.0;
            rh = h100 / 100.0;
            got_th = 1;
        }
        fclose(tf);
    }
    if (!got_th && i2c_probe_temp_humi(2, 0x70, &tc, &rh) == 0
        && tc > -40 && tc < 85 && rh >= 0 && rh <= 100)
        got_th = 1;
    if (got_th) {
        raw_tc = tc;
        raw_rh = rh;

        double tj = 0;
        int got_tj = 0;
        FILE *jf = fopen("/sys/bus/i2c/devices/2-0058/temp_tjunction", "r");
        if (jf) {
            if (fscanf(jf, "%lf", &tj) == 1) got_tj = 1;
            fclose(jf);
        }
        if (!got_tj) {
            FILE *sf = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
            if (sf) {
                int milli = 0;
                if (fscanf(sf, "%d", &milli) == 1) {
                    tj = milli / 1000.0;
                    got_tj = 1;
                }
                fclose(sf);
            }
        }
        double delta = got_tj ? TH_COMP_OFFSET + TH_COMP_GAIN * (tj - 25.0) : 17.5;
        if (delta < 0) delta = 0;
        if (delta > 22) delta = 22;
        double t_amb = tc - delta;
        double vapour = raw_rh / 100.0 * es_hpa(raw_tc);
        /* Preserve absolute humidity: vapour pressure also changes with the
           Kelvin temperature in the ideal-gas conversion. */
        double rh_amb = vapour / es_hpa(t_amb) * 100.0 *
                        (t_amb + 273.15) / (raw_tc + 273.15);
        if (rh_amb > 99) rh_amb = 99;
        if (rh_amb < 1) rh_amb = 1;
        comp_tc = t_amb;
        comp_rh = rh_amb;
        hist_sample(1, t_amb);
        hist_sample(2, rh_amb);

        snprintf(SENS[1].value, sizeof SENS[1].value, "%.1f", t_amb);
        snprintf(SENS[2].value, sizeof SENS[2].value, "%.1f", rh_amb);
        set_tag(&SENS[1], t_amb < 18 ? "偏凉" : t_amb > 28 ? "偏热" : "舒适", 0x609CE2);
        set_tag(&SENS[2], rh_amb < 35 ? "偏干" : rh_amb > 70 ? "偏湿" : "舒适", 0x54B0D6);
    } else {
        snprintf(SENS[1].value, sizeof SENS[1].value, "-");
        snprintf(SENS[2].value, sizeof SENS[2].value, "-");
        set_tag(&SENS[1], "无响应", 0x78808C);
        set_tag(&SENS[2], "无响应", 0x78808C);
    }

    double lux = 0;
    int got_lux = 0, li = 0;
    if (read_int_file("/sys/bus/i2c/devices/1-0023/get_light_val", &li) >= 0) {
        lux = li;
        got_lux = 1;
    } else if (i2c_probe_light(1, 0x23, &lux) == 0) {
        got_lux = 1;
    }
    if (got_lux && lux >= 0 && lux < 200000) {
        hist_sample(4, lux);
        snprintf(SENS[5].value, sizeof SENS[5].value, "%.0f", lux);
        set_tag(&SENS[5], lux < 10 ? "暗" : lux > 300 ? "明亮" : "适中", 0xD6BC4E);
    } else {
        snprintf(SENS[5].value, sizeof SENS[5].value, "-");
        set_tag(&SENS[5], "无响应", 0x78808C);
    }

    double tv = gas_tvoc();
    if (tv >= 0) {
        hist_sample(3, tv);
        snprintf(SENS[4].value, sizeof SENS[4].value, "%.0f", tv);
        set_tag(&SENS[4], tv >= 60000 ? "超量程" : tv > 1000 ? "偏高" : "良好",
                tv > 1000 ? 0xDE6E6E : 0x3FB98A);
    } else {
        snprintf(SENS[4].value, sizeof SENS[4].value, "-");
        set_tag(&SENS[4], "无响应", 0x78808C);
    }

    /* PM2.5: the particulate module answers on /dev/ttyS3 */
    if (!pm_async) {
        double pm = -1;
        if (pm25_ugm3(&pm)) pm = -1;
        pm_apply(pm);
    }
}


static void i2c_spy(void)
{
    struct { const char *name; int bus, addr; uint8_t c[3]; int n; int delay_ms; int readn; } tests[] = {
        {"wake",        2, 0x70, {0x35, 0x17}, 2, 1,  0},
        {"T-stretch",   2, 0x70, {0x7C, 0xA2}, 2, 0,  6},
        {"T-nostretch", 2, 0x70, {0x78, 0x66}, 2, 15, 6},
        {"RH-stretch",  2, 0x70, {0x5C, 0x24}, 2, 0,  6},
        {"RH-nostretch",2, 0x70, {0x58, 0xE0}, 2, 15, 6},
        {"SHT30-2C06",  2, 0x70, {0x2C, 0x06}, 2, 0,  6},
        {"SHT30-2400",  2, 0x70, {0x24, 0x00}, 2, 20, 6},
        {"BH1721-0x20", 1, 0x23, {0x20},       1, 180, 2},
        {"BH1721-0x10", 1, 0x23, {0x10},       1, 180, 2},
        {"BH1750-0x10", 1, 0x23, {0x10},       1, 180, 2},
        {"SGP30-2008",  2, 0x58, {0x20, 0x08}, 2, 25, 6},
        {"SGP30-2003",  2, 0x58, {0x20, 0x03}, 2, 20, 3},
        {"SGP10-0x00",  2, 0x58, {0x00},       1, 20, 9},
    };
    int n = (int)(sizeof tests / sizeof tests[0]);
    for (int k = 0; k < n; k++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/i2c-%d", tests[k].bus);
        int fd = open(path, O_RDWR);
        if (fd < 0) { printf("%-14s open errno %d\n", tests[k].name, errno); continue; }
        if (i2c_claim(fd, tests[k].addr) < 0) { close(fd); continue; }
        if (tests[k].c[0] == 0x20 && tests[k].n == 2 && tests[k].addr == 0x58) { }
        if (write(fd, tests[k].c, tests[k].n) != tests[k].n) {
            printf("%-14s write errno %d\n", tests[k].name, errno);
            close(fd);
            continue;
        }
        if (tests[k].delay_ms) usleep(tests[k].delay_ms * 1000);
        uint8_t b[16] = {0};
        ssize_t r = read(fd, b, tests[k].readn);
        close(fd);
        printf("%-14s r=%d bytes=", tests[k].name, (int)r);
        for (int i = 0; i < tests[k].readn; i++) printf("%02X ", b[i]);
        if (tests[k].readn >= 6) {
            unsigned w0 = b[0] << 8 | b[1], w1 = b[3] << 8 | b[4];
            printf("| w0=%u T=%.2f RH=%.2f | w1=%u T=%.2f RH=%.2f | crc0 %s crc1 %s",
                   w0, -45.0 + 175.0 * w0 / 65536.0, 100.0 * w0 / 65536.0,
                   w1, -45.0 + 175.0 * w1 / 65536.0, 100.0 * w1 / 65536.0,
                   crc8(b, 2) == b[2] ? "ok" : "BAD", crc8(b + 3, 2) == b[5] ? "ok" : "BAD");
        }
        printf("\n");
        fflush(stdout);
    }
}

/* ------------------------------------------------------------------ input */
static int evfd = -1;
static long ev_seen, tap_seen;
static int touch_down, touch_x, touch_y, touch_x0, touch_y0;
static long touch_t0;
static int long_fired;
static int swiped;
static int touch_moved;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int input_open(void)
{
    const char *paths[] = {"/dev/input/event1", "/dev/input/event0", "/dev/input/event2"};
    for (int i = 0; i < 3; i++) {
        int fd = open(paths[i], O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        char name[64] = {0};
        ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
        printf("input %s -> %s\n", paths[i], name);
        if (strstr(name, "fts")) { evfd = fd; return 0; }
        close(fd);
    }
    return -1;
}

/* panel coords -> landscape */
static void input_reopen(void)
{
    if (evfd >= 0) close(evfd);
    evfd = -1;
    input_open();
}

static void panel_to_land(int px, int py, int *vx, int *vy)
{
    *vx = py;
    *vy = (LH - 1) - px;
}


static void on_tap(int x, int y)
{
    tap_seen++;
    printf("tap %d %d (page %d app %d kb %d)\n", x, y, page, app_open, kb_open);
    fflush(stdout);
    if (kb_open) { kb_hit(x, y); return; }
    if (clk_opt_hit(x, y)) return;
    if (history_open) { hist_hit(x, y); return; }
    if (detail_open) {
        int hx0, hy0, hx1, hy1;
        detail_history_btn(&hx0, &hy0, &hx1, &hy1);
        if (x >= hx0 && x <= hx1 && y >= hy0 && y <= hy1) {
            hist_open_card(detail_idx);
            return;
        }
        if (detail_idx == 4) {
            int bx0, by0, bx1, by1;
            detail_btn(&bx0, &by0, &bx1, &by1);
            if (x >= bx0 && x <= bx1 && y >= by0 && y <= by1) {
                if (cal_state != CAL_RUN && cal_state != CAL_SAVE) {
                    cal_state = CAL_RUN;
                    cal_start_ms = now_ms();
                    cal_reason[0] = 0;
                    persist_spawn("/tmp/cal_breadcrumb.txt", "started\n");
                    dirty = 1;
                }
                return;
            }
        }
        detail_open = 0;
        dirty = 1;
        return;
    }
    if (info_open) {
        info_open = 0;
        dirty = 1;
        return;
    }
    if (chooser_open) {
        int w = 420, h = 240, cx0 = (LW - w) / 2, cy0 = (LH - h) / 2;
        int by = cy0 + 96;
        for (int i = 0; i < 2; i++) {
            int bx = cx0 + 32 + i * 190;
            if (x >= bx && x <= bx + 160 && y >= by && y <= by + 96) {
                theme_idx = i;
                TH = i ? &TH_LIGHT : &TH_DARK;
                if (!font_user)
                    font_combo = FAMILY_BASE[i ? 1 : 0] + 2;
                chooser_open = 0;
                theme_save();
                dirty = 1;
                return;
            }
        }
        /* a tap outside the two cards simply dismisses the sheet */
        chooser_open = 0;
        dirty = 1;
        return;
    }
    if (menu_open) {
        int mx, my;
        menu_rect(&mx, &my);
        if (x >= mx && x <= mx + MENU_W && y >= my && y <= my + MENU_H) {
            int idx = (y - my - MENU_TOP) / MENU_ROW;
            int n = (int)(sizeof MENU_ITEMS / sizeof MENU_ITEMS[0]);
            if (idx < 0) idx = 0;
            if (idx > n - 1) idx = n - 1;
            if (idx == 0) {
                theme_idx ^= 1;
                TH = theme_idx ? &TH_LIGHT : &TH_DARK;
                theme_save();
            } else if (idx == 1) {
                wall_dim ^= 1;
            } else {
                info_open = 1;
            }
        }
        menu_open = 0;
        dirty = 1;
        return;
    }
    if (app_open >= 0 && x <= 70 && y >= STATUS_H && y <= STATUS_H + 56) {
        if (net_sub) net_sub = 0;
        else if (app_open == 2 && settings_page == 1 && status_detail) status_detail = 0;
        else if (app_open == 2 && settings_page) settings_page = 0;
        else if (app_open == 6) { file_back(); return; }
        else app_open = -1;
        dirty = 1;
        return;
    }
    if (app_open == 1) {
        if (apikey[0] && y >= STATUS_H + 8 && y <= STATUS_H + 48 &&
            x >= 300 && x < 618) {
            int target = (x - 300) / 106;
            if (target >= 0 && target < 3) { wx_page = target; dirty = 1; }
            return;
        }
        if (x >= 628 && x <= LW - MARGIN && y >= STATUS_H + 8 && y <= STATUS_H + 48)
            wx_spawn_fetch();
        return;
    }
    if (app_open == 2 && net_sub) {
        if (network_hit(x, y)) return;
        return;
    }
    if (app_open == 2 && settings_hit(x, y)) return;
    if (app_open == 6 && file_hit(x, y)) return;
    if (app_open == 4 && org_calendar_hit(x, y)) return;
    if (app_open == 5 && org_notes_hit(x, y)) return;
    if (app_open >= 0) {
        if (app_open == 0) {
            clock_style ^= 1;
            theme_save();
            dirty = 1;
        }
        return;
    }
    if (page == 0) {
        int cw = (LW - 2 * MARGIN - 2 * 16) / 3, ch = 118, gx = 16, gy = 20, y0 = 108;
        for (int i = 0; i < 6; i++) {
            int cx0 = MARGIN + (i % 3) * (cw + gx);
            int cy0 = y0 + (i / 3) * (ch + gy);
            if (x >= cx0 && x <= cx0 + cw && y >= cy0 && y <= cy0 + ch) {
                detail_open = 1;
                detail_idx = i;
                dirty = 1;
                return;
            }
        }
    }
    if (page == 1) {
        int cell = (LW - 2 * MARGIN) / 5;
        for (int i = 0; i < N_HOME_APPS; i++) {
            int r = i / 5, c = i % 5;
            int cx = MARGIN + cell * c + cell / 2;
            int iy = 100 + r * 128;
            if (x >= cx - cell / 2 && x <= cx + cell / 2 && y >= iy - 8 && y <= iy + ICON + 34) {
                int app = HOME_APPS[i];
                app_open = app;
                if (app == 1) wx_page = 0;
                if (app == 6) file_reset();
                if (app == 4) org_today();
                if (app == 5) org_note_selected = org_note_count ? org_note_scroll : -1;
                dirty = 1;
                return;
            }
        }
    }
}

static void on_long_press(int x, int y)
{
    if (kb_open || clk_opt_open || detail_open || history_open || chooser_open || info_open || menu_open)
        return;
    if (app_open == 0) { clk_opt_open = 1; dirty = 1; return; }
    if (app_open >= 0 || page != 1) return;
    menu_open = 1;
    menu_x = x;
    menu_y = y;
    dirty = 1;
}

static void handle_event(const struct input_event *ev)
{
    if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
        if (touch_down && (abs(touch_x - touch_x0) > 18 || abs(touch_y - touch_y0) > 18)) {
            touch_moved = 1;
            press_active = 0;
            press_pending = 0;
            dirty = 1;
        }
        if (press_pending && press_active) {
            panel_to_land(touch_x, touch_y, &press_x, &press_y);
            if (land_valid) { draw_pressed(); present(); }
            press_pending = 0;
        }
    } else if (ev->type == EV_ABS) {
        if (ev->code == ABS_X || ev->code == ABS_MT_POSITION_X) touch_x = ev->value;
        else if (ev->code == ABS_Y || ev->code == ABS_MT_POSITION_Y) touch_y = ev->value;
    } else if (ev->type == EV_KEY && ev->code == BTN_TOUCH) {
        if (ev->value == 1) {
            touch_down = 1;
            touch_x0 = touch_x;
            touch_y0 = touch_y;
            touch_t0 = now_ms();
            long_fired = 0;
            swiped = 0;
            touch_moved = 0;
            panel_to_land(touch_x, touch_y, &press_x, &press_y);
            press_active = 1;
            press_pending = 1;
            dirty = 1;
        } else if (ev->value == 0 && touch_down) {
            touch_down = 0;
            press_active = 0;
            press_pending = 0;
            dirty = 1;
            int vx, vy, vx0, vy0;
            panel_to_land(touch_x, touch_y, &vx, &vy);
            panel_to_land(touch_x0, touch_y0, &vx0, &vy0);
            if (!long_fired) {
                int dx = vx - vx0, dy = vy - vy0;
                if (history_open && vy0 > STATUS_H + 56 &&
                    abs(dx) > 55 && abs(dx) > abs(dy) * 1.25f) {
                    hist_swipe(dx);
                    swiped = 1;
                } else if (app_open == 1 && apikey[0] && vy0 > STATUS_H + 56 &&
                    abs(dx) > 55 && abs(dx) > abs(dy) * 1.25f) {
                    int next = wx_page + (dx < 0 ? 1 : -1);
                    if (next < 0) next = 0;
                    if (next > 2) next = 2;
                    wx_page = next;
                    swiped = 1;
                    dirty = 1;
                } else if (app_open == 6 && !file_info_open && vy0 >= 152 &&
                           abs(dy) > 42 && abs(dy) > abs(dx) * 1.2f) {
                    file_swipe(dy);
                    swiped = 1;
                } else if (app_open == 5 && vy0 >= 146 && vx0 < 524 &&
                           abs(dy) > 42 && abs(dy) > abs(dx) * 1.2f) {
                    org_notes_swipe(dy);
                    swiped = 1;
                } else if (app_open < 0 && !menu_open && !info_open && !detail_open && !chooser_open &&
                           abs(dx) > 55 && abs(dx) > abs(dy) * 1.25f) {
                    page = (dx < 0) ? 1 : 0;
                    swiped = 1;
                    dirty = 1;
                } else if (!touch_moved && abs(dx) <= 18 && abs(dy) <= 18) {
                    on_tap(vx, vy);
                }
            }
        }
    }
}


/* ------------------------------------------------------------- power watch
   The panel sometimes blanks / flickers while the unit runs on battery even
   though the gauge reads nearly full.  Nothing in our code ever lowers the
   backlight, so log every state change of the pieces that could explain it:
   the backlight node, the disp "lcd" flag, USB/AC presence, and the battery
   voltage/current under load.  Pure read-only sampling. */
static int pw_lcdbl = -1, pw_lcd_on = -1, pw_usb = -1, pw_ac = -1;
static int pw_mv = -1, pw_ma = -1; /* sysfs microvolts and microamps */
static long pw_last_line, pw_last_sample;

static int rd_int(const char *path, int *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v;
    if (fscanf(f, "%d", &v) != 1) { fclose(f); return -1; }
    fclose(f);
    *out = v;
    return 0;
}

static int rd_lcd_on(void)
{
    FILE *f = fopen("/sys/class/disp/disp/attr/lcd", "r");
    if (!f) return -1;
    char b[128] = {0};
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return strstr(b, " on") ? 1 : 0;
}

static void power_watch(long now)
{
    int bl = -1, lcd = rd_lcd_on(), usb = -1, ac = -1;
    rd_int("/sys/class/disp/disp/attr/lcdbl", &bl);
    rd_int("/sys/class/power_supply/usb/online", &usb);
    rd_int("/sys/class/power_supply/ac/online", &ac);
    int changed = (bl != pw_lcdbl) || (lcd != pw_lcd_on) || (usb != pw_usb) || (ac != pw_ac);
    if (changed) {
        int mv = -1, ma = -1;
        rd_int("/sys/class/power_supply/battery/voltage_now", &mv);
        rd_int("/sys/class/power_supply/battery/current_now", &ma);
        if (mv >= 0) pw_mv = mv;
        if (ma >= 0) pw_ma = ma;
        printf("[%lds] power: lcdbl %d->%d lcd %d->%d usb %d->%d ac %d->%d  %dmV %dmA\n",
               now / 1000, pw_lcdbl, bl, pw_lcd_on, lcd, pw_usb, usb, pw_ac, ac,
               mv < 0 ? -1 : mv / 1000, ma < 0 ? -1 : ma / 1000);
        /* a blank we did not ask for is the thing we are hunting: grab context */
        if (pw_lcd_on == 1 && lcd == 0) {
            fflush(stdout);
            system("dmesg | tail -12 >> /tmp/lnc.log 2>&1");
        }
        fflush(stdout);
        pw_lcdbl = bl; pw_lcd_on = lcd; pw_usb = usb; pw_ac = ac;
        pw_last_line = now;
        if (app_open == 2 && settings_page == 1 && status_detail == 1) dirty = 1;
    }
    /* coarse trace whenever we are running off the cell.  Do NOT gate this on
       ac/online: the PMIC on this board reports ac=1 with the cable removed. */
    if ((usb == 0 || status_detail == 1) && now - pw_last_sample > 10000) {
        int mv = -1, ma = -1, cap = -1, st = -1;
        rd_int("/sys/class/power_supply/battery/voltage_now", &mv);
        rd_int("/sys/class/power_supply/battery/current_now", &ma);
        if (mv >= 0) pw_mv = mv;
        if (ma >= 0) pw_ma = ma;
        rd_int("/sys/class/power_supply/battery/capacity", &cap);
        rd_int("/sys/class/power_supply/ac/online", &st);
        if (usb == 0) {
            printf("[%lds] battery: %dmV %dmA %d%% ac=%d lcdbl=%d lcd=%d\n",
                   now / 1000, mv < 0 ? -1 : mv / 1000,
                   ma < 0 ? -1 : ma / 1000, cap, st, bl, lcd);
            fflush(stdout);
        }
        pw_last_sample = now;
        if (app_open == 2 && settings_page == 1 && status_detail == 1) dirty = 1;
    }
}

static void draw_status_detail(void)
{
    char buf[96];
    if (status_detail == 1) {
        draw_set_row(0, "供电来源", batt_usb_online == 1 ? "USB" :
                     battery_external_online() ? "充电器" :
                     battery_voltage_low() ? "电池 / 电压偏低" : "电池");
        if (batt_pct < 0) snprintf(buf, sizeof buf, "-");
        else snprintf(buf, sizeof buf, "%d%%", batt_pct);
        draw_set_row(1, "电池电量", buf);
        int bs = battery_ui_state();
        draw_set_row(2, "充电状态", bs == BAT_FULL ? "已充满" :
                     bs == BAT_CHARGING ? "充电中" :
                     bs == BAT_EXTERNAL ? "外接电源 / 未充电" : "未充电");
        const char *flow = bs == BAT_CHARGING && battery_external_online() ? "电源输入 / 电池充电" :
                           bs == BAT_FULL && battery_external_online() ? "电源输入 / 电池已满" :
                           bs == BAT_EXTERNAL ? "电源输入 / 电池待机" :
                           !battery_external_online() && !batt_charging ? "电池输出 / 设备运行" :
                           "状态待确认";
        draw_set_row(3, "电能流向", flow);
        if (pw_mv > 0 && pw_ma >= 0)
            snprintf(buf, sizeof buf, "约 %.2f W", (double)pw_mv * pw_ma / 1e12);
        else snprintf(buf, sizeof buf, "待确认");
        draw_set_row(4, "电池侧功率估算", buf);
        if (pw_mv > 0 && pw_ma >= 0)
            snprintf(buf, sizeof buf, "%.2f V · %.0f mA", pw_mv / 1e6,
                     pw_ma / 1e3);
        else snprintf(buf, sizeof buf, "待确认");
        draw_set_row(5, "电压 · 电流", buf);
    } else if (status_detail == 2) {
        draw_set_row(0, "连接状态", wifi_up ? "已连接" : "未连接");
        const char *sep = strrchr(net_link, ' ');
        if (wifi_up && sep) {
            snprintf(buf, sizeof buf, "%.*s", (int)(sep - net_link), net_link);
            draw_set_row(1, "网络名称", buf);
            draw_set_row(2, "IPv4 地址", sep + 1);
        } else {
            draw_set_row(1, "网络名称", wifi_up ? net_link : "-");
            draw_set_row(2, "IPv4 地址", "-");
        }
        draw_set_row(3, "无线接口", "wlan0");
        draw_set_row(4, "天气数据", apikey[0] ? "华风爱科" : "Open-Meteo");
        draw_set_row(5, "获取状态", wx_state == 1 ? "获取中" :
                     wx_state == 2 ? "已更新" : wx_state == 3 ? "获取失败" : "等待网络");
    } else if (status_detail == 3) {
        static const int order[6] = {0, 1, 2, 4, 5, 3};
        for (int i = 0; i < 6; i++) {
            int j = order[i];
            snprintf(buf, sizeof buf, "%s %s · %s", SENS[j].value,
                     SENS[j].unit, SENS[j].tag);
            draw_set_row(i, SENS[j].name, buf);
        }
    }
}

/* ------------------------------------------------------------------ backlight
   lcdbl is owned by whoever is driving the panel.  The vendor app leaves it at 0
   when it blanks, so once our UI takes over nobody would light the panel again. */
#define BL_PATH "/sys/class/disp/disp/attr/lcdbl"

static int bl_fd = -1;
static int bl_level = 80;      /* working brightness */
static int bl_dim = 0;         /* idle level; 0 means "never dim" */
static int bl_dimmed = 0;
static long bl_last_touch = 0;
static long bl_idle_ms = 90000L;   /* dim after 90 s without touch */

static int bl_read(void)
{
    char b[16];
    if (bl_fd < 0) return -1;
    memset(b, 0, sizeof b);
    if (pread(bl_fd, b, sizeof b - 1, 0) <= 0) return -1;
    return atoi(b);
}

static int bl_write(int v)
{
    char b[16];
    int n;
    if (bl_fd < 0) return -1;
    n = snprintf(b, sizeof b, "%d", v);
    if (pwrite(bl_fd, b, (size_t)n, 0) != n) return -1;
    return 0;
}

static void bl_init(void)
{
    if (bl_fd < 0) bl_fd = open(BL_PATH, O_RDWR);
    bl_write(bl_level);
}

/* anything that turns the light off while we own the panel gets undone;
   note we restore to a non-zero level even when idling, see the comment above */
static void bl_guard(void)
{
    /* the vendor dims the panel (100-130) before blanking it, and a blanked
       panel takes the touch controller down with it, so keep asserting our
       target instead of only reacting to a hard zero */
    int want = bl_dimmed ? (bl_dim > 0 ? bl_dim : bl_level) : bl_level;
    int cur = bl_read();
    if (cur >= 0 && cur != want) bl_write(want);
}

static void bl_wake(void)
{
    if (bl_read() < bl_level) bl_write(bl_level);
    bl_dimmed = 0;
}

static void bl_idle_check(long now)
{
    if (!bl_dimmed && bl_last_touch && now - bl_last_touch > bl_idle_ms &&
        bl_dim > 0 && bl_dim < bl_level) {
        bl_write(bl_dim);
        bl_dimmed = 1;
    }
}

static void bl_close(void)
{
    if (bl_fd >= 0) {
        bl_write(bl_level);
        close(bl_fd);
        bl_fd = -1;
    }
}

/* User changes are rare. Keep the key and preferences on mounted UDISK only,
   with an atomic replacement; a child absorbs slow NAND fsyncs. */
#define CONFIG_FILE "/mnt/UDISK/launcher/config.ini"
static unsigned long config_rev;
static int config_state;            /* 0 idle, 1 saving, 2 saved, 3 failed */
static long config_started_ms;

static void config_poll(void)
{
    if (config_state != 1) return;
    char path[64], result[8] = {0};
    snprintf(path, sizeof path, "/tmp/config_result.%lu", config_rev);
    FILE *f = fopen(path, "r");
    if (f) {
        (void)fgets(result, sizeof result, f);
        fclose(f); unlink(path);
        config_state = !strncmp(result, "ok", 2) ? 2 : 3;
        dirty = 1;
    } else if (now_ms() - config_started_ms > 15000) {
        config_state = 3; dirty = 1;
    }
}

static int config_mount_ready(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    char dev[96], mount[96], fs[32], rest[256];
    int ready = 0;
    while (fscanf(f, "%95s %95s %31s %255s %*d %*d", dev, mount, fs, rest) == 4) {
        if (!strcmp(mount, "/mnt/UDISK") && !strcmp(fs, "ext4") && strstr(rest, "rw")) {
            ready = 1; break;
        }
    }
    fclose(f);
    return ready;
}

static void config_save(void)
{
    if (!config_mount_ready()) { config_state = 3; dirty = 1; return; }
    unsigned long rev = ++config_rev;
    config_state = 1;
    config_started_ms = now_ms();
    dirty = 1;
    pid_t pid = fork();
    if (pid != 0) { if (pid < 0) config_state = 3; return; }
    mkdir(BASELINE_DIR, 0700);
    chmod(BASELINE_DIR, 0700);
    int lockfd = open(BASELINE_DIR "/config.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lockfd < 0 || flock(lockfd, LOCK_EX)) _exit(1);
    FILE *old = fopen(CONFIG_FILE, "r");
    if (old) {
        unsigned long prior = 0;
        char line[96];
        if (fgets(line, sizeof line, old) && fgets(line, sizeof line, old) &&
            sscanf(line, "revision=%lu", &prior) == 1 && prior > rev) {
            fclose(old); _exit(0);
        }
        fclose(old);
    }
    char tmp[128];
    snprintf(tmp, sizeof tmp, BASELINE_DIR "/config.%ld.tmp", (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) _exit(1);
    int n = dprintf(fd,
        "version=2\nrevision=%lu\ntheme=%d\nfont=%d\nclock_style=%d\n"
        "clock0=%d,%d,%d\nclock1=%d,%d,%d\ncity=%s\napikey=%s\n"
        "brightness=%d\ndim=%d\nidle_ms=%ld\nweather_min=%d\nlanguage=%d\n"
        "timezone_min=%d\nsync_hours=%d\n",
        rev, theme_idx, font_combo, clock_style,
        clock_family[0], clock_size_idx[0], clock_weight[0],
        clock_family[1], clock_size_idx[1], clock_weight[1],
        city, apikey, bl_level, bl_dim, bl_idle_ms, wx_refresh_min, ui_lang,
        tz_offset_min, sync_interval_hours);
    int ok = n > 0 && fsync(fd) == 0;
    if (close(fd)) ok = 0;
    if (ok) ok = rename(tmp, CONFIG_FILE) == 0;
    if (!ok) {
        unlink(tmp); _exit(1);
    }
    int dirfd = open(BASELINE_DIR, O_RDONLY | O_DIRECTORY);
    if (dirfd >= 0) { fsync(dirfd); close(dirfd); }
    char result[64];
    snprintf(result, sizeof result, "/tmp/config_result.%lu", rev);
    int res = open(result, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (res >= 0) { (void)write(res, "ok\n", 3); close(res); }
    _exit(0);
}

static void config_load(void)
{
    if (!config_mount_ready()) return;
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        char *e = strchr(line, '=');
        if (!e) continue;
        *e++ = 0;
        e[strcspn(e, "\r\n")] = 0;
        int v = atoi(e), fam, sz, wt;
        if (!strcmp(line, "revision")) config_rev = strtoul(e, NULL, 10);
        else if (!strcmp(line, "theme") && v >= 0 && v <= 1) {
            theme_idx = v; TH = v ? &TH_LIGHT : &TH_DARK;
            chooser_open = 0; /* persistent choice; /tmp is cleared on reboot */
        }
        else if (!strcmp(line, "font") && v >= 0 && v < 10) { font_combo = v; font_user = 1; }
        else if (!strcmp(line, "clock_style") && v >= 0 && v <= 1) clock_style = v;
        else if (!strcmp(line, "clock0") || !strcmp(line, "clock1")) {
            int i = line[5] - '0';
            if (sscanf(e, "%d,%d,%d", &fam, &sz, &wt) == 3 &&
                fam >= 0 && fam <= 2 && sz >= 0 && sz <= 2 && wt >= 0 && wt <= 3) {
                clock_family[i] = fam; clock_size_idx[i] = sz; clock_weight[i] = wt;
            }
        } else if (!strcmp(line, "city")) snprintf(city, sizeof city, "%s", e);
        else if (!strcmp(line, "apikey")) snprintf(apikey, sizeof apikey, "%s", e);
        else if (!strcmp(line, "brightness") && v >= 20 && v <= 100) bl_level = v;
        else if (!strcmp(line, "dim") && v >= 0 && v <= 100) bl_dim = v;
        else if (!strcmp(line, "idle_ms") && v >= 15000 && v <= 600000) bl_idle_ms = v;
        else if (!strcmp(line, "weather_min") && v >= 15 && v <= 180) wx_refresh_min = v;
        else if (!strcmp(line, "language") && v >= 0 && v <= 1) ui_lang = v;
        else if (!strcmp(line, "timezone_min") && v >= -720 && v <= 840 && v % 30 == 0)
            tz_offset_min = v;
        else if (!strcmp(line, "sync_hours") &&
                 (v == 3 || v == 12 || v == 24 || v == 72 || v == 168))
            sync_interval_hours = v;
    }
    fclose(f);
}


/* ------------------------------------------------------------------ vendor app
   BranOS keeps repainting the panel, so whoever owns the screen has to stop it.
   SIGSTOP is used instead of SIGKILL because watchdog.sh restarts a dead BranOS
   and reboots after repeated failures. */
static int vendor_pid;

static int find_vendor(void)
{
    DIR *proc = opendir("/proc");
    if (!proc) return 0;
    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL) {
        char *end;
        long candidate = strtol(entry->d_name, &end, 10);
        if (*end || candidate <= 0 || candidate > 2147483647L) continue;
        int pid = (int)candidate;
        char p[32];
        snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        char buf[256] = {0};
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        if (n > 8 && strstr(buf, "/usr/bin/data/bin/BranOS")) {
            closedir(proc);
            return pid;
        }
    }
    closedir(proc);
    return 0;
}

static void vendor_freeze(void)
{
    if (getenv("NO_FREEZE")) return;
    if (!vendor_pid) vendor_pid = find_vendor();
    if (vendor_pid > 0 && kill(vendor_pid, SIGSTOP) == 0)
        printf("vendor app %d stopped\n", vendor_pid);
    fflush(stdout);
}

/* returns 1 when the process is currently stopped (state 'T') */
static int vendor_is_stopped(void)
{
    if (vendor_pid <= 0) return 0;
    char path[40], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", vendor_pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strrchr(buf, ')');
    if (!p || p[1] != ' ' || p[2] == 0) return 0;
    return (p[2] == 'T' || p[2] == 't');
}

/* the panel has a single painter: if the vendor app comes back to life the
   two would alternate frames, so re-stop it as soon as that is detected */
static void vendor_ensure(void)
{
    if (getenv("NO_FREEZE")) return;
    if (vendor_pid > 0 && !vendor_is_stopped()) {
        kill(vendor_pid, SIGSTOP);
        printf("vendor app %d re-stopped\n", vendor_pid);
        fflush(stdout);
    }
}

static void vendor_thaw(void)
{
    if (vendor_pid > 0) {
        kill(vendor_pid, SIGCONT);
        vendor_pid = 0;
    }
}

static void on_term(int sig)
{
    (void)sig;
    vendor_thaw();
    bl_close();
    _exit(0);
}

static void poll_input(int timeout_ms)
{
    struct pollfd p = {.fd = evfd, .events = POLLIN};
    int r = poll(&p, 1, timeout_ms);
    if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        printf("input poll flags %x -> reopen\n", p.revents);
        fflush(stdout);
        input_reopen();
        return;
    }
    if (r > 0 && (p.revents & POLLIN)) {
        struct input_event evs[32];
        bl_last_touch = now_ms();
        bl_wake();
        ssize_t n = read(evfd, evs, sizeof evs);
        if (n <= 0) {
            printf("input read %d -> reopen\n", (int)n);
            fflush(stdout);
            input_reopen();
            return;
        }
        ev_seen += (long)(n / (ssize_t)sizeof evs[0]);
        for (ssize_t i = 0; i < n / (ssize_t)sizeof evs[0]; i++)
            handle_event(&evs[i]);
    }
    if (touch_down && !long_fired && !touch_moved && now_ms() - touch_t0 > 460) {
        int vx, vy;
        panel_to_land(touch_x, touch_y, &vx, &vy);
        long_fired = 1;
        press_active = 0;
        on_long_press(vx, vy);
        dirty = 1;
    }
}

/* ------------------------------------------------------------------- main */
int main(int argc, char **argv)
{
    const char *atlas_path = argc > 1 ? argv[1] : "/tmp/atlas.bin";
    int offline_atlas = argc > 2 && !strcmp(argv[2], "shot");
    int guarded_boot = getenv("AUTOBOOT_GUARD") != NULL;
    freopen("/tmp/lnc.log", "a", stdout);
    setvbuf(stdout, NULL, _IOLBF, 0);
    theme_init();
    /* the device boots with TZ unset (UTC); pin it so localtime_r() and the
       clock page show wall-clock time without writing to the read-only root */
    setenv("TZ", "CST-8", 1);
    tzset();
    if (guarded_boot ? atlas_load(atlas_path) :
        ((offline_atlas ? atlas_load(atlas_path) : atlas_load("/tmp/atlas.bin")) &&
         (offline_atlas ? atlas_load("/tmp/atlas.bin") : atlas_load(atlas_path)))) {
        printf("atlas load failed for %s and /tmp/atlas.bin\n", atlas_path);
        fflush(stdout);
        return 1;
    }
    atexit(vendor_thaw);
    if (argc > 2 && !strcmp(argv[2], "historytest")) {
        hist_path = "/tmp/launcher-history-test.bin";
        hist_temp_path = "/tmp/launcher-history-test.tmp";
        unlink(hist_path);
        hist_runtime = 1;
        hist_sample(0, 12.3);
        hist_sample(0, 17.7);
        hist_sample(3, 200);
        int64_t slot = hist_live_slot;
        struct hist_record *sample = hist_get(slot);
        if (!sample || sample->metric[0].count != 2) return 31;
        hist_save(sample);
        for (int i = 0; i < 500 && hist_save_pipe >= 0; i++) {
            hist_poll();
            usleep(10000);
        }
        if (hist_save_state != 2) return 32;
        memset(hist_ring, 0, sizeof hist_ring);
        hist_live_slot = -1;
        hist_load();
        sample = hist_get(slot);
        if (!sample || sample->metric[0].count != 2 ||
            fabs(sample->metric[0].sum - 30.0) > .001 ||
            sample->metric[3].count != 1) return 33;
        int fd = open(hist_path, O_WRONLY | O_APPEND);
        if (fd < 0 || write(fd, "bad", 3) != 3) return 34;
        close(fd);
        hist_load();
        struct stat st;
        if (stat(hist_path, &st) || st.st_size != sizeof(struct hist_record)) return 35;
        printf("historytest: save, restore, invalid-tail recovery passed\n");
        unlink(hist_path);
        return 0;
    }
    if (argc > 2 && strcmp(argv[2], "ntptest") == 0) {
        struct timeval sample;
        int rc = ntp_query(&sample);
        printf("ntptest rc=%d epoch=%ld usec=%ld server=%s\n", rc,
               rc ? 0L : (long)sample.tv_sec,
               rc ? 0L : (long)sample.tv_usec, NTP_HOST);
        return rc != 0;
    }
    if (argc > 2 && strcmp(argv[2], "gas") == 0) {
        for (int i = 0; i < 6; i++) {
            int w0 = -1, w1 = -1;
            int rc = i2c_probe_gas(2, 0x58, &w0, &w1);
            printf("gas rc=%d word0(co2_eq)=%d word1(tvoc)=%d sysfs_tvoc=%.2f\n", rc, w0, w1, gas_tvoc());
            fflush(stdout);
            sleep(1);
        }
        return 0;
    }
    if (argc > 2 && strcmp(argv[2], "timetest") == 0) {
        FILE *f = fopen("/tmp/http_head.txt", "r");
        if (!f) { printf("no /tmp/http_head.txt\n"); return 1; }
        char line[256];
        long epoch = 0;
        while (fgets(line, sizeof line, f)) {
            char low[8] = {0};
            for (int i = 0; i < 5 && line[i]; i++) low[i] = (char)tolower((unsigned char)line[i]);
            if (!strncmp(low, "date:", 5)) { epoch = parse_http_date(line + 5); break; }
        }
        fclose(f);
        time_t tt = (time_t)epoch;
        struct tm tm2;
        localtime_r(&tt, &tm2);
        printf("epoch=%ld local=%04d-%02d-%02d %02d:%02d:%02d\n", epoch,
               tm2.tm_year + 1900, tm2.tm_mon + 1, tm2.tm_mday,
               tm2.tm_hour, tm2.tm_min, tm2.tm_sec);
        return 0;
    }
    if (argc > 2 && strcmp(argv[2], "spy") == 0) { i2c_spy(); return 0; }
    if (argc > 2 && strcmp(argv[2], "pm") == 0) {
        double pm = -1;
        int rc = pm25_ugm3(&pm);
        if (rc == 0) printf("pm25=%.2f ug/m3\n", pm);
        else printf("pm25 read failed: %d\n", rc);
        fflush(stdout);
        return rc == 0 ? 0 : 1;
    }
    if (argc > 2 && strcmp(argv[2], "sense") == 0) {
        double tc = 0, rh = 0, lux = 0;
        int ec = -1, tv = -1, cap = -1;
        int a = i2c_probe_temp_humi(2, 0x70, &tc, &rh);
        int b2 = i2c_probe_light(1, 0x23, &lux);
        int c = i2c_probe_gas(2, 0x58, &ec, &tv);
        read_int_file("/sys/class/power_supply/battery/capacity", &cap);
        printf("shtc3(2-0x70)=%d t=%.2f rh=%.2f | bh1721(1-0x23)=%d lux=%.1f | sgp30(2-0x58)=%d tvoc=%d eco2=%d | batt=%d\n",
               a, tc, rh, b2, lux, c, tv, ec, cap);
        fflush(stdout);
        return 0;
    }
    int shot_mode = (argc > 2 && strcmp(argv[2], "shot") == 0);
    if (!shot_mode) {
        if (fb_open()) return 1;
        fb_ready = 1;
        if (input_open()) { printf("input: no touch device\n"); }
        const char *bidle = getenv("BL_IDLE");
        if (bidle) bl_idle_ms = atol(bidle);
        const char *bdim = getenv("BL_DIM");
        if (bdim) bl_dim = atoi(bdim);
        bl_init();
        atexit(bl_close);
        signal(SIGTERM, on_term);
        signal(SIGINT, on_term);
        signal(SIGHUP, on_term);
        bl_last_touch = now_ms();
        vendor_freeze();
        printf("backlight: %d -> %d\n", bl_read(), bl_level);
        fflush(stdout);
    }

    for (int i = 0; i < 6; i++) {
        snprintf(SENS[i].tag, sizeof SENS[i].tag, "—");
        SENS[i].color = 0x78808C;
    }
    snprintf(SENS[0].name, sizeof SENS[0].name, "PM2.5");
    snprintf(SENS[0].unit, sizeof SENS[0].unit, "μg/m³");
    snprintf(SENS[1].name, sizeof SENS[1].name, "温度");
    snprintf(SENS[1].unit, sizeof SENS[1].unit, "°C");
    snprintf(SENS[2].name, sizeof SENS[2].name, "湿度");
    snprintf(SENS[2].unit, sizeof SENS[2].unit, "%%");
    snprintf(SENS[3].name, sizeof SENS[3].name, "空气质量");
    snprintf(SENS[3].unit, sizeof SENS[3].unit, "AQI");
    snprintf(SENS[4].name, sizeof SENS[4].name, "TVOC");
    snprintf(SENS[4].unit, sizeof SENS[4].unit, "ppb");
    snprintf(SENS[5].name, sizeof SENS[5].name, "光照");
    snprintf(SENS[5].unit, sizeof SENS[5].unit, "lx");
    for (int i = 0; i < 6; i++)
        if (!SENS[i].value[0]) snprintf(SENS[i].value, sizeof SENS[i].value, "-");

    theme_load();
    {
        FILE *cf = fopen("/tmp/launcher_city.txt", "r");
        if (cf) {
            if (fgets(city, sizeof city, cf)) {
                size_t l = strlen(city);
                while (l && (city[l - 1] == '\n' || city[l - 1] == '\r')) city[--l] = 0;
            }
            fclose(cf);
        }
        FILE *kf = fopen("/tmp/launcher_apikey.txt", "r");
        if (kf) {
            if (fgets(apikey, sizeof apikey, kf)) {
                size_t l = strlen(apikey);
                while (l && (apikey[l - 1] == '\n' || apikey[l - 1] == '\r')) apikey[--l] = 0;
            }
            fclose(kf);
        }
    }
    config_load();
    org_load();
    if (!shot_mode) hist_load();
    org_today();
    apply_launcher_timezone();
    if (shot_mode && getenv("UI_LANG")) ui_lang = atoi(getenv("UI_LANG")) == 1;
    if (!shot_mode) bl_write(bl_level);
    upd_time();
    if (!shot_mode || !getenv("SHOT_NO_SENSORS")) sense_all();
    pm_async = !shot_mode;
    printf("state: batt=%d%% charging=%d full=%d wifi=%d\n", batt_pct, batt_charging, batt_full, wifi_up);
    fflush(stdout);

    long last_wxpoll = 0;
    long last_frame = 0, last_sense = now_ms(), last_clock = 0, last_bl = 0;
    long last_gas = now_ms();
    long frames = 0, t_report = now_ms();
    long render_ms_sum = 0, render_window_count = 0;
    long last_heartbeat = 0;
    if (!shot_mode) gas_restore_baseline();
    wx_try_load();
    if (shot_mode) {
        dump_path = argc > 3 ? argv[3] : "/tmp/shot.raw";
        theme_idx = argc > 4 ? atoi(argv[4]) : 0;
    font_combo = FAMILY_BASE[theme_idx ? 1 : 0] + 2;
        page = argc > 5 ? atoi(argv[5]) : 0;
        TH = theme_idx ? &TH_LIGHT : &TH_DARK;
        chooser_open = 0;          /* offline renders never show the overlay */
        int ov = argc > 6 ? atoi(argv[6]) : 0;   /* 1 menu, 2 about, 3 chooser */
        if (argc > 7) app_open = atoi(argv[7]);
        if (app_open == 6) {
            const char *fp = getenv("FILE_PATH");
            if (fp && fp[0] == '/' && strlen(fp) < sizeof file_path)
                snprintf(file_path, sizeof file_path, "%s", fp);
            file_scan();
            const char *fs = getenv("FILE_SCROLL");
            if (fs) {
                file_scroll = atoi(fs);
                if (file_scroll < 0) file_scroll = 0;
                if (file_scroll > file_count - FILE_VISIBLE_ROWS)
                    file_scroll = file_count > FILE_VISIBLE_ROWS ? file_count - FILE_VISIBLE_ROWS : 0;
            }
            const char *fi = getenv("FILE_INFO_IDX");
            if (fi) {
                file_info_index = atoi(fi);
                file_info_open = file_info_index >= 0 && file_info_index < file_count;
            }
        }
        if (argc > 8) clock_style = atoi(argv[8]);
        const char *wp = getenv("WX_PAGE");
        if (wp) { wx_page = atoi(wp); if (wx_page < 0) wx_page = 0; if (wx_page > 2) wx_page = 2; }
        const char *sp = getenv("SETTINGS_PAGE"), *sd = getenv("STATUS_DETAIL");
        if (sp) settings_page = atoi(sp);
        if (sd) status_detail = atoi(sd);
        if (argc > 9) preview_hour = atoi(argv[9]);
        if (argc > 10) { int v = atoi(argv[10]); clock_size_idx[clock_style ? 1 : 0] = v < 0 ? 0 : (v > 2 ? 2 : v); }
        if (argc > 11) { int v = atoi(argv[11]); clock_weight[clock_style ? 1 : 0] = v < 0 ? 0 : (v > 3 ? 3 : v); }
        if (ov == 1) { menu_open = 1; menu_x = 470; menu_y = 252; }
        else if (ov == 2) info_open = 1;
        else if (ov == 3) chooser_open = 1;
        else if (ov == 4) { detail_open = 1; detail_idx = argc > 8 ? atoi(argv[8]) : 1; }
        else if (ov == 8) {          /* preview of the network page */
            app_open = 2;
            net_sub = 1;
            if (access("/tmp/net_scan.txt", F_OK) == 0) net_parse_scan();
            else { net_state = 1; net_scan(); }
        }
        else if (ov == 7) {          /* preview of the clock options sheet */
            clk_opt_open = 1;
            app_open = 0;
        }
        else if (ov == 6) {          /* preview of the soft keyboard */
            kb_open = 1;
            snprintf(kb_text, sizeof kb_text, "miot-2.4g");
            kb_len = (int)strlen(kb_text);
        }
        else if (ov == 5) {          /* preview of a calibration in progress */
            detail_open = 1;
            detail_idx = argc > 8 ? atoi(argv[8]) : 4;
            cal_state = CAL_RUN;
            cal_start_ms = now_ms() - 7L * 60L * 1000L - 38L * 1000L;
        }
        else if (ov == 9) {
            history_open = 1;
            history_idx = argc > 8 ? atoi(argv[8]) : 0;
            if (history_idx < 0 || history_idx > 5) history_idx = 0;
            history_span = getenv("HISTORY_SPAN") && atoi(getenv("HISTORY_SPAN")) == 1;
            history_page = history_span ? 2 : 3;
            const char *hp = getenv("HISTORY_PAGE");
            if (hp) history_page = atoi(hp);
            hist_page_clamp();
            if (getenv("HISTORY_PREVIEW")) hist_demo_fill();
        }
        /* offline render: never touch the vendor app, another process owns the panel */
        upd_time();
        if (!getenv("SHOT_NO_SENSORS")) sense_all();
        if (getenv("SHOT_WIFI")) wifi_up = atoi(getenv("SHOT_WIFI")) != 0;
        if (getenv("SHOT_BATTERY")) {
            batt_pct = atoi(getenv("SHOT_BATTERY"));
            snprintf(batt_s, sizeof batt_s, "%d%%", batt_pct);
        }
        if (getenv("SHOT_CHARGING")) batt_charging = atoi(getenv("SHOT_CHARGING")) != 0;
        if (getenv("SHOT_FULL")) batt_full = atoi(getenv("SHOT_FULL")) != 0;
        if (getenv("SHOT_USB")) {
            batt_usb_online = atoi(getenv("SHOT_USB")) != 0;
            pw_usb = batt_usb_online;
        }
        if (getenv("SHOT_AC")) {
            batt_ac_online = atoi(getenv("SHOT_AC")) != 0;
            pw_ac = batt_ac_online;
        }
        if (getenv("SHOT_CURRENT_UA")) batt_current_ua = atoi(getenv("SHOT_CURRENT_UA"));
        if (getenv("SHOT_EXTERNAL_HITS")) batt_external_hits = atoi(getenv("SHOT_EXTERNAL_HITS"));
        if (getenv("SHOT_DISCHARGING")) batt_discharging = atoi(getenv("SHOT_DISCHARGING")) != 0;
        if (getenv("SHOT_NOT_CHARGING")) batt_not_charging = atoi(getenv("SHOT_NOT_CHARGING")) != 0;
        if (getenv("SHOT_LOW_VOLTAGE")) batt_low_voltage_hits = atoi(getenv("SHOT_LOW_VOLTAGE")) ? 2 : 0;
        if (getenv("SHOT_SENSOR_DEMO")) {
            static const char *values[6] = {"16.3", "26.8", "74.1", "72", "44", "128"};
            static const char *tags[6] = {"良好", "舒适", "偏湿", "轻度污染", "良好", "适中"};
            for (int i = 0; i < 6; i++) {
                snprintf(SENS[i].value, sizeof SENS[i].value, "%s", values[i]);
                snprintf(SENS[i].tag, sizeof SENS[i].tag, "%s", tags[i]);
                SENS[i].color = i == 3 ? 0xDE9162 : 0x3FB98A;
            }
        }
        render();
        return 0;
    }
    int demo = (argc > 2 && strcmp(argv[2], "demo") == 0);
    int bench = (argc > 2 && strcmp(argv[2], "bench") == 0);
    int demo_step = 0;
    long t_demo0 = now_ms();
    printf("launcher running (demo=%d)\n", demo);
    { time_t tt = time(NULL); struct tm t3; localtime_r(&tt, &t3);
      printf("clock: epoch=%ld local=%04d-%02d-%02d %02d:%02d TZ=%s\n", (long)tt,
             t3.tm_year + 1900, t3.tm_mon + 1, t3.tm_mday, t3.tm_hour, t3.tm_min,
             getenv("TZ") ? getenv("TZ") : "unset"); }
    fflush(stdout);

    for (;;) {
        poll_input(16);
        /* reap the short-lived persistence children */
        while (waitpid(-1, 0, WNOHANG) > 0) {
        }
        long t = now_ms();
        pm_poll(t);
        hist_poll();
        config_poll();
        org_poll();
        if (t - last_bl >= 1000) {
            last_bl = t;
            bl_guard();
            bl_idle_check(t);
    power_watch(t);
        }
        if (demo) {
            long e = t - t_demo0;
            if (demo_step == 0 && e > 1200) {
                chooser_open = 0; theme_idx = 0; TH = &TH_DARK; dirty = 1; demo_step = 1;
                printf("DEMO t=%ld dismiss chooser (dark)\n", e);
            } else if (demo_step == 1 && e > 4000) {
                theme_idx = 1; TH = &TH_LIGHT; dirty = 1; demo_step = 2;
                printf("DEMO t=%ld switch to light\n", e);
            } else if (demo_step == 2 && e > 7000) {
                page = 1; dirty = 1; demo_step = 3;
                printf("DEMO t=%ld icons page (light)\n", e);
            } else if (demo_step == 3 && e > 9200) {
                theme_idx = 0; TH = &TH_DARK; dirty = 1; demo_step = 4;
                printf("DEMO t=%ld back to dark\n", e);
            } else if (demo_step == 4 && e > 11500) {
                page = 1; dirty = 1; demo_step = 5;
                printf("DEMO t=%ld icons page (dark)\n", e);
            } else if (demo_step == 5 && e > 14500) {
                menu_open = 1; menu_x = 400; menu_y = 250; dirty = 1; demo_step = 6;
                printf("DEMO t=%ld long-press menu\n", e);
            } else if (demo_step == 6 && e > 16000) {
                menu_open = 0; info_open = 1; dirty = 1; demo_step = 7;
                printf("DEMO t=%ld about dialog\n", e);
            } else if (demo_step == 7 && e > 19000) {
                info_open = 0; chooser_open = 1; dirty = 1; demo_step = 8;
                printf("DEMO t=%ld theme chooser\n", e);
            } else if (demo_step == 8 && e > 21500) {
                printf("DEMO done\n");
                fflush(stdout);
                _exit(0);
            }
        }
        if (t - last_clock > 500) {
            char old_clock[sizeof clock_s], old_date[sizeof date_s];
            memcpy(old_clock, clock_s, sizeof old_clock);
            memcpy(old_date, date_s, sizeof old_date);
            upd_time(); vendor_ensure(); gas_cal_tick(t); last_clock = t;
            if (strcmp(old_clock, clock_s) || strcmp(old_date, date_s) ||
                (cal_state == CAL_RUN && detail_open)) dirty = 1;
        }
        if (t - last_gas > 1000) {
            char old_tvoc[sizeof SENS[4].value];
            memcpy(old_tvoc, SENS[4].value, sizeof old_tvoc);
            sense_gas(); last_gas = t;
            if (strcmp(old_tvoc, SENS[4].value) &&
                ((app_open < 0 && page == 0) || detail_open ||
                 (app_open == 2 && settings_page == 1 && status_detail == 3))) dirty = 1;
        }
        if (t - last_wxpoll > 700) { wx_poll(); net_poll(); last_wxpoll = t; }
        if (t - last_auto > 2000) { last_auto = t; net_autopilot(); }
        if (t - last_sense > 2500) {
            hist_tick();
            int old_batt = batt_pct, old_wifi = wifi_up, old_charge = batt_charging;
            int old_battery_state = battery_ui_state();
            char old_temp[sizeof SENS[1].value];
            memcpy(old_temp, SENS[1].value, sizeof old_temp);
            sense_all(); pm_start(t); last_sense = t;
            if (old_batt != batt_pct || old_wifi != wifi_up || old_charge != batt_charging ||
                old_battery_state != battery_ui_state() ||
                (app_open == 0 && strcmp(old_temp, SENS[1].value)) ||
                (app_open < 0 && page == 0) || detail_open ||
                (app_open == 2 && settings_page == 1)) dirty = 1;
        }
        if (dirty && t - last_frame >= 16) {
            long t0 = now_ms();
            render();
            last_frame = now_ms();
            render_ms_sum += last_frame - t0;
            render_window_count++;
            frames++;
            if (render_window_count >= 30) {
                printf("render avg %.1f ms over %ld renders\n", (double)render_ms_sum / render_window_count, render_window_count);
                fflush(stdout);
                render_ms_sum = 0;
                render_window_count = 0;
            }
        }
        if (guarded_boot && land_valid && t - last_heartbeat >= 2000) {
            FILE *hb = fopen("/tmp/launcherboot.hb.tmp", "w");
            if (hb) {
                fprintf(hb, "%ld\n", t / 1000);
                if (fclose(hb) == 0)
                    rename("/tmp/launcherboot.hb.tmp", "/tmp/launcherboot.hb");
            }
            last_heartbeat = t;
        }
        if (bench && frames >= 90) { printf("bench: 90 renders done\n"); fflush(stdout); _exit(0); }
        if (t - t_report > 3000) {
            printf("~%.1f renders/s (last 3s: %ld) ev=%ld tap=%ld lcdbl=%d\n",
                   frames / 3.0, frames, ev_seen, tap_seen, bl_read());
            fflush(stdout);
            frames = 0;
            t_report = t;
        }
    }
    return 0;
}



