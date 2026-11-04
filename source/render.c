/*
 * Elemental Defense - top screen renderer
 *
 * Draws the battlefield with citro2d (through the portable gfx.h API) plus the
 * menu / pause / result overlays.  Coordinate conventions:
 *   - "cell space" comes from include/game.h: 1.0 == one grid cell, cell
 *     (col,row) spans [col, col+1) x [row, row+1).
 *   - top screen pixels: 400x240, origin top-left, +y downwards.
 *
 * The board is 26x13 cells at 15 px, so BOARD_W/H have 7/12 px of spare room
 * around them.  Everything is floored to whole pixels; no smoothing anywhere.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "render.h"

/* ------------------------------------------------------------------------ */
/* Small drawing helpers                                                     */
/* ------------------------------------------------------------------------ */

static void poly_fill(const float *xs, const float *ys, int count, GfxColor color)
{
    float cx = 0.0f;
    float cy = 0.0f;
    int i;

    if (count < 3)
        return;

    for (i = 0; i < count; i++) {
        cx += xs[i];
        cy += ys[i];
    }
    cx /= (float)count;
    cy /= (float)count;

    /* Fan from the centroid: all element glyphs below are star shaped. */
    for (i = 0; i < count; i++) {
        int next = (i + 1) % count;
        gfx_fill_triangle(cx, cy, xs[i], ys[i], xs[next], ys[next], color);
    }
}

static GfxColor rgba(float r, float g, float b, float a)
{
    return gfx_rgba(r, g, b, a);
}

/* ------------------------------------------------------------------------ */
/* Public math helpers                                                       */
/* ------------------------------------------------------------------------ */

float cell_px_x(float cellX)
{
    return BOARD_X + cellX * BOARD_CELL;
}

float cell_px_y(float cellY)
{
    return BOARD_Y + cellY * BOARD_CELL;
}

float board_cell_left(int col)
{
    return BOARD_X + (float)col * BOARD_CELL;
}

float board_cell_top(int row)
{
    return BOARD_Y + (float)row * BOARD_CELL;
}

bool board_hit_cell(float px, float py, int *col, int *row)
{
    int c = (int)floorf((px - BOARD_X) / BOARD_CELL);
    int r = (int)floorf((py - BOARD_Y) / BOARD_CELL);

    if (c < 0 || c >= GRID_COLS || r < 0 || r >= GRID_ROWS)
        return false;
    if (col != NULL)
        *col = c;
    if (row != NULL)
        *row = r;
    return true;
}

GfxColor art_spec_color(const float rgb[3], float alpha)
{
    GfxColor color;

    color.r = rgb[0];
    color.g = rgb[1];
    color.b = rgb[2];
    color.a = alpha;
    return color;
}

void text_centered(float cx, float y, float scale, GfxColor color, const char *text)
{
    float width = gfx_text_width(text, scale);
    gfx_text(floorf(cx - width * 0.5f), floorf(y), scale, color, text);
}

void text_right(float rightX, float y, float scale, GfxColor color, const char *text)
{
    float width = gfx_text_width(text, scale);
    gfx_text(floorf(rightX - width), floorf(y), scale, color, text);
}

/* ------------------------------------------------------------------------ */
/* Tower damage-type glyphs (docs/elements-spec.md §3)                      */
/* ------------------------------------------------------------------------ */

/* GUN (normal): a fat grey bullet with a dark driving band. */
static void icon_gun(float cx, float cy, float size, GfxColor color)
{
    float s = size;

    gfx_fill_rect(cx - s * 0.85f, cy - s * 0.32f, s * 1.25f, s * 0.64f, color);
    gfx_fill_triangle(cx + s * 0.40f, cy - s * 0.32f, cx + s * 0.95f, cy,
                      cx + s * 0.40f, cy + s * 0.32f, color);
    gfx_fill_rect(cx - s * 0.85f, cy - s * 0.32f, s * 0.55f, s * 0.24f,
                  rgba(1.0f, 1.0f, 1.0f, 0.35f));
    gfx_fill_rect(cx - s * 0.30f, cy - s * 0.32f, s * 0.16f, s * 0.64f,
                  rgba(0.16f, 0.16f, 0.20f, 0.85f));
}

/* FIRE: the original LÖVE2D flame polygon, kept for continuity. */
static void icon_fire(float cx, float cy, float size, GfxColor color)
{
    float w = size * 0.8f;
    float xs[6] = { cx, cx - w, cx - w * 0.3f, cx, cx + w * 0.3f, cx + w };
    float ys[6] = { cy - size, cy + size * 0.6f, cy + size * 0.2f,
                    cy + size * 0.8f, cy + size * 0.2f, cy + size * 0.6f };
    float hx[6] = { cx, cx - w * 0.4f, cx - w * 0.1f, cx,
                    cx + w * 0.1f, cx + w * 0.4f };
    float hy[6] = { cy - size * 0.6f, cy + size * 0.3f, cy + size * 0.1f,
                    cy + size * 0.4f, cy + size * 0.1f, cy + size * 0.3f };

    poly_fill(xs, ys, 6, color);
    poly_fill(hx, hy, 6, rgba(1.0f, 1.0f, 1.0f, 0.22f));
}

/* SHOCK (electric): a Z-shaped lightning bolt. */
static void icon_shock(float cx, float cy, float size, GfxColor color)
{
    float s = size;
    float xs[6] = { cx + s * 0.30f, cx - s * 0.70f, cx - s * 0.08f,
                    cx - s * 0.35f, cx + s * 0.70f, cx + s * 0.08f };
    float ys[6] = { cy - s, cy + s * 0.12f, cy + s * 0.12f,
                    cy + s, cy - s * 0.12f, cy - s * 0.12f };
    float hx[4] = { cx + s * 0.05f, cx - s * 0.28f, cx - s * 0.02f, cx - s * 0.16f };
    float hy[4] = { cy - s * 0.62f, cy - s * 0.02f, cy - s * 0.02f, cy + s * 0.55f };

    poly_fill(xs, ys, 6, color);
    poly_fill(hx, hy, 4, rgba(1.0f, 1.0f, 1.0f, 0.30f));
}

