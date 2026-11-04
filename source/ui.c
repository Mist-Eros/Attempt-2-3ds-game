/*
 * Elemental Defense - bottom screen (touch) UI
 *
 * Layout is hand-packed into 320x240 and optimised for the 8x8 bitmap font at
 * scale 1 (labels) and scale 2 (numbers / main buttons).  Every interactive
 * element has a matching ui_*_rect() function so the drawing code and the hit
 * test can never drift apart, and so the PC preview harness can click buttons
 * by name.
 *
 * Vertical budget for the play screens:
 *   0  .. 31    header: gold | lives | wave | speed badge | pause badge
 *   34 .. 87    tower palette: six damage types in 2 rows x 3 (96x25 each)
 *   89 .. 155   info panel (selected/cursor tower: stats, effect, UPGRADE/SELL)
 *   159.. 236   NEXT WAVE + HP/SHIELD/ARMOR/elite preview   (build phase)
 *               wave progress + stats                       (wave phase)
 *
 * Placement note: the top screen is not touch sensitive on a 3DS, so towers
 * are built by moving the D-pad cursor and pressing A.  The bottom palette is
 * still tappable because it selects which tower type A will build.
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

#define HEADER_H 32.0f
#define PALETTE_Y 34.0f
#define PALETTE_ROW_H 25.0f
#define PALETTE_ROW_GAP 3.0f
#define PANEL_X 4.0f
#define PANEL_Y 89.0f
#define PANEL_W 312.0f
#define PANEL_H 66.0f
#define BOTTOM_Y 159.0f
#define BOTTOM_H 77.0f

bool ui_rect_contains(UiRect r, float x, float y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

UiRect ui_start_rect(void)
{
    UiRect r = { 44.0f, 126.0f, 232.0f, 64.0f };
    return r;
}

/* Six tower types in a 2x3 grid: 96x25 px cells, 8 px gaps, rows start at
 * y=34 and y=62 (palette ends at 87, info panel starts at 89). */
UiRect ui_tower_button_rect(int type)
{
    UiRect r;

    if (type < 0)
        type = 0;
    if (type >= TOWER_TYPE_COUNT)
        type = TOWER_TYPE_COUNT - 1;

    r.w = 96.0f;
    r.h = PALETTE_ROW_H;
    r.x = 6.0f + (float)(type % 3) * 104.0f;
    r.y = PALETTE_Y + (float)(type / 3) * (PALETTE_ROW_H + PALETTE_ROW_GAP);
    return r;
}

/* Effect line lives between the stats row and the buttons. */
UiRect ui_upgrade_rect(void)
{
    UiRect r = { 8.0f, 126.0f, 150.0f, 25.0f };
    return r;
}

UiRect ui_sell_rect(void)
{
    UiRect r = { 164.0f, 126.0f, 148.0f, 25.0f };
    return r;
}

UiRect ui_next_wave_rect(void)
{
    UiRect r = { 6.0f, BOTTOM_Y, 190.0f, BOTTOM_H };
    return r;
}

UiRect ui_speed_rect(void)
{
    UiRect r = { 244.0f, 2.0f, 34.0f, 28.0f };
    return r;
}

UiRect ui_pause_rect(void)
{
    UiRect r = { 282.0f, 2.0f, 34.0f, 28.0f };
    return r;
}

/* The wave-phase row no longer carries its own speed/pause buttons (two
 * controls for one action was confusing); the header badges above are the one
 * touch control set.  These accessors are kept as aliases so the preview
 * harness / earlier callers do not need to change. */
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
/* Drawing helpers                                                           */
/* ------------------------------------------------------------------------ */

static GfxColor rgba(float r, float g, float b, float a)
{
    return gfx_rgba(r, g, b, a);
}

static void fill_panel(UiRect r)
{
    /* BL2 chamfered panel: near-black body, gold trim, dark keyline, shadow.
     * The rect is exactly the old panel rect, so no hitbox ever moves. */
    art_panel(r.x, r.y, r.w, r.h, 6.0f, COL_GOLD, true);
}

static void panel_with_title(UiRect r, const char *title)
{
    fill_panel(r);
    if (title != NULL)
        text_centered(r.x + r.w * 0.5f, r.y + 3.0f, 1.0f, COL_MUTED, title);
}

