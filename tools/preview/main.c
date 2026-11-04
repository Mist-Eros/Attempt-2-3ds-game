/*
 * main.c - scripted PC preview driver for Elemental Defense (3DS)
 *
 * Runs the real platform-independent game code (source/app.c, render.c, ui.c,
 * core .c files) against the software gfx.h backend in plat_host.c, feeding it
 * a deterministic scripted Input sequence instead of a 3DS controller.  Every
 * captured frame is dumped as two PNGs (top 400x240, bottom 320x240) so the
 * layout can be inspected without hardware.
 *
 * Usage (see tools/preview/Makefile):
 *   preview.exe [--scenario NAME] [--frames N] [--capture-every K]
 *               [--out DIR] [--seed S]
 *
 * Scenarios: menu, build, maze, wave, status, selected, gameover, victory,
 * all (default all).  v0.3 adds six damage-type towers, layered enemies and
 * status effects; see docs/elements-spec.md.
 *
 * The driver never pokes at rendering code; it only talks to app.h plus the
 * read-only ui_*_rect() hit boxes that ui.h exposes for exactly this purpose.
 * For the result screens it forces Game.phase directly (there is no scripted
 * way to lose 20 lives in 30 seconds), which is what those states are: pure
 * overlays over the last battlefield frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#include <direct.h>
#define HOST_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define HOST_MKDIR(p) mkdir((p), 0777)
#endif

#include "app.h"
#include "audio.h"
#include "game.h"
#include "gfx.h"
#include "render.h"
#include "ui.h"
#include "host.h"

#define PREVIEW_DT       (1.0f / 60.0f)
#define DEFAULT_FRAMES   1800   /* 30 s of game time per scenario */
#define DEFAULT_SEED     1337u

/* ------------------------------------------------------------------ */
/* driver state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    App app;
    const char *scenario;
    const char *outdir;
    int max_frames;
    int capture_every;
    int frame;          /* frames ticked in the current scenario        */
    int capture_seq;    /* global capture serial, used in the file name */
    int captures;
    int failures;
    int duplicates;     /* captures identical to the previous one       */
    int path_initial;   /* maze scenario: path.length before building   */
    int path_final;     /* maze scenario: path.length after building    */
    uint32_t last_top_sig;
    uint32_t last_bottom_sig;
} Driver;

static Input blank_input(void)
{
    Input in;
    memset(&in, 0, sizeof in);
    return in;
}

/* ------------------------------------------------------------------ */
/* output directory                                                    */
/* ------------------------------------------------------------------ */

static void ensure_dir(const char *path)
{
    char buf[1024];
    size_t i;
    size_t n;

    if (!path || !path[0])
        return;
    n = strlen(path);
    if (n >= sizeof buf)
        n = sizeof buf - 1;
    memcpy(buf, path, n);
    buf[n] = '\0';

    for (i = 1; i < n; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char saved = buf[i];
            buf[i] = '\0';
            (void)HOST_MKDIR(buf);
            buf[i] = saved;
        }
    }
    (void)HOST_MKDIR(buf);
}

/* ------------------------------------------------------------------ */
/* one frame                                                           */
/* ------------------------------------------------------------------ */

static void run_frame(Driver *d, const Input *input, bool force_capture)
{
    Input zero;
    bool capture;
    int seq;

    if (!d || d->frame >= d->max_frames)
        return;
    if (!input) {
        memset(&zero, 0, sizeof zero);
        input = &zero;
    }

    capture = force_capture;
    if (!capture && d->capture_every > 0 && (d->frame % d->capture_every) == 0)
        capture = true;

    seq = d->capture_seq;
    if (capture)
        host_capture_request(seq);

    app_tick(&d->app, input, PREVIEW_DT);
    app_draw(&d->app);

    if (capture) {
        uint32_t top_sig;
        uint32_t bot_sig;
        int top_colors;
        int bot_colors;
        bool duplicate;

        host_capture_finish(seq);
        if (host_last_captured_frame() != seq) {
            fprintf(stderr,
                    "[capture] FAILED scenario=%s frame=%d (PNG write error)\n",
                    d->scenario, d->frame);
            d->failures++;
            d->capture_seq++;
            d->frame++;
            return;
        }

        top_sig = (uint32_t)host_screen_signature(SCREEN_TOP);
        bot_sig = (uint32_t)host_screen_signature(SCREEN_BOTTOM);
        top_colors = host_screen_distinct_colors(SCREEN_TOP);
        bot_colors = host_screen_distinct_colors(SCREEN_BOTTOM);
        duplicate = (d->captures > 0 && top_sig == d->last_top_sig &&
                     bot_sig == d->last_bottom_sig);
        if (duplicate)
            d->duplicates++;
        d->last_top_sig = top_sig;
        d->last_bottom_sig = bot_sig;

        printf("[capture] %-9s frame=%-4d %s/frame_%04d_top.png %s/frame_%04d_bottom.png"
               "  top_colors=%-5d bottom_colors=%-5d%s\n",
               d->scenario, d->frame, d->outdir, seq, d->outdir, seq,
               top_colors, bot_colors, duplicate ? "  (identical to previous)" : "");
        d->captures++;
        d->capture_seq++;
    }

    d->frame++;
}