/* CORROSIVE: a dripping acid blob. */
static void icon_corrosive(float cx, float cy, float size, GfxColor color)
{
    float s = size;

    gfx_fill_circle(cx, cy + s * 0.12f, s * 0.66f, color);
    gfx_fill_circle(cx - s * 0.38f, cy - s * 0.34f, s * 0.28f, color);
    gfx_fill_triangle(cx - s * 0.34f, cy + s * 0.42f, cx + s * 0.34f,
                      cy + s * 0.42f, cx, cy + s * 1.05f, color);
    gfx_fill_circle(cx - s * 0.18f, cy - s * 0.02f, s * 0.20f,
                    rgba(1.0f, 1.0f, 1.0f, 0.28f));
    gfx_fill_circle(cx + s * 0.34f, cy + s * 0.30f, s * 0.13f,
                    rgba(0.10f, 0.28f, 0.04f, 0.55f));
}

/* SLAG (purple): a crystal shard. */
static void icon_slag(float cx, float cy, float size, GfxColor color)
{
    float s = size;
    float xs[4] = { cx, cx + s * 0.64f, cx, cx - s * 0.64f };
    float ys[4] = { cy - s, cy, cy + s, cy };
    float hx[3] = { cx, cx + s * 0.30f, cx + s * 0.02f };
    float hy[3] = { cy - s * 0.55f, cy - s * 0.05f, cy + s * 0.45f };

    poly_fill(xs, ys, 4, color);
    poly_fill(hx, hy, 3, rgba(1.0f, 1.0f, 1.0f, 0.30f));
}

/* BLASTER (explosive): a round bomb with a lit fuse. */
static void icon_blaster(float cx, float cy, float size, GfxColor color)
{
    float s = size;
    GfxColor body = { 0.22f, 0.20f, 0.26f, 1.0f };
    GfxColor fuse = { 0.78f, 0.72f, 0.60f, 1.0f };

    gfx_fill_circle(cx, cy + s * 0.14f, s * 0.70f, body);
    gfx_fill_circle(cx - s * 0.24f, cy - s * 0.10f, s * 0.22f,
                    rgba(1.0f, 1.0f, 1.0f, 0.16f));
    gfx_fill_rect(cx - s * 0.10f, cy - s * 0.86f, s * 0.20f, s * 0.26f, fuse);
    gfx_fill_rect(cx + s * 0.02f, cy - s * 1.02f, s * 0.34f, s * 0.14f, fuse);
    gfx_fill_circle(cx + s * 0.44f, cy - s * 0.96f, s * 0.15f, color);
}

void art_tower_icon(float cx, float cy, float size, int damageType)
{
    GfxColor color = art_damage_color(damageType, 1.0f);

    switch (damageType) {
    case DMG_FIRE:
        icon_fire(cx, cy, size, color);
        break;
    case DMG_ELECTRIC:
        icon_shock(cx, cy, size, color);
        break;
    case DMG_CORROSIVE:
        icon_corrosive(cx, cy, size, color);
        break;
    case DMG_SLAG:
        icon_slag(cx, cy, size, color);
        break;
    case DMG_EXPLOSIVE:
        icon_blaster(cx, cy, size, color);
        break;
    case DMG_NORMAL:
    default:
        icon_gun(cx, cy, size, color);
        break;
    }
}

GfxColor art_damage_color(int damageType, float alpha)
{
    GfxColor c;

    if (damageType < 0)
        damageType = 0;
    if (damageType >= TOWER_TYPE_COUNT)
        damageType = TOWER_TYPE_COUNT - 1;
    c = COL_DAMAGE_TYPE[damageType];
    c.a = alpha;
    return c;
}

GfxColor art_damage_text_color(int damageType)
{
    if (damageType < 0)
        damageType = 0;
    if (damageType >= TOWER_TYPE_COUNT)
        damageType = TOWER_TYPE_COUNT - 1;
    return COL_DAMAGE_TEXT[damageType];
}

GfxColor art_layer_color(int layerType, float alpha)
{
    GfxColor c;

    if (layerType < 0)
        layerType = 0;
    if (layerType >= MAX_LAYERS)
        layerType = MAX_LAYERS - 1;
    c = COL_LAYER[layerType];
    c.a = alpha;
    return c;
}

GfxColor art_layer_text_color(int layerType)
{
    if (layerType < 0)
        layerType = 0;
    if (layerType >= MAX_LAYERS)
        layerType = MAX_LAYERS - 1;
    return COL_LAYER_TEXT[layerType];
}

void art_star(float cx, float cy, float size, GfxColor color)
{
    float s = size;
    float xs[10] = { cx, cx + s * 0.3f, cx + s, cx + s * 0.4f, cx + s * 0.6f,
                     cx, cx - s * 0.6f, cx - s * 0.4f, cx - s, cx - s * 0.3f };
    float ys[10] = { cy - s, cy - s * 0.3f, cy - s * 0.3f, cy + s * 0.1f,
                     cy + s * 0.8f, cy + s * 0.4f, cy + s * 0.8f,
                     cy + s * 0.1f, cy - s * 0.3f, cy - s * 0.3f };

    poly_fill(xs, ys, 10, color);
}

void art_coin(float cx, float cy, float radius)
{
    gfx_fill_circle(cx, cy, radius, COL_GOLD);
    gfx_fill_circle(cx - radius * 0.18f, cy - radius * 0.18f, radius * 0.42f,
                    rgba(1.0f, 1.0f, 0.5f, 0.6f));
    gfx_outline_circle(cx, cy, radius, 1.0f, rgba(0.8f, 0.6f, 0.1f, 1.0f));
}

