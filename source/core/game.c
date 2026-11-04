/*
 * Elemental Defense - simulation core.
 *
 * Deterministic, allocation-free, portable C99. The 3DS front end only ever
 * calls the API in include/game.h; everything in this file is driven either by
 * game_update() (real frames) or by game_step() (fixed steps, used by the host
 * tests and the headless sim).
 *
 * Frame model
 * -----------
 * game_update(dt) clamps real time, accumulates it and runs at most
 * CORE_MAX_STEPS_PER_UPDATE fixed 1/60 s steps of game_step(). game_step()
 * multiplies dt by g->speedMultiplier, so at 2x/4x each fixed step simulates
 * 2/60 or 4/60 seconds. game_step() itself never accumulates: tests calling it
 * directly get exactly the amount of simulation they asked for, which is what
 * makes both the unit tests and the headless sim reproducible.
 *
 * Order of operations inside a wave step (mirrors main.lua love.update):
 *   spawn -> move enemies (leaks) -> towers shoot -> apply damage -> cleanup
 *   -> wave complete check.
 */
#include <string.h>

#include "game.h"
#include "core_internal.h"

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static void set_message(Game *g, const char *text)
{
    int i = 0;

    if (!g) {
        return;
    }
    if (!text) {
        text = "";
    }
    while (text[i] != '\0' && i < MAX_MESSAGE_LEN - 1) {
        g->message[i] = text[i];
        ++i;
    }
    g->message[i] = '\0';
    g->messageTimer = CORE_MESSAGE_TIME;
}

static void tick_message(Game *g, float dt)
{
    if (g->messageTimer <= 0.0f) {
        return;
    }
    g->messageTimer -= dt;
    if (g->messageTimer <= 0.0f) {
        g->messageTimer = 0.0f;
        g->message[0] = '\0';
    }
}

static bool phase_is_playable(GamePhase phase)
{
    return phase == PHASE_BUILD || phase == PHASE_WAVE;
}

static bool tower_index_valid(const Game *g, int index)
{
    if (!g || index < 0 || index >= g->towerCount) {
        return false;
    }
    return g->towers[index].active;
}

/* xorshift32: the only source of randomness in the simulation. Every roll goes
 * through here so a fixed seed always produces the same run. */