static void draw_button_frame(UiRect r, GfxColor fill, GfxColor border, bool enabled)
{
    GfxColor body = fill;
    GfxColor edge = border;

    if (!enabled) {
        body.a *= 0.58f;
        edge.a *= 0.58f;
    }

    /* chamfered BL2 action button: trim follows the cut corners */
    art_panel(r.x, r.y, r.w, r.h, 4.0f, edge, true);
    gfx_fill_rect(r.x + 4.0f, r.y + 3.0f, r.w - 8.0f, r.h - 6.0f, body);
    gfx_fill_rect(r.x + 6.0f, r.y + 4.0f, r.w - 12.0f, 2.0f,
                  rgba(1.0f, 1.0f, 1.0f, enabled ? 0.14f : 0.06f));
}

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

/* One row of the "incoming" panel: a coloured layer dot + "LABEL xCOUNT".
 * The elite row uses the gold star instead of a layer dot. */
static void draw_preview_row(float x, float y, int count, int layerType, bool elite)
{
    char buf[24];
    GfxColor color;

    if (elite) {
        art_star(x + 8.0f, y + 4.0f, 4.0f, COL_GOLD);
        color = COL_GOLD;
    } else {
        if (layerType < 0 || layerType >= MAX_LAYERS)
            layerType = 0;
        color = art_layer_text_color(layerType);
        gfx_fill_circle(x + 8.0f, y + 4.0f, 3.0f,
                        art_layer_color(layerType, 0.95f));
        gfx_outline_circle(x + 8.0f, y + 4.0f, 3.6f, 1.0f,
                           rgba(0.0f, 0.0f, 0.0f, 0.75f));
    }

    snprintf(buf, sizeof(buf), "%s x%d", elite ? "ELITE" : LAYER_NAMES[layerType],
             count);
    gfx_text(x + 16.0f, y, 1.0f, color, buf);
}

/* ------------------------------------------------------------------------ */
/* Menu screen                                                               */
/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/* Menu screen (Borderlands-style: comic title, emblem row, chunky START)    */
/* ------------------------------------------------------------------------ */

/* Chunky BL2 START button: shared comic-button helper with a red face and the
 * usual black outline + gold trim + hazard-stripe caps. */
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

    /* thin gold rule with hazard-stripe ends */
    art_hazard_strip(24.0f, 55.0f, 10.0f, 6.0f);
    art_hazard_strip(286.0f, 55.0f, 10.0f, 6.0f);
    gfx_fill_rect(38.0f, 57.0f, 244.0f, 2.0f, COL_GOLD);

    /* compact element card strip echoing the top-screen weapon cards */
    active = (int)(app->menuTime * 0.8f + 2.0f) % 5;
    pulse = 0.5f + 0.5f * sinf(app->menuTime * 2.6f);
    for (i = 0; i < 5; i++) {
        int type = kCardTypes[i];
        float x = 12.0f + (float)i * (cardW + 4.0f);
        float y = cardTop;
        float cx = x + cardW * 0.5f;
        GfxColor color = art_damage_color(type, 1.0f);
        bool isActive = (i == active);

        if (isActive) {
            y -= 2.0f;
            art_glow(cx, y + cardH * 0.5f, 32.0f, color, 0.10f + 0.08f * pulse);
        }
        art_panel(x, y, cardW, cardH, 5.0f,
                  rgba(color.r, color.g, color.b, isActive ? 0.90f : 0.42f), true);
        /* element header strip, shaped to respect the chamfer */
        gfx_fill_rect(x + 5.0f, y, cardW - 10.0f, 8.0f,
                      rgba(color.r, color.g, color.b, isActive ? 1.0f : 0.72f));
        gfx_fill_rect(x, y + 5.0f, cardW, 3.0f,
                      rgba(color.r, color.g, color.b, isActive ? 1.0f : 0.72f));
        art_glow(cx, y + 30.0f, 18.0f, color, isActive ? 0.22f : 0.10f);
        art_weapon_emblem(cx, y + 30.0f, isActive ? 11.5f : 10.5f, type);
    }

    pulse = 0.70f + 0.30f * (0.5f + 0.5f * sinf(app->menuTime * 3.0f));
    draw_start_button(pulse);

    text_centered(160.0f, 202.0f, 1.0f, COL_DIM,
                  "ATTEMPT 2 - 3DS HOMEBREW");
    text_centered(160.0f, 220.0f, 1.0f, COL_DIM,
                  "D-PAD TO BUILD - TOUCH FOR BUTTONS");
}