void art_heart(float cx, float cy, float size, GfxColor color)
{
    float r = size * 0.42f;

    gfx_fill_circle(cx - r * 1.05f, cy - r * 0.55f, r, color);
    gfx_fill_circle(cx + r * 1.05f, cy - r * 0.55f, r, color);
    gfx_fill_triangle(cx - r * 2.05f, cy - r * 0.15f,
                      cx + r * 2.05f, cy - r * 0.15f,
                      cx, cy + size * 0.85f, color);
}

void art_glow(float cx, float cy, float radius, GfxColor color, float alpha)
{
    int i;
    for (i = 4; i >= 1; i--) {
        float a = alpha * (float)i * 0.25f;
        float r = radius * (float)i * 0.25f;
        gfx_fill_circle(cx, cy, r, rgba(color.r, color.g, color.b, a));
    }
}

void art_health_bar(float cx, float y, float width, float height, float frac)
{
    float x = cx - width * 0.5f;

    if (frac < 0.0f)
        frac = 0.0f;
    else if (frac > 1.0f)
        frac = 1.0f;

    gfx_fill_rect(x, y, width, height, rgba(0.0f, 0.0f, 0.0f, 0.7f));
    gfx_fill_rect(x + 1.0f, y + 1.0f, (width - 2.0f) * frac, height - 2.0f,
                  rgba(1.0f - frac, frac, 0.0f, 1.0f));
}

/* ------------------------------------------------------------------------ */
/* Background                                                                */
/* ------------------------------------------------------------------------ */

void render_background_gradient(int width, int height)
{
    int bands = 24;
    int i;

    for (i = 0; i < bands; i++) {
        float t = (float)i / (float)(bands - 1);
        int y0 = (i * height) / bands;
        int y1 = ((i + 1) * height) / bands;
        GfxColor color = rgba(0.02f + t * 0.03f, 0.02f + t * 0.03f,
                              0.05f + t * 0.07f, 1.0f);
        gfx_fill_rect(0.0f, (float)y0, (float)width, (float)(y1 - y0 + 1), color);
    }
}

/* ------------------------------------------------------------------------ */
/* Title screen                                                              */
/* ------------------------------------------------------------------------ */

void render_menu_top(const App *app)
{
    static const GfxColor kShadow = { 0.25f, 0.15f, 0.03f, 0.9f };
    static const GfxColor kDefenseShadow = { 0.20f, 0.20f, 0.35f, 0.55f };
    static const GfxColor kDivider = { 0.30f, 0.20f, 0.10f, 0.5f };
    float pulse;
    float width;
    float x;
    int i;

    /* drifting element motes */
    for (i = 0; i < MENU_PARTICLE_COUNT; i++) {
        const MenuParticle *p = &app->particles[i];
        float alpha = p->life / p->maxLife;
        if (alpha <= 0.0f)
            continue;
        art_glow(p->x, p->y, p->size * 3.0f, rgba(p->r, p->g, p->b, 1.0f),
                 alpha * 0.22f);
        gfx_fill_rect(p->x, p->y, p->size, p->size,
                      rgba(p->r, p->g, p->b, alpha));
    }

    /* warm halo behind the title */
    art_glow(200.0f, 50.0f, 120.0f, COL_GOLD, 0.10f);

    /* layered title, like the original's shadow + glow text */
    text_centered(202.0f, 28.0f, 4.0f, kShadow, "ELEMENTAL");
    text_centered(200.0f, 26.0f, 4.0f, COL_GOLD_SOFT, "ELEMENTAL");

    text_centered(201.0f, 71.0f, 3.0f, kDefenseShadow, "DEFENSE");
    text_centered(200.0f, 70.0f, 3.0f, COL_WHITE, "DEFENSE");

    text_centered(200.0f, 103.0f, 1.0f, COL_MUTED, "TOWER DEFENSE");
    gfx_fill_rect(120.0f, 116.0f, 160.0f, 1.0f, kDivider);

    /* three representative tower types on the title screen */
    {
        static const int kMenuTypes[3] = { DMG_FIRE, DMG_ELECTRIC, DMG_SLAG };

        for (i = 0; i < 3; i++) {
            int type = kMenuTypes[i];
            float cx = 140.0f + (float)i * 60.0f;
            float bob = sinf(app->menuTime * 2.0f + (float)i * 1.7f) * 2.0f;
            float cy = 148.0f + bob;
            GfxColor color = art_damage_color(type, 1.0f);
            const TowerSpec *spec = game_tower_spec(type);

            art_glow(cx, cy, 30.0f, color, 0.20f);
            art_tower_icon(cx, cy, 14.0f, type);
            text_centered(cx, 168.0f, 1.0f, art_damage_text_color(type),
                          spec != NULL ? spec->name : "");
        }
    }

    /* "press start" prompt */
    pulse = 0.82f + 0.18f * (0.5f + 0.5f * sinf(app->menuTime * 3.0f));
    width = gfx_text_width("PRESS A OR TAP START", 1.0f) + 18.0f;
    x = floorf((float)TOP_W * 0.5f - width * 0.5f);
    gfx_fill_rect(x, 190.0f, width, 18.0f, rgba(0.04f, 0.04f, 0.10f, 0.85f));
    gfx_outline_rect(x, 190.0f, width, 18.0f, 1.0f,
                     rgba(0.40f, 0.35f, 0.15f, 0.45f));
    text_centered(200.0f, 195.0f, 1.0f,
                  rgba(1.0f, 1.0f, 1.0f, pulse), "PRESS A OR TAP START");

    text_centered(200.0f, 222.0f, 1.0f, rgba(0.74f, 0.77f, 0.90f, 0.85f),
                  "A NATIVE 3DS REMAKE");
}

