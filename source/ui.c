/*
 * Elemental Defense - bottom screen (touch) UI
 *
 * The top screen is now a pure battlefield: every persistent number lives on
 * this screen.  The design keeps the BL2 comic identity but rounds it off into
 * a warmer "toy" look: chunky rounded panels, pastel stat chips, big element
 * emblems and a tiny mascot blob in the bottom-right corner that reacts to the
 * run (happy hop on a build, worried wobble on a leak, pale when lives run low).
 *
 * Layout (320x240, origin top-left):
 *   4   .. 32    stat chips: gold | lives | WAVE x/1000 | towers | speed | pause
 *   36  .. 93    2x3 tower palette (100x27 rounded cells)
 *   97  .. 153   selected-tower / cursor panel with UPGRADE + SELL
 *   157 .. 233   NEXT WAVE + INCOMING (build)  /  wave progress + stats (wave)
 *   284 .. 316   mascot corner (bottom-right, outside the bottom panels)
 *
 * Every interactive element keeps its own ui_*_rect() accessor so the drawing
 * code, the hit test and the PC preview harness can never drift apart.  The
 * names are unchanged from v0.6.0 so the preview scenarios keep working.
 *
 * Maze building: towers may stand on the road, and the core re-runs a BFS after
 * every build/sell, so a buildable road cell is a positive hint ("ROAD - BUILD
 * HERE TO REROUTE THEM").  Only the fixed entrance/exit cells and placements
 * that would seal the exit are refused by game_can_place_at(); the panel shows
 * "ROAD - THIS CELL CAN'T BE BLOCKED" for those.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"

#include "game.h"
#include "render.h"

/* ------------------------------------------------------------------------ */
/* Layout                                                                    */
/* ------------------------------------------------------------------------ */

#define CHIP_Y 4.0f
#define CHIP_H 28.0f
#define PALETTE_Y 36.0f
#define PALETTE_ROW_H 27.0f
#define PALETTE_ROW_GAP 3.0f
#define PANEL_X 6.0f
#define PANEL_Y 97.0f
#define PANEL_W 308.0f
#define PANEL_H 56.0f
#define BOTTOM_Y 157.0f
#define BOTTOM_H 76.0f

/* Mascot corner: bottom-right, clear of the bottom panels (which end at 282). */
#define MASCOT_X 300.0f
#define MASCOT_Y 214.0f
#define MASCOT_R 12.0f