/* ------------------------------------------------------------------------ */
/* Play header                                                               */
/* ------------------------------------------------------------------------ */

static void draw_header(const App *app)
{
    const Game *g = &app->game;
    char buf[32];
    UiRect speed = ui_speed_rect();
    UiRect pause = ui_pause_rect();
    float speedCx = speed.x + speed.w * 0.5f;
    GfxColor badgeTrim = rgba(0.62f, 0.50f, 0.16f, 0.95f);

    /* BL2 header band: near-black body, gold trim along the bottom edge and
     * hazard ticks between the stat groups. */
    gfx_fill_rect(0.0f, 0.0f, (float)BOT_W, HEADER_H,
                  rgba(0.04f, 0.04f, 0.075f, 1.0f));
    art_hazard_strip(90.0f, 10.0f, 4.0f, 11.0f);
    art_hazard_strip(148.0f, 10.0f, 4.0f, 11.0f);
    gfx_fill_rect(0.0f, HEADER_H - 4.0f, (float)BOT_W, 1.0f,
                  rgba(0.12f, 0.10f, 0.06f, 0.95f));
    gfx_fill_rect(0.0f, HEADER_H - 2.0f, (float)BOT_W, 2.0f, COL_GOLD);

    /* gold */
    art_coin(14.0f, 16.0f, 6.0f);
    snprintf(buf, sizeof(buf), "%d", g->gold);
    gfx_text(26.0f, 8.0f, 2.0f, COL_GOLD, buf);

    /* lives */
    art_heart(100.0f, 16.0f, 6.0f, COL_RED);
    snprintf(buf, sizeof(buf), "%d", g->lives);
    gfx_text(112.0f, 8.0f, 2.0f, COL_WHITE, buf);

    /* wave: single line so the two header buttons fit on the right */
    gfx_text(160.0f, 12.0f, 1.0f, COL_MUTED, "WAVE");
    snprintf(buf, sizeof(buf), "%d/%d", g->wave, g->totalWaves);
    gfx_text(200.0f, 12.0f, 1.0f, COL_WHITE, buf);

    /* speed badge: the one and only touch control for game speed */
    art_panel(speed.x, speed.y, speed.w, speed.h, 3.0f, badgeTrim, true);
    speed_text(g->speedMultiplier, buf, sizeof(buf));
    text_centered(speedCx, speed.y + 7.0f, 2.0f,
                  g->speedMultiplier > 1.0f ? COL_YELLOW : COL_WHITE, buf);

    /* pause badge */
    art_panel(pause.x, pause.y, pause.w, pause.h, 3.0f, badgeTrim, true);
    gfx_fill_rect(pause.x + 10.0f, pause.y + 8.0f, 5.0f, 12.0f, COL_WHITE);
    gfx_fill_rect(pause.x + 19.0f, pause.y + 8.0f, 5.0f, 12.0f, COL_WHITE);
}

/* ------------------------------------------------------------------------ */
/* Tower palette (six damage types, 2 x 3)                                   */
/* ------------------------------------------------------------------------ */

