/*
 * Elemental Defense - minimal 2D drawing abstraction
 *
 * The game's rendering code only talks to this API, which has two backends:
 *   - source/plat_3ds.c   citro2d / citro3d  (the real thing, 3DS hardware)
 *   - tools/plat_host.c   software rasterizer writing BMP frames (PC preview)
 *
 * Screen coordinates are in pixels, origin top-left, +y downwards.
 * The top screen is 400x240, the bottom (touch) screen is 320x240.
 */
#ifndef GFX_H
#define GFX_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SCREEN_TOP = 0,
    SCREEN_BOTTOM = 1
} Screen;

#define TOP_W 400
#define TOP_H 240
#define BOT_W 320
#define BOT_H 240

typedef struct {
    float r, g, b, a;   /* 0..1 */
} GfxColor;

static inline GfxColor gfx_rgba(float r, float g, float b, float a) {
    GfxColor c = { r, g, b, a };
    return c;
}
static inline GfxColor gfx_rgb(float r, float g, float b) {
    GfxColor c = { r, g, b, 1.0f };
    return c;
}

/* ---- lifecycle ---- */
void gfx_init(void);
void gfx_shutdown(void);

/* One frame: begin_frame() -> target()/draw calls for each screen -> present() */
void gfx_begin_frame(void);
void gfx_target(Screen screen);     /* select which screen following draws go to */
void gfx_present(void);             /* display the finished frame */

/* ---- drawing ---- */
void gfx_clear(GfxColor color);
void gfx_fill_rect(float x, float y, float w, float h, GfxColor color);
void gfx_outline_rect(float x, float y, float w, float h, float thickness, GfxColor color);
void gfx_fill_circle(float cx, float cy, float radius, GfxColor color);
void gfx_outline_circle(float cx, float cy, float radius, float thickness, GfxColor color);
void gfx_line(float x1, float y1, float x2, float y2, float thickness, GfxColor color);
void gfx_fill_triangle(float x1, float y1, float x2, float y2, float x3, float y3, GfxColor color);

/* 8x8 bitmap font (see source/gfx/font8x8_basic.h). scale is usually 1 or 2.
 * (x,y) is the top-left of the first glyph. newlines are not supported. */
void gfx_text(float x, float y, float scale, GfxColor color, const char *text);
float gfx_text_width(const char *text, float scale);   /* width in pixels */
float gfx_text_height(float scale);                    /* height in pixels */

#endif /* GFX_H */