/* ------------------------------------------------------------------------ */
/* Battlefield                                                               */
/* ------------------------------------------------------------------------ */

/* Maps board cells to the index of the path cell occupying them (-1 if none).
 * The core stores path points in cell space; cell centres are (col+0.5,
 * row+0.5), so flooring is enough to find the owning cell. */
static void build_path_map(const Game *g, int16_t map[GRID_ROWS][GRID_COLS])
{
    int i;

    memset(map, 0xFF, sizeof(int16_t) * GRID_ROWS * GRID_COLS);
    if (g->path.count <= 0)
        return;

    for (i = 0; i < g->path.count; i++) {
        int col = (int)floorf(g->path.pts[i].x);
        int row = (int)floorf(g->path.pts[i].y);
        if (col >= 0 && col < GRID_COLS && row >= 0 && row < GRID_ROWS)
            map[row][col] = (int16_t)i;
    }
}

static void draw_hud(const App *app)
{
    const Game *g = &app->game;
    char buf[32];
    const char *phaseTag;
    GfxColor phaseColor;
    int shownWave = g->wave > g->totalWaves ? g->totalWaves : g->wave;

    gfx_fill_rect(0.0f, 0.0f, (float)TOP_W, HUD_H, rgba(0.035f, 0.035f, 0.09f, 1.0f));
    gfx_fill_rect(0.0f, HUD_H - 1.0f, (float)TOP_W, 1.0f,
                  rgba(0.20f, 0.20f, 0.38f, 0.45f));

    /* gold */
    art_coin(14.0f, 19.0f, 6.0f);
    snprintf(buf, sizeof(buf), "%d", g->gold);
    gfx_text(26.0f, 11.0f, 2.0f, COL_GOLD, buf);

    /* lives */
    art_heart(100.0f, 19.0f, 6.0f, COL_RED);
    snprintf(buf, sizeof(buf), "%d", g->lives);
    gfx_text(112.0f, 11.0f, 2.0f, COL_WHITE, buf);

    /* phase tag + wave counter: the only status the playfield needs, kept in
     * the HUD strip so the board itself is never covered. */
    if (g->paused) {
        phaseTag = "PAUSED";
        phaseColor = rgba(0.80f, 0.80f, 0.85f, 1.0f);
    } else if (g->phase == PHASE_WAVE) {
        phaseTag = "WAVE";
        phaseColor = COL_WAVE_TAG;
    } else if (g->phase == PHASE_GAMEOVER) {
        phaseTag = "OVER";
        phaseColor = COL_RED;
    } else if (g->phase == PHASE_VICTORY) {
        phaseTag = "WIN";
        phaseColor = COL_GOLD;
    } else {
        phaseTag = "BUILD";
        phaseColor = COL_BUILD_TAG;
    }
    gfx_text(160.0f, 5.0f, 1.0f, phaseColor, phaseTag);
    snprintf(buf, sizeof(buf), "%d/%d", shownWave, g->totalWaves);
    gfx_text(160.0f, 17.0f, 2.0f, COL_WHITE, buf);

    /* alive / tower count */
    if (g->phase == PHASE_WAVE) {
        gfx_text(244.0f, 5.0f, 1.0f, COL_MUTED, "ALIVE");
        snprintf(buf, sizeof(buf), "%d", g->enemiesAlive);
        gfx_text(244.0f, 17.0f, 2.0f, COL_WHITE, buf);
    } else {
        gfx_text(244.0f, 5.0f, 1.0f, COL_MUTED, "TOWERS");
        snprintf(buf, sizeof(buf), "%d", g->towerCount);
        gfx_text(244.0f, 17.0f, 2.0f, COL_WHITE, buf);
    }

    /* speed */
    gfx_text(330.0f, 5.0f, 1.0f, COL_MUTED, "SPEED");
    snprintf(buf, sizeof(buf), "%.0fX", (double)g->speedMultiplier);
    gfx_text(330.0f, 17.0f, 2.0f, g->speedMultiplier > 1.0f ? COL_YELLOW : COL_WHITE,
             buf);
}