static void idle(Driver *d, int frames)
{
    int i;
    for (i = 0; i < frames; i++)
        run_frame(d, NULL, false);
}

/* Press one button for exactly one frame (edge input). */
#define PRESS(d, field, capture_after)                       \
    do {                                                     \
        Input press_in_ = blank_input();                     \
        press_in_.field = true;                              \
        run_frame((d), &press_in_, (capture_after));         \
    } while (0)

/* Tap one bottom-screen UI rect (touchscreen "pressed this frame"). */
static bool tap_rect(Driver *d, UiRect r, bool capture_after)
{
    Input in;

    if (r.w <= 0.0f || r.h <= 0.0f) {
        fprintf(stderr, "[tap] scenario=%s frame=%d: invalid rect %.0f,%.0f %.0fx%.0f\n",
                d->scenario, d->frame, r.x, r.y, r.w, r.h);
        return false;
    }

    in = blank_input();
    in.touchDown = true;
    in.touchPressed = true;
    in.touchX = r.x + r.w * 0.5f;
    in.touchY = r.y + r.h * 0.5f;
    in.touchTop = false;
    run_frame(d, &in, capture_after);
    return true;
}

/* Move the D-pad cursor by dcol/drow grid cells (one press per cell). */
static void move_cursor(Driver *d, int dcol, int drow)
{
    int i;

    for (i = 0; i < dcol; i++)
        PRESS(d, right, false);
    for (i = 0; i < -dcol; i++)
        PRESS(d, left, false);
    for (i = 0; i < drow; i++)
        PRESS(d, down, false);
    for (i = 0; i < -drow; i++)
        PRESS(d, up, false);
}

/* Start a run from the title screen (A does it; app.c owns the transition). */
static void start_run_from_menu(Driver *d)
{
    idle(d, 2);
    PRESS(d, a, false);
    idle(d, 2);
}

/* Move the D-pad cursor to an absolute board cell (one edge press per cell).
 * Uses Game.cursorCol/cursorRow so it works whatever cell a run starts on. */
static void move_cursor_to(Driver *d, int col, int row)
{
    Game *g = &d->app.game;
    move_cursor(d, col - g->cursorCol, row - g->cursorRow);
}

/* Cycle the build type with R until it is `type` (0..TOWER_TYPE_COUNT-1). */
static void select_type(Driver *d, int type)
{
    Game *g = &d->app.game;
    int guard = 0;

    while (g->selectedType != type && guard < TOWER_TYPE_COUNT + 1) {
        PRESS(d, r, false);
        ++guard;
    }
}

/* Try one scripted placement.  Never loops: reports and returns false when the
 * core rejects the cell (e.g. it would seal the exit, or we are out of gold). */
static bool try_place_tower(Driver *d, int col, int row, int type)
{
    Game *g = &d->app.game;
    const TowerSpec *spec = game_tower_spec(type);

    move_cursor_to(d, col, row);
    select_type(d, type);

    if (!game_can_place_at(g, col, row, type)) {
        printf("[place] skip (%2d,%2d) type=%d: can_place_at=false (gold=%d path.length=%.0f)\n",
               col, row, type, g->gold, (double)g->path.length);
        return false;
    }

    PRESS(d, a, false);

    if (game_tower_at(g, col, row) < 0) {
        printf("[place] rejected (%2d,%2d) type=%d: %s (gold=%d)\n",
               col, row, type, g->message[0] ? g->message : "placement failed", g->gold);
        return false;
    }

    printf("[place] placed %-9s at (%2d,%2d) towers=%d gold=%3d path.length=%.0f\n",
           spec ? spec->name : "?", col, row, g->towerCount, g->gold,
           (double)g->path.length);
    return true;
}

/* v0.3 helper: summarise live enemies by layer count and outermost layer type. */
static void enemy_layer_stats(const Game *g, int byLayers[3], int byOuter[3])
{
    int i;

    byLayers[0] = byLayers[1] = byLayers[2] = 0;
    byOuter[0] = byOuter[1] = byOuter[2] = 0;
    for (i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *e = &g->enemies[i];

        if (!e->active)
            continue;
        if (e->layerCount >= 1 && e->layerCount <= 3)
            byLayers[e->layerCount - 1]++;
        if (e->layers[0].type < 3)
            byOuter[e->layers[0].type]++;
    }
}

