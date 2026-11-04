/*
 * Elemental Defense - top screen rendering + shared pixel-art helpers
 *
 * render.c owns the battlefield (top screen): background, path, grid, towers,
 * enemies, beams, HUD strip, cursor, range previews and the menu / pause /
 * result overlays.  The same file also implements the small pixel-art element
 * icons that the bottom-screen UI reuses, so both screens look like one game.
 *
 * Everything here is portable C99 and only talks to gfx.h, exactly like
 * app.c and ui.c, so the PC preview harness can render the same frames.
 *
 * Board geometry: the 26x13 board is drawn at 15 px per cell, i.e. 390x195 px,
 * anchored at (5, 38) on the 400x240 top screen.  That leaves a 38 px HUD strip
 * at the top and a 7 px margin at the bottom.  Cell space (see game.h) maps to
 * top-screen pixels with `BOARD_X + cellX * BOARD_CELL`.
 */
#ifndef RENDER_H
#define RENDER_H

#include "app.h"
#include "game.h"
#include "gfx.h"

/* ---- board geometry on the 400x240 top screen ---- */
#define BOARD_CELL 15.0f
#define BOARD_X 5.0f
#define BOARD_Y 38.0f
#define BOARD_W ((float)GRID_COLS * BOARD_CELL)     /* 390 */
#define BOARD_H ((float)GRID_ROWS * BOARD_CELL)     /* 195 */
#define HUD_H BOARD_Y                               /* top HUD strip height */

/* ---- palette -----------------------------------------------------------
 * Tuned for the 240p, glossy, often-outdoors 3DS LCD: every colour used for
 * actual TEXT sits at >= ~0.75 perceived luminance against the dark board, and
 * hierarchy is carried by size/weight/colour hue instead of dimming.  The
 * "muted"/"dim" names are historical; both are now light, they just differ in
 * emphasis.  Shape-only colours (path, cell, grid, glows) keep the dark look. */
static const GfxColor COL_WHITE      = { 1.00f, 1.00f, 1.00f, 1.00f };
static const GfxColor COL_PANEL      = { 0.05f, 0.05f, 0.12f, 0.96f };
static const GfxColor COL_PANEL_SOLID= { 0.05f, 0.05f, 0.12f, 1.00f };
static const GfxColor COL_BORDER     = { 0.22f, 0.22f, 0.42f, 0.65f };
static const GfxColor COL_BORDER_HI  = { 0.45f, 0.45f, 0.70f, 0.80f };
static const GfxColor COL_GOLD       = { 1.00f, 0.82f, 0.24f, 1.00f };
static const GfxColor COL_GOLD_SOFT  = { 1.00f, 0.92f, 0.45f, 1.00f };
static const GfxColor COL_MUTED      = { 0.86f, 0.88f, 0.96f, 0.95f };  /* secondary text */
static const GfxColor COL_DIM        = { 0.80f, 0.82f, 0.92f, 0.92f };  /* tertiary text */
static const GfxColor COL_RED        = { 1.00f, 0.40f, 0.36f, 1.00f };
static const GfxColor COL_GREEN      = { 0.35f, 1.00f, 0.42f, 1.00f };
static const GfxColor COL_PATH       = { 0.26f, 0.15f, 0.06f, 1.00f };
static const GfxColor COL_PATH_EDGE  = { 0.40f, 0.27f, 0.11f, 0.90f };
static const GfxColor COL_CELL       = { 0.05f, 0.05f, 0.08f, 1.00f };
static const GfxColor COL_GRIDLINE   = { 0.14f, 0.14f, 0.22f, 0.30f };
static const GfxColor COL_YELLOW     = { 1.00f, 1.00f, 0.35f, 1.00f };
static const GfxColor COL_BUILD_TAG  = { 1.00f, 1.00f, 0.45f, 1.00f };
static const GfxColor COL_WAVE_TAG   = { 0.50f, 1.00f, 0.55f, 1.00f };

/* ---- v0.3 damage model: layer + damage-type colours --------------------
 * Layer colours are fixed by docs/elements-spec.md §1; damage-type colours
 * drive beams, tower icons and palette highlights (§2/§3).  *_TEXT variants are
 * lightened for actual glyphs so they stay legible on the 240p LCD. */
