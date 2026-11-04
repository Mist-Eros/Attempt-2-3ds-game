/*
 * plat_host.c - PC software rasterizer backend for include/gfx.h
 *
 * Part of the host preview harness (tools/preview).  Implements every gfx.h
 * entry point with a plain RGBA8 software rasterizer.  Both 3DS screens are
 * kept in memory; gfx_present() can dump them as PNGs (via the public-domain
 * stb_image_write.h) so the game's layout can be inspected without hardware.
 *
 * Conventions:
 *   - top-left origin, +y down, strict clipping to the current target
 *   - source-over alpha blending, destination kept opaque (alpha 255)
 *   - geometry is snapped to the pixel grid with round-to-nearest on rect
 *     edges (so a 1 px line at integer coordinates stays exactly 1 px)
 *   - gfx_clear() replaces pixels (it does not blend)
 *
 * This file is layout-verification tooling only; it is never built into the
 * 3DS binary.  No 3DS headers, no SDL, no window.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "gfx.h"
#include "host.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* The 8x8 public-domain font table.  It is a *definition* (not just a
 * declaration), so this is the only translation unit allowed to include it. */
#include "../../source/gfx/font8x8_basic.h"

/* ------------------------------------------------------------------ */
/* framebuffers                                                        */
/* ------------------------------------------------------------------ */

#define TOP_BYTES    ((size_t)TOP_W * TOP_H * 4u)
#define BOT_BYTES    ((size_t)BOT_W * BOT_H * 4u)

typedef struct {
    int w, h;
    uint8_t *px;   /* RGBA8, row-major, w*4 stride */
} Surface;

static uint8_t g_top_px[TOP_W * TOP_H * 4];
static uint8_t g_bot_px[BOT_W * BOT_H * 4];

static Surface g_top    = { TOP_W, TOP_H, g_top_px };
static Surface g_bot    = { BOT_W, BOT_H, g_bot_px };
static Surface *g_cur   = &g_top;
static Screen   g_cur_screen = SCREEN_TOP;

/* ------------------------------------------------------------------ */
/* capture state                                                       */
/* ------------------------------------------------------------------ */

static char g_outdir[1024] = ".";
static int  g_capture_enabled = 0;
static int  g_capture_defer = 0;
static int  g_req_frame = -1;      /* frame armed for capture, -1 = none */
static int  g_written_frame = -1;  /* frame already dumped */
static int  g_present_count = 0;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int iroundf(float v)
{
    return (int)floorf(v + 0.5f);
}

typedef struct { int r, g, b, a; } C8;

static int clamp255(float v)
{
    float f = v * 255.0f;
    if (f <= 0.0f) return 0;
    if (f >= 255.0f) return 255;
    return (int)(f + 0.5f);
}

static C8 color8(GfxColor c)
{
    C8 o;
    o.r = clamp255(c.r);
    o.g = clamp255(c.g);
    o.b = clamp255(c.b);
    o.a = clamp255(c.a);
    return o;
}

static inline void blend_px(Surface *s, int x, int y, C8 c)
{
    uint8_t *p;
    int ia;
    if (c.a <= 0) return;
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    p = s->px + ((size_t)y * (size_t)s->w + (size_t)x) * 4u;
    if (c.a >= 255) {
        p[0] = (uint8_t)c.r;
        p[1] = (uint8_t)c.g;
        p[2] = (uint8_t)c.b;
        p[3] = 255;
        return;
    }
    ia = 255 - c.a;
    p[0] = (uint8_t)((c.r * c.a + (int)p[0] * ia + 127) / 255);
    p[1] = (uint8_t)((c.g * c.a + (int)p[1] * ia + 127) / 255);
    p[2] = (uint8_t)((c.b * c.a + (int)p[2] * ia + 127) / 255);
    p[3] = 255;
}

/* Clip an inclusive/exclusive integer rect to the surface.
 * Returns 0 when completely clipped away. */
