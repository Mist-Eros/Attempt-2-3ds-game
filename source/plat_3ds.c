/*
 * Elemental Defense - citro2d / citro3d rendering backend for the 3DS
 *
 * Implements the portable gfx.h drawing API on real 3DS hardware.  Together
 * with source/main.c this is the only file in the project allowed to include
 * <3ds.h> / <citro2d.h>; app.c, render.c, ui.c and audio.c stay portable so
 * the PC preview harness can render the same frames with a software backend.
 *
 * Batching notes:
 *  - citro2d automatically flushes its vertex batch when C2D_DEFAULT_MAX_OBJECTS
 *    is reached, so text-heavy screens (one small quad per font pixel run) are
 *    safe without manual C2D_Flush() calls.
 *  - every primitive is drawn at the same depth (0.5) and relies on draw order,
 *    which is what the 2D scene wants.
 *  - gfx_text renders the public domain 8x8 font from source/gfx/font8x8_basic.h
 *    one horizontal run of set bits per quad, at integer scales, so the result
 *    is crisp and identical to the PC preview.
 */
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "gfx.h"
#include "gfx/font8x8_basic.h"

#define GFX_DEPTH 0.5f
#define GFX_CIRCLE_SEGMENTS 20
#define GFX_TWO_PI 6.28318530718f

static C3D_RenderTarget *s_targets[2];
static Screen s_screen = SCREEN_TOP;

/* ------------------------------------------------------------------------ */
/* Colour conversion                                                         */
/* ------------------------------------------------------------------------ */

static u32 to_color(GfxColor c)
{
    float r = c.r;
    float g = c.g;
    float b = c.b;
    float a = c.a;

    if (r < 0.0f)
        r = 0.0f;
    if (r > 1.0f)
        r = 1.0f;
    if (g < 0.0f)
        g = 0.0f;
    if (g > 1.0f)
        g = 1.0f;
    if (b < 0.0f)
        b = 0.0f;
    if (b > 1.0f)
        b = 1.0f;
    if (a < 0.0f)
        a = 0.0f;
    if (a > 1.0f)
        a = 1.0f;

    return C2D_Color32((u8)(r * 255.0f + 0.5f), (u8)(g * 255.0f + 0.5f),
                       (u8)(b * 255.0f + 0.5f), (u8)(a * 255.0f + 0.5f));
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

void gfx_init(void)
{
    gfxInitDefault();

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    /* Both screens are drawn inside one frame and citro2d only recycles its
     * vertex/index buffers at frame end (C2D_SceneBegin flushes but does not
     * reset them).  Drawing routines silently fail when the buffer is full.
     * v0.3 layered enemies are the expensive case: a pathological frame with
     * 96 towers + all 128 enemy slots as 3-layer statused enemies + 128 beams
     * + 16 puddles measures ~12.2k quads in the host renderer, so the 4096
     * default is far too small.  16384 objects = 4x default (~2.3 MB of linear
     * memory, recycled every frame) leaves comfortable headroom. */
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 4);
    C2D_Prepare();

    s_targets[SCREEN_TOP] = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_targets[SCREEN_BOTTOM] = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    s_screen = SCREEN_TOP;
}