/* Any live enemy currently affected by a status?  Used to capture effect
 * visuals from the real simulation instead of poking at pixels. */
static int max_burn_stacks(const Game *g)
{
    int i;
    int best = 0;
    for (i = 0; i < MAX_ENEMIES; i++)
        if (g->enemies[i].active && (int)g->enemies[i].burnStacks > best)
            best = (int)g->enemies[i].burnStacks;
    return best;
}
static bool any_stun(const Game *g)
{
    int i;
    for (i = 0; i < MAX_ENEMIES; i++)
        if (g->enemies[i].active && g->enemies[i].stunTimer > 0.0f)
            return true;
    return false;
}
static bool any_slag(const Game *g)
{
    int i;
    for (i = 0; i < MAX_ENEMIES; i++)
        if (g->enemies[i].active && g->enemies[i].slagTimer > 0.0f)
            return true;
    return false;
}
static bool any_puddle(const Game *g)
{
    int i;
    for (i = 0; i < MAX_PUDDLES; i++)
        if (g->puddles[i].active)
            return true;
    return false;
}
static bool any_explosion_beam(const Game *g)
{
    int i;
    for (i = 0; i < MAX_BEAMS; i++)
        if (g->beams[i].active && g->beams[i].kind == 1)
            return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* scenarios                                                           */
/* ------------------------------------------------------------------ */

/* Title screen: six moments over the first 2.5 s so the animated redesign
 * (bobbing element emblems, pulsing prompt/menu copy, drifting background) can
 * be compared and a good README frame picked; the menu animator runs at 60 Hz. */
static void scenario_menu(Driver *d)
{
    static const int captureFrames[] = { 0, 30, 60, 90, 120, 150 };
    const int captureCount = (int)(sizeof captureFrames / sizeof captureFrames[0]);
    int next = 0;

    while (d->frame <= captureFrames[captureCount - 1] &&
           d->frame < d->max_frames) {
        bool capture = (next < captureCount && d->frame == captureFrames[next]);
        run_frame(d, NULL, capture);
        if (capture)
            ++next;
    }
}

/* Menu -> build phase; place one tower of every damage type so the six-button
 * palette, the selected-tower panel and the layer-based INCOMING preview can
 * be checked, then show the pause overlay. */
static void scenario_build(Driver *d)
{
    static const struct { int col, row, type; } plan[] = {
        {  3, 2, DMG_NORMAL    },
        {  6, 2, DMG_FIRE      },
        {  9, 2, DMG_ELECTRIC  },
        { 12, 2, DMG_CORROSIVE },
        { 15, 2, DMG_SLAG      },
        { 18, 2, DMG_EXPLOSIVE }
    };
    const int planCount = (int)(sizeof plan / sizeof plan[0]);
    int i;

    start_run_from_menu(d);
    run_frame(d, NULL, true);           /* empty build board: 6-type palette + layer preview */

    for (i = 0; i < planCount; i++) {
        if (!try_place_tower(d, plan[i].col, plan[i].row, plan[i].type))
            continue;
        if (i == 2) {
            idle(d, 2);
            run_frame(d, NULL, true);   /* 3 towers: palette + selected panel */
        }
    }
    idle(d, 2);
    run_frame(d, NULL, true);           /* six towers, BLASTER selected (AOE line) */

    move_cursor_to(d, 12, 0);           /* (12,0), clears the selection */
    idle(d, 4);
    run_frame(d, NULL, true);           /* plain build HUD, cursor only */

    PRESS(d, start, false);             /* pause */
    idle(d, 2);
    run_frame(d, NULL, true);           /* pause overlay */
    PRESS(d, start, false);             /* resume */
    idle(d, 2);
}

/* BFS maze scenario: towers may be built on the road and every build re-runs
 * the pathfinder, so a short wall of towers makes the creeps walk a long
 * detour.  The wall grows up from the road at column 6 and contains all six
 * damage types (7 towers = 145 of 150 gold), turning the straight 25-cell road
 * into a 39-cell serpentine.  Captures: straight road, first detour, mid-wall,
 * road-cell UI hint, full maze, then wave 1 creeps on the rerouted road. */
static void scenario_maze(Driver *d)
{
    static const struct { int col, row, type; } plan[] = {
        { 6, 12, DMG_NORMAL    },   /* on the road -> first detour    */
        { 6, 11, DMG_FIRE      },
        { 6, 10, DMG_ELECTRIC  },
        { 6,  9, DMG_CORROSIVE },   /* wall shows all six damage types */
        { 6,  8, DMG_SLAG      },
        { 6,  7, DMG_EXPLOSIVE },
        { 6,  6, DMG_NORMAL    }    /* full wall -> longest detour    */
    };
    const int planCount = (int)(sizeof plan / sizeof plan[0]);
    int i;

    start_run_from_menu(d);
    idle(d, 2);

    d->path_initial = (int)(d->app.game.path.length + 0.5f);
    printf("[maze] initial path.length=%.0f (entrance (%d,%d) -> exit (%d,%d)), towers=%d\n",
           (double)d->app.game.path.length, 0, GRID_ROWS - 1, GRID_COLS - 1, GRID_ROWS - 1,
           d->app.game.towerCount);
    run_frame(d, NULL, true);           /* (1) straight road, no towers */

    for (i = 0; i < planCount; i++) {
        bool capture = (i == 0 || i == 3 || i == planCount - 1);
        if (try_place_tower(d, plan[i].col, plan[i].row, plan[i].type) && capture) {
            idle(d, 2);
            run_frame(d, NULL, true);   /* (2) first detour / (3) mid-wall / (5) full maze */
        }
        if (i == 3) {
            /* Cursor on an empty road cell while we can still afford a tower:
             * the bottom info panel should show the new reroute hint copy. */
            move_cursor_to(d, 15, 12);
            idle(d, 2);
            run_frame(d, NULL, true);   /* (4) road-cell reroute hint */
        }
    }

    d->path_final = (int)(d->app.game.path.length + 0.5f);
    printf("[maze] RESULT path.length %d -> %d  towers=%d  gold=%d\n",
           d->path_initial, d->path_final, d->app.game.towerCount, d->app.game.gold);

    /* Drive wave 1 so creeps are actually walking the rerouted road. */
    if (d->app.game.phase == PHASE_BUILD &&
        tap_rect(d, ui_next_wave_rect(), true)) {   /* (6) wave start */
        int wave_captures = 0;
        for (i = 0; i < 900 && d->frame < d->max_frames; i++) {
            bool capture = false;

            if (d->app.game.phase != PHASE_WAVE)
                break;
            /* Three evenly spaced shots of the creeps on the long detour. */
            if (d->app.game.enemiesAlive > 0 && wave_captures < 3 &&
                (d->frame % 150) == 0) {
                capture = true;
                ++wave_captures;
            }
            run_frame(d, NULL, capture);
        }
        idle(d, 2);
        run_frame(d, NULL, true);       /* (7) creeps on the rerouted road */
    }
}

/* Mixed six-type battery above the road.  The wave counter is forced to 9 so
 * enemies spawn with 3 concentric HP/SHIELD/ARMOR layers (waves 1-3 = 1 layer,
 * 4-8 = 2, 9+ = 3); captures pick up the layered rings and combat effects. */
static void scenario_wave(Driver *d)
{
    static const struct { int col, row, type; } plan[] = {
        {  2, 11, DMG_NORMAL    },
        {  4, 11, DMG_FIRE      },
        {  6, 11, DMG_ELECTRIC  },
        {  8, 11, DMG_CORROSIVE },
        { 10, 11, DMG_SLAG      },
        { 12, 11, DMG_EXPLOSIVE }
    };
    const int planCount = (int)(sizeof plan / sizeof plan[0]);
    int i;
    int byLayers[3];
    int byOuter[3];
    bool gotLayered = false;
    int combatCaptures = 0;

    start_run_from_menu(d);
    for (i = 0; i < planCount; i++)
        (void)try_place_tower(d, plan[i].col, plan[i].row, plan[i].type);
    idle(d, 2);
    run_frame(d, NULL, true);           /* build phase, six-type battery ready */

    /* Cheat the wave counter forward: layers are 1 for waves 1-3, 2 for 4-8 and
     * 3 for waves 9+.  Wave 9 has no elites, so the spawn sequence is stable. */
    d->app.game.wave = 9;
    printf("[wave] forcing wave=%d -> 3-layer enemies (HP/SHIELD/ARMOR rings)\n",
           d->app.game.wave);

    if (!tap_rect(d, ui_next_wave_rect(), true)) {
        fprintf(stderr, "[wave] ui_next_wave_rect() is empty; cannot start wave\n");
        return;
    }

    for (i = 0; i < 1500 && d->frame < d->max_frames; i++) {
        bool capture = false;
        bool beam = false;
        int b;

        if (d->app.game.phase != PHASE_WAVE)
            break;

        enemy_layer_stats(&d->app.game, byLayers, byOuter);
        for (b = 0; b < MAX_BEAMS; b++) {
            if (d->app.game.beams[b].active) {
                beam = true;
                break;
            }
        }

        if (!gotLayered && byLayers[2] > 0) {
            gotLayered = true;
            capture = true;
            printf("[wave] capture 3-layer enemies: %d alive with 2 layers, %d with 3;"
                   " outer layers HP=%d SHIELD=%d ARMOR=%d\n",
                   byLayers[1], byLayers[2], byOuter[0], byOuter[1], byOuter[2]);
        } else if (beam && combatCaptures < 10 && (d->frame % 30) == 0) {
            capture = true;
            ++combatCaptures;
        } else if (!beam && d->app.game.enemiesAlive > 0 && (d->frame % 120) == 0) {
            capture = true;
        }
        run_frame(d, NULL, capture);
    }
    idle(d, 2);
    run_frame(d, NULL, true);           /* last combat state of the run */
}

/* Status-effect showcase: five damage-type towers next to the road and a
 * forced wave-9 spawn.  Each effect screenshot is taken exactly when the real
 * simulation reports it (burn stacks, stun timer, acid puddle, slag timer,
 * explosion beam) - no rendered pixels are faked. */
static void scenario_status(Driver *d)
{
    static const struct { int col, row, type; } plan[] = {
        {  2, 11, DMG_FIRE      },
        {  4, 11, DMG_ELECTRIC  },
        {  6, 11, DMG_CORROSIVE },
        {  8, 11, DMG_SLAG      },
        { 10, 11, DMG_EXPLOSIVE }
    };
    const int planCount = (int)(sizeof plan / sizeof plan[0]);
    bool gotLayers = false;
    bool gotBurn = false;
    bool gotStun = false;
    bool gotPuddle = false;
    bool gotSlag = false;
    bool gotExplosion = false;
    int i;

    start_run_from_menu(d);
    for (i = 0; i < planCount; i++)
        (void)try_place_tower(d, plan[i].col, plan[i].row, plan[i].type);
    idle(d, 2);
    run_frame(d, NULL, true);           /* five status towers above the road */

    d->app.game.wave = 9;               /* 3-layer enemies */
    if (!tap_rect(d, ui_next_wave_rect(), true)) {
        fprintf(stderr, "[status] ui_next_wave_rect() is empty; cannot start wave\n");
        return;
    }

    for (i = 0; i < 1600 && d->frame < d->max_frames; i++) {
        const char *why = NULL;
        bool capture = false;
        int byLayers[3];
        int byOuter[3];

        if (d->app.game.phase != PHASE_WAVE)
            break;

        enemy_layer_stats(&d->app.game, byLayers, byOuter);

        if (!gotLayers && byLayers[2] > 0) {
            gotLayers = true;
            capture = true;
            printf("[status] capture: 3-layer enemy; outer layers HP=%d SHIELD=%d ARMOR=%d\n",
                   byOuter[0], byOuter[1], byOuter[2]);
        } else if (!gotPuddle && any_puddle(&d->app.game)) {
            gotPuddle = true;
            capture = true;
            why = "corrosive acid puddle";
        } else if (!gotExplosion && any_explosion_beam(&d->app.game)) {
            gotExplosion = true;
            capture = true;
            why = "explosive AoE ring";
        } else if (!gotBurn && max_burn_stacks(&d->app.game) >= 2) {
            gotBurn = true;
            capture = true;
            why = "fire burn stacks (>=2 motes)";
        } else if (!gotStun && any_stun(&d->app.game)) {
            gotStun = true;
            capture = true;
            why = "electric stun flash";
        } else if (!gotSlag && any_slag(&d->app.game)) {
            gotSlag = true;
            capture = true;
            why = "slag purple outline";
        }

        if (why)
            printf("[status] capture: %s\n", why);
        run_frame(d, NULL, capture);

        if (gotLayers && gotBurn && gotStun && gotPuddle && gotSlag && gotExplosion)
            break;
    }
    idle(d, 2);
    run_frame(d, NULL, true);           /* final status state */

    printf("[status] effects seen: layers=%d burn=%d stun=%d puddle=%d slag=%d explosion=%d\n",
           (int)gotLayers, (int)gotBurn, (int)gotStun, (int)gotPuddle,
           (int)gotSlag, (int)gotExplosion);
}

/* Selected-tower panel for several damage types: the info line must show the
 * per-type effect (`BURNS`, `STUNS`, `ACID PUDDLE`, `SLAG: +50% DMG TAKEN`,
 * `AOE 1.3`) in the damage-type colour.  Also exercises the upgrade path. */
static void scenario_selected(Driver *d)
{
    start_run_from_menu(d);

    /* FIRE: burn effect line + range circle over the road. */
    (void)try_place_tower(d, 4, 11, DMG_FIRE);
    idle(d, 2);
    run_frame(d, NULL, true);           /* FIRE selected: "BURNS" line */

    tap_rect(d, ui_upgrade_rect(), false);
    idle(d, 2);
    run_frame(d, NULL, true);           /* FIRE LV.2, next upgrade cost */

    /* One tower of each remaining effect type; placement auto-selects it, so
     * each panel is captured straight away. */
    (void)try_place_tower(d, 6, 11, DMG_CORROSIVE);
    idle(d, 2);
    run_frame(d, NULL, true);           /* CORROSIVE: "ACID PUDDLE" line */

    (void)try_place_tower(d, 8, 11, DMG_SLAG);
    idle(d, 2);
    run_frame(d, NULL, true);           /* SLAG: "+50% DMG TAKEN" line */

    (void)try_place_tower(d, 10, 11, DMG_EXPLOSIVE);
    idle(d, 2);
    run_frame(d, NULL, true);           /* BLASTER: "AOE 1.3" line */

    (void)try_place_tower(d, 12, 11, DMG_ELECTRIC);
    idle(d, 2);
    run_frame(d, NULL, true);           /* SHOCK: "STUNS" line */

    tap_rect(d, ui_sell_rect(), false);
    idle(d, 2);
    run_frame(d, NULL, true);           /* after sell: empty-cell panel */
}

/* Force the game-over overlay (no way to lose 20 lives in a 30 s script). */
static void scenario_gameover(Driver *d)
{
    start_run_from_menu(d);

    (void)try_place_tower(d, 4, 0, DMG_FIRE);
    (void)try_place_tower(d, 9, 0, DMG_ELECTRIC);
    (void)try_place_tower(d, 14, 0, DMG_CORROSIVE);
    idle(d, 3);

    d->app.game.phase = PHASE_GAMEOVER;
    d->app.game.lives = 0;
    d->app.game.wave = 4;
    d->app.game.kills = 17;
    d->app.game.enemiesAlive = 0;
    idle(d, 2);
    run_frame(d, NULL, true);           /* game-over overlay */
}

/* Force the victory overlay (wave == totalWaves). */
static void scenario_victory(Driver *d)
{
    start_run_from_menu(d);

    (void)try_place_tower(d, 4, 0, DMG_FIRE);
    (void)try_place_tower(d, 9, 0, DMG_SLAG);
    (void)try_place_tower(d, 14, 0, DMG_EXPLOSIVE);
    idle(d, 3);

    d->app.game.phase = PHASE_VICTORY;
    d->app.game.wave = d->app.game.totalWaves;
    d->app.game.kills = 143;
    d->app.game.lives = 12;
    d->app.game.enemiesAlive = 0;
    idle(d, 2);
    run_frame(d, NULL, true);           /* victory overlay */
}

/* ------------------------------------------------------------------ */
/* v0.7 late-game UI stress test (1000-wave campaign, no tower cap)   */
/* ------------------------------------------------------------------ */

/* Fill every non-road cell with a late-game tower so the HUD's tower counter
 * reaches 3 digits (the straight road leaves 312 buildable cells).  This is a
 * hand-built UI fixture, not a played run - the harness writes Game state
 * directly, exactly like the gameover/victory scenarios force their phases. */
static int late_fill_board(Game *g)
{
    int8_t isPath[GRID_ROWS][GRID_COLS];
    int col, row, n = 0;
    int i;

    memset(isPath, 0, sizeof isPath);
    for (i = 0; i < g->path.count; i++) {
        int pc = (int)g->path.pts[i].x;   /* cell centres are (col+0.5) */
        int pr = (int)g->path.pts[i].y;
        if (pc >= 0 && pc < GRID_COLS && pr >= 0 && pr < GRID_ROWS)
            isPath[pr][pc] = 1;
    }

    memset(g->occupancy, 0, sizeof g->occupancy);
    for (row = 0; row < GRID_ROWS; row++) {
        for (col = 0; col < GRID_COLS; col++) {
            Tower *t;
            if (isPath[row][col] || n >= MAX_TOWERS)
                continue;
            t = &g->towers[n];
            memset(t, 0, sizeof *t);
            t->active = true;
            t->damageType = (uint8_t)(n % TOWER_TYPE_COUNT);
            t->level = (uint8_t)(1 + (n * 7) % TOWER_MAX_LEVEL);
            if (n == 0)
                t->level = (uint8_t)(TOWER_MAX_LEVEL - 1);  /* LV.9: expensive upgrade label */
            t->col = (int16_t)col;
            t->row = (int16_t)row;
            t->cooldown = (float)(n % 10) * 0.08f;
            t->invested = 20 * (1 << (n % 5));
            t->animPhase = (float)(n % 17) * 0.37f;
            g->occupancy[row][col] = (int16_t)(n + 1);
            ++n;
        }
    }
    g->towerCount = n;
    return n;
}

/* Late-game fixture: four captures that exercise the widest HUD values.
 * The real 1000-wave sim ends around 253,060 gold, so the gold chip is tested
 * with realistic 6-digit balances (engine-3ds abbreviates them); the cursor
 * tower is LV.9 so the panel shows its most expensive UPGRADE label.
 *   1) BUILD at wave 900/1000, gold 172,360 - mid-late reference value.
 *   2) BUILD at wave 1000/1000, gold 253,060 - end-of-campaign balance,
 *      3-digit TOWERS chip, NEXT WAVE "WAVE 1000 OF 1000", the full INCOMING
 *      preview (game_wave_preview at wave 1000: 80 spawns / 10 elites) and the
 *      LV.9 -> LV.10 upgrade cost label.
 *   3) WAVE at wave 1000/1000, fast-forwarded to a mid-wave state so the
 *      progress bar, LEFT/ALIVE/QUEUED/KILLS counters and the wide wave title
 *      are all on screen at once.
 *   4) VICTORY with internal wave=1001 but totalWaves=1000: both screens must
 *      clamp the displayed text to 1000/1000. */
static void scenario_late(Driver *d)
{
    Game *g = &d->app.game;
    int hpLayers = 0, shieldLayers = 0, armorLayers = 0, elites = 0;
    int towers, upgradeCost;
    int i;

    start_run_from_menu(d);
    idle(d, 2);

    towers = late_fill_board(g);

    /* ---- (1) BUILD at wave 900/1000, gold 172,360 ------------------- */
    g->phase = PHASE_BUILD;
    g->wave = 900;
    g->totalWaves = 1000;
    g->gold = 172360;
    g->lives = 20;
    g->kills = 48000;
    g->leaks = 4;
    g->timeTotal = 20123.4f;
    g->speedMultiplier = 1.0f;
    g->enemiesAlive = 0;
    g->enemiesToSpawn = 0;
    g->selectedTower = -1;
    g->cursorCol = 0;
    g->cursorRow = 0;
    game_wave_preview(g, &hpLayers, &shieldLayers, &armorLayers, &elites);
    upgradeCost = game_upgrade_cost(g, 0);
    printf("[late] BUILD wave=%d/%d towers=%d gold=%d; INCOMING hp=%d "
           "shield=%d armor=%d elites=%d; selected GUN LV.%d upgrade %dG\n",
           g->wave, g->totalWaves, towers, g->gold, hpLayers, shieldLayers,
           armorLayers, elites, (int)g->towers[0].level, upgradeCost);
    idle(d, 2);
    run_frame(d, NULL, true);           /* late-1: BUILD @ 900/1000, gold 172K */

    /* ---- (2) BUILD at wave 1000/1000, real end-of-run gold 253,060 -- */
    g->wave = 1000;
    g->gold = 253060;
    g->kills = 52000;
    g->leaks = 5;
    g->timeTotal = 23456.7f;
    game_wave_preview(g, &hpLayers, &shieldLayers, &armorLayers, &elites);
    printf("[late] BUILD wave=%d/%d towers=%d gold=%d; INCOMING hp=%d "
           "shield=%d armor=%d elites=%d\n",
           g->wave, g->totalWaves, towers, g->gold, hpLayers, shieldLayers,
           armorLayers, elites);
    idle(d, 2);
    run_frame(d, NULL, true);           /* late-2: BUILD @ 1000/1000, gold 253K */

    /* ---- (3) WAVE at wave 1000/1000, mid-progress ------------------- */
    g->lives = 999;                     /* fixture: keep the demo alive... */
    game_start_next_wave(g);
    for (i = 0; i < 480 && d->frame < d->max_frames; i++) {
        if (g->phase != PHASE_WAVE)
            break;
        run_frame(d, NULL, false);
    }
    g->lives = 16;                      /* ...then show realistic values */
    g->leaks = 5;
    g->gold = 253060;
    printf("[late] WAVE wave=%d/%d alive=%d queued=%d kills=%d leaks=%d "
           "progress_total=%d\n",
           g->wave, g->totalWaves, g->enemiesAlive, g->enemiesToSpawn,
           g->kills, g->leaks, d->app.waveEnemyTotal);
    idle(d, 2);
    run_frame(d, NULL, true);           /* late-3: WAVE @ 1000/1000 */

    /* ---- (4) VICTORY at wave 1000 (internal wave 1001) ------------- */
    g->phase = PHASE_VICTORY;
    g->wave = 1001;                     /* one past the end -> must clamp */
    g->totalWaves = 1000;
    g->kills = 52000;
    g->leaks = 5;
    g->timeTotal = 23456.7f;
    g->gold = 253060;
    g->lives = 16;
    g->enemiesAlive = 0;
    g->enemiesToSpawn = 0;
    printf("[late] VICTORY internal wave=%d totalWaves=%d gold=%d "
           "(expect clamp to %d/%d)\n",
           g->wave, g->totalWaves, g->gold, g->totalWaves, g->totalWaves);
    idle(d, 2);
    run_frame(d, NULL, true);           /* late-4: VICTORY @ 1000/1000 */
}

/* ------------------------------------------------------------------ */
/* scenario dispatch                                                   */
/* ------------------------------------------------------------------ */

typedef void (*ScenarioFn)(Driver *d);

typedef struct {
    const char *name;
    ScenarioFn fn;
} ScenarioEntry;

static const ScenarioEntry kScenarios[] = {
    { "menu",     scenario_menu     },
    { "build",    scenario_build    },
    { "maze",     scenario_maze     },
    { "wave",     scenario_wave     },
    { "status",   scenario_status   },
    { "selected", scenario_selected },
    { "gameover", scenario_gameover },
    { "victory",  scenario_victory  },
    { "late",     scenario_late     }
};

#define SCENARIO_COUNT ((int)(sizeof kScenarios / sizeof kScenarios[0]))

/* Shared capture serial: PNG file names stay unique across "all" scenarios. */
static int g_capture_seq = 0;

static int run_scenario(const char *name, const char *outdir, int seed,
                        int capture_every, int max_frames)
{
    Driver d;
    int i;
    int found = 0;
    int result = 0;

    memset(&d, 0, sizeof d);
    d.scenario = name;
    d.outdir = outdir;
    d.max_frames = max_frames;
    d.capture_every = capture_every;
    d.capture_seq = g_capture_seq;
    d.path_initial = -1;
    d.path_final = -1;
    d.last_top_sig = 0xFFFFFFFFu;
    d.last_bottom_sig = 0xFFFFFFFFu;

    gfx_init();
    host_capture_configure(outdir, 1);
    host_capture_set_defer(0);   /* app_draw() ends with exactly one gfx_present() */

    app_init(&d.app);
    d.app.seed = (uint32_t)seed;
    audio_init();

    for (i = 0; i < SCENARIO_COUNT; i++) {
        if (strcmp(kScenarios[i].name, name) == 0) {
            found = 1;
            printf("\n=== scenario: %s (seed=%u, max_frames=%d) ===\n",
                   name, (unsigned)d.app.seed, max_frames);
            kScenarios[i].fn(&d);
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "unknown scenario '%s' (try --help)\n", name);
        return -1;
    }

    audio_exit();
    gfx_shutdown();

    printf("--- scenario %s: %d capture(s), %d failure(s), %d identical-to-previous,",
           name, d.captures, d.failures, d.duplicates);
    if (d.path_initial >= 0 && d.path_final >= 0)
        printf(" path.length %d -> %d,", d.path_initial, d.path_final);
    printf(" %d frame(s) simulated\n", d.frame);
    if (d.captures == 0) {
        fprintf(stderr, "!!! scenario %s produced no screenshots\n", name);
        result = 1;
    }
    if (d.failures > 0)
        result = 1;

    g_capture_seq = d.capture_seq;
    return result;
}

/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
    printf("Elemental Defense - PC preview screenshot harness\n"
           "\n"
           "Usage: %s [options]\n"
           "\n"
           "  --scenario NAME   menu | build | maze | wave | status | selected |\n"
           "                    gameover | victory | late | all (default: all)\n"
           "  --frames N        frame cap per scenario        (default: %d)\n"
           "  --capture-every K also capture every K-th frame (default: 0 = off)\n"
           "  --out DIR         screenshot directory          (default: out)\n"
           "  --seed S          run seed                      (default: %u)\n"
           "  --help            this text\n"
           "\n"
           "Output: DIR/frame_<N>_top.png (400x240) and DIR/frame_<N>_bottom.png\n"
           "(320x240), N counting captures across the whole run.\n",
           argv0, DEFAULT_FRAMES, (unsigned)DEFAULT_SEED);
}