static void draw_board(const App *app, const int16_t pathMap[GRID_ROWS][GRID_COLS])
{
    const Game *g = &app->game;
    int col, row;

    /* board backing (2 px lip so the outer border reads) */
    gfx_fill_rect(BOARD_X - 2.0f, BOARD_Y - 2.0f, BOARD_W + 4.0f, BOARD_H + 4.0f,
                  rgba(0.03f, 0.03f, 0.06f, 1.0f));

    for (row = 0; row < GRID_ROWS; row++) {
        for (col = 0; col < GRID_COLS; col++) {
            float x = board_cell_left(col);
            float y = board_cell_top(row);
            int16_t pathIndex = pathMap[row][col];

            if (pathIndex >= 0) {
                float t = g->path.count > 1
                              ? (float)pathIndex / (float)(g->path.count - 1)
                              : 0.0f;
                gfx_fill_rect(x, y, BOARD_CELL, BOARD_CELL,
                              rgba(0.24f + t * 0.10f, 0.14f + t * 0.05f,
                                   0.05f + t * 0.03f, 1.0f));
            } else {
                gfx_fill_rect(x, y, BOARD_CELL, BOARD_CELL, COL_CELL);
            }
        }
    }

    /* Lighter shoulder lines where the road meets buildable cells: at 240p the
     * brown road alone is too low contrast against the dark board. */
    for (row = 0; row < GRID_ROWS; row++) {
        for (col = 0; col < GRID_COLS; col++) {
            float x;
            float y;

            if (pathMap[row][col] < 0)
                continue;
            x = board_cell_left(col);
            y = board_cell_top(row);

            if (row == 0 || pathMap[row - 1][col] < 0)
                gfx_fill_rect(x, y, BOARD_CELL, 1.0f, COL_PATH_EDGE);
            if (row == GRID_ROWS - 1 || pathMap[row + 1][col] < 0)
                gfx_fill_rect(x, y + BOARD_CELL - 1.0f, BOARD_CELL, 1.0f, COL_PATH_EDGE);
            if (col == 0 || pathMap[row][col - 1] < 0)
                gfx_fill_rect(x, y, 1.0f, BOARD_CELL, COL_PATH_EDGE);
            if (col == GRID_COLS - 1 || pathMap[row][col + 1] < 0)
                gfx_fill_rect(x + BOARD_CELL - 1.0f, y, 1.0f, BOARD_CELL,
                              COL_PATH_EDGE);
        }
    }

    /* tint the cells that hold a tower with the tower's damage-type colour */
    for (col = 0; col < g->towerCount; col++) {
        const Tower *tower = &g->towers[col];
        GfxColor tint;

        if (!tower->active)
            continue;
        tint = art_damage_color(tower->damageType, 0.06f);
        gfx_fill_rect(board_cell_left(tower->col), board_cell_top(tower->row),
                      BOARD_CELL, BOARD_CELL, tint);
    }

    /* grid lines across the whole board */
    for (col = 1; col < GRID_COLS; col++) {
        gfx_fill_rect(BOARD_X + (float)col * BOARD_CELL, BOARD_Y, 1.0f, BOARD_H,
                      COL_GRIDLINE);
    }
    for (row = 1; row < GRID_ROWS; row++) {
        gfx_fill_rect(BOARD_X, BOARD_Y + (float)row * BOARD_CELL, BOARD_W, 1.0f,
                      COL_GRIDLINE);
    }

    gfx_outline_rect(BOARD_X - 2.0f, BOARD_Y - 2.0f, BOARD_W + 4.0f, BOARD_H + 4.0f,
                     1.0f, rgba(0.10f, 0.10f, 0.20f, 0.30f));
}

static void tower_stats(const Tower *tower, float *damage, float *fireRate, float *range)
{
    game_tower_stats(tower->damageType, (int)tower->level, damage, fireRate, range);
}

/* Cheap ring built from thick line segments: used for enemy layers, status
 * outlines and explosion rings.  Segments are tuned for 6-8 px radii at 240p;
 * 3DS quads are cheap but this keeps a full 128-enemy wave well inside the
 * citro2d object budget. */
static void draw_ring(float cx, float cy, float radius, float thickness,
                      GfxColor color, int segments)
{
    int i;
    float step = 6.28318530718f / (float)segments;

    if (radius <= 0.0f || segments < 3)
        return;

    for (i = 0; i < segments; i++) {
        float a0 = (float)i * step;
        float a1 = (float)(i + 1) * step;

        gfx_line(cx + cosf(a0) * radius, cy + sinf(a0) * radius,
                 cx + cosf(a1) * radius, cy + sinf(a1) * radius, thickness, color);
    }
}

/* Range circles for the selected tower (bright) and the tower under the cursor
 * (dim).  The selected tower's cell also gets an outline. */
static void draw_range_previews(const App *app)
{
    const Game *g = &app->game;
    int selected = -1;
    int cursorTower;
    float damage, fireRate, range;

    if (g->selectedTower >= 0 && g->selectedTower < g->towerCount &&
        g->towers[g->selectedTower].active)
        selected = g->selectedTower;

    cursorTower = game_tower_at(g, g->cursorCol, g->cursorRow);

    if (cursorTower >= 0 && cursorTower != selected &&
        cursorTower < g->towerCount && g->towers[cursorTower].active) {
        const Tower *tower = &g->towers[cursorTower];
        GfxColor color = art_damage_color(tower->damageType, 1.0f);

        tower_stats(tower, &damage, &fireRate, &range);
        gfx_fill_circle(cell_px_x((float)tower->col + 0.5f),
                        cell_px_y((float)tower->row + 0.5f),
                        range * BOARD_CELL, rgba(color.r, color.g, color.b, 0.04f));
    }

    if (selected >= 0) {
        const Tower *tower = &g->towers[selected];
        GfxColor color = art_damage_color(tower->damageType, 1.0f);
        float cx = cell_px_x((float)tower->col + 0.5f);
        float cy = cell_px_y((float)tower->row + 0.5f);

        tower_stats(tower, &damage, &fireRate, &range);
        gfx_fill_circle(cx, cy, range * BOARD_CELL,
                        rgba(color.r, color.g, color.b, 0.06f));
        gfx_outline_circle(cx, cy, range * BOARD_CELL, 1.0f,
                           rgba(color.r, color.g, color.b, 0.32f));
        gfx_outline_rect(board_cell_left(tower->col) + 0.5f,
                         board_cell_top(tower->row) + 0.5f,
                         BOARD_CELL - 1.0f, BOARD_CELL - 1.0f, 2.0f, color);
    }
}

/* Acid puddles: translucent green blobs with a brighter rim, fading with the
 * remaining life.  Drawn under towers and enemies. */
static void draw_puddles(const App *app)
{
    const Game *g = &app->game;
    int i;

    for (i = 0; i < MAX_PUDDLES; i++) {
        const Puddle *puddle = &g->puddles[i];
        float frac;
        float cx, cy, radius;

        if (!puddle->active)
            continue;
        frac = puddle->maxLife > 0.0f ? puddle->life / puddle->maxLife : 0.0f;
        if (frac <= 0.0f)
            continue;
        if (frac > 1.0f)
            frac = 1.0f;

        cx = cell_px_x(puddle->x);
        cy = cell_px_y(puddle->y);
        radius = puddle->radius * BOARD_CELL;

        gfx_fill_circle(cx, cy, radius,
                        rgba(COL_PUDDLE.r, COL_PUDDLE.g, COL_PUDDLE.b,
                             0.10f + 0.18f * frac));
        gfx_outline_circle(cx, cy, radius, 1.0f,
                           rgba(COL_PUDDLE_RIM.r, COL_PUDDLE_RIM.g,
                                COL_PUDDLE_RIM.b, 0.30f + 0.45f * frac));
    }
}