static int clip_rect(Surface *s, int *x0, int *y0, int *x1, int *y1)
{
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > s->w) *x1 = s->w;
    if (*y1 > s->h) *y1 = s->h;
    return (*x0 < *x1) && (*y0 < *y1);
}

static void surface_fill(Surface *s, C8 c)
{
    size_t n = (size_t)s->w * (size_t)s->h;
    size_t i;
    for (i = 0; i < n; i++) {
        s->px[i * 4 + 0] = (uint8_t)c.r;
        s->px[i * 4 + 1] = (uint8_t)c.g;
        s->px[i * 4 + 2] = (uint8_t)c.b;
        s->px[i * 4 + 3] = 255;
    }
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

void gfx_init(void)
{
    memset(g_top_px, 0, sizeof(g_top_px));
    memset(g_bot_px, 0, sizeof(g_bot_px));
    /* opaque black start, not transparent black */
    surface_fill(&g_top, (C8){ 0, 0, 0, 255 });
    surface_fill(&g_bot, (C8){ 0, 0, 0, 255 });
    g_cur = &g_top;
    g_cur_screen = SCREEN_TOP;
    g_present_count = 0;
    g_req_frame = -1;
    g_written_frame = -1;
}

void gfx_shutdown(void)
{
    /* static buffers; nothing to release */
}

void gfx_begin_frame(void)
{
    g_cur = &g_top;
    g_cur_screen = SCREEN_TOP;
}

void gfx_target(Screen screen)
{
    if (screen == SCREEN_BOTTOM) {
        g_cur = &g_bot;
        g_cur_screen = SCREEN_BOTTOM;
    } else {
        g_cur = &g_top;
        g_cur_screen = SCREEN_TOP;
    }
}

/* Write both screens for `frame_no` as PNGs. */
static int write_frame_pngs(int frame_no)
{
    char path_top[1200];
    char path_bot[1200];
    int ok_top, ok_bot;

    if (frame_no < 0) return 0;
    snprintf(path_top, sizeof(path_top), "%s/frame_%04d_top.png",
             g_outdir, frame_no);
    snprintf(path_bot, sizeof(path_bot), "%s/frame_%04d_bottom.png",
             g_outdir, frame_no);

    ok_top = stbi_write_png(path_top, TOP_W, TOP_H, 4, g_top_px, TOP_W * 4);
    ok_bot = stbi_write_png(path_bot, BOT_W, BOT_H, 4, g_bot_px, BOT_W * 4);

    if (!ok_top) fprintf(stderr, "[host] FAILED to write %s\n", path_top);
    if (!ok_bot) fprintf(stderr, "[host] FAILED to write %s\n", path_bot);
    if (ok_top && ok_bot) {
        g_written_frame = frame_no;
        return 1;
    }
    return 0;
}

void gfx_present(void)
{
    g_present_count++;
    if (!g_capture_enabled || g_capture_defer) return;
    if (g_req_frame < 0 || g_req_frame == g_written_frame) return;
    (void)write_frame_pngs(g_req_frame);
}

/* ------------------------------------------------------------------ */
/* host capture control                                                */
/* ------------------------------------------------------------------ */

void host_capture_configure(const char *outdir, int enabled)
{
    if (outdir && outdir[0]) {
        size_t n = strlen(outdir);
        if (n >= sizeof(g_outdir)) n = sizeof(g_outdir) - 1;
        memcpy(g_outdir, outdir, n);
        g_outdir[n] = '\0';
        /* strip trailing slashes / backslashes */
        while (n > 1 && (g_outdir[n - 1] == '/' || g_outdir[n - 1] == '\\')) {
            g_outdir[--n] = '\0';
        }
    }
    g_capture_enabled = enabled ? 1 : 0;
}

void host_capture_set_defer(int defer)
{
    g_capture_defer = defer ? 1 : 0;
}

void host_capture_request(int frame_no)
{
    if (!g_capture_enabled) return;
    g_req_frame = frame_no;
}

int host_capture_finish(int frame_no)
{
    if (!g_capture_enabled) return 0;
    if (frame_no < 0) frame_no = g_req_frame;
    if (frame_no < 0 || frame_no == g_written_frame) return 0;
    return write_frame_pngs(frame_no);
}

int host_present_count(void)
{
    return g_present_count;
}

int host_last_captured_frame(void)
{
    return g_written_frame;
}

static const Surface *screen_surface(int screen)
{
    return (screen == 1) ? &g_bot : &g_top;
}

uint32_t host_screen_signature(int screen)
{
    const Surface *s = screen_surface(screen);
    const uint8_t *p = s->px;
    size_t n = (size_t)s->w * (size_t)s->h * 4u;
    uint32_t h = 2166136261u;   /* FNV-1a */
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    return h;
}

int host_screen_distinct_colors(int screen)
{
    const Surface *s = screen_surface(screen);
    const uint8_t *p = s->px;
    size_t npx = (size_t)s->w * (size_t)s->h;
    enum { BITS = 17, TABLE = 1 << BITS };
    static uint32_t seen[TABLE];
    static uint8_t used[TABLE];
    size_t i;
    int count = 0;

    memset(used, 0, sizeof used);
    for (i = 0; i < npx; i++) {
        uint32_t key = ((uint32_t)p[i * 4 + 0] << 16) |
                       ((uint32_t)p[i * 4 + 1] << 8) |
                       ((uint32_t)p[i * 4 + 2]);
        uint32_t slot = (key * 2654435761u) >> (32 - BITS);
        while (used[slot]) {
            if (seen[slot] == key)
                break;
            slot = (slot + 1u) & (TABLE - 1u);
        }
        if (!used[slot]) {
            used[slot] = 1;
            seen[slot] = key;
            ++count;
        }
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* drawing primitives                                                  */
/* ------------------------------------------------------------------ */

void gfx_clear(GfxColor color)
{
    surface_fill(g_cur, color8(color));
}

void gfx_fill_rect(float x, float y, float w, float h, GfxColor color)
{
    C8 c = color8(color);
    int x0, y0, x1, y1, px, py;

    if (c.a <= 0) return;
    if (w < 0.0f) { x += w; w = -w; }
    if (h < 0.0f) { y += h; h = -h; }
    if (w <= 0.0f || h <= 0.0f) return;

    x0 = iroundf(x);
    y0 = iroundf(y);
    x1 = iroundf(x + w);
    y1 = iroundf(y + h);
    if (!clip_rect(g_cur, &x0, &y0, &x1, &y1)) return;

    for (py = y0; py < y1; py++) {
        uint8_t *row = g_cur->px + ((size_t)py * (size_t)g_cur->w) * 4u;
        for (px = x0; px < x1; px++) {
            uint8_t *p = row + (size_t)px * 4u;
            if (c.a >= 255) {
                p[0] = (uint8_t)c.r;
                p[1] = (uint8_t)c.g;
                p[2] = (uint8_t)c.b;
                p[3] = 255;
            } else {
                int ia = 255 - c.a;
                p[0] = (uint8_t)((c.r * c.a + (int)p[0] * ia + 127) / 255);
                p[1] = (uint8_t)((c.g * c.a + (int)p[1] * ia + 127) / 255);
                p[2] = (uint8_t)((c.b * c.a + (int)p[2] * ia + 127) / 255);
                p[3] = 255;
            }
        }
    }
}

void gfx_outline_rect(float x, float y, float w, float h,
                      float thickness, GfxColor color)
{
    C8 c = color8(color);
    float x0f, y0f, x1f, y1f, t;
    int x0, y0, x1, y1, px, py;

    if (c.a <= 0) return;
    if (w < 0.0f) { x += w; w = -w; }
    if (h < 0.0f) { y += h; h = -h; }
    if (w <= 0.0f || h <= 0.0f) return;
    t = thickness;
    if (t <= 0.0f) return;
    if (t > w * 0.5f) t = w * 0.5f;
    if (t > h * 0.5f) t = h * 0.5f;
    if (t <= 0.0f) return;

    x0f = x;
    y0f = y;
    x1f = x + w;
    y1f = y + h;

    x0 = (int)floorf(x0f);
    y0 = (int)floorf(y0f);
    x1 = (int)ceilf(x1f);
    y1 = (int)ceilf(y1f);
    if (!clip_rect(g_cur, &x0, &y0, &x1, &y1)) return;

    for (py = y0; py < y1; py++) {
        float cy = (float)py + 0.5f;
        for (px = x0; px < x1; px++) {
            float cx = (float)px + 0.5f;
            if (cx < x0f + t || cx > x1f - t || cy < y0f + t || cy > y1f - t)
                blend_px(g_cur, px, py, c);
        }
    }
}

void gfx_fill_circle(float cx, float cy, float radius, GfxColor color)
{
    C8 c = color8(color);
    float r2;
    int x0, y0, x1, y1, px, py;

    if (c.a <= 0 || radius <= 0.0f) return;
    r2 = radius * radius;

    x0 = (int)floorf(cx - radius);
    y0 = (int)floorf(cy - radius);
    x1 = (int)ceilf(cx + radius);
    y1 = (int)ceilf(cy + radius);
    if (!clip_rect(g_cur, &x0, &y0, &x1, &y1)) return;

    for (py = y0; py < y1; py++) {
        float dy = (float)py + 0.5f - cy;
        for (px = x0; px < x1; px++) {
            float dx = (float)px + 0.5f - cx;
            if (dx * dx + dy * dy <= r2) blend_px(g_cur, px, py, c);
        }
    }
}

void gfx_outline_circle(float cx, float cy, float radius,
                        float thickness, GfxColor color)
{
    C8 c = color8(color);
    float r_out, r_in, ro2, ri2;
    int x0, y0, x1, y1, px, py;

    if (c.a <= 0 || radius <= 0.0f || thickness <= 0.0f) return;
    r_out = radius;
    r_in = radius - thickness;
    if (r_in < 0.0f) r_in = 0.0f;
    ro2 = r_out * r_out;
    ri2 = r_in * r_in;

    x0 = (int)floorf(cx - radius - 1.0f);
    y0 = (int)floorf(cy - radius - 1.0f);
    x1 = (int)ceilf(cx + radius + 1.0f);
    y1 = (int)ceilf(cy + radius + 1.0f);
    if (!clip_rect(g_cur, &x0, &y0, &x1, &y1)) return;

    for (py = y0; py < y1; py++) {
        float dy = (float)py + 0.5f - cy;
        for (px = x0; px < x1; px++) {
            float dx = (float)px + 0.5f - cx;
            float d2 = dx * dx + dy * dy;
            if (d2 <= ro2 && d2 >= ri2) blend_px(g_cur, px, py, c);
        }
    }
}

void gfx_line(float x1, float y1, float x2, float y2,
              float thickness, GfxColor color)
{
    C8 c = color8(color);
    float dx, dy, len2, half, rad2;
    int bx0, by0, bx1, by1, px, py;

    if (c.a <= 0) return;
    if (thickness <= 0.0f) thickness = 1.0f;
    half = thickness * 0.5f;
    if (half < 0.5f) half = 0.5f;
    rad2 = half * half;

    dx = x2 - x1;
    dy = y2 - y1;
    len2 = dx * dx + dy * dy;

    bx0 = (int)floorf(fminf(x1, x2) - half - 1.0f);
    by0 = (int)floorf(fminf(y1, y2) - half - 1.0f);
    bx1 = (int)ceilf(fmaxf(x1, x2) + half + 1.0f);
    by1 = (int)ceilf(fmaxf(y1, y2) + half + 1.0f);
    if (!clip_rect(g_cur, &bx0, &by0, &bx1, &by1)) return;

    for (py = by0; py < by1; py++) {
        float sy = (float)py + 0.5f;
        for (px = bx0; px < bx1; px++) {
            float sx = (float)px + 0.5f;
            float qx, qy, ex, ey;
            if (len2 <= 1e-9f) {
                qx = x1;
                qy = y1;
            } else {
                float t = (((sx - x1) * dx) + ((sy - y1) * dy)) / len2;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                qx = x1 + t * dx;
                qy = y1 + t * dy;
            }
            ex = sx - qx;
            ey = sy - qy;
            if (ex * ex + ey * ey <= rad2) blend_px(g_cur, px, py, c);
        }
    }
}

void gfx_fill_triangle(float x1, float y1, float x2, float y2,
                       float x3, float y3, GfxColor color)
{
    C8 c = color8(color);
    float area2, sign;
    int bx0, by0, bx1, by1, px, py;
    float ex1, ey1, ex2, ey2, ex3, ey3;

    if (c.a <= 0) return;

    area2 = (x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1);
    if (fabsf(area2) < 1e-6f) return;   /* degenerate */
    sign = (area2 > 0.0f) ? 1.0f : -1.0f;

    /* edge vectors */
    ex1 = x2 - x1; ey1 = y2 - y1;
    ex2 = x3 - x2; ey2 = y3 - y2;
    ex3 = x1 - x3; ey3 = y1 - y3;

    bx0 = (int)floorf(fminf(x1, fminf(x2, x3)));
    by0 = (int)floorf(fminf(y1, fminf(y2, y3)));
    bx1 = (int)ceilf(fmaxf(x1, fmaxf(x2, x3)));
    by1 = (int)ceilf(fmaxf(y1, fmaxf(y2, y3)));
    if (!clip_rect(g_cur, &bx0, &by0, &bx1, &by1)) return;

    for (py = by0; py < by1; py++) {
        float sy = (float)py + 0.5f;
        for (px = bx0; px < bx1; px++) {
            float sx = (float)px + 0.5f;
            float e1 = (ex1 * (sy - y1) - ey1 * (sx - x1)) * sign;
            float e2 = (ex2 * (sy - y2) - ey2 * (sx - x2)) * sign;
            float e3 = (ex3 * (sy - y3) - ey3 * (sx - x3)) * sign;
            if (e1 >= -1e-4f && e2 >= -1e-4f && e3 >= -1e-4f)
                blend_px(g_cur, px, py, c);
        }
    }
}

/* ------------------------------------------------------------------ */
/* text                                                                */
/* ------------------------------------------------------------------ */

static int text_scale(float scale)
{
    int s = iroundf(scale);
    if (s < 1) s = 1;
    return s;
}

void gfx_text(float x, float y, float scale, GfxColor color, const char *text)
{
    int s = text_scale(scale);
    int px = iroundf(x);
    int py = iroundf(y);
    const unsigned char *t;
    char glyph_advance = (char)(8 * s);

    if (!text) return;
    if (color8(color).a <= 0) return;

    for (t = (const unsigned char *)text; *t; ++t) {
        unsigned ch = (unsigned)*t;
        if (ch < 128u) {
            const unsigned char *rows = (const unsigned char *)font8x8_basic[ch];
            int gy;
            for (gy = 0; gy < 8; gy++) {
                unsigned bits = (unsigned)rows[gy];
                int gx;
                if (bits == 0u) continue;
                for (gx = 0; gx < 8; gx++) {
                    if (bits & (1u << (unsigned)gx)) {
                        gfx_fill_rect((float)(px + gx * s), (float)(py + gy * s),
                                      (float)s, (float)s, color);
                    }
                }
            }
        }
        px += (int)(unsigned char)glyph_advance;
    }
}

float gfx_text_width(const char *text, float scale)
{
    int s = text_scale(scale);
    size_t n = text ? strlen(text) : 0u;
    return (float)(n * (size_t)(8 * s));
}

float gfx_text_height(float scale)
{
    return (float)(8 * text_scale(scale));
}