void gfx_shutdown(void)
{
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

void gfx_begin_frame(void)
{
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
}

void gfx_target(Screen screen)
{
    if (screen != SCREEN_TOP && screen != SCREEN_BOTTOM)
        return;
    if (s_targets[screen] == NULL)
        return;

    s_screen = screen;
    /* Important: do NOT call C3D_FrameDrawOn() before C2D_SceneBegin().  Begin
     * flushes the previous screen's pending vertex batch, and that flush has to
     * land on the target the vertices were drawn for.  Switching the render
     * target first sent the tail of the top screen's batch into the bottom
     * screen instead - which is why the top-screen menu used to lose the
     * forest glyph and the "press A" prompt. */
    C2D_SceneBegin(s_targets[screen]);
}

void gfx_present(void)
{
    /* Flush the last screen's pending batch before ending the frame; citro2d
     * only recycles its vertex buffer at frame end, so anything still queued
     * here would be submitted against the wrong target. */
    C2D_Flush();
    C3D_FrameEnd(0);
}

/* ------------------------------------------------------------------------ */
/* Primitives                                                                */
/* ------------------------------------------------------------------------ */

void gfx_clear(GfxColor color)
{
    float w = s_screen == SCREEN_TOP ? (float)TOP_W : (float)BOT_W;

    gfx_fill_rect(0.0f, 0.0f, w, (float)TOP_H, color);
}

void gfx_fill_rect(float x, float y, float w, float h, GfxColor color)
{
    if (w <= 0.0f || h <= 0.0f)
        return;
    C2D_DrawRectSolid(floorf(x), floorf(y), GFX_DEPTH, w, h, to_color(color));
}

void gfx_outline_rect(float x, float y, float w, float h, float thickness, GfxColor color)
{
    float t = thickness > 0.0f ? thickness : 1.0f;

    if (w <= 0.0f || h <= 0.0f)
        return;

    gfx_fill_rect(x, y, w, t, color);
    gfx_fill_rect(x, y + h - t, w, t, color);
    gfx_fill_rect(x, y, t, h, color);
    gfx_fill_rect(x + w - t, y, t, h, color);
}

void gfx_fill_circle(float cx, float cy, float radius, GfxColor color)
{
    if (radius <= 0.0f)
        return;
    C2D_DrawCircleSolid(cx, cy, GFX_DEPTH, radius, to_color(color));
}

void gfx_outline_circle(float cx, float cy, float radius, float thickness, GfxColor color)
{
    int i;
    float step = GFX_TWO_PI / (float)GFX_CIRCLE_SEGMENTS;

    if (radius <= 0.0f)
        return;

    for (i = 0; i < GFX_CIRCLE_SEGMENTS; i++) {
        float a0 = (float)i * step;
        float a1 = (float)(i + 1) * step;
        gfx_line(cx + cosf(a0) * radius, cy + sinf(a0) * radius,
                 cx + cosf(a1) * radius, cy + sinf(a1) * radius, thickness, color);
    }
}

void gfx_line(float x1, float y1, float x2, float y2, float thickness, GfxColor color)
{
    float t = thickness > 0.0f ? thickness : 1.0f;
    u32 c = to_color(color);

    /* C2D_DrawLine divides by the segment length; a degenerate segment would
     * produce NaN vertices. */
    if (x1 == x2 && y1 == y2)
        return;

    C2D_DrawLine(x1, y1, c, x2, y2, c, t, GFX_DEPTH);
}

void gfx_fill_triangle(float x1, float y1, float x2, float y2, float x3, float y3,
                       GfxColor color)
{
    u32 c = to_color(color);

    C2D_DrawTriangle(x1, y1, c, x2, y2, c, x3, y3, c, GFX_DEPTH);
}

/* ------------------------------------------------------------------------ */
/* 8x8 bitmap text                                                           */
/* ------------------------------------------------------------------------ */

void gfx_text(float x, float y, float scale, GfxColor color, const char *text)
{
    const char *p;
    float px;
    float py;
    u32 packed;

    if (text == NULL || scale <= 0.0f)
        return;

    packed = to_color(color);
    px = floorf(x);
    py = floorf(y);

    for (p = text; *p != '\0'; p++) {
        unsigned char ch = (unsigned char)*p;

        if (ch < 128) {
            /* font8x8_basic[ch][row] has bit 0 as the leftmost pixel */
            const unsigned char *glyph = (const unsigned char *)font8x8_basic[ch];
            int row;

            for (row = 0; row < 8; row++) {
                unsigned bits = glyph[row];
                int col = 0;

                while (col < 8) {
                    int start;

                    if ((bits & (1u << col)) == 0) {
                        col++;
                        continue;
                    }

                    /* coalesce horizontal runs into a single quad */
                    start = col;
                    while (col < 8 && (bits & (1u << col)) != 0)
                        col++;

                    C2D_DrawRectSolid(px + (float)start * scale,
                                      py + (float)row * scale, GFX_DEPTH,
                                      (float)(col - start) * scale, scale, packed);
                }
            }
        }

        px += 8.0f * scale;
    }
}

float gfx_text_width(const char *text, float scale)
{
    if (text == NULL)
        return 0.0f;
    return 8.0f * scale * (float)strlen(text);
}

float gfx_text_height(float scale)
{
    return 8.0f * scale;
}