static void draw_towers(const App *app)
{
    const Game *g = &app->game;
    int i;

    for (i = 0; i < g->towerCount; i++) {
        const Tower *tower = &g->towers[i];
        const TowerSpec *spec;
        GfxColor color;
        GfxColor glow;
        float cx, cy, size, pulse, levelFrac;

        if (!tower->active || tower->col < 0 || tower->row < 0)
            continue;

        spec = game_tower_spec(tower->damageType);
        color = art_damage_color(tower->damageType, 1.0f);
        glow = spec != NULL ? art_spec_color(spec->glow, 1.0f) : color;

        cx = cell_px_x((float)tower->col + 0.5f);
        cy = cell_px_y((float)tower->row + 0.5f);
        /* icon radius by tier, matching the original 12.4..16 px at 28 px
         * cells scaled to our 15 px cells (design.md 11.1) */
        size = tower->level >= TOWER_MAX_LEVEL ? 9.0f
                                               : (tower->level >= 5 ? 8.0f : 7.0f);
        pulse = 0.5f + 0.5f * sinf(app->time * 2.4f + tower->animPhase);

        art_glow(cx, cy, size * 2.2f, glow, 0.09f + 0.05f * pulse);

        /* a little platform under the glyph */
        gfx_fill_rect(cx - 5.5f, cy + size * 0.55f, 11.0f, 2.0f,
                      rgba(0.0f, 0.0f, 0.0f, 0.35f));

        art_tower_icon(cx, cy, size, tower->damageType);

        /* level bar: 1 px progress line pinned to the bottom of the cell, so it
         * never collides with the glyph or the neighbouring row */
        levelFrac = (float)tower->level / (float)TOWER_MAX_LEVEL;
        if (levelFrac > 1.0f)
            levelFrac = 1.0f;
        {
            float barX = board_cell_left(tower->col) + 2.0f;
            float barY = board_cell_top(tower->row) + BOARD_CELL - 2.0f;

            gfx_fill_rect(barX, barY, 11.0f, 1.0f, rgba(0.0f, 0.0f, 0.0f, 0.60f));
            gfx_fill_rect(barX, barY, 11.0f * levelFrac, 1.0f,
                          tower->level >= TOWER_MAX_LEVEL ? COL_GOLD_SOFT : COL_GOLD);
        }

        /* max level towers get a golden cap, like the original */
        if (tower->level >= TOWER_MAX_LEVEL) {
            gfx_fill_rect(cx - 3.0f, cy - size - 4.0f, 6.0f, 2.0f,
                          rgba(1.0f, 1.0f, 0.4f, 0.9f));
        }
    }
}

static void draw_beams(const App *app)
{
    const Game *g = &app->game;
    int i;

    for (i = 0; i < MAX_BEAMS; i++) {
        const Beam *beam = &g->beams[i];
        GfxColor color;
        float alpha;
        float x1, y1, x2, y2;

        if (!beam->active)
            continue;

        alpha = beam->maxLife > 0.0f ? beam->life / beam->maxLife : 0.0f;
        if (alpha <= 0.0f)
            continue;
        if (alpha > 1.0f)
            alpha = 1.0f;

        color = art_damage_color(beam->damageType, 1.0f);

        x1 = cell_px_x(beam->x1);
        y1 = cell_px_y(beam->y1);
        x2 = cell_px_x(beam->x2);
        y2 = cell_px_y(beam->y2);

        if (beam->kind == 1) {
            /* explosive: expanding ring on the target instead of a line */
            float progress = 1.0f - alpha;
            float radius = beam->radius * BOARD_CELL * progress;

            draw_ring(x2, y2, radius, 2.0f,
                      rgba(color.r, color.g, color.b, alpha * 0.90f), 14);
            draw_ring(x2, y2, radius * 0.62f, 1.0f,
                      rgba(1.0f, 1.0f, 1.0f, alpha * 0.40f), 12);
            return;
        }

        /* soft wide pass first, then the bright core (1 px core / 3 px glow) */
        gfx_line(x1, y1, x2, y2, 3.0f,
                 rgba(color.r, color.g, color.b, alpha * 0.14f));
        gfx_line(x1, y1, x2, y2, 1.0f,
                 rgba(color.r, color.g, color.b, alpha));
        gfx_line(x1, y1, x2, y2, 1.0f,
                 rgba(1.0f, 1.0f, 1.0f, alpha * 0.55f));
    }
}