static uint32_t rng_next(Game *g)
{
    uint32_t x = g->rng;

    if (x == 0) {
        x = 0x9E3779B9u; /* xorshift dies at zero; never let that happen */
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->rng = x;
    return x;
}

static int rng_below(Game *g, int bound)
{
    if (bound <= 0) {
        return 0;
    }
    return (int)(rng_next(g) % (uint32_t)bound);
}

static void rebuild_occupancy(Game *g)
{
    int i;

    memset(g->occupancy, 0, sizeof g->occupancy);
    for (i = 0; i < g->towerCount; ++i) {
        if (g->towers[i].active) {
            g->occupancy[g->towers[i].row][g->towers[i].col] = (int8_t)(i + 1);
        }
    }
}

static int spawn_enemy(Game *g, bool elite, int element)
{
    int slot;
    Enemy *e;
    Vec2 start;

    slot = -1;
    if (g->enemiesAlive < MAX_ENEMIES) {
        for (int i = 0; i < MAX_ENEMIES; ++i) {
            if (!g->enemies[i].active) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        return -1; /* full: caller retries on a later step */
    }

    e = &g->enemies[slot];
    e->active = true;
    e->element = (uint8_t)element;
    e->elite = elite;
    e->maxHp = core_enemy_hp(g->wave, elite);
    e->hp = e->maxHp;
    e->pathPos = 0.0f;
    e->hitFlash = 0.0f;
    core_path_position(&g->path, 0.0f, &start);
    e->x = start.x;
    e->y = start.y;

    ++g->enemiesAlive;
    return slot;
}

/* Cells per second. The original moved enemies by t += dt*speed/100, where t
 * runs 0..1 over the whole path, so in cell space they cross speed/100 times
 * the path length each second. */
static float enemy_speed_cells(const Game *g, bool elite)
{
    if (g->path.length <= 0.0f) {
        return 0.0f;
    }
    return (core_enemy_speed(g->wave, elite) / 100.0f) * g->path.length;
}

/*
 * Element quotas.
 *
 * The original computed a per-element quota for the HUD and then rolled every
 * spawn uniformly at random (main.lua:558-576), so the preview was a lie and
 * waves swung wildly. This port spawns the exact quota the preview shows; the
 * order is a deterministic shuffle (draw from the remaining multiset with the
 * xorshift RNG) so consecutive waves do not feel identical.
 */
static int next_regular_element(Game *g)
{
    int total = 0;
    int pick;
    int element;

    for (element = 0; element < TOWER_TYPE_COUNT; ++element) {
        total += (int)g->spawnQuota[element];
    }
    if (total <= 0) {
        /* Quota exhausted (only possible if the wave formula was edited);
         * fall back to a uniform roll rather than dividing by zero. */
        return rng_below(g, TOWER_TYPE_COUNT);
    }

    pick = rng_below(g, total);
    for (element = 0; element < TOWER_TYPE_COUNT; ++element) {
        int available = (int)g->spawnQuota[element];
        if (pick < available) {
            --g->spawnQuota[element];
            return element;
        }
        pick -= available;
    }
    return 0; /* unreachable: pick < total */
}

static void start_wave(Game *g)
{
    int base;
    int elites;
    int regular;
    int fireCount;
    int waterCount;

    g->phase = PHASE_WAVE;
    g->waveTime = 0.0f;
    g->spawnTimer = 0.0f;
    g->selectedTower = -1;

    base = core_wave_base_enemies(g->wave);
    elites = core_wave_elite_count(g->wave);
    regular = base - elites;
    if (regular < 0) {
        regular = 0;
    }

    /* Exact element quotas: fire = water = floor(regular/3), forest takes the
     * remainder (main.lua:558-565; this port spawns the quota instead of
     * rolling every spawn randomly). */
    fireCount = regular / 3;
    waterCount = regular / 3;
    g->spawnQuota[ELEMENT_FIRE] = (uint8_t)fireCount;
    g->spawnQuota[ELEMENT_WATER] = (uint8_t)waterCount;
    g->spawnQuota[ELEMENT_FOREST] = (uint8_t)(regular - fireCount - waterCount);

    /* The original's per-elite delay list always resolves to the same net
     * effect (design.md 6.4): spawn slot 1 is a regular, then all elites spawn
     * consecutively in slots 2..1+E, then regulars again. Do that. */
    g->elitePending = elites;
    g->eliteDelayCount = elites; /* informational for the UI */
    g->enemiesToSpawn = base;    /* wave size includes the elites */
    g->spawnInterval = core_wave_spawn_interval(g->wave);
    g->enemiesAlive = 0;

    g->events |= GAME_EVT_WAVE_START;
}

static void update_spawning(Game *g, float sdt)
{
    if (g->enemiesToSpawn <= 0) {
        return;
    }

    g->spawnTimer += sdt;
    while (g->spawnTimer >= g->spawnInterval && g->enemiesToSpawn > 0) {
        int total;
        int slotIndex;
        bool elite;
        int element;

        /* Do not consume a spawn when the enemy array is full; the wave would
         * silently lose enemies otherwise. The balance never gets close. */
        if (g->enemiesAlive >= MAX_ENEMIES) {
            if (g->spawnTimer > g->spawnInterval) {
                g->spawnTimer = g->spawnInterval;
            }
            break;
        }
        g->spawnTimer -= g->spawnInterval;

        total = core_wave_total_spawns(g->wave);
        slotIndex = total - g->enemiesToSpawn; /* 0-based spawn slot */
        elite = (slotIndex >= 1 && g->elitePending > 0);
        if (elite) {
            --g->elitePending;
            if (g->eliteDelayCount > 0) {
                --g->eliteDelayCount;
            }
        }
        /* Elites roll their element (they are not part of the preview quota,
         * which only describes regulars). */
        element = elite ? rng_below(g, TOWER_TYPE_COUNT)
                        : next_regular_element(g);
        if (spawn_enemy(g, elite, element) < 0) {
            break;
        }
        --g->enemiesToSpawn;
    }
}

/* Returns false if the run ended (lives hit zero) during this step. */
static bool update_enemies(Game *g, float sdt)
{
    int i;

    for (i = 0; i < MAX_ENEMIES; ++i) {
        Enemy *e = &g->enemies[i];
        Vec2 pos;

        if (!e->active) {
            continue;
        }

        if (e->hitFlash > 0.0f) {
            e->hitFlash -= sdt;
            if (e->hitFlash < 0.0f) {
                e->hitFlash = 0.0f;
            }
        }

        e->pathPos += sdt * enemy_speed_cells(g, e->elite);

        if (e->pathPos >= g->path.length) {
            /* Reached the goal: costs one life, gives no gold (the original
             * gave +2 gold for this - not keeping that bug). */
            e->active = false;
            --g->enemiesAlive;
            ++g->leaks;
            if (g->lives > 0) {
                --g->lives;
            }
            g->events |= GAME_EVT_LEAK;
            if (g->lives <= 0) {
                g->lives = 0;
                g->phase = PHASE_GAMEOVER;
                g->events |= GAME_EVT_GAMEOVER;
                return false;
            }
            continue;
        }

        core_path_position(&g->path, e->pathPos, &pos);
        e->x = pos.x;
        e->y = pos.y;
    }
    return true;
}

static void spawn_beam(Game *g, const Tower *tower, const Enemy *enemy)
{
    int slot;
    Beam *beam;
    Vec2 from;

    slot = -1;
    for (int i = 0; i < MAX_BEAMS; ++i) {
        if (!g->beams[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return; /* purely cosmetic: dropping a beam is fine */
    }

    from = game_cell_center(tower->col, tower->row);
    beam = &g->beams[slot];
    beam->active = true;
    beam->element = tower->element;
    beam->x1 = from.x;
    beam->y1 = from.y;
    beam->x2 = enemy->x;
    beam->y2 = enemy->y;
    beam->life = CORE_BEAM_LIFE;
    beam->maxLife = CORE_BEAM_LIFE;
}

static void update_towers(Game *g, float sdt)
{
    int i;

    for (i = 0; i < g->towerCount; ++i) {
        Tower *tower = &g->towers[i];
        float damage;
        float fireRate;
        float range;
        float rangeSq;
        float bestDistSq;
        Vec2 centre;
        Enemy *target;
        int j;

        if (!tower->active) {
            continue;
        }

        tower->animPhase += sdt;
        if (tower->cooldown > 0.0f) {
            tower->cooldown -= sdt;
            if (tower->cooldown < 0.0f) {
                tower->cooldown = 0.0f;
            }
        }
        if (tower->cooldown > 0.0f || g->enemiesAlive <= 0) {
            continue;
        }

        game_tower_stats(tower->element, tower->level, &damage, &fireRate,
                         &range);
        rangeSq = range * range;
        bestDistSq = rangeSq;
        target = NULL;
        centre = game_cell_center(tower->col, tower->row);

        /* Original targeting (main.lua:725-737): nearest enemy in range. */
        for (j = 0; j < MAX_ENEMIES; ++j) {
            Enemy *enemy = &g->enemies[j];
            float dx;
            float dy;
            float distSq;

            if (!enemy->active) {
                continue;
            }
            dx = enemy->x - centre.x;
            dy = enemy->y - centre.y;
            distSq = dx * dx + dy * dy;
            if (distSq > rangeSq) {
                continue;
            }
            if (target == NULL || distSq < bestDistSq) {
                target = enemy;
                bestDistSq = distSq;
            }
        }

        if (target == NULL) {
            continue;
        }

        tower->cooldown = fireRate;
        target->hp -= damage * game_damage_multiplier(tower->element,
                                                      target->element);
        target->hitFlash = CORE_HIT_FLASH_TIME;
        spawn_beam(g, tower, target);
        g->events |= GAME_EVT_LASER;

        if (target->hp <= 0.0f) {
            target->active = false;
            target->hp = 0.0f;
            --g->enemiesAlive;
            ++g->kills;
            g->gold += CORE_KILL_REWARD;
            g->events |= GAME_EVT_ENEMY_DIE;
        }
    }
}

static void update_tower_timers(Game *g, float sdt)
{
    int i;

    for (i = 0; i < g->towerCount; ++i) {
        Tower *tower = &g->towers[i];
        if (!tower->active) {
            continue;
        }
        tower->animPhase += sdt;
        if (tower->cooldown > 0.0f) {
            tower->cooldown -= sdt;
            if (tower->cooldown < 0.0f) {
                tower->cooldown = 0.0f;
            }
        }
    }
}

static void update_beams(Game *g, float sdt)
{
    int i;

    for (i = 0; i < MAX_BEAMS; ++i) {
        Beam *beam = &g->beams[i];
        if (!beam->active) {
            continue;
        }
        beam->life -= sdt;
        if (beam->life <= 0.0f) {
            beam->life = 0.0f;
            beam->active = false;
        }
    }
}

/* Keep enemy order stable (it decides ties when several enemies are equally
 * close to a tower) and recompute the live counter defensively. */
static void compact_enemies(Game *g)
{
    int write;
    int i;

    write = 0;
    for (i = 0; i < MAX_ENEMIES; ++i) {
        if (!g->enemies[i].active) {
            continue;
        }
        if (write != i) {
            g->enemies[write] = g->enemies[i];
            g->enemies[i].active = false;
        }
        ++write;
    }
    for (i = write; i < MAX_ENEMIES; ++i) {
        g->enemies[i].active = false;
    }
    g->enemiesAlive = write;
}

static void check_wave_complete(Game *g)
{
    if (g->phase != PHASE_WAVE) {
        return;
    }
    if (g->enemiesToSpawn > 0 || g->enemiesAlive > 0) {
        return;
    }

    ++g->wave;
    if (g->wave > g->totalWaves) {
        g->phase = PHASE_VICTORY;
        g->events |= GAME_EVT_VICTORY;
    } else {
        g->phase = PHASE_BUILD;
    }
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

void game_init(Game *g)
{
    if (!g) {
        return;
    }
    memset(g, 0, sizeof *g);
    g->phase = PHASE_MENU;
    g->wave = 1;
    g->totalWaves = CORE_TOTAL_WAVES;
    g->gold = STARTING_GOLD;
    g->lives = STARTING_LIVES;
    g->selectedType = ELEMENT_FIRE;
    g->selectedTower = -1;
    g->speedMultiplier = 1.0f;
    g->cursorCol = 0;
    g->cursorRow = 0;
    g->rng = 0x2545F491u; /* any non-zero seed for the menu background */
    g->spawnInterval = core_wave_spawn_interval(1);
}

void game_new_run(Game *g, uint32_t seed)
{
    if (!g) {
        return;
    }
    game_init(g);

    g->phase = PHASE_BUILD;
    g->wave = 1;
    g->totalWaves = CORE_TOTAL_WAVES;
    g->gold = STARTING_GOLD;
    g->lives = STARTING_LIVES;
    g->speedMultiplier = 1.0f;
    g->rng = (seed != 0u) ? seed : 0x9E3779B9u;

    /* One fixed path per run. Towers can never invalidate it because they
     * cannot be built on path cells at all. */
    g->pathBlocked = (core_build_path(g) < 2);
    g->spawnInterval = core_wave_spawn_interval(g->wave);
}

void game_update(Game *g, float dt)
{
    int steps;

    if (!g) {
        return;
    }

    if (dt < 0.0f) {
        dt = 0.0f;
    }
    if (dt > CORE_MAX_UPDATE_DT) {
        dt = CORE_MAX_UPDATE_DT;
    }

    if (g->paused) {
        tick_message(g, dt);
        return;
    }

    g->stepAccumulator += dt;
    steps = 0;
    while (g->stepAccumulator >= CORE_FIXED_STEP &&
           steps < CORE_MAX_STEPS_PER_UPDATE) {
        g->stepAccumulator -= CORE_FIXED_STEP;
        game_step(g, CORE_FIXED_STEP);
        ++steps;
    }
    /* If the machine fell far behind, drop the leftover instead of building
     * up an ever-growing catch-up spiral. */
    if (steps >= CORE_MAX_STEPS_PER_UPDATE) {
        g->stepAccumulator = 0.0f;
    }
}

void game_step(Game *g, float dt)
{
    float speed;
    float sdt;

    if (!g) {
        return;
    }
    if (dt < 0.0f) {
        dt = 0.0f;
    }
    if (dt > CORE_MAX_STEP_DT) {
        dt = CORE_MAX_STEP_DT;
    }

    /* UI messages run on wall-clock time: they still expire while paused or
     * while the game is at 4x speed. */
    tick_message(g, dt);

    if (g->paused) {
        return;
    }
    if (g->phase != PHASE_BUILD && g->phase != PHASE_WAVE) {
        return; /* menu / game over / victory freeze the battlefield */
    }

    speed = g->speedMultiplier;
    if (speed != 2.0f && speed != 4.0f) {
        speed = 1.0f;
    }
    sdt = dt * speed;

    g->timeTotal += sdt;
    update_beams(g, sdt);

    if (g->phase == PHASE_BUILD) {
        update_tower_timers(g, sdt);
        return;
    }

    /* PHASE_WAVE */
    g->waveTime += sdt;
    update_spawning(g, sdt);
    if (!update_enemies(g, sdt)) {
        return; /* run ended during this step */
    }
    compact_enemies(g);
    update_towers(g, sdt);
    compact_enemies(g);
    check_wave_complete(g);
}

/* ------------------------------------------------------------------ */
/* player actions                                                     */
/* ------------------------------------------------------------------ */

bool game_place_tower(Game *g, int col, int row)
{
    const TowerSpec *spec;
    Tower *tower;
    int type;
    int index;

    if (!g) {
        return false;
    }
    if (!phase_is_playable(g->phase)) {
        set_message(g, "can't build now");
        return false;
    }

    type = g->selectedType;
    if (type < 0) type = 0;
    if (type >= TOWER_TYPE_COUNT) type = TOWER_TYPE_COUNT - 1;
    spec = game_tower_spec(type);
    if (!spec) {
        return false;
    }

    if (col < 0 || col >= GRID_COLS || row < 0 || row >= GRID_ROWS) {
        set_message(g, "can't build there");
        return false;
    }
    if (g->path.count < 2) {
        set_message(g, "can't build there");
        return false;
    }
    if (core_path_contains(g, col, row)) {
        set_message(g, "can't build on the path");
        return false;
    }
    if (g->occupancy[row][col] != 0) {
        set_message(g, "cell occupied");
        return false;
    }
    if (g->towerCount >= MAX_TOWERS) {
        set_message(g, "tower limit");
        return false;
    }
    if (g->gold < spec->cost) {
        set_message(g, "not enough gold");
        return false;
    }

    index = g->towerCount;
    tower = &g->towers[index];
    memset(tower, 0, sizeof *tower);
    tower->active = true;
    tower->element = (uint8_t)type;
    tower->level = 1;
    tower->col = (int16_t)col;
    tower->row = (int16_t)row;
    tower->cooldown = 0.0f;
    tower->invested = spec->cost;
    tower->animPhase = 0.0f;

    ++g->towerCount;
    g->occupancy[row][col] = (int8_t)(index + 1);
    g->gold -= spec->cost;
    g->selectedTower = index;
    g->events |= GAME_EVT_BUILD;
    return true;
}

bool game_upgrade_selected(Game *g)
{
    Tower *tower;
    int cost;

    if (!g) {
        return false;
    }
    if (!phase_is_playable(g->phase)) {
        set_message(g, "can't upgrade now");
        return false;
    }
    if (!tower_index_valid(g, g->selectedTower)) {
        set_message(g, "no tower selected");
        return false;
    }

    tower = &g->towers[g->selectedTower];
    cost = core_upgrade_cost_for_level(tower->level);
    if (tower->level >= TOWER_MAX_LEVEL || cost < 0) {
        set_message(g, "max level");
        return false;
    }
    if (g->gold < cost) {
        set_message(g, "not enough gold");
        return false;
    }

    g->gold -= cost;
    tower->invested += cost;
    ++tower->level;
    g->events |= GAME_EVT_UPGRADE;
    return true;
}

bool game_sell_selected(Game *g)
{
    int index;
    int refund;
    int i;

    if (!g) {
        return false;
    }
    if (!phase_is_playable(g->phase)) {
        set_message(g, "can't sell now");
        return false;
    }
    index = g->selectedTower;
    if (!tower_index_valid(g, index)) {
        set_message(g, "no tower selected");
        return false;
    }

    refund = game_sell_value(g, index);
    g->gold += refund;

    /* Compact the tower array and rebuild the grid indices. MAX_TOWERS is 96
     * so the memmove-ish shift is trivial. */
    for (i = index; i < g->towerCount - 1; ++i) {
        g->towers[i] = g->towers[i + 1];
    }
    --g->towerCount;
    memset(&g->towers[g->towerCount], 0, sizeof g->towers[g->towerCount]);
    rebuild_occupancy(g);

    g->selectedTower = -1;
    g->events |= GAME_EVT_SELL;
    return true;
}

void game_start_next_wave(Game *g)
{
    if (!g) {
        return;
    }
    if (g->phase != PHASE_BUILD || g->paused) {
        return;
    }
    start_wave(g);
}

void game_set_selected_type(Game *g, int type)
{
    if (!g || type < 0 || type >= TOWER_TYPE_COUNT) {
        return;
    }
    g->selectedType = type;
}

void game_select_tower(Game *g, int towerIndex)
{
    if (!g) {
        return;
    }
    g->selectedTower = tower_index_valid(g, towerIndex) ? towerIndex : -1;
}

void game_set_cursor(Game *g, int col, int row)
{
    if (!g) {
        return;
    }
    if (col < 0) col = 0;
    if (col >= GRID_COLS) col = GRID_COLS - 1;
    if (row < 0) row = 0;
    if (row >= GRID_ROWS) row = GRID_ROWS - 1;
    g->cursorCol = col;
    g->cursorRow = row;
}

void game_toggle_pause(Game *g)
{
    if (!g) {
        return;
    }
    g->paused = !g->paused;
    g->stepAccumulator = 0.0f; /* no catch-up burst after unpausing */
}

void game_cycle_speed(Game *g)
{
    if (!g) {
        return;
    }
    if (g->speedMultiplier < 1.5f) {
        g->speedMultiplier = 2.0f;
    } else if (g->speedMultiplier < 3.0f) {
        g->speedMultiplier = 4.0f;
    } else {
        g->speedMultiplier = 1.0f;
    }
}

void game_clear_events(Game *g)
{
    if (g) {
        g->events = 0u;
    }
}

void game_clear_message(Game *g)
{
    if (!g) {
        return;
    }
    g->message[0] = '\0';
    g->messageTimer = 0.0f;
}