bool ui_rect_contains(UiRect r, float x, float y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

UiRect ui_start_rect(void)
{
    UiRect r = { 44.0f, 126.0f, 232.0f, 64.0f };
    return r;
}

/* Six tower types in a 2x3 grid of 100x27 rounded cells. */
UiRect ui_tower_button_rect(int type)
{
    UiRect r;

    if (type < 0)
        type = 0;
    if (type >= TOWER_TYPE_COUNT)
        type = TOWER_TYPE_COUNT - 1;

    r.w = 100.0f;
    r.h = PALETTE_ROW_H;
    r.x = 6.0f + (float)(type % 3) * 104.0f;
    r.y = PALETTE_Y + (float)(type / 3) * (PALETTE_ROW_H + PALETTE_ROW_GAP);
    return r;
}

/* Buttons live in the bottom row of the selected-tower panel. */
UiRect ui_upgrade_rect(void)
{
    UiRect r = { 8.0f, 128.0f, 150.0f, 24.0f };
    return r;
}

UiRect ui_sell_rect(void)
{
    UiRect r = { 164.0f, 128.0f, 148.0f, 24.0f };
    return r;
}

UiRect ui_next_wave_rect(void)
{
    UiRect r = { 6.0f, BOTTOM_Y, 160.0f, BOTTOM_H };
    return r;
}

/* Header chips: the speed and pause pills are the single touch controls for
 * those two actions in every play phase. */
UiRect ui_speed_rect(void)
{
    UiRect r = { 252.0f, CHIP_Y, 34.0f, CHIP_H };
    return r;
}

UiRect ui_pause_rect(void)
{
    UiRect r = { 289.0f, CHIP_Y, 28.0f, CHIP_H };
    return r;
}

/* Kept as aliases so the preview harness / earlier callers do not break. */
UiRect ui_wave_speed_rect(void)
{
    return ui_speed_rect();
}

UiRect ui_wave_pause_rect(void)
{
    return ui_pause_rect();
}

UiRect ui_resume_rect(void)
{
    UiRect r = { 60.0f, 104.0f, 200.0f, 52.0f };
    return r;
}

UiRect ui_pause_menu_rect(void)
{
    UiRect r = { 100.0f, 168.0f, 120.0f, 28.0f };
    return r;
}

UiRect ui_retry_rect(void)
{
    UiRect r = { 40.0f, 140.0f, 240.0f, 56.0f };
    return r;
}

UiRect ui_result_menu_rect(void)
{
    UiRect r = { 80.0f, 204.0f, 160.0f, 28.0f };
    return r;
}

/* ------------------------------------------------------------------------ */
/* Cute palette + rounded drawing helpers                                    */
/* ------------------------------------------------------------------------ */

static const GfxColor COL_CHIP_GOLD  = { 1.00f, 0.84f, 0.38f, 1.00f };
static const GfxColor COL_CHIP_PINK  = { 1.00f, 0.62f, 0.70f, 1.00f };
static const GfxColor COL_CHIP_SKY   = { 0.56f, 0.84f, 1.00f, 1.00f };
static const GfxColor COL_CHIP_MINT  = { 0.62f, 0.96f, 0.70f, 1.00f };
static const GfxColor COL_CHIP_LILAC = { 0.76f, 0.72f, 1.00f, 1.00f };
static const GfxColor COL_CHIP_LEMON = { 1.00f, 0.92f, 0.52f, 1.00f };
static const GfxColor COL_PANEL_BG   = { 0.055f, 0.060f, 0.115f, 0.97f };
static const GfxColor COL_PANEL_EDGE = { 0.20f, 0.21f, 0.36f, 1.00f };

static GfxColor rgba(float r, float g, float b, float a)
{
    return gfx_rgba(r, g, b, a);
}

/* Mixes a colour towards white (used to make pastel trims from the saturated
 * element/damage colours without adding another table). */
static GfxColor pastel(GfxColor c, float amount)
{
    return rgba(c.r + (1.0f - c.r) * amount,
                c.g + (1.0f - c.g) * amount,
                c.b + (1.0f - c.b) * amount, c.a);
}

/* Rounded rectangle: centre bars + four quarter-circle corners.  Cheap enough
 * for the bottom screen (every panel is a handful of primitives). */
static void fill_round_rect(float x, float y, float w, float h, float r,
                            GfxColor color)
{
    if (r < 0.0f)
        r = 0.0f;
    if (r > w * 0.5f)
        r = w * 0.5f;
    if (r > h * 0.5f)
        r = h * 0.5f;

    gfx_fill_rect(x + r, y, w - 2.0f * r, h, color);
    gfx_fill_rect(x, y + r, w, h - 2.0f * r, color);
    if (r > 0.5f) {
        gfx_fill_circle(x + r, y + r, r, color);
        gfx_fill_circle(x + w - r, y + r, r, color);
        gfx_fill_circle(x + r, y + h - r, r, color);
        gfx_fill_circle(x + w - r, y + h - r, r, color);
    }
}

/* Chunky toy panel: pastel border ring, dark body, soft top highlight and a
 * couple of corner studs so it reads as physically rounded. */
static void cute_panel(float x, float y, float w, float h, float r,
                       GfxColor trim)
{
    fill_round_rect(x, y, w, h, r, trim);
    fill_round_rect(x + 2.0f, y + 2.0f, w - 4.0f, h - 4.0f,
                    r > 2.5f ? r - 1.5f : r, COL_PANEL_BG);
    gfx_fill_rect(x + r + 4.0f, y + 2.5f, w - 2.0f * r - 8.0f, 1.0f,
                  rgba(1.0f, 1.0f, 1.0f, 0.12f));
    gfx_fill_rect(x + r + 4.0f, y + h - 3.0f, w - 2.0f * r - 8.0f, 1.0f,
                  rgba(0.0f, 0.0f, 0.0f, 0.35f));
    gfx_fill_circle(x + 5.0f, y + 5.0f, 1.3f, trim);
    gfx_fill_circle(x + w - 5.0f, y + 5.0f, 1.3f, trim);
}

/* Rounded chip with a pastel ring and a dark body: the stat pills and the
 * small status labels use this. */
static void cute_chip(float x, float y, float w, float h, GfxColor trim)
{
    fill_round_rect(x, y, w, h, h * 0.5f, trim);
    fill_round_rect(x + 1.5f, y + 1.5f, w - 3.0f, h - 3.0f,
                    h * 0.5f - 1.0f, COL_PANEL_BG);
    gfx_fill_rect(x + 6.0f, y + 2.5f, w - 12.0f, 1.0f,
                  rgba(1.0f, 1.0f, 1.0f, 0.14f));
}

/* Chunky rounded action button: coloured ring + tinted body + tiny highlight. */
static void cute_button(UiRect r, GfxColor trim, GfxColor body, float pulse)
{
    if (pulse > 0.0f)
        art_glow(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.55f, trim,
                 0.06f + 0.06f * pulse);
    fill_round_rect(r.x, r.y, r.w, r.h, 10.0f, trim);
    fill_round_rect(r.x + 2.0f, r.y + 2.0f, r.w - 4.0f, r.h - 4.0f, 8.0f,
                    body);
    gfx_fill_rect(r.x + 10.0f, r.y + 3.0f, r.w - 20.0f, 2.0f,
                  rgba(1.0f, 1.0f, 1.0f, 0.18f));
    gfx_fill_rect(r.x + 10.0f, r.y + r.h - 4.0f, r.w - 20.0f, 1.0f,
                  rgba(0.0f, 0.0f, 0.0f, 0.35f));
}

/* A tiny flat tower used by the stat chips. */
static void tiny_tower_icon(float cx, float cy, float s, GfxColor color)
{
    gfx_fill_rect(cx - s * 0.9f, cy + s * 0.2f, s * 1.8f, s * 0.7f, color);
    gfx_fill_rect(cx - s * 0.5f, cy - s * 0.5f, s * 1.0f, s * 0.7f, color);
    gfx_fill_triangle(cx - s * 0.7f, cy - s * 0.5f, cx + s * 0.7f, cy - s * 0.5f,
                      cx, cy - s * 1.2f, color);
}

/* ------------------------------------------------------------------------ */
/* Shared UX helpers                                                         */
/* ------------------------------------------------------------------------ */

/* The tower index shown in the info panel: the explicitly selected tower wins,
 * otherwise whatever sits under the D-pad cursor. */
static int panel_tower_index(const App *app)
{
    const Game *g = &app->game;

    if (g->selectedTower >= 0 && g->selectedTower < g->towerCount &&
        g->towers[g->selectedTower].active)
        return g->selectedTower;
    return game_tower_at(g, g->cursorCol, g->cursorRow);
}

static bool cell_is_path(const Game *g, int col, int row)
{
    int i;

    for (i = 0; i < g->path.count; i++) {
        int pc = (int)floorf(g->path.pts[i].x);
        int pr = (int)floorf(g->path.pts[i].y);
        if (pc == col && pr == row)
            return true;
    }
    return false;
}

static void speed_text(float multiplier, char *buf, size_t size)
{
    snprintf(buf, size, "%.0fX", (double)multiplier);
}

/* Short description of a damage type's signature effect (elements-spec 2).
 * Plain letters only: the 8x8 font has no middot/arrow glyphs. */
static void tower_effect_text(int damageType, char *buf, size_t size)
{
    switch (damageType) {
    case DMG_FIRE:
        snprintf(buf, size, "BURNS - 2X VS HP");
        break;
    case DMG_ELECTRIC:
        snprintf(buf, size, "STUNS - 2X VS SHIELD");
        break;
    case DMG_CORROSIVE:
        snprintf(buf, size, "ACID PUDDLE - 2X VS ARMOR");
        break;
    case DMG_SLAG:
        snprintf(buf, size, "SLAG: +50%% DMG TAKEN");
        break;
    case DMG_EXPLOSIVE:
        snprintf(buf, size, "AOE 1.3 - 3X SLOWER");
        break;
    case DMG_NORMAL:
    default:
        snprintf(buf, size, "NO SPECIAL EFFECT");
        break;
    }
}

static const char *const kTowerNames[TOWER_TYPE_COUNT] = {
    "GUN", "FIRE", "SHOCK", "CORROSIVE", "SLAG", "EXPLOSIVE"
};

static const char *tower_display_name(int damageType)
{
    if (damageType < 0)
        damageType = 0;
    if (damageType >= TOWER_TYPE_COUNT)
        damageType = TOWER_TYPE_COUNT - 1;
    return kTowerNames[damageType];
}

/* One row of the "incoming" panel: a coloured layer dot + "LABEL xCOUNT".
 * The elite row uses the gold star instead of a layer dot. */
static void draw_preview_row(float x, float y, int count, int layerType,
                             bool elite)
{
    char buf[24];
    GfxColor color;

    if (elite) {
        art_star(x + 8.0f, y + 4.0f, 4.0f, COL_GOLD);
        color = COL_GOLD_SOFT;
    } else {
        if (layerType < 0 || layerType >= MAX_LAYERS)
            layerType = 0;
        color = pastel(art_layer_text_color(layerType), 0.10f);
        gfx_fill_circle(x + 8.0f, y + 4.0f, 3.2f,
                        art_layer_color(layerType, 0.95f));
        gfx_outline_circle(x + 8.0f, y + 4.0f, 3.9f, 1.0f,
                           rgba(0.0f, 0.0f, 0.0f, 0.70f));
    }

    snprintf(buf, sizeof(buf), "%s x%d", elite ? "ELITE" : LAYER_NAMES[layerType],
             count);
    gfx_text(x + 16.0f, y, 1.0f, color, buf);
}

/* ------------------------------------------------------------------------ */
/* Mascot blob                                                               */
/* ------------------------------------------------------------------------ */

enum {
    MASCOT_IDLE = 0,
    MASCOT_HAPPY,
    MASCOT_WORRIED,
    MASCOT_PALE
};

/* A small procedural slime: idle bounce + blink + looking around; happy hop
 * with sparkles when a tower is built; worried wobble with a sweat drop when a
 * creep leaks; pale, wide-eyed and shivering when lives run low. */
static void draw_mascot(const App *app, float x, float y, float r, int mood)
{
    const float t = app->time;
    float bob;
    float hop = 0.0f;
    float jitter = 0.0f;
    float blink;
    float squash;
    GfxColor body;
    GfxColor outline = rgba(0.02f, 0.05f, 0.04f, 1.0f);
    float look;
    int i;

    if (mood == MASCOT_HAPPY && app->mascotHappy > 0.0f) {
        float p = 1.0f - app->mascotHappy / 1.0f;
        if (p < 0.0f)
            p = 0.0f;
        if (p > 1.0f)
            p = 1.0f;
        hop = -fabsf(sinf(p * 3.14159f)) * (r * 0.55f);
    }
    if (mood == MASCOT_WORRIED)
        jitter = sinf(t * 34.0f) * 1.4f;

    bob = sinf(t * 2.2f) * 1.5f;
    blink = fmodf(t + 0.35f, 3.1f) < 0.12f ? 1.0f : 0.0f;
    if (mood == MASCOT_PALE)
        blink = 0.0f;
    squash = 1.0f + 0.05f * sinf(t * 2.2f + 1.2f);

    if (mood == MASCOT_PALE)
        body = rgba(0.55f, 0.62f, 0.60f, 1.0f);
    else if (mood == MASCOT_WORRIED)
        body = rgba(0.58f, 0.80f, 0.62f, 1.0f);
    else
        body = rgba(0.52f, 0.94f, 0.66f, 1.0f);

    x += jitter;
    y += bob + hop;

    /* shadow + body (outline pass first, then the body on top) */
    gfx_fill_circle(x, y + r * 1.00f, r * 0.86f, rgba(0.0f, 0.0f, 0.0f, 0.30f));
    gfx_fill_circle(x, y + r * 0.95f - bob * 0.2f, r * 0.72f,
                    rgba(0.0f, 0.0f, 0.0f, 0.24f));

    gfx_fill_circle(x, y - r * 0.10f, (r + 1.7f) * squash, outline);
    gfx_fill_circle(x - r * 0.55f, y + r * 0.42f, r * 0.74f + 1.7f, outline);
    gfx_fill_circle(x + r * 0.55f, y + r * 0.42f, r * 0.74f + 1.7f, outline);
    gfx_fill_rect(x - r * 1.10f, y + r * 0.15f, r * 2.20f, r * 0.95f,
                  outline);

    gfx_fill_circle(x, y - r * 0.10f, r * squash, body);
    gfx_fill_circle(x - r * 0.55f, y + r * 0.42f, r * 0.74f, body);
    gfx_fill_circle(x + r * 0.55f, y + r * 0.42f, r * 0.74f, body);
    gfx_fill_rect(x - r * 1.10f, y + r * 0.15f, r * 2.20f, r * 0.95f, body);

    /* glossy highlight */
    gfx_fill_circle(x - r * 0.34f, y - r * 0.46f, r * 0.30f,
                    rgba(1.0f, 1.0f, 1.0f, 0.38f));
    gfx_fill_circle(x - r * 0.46f, y - r * 0.58f, r * 0.13f,
                    rgba(1.0f, 1.0f, 1.0f, 0.55f));

    /* little sprout on top (waves with the body) */
    gfx_fill_rect(x - 1.0f, y - r * 1.55f, 2.0f, r * 0.55f,
                  rgba(0.20f, 0.55f, 0.26f, 1.0f));
    gfx_fill_circle(x - 3.5f, y - r * 1.60f, 2.6f,
                    rgba(0.42f, 0.92f, 0.45f, 1.0f));
    gfx_fill_circle(x + 3.0f, y - r * 1.70f, 2.2f,
                    rgba(0.34f, 0.82f, 0.40f, 1.0f));

    /* cheeks */
    gfx_fill_circle(x - r * 0.62f, y + r * 0.34f, r * 0.14f,
                    rgba(1.00f, 0.55f, 0.62f, 0.70f));
    gfx_fill_circle(x + r * 0.62f, y + r * 0.34f, r * 0.14f,
                    rgba(1.00f, 0.55f, 0.62f, 0.70f));

    /* eyes: white ovals, dark pupils, blink squeezes them shut */
    look = sinf(t * 0.9f) * r * 0.10f;
    if (blink > 0.5f) {
        gfx_fill_rect(x - r * 0.45f, y - r * 0.10f, r * 0.24f, 1.6f, outline);
        gfx_fill_rect(x + r * 0.21f, y - r * 0.10f, r * 0.24f, 1.6f, outline);
    } else {
        gfx_fill_circle(x - r * 0.30f, y - r * 0.12f, r * 0.24f,
                        rgba(1.0f, 1.0f, 1.0f, 1.0f));
        gfx_fill_circle(x + r * 0.30f, y - r * 0.12f, r * 0.24f,
                        rgba(1.0f, 1.0f, 1.0f, 1.0f));
        gfx_fill_circle(x - r * 0.30f + look, y - r * 0.10f, r * 0.115f,
                        outline);
        gfx_fill_circle(x + r * 0.30f + look, y - r * 0.10f, r * 0.115f,
                        outline);
        gfx_fill_circle(x - r * 0.33f + look, y - r * 0.14f, r * 0.045f,
                        rgba(1.0f, 1.0f, 1.0f, 0.9f));
        gfx_fill_circle(x + r * 0.27f + look, y - r * 0.14f, r * 0.045f,
                        rgba(1.0f, 1.0f, 1.0f, 0.9f));
    }

    /* mouth per mood */
    if (mood == MASCOT_HAPPY) {
        gfx_fill_circle(x - r * 0.16f, y + r * 0.30f, r * 0.06f, outline);
        gfx_fill_circle(x, y + r * 0.36f, r * 0.06f, outline);
        gfx_fill_circle(x + r * 0.16f, y + r * 0.30f, r * 0.06f, outline);
    } else if (mood == MASCOT_WORRIED) {
        gfx_fill_rect(x - r * 0.18f, y + r * 0.36f, r * 0.36f, 1.5f, outline);
        gfx_fill_rect(x + r * 0.72f, y - r * 0.02f, 2.2f, 3.4f,
                      rgba(0.55f, 0.80f, 1.0f, 0.90f));
    } else if (mood == MASCOT_PALE) {
        gfx_fill_rect(x - r * 0.20f, y + r * 0.36f, r * 0.40f, 1.4f, outline);
    } else {
        gfx_fill_rect(x - r * 0.10f, y + r * 0.34f, r * 0.20f, 1.6f, outline);
    }

    /* happy sparkles */
    if (mood == MASCOT_HAPPY && app->mascotHappy > 0.0f) {
        for (i = 0; i < 3; i++) {
            float ang = t * 3.0f + (float)i * 2.1f;
            float sx = x + cosf(ang) * (r * 1.5f);
            float sy = y + sinf(ang) * (r * 1.1f) - r * 0.4f;
            GfxColor spark = pastel(COL_CHIP_LEMON, 0.1f);

            spark.a = 0.85f;
            art_star(sx, sy, 2.4f, spark);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Menu screen                                                               */
/* ------------------------------------------------------------------------ */

/* Chunky BL2 START button: red face, comic lettering, hazard-stripe caps. */
static void draw_start_button(float pulse)
{
    UiRect start = ui_start_rect();
    float cx = start.x + start.w * 0.5f;

    art_comic_button(start.x, start.y, start.w, start.h,
                     rgba(0.80f, 0.12f, 0.12f, 1.0f), COL_RED, pulse);

    text_comic_centered(cx, start.y + 10.0f, 3.0f, COL_WHITE, "START");
    text_centered(cx, start.y + 44.0f, 1.0f, COL_GOLD_SOFT, "TAP OR PRESS A");
}

static void draw_menu(const App *app)
{
    static const int kCardTypes[5] = { DMG_FIRE, DMG_ELECTRIC, DMG_CORROSIVE,
                                       DMG_SLAG, DMG_EXPLOSIVE };
    static const char *const kMenuCardNames[5] = { "FIRE", "SHOCK", "ACID",
                                                   "SLAG", "BLAST" };
    const float cardW = 56.0f;
    const float cardH = 52.0f;
    const float cardTop = 64.0f;
    float pulse;
    int active;
    int i;

    /* faint drifting hazard bands so the bottom matches the top-screen mood */
    gfx_fill_triangle(30.0f, 0.0f, 96.0f, 0.0f, -30.0f, 240.0f,
                      rgba(0.10f, 0.12f, 0.20f, 0.07f));
    gfx_fill_triangle(96.0f, 0.0f, 40.0f, 240.0f, -30.0f, 240.0f,
                      rgba(0.10f, 0.12f, 0.20f, 0.07f));
    gfx_fill_triangle(200.0f, 0.0f, 276.0f, 0.0f, 150.0f, 240.0f,
                      rgba(0.10f, 0.12f, 0.20f, 0.06f));
    gfx_fill_triangle(276.0f, 0.0f, 220.0f, 240.0f, 150.0f, 240.0f,
                      rgba(0.10f, 0.12f, 0.20f, 0.06f));

    /* comic title (same lettering treatment as the top screen) */
    art_glow(160.0f, 34.0f, 110.0f, COL_GOLD, 0.07f);
    text_comic_centered(160.0f, 8.0f, 3.0f, COL_GOLD_SOFT, "ELEMENTAL");
    text_comic_centered(160.0f, 36.0f, 2.0f, COL_WHITE, "DEFENSE");

    /* friendly floating hearts either side of the title block */
    {
        float hyL = 46.0f + sinf(app->menuTime * 1.7f) * 4.0f;
        float hyR = 46.0f + sinf(app->menuTime * 1.7f + 2.6f) * 4.0f;
        GfxColor heartL = COL_CHIP_PINK;
        GfxColor heartR = COL_CHIP_LEMON;

        heartL.a = 0.85f;
        heartR.a = 0.85f;
        art_heart(34.0f, hyL, 3.6f, heartL);
        art_heart(286.0f, hyR, 3.6f, heartR);
    }

    /* thin gold rule with hazard-stripe ends */
    art_hazard_strip(24.0f, 55.0f, 10.0f, 6.0f);
    art_hazard_strip(286.0f, 55.0f, 10.0f, 6.0f);
    gfx_fill_rect(38.0f, 57.0f, 244.0f, 2.0f, COL_GOLD);

    /* compact element card strip: rounded toy cards with big emblems */
    active = (int)(app->menuTime * 0.8f + 2.0f) % 5;
    pulse = 0.5f + 0.5f * sinf(app->menuTime * 2.6f);
    for (i = 0; i < 5; i++) {
        int type = kCardTypes[i];
        float x = 12.0f + (float)i * (cardW + 4.0f);
        float y = cardTop;
        float cx = x + cardW * 0.5f;
        GfxColor color = art_damage_color(type, 1.0f);
        GfxColor trim = pastel(color, 0.45f);
        bool isActive = (i == active);
        UiRect card = { x, y, cardW, cardH };

        if (isActive) {
            card.y -= 2.0f;
            art_glow(cx, card.y + cardH * 0.5f, 32.0f, color,
                     0.10f + 0.08f * pulse);
        }
        cute_panel(card.x, card.y, card.w, card.h, 9.0f,
                   isActive ? pastel(color, 0.20f) : COL_PANEL_EDGE);
        /* element colour header band */
        fill_round_rect(card.x + 6.0f, card.y + 5.0f, card.w - 12.0f, 5.0f,
                        2.5f, trim);
        art_glow(cx, card.y + 28.0f, 18.0f, color, isActive ? 0.26f : 0.10f);
        art_weapon_emblem(cx, card.y + 28.0f, isActive ? 12.0f : 11.0f, type);
        text_centered(cx, card.y + 42.0f, 1.0f,
                      isActive ? COL_WHITE : COL_MUTED, kMenuCardNames[i]);
    }

    pulse = 0.70f + 0.30f * (0.5f + 0.5f * sinf(app->menuTime * 3.0f));
    draw_start_button(pulse);

    /* the mascot waves hello from the bottom-right corner */
    draw_mascot(app, 304.0f, 206.0f, 12.0f, MASCOT_HAPPY);

    text_centered(160.0f, 202.0f, 1.0f, COL_DIM, "ATTEMPT 2 - 3DS HOMEBREW");
    text_centered(160.0f, 220.0f, 1.0f, COL_DIM,
                  "D-PAD TO BUILD - TOUCH FOR BUTTONS");
}

/* ------------------------------------------------------------------------ */
/* Header stat chips                                                         */
/* ------------------------------------------------------------------------ */

/* Compact 0-6+ digit numbers for narrow chips: 253060 -> "253K", 4321 -> "4.3K",
 * 1200000 -> "1.2M".  Keeps every readout inside its chip at real late-game
 * values (the 1000-wave campaign can end with ~253,060 gold). */
static void format_compact(int value, char *buf, size_t size)
{
    if (value < 0)
        value = 0;
    /* thresholds are chosen so rounding can never produce a 5-character
     * string (e.g. 9999 must not become "10.0K"): each branch is <= 4 glyphs. */
    if (value < 1000)
        snprintf(buf, size, "%d", value);
    else if (value < 9950)
        snprintf(buf, size, "%.1fK", (double)value / 1000.0);
    else if (value < 999500)
        snprintf(buf, size, "%.0fK", (double)value / 1000.0);
    else if (value < 9950000)
        snprintf(buf, size, "%.1fM", (double)value / 1000000.0);
    else
        snprintf(buf, size, "%.0fM", (double)value / 1000000.0);
}

/* Big value that shrinks to scale 1 when it would not fit the chip. */
static void chip_number(float cx, float y, float chipW, int value, GfxColor color)
{
    char buf[16];
    float scale = 2.0f;

    snprintf(buf, sizeof(buf), "%d", value);
    if (gfx_text_width(buf, scale) > chipW)
        scale = 1.0f;
    text_centered(cx, y, scale, color, buf);
}

static void draw_stat_chips(const App *app)
{
    const Game *g = &app->game;
    char buf[32];
    int shownWave = g->wave > g->totalWaves ? g->totalWaves : g->wave;

    /* gold (52 px): plain below 1000, compact K/M above so 253,060 never
     * spills into the lives chip */
    cute_chip(4.0f, CHIP_Y, 52.0f, CHIP_H, COL_CHIP_GOLD);
    art_coin(14.0f, 18.0f, 5.0f);
    if (g->gold < 1000) {
        chip_number(36.0f, 10.0f, 34.0f, g->gold, COL_GOLD);
    } else {
        format_compact(g->gold, buf, sizeof(buf));
        text_centered(36.0f, 10.0f, 1.0f, COL_GOLD, buf);
    }

    /* lives (46 px) */
    cute_chip(59.0f, CHIP_Y, 46.0f, CHIP_H, COL_CHIP_PINK);
    art_heart(67.0f, 18.0f, 5.5f, COL_RED);
    chip_number(88.0f, 10.0f, 34.0f, g->lives, COL_WHITE);

    /* wave: two scale-1 lines so 4-digit totals always fit (88 px) */
    cute_chip(108.0f, CHIP_Y, 88.0f, CHIP_H, COL_CHIP_SKY);
    text_centered(152.0f, 6.0f, 1.0f, COL_CHIP_SKY, "WAVE");
    snprintf(buf, sizeof(buf), "%d/%d", shownWave, g->totalWaves);
    text_centered(152.0f, 16.0f, 1.0f, COL_WHITE, buf);

    /* towers owned (50 px: 3 digits auto-shrink to scale 1) */
    cute_chip(199.0f, CHIP_Y, 50.0f, CHIP_H, COL_CHIP_MINT);
    tiny_tower_icon(207.0f, 18.0f, 4.5f, COL_CHIP_MINT);
    chip_number(230.0f, 10.0f, 32.0f, g->towerCount, COL_WHITE);

    /* speed pill (touch control) */
    cute_chip(252.0f, CHIP_Y, 34.0f, CHIP_H, COL_CHIP_LEMON);
    speed_text(g->speedMultiplier, buf, sizeof(buf));
    text_centered(269.0f, 10.0f, 2.0f,
                  g->speedMultiplier > 1.0f ? COL_YELLOW : COL_WHITE, buf);

    /* pause pill (touch control) */
    cute_chip(289.0f, CHIP_Y, 28.0f, CHIP_H, COL_CHIP_LILAC);
    gfx_fill_rect(297.0f, 12.0f, 4.0f, 12.0f, COL_WHITE);
    gfx_fill_rect(305.0f, 12.0f, 4.0f, 12.0f, COL_WHITE);
}

/* ------------------------------------------------------------------------ */
/* Tower palette                                                             */
/* ------------------------------------------------------------------------ */

static void draw_tower_palette(const App *app)
{
    const Game *g = &app->game;
    char buf[16];
    int i;

    for (i = 0; i < TOWER_TYPE_COUNT; i++) {
        UiRect r = ui_tower_button_rect(i);
        const TowerSpec *spec = game_tower_spec(i);
        int cost = spec != NULL ? spec->cost : 0;
        GfxColor color = art_damage_color(i, 1.0f);
        GfxColor trim = pastel(color, 0.45f);
        bool selected = g->selectedType == i;
        bool affordable = g->gold >= cost;
        float nameX = r.x + 25.0f +
                      gfx_text_width(kTowerNames[i], 1.0f) * 0.5f;

        if (selected) {
            art_glow(r.x + 18.0f, r.y + r.h * 0.5f, 36.0f, color, 0.18f);
            cute_button(r, trim, rgba(0.10f, 0.12f, 0.20f, 1.0f), 0.6f);
            fill_round_rect(r.x + 4.0f, r.y + r.h - 6.0f, r.w - 8.0f, 3.0f,
                            1.5f, color);
        } else {
            cute_panel(r.x, r.y, r.w, r.h, 8.0f, COL_PANEL_EDGE);
            fill_round_rect(r.x + 4.0f, r.y + 5.0f, 3.0f, r.h - 10.0f, 1.5f,
                            color);
        }

        /* bigger element emblem in its own little well */
        gfx_fill_circle(r.x + 15.0f, r.y + 13.5f, 9.5f,
                        rgba(color.r * 0.22f, color.g * 0.22f,
                             color.b * 0.22f, 1.0f));
        gfx_outline_circle(r.x + 15.0f, r.y + 13.5f, 9.5f, 1.0f,
                           rgba(color.r, color.g, color.b, 0.65f));
        art_glow(r.x + 15.0f, r.y + 13.5f, 10.0f, color,
                 selected ? 0.30f : 0.12f);
        art_weapon_emblem(r.x + 15.0f, r.y + 13.5f, 7.0f, i);

        text_centered(nameX, r.y + 4.0f, 1.0f,
                      selected ? COL_WHITE : COL_MUTED, kTowerNames[i]);
        gfx_fill_circle(r.x + 28.0f, r.y + 20.0f, 2.5f, COL_GOLD);
        snprintf(buf, sizeof(buf), "%d", cost);
        gfx_text(r.x + 33.0f, r.y + 16.0f, 1.0f,
                 affordable ? COL_GOLD : rgba(0.90f, 0.82f, 0.60f, 0.95f), buf);
    }
}

/* ------------------------------------------------------------------------ */
/* Selected tower / cursor panel                                             */
/* ------------------------------------------------------------------------ */

static void draw_upgrade_sell(const App *app, int towerIndex)
{
    const Game *g = &app->game;
    char buf[32];
    int upgradeCost = game_upgrade_cost(g, towerIndex);
    int sellValue = game_sell_value(g, towerIndex);
    bool canUpgrade = upgradeCost >= 0 && g->gold >= upgradeCost;
    UiRect up = ui_upgrade_rect();
    UiRect sell = ui_sell_rect();

    if (upgradeCost < 0)
        snprintf(buf, sizeof(buf), "MAX LEVEL");
    else
        snprintf(buf, sizeof(buf), "UPGRADE %dG", upgradeCost);
    cute_button(up, rgba(0.35f, 0.95f, 0.48f, canUpgrade ? 0.95f : 0.40f),
                rgba(0.05f, 0.24f, 0.12f, canUpgrade ? 1.0f : 0.50f), 0.0f);
    text_centered(up.x + up.w * 0.5f, up.y + 8.0f, 1.0f,
                  upgradeCost < 0 ? COL_GOLD_SOFT
                                  : (canUpgrade ? COL_WHITE : COL_DIM),
                  buf);

    if (sellValue < 0)
        sellValue = 0;
    snprintf(buf, sizeof(buf), "SELL +%dG", sellValue);
    cute_button(sell, rgba(1.00f, 0.55f, 0.58f, 0.95f),
                rgba(0.28f, 0.07f, 0.09f, 1.0f), 0.0f);
    text_centered(sell.x + sell.w * 0.5f, sell.y + 8.0f, 1.0f, COL_WHITE, buf);
}

static void draw_info_panel(const App *app)
{
    const Game *g = &app->game;
    UiRect panel = { PANEL_X, PANEL_Y, PANEL_W, PANEL_H };
    int towerIndex = panel_tower_index(app);
    char buf[64];

    cute_panel(panel.x, panel.y, panel.w, panel.h, 12.0f, COL_PANEL_EDGE);

    if (towerIndex >= 0 && towerIndex < g->towerCount &&
        g->towers[towerIndex].active) {
        const Tower *tower = &g->towers[towerIndex];
        GfxColor color = art_damage_color(tower->damageType, 1.0f);
        GfxColor trim = pastel(color, 0.45f);
        GfxColor nameColor = art_damage_text_color(tower->damageType);
        float damage = 0.0f;
        float fireRate = 1.0f;
        float range = 0.0f;
        float dps;

        game_tower_stats(tower->damageType, (int)tower->level, &damage,
                         &fireRate, &range);
        dps = fireRate > 0.0f ? damage / fireRate : 0.0f;

        /* cute element badge in its own ring */
        gfx_fill_circle(22.0f, 114.0f, 13.0f,
                        rgba(color.r * 0.25f, color.g * 0.25f,
                             color.b * 0.25f, 1.0f));
        gfx_outline_circle(22.0f, 114.0f, 13.0f, 2.0f, trim);
        art_glow(22.0f, 114.0f, 13.0f, color, 0.26f);
        art_weapon_emblem(22.0f, 114.0f, 7.5f, tower->damageType);

        snprintf(buf, sizeof(buf), "%s LV.%d",
                 tower_display_name(tower->damageType), (int)tower->level);
        gfx_text(42.0f, 100.0f, 1.0f, nameColor, buf);
        snprintf(buf, sizeof(buf), "DMG %.1f  RNG %.1f  DPS %.1f",
                 (double)damage, (double)range, (double)dps);
        gfx_text(42.0f, 110.0f, 1.0f, COL_MUTED, buf);

        tower_effect_text(tower->damageType, buf, sizeof(buf));
        gfx_text(42.0f, 120.0f, 1.0f, nameColor, buf);

        draw_upgrade_sell(app, towerIndex);
    } else {
        bool isPath = cell_is_path(g, g->cursorCol, g->cursorRow);
        bool canPlace = game_can_place_at(g, g->cursorCol, g->cursorRow,
                                          g->selectedType);
        const TowerSpec *selectedSpec = game_tower_spec(g->selectedType);
        const char *name = tower_display_name(g->selectedType);
        int cost = selectedSpec != NULL ? selectedSpec->cost : 0;
        GfxColor selColor = art_damage_text_color(g->selectedType);

        /* cursor pill + cell state pill */
        cute_chip(10.0f, 100.0f, 60.0f, 15.0f, COL_CHIP_SKY);
        snprintf(buf, sizeof(buf), "C%d R%d", g->cursorCol, g->cursorRow);
        text_centered(40.0f, 103.0f, 1.0f, COL_WHITE, buf);
        cute_chip(222.0f, 100.0f, 82.0f, 15.0f,
                  isPath ? COL_CHIP_LEMON : COL_PANEL_EDGE);
        text_centered(263.0f, 103.0f, 1.0f,
                      isPath ? COL_CHIP_LEMON : COL_DIM,
                      isPath ? "ROAD CELL" : "EMPTY CELL");

        /* Maze building: a tower on the road forces the BFS to find a detour,
         * so a buildable road cell is a *positive* hint, not a warning. */
        if (canPlace && isPath) {
            gfx_text(10.0f, 120.0f, 1.0f, rgba(0.62f, 1.00f, 0.70f, 1.0f),
                     "ROAD - BUILD TO REROUTE THEM");
        } else if (canPlace) {
            snprintf(buf, sizeof(buf), "A BUILDS %s FOR %dG", name, cost);
            gfx_text(10.0f, 120.0f, 1.0f, selColor, buf);
        } else if (isPath) {
            gfx_text(10.0f, 120.0f, 1.0f, COL_MUTED,
                     "ROAD - THIS CELL CAN'T BE BLOCKED");
        } else {
            gfx_text(10.0f, 120.0f, 1.0f, COL_MUTED,
                     "CAN'T BUILD HERE RIGHT NOW");
        }

        gfx_text(10.0f, 134.0f, 1.0f, COL_DIM,
                 "L/R SWAP TYPE  -  TAP A TYPE ABOVE");
        snprintf(buf, sizeof(buf), "SELECTED: %s", name);
        gfx_text(10.0f, 144.0f, 1.0f, COL_DIM, buf);
    }
}

/* ------------------------------------------------------------------------ */
/* Transient message toast                                                   */
/* ------------------------------------------------------------------------ */

static void draw_message(const App *app)
{
    const Game *g = &app->game;
    float alpha;
    float width;
    float x;
    char buf[40];

    if (g->messageTimer <= 0.0f || g->message[0] == '\0')
        return;

    alpha = g->messageTimer > 0.4f ? 1.0f : g->messageTimer / 0.4f;
    if (alpha < 0.0f)
        alpha = 0.0f;
    if (alpha > 1.0f)
        alpha = 1.0f;

    snprintf(buf, sizeof(buf), "%.36s", g->message);
    width = gfx_text_width(buf, 1.0f) + 20.0f;
    if (width > 300.0f)
        width = 300.0f;
    x = floorf(((float)BOT_W - width) * 0.5f);

    cute_panel(x, PANEL_Y + 2.0f, width, 18.0f, 9.0f,
               rgba(1.00f, 0.62f, 0.70f, 0.85f * alpha));
    text_centered((float)BOT_W * 0.5f, PANEL_Y + 7.0f, 1.0f,
                  rgba(1.0f, 0.88f, 0.90f, alpha), buf);
}

/* ------------------------------------------------------------------------ */
/* Build phase: NEXT WAVE + incoming preview                                 */
/* ------------------------------------------------------------------------ */

static void draw_incoming_bracket(UiRect r, GfxColor color)
{
    const float l = 7.0f;

    gfx_fill_rect(r.x - 3.0f, r.y - 3.0f, l, 2.0f, color);
    gfx_fill_rect(r.x - 3.0f, r.y - 3.0f, 2.0f, l, color);
    gfx_fill_rect(r.x + r.w - l + 3.0f, r.y - 3.0f, l, 2.0f, color);
    gfx_fill_rect(r.x + r.w + 1.0f, r.y - 3.0f, 2.0f, l, color);
    gfx_fill_rect(r.x - 3.0f, r.y + r.h - l + 3.0f, l, 2.0f, color);
    gfx_fill_rect(r.x - 3.0f, r.y + r.h + 1.0f, 2.0f, l, color);
    gfx_fill_rect(r.x + r.w - l + 3.0f, r.y + r.h - l + 3.0f, l, 2.0f, color);
    gfx_fill_rect(r.x + r.w + 1.0f, r.y + r.h + 1.0f, 2.0f, l, color);
}

static void draw_build_section(const App *app)
{
    const Game *g = &app->game;
    UiRect next = ui_next_wave_rect();
    UiRect preview = { 170.0f, BOTTOM_Y, 112.0f, BOTTOM_H };
    float bounce = sinf(app->time * 3.2f) * 2.0f;
    float pulse = 0.5f + 0.5f * sinf(app->time * 2.5f);
    int shownWave = g->wave > g->totalWaves ? g->totalWaves : g->wave;
    char buf[40];
    int hpLayers = 0, shieldLayers = 0, armorLayers = 0, elites = 0;

    /* NEXT WAVE: chunky rounded button with a gentle bounce + glow */
    art_glow(next.x + next.w * 0.5f, next.y + next.h * 0.5f, 74.0f,
             COL_GREEN, 0.10f + 0.10f * pulse);
    cute_button(next, COL_CHIP_MINT, rgba(0.06f, 0.26f, 0.14f, 1.0f), pulse);

    text_centered(next.x + next.w * 0.5f, next.y + 9.0f + bounce, 2.0f,
                  COL_WHITE, "NEXT WAVE");
    snprintf(buf, sizeof(buf), "WAVE %d OF %d", shownWave, g->totalWaves);
    text_centered(next.x + next.w * 0.5f, next.y + 34.0f, 1.0f,
                  rgba(0.90f, 1.00f, 0.92f, 0.95f), buf);
    text_centered(next.x + next.w * 0.5f, next.y + 50.0f, 1.0f,
                  rgba(0.86f, 0.98f, 0.88f, 0.92f), "TAP TO START");
    art_hazard_strip(next.x + 8.0f, next.y + next.h - 9.0f, 9.0f, 4.0f);
    art_hazard_strip(next.x + next.w - 17.0f, next.y + next.h - 9.0f, 9.0f,
                     4.0f);

    /* incoming composition with a cute bracket frame */
    cute_panel(preview.x, preview.y, preview.w, preview.h, 10.0f,
               COL_CHIP_LEMON);
    draw_incoming_bracket(preview, COL_CHIP_LEMON);
    text_centered(preview.x + preview.w * 0.5f, preview.y + 4.0f, 1.0f,
                  COL_CHIP_LEMON, "INCOMING");

    game_wave_preview(g, &hpLayers, &shieldLayers, &armorLayers, &elites);
    draw_preview_row(preview.x + 10.0f, preview.y + 17.0f, hpLayers, LAYER_HP,
                     false);
    draw_preview_row(preview.x + 10.0f, preview.y + 29.0f, shieldLayers,
                     LAYER_SHIELD, false);
    draw_preview_row(preview.x + 10.0f, preview.y + 41.0f, armorLayers,
                     LAYER_ARMOR, false);
    draw_preview_row(preview.x + 10.0f, preview.y + 53.0f, elites, 0, true);
}

/* ------------------------------------------------------------------------ */
/* Wave phase: progress + stats                                              */
/* ------------------------------------------------------------------------ */

static void draw_wave_section(const App *app)
{
    const Game *g = &app->game;
    UiRect panel = { 6.0f, BOTTOM_Y, 276.0f, BOTTOM_H };
    int shownWave = g->wave > g->totalWaves ? g->totalWaves : g->wave;
    char buf[48];
    int total = app->waveEnemyTotal;
    int remaining;
    float frac;

    cute_panel(panel.x, panel.y, panel.w, panel.h, 12.0f, COL_CHIP_SKY);

    snprintf(buf, sizeof(buf), "WAVE %d/%d", shownWave, g->totalWaves);
    text_centered(panel.x + panel.w * 0.5f, panel.y + 5.0f, 2.0f, COL_WHITE,
                  buf);

    remaining = g->enemiesAlive + g->enemiesToSpawn;
    if (remaining < 0)
        remaining = 0;
    frac = total > 0 ? 1.0f - (float)remaining / (float)total : 0.0f;
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;

    /* rounded progress bar with a pastel amber fill and cute end caps */
    fill_round_rect(14.0f, panel.y + 24.0f, 258.0f, 16.0f, 8.0f,
                    rgba(0.24f, 0.26f, 0.42f, 1.0f));
    fill_round_rect(16.0f, panel.y + 26.0f, 254.0f, 12.0f, 6.0f,
                    rgba(0.07f, 0.08f, 0.16f, 1.0f));
    if (frac > 0.0f) {
        float fillW = 254.0f * frac;
        float radius;

        if (fillW < 12.0f)
            fillW = 12.0f;
        radius = fillW * 0.5f;
        if (radius > 6.0f)
            radius = 6.0f;
        fill_round_rect(16.0f, panel.y + 26.0f, fillW, 12.0f, radius,
                        rgba(1.00f, 0.72f, 0.32f, 1.0f));
        gfx_fill_rect(20.0f, panel.y + 27.0f,
                      fillW > 16.0f ? fillW - 8.0f : 1.0f, 2.0f,
                      rgba(1.0f, 0.92f, 0.68f, 0.85f));
    }
    snprintf(buf, sizeof(buf), "CLEARED %d%%   LEFT %d",
             (int)(frac * 100.0f + 0.5f), remaining);
    text_centered(145.0f, panel.y + 27.0f, 1.0f, COL_WHITE, buf);

    {
        char kills[16];

        format_compact(g->kills, kills, sizeof(kills));
        snprintf(buf, sizeof(buf), "ALIVE %d   QUEUED %d   KILLS %s",
                 g->enemiesAlive, g->enemiesToSpawn, kills);
    }
    text_centered(145.0f, panel.y + 45.0f, 1.0f, COL_CHIP_MINT, buf);
    snprintf(buf, sizeof(buf), "LEAKS %d   TIME %d:%02d",
             g->leaks, (int)g->timeTotal / 60, (int)g->timeTotal % 60);
    text_centered(145.0f, panel.y + 55.0f, 1.0f,
                  g->leaks > 0 ? COL_RED : COL_MUTED, buf);
    text_centered(145.0f, panel.y + 65.0f, 1.0f, COL_DIM,
                  "START: PAUSE    SELECT: SPEED");
}

/* ------------------------------------------------------------------------ */
/* Pause overlay                                                             */
/* ------------------------------------------------------------------------ */

static void draw_pause_overlay(const App *app)
{
    UiRect panel = { 50.0f, 24.0f, 220.0f, 196.0f };
    UiRect resume = ui_resume_rect();
    UiRect menu = ui_pause_menu_rect();

    gfx_fill_rect(0.0f, 0.0f, (float)BOT_W, (float)BOT_H,
                  rgba(0.02f, 0.02f, 0.05f, 0.68f));
    cute_panel(panel.x, panel.y, panel.w, panel.h, 16.0f, COL_CHIP_LILAC);
    art_hazard_strip(62.0f, 31.0f, 14.0f, 5.0f);
    art_hazard_strip(244.0f, 31.0f, 14.0f, 5.0f);

    text_centered(160.0f, 40.0f, 3.0f, COL_WHITE, "PAUSED");
    text_centered(160.0f, 74.0f, 1.0f, COL_MUTED, "THE WAVE IS FROZEN");
    draw_mascot(app, 250.0f, 58.0f, 11.0f, MASCOT_WORRIED);

    cute_button(resume, COL_CHIP_MINT, rgba(0.07f, 0.28f, 0.16f, 1.0f), 0.6f);
    text_centered(resume.x + resume.w * 0.5f, resume.y + 14.0f, 2.0f,
                  COL_WHITE, "RESUME");

    cute_button(menu, COL_CHIP_LEMON, rgba(0.10f, 0.10f, 0.18f, 1.0f), 0.0f);
    text_centered(menu.x + menu.w * 0.5f, menu.y + 10.0f, 1.0f, COL_WHITE,
                  "MAIN MENU");

    text_centered(160.0f, 208.0f, 1.0f, COL_MUTED, "START ALSO RESUMES");
}

/* ------------------------------------------------------------------------ */
/* Result screen                                                             */
/* ------------------------------------------------------------------------ */

static void draw_result(const App *app)
{
    const Game *g = &app->game;
    bool victory = g->phase == PHASE_VICTORY;
    UiRect retry = ui_retry_rect();
    UiRect menu = ui_result_menu_rect();
    UiRect stats = { 30.0f, 64.0f, 260.0f, 66.0f };
    char buf[64];
    int seconds = (int)g->timeTotal;
    int shownWave = g->wave > g->totalWaves ? g->totalWaves : g->wave;

    if (seconds < 0)
        seconds = 0;

    art_glow(160.0f, 40.0f, 140.0f, victory ? COL_GOLD : COL_RED, 0.12f);
    text_comic_centered(160.0f, 14.0f, 4.0f, victory ? COL_GOLD_SOFT : COL_RED,
                        victory ? "VICTORY!" : "GAME OVER");
    text_centered(160.0f, 52.0f, 1.0f, COL_MUTED,
                  victory ? "ALL WAVES CLEARED" : "THE PATH WAS BREACHED");

    cute_panel(stats.x, stats.y, stats.w, stats.h, 12.0f,
               victory ? COL_CHIP_MINT : COL_CHIP_PINK);
    snprintf(buf, sizeof(buf), "WAVE %d/%d", shownWave, g->totalWaves);
    text_centered(160.0f, 71.0f, 1.0f, COL_WHITE, buf);
    snprintf(buf, sizeof(buf), "KILLS %d     LEAKS %d", g->kills, g->leaks);
    text_centered(160.0f, 87.0f, 1.0f, COL_MUTED, buf);
    snprintf(buf, sizeof(buf), "TIME %d:%02d     GOLD %d", seconds / 60,
             seconds % 60, g->gold);
    text_centered(160.0f, 103.0f, 1.0f, COL_MUTED, buf);

    draw_mascot(app, 304.0f, 96.0f, 12.0f,
                victory ? MASCOT_HAPPY : MASCOT_PALE);

    cute_button(retry, COL_CHIP_MINT, rgba(0.07f, 0.28f, 0.16f, 1.0f), 0.6f);
    text_centered(retry.x + retry.w * 0.5f, retry.y + 11.0f, 2.0f, COL_WHITE,
                  "PLAY AGAIN");
    text_centered(retry.x + retry.w * 0.5f, retry.y + 38.0f, 1.0f,
                  rgba(0.85f, 1.00f, 0.90f, 0.90f), "TAP TO RETRY");

    cute_button(menu, COL_CHIP_LEMON, rgba(0.10f, 0.10f, 0.18f, 1.0f), 0.0f);
    text_centered(menu.x + menu.w * 0.5f, menu.y + 10.0f, 1.0f, COL_WHITE,
                  "MAIN MENU");
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                               */
/* ------------------------------------------------------------------------ */

void ui_draw(const App *app)
{
    const Game *g = &app->game;
    int mascotMood = MASCOT_IDLE;

    render_background_gradient(BOT_W, BOT_H);

    if (g->phase == PHASE_MENU) {
        draw_menu(app);
        return;
    }
    if (g->phase == PHASE_GAMEOVER || g->phase == PHASE_VICTORY) {
        draw_result(app);
        return;
    }

    if (app->mascotHappy > 0.0f && app->mascotWorried <= 0.0f)
        mascotMood = MASCOT_HAPPY;
    else if (app->mascotWorried > 0.0f)
        mascotMood = MASCOT_WORRIED;
    else if (g->lives <= 3)
        mascotMood = MASCOT_PALE;

    draw_stat_chips(app);
    draw_tower_palette(app);
    draw_info_panel(app);
    draw_message(app);

    if (g->phase == PHASE_BUILD)
        draw_build_section(app);
    else
        draw_wave_section(app);

    draw_mascot(app, MASCOT_X, MASCOT_Y, MASCOT_R, mascotMood);

    if (g->paused)
        draw_pause_overlay(app);
}

/* ------------------------------------------------------------------------ */
/* Hit testing                                                               */
/* ------------------------------------------------------------------------ */

bool ui_hit_test(const App *app, float x, float y, UiEvent *ev)
{
    const Game *g = &app->game;
    UiEvent local;
    int i;
    int towerIndex;

    if (ev == NULL)
        ev = &local;
    ev->type = UI_EVENT_NONE;
    ev->index = 0;

    if (app == NULL)
        return false;

    if (g->phase == PHASE_MENU) {
        if (ui_rect_contains(ui_start_rect(), x, y)) {
            ev->type = UI_EVENT_START;
            return true;
        }
        return false;
    }

    if (g->phase == PHASE_GAMEOVER || g->phase == PHASE_VICTORY) {
        if (ui_rect_contains(ui_retry_rect(), x, y)) {
            ev->type = UI_EVENT_RETRY;
            return true;
        }
        if (ui_rect_contains(ui_result_menu_rect(), x, y)) {
            ev->type = UI_EVENT_MENU;
            return true;
        }
        return false;
    }

    /* play phases */
    if (g->paused) {
        if (ui_rect_contains(ui_resume_rect(), x, y)) {
            ev->type = UI_EVENT_RESUME;
            return true;
        }
        if (ui_rect_contains(ui_pause_menu_rect(), x, y)) {
            ev->type = UI_EVENT_MENU;
            return true;
        }
        return false;
    }

    if (ui_rect_contains(ui_speed_rect(), x, y)) {
        ev->type = UI_EVENT_CYCLE_SPEED;
        return true;
    }
    if (ui_rect_contains(ui_pause_rect(), x, y)) {
        ev->type = UI_EVENT_TOGGLE_PAUSE;
        return true;
    }

    for (i = 0; i < TOWER_TYPE_COUNT; i++) {
        if (ui_rect_contains(ui_tower_button_rect(i), x, y)) {
            ev->type = UI_EVENT_SELECT_TYPE;
            ev->index = i;
            return true;
        }
    }

    towerIndex = panel_tower_index(app);
    if (towerIndex >= 0 && towerIndex < g->towerCount &&
        g->towers[towerIndex].active) {
        if (ui_rect_contains(ui_upgrade_rect(), x, y)) {
            ev->type = UI_EVENT_UPGRADE;
            return true;
        }
        if (ui_rect_contains(ui_sell_rect(), x, y)) {
            ev->type = UI_EVENT_SELL;
            return true;
        }
    }

    if (g->phase == PHASE_BUILD && ui_rect_contains(ui_next_wave_rect(), x, y)) {
        ev->type = UI_EVENT_NEXT_WAVE;
        return true;
    }

    return false;
}