static void draw_enemies(const App *app)
{
    /* outer -> inner ring radii.  Nudged up from the design-spec 6.0/4.2/2.4 at
     * 1.5 px for the 240p LCD: 6.5/4.6/2.7 at 2 px reads clearly at 1x on the
     * 3DS screen without leaving the 15 px cell. */
    static const float kLayerRadius[MAX_LAYERS] = { 6.5f, 4.6f, 2.7f };
    const Game *g = &app->game;
    int i, l;
    int shown = 0;

    for (i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *enemy = &g->enemies[i];
        float cx, cy;
        int layers;
        int innermost = -1;
        float innermostFrac = 0.0f;

        if (!enemy->active)
            continue;
        if (++shown > MAX_ENEMIES)
            break;

        cx = cell_px_x(enemy->x);
        cy = cell_px_y(enemy->y);
        layers = enemy->layerCount;
        if (layers < 1)
            layers = 1;
        if (layers > MAX_LAYERS)
            layers = MAX_LAYERS;

        if (enemy->elite)
            art_glow(cx, cy, 14.0f, COL_GOLD, 0.14f);

        /* concentric layer rings, outside-in; destroyed layers vanish */
        for (l = 0; l < layers; l++) {
            const EnemyLayer *layer = &enemy->layers[l];
            float frac;
            GfxColor color;

            if (layer->maxHp <= 0.0f)
                continue;
            frac = layer->hp / layer->maxHp;
            if (frac <= 0.0f)
                continue;
            if (frac > 1.0f)
                frac = 1.0f;

            color = art_layer_color(layer->type, 0.45f + 0.55f * frac);
            draw_ring(cx, cy, kLayerRadius[l], 2.0f, color, 12);
            innermost = l;
            innermostFrac = frac;
        }

        /* coloured core dot so the current layer colour is obvious at a glance */
        if (innermost >= 0) {
            GfxColor coreColor =
                art_layer_color(enemy->layers[innermost].type,
                                0.45f + 0.55f * innermostFrac);
            gfx_fill_circle(cx, cy, 2.3f, coreColor);
        }

        /* white hit flash */
        if (enemy->hitFlash > 0.0f) {
            float flash = enemy->hitFlash * 2.5f;
            if (flash > 0.80f)
                flash = 0.80f;
            gfx_fill_circle(cx, cy, 6.2f, rgba(1.0f, 1.0f, 1.0f, flash));
        }

        /* elite: extra white outline ring */
        if (enemy->elite)
            draw_ring(cx, cy, 7.5f, 1.0f, rgba(1.0f, 1.0f, 1.0f, 0.64f), 12);

        /* slag: pulsing purple outline */
        if (enemy->slagTimer > 0.0f) {
            float pulse = 0.45f + 0.35f * (0.5f + 0.5f * sinf(app->time * 10.0f));
            draw_ring(cx, cy, 8.4f, 1.0f,
                      rgba(0.80f, 0.45f, 1.0f, pulse), 12);
        }

        /* stunned: white flash */
        if (enemy->stunTimer > 0.0f) {
            float pulse = 0.16f + 0.26f * (0.5f + 0.5f * sinf(app->time * 24.0f));
            gfx_fill_circle(cx, cy, 6.5f,
                            rgba(COL_STUN_SPARK.r, COL_STUN_SPARK.g,
                                 COL_STUN_SPARK.b, pulse));
        }

        /* burn: small orange motes, one per stack (capped for readability) */
        if (enemy->burnStacks > 0) {
            int stacks = enemy->burnStacks > 5 ? 5 : (int)enemy->burnStacks;
            int m;

            for (m = 0; m < stacks; m++) {
                float angle = (float)m * 1.7f + app->time * 6.0f;
                float rad = 4.5f + (float)(m % 2) * 1.6f;
                float mx = cx + cosf(angle) * rad;
                float my = cy - 1.0f + sinf(angle) * rad * 0.75f;

                gfx_fill_rect(floorf(mx), floorf(my), 2.0f, 2.0f, COL_BURN_MOTE);
            }
        }
    }
}

static void draw_cursor(const App *app)
{
    const Game *g = &app->game;
    float x = board_cell_left(g->cursorCol);
    float y = board_cell_top(g->cursorRow);
    int tower = game_tower_at(g, g->cursorCol, g->cursorRow);
    int canPlace = game_can_place_at(g, g->cursorCol, g->cursorRow, g->selectedType);
    const TowerSpec *spec = game_tower_spec(g->selectedType);
    int affordable = spec != NULL && g->gold >= spec->cost;
    const float corner = 2.0f;
    GfxColor edge;

    gfx_fill_rect(x, y, BOARD_CELL, BOARD_CELL, rgba(1.0f, 1.0f, 1.0f, 0.07f));

    /* ghost of the selected tower type on an empty, buildable cell */
    if (tower < 0 && canPlace) {
        GfxColor ghost = art_damage_color(g->selectedType, 0.28f);
        GfxColor rim = art_damage_color(g->selectedType, 0.55f);
        float gx = x + BOARD_CELL * 0.5f;

        gfx_fill_circle(gx, y + BOARD_CELL * 0.5f, 4.4f, ghost);
        gfx_outline_circle(gx, y + BOARD_CELL * 0.5f, 5.4f, 1.0f, rim);
    }

    /* green = can build here, yellow = legal but unaffordable, white = tower,
     * dim = blocked cell (start/goal, or the placement would seal the route) */
    if (tower >= 0)
        edge = rgba(1.0f, 1.0f, 1.0f, 0.45f);
    else if (canPlace && affordable)
        edge = rgba(0.40f, 1.00f, 0.40f, 0.55f);
    else if (canPlace)
        edge = rgba(1.00f, 0.85f, 0.30f, 0.55f);
    else
        edge = rgba(0.70f, 0.70f, 0.80f, 0.25f);

    gfx_outline_rect(x + 0.5f, y + 0.5f, BOARD_CELL - 1.0f, BOARD_CELL - 1.0f,
                     1.0f, edge);

    /* corner brackets make the cursor readable over busy tiles */
    gfx_fill_rect(x, y, corner, corner, rgba(1.0f, 1.0f, 1.0f, 0.75f));
    gfx_fill_rect(x + BOARD_CELL - corner, y, corner, corner,
                  rgba(1.0f, 1.0f, 1.0f, 0.75f));
    gfx_fill_rect(x, y + BOARD_CELL - corner, corner, corner,
                  rgba(1.0f, 1.0f, 1.0f, 0.75f));
    gfx_fill_rect(x + BOARD_CELL - corner, y + BOARD_CELL - corner, corner, corner,
                  rgba(1.0f, 1.0f, 1.0f, 0.75f));
}

/* ------------------------------------------------------------------------ */
/* Path entrance / exit markers                                              */
/* ------------------------------------------------------------------------ */

/* One small arrow pointing along the direction of travel.  The dark backing
 * triangle keeps it readable on the brown road.  Cheap: 2 quads. */