/* Display names follow docs/elements-spec.md §2 (the core spec names for #5 is
 * "BLASTER" but the design calls that damage type EXPLOSIVE, and the panel uses
 * the damage-type names, so the whole UI stays consistent).  9 chars max:
 * x+21 + 9*8 = x+93 fits the 96 px button. */
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
        GfxColor glow = spec != NULL ? art_spec_color(spec->glow, 1.0f) : color;
        bool selected = g->selectedType == i;

        if (selected)
            art_glow(r.x + r.w * 0.5f, r.y + r.h * 0.5f, 50.0f, glow, 0.14f);

        /* chamfered BL2 cell: selected = gold trim, unselected = dark trim plus
         * a damage-type colour tab on the left edge */
        art_panel(r.x, r.y, r.w, r.h, 5.0f,
                  selected ? COL_GOLD : rgba(0.20f, 0.19f, 0.32f, 0.90f), true);
        if (selected) {
            gfx_fill_rect(r.x + 3.0f, r.y + 4.0f, r.w - 6.0f, r.h - 8.0f,
                          rgba(color.r, color.g, color.b, 0.16f));
            gfx_fill_rect(r.x + 6.0f, r.y + r.h - 4.0f, r.w - 12.0f, 2.0f,
                          rgba(color.r, color.g, color.b, 0.95f));
        } else {
            gfx_fill_rect(r.x + 3.0f, r.y + 4.0f, 3.0f, r.h - 8.0f,
                          rgba(color.r, color.g, color.b, 0.80f));
        }

        /* narrow icon column so the full 9-character names fit */
        art_glow(r.x + 12.0f, r.y + 13.0f, 9.0f, glow, selected ? 0.30f : 0.14f);
        art_weapon_emblem(r.x + 12.0f, r.y + 13.0f, 4.5f, i);

        gfx_text(r.x + 18.0f, r.y + 4.0f, 1.0f,
                 selected ? COL_WHITE : COL_MUTED, kTowerNames[i]);

        art_coin(r.x + 26.0f, r.y + 19.0f, 2.5f);
        snprintf(buf, sizeof(buf), "%d", cost);
        gfx_text(r.x + 31.0f, r.y + 15.0f, 1.0f,
                 g->gold >= cost ? COL_GOLD : rgba(0.90f, 0.82f, 0.60f, 0.95f), buf);
    }
}

/* ------------------------------------------------------------------------ */
/* Info / upgrade panel                                                      */
/* ------------------------------------------------------------------------ */

/* Short description of a damage type's signature effect (elements-spec §2).
 * Plain letters only: the 8x8 font has no middot/arrow glyphs. */
static void tower_effect_text(int damageType, char *buf, size_t size)
{
    switch (damageType) {
    case DMG_FIRE:
        snprintf(buf, size, "BURNS - 2x vs HP");
        break;
    case DMG_ELECTRIC:
        snprintf(buf, size, "STUNS - 2x vs SHIELD");
        break;
    case DMG_CORROSIVE:
        snprintf(buf, size, "ACID PUDDLE - 2x vs ARMOR");
        break;
    case DMG_SLAG:
        snprintf(buf, size, "SLAG: +50%% DMG TAKEN");
        break;
    case DMG_EXPLOSIVE:
        snprintf(buf, size, "AOE 1.3 - 3x SLOWER");
        break;
    case DMG_NORMAL:
    default:
        snprintf(buf, size, "NO SPECIAL EFFECT");
        break;
    }
}

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
    draw_button_frame(up, rgba(0.10f, 0.35f, 0.15f, 1.0f),
                      rgba(0.30f, 0.90f, 0.40f, 0.80f), canUpgrade);
    text_centered(up.x + up.w * 0.5f, up.y + 9.0f, 1.0f,
                  upgradeCost < 0 ? COL_GOLD_SOFT
                                  : (canUpgrade ? COL_WHITE : COL_DIM),
                  buf);

    if (sellValue < 0)
        sellValue = 0;
    snprintf(buf, sizeof(buf), "SELL +%dG", sellValue);
    draw_button_frame(sell, rgba(0.35f, 0.10f, 0.10f, 1.0f),
                      rgba(0.90f, 0.30f, 0.30f, 0.80f), true);
    text_centered(sell.x + sell.w * 0.5f, sell.y + 9.0f, 1.0f, COL_WHITE, buf);
}