int main(int argc, char **argv)
{
    const char *scenario = "all";
    const char *outdir = "out";
    int frames = DEFAULT_FRAMES;
    int capture_every = 0;
    unsigned seed = DEFAULT_SEED;
    int i;
    int exit_code = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            scenario = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--capture-every") == 0 && i + 1 < argc) {
            capture_every = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            outdir = argv[++i];
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option '%s' (try --help)\n", argv[i]);
            return 2;
        }
    }

    if (frames <= 0)
        frames = DEFAULT_FRAMES;
    if (capture_every < 0)
        capture_every = 0;

    ensure_dir(outdir);

    printf("Elemental Defense preview harness\n");
    printf("  outdir='%s' scenario='%s' seed=%u frames=%d capture_every=%d\n",
           outdir, scenario, seed, frames, capture_every);

    if (strcmp(scenario, "all") == 0) {
        int s;
        for (s = 0; s < SCENARIO_COUNT && exit_code == 0; s++) {
            if (run_scenario(kScenarios[s].name, outdir, (int)seed,
                             capture_every, frames) != 0)
                exit_code = 1;
        }
    } else {
        if (run_scenario(scenario, outdir, (int)seed, capture_every, frames) < 0)
            return 2;
    }

    printf("\npreview harness finished: %s\n", exit_code == 0 ? "OK" : "FAILURES");
    return exit_code;
}