static const GfxColor COL_LAYER[MAX_LAYERS] = {
    { 0.95f, 0.18f, 0.18f, 1.00f },   /* LAYER_HP     red    */
    { 0.28f, 0.55f, 1.00f, 1.00f },   /* LAYER_SHIELD blue   */
    { 0.95f, 0.78f, 0.18f, 1.00f }    /* LAYER_ARMOR  yellow */
};
static const GfxColor COL_LAYER_TEXT[MAX_LAYERS] = {
    { 1.00f, 0.60f, 0.56f, 1.00f },
    { 0.62f, 0.80f, 1.00f, 1.00f },
    { 1.00f, 0.90f, 0.52f, 1.00f }
};
static const char *const LAYER_NAMES[MAX_LAYERS] = { "HP", "SHIELD", "ARMOR" };

static const GfxColor COL_DAMAGE_TYPE[TOWER_TYPE_COUNT] = {
    { 0.92f, 0.92f, 0.88f, 1.00f },   /* DMG_NORMAL    off-white */
    { 1.00f, 0.45f, 0.12f, 1.00f },   /* DMG_FIRE      orange    */
    { 0.25f, 0.85f, 1.00f, 1.00f },   /* DMG_ELECTRIC  cyan      */
    { 0.55f, 1.00f, 0.20f, 1.00f },   /* DMG_CORROSIVE acid green*/
    { 0.78f, 0.35f, 1.00f, 1.00f },   /* DMG_SLAG      purple    */
    { 1.00f, 0.65f, 0.10f, 1.00f }    /* DMG_EXPLOSIVE amber     */
};
static const GfxColor COL_DAMAGE_TEXT[TOWER_TYPE_COUNT] = {
    { 0.95f, 0.95f, 0.92f, 1.00f },   /* normal    */
    { 1.00f, 0.74f, 0.52f, 1.00f },   /* fire      */
    { 0.62f, 0.93f, 1.00f, 1.00f },   /* electric  */
    { 0.72f, 1.00f, 0.58f, 1.00f },   /* corrosive */
    { 0.90f, 0.70f, 1.00f, 1.00f },   /* slag      */
    { 1.00f, 0.84f, 0.52f, 1.00f }    /* explosive */
};
static const GfxColor COL_PUDDLE      = { 0.40f, 0.85f, 0.14f, 1.00f };
static const GfxColor COL_PUDDLE_RIM  = { 0.78f, 1.00f, 0.40f, 1.00f };
static const GfxColor COL_BURN_MOTE   = { 1.00f, 0.58f, 0.12f, 1.00f };
static const GfxColor COL_STUN_SPARK  = { 0.92f, 0.97f, 1.00f, 1.00f };

/* ---- Borderlands-style weapon emblems ----------------------------------
 * Reference shapes only (drawn from scratch, no image files): incendiary
 * flame, three-bolt shock fan, corrosive trefoil-over-ring, slag wave, gold
 * explosive starburst.  Colours follow the BL2 emblems. */
static const GfxColor COL_EMBLEM[TOWER_TYPE_COUNT] = {
    { 0.80f, 0.80f, 0.84f, 1.00f },   /* GUN       grey          */
    { 1.00f, 0.40f, 0.00f, 1.00f },   /* FIRE      #FF6600       */
    { 0.00f, 0.39f, 0.88f, 1.00f },   /* SHOCK     #0064E0       */
    { 0.00f, 0.83f, 0.00f, 1.00f },   /* CORROSIVE #00D400       */
    { 0.71f, 0.24f, 1.00f, 1.00f },   /* SLAG      #B43CFF       */
    { 1.00f, 0.82f, 0.00f, 1.00f }    /* EXPLOSIVE #FFD200       */
};
static const GfxColor COL_EMBLEM_DARK[TOWER_TYPE_COUNT] = {
    { 0.38f, 0.38f, 0.44f, 1.00f },   /* gun body / driving band */
    { 0.78f, 0.22f, 0.00f, 1.00f },   /* flame inner shadow      */
    { 0.45f, 0.75f, 1.00f, 1.00f },   /* bolt highlight          */
    { 0.09f, 0.47f, 0.15f, 1.00f },   /* corrosive ring          */
    { 0.90f, 0.55f, 1.00f, 1.00f },   /* slag highlight          */
    { 1.00f, 0.40f, 0.00f, 1.00f }    /* explosive shadow        */
};
static const GfxColor COL_MENU_INK   = { 0.02f, 0.02f, 0.05f, 1.00f };