static void draw_portal_arrow(float cx, float cy, float dx, float dy, GfxColor color)
{
    float len = sqrtf(dx * dx + dy * dy);
    float ux = 1.0f;
    float uy = 0.0f;
    float px, py;
    float apexX, apexY, baseX, baseY;

    if (len > 0.0001f) {
        ux = dx / len;
        uy = dy / len;
    }
    px = -uy;
    py = ux;

    apexX = cx + ux * 6.0f;
    apexY = cy + uy * 6.0f;
    baseX = cx - ux * 3.0f;
    baseY = cy - uy * 3.0f;

    gfx_fill_triangle(apexX + ux, apexY + uy,
                      baseX + px * 5.5f, baseY + py * 5.5f,
                      baseX - px * 5.5f, baseY - py * 5.5f,
                      rgba(0.0f, 0.0f, 0.0f, 0.55f));
    gfx_fill_triangle(apexX, apexY,
                      baseX + px * 4.5f, baseY + py * 4.5f,
                      baseX - px * 4.5f, baseY - py * 4.5f, color);
}

/* Green arrow where creeps enter the board, red arrow where they leave.  The
 * road is re-routed by the core when towers are built on it, so these have to
 * follow g->path.pts rather than fixed board corners. */
static void draw_portal_markers(const Game *g)
{
    Vec2 first, second, last, prev;

    if (g->path.count < 1)
        return;

    first = g->path.pts[0];
    last = g->path.pts[g->path.count - 1];
    second = g->path.count > 1 ? g->path.pts[1] : first;
    prev = g->path.count > 1 ? g->path.pts[g->path.count - 2] : last;

    draw_portal_arrow(cell_px_x(first.x), cell_px_y(first.y),
                      second.x - first.x, second.y - first.y,
                      rgba(0.35f, 1.00f, 0.45f, 1.0f));
    draw_portal_arrow(cell_px_x(last.x), cell_px_y(last.y),
                      last.x - prev.x, last.y - prev.y,
                      rgba(1.00f, 0.35f, 0.30f, 1.0f));
}

void render_game_top(const App *app)
{
    const Game *g = &app->game;
    int16_t pathMap[GRID_ROWS][GRID_COLS];

    build_path_map(g, pathMap);

    draw_hud(app);
    draw_board(app, pathMap);
    draw_puddles(app);          /* ground decals: under towers and enemies */
    draw_portal_markers(g);
    draw_range_previews(app);
    draw_towers(app);
    draw_beams(app);
    draw_enemies(app);
    draw_cursor(app);
}

/* ------------------------------------------------------------------------ */
/* Overlays                                                                  */
/* ------------------------------------------------------------------------ */

void render_pause_overlay_top(void)
{
    gfx_fill_rect(0.0f, 0.0f, (float)TOP_W, (float)TOP_H,
                  rgba(0.0f, 0.0f, 0.0f, 0.62f));
    gfx_fill_rect(96.0f, 72.0f, 208.0f, 92.0f, COL_PANEL);
    gfx_outline_rect(96.0f, 72.0f, 208.0f, 92.0f, 1.0f, COL_BORDER_HI);

    text_centered(200.0f, 86.0f, 3.0f, COL_WHITE, "PAUSED");
    text_centered(200.0f, 120.0f, 1.0f, COL_MUTED, "START TO RESUME");
    text_centered(200.0f, 136.0f, 1.0f, COL_DIM, "SELECT CHANGES SPEED");
}

void render_result_overlay_top(const App *app)
{
    const Game *g = &app->game;
    bool victory = g->phase == PHASE_VICTORY;
    char buf[40];
    int seconds;
    int shownWave = g->wave > g->totalWaves ? g->totalWaves : g->wave;

    gfx_fill_rect(0.0f, 0.0f, (float)TOP_W, (float)TOP_H,
                  rgba(0.0f, 0.0f, 0.0f, 0.78f));
    gfx_fill_rect(60.0f, 56.0f, 280.0f, 136.0f, COL_PANEL);
    gfx_outline_rect(60.0f, 56.0f, 280.0f, 136.0f, 1.0f,
                     victory ? rgba(1.0f, 0.8f, 0.2f, 0.6f)
                             : rgba(1.0f, 0.2f, 0.2f, 0.6f));

    text_centered(200.0f, 68.0f, 3.0f,
                  victory ? COL_GOLD_SOFT : COL_RED,
                  victory ? "VICTORY!" : "GAME OVER");

    snprintf(buf, sizeof(buf), "WAVE %d/%d", shownWave, g->totalWaves);
    text_centered(200.0f, 108.0f, 1.0f, COL_WHITE, buf);

    snprintf(buf, sizeof(buf), "KILLS %d   LEAKS %d", g->kills, g->leaks);
    text_centered(200.0f, 122.0f, 1.0f, COL_MUTED, buf);

    seconds = (int)g->timeTotal;
    if (seconds < 0)
        seconds = 0;
    snprintf(buf, sizeof(buf), "TIME %d:%02d   GOLD %d", seconds / 60, seconds % 60,
             g->gold);
    text_centered(200.0f, 136.0f, 1.0f, COL_MUTED, buf);

    text_centered(200.0f, 164.0f, 1.0f, rgba(0.85f, 0.85f, 0.95f, 0.85f),
                  "A: PLAY AGAIN    B: MENU");
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                               */
/* ------------------------------------------------------------------------ */

void render_top(const App *app)
{
    const Game *g = &app->game;

    render_background_gradient(TOP_W, TOP_H);

    if (g->phase == PHASE_MENU) {
        render_menu_top(app);
        return;
    }

    render_game_top(app);

    if (g->phase == PHASE_GAMEOVER || g->phase == PHASE_VICTORY) {
        render_result_overlay_top(app);
    } else if (g->paused) {
        render_pause_overlay_top();
    }
}