static void draw_info_panel(const App *app)
{
    const Game *g = &app->game;
    UiRect panel = { PANEL_X, PANEL_Y, PANEL_W, PANEL_H };
    int towerIndex = panel_tower_index(app);
    char buf[64];

    fill_panel(panel);

    if (towerIndex >= 0 && towerIndex < g->towerCount &&
        g->towers[towerIndex].active) {
        const Tower *tower = &g->towers[towerIndex];
        GfxColor color = art_damage_color(tower->damageType, 1.0f);
        GfxColor nameColor = art_damage_text_color(tower->damageType);
        float damage = 0.0f;
        float fireRate = 1.0f;
        float range = 0.0f;
        float dps;

        game_tower_stats(tower->damageType, (int)tower->level, &damage, &fireRate,
                         &range);
        dps = fireRate > 0.0f ? damage / fireRate : 0.0f;

        /* damage-type coloured header bar replaces the top gold trim segment */
        gfx_fill_rect(panel.x + 7.0f, panel.y, panel.w - 14.0f, 2.0f, color);

        art_glow(15.0f, 96.0f, 11.0f, color, 0.18f);
        art_weapon_emblem(15.0f, 96.0f, 6.0f, tower->damageType);

        snprintf(buf, sizeof(buf), "%s LV.%d",
                 tower_display_name(tower->damageType), (int)tower->level);
        gfx_text(28.0f, 91.0f, 1.0f, nameColor, buf);

        snprintf(buf, sizeof(buf), "DMG %.1f   RNG %.1f   DPS %.1f",
                 (double)damage, (double)range, (double)dps);
        gfx_text(10.0f, 104.0f, 1.0f, COL_MUTED, buf);

        tower_effect_text(tower->damageType, buf, sizeof(buf));
        gfx_text(10.0f, 115.0f, 1.0f, nameColor, buf);

        draw_upgrade_sell(app, towerIndex);
    } else {
        bool isPath = cell_is_path(g, g->cursorCol, g->cursorRow);
        bool canPlace = game_can_place_at(g, g->cursorCol, g->cursorRow,
                                          g->selectedType);
        const TowerSpec *selectedSpec = game_tower_spec(g->selectedType);
        const char *name = tower_display_name(g->selectedType);
        int cost = selectedSpec != NULL ? selectedSpec->cost : 0;
        GfxColor selColor = art_damage_text_color(g->selectedType);

        snprintf(buf, sizeof(buf), "CURSOR C%d R%d", g->cursorCol, g->cursorRow);
        gfx_text(10.0f, 92.0f, 1.0f, COL_MUTED, buf);
        text_right(308.0f, 92.0f, 1.0f,
                   isPath ? COL_GOLD : COL_DIM, isPath ? "ROAD" : "EMPTY CELL");

        /* Maze building: a tower on the road forces the BFS to find a detour,
         * so a buildable road cell is a *positive* hint, not a warning.  Cells
         * the core refuses (start/goal, or a placement that would seal the
         * route) read as blocked instead. */
        if (canPlace && isPath) {
            gfx_text(10.0f, 106.0f, 1.0f, rgba(0.60f, 1.00f, 0.65f, 1.0f),
                     "ROAD - BUILD HERE TO REROUTE THEM");
        } else if (canPlace) {
            snprintf(buf, sizeof(buf), "A BUILDS %s FOR %dG", name, cost);
            gfx_text(10.0f, 106.0f, 1.0f, selColor, buf);
        } else if (isPath) {
            gfx_text(10.0f, 106.0f, 1.0f, COL_MUTED,
                     "ROAD - THIS CELL CAN'T BE BLOCKED");
        } else {
            gfx_text(10.0f, 106.0f, 1.0f, COL_MUTED, "CAN'T BUILD HERE RIGHT NOW");
        }

        gfx_text(10.0f, 121.0f, 1.0f, COL_DIM,
                 "L/R TYPE (6)   A BUILD   B CLEAR");
        gfx_text(10.0f, 135.0f, 1.0f, COL_DIM,
                 "TAP A TYPE ABOVE TO SWITCH");
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
    width = gfx_text_width(buf, 1.0f) + 18.0f;
    if (width > 302.0f)
        width = 302.0f;
    x = floorf(((float)BOT_W - width) * 0.5f);

    art_panel(x, 35.0f, width, 18.0f, 4.0f,
              rgba(1.0f, 0.25f, 0.22f, 0.85f * alpha), true);
    text_centered((float)BOT_W * 0.5f, 40.0f, 1.0f,
                  rgba(1.0f, 0.62f, 0.58f, alpha), buf);
}

/* ------------------------------------------------------------------------ */
/* Build phase: NEXT WAVE + incoming preview                                 */
/* ------------------------------------------------------------------------ */

static void draw_build_section(const App *app)
{
    const Game *g = &app->game;
    UiRect next = ui_next_wave_rect();
    UiRect preview = { 202.0f, BOTTOM_Y, 112.0f, BOTTOM_H };
    char buf[40];
    int hpLayers = 0, shieldLayers = 0, armorLayers = 0, elites = 0;

    /* next wave button: chunky BL2 button with a pulsing green glow */
    art_comic_button(next.x, next.y, next.w, next.h,
                     rgba(0.13f, 0.45f, 0.20f, 1.0f), COL_GREEN,
                     0.65f + 0.35f * (0.5f + 0.5f * sinf(app->time * 2.5f)));

    text_centered(next.x + next.w * 0.5f, next.y + 10.0f, 2.0f, COL_WHITE,
                  "NEXT WAVE");
    snprintf(buf, sizeof(buf), "WAVE %d OF %d", g->wave, g->totalWaves);
    text_centered(next.x + next.w * 0.5f, next.y + 34.0f, 1.0f,
                  rgba(0.90f, 1.00f, 0.92f, 0.95f), buf);
    text_centered(next.x + next.w * 0.5f, next.y + 48.0f, 1.0f,
                  rgba(0.86f, 0.98f, 0.88f, 0.92f), "PRESS WHEN READY");

    /* incoming composition: total layers per layer type + elite count */
    panel_with_title(preview, "INCOMING");
    game_wave_preview(g, &hpLayers, &shieldLayers, &armorLayers, &elites);
    draw_preview_row(preview.x + 10.0f, preview.y + 20.0f, hpLayers, LAYER_HP,
                     false);
    draw_preview_row(preview.x + 10.0f, preview.y + 34.0f, shieldLayers,
                     LAYER_SHIELD, false);
    draw_preview_row(preview.x + 10.0f, preview.y + 48.0f, armorLayers,
                     LAYER_ARMOR, false);
    draw_preview_row(preview.x + 10.0f, preview.y + 62.0f, elites, 0, true);
}

/* ------------------------------------------------------------------------ */
/* Wave phase: progress + stats (speed/pause live in the header badges)      */
/* ------------------------------------------------------------------------ */

static void draw_wave_section(const App *app)
{
    const Game *g = &app->game;
    UiRect panel = { 6.0f, BOTTOM_Y, 308.0f, BOTTOM_H };
    char buf[40];
    int total = app->waveEnemyTotal;
    int remaining;
    float frac;

    fill_panel(panel);

    /* scale-2 headline for the one thing the player checks most often */
    snprintf(buf, sizeof(buf), "WAVE %d", g->wave);
    gfx_text(14.0f, 162.0f, 2.0f, COL_WHITE, buf);
    text_right(306.0f, 168.0f, 1.0f, COL_MUTED, "IN PROGRESS");

    remaining = g->enemiesAlive + g->enemiesToSpawn;
    if (remaining < 0)
        remaining = 0;
    frac = total > 0 ? 1.0f - (float)remaining / (float)total : 0.0f;
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;

    /* chunky progress bar: hazard-striped amber fill with a gold keyline */
    gfx_fill_rect(14.0f, 181.0f, 292.0f, 15.0f, rgba(0.10f, 0.10f, 0.18f, 1.0f));
    if (frac > 0.0f)
        art_hazard_fill(16.0f, 183.0f, 288.0f * frac, 11.0f,
                        rgba(0.95f, 0.60f, 0.15f, 1.0f),
                        rgba(0.34f, 0.20f, 0.03f, 0.90f));
    gfx_outline_rect(14.0f, 181.0f, 292.0f, 15.0f, 1.0f, COL_GOLD);
    snprintf(buf, sizeof(buf), "CLEARED %d%%   LEFT %d", (int)(frac * 100.0f + 0.5f),
             remaining);
    text_centered(160.0f, 184.0f, 1.0f, rgba(1.0f, 1.0f, 1.0f, 0.98f), buf);

    snprintf(buf, sizeof(buf), "ALIVE %d", g->enemiesAlive);
    gfx_text(14.0f, 200.0f, 1.0f, COL_WHITE, buf);
    snprintf(buf, sizeof(buf), "QUEUED %d", g->enemiesToSpawn);
    gfx_text(112.0f, 200.0f, 1.0f, COL_MUTED, buf);
    snprintf(buf, sizeof(buf), "KILLS %d", g->kills);
    gfx_text(212.0f, 200.0f, 1.0f, COL_MUTED, buf);

    snprintf(buf, sizeof(buf), "LEAKS %d", g->leaks);
    gfx_text(14.0f, 212.0f, 1.0f, g->leaks > 0 ? COL_RED : COL_MUTED, buf);
    snprintf(buf, sizeof(buf), "TOWERS %d", g->towerCount);
    gfx_text(112.0f, 212.0f, 1.0f, COL_MUTED, buf);
    {
        int seconds = (int)g->timeTotal;
        if (seconds < 0)
            seconds = 0;
        snprintf(buf, sizeof(buf), "TIME %d:%02d", seconds / 60, seconds % 60);
        gfx_text(212.0f, 212.0f, 1.0f, COL_MUTED, buf);
    }

    text_centered(160.0f, 224.0f, 1.0f, COL_DIM,
                  "START: PAUSE    SELECT: SPEED");
}

/* ------------------------------------------------------------------------ */
/* Pause overlay                                                             */
/* ------------------------------------------------------------------------ */

static void draw_pause_overlay(void)
{
    UiRect panel = { 50.0f, 24.0f, 220.0f, 196.0f };
    UiRect resume = ui_resume_rect();
    UiRect menu = ui_pause_menu_rect();

    gfx_fill_rect(0.0f, 0.0f, (float)BOT_W, (float)BOT_H,
                  rgba(0.0f, 0.0f, 0.0f, 0.65f));
    fill_panel(panel);
    art_hazard_strip(62.0f, 31.0f, 14.0f, 5.0f);
    art_hazard_strip(244.0f, 31.0f, 14.0f, 5.0f);

    text_centered(160.0f, 40.0f, 3.0f, COL_WHITE, "PAUSED");
    text_centered(160.0f, 74.0f, 1.0f, COL_MUTED, "THE WAVE IS FROZEN");

    art_comic_button(resume.x, resume.y, resume.w, resume.h,
                     rgba(0.12f, 0.40f, 0.18f, 1.0f), COL_GREEN, 0.85f);
    text_centered(resume.x + resume.w * 0.5f, resume.y + 14.0f, 2.0f, COL_WHITE,
                  "RESUME");

    art_comic_button(menu.x, menu.y, menu.w, menu.h,
                     rgba(0.10f, 0.10f, 0.18f, 1.0f),
                     rgba(0.45f, 0.45f, 0.75f, 1.0f), 0.55f);
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

    fill_panel(stats);
    snprintf(buf, sizeof(buf), "WAVE %d/%d", shownWave, g->totalWaves);
    text_centered(160.0f, 71.0f, 1.0f, COL_WHITE, buf);
    snprintf(buf, sizeof(buf), "KILLS %d     LEAKS %d", g->kills, g->leaks);
    text_centered(160.0f, 87.0f, 1.0f, COL_MUTED, buf);
    snprintf(buf, sizeof(buf), "TIME %d:%02d     GOLD %d", seconds / 60,
             seconds % 60, g->gold);
    text_centered(160.0f, 103.0f, 1.0f, COL_MUTED, buf);

    art_comic_button(retry.x, retry.y, retry.w, retry.h,
                     rgba(0.12f, 0.40f, 0.18f, 1.0f), COL_GREEN, 0.85f);
    text_centered(retry.x + retry.w * 0.5f, retry.y + 11.0f, 2.0f, COL_WHITE,
                  "PLAY AGAIN");
    text_centered(retry.x + retry.w * 0.5f, retry.y + 38.0f, 1.0f,
                  rgba(0.85f, 1.00f, 0.90f, 0.90f), "TAP TO RETRY");

    art_comic_button(menu.x, menu.y, menu.w, menu.h,
                     rgba(0.10f, 0.10f, 0.18f, 1.0f),
                     rgba(0.45f, 0.45f, 0.75f, 1.0f), 0.55f);
    text_centered(menu.x + menu.w * 0.5f, menu.y + 10.0f, 1.0f, COL_WHITE,
                  "MAIN MENU");
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                               */
/* ------------------------------------------------------------------------ */

void ui_draw(const App *app)
{
    const Game *g = &app->game;

    render_background_gradient(BOT_W, BOT_H);

    if (g->phase == PHASE_MENU) {
        draw_menu(app);
        return;
    }
    if (g->phase == PHASE_GAMEOVER || g->phase == PHASE_VICTORY) {
        draw_result(app);
        return;
    }

    draw_header(app);
    draw_tower_palette(app);
    draw_info_panel(app);
    draw_message(app);

    if (g->phase == PHASE_BUILD)
        draw_build_section(app);
    else
        draw_wave_section(app);

    if (g->paused)
        draw_pause_overlay();
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
    if (towerIndex >= 0 && towerIndex < g->towerCount && g->towers[towerIndex].active) {
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