/* ------------------------------------------------------------------------ */
/* Screens                                                                   */
/* ------------------------------------------------------------------------ */

/* Draws the complete top screen for the current phase, including menu, pause
 * and result overlays.  Prefer this entry point; it is what app_draw calls. */
void render_top(const App *app);

/* Lower level pieces.  They do NOT clear the screen, on the assumption that
 * the caller (render_top) has already painted the gradient background. */
void render_menu_top(const App *app);
void render_game_top(const App *app);
void render_pause_overlay_top(void);
void render_result_overlay_top(const App *app);

/* Paints the dark vertical gradient used by every screen. */
void render_background_gradient(int width, int height);

/* ------------------------------------------------------------------------ */
/* Shared pixel-art helpers (used by render.c and ui.c)                       */
/* ------------------------------------------------------------------------ */

/* Cell space -> top-screen pixels. */
float cell_px_x(float cellX);
float cell_px_y(float cellY);
float board_cell_left(int col);
float board_cell_top(int row);
/* Top-screen pixels -> board cell.  Returns false when (px,py) is outside the
 * board, otherwise writes col/row (already clamped to the grid if outside by
 * a fraction of a cell is handled by the caller). */
bool board_hit_cell(float px, float py, int *col, int *row);

/* Borderlands-style weapon emblem, one per DamageType (0..5): bullet, flame,
 * three-bolt fan, biohazard trefoil, slag wave, explosive starburst.
 * `cx/cy` is the centre, `size` is roughly half the height in pixels.  Two
 * detail levels are picked automatically: size >= 12 draws the detailed version
 * (menus), smaller sizes draw a simplified glyph (palette, board towers).
 * Colours come from COL_EMBLEM/COL_EMBLEM_DARK. */
void art_weapon_emblem(float cx, float cy, float size, int damageType);

/* Generic art bits still used by menus / previews. */
void art_star(float cx, float cy, float size, GfxColor color);

void art_coin(float cx, float cy, float radius);
void art_heart(float cx, float cy, float size, GfxColor color);
void art_glow(float cx, float cy, float radius, GfxColor color, float alpha);

/* Chunky comic-style button dressing shared by both menu screens:
 * - text_comic_centered: 8-direction near-black outline + drop shadow + fill
 *   (the BL2 title look).  Costs ~10 text passes: fine for the light menu
 *   frame, never used on the battlefield.
 * - art_hazard_strip: black block with yellow/black diagonal hazard stripes,
 *   used at the ends of menu buttons. */
void text_comic_centered(float cx, float y, float scale, GfxColor fill,
                         const char *text);
void art_hazard_strip(float x, float y, float w, float h);

/* Colour tables, clamped to range; alpha is applied to the result. */
GfxColor art_damage_color(int damageType, float alpha);
GfxColor art_damage_text_color(int damageType);
GfxColor art_layer_color(int layerType, float alpha);
GfxColor art_layer_text_color(int layerType);
GfxColor art_emblem_color(int damageType, float alpha);

/* Health bar centred on cx; y is the top edge.  frac in 0..1, green -> red. */
void art_health_bar(float cx, float y, float width, float height, float frac);

/* Text helpers (8x8 font, integer scale).  Fractions are floored so glyphs
 * always land on whole pixels. */
void text_centered(float cx, float y, float scale, GfxColor color, const char *text);
void text_right(float rightX, float y, float scale, GfxColor color, const char *text);

/* Converts a TowerSpec colour triplet to a GfxColor. */
GfxColor art_spec_color(const float rgb[3], float alpha);

#endif /* RENDER_H */
