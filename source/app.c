/*
 * Elemental Defense - application / scene layer
 *
 * Glue between the portable simulation (include/game.h), the input snapshot
 * and the two screens.  This file is deliberately platform free: it talks to
 * the simulation and to the render_* and ui_* modules, never to the 3DS SDK.
 * Both the real
 * 3DS build (source/main.c) and the PC preview harness drive it the same way.
 *
 * Scene flow is driven entirely by Game.phase:
 *   PHASE_MENU      title screen, A/START or the bottom START button start a run
 *   PHASE_BUILD     battlefield + palette + NEXT WAVE
 *   PHASE_WAVE      same, plus the wave progress panel
 *   PHASE_GAMEOVER  result overlay on top, PLAY AGAIN / MAIN MENU on the bottom
 *   PHASE_VICTORY   ditto with the victory styling
 *
 * Pause lives in Game.paused and freezes the simulation.  Select cycles the
 * game speed (1x/2x/4x); the core applies it in game_update.
 */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "app.h"

#include "audio.h"
#include "game.h"
#include "render.h"
#include "ui.h"

/* ------------------------------------------------------------------------ */
/* Menu particles                                                            */
/* ------------------------------------------------------------------------ */

static uint32_t rng_next(uint32_t *state)
{
    /* xorshift32: tiny, deterministic and identical on 3DS and PC. */
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static float rng_unit(uint32_t *state)
{
    return (float)(rng_next(state) >> 8) * (1.0f / 16777216.0f);
}

static void particle_spawn(App *app, MenuParticle *p, bool anywhere)
{
    uint32_t *rng = &app->particleRng;
    int type = (int)(rng_unit(rng) * (float)TOWER_TYPE_COUNT);
    float brightness;

    type %= TOWER_TYPE_COUNT;
    if (type < 0)
        type += TOWER_TYPE_COUNT;

    brightness = 0.55f + 0.45f * rng_unit(rng);

    p->x = rng_unit(rng) * (float)TOP_W;
    p->y = anywhere ? rng_unit(rng) * (float)TOP_H : (float)TOP_H + 4.0f;
    p->vx = (rng_unit(rng) - 0.5f) * 18.0f;
    p->vy = -(5.0f + rng_unit(rng) * 13.0f);   /* slowly drifting upwards */
    p->size = 1.0f + floorf(rng_unit(rng) * 3.0f);   /* 1..3 px squares */
    p->maxLife = 1.4f + rng_unit(rng) * 3.4f;
    p->life = anywhere ? rng_unit(rng) * p->maxLife : p->maxLife;
    p->r = COL_DAMAGE_TYPE[type].r * brightness;
    p->g = COL_DAMAGE_TYPE[type].g * brightness;
    p->b = COL_DAMAGE_TYPE[type].b * brightness;
}

static void update_particles(App *app, float dt)
{
    int i;
    for (i = 0; i < MENU_PARTICLE_COUNT; i++) {
        MenuParticle *p = &app->particles[i];
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->life -= dt;

        if (p->x < 0.0f) {
            p->x = 0.0f;
            p->vx = -p->vx;
        } else if (p->x > (float)TOP_W) {
            p->x = (float)TOP_W;
            p->vx = -p->vx;
        }

        if (p->life <= 0.0f || p->y < -4.0f)
            particle_spawn(app, p, false);
    }
}

/* ------------------------------------------------------------------------ */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------ */

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

void app_new_run(App *app, uint32_t seed)
{
    game_new_run(&app->game, seed);
    /* Make the next menu start differ from this one.  main.c also reseeds
     * app->seed from the RTC before the first run. */
    app->seed = seed * 1664525u + 1013904223u;
    app->menuTime = 0.0f;
    app->waveTrackedWave = app->game.wave;
    app->waveEnemyTotal = 0;

    /* fresh mascot/banner state for the new run */
    app->mascotHappy = 0.0f;
    app->mascotWorried = 0.0f;
    app->mascotTowers = app->game.towerCount;
    app->mascotLeaks = app->game.leaks;
    app->mascotLives = app->game.lives;
    app->waveBannerTimer = 0.0f;
    app->waveBannerWave = app->game.wave;
}

bool app_is_paused(const App *app)
{
    return app->game.paused;
}

/* ------------------------------------------------------------------------ */
/* Player actions                                                            */
/* ------------------------------------------------------------------------ */

static void move_cursor(App *app, int dcol, int drow)
{
    Game *g = &app->game;
    int col = clamp_int(g->cursorCol + dcol, 0, GRID_COLS - 1);
    int row = clamp_int(g->cursorRow + drow, 0, GRID_ROWS - 1);

    if (col == g->cursorCol && row == g->cursorRow)
        return;

    game_set_cursor(g, col, row);

    /* Keep the info panel honest: when the cursor leaves the selected tower,
     * drop the selection instead of silently upgrading a far away tower. */
    if (g->selectedTower >= 0 && game_tower_at(g, col, row) != g->selectedTower)
        game_select_tower(g, -1);
}

static void cycle_type(App *app, int delta)
{
    Game *g = &app->game;
    int type = g->selectedType;

    if (type < 0 || type >= TOWER_TYPE_COUNT)
        type = 0;
    type = (type + delta) % TOWER_TYPE_COUNT;
    if (type < 0)
        type += TOWER_TYPE_COUNT;
    game_set_selected_type(g, type);
}

static void action_confirm(App *app)
{
    Game *g = &app->game;
    int col = g->cursorCol;
    int row = g->cursorRow;
    int tower = game_tower_at(g, col, row);

    if (tower >= 0) {
        game_select_tower(g, tower);
        return;
    }

    /* Placement is allowed in both build and wave phases, like the original.
     * The core validates gold / path rules and sets Game.message on failure. */
    game_place_tower(g, col, row);
}

static void action_upgrade(App *app)
{
    Game *g = &app->game;

    if (g->selectedTower < 0) {
        int tower = game_tower_at(g, g->cursorCol, g->cursorRow);
        if (tower >= 0)
            game_select_tower(g, tower);
    }
    game_upgrade_selected(g);
}

static void action_sell(App *app)
{
    Game *g = &app->game;

    if (g->selectedTower < 0) {
        int tower = game_tower_at(g, g->cursorCol, g->cursorRow);
        if (tower >= 0)
            game_select_tower(g, tower);
    }
    game_sell_selected(g);
}

/* Applies one bottom-screen button press.  Game actions all live here so the
 * UI file never needs to know about the simulation. */
static void apply_ui_event(App *app, const UiEvent *ev)
{
    Game *g = &app->game;

    switch (ev->type) {
    case UI_EVENT_START:
        if (g->phase == PHASE_MENU)
            app_new_run(app, app->seed);
        break;

    case UI_EVENT_SELECT_TYPE:
        if (ev->index >= 0 && ev->index < TOWER_TYPE_COUNT)
            game_set_selected_type(g, ev->index);
        break;

    case UI_EVENT_UPGRADE:
        action_upgrade(app);
        break;

    case UI_EVENT_SELL:
        action_sell(app);
        break;

    case UI_EVENT_NEXT_WAVE:
        if (g->phase == PHASE_BUILD)
            game_start_next_wave(g);
        break;

    case UI_EVENT_CYCLE_SPEED:
        game_cycle_speed(g);
        break;

    case UI_EVENT_TOGGLE_PAUSE:
        if (g->phase == PHASE_BUILD || g->phase == PHASE_WAVE)
            game_toggle_pause(g);
        break;

    case UI_EVENT_RESUME:
        if (g->paused)
            game_toggle_pause(g);
        break;

    case UI_EVENT_RETRY:
        if (g->phase == PHASE_GAMEOVER || g->phase == PHASE_VICTORY)
            app_new_run(app, app->seed);
        break;

    case UI_EVENT_MENU:
        if (g->phase == PHASE_GAMEOVER || g->phase == PHASE_VICTORY || g->paused)
            game_init(g);
        break;

    case UI_EVENT_NONE:
    default:
        break;
    }
}

/* Host-preview debug channel: a click on the TOP screen moves the build cursor
 * and selects whatever tower is under it.  Real hardware has no top touch, so
 * this is only reachable when Input.touchTop is set by the preview harness. */
static void handle_top_tap(App *app, float px, float py)
{
    Game *g = &app->game;
    int col, row;

    if (!board_hit_cell(px, py, &col, &row))
        return;

    game_set_cursor(g, col, row);
    game_select_tower(g, game_tower_at(g, col, row));
}

/* ------------------------------------------------------------------------ */
/* Per-phase tick                                                            */
/* ------------------------------------------------------------------------ */

static void tick_menu(App *app, const Input *in)
{
    UiEvent ev;

    if (in->touchPressed) {
        if (in->touchTop) {
            app_new_run(app, app->seed);
            return;
        }
        if (ui_hit_test(app, in->touchX, in->touchY, &ev) && ev.type == UI_EVENT_START) {
            app_new_run(app, app->seed);
            return;
        }
    }

    if (in->a || in->start)
        app_new_run(app, app->seed);
}

static void tick_result(App *app, const Input *in)
{
    UiEvent ev;

    if (in->touchPressed) {
        if (in->touchTop) {
            app_new_run(app, app->seed);
            return;
        }
        if (ui_hit_test(app, in->touchX, in->touchY, &ev)) {
            if (ev.type == UI_EVENT_RETRY) {
                app_new_run(app, app->seed);
                return;
            }
            if (ev.type == UI_EVENT_MENU) {
                game_init(&app->game);
                return;
            }
        }
    }

    if (in->a) {
        app_new_run(app, app->seed);
        return;
    }
    if (in->b)
        game_init(&app->game);
}

static void tick_play(App *app, const Input *in, float dt)
{
    Game *g = &app->game;
    UiEvent ev;

    /* Pause is deliberately handled before anything else so a pause press can
     * never also trigger an action in the same frame. */
    if (in->start) {
        game_toggle_pause(g);
        if (g->paused)
            return;
    }

    if (g->paused) {
        if (in->touchPressed && !in->touchTop &&
            ui_hit_test(app, in->touchX, in->touchY, &ev))
            apply_ui_event(app, &ev);

        /* The core keeps ticking UI message timers while paused; hand it the
         * frame so "not enough gold" toasts still expire. */
        game_update(g, dt);
        return;
    }

    if (in->select)
        game_cycle_speed(g);

    if (in->touchPressed) {
        if (in->touchTop)
            handle_top_tap(app, in->touchX, in->touchY);
        else if (ui_hit_test(app, in->touchX, in->touchY, &ev))
            apply_ui_event(app, &ev);
    }

    if (in->up)
        move_cursor(app, 0, -1);
    if (in->down)
        move_cursor(app, 0, 1);
    if (in->left)
        move_cursor(app, -1, 0);
    if (in->right)
        move_cursor(app, 1, 0);

    if (in->l)
        cycle_type(app, -1);
    if (in->r)
        cycle_type(app, 1);
    if (in->a)
        action_confirm(app);
    if (in->x)
        action_upgrade(app);
    if (in->y)
        action_sell(app);
    if (in->b)
        game_select_tower(g, -1);

    game_update(g, dt);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

void app_init(App *app)
{
    int i;

    memset(app, 0, sizeof(*app));
    game_init(&app->game);

    /* Deterministic by default so the host preview tool produces identical
     * screenshots run to run; main.c overwrites this with real entropy. */
    app->seed = 0x5EED1234u;
    app->particleRng = 0x1BADB002u;
    app->lastMusicPhase = -1;
    app->mascotTowers = app->game.towerCount;
    app->mascotLeaks = app->game.leaks;
    app->mascotLives = app->game.lives;
    app->waveBannerTimer = 0.0f;
    app->waveBannerWave = app->game.wave;

    for (i = 0; i < MENU_PARTICLE_COUNT; i++)
        particle_spawn(app, &app->particles[i], true);
}

void app_tick(App *app, const Input *input, float dt)
{
    static const Input kEmptyInput;
    Game *g;

    if (app == NULL)
        return;
    if (input == NULL)
        input = &kEmptyInput;

    /* A long pause (or a stutter while the OS suspends the process) must not
     * teleport enemies across half the map. */
    if (!(dt > 0.0f))
        dt = 0.0f;
    if (dt > 0.1f)
        dt = 0.1f;

    app->time += dt;
    app->frames++;

    g = &app->game;
    if (g->phase == PHASE_MENU) {
        app->menuTime += dt;
        update_particles(app, dt);
    }

    switch (g->phase) {
    case PHASE_MENU:
        tick_menu(app, input);
        break;

    case PHASE_BUILD:
    case PHASE_WAVE:
        tick_play(app, input, dt);
        break;

    case PHASE_GAMEOVER:
    case PHASE_VICTORY:
        tick_result(app, input);
        break;

    default:
        break;
    }

    /* Wave progress denominator for ui.c: the core does not expose a wave's
     * total spawn count, so remember the largest (alive + queued) seen during
     * the wave.  Reset as soon as Game.wave advances. */
    if (g->wave != app->waveTrackedWave) {
        app->waveTrackedWave = g->wave;
        app->waveEnemyTotal = 0;
    }
    if (g->phase == PHASE_WAVE) {
        int remaining = g->enemiesAlive + g->enemiesToSpawn;
        if (remaining > app->waveEnemyTotal)
            app->waveEnemyTotal = remaining;
    }

    /* Cute mascot reactions (edge-detected so ui.c stays a pure renderer). */
    if (g->towerCount > app->mascotTowers)
        app->mascotHappy = 1.0f;            /* tower placed / upgraded  */
    else if (g->towerCount < app->mascotTowers)
        app->mascotHappy = 0.55f;           /* tower sold               */
    app->mascotTowers = g->towerCount;
    if (g->leaks > app->mascotLeaks)
        app->mascotWorried = 1.30f;         /* a creep got through      */
    app->mascotLeaks = g->leaks;
    if (g->lives < app->mascotLives)
        app->mascotWorried = 1.60f;         /* lives lost               */
    app->mascotLives = g->lives;
    if (app->mascotHappy > 0.0f) {
        app->mascotHappy -= dt;
        if (app->mascotHappy < 0.0f)
            app->mascotHappy = 0.0f;
    }
    if (app->mascotWorried > 0.0f) {
        app->mascotWorried -= dt;
        if (app->mascotWorried < 0.0f)
            app->mascotWorried = 0.0f;
    }

    /* One-shot top-screen banner on the BUILD -> WAVE edge. */
    if (g->phase == PHASE_WAVE && app->lastMusicPhase != (int)PHASE_WAVE) {
        app->waveBannerTimer = 1.5f;
        app->waveBannerWave = g->wave;
    }
    if (g->phase == PHASE_WAVE) {
        if (app->waveBannerTimer > 0.0f) {
            app->waveBannerTimer -= dt;
            if (app->waveBannerTimer < 0.0f)
                app->waveBannerTimer = 0.0f;
        }
    } else {
        app->waveBannerTimer = 0.0f;
        app->waveBannerWave = g->wave;
    }

    /* Music track follows the phase.  Only call audio_set_music() when the
     * phase actually changes: the 3DS backend is idempotent, but this keeps
     * the portable fallback's recorded track deterministic for the preview
     * harness/tests.  Results keep the game track (stings arrive as events). */
    if ((int)g->phase != app->lastMusicPhase) {
        AudioMusic track = (g->phase == PHASE_MENU) ? AUDIO_MUSIC_MENU
                                                    : AUDIO_MUSIC_GAME;
        audio_set_music(track);
        app->lastMusicPhase = (int)g->phase;
    }

    /* Never leave feedback bits sitting in the state: the platform audio
     * layer consumes them and the core clears them. */
    audio_play_event(g->events);
    game_clear_events(g);
}

void app_draw(App *app)
{
    gfx_begin_frame();

    gfx_target(SCREEN_TOP);
    render_top(app);

    gfx_target(SCREEN_BOTTOM);
    ui_draw(app);

    gfx_present();
}
