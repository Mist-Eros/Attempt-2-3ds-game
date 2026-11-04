/*
 * Elemental Defense - simulation core (v0.3 combat model).
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
 * Order of operations inside a wave step (mirrors main.lua love.update, with the
 * v0.3 damage-over-time phases added):
 *   spawn -> status effects tick (burn/acid, slag timers) -> move enemies
 *   (stun/slag aware, leaks) -> towers shoot (layers + on-hit effects) ->
 *   cleanup -> wave complete check.
 *
 * Damage model (elements-spec.md):
 *   enemies are concentric layers, outermost first. All damage (direct, burn,
 *   acid, blast) goes through apply_damage(): the current outermost layer takes
 *   damage * type-vs-layer multiplier; a destroyed layer carries the unspent
 *   raw damage into the next layer, which applies its own multiplier. Slagged
 *   enemies take x1.5 from every source.
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

/* ------------------------------------------------------------------ */
/* layered damage                                                     */
/* ------------------------------------------------------------------ */

/*
 * Apply `amount` raw damage of `damageType` to an enemy, outside-in, with the
 * excess carrying into the next layer (elements-spec.md section 1/2). Returns
 * true when the innermost layer is destroyed (the caller handles the kill).
 */
static bool apply_damage(Game *g, Enemy *e, int damageType, float amount)
{
    float pool;
    float amp;

    (void)g;
    if (!e || !e->active || amount <= 0.0f) {
        return false;
    }

    pool = amount;
    /* Slag: +50% damage taken from everything (direct, burn, acid, blast). */
    amp = (e->slagTimer > 0.0f) ? core_slag_damage_amp : 1.0f;

    while (pool > 0.0f && e->layerCount > 0) {
        EnemyLayer *layer = &e->layers[0];
        float multiplier = game_damage_multiplier(damageType, layer->type) * amp;
        float scaled = pool * multiplier;

        if (multiplier <= 0.0f) {
            break; /* cannot happen with the spec matrix, but do not divide */
        }
        if (scaled < layer->hp) {
            layer->hp -= scaled;
            pool = 0.0f;
            break;
        }

        /* Layer destroyed. Work out how much raw damage it ate, then carry the
         * rest into the next layer (which applies its own multiplier). */
        pool -= layer->hp / multiplier;
        if (pool < 0.0f) {
            pool = 0.0f;
        }
        layer->hp = 0.0f;
        {
            int i;
            for (i = 1; i < e->layerCount; ++i) {
                e->layers[i - 1] = e->layers[i];
            }
            memset(&e->layers[e->layerCount - 1], 0,
                   sizeof e->layers[e->layerCount - 1]);
            --e->layerCount;
        }
    }

    return e->layerCount == 0;
}

static void kill_enemy(Game *g, Enemy *e)
{
    if (!e->active) {
        return;
    }
    e->active = false;
    e->layerCount = 0;
    --g->enemiesAlive;
    ++g->kills;
    g->gold += CORE_KILL_REWARD;
    g->events |= GAME_EVT_ENEMY_DIE;
}

/* ------------------------------------------------------------------ */
/* beams and puddles                                                  */
/* ------------------------------------------------------------------ */

static void spawn_beam(Game *g, const Tower *tower, float x2, float y2,
                       int kind, float radius)
{
    int slot;
    Beam *beam;
    Vec2 from;
    float life;

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
    beam->damageType = tower->damageType;
    beam->kind = (uint8_t)kind;
    beam->x1 = from.x;
    beam->y1 = from.y;
    beam->x2 = x2;
    beam->y2 = y2;
    beam->radius = radius;
    life = (kind == 1) ? core_explosion_life : CORE_BEAM_LIFE;
    beam->life = life;
    beam->maxLife = life;
}

/* Spawn an acid puddle at a cell-space position. At most MAX_PUDDLES live;
 * when full the round-robin slot (the oldest puddle) is replaced. */
static void spawn_puddle(Game *g, float x, float y)
{
    int slot = -1;
    Puddle *puddle;

    for (int i = 0; i < MAX_PUDDLES; ++i) {
        if (!g->puddles[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = g->puddleNext;
        g->puddleNext = (g->puddleNext + 1) % MAX_PUDDLES;
    }

    puddle = &g->puddles[slot];
    puddle->active = true;
    puddle->x = x;
    puddle->y = y;
    puddle->radius = core_puddle_radius;
    puddle->life = core_puddle_life;
    puddle->maxLife = core_puddle_life;
    puddle->dps = core_puddle_dps;
}

/* ------------------------------------------------------------------ */
/* spawning                                                           */
/* ------------------------------------------------------------------ */

static int spawn_enemy(Game *g, bool elite, int spawnSlot)
{
    int slot;
    Enemy *e;
    Vec2 start;
    uint8_t types[MAX_LAYERS];
    float totalHp;
    float perLayer;
    int layerCount;
    int i;

    slot = -1;
    if (g->enemiesAlive < MAX_ENEMIES) {
        for (i = 0; i < MAX_ENEMIES; ++i) {
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
    memset(e, 0, sizeof *e);
    e->active = true;
    e->elite = elite;

    /* Layers: count and types are a pure function of (seed, wave, spawn slot),
     * so game_wave_preview() reports exactly what will spawn (spec section 1). */
    layerCount = core_enemy_layer_types(g->rng, g->wave, elite, spawnSlot,
                                        types);
    totalHp = core_enemy_hp(g->wave, elite);
    perLayer = totalHp / (float)layerCount;
    e->layerCount = (uint8_t)layerCount;
    for (i = 0; i < layerCount; ++i) {
        e->layers[i].type = types[i];
        e->layers[i].maxHp = perLayer;
        e->layers[i].hp = perLayer;
    }

    e->pathPos = 0.0f;
    e->hitFlash = 0.0f;
    e->burnStacks = 0;
    e->burnTimer = 0.0f;
    e->stunTimer = 0.0f;
    e->slagTimer = 0.0f;
    core_path_position(&g->path, 0.0f, &start);
    e->x = start.x;
    e->y = start.y;

    ++g->enemiesAlive;
    return slot;
}

/* Absolute movement speed in cells per second (see core_enemy_speed_cells():
 * the original normalised speed over the path, which made mazing pointless;
 * this port deliberately does not). */
static float enemy_speed_cells(const Game *g, bool elite)
{
    return core_enemy_speed_cells(g->wave, elite);
}

static void start_wave(Game *g)
{
    int hpLayers;
    int shieldLayers;
    int armorLayers;
    int elites;

    g->phase = PHASE_WAVE;
    g->waveTime = 0.0f;
    g->spawnTimer = 0.0f;
    g->selectedTower = -1;

    g->enemiesToSpawn = core_wave_total_spawns(g->wave);
    g->spawnInterval = core_wave_spawn_interval(g->wave);
    g->elitePending = core_wave_elite_count(g->wave);
    g->eliteDelayCount = g->elitePending;
    g->enemiesAlive = 0;

    /* Stage the truthful per-layer totals (same pure function the preview uses)
     * so the UI can read them during the wave if it wants to. */
    game_wave_preview(g, &hpLayers, &shieldLayers, &armorLayers, &elites);
    g->layerQuota[LAYER_HP] = (uint8_t)hpLayers;
    g->layerQuota[LAYER_SHIELD] = (uint8_t)shieldLayers;
    g->layerQuota[LAYER_ARMOR] = (uint8_t)armorLayers;

    g->events |= GAME_EVT_WAVE_START;
}

static void update_spawning(Game *g, float sdt)
{
    int eliteCount;

    if (g->enemiesToSpawn <= 0) {
        return;
    }
    eliteCount = core_wave_elite_count(g->wave);

    g->spawnTimer += sdt;
    while (g->spawnTimer >= g->spawnInterval && g->enemiesToSpawn > 0) {
        int spawnSlot;
        bool elite;

        /* Do not consume a spawn when the enemy array is full; the wave would
         * silently lose enemies otherwise. The balance never gets close. */
        if (g->enemiesAlive >= MAX_ENEMIES) {
            if (g->spawnTimer > g->spawnInterval) {
                g->spawnTimer = g->spawnInterval;
            }
            break;
        }
        g->spawnTimer -= g->spawnInterval;

        spawnSlot = core_wave_total_spawns(g->wave) - g->enemiesToSpawn;
        elite = core_spawn_slot_is_elite(spawnSlot, eliteCount);
        if (spawn_enemy(g, elite, spawnSlot) < 0) {
            break;
        }
        --g->enemiesToSpawn;
        if (elite) {
            if (g->elitePending > 0) {
                --g->elitePending;
            }
            if (g->eliteDelayCount > 0) {
                --g->eliteDelayCount;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* status effects (burn / stun / slag / acid puddles)                 */
/* ------------------------------------------------------------------ */

static void update_status_effects(Game *g, float sdt)
{
    int i;

    /* Per-enemy effects. Burn ticks first, then the timers run out. */
    for (i = 0; i < MAX_ENEMIES; ++i) {
        Enemy *e = &g->enemies[i];

        if (!e->active) {
            continue;
        }
        if (e->hitFlash > 0.0f) {
            e->hitFlash -= sdt;
            if (e->hitFlash < 0.0f) {
                e->hitFlash = 0.0f;
            }
        }
        if (e->burnStacks > 0) {
            float burnDamage = (float)e->burnStacks * core_burn_dps_per_stack * sdt;
            if (apply_damage(g, e, DMG_FIRE, burnDamage)) {
                kill_enemy(g, e);
                continue;
            }
            e->burnTimer -= sdt;
            if (e->burnTimer <= 0.0f) {
                e->burnTimer = 0.0f;
                e->burnStacks = 0;
            }
        }
        if (e->slagTimer > 0.0f) {
            e->slagTimer -= sdt;
            if (e->slagTimer < 0.0f) {
                e->slagTimer = 0.0f;
            }
        }
    }

    /* Acid puddles: lifetime, then damage every enemy inside the radius. The
     * damage type is corrosive, so each enemy's outer layer multiplier applies. */
    for (i = 0; i < MAX_PUDDLES; ++i) {
        Puddle *puddle = &g->puddles[i];
        float radiusSq;

        if (!puddle->active) {
            continue;
        }
        puddle->life -= sdt;
        if (puddle->life <= 0.0f) {
            puddle->life = 0.0f;
            puddle->active = false;
            continue;
        }
        radiusSq = puddle->radius * puddle->radius;
        for (int j = 0; j < MAX_ENEMIES; ++j) {
            Enemy *e = &g->enemies[j];
            float dx;
            float dy;

            if (!e->active) {
                continue;
            }
            dx = e->x - puddle->x;
            dy = e->y - puddle->y;
            if (dx * dx + dy * dy > radiusSq) {
                continue;
            }
            if (apply_damage(g, e, DMG_CORROSIVE, puddle->dps * sdt)) {
                kill_enemy(g, e);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* movement                                                           */
/* ------------------------------------------------------------------ */

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

        /* Stunned enemies do not advance at all (spec section 2). */
        if (e->stunTimer > 0.0f) {
            e->stunTimer -= sdt;
            if (e->stunTimer < 0.0f) {
                e->stunTimer = 0.0f;
            }
            continue;
        }

        {
            float speed = enemy_speed_cells(g, e->elite);
            if (e->slagTimer > 0.0f) {
                speed *= core_slag_speed_mult; /* slagged enemies are slowed */
            }
            e->pathPos += sdt * speed;
        }

        if (e->pathPos >= g->path.length) {
            /* Reached the goal: costs one life, gives no gold. */
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

/* ------------------------------------------------------------------ */
/* towers and combat                                                  */
/* ------------------------------------------------------------------ */

/*
 * Target selection (spec section 2): nearest enemy in range, except FIRE
 * prefers enemies whose outer layer is HP, SHOCK prefers SHIELD and CORROSIVE
 * prefers ARMOR; if none in range has the preferred outer layer, fall back to
 * nearest. Ties go to the oldest enemy (lowest index in the array).
 */
static Enemy *find_target(Game *g, Vec2 centre, float range, int damageType)
{
    float rangeSq = range * range;
    Enemy *nearest = NULL;
    float nearestSq = rangeSq;
    Enemy *preferred = NULL;
    float preferredSq = rangeSq;
    int preferredLayer = -1;
    int i;

    switch (damageType) {
        case DMG_FIRE: preferredLayer = LAYER_HP; break;
        case DMG_ELECTRIC: preferredLayer = LAYER_SHIELD; break;
        case DMG_CORROSIVE: preferredLayer = LAYER_ARMOR; break;
        default: preferredLayer = -1; break;
    }

    for (i = 0; i < MAX_ENEMIES; ++i) {
        Enemy *e = &g->enemies[i];
        float dx;
        float dy;
        float distSq;

        if (!e->active || e->layerCount == 0) {
            continue;
        }
        dx = e->x - centre.x;
        dy = e->y - centre.y;
        distSq = dx * dx + dy * dy;
        if (distSq > rangeSq) {
            continue;
        }

        /* Strict "<" so an equal-distance, older enemy wins the tie. */
        if (nearest == NULL || distSq < nearestSq) {
            nearest = e;
            nearestSq = distSq;
        }
        if (preferredLayer >= 0 && e->layers[0].type == preferredLayer) {
            if (preferred == NULL || distSq < preferredSq) {
                preferred = e;
                preferredSq = distSq;
            }
        }
    }

    return (preferred != NULL) ? preferred : nearest;
}

/* On-hit status effects (direct tower hits only; burn/acid ticks never retrigger). */
static void apply_on_hit_effect(Game *g, Enemy *target, int damageType)
{
    switch (damageType) {
        case DMG_FIRE:
            if (target->burnStacks < (uint8_t)core_burn_max_stacks) {
                ++target->burnStacks;
            }
            target->burnTimer = core_burn_duration;
            break;
        case DMG_ELECTRIC:
            if (target->stunTimer < core_stun_duration) {
                target->stunTimer = core_stun_duration;
            }
            break;
        case DMG_CORROSIVE:
            spawn_puddle(g, target->x, target->y);
            break;
        case DMG_SLAG:
            target->slagTimer = core_slag_duration;
            break;
        default:
            break;
    }
}

static void update_towers(Game *g, float sdt)
{
    int i;

    for (i = 0; i < g->towerCount; ++i) {
        Tower *tower = &g->towers[i];
        const TowerSpec *spec;
        float damage;
        float fireRate;
        float range;
        Vec2 centre;
        Enemy *target;

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

        spec = game_tower_spec(tower->damageType);
        if (!spec) {
            continue;
        }
        game_tower_stats(tower->damageType, tower->level, &damage, &fireRate,
                         &range);
        centre = game_cell_center(tower->col, tower->row);
        target = find_target(g, centre, range, spec->damageType);
        if (target == NULL) {
            continue;
        }

        tower->cooldown = fireRate;
        g->events |= GAME_EVT_LASER;

        if (spec->aoeRadius > 0.0f) {
            /* BLASTER: pick a target, then hit everything near it. */
            float radiusSq = spec->aoeRadius * spec->aoeRadius;
            float tx = target->x;
            float ty = target->y;
            int j;

            for (j = 0; j < MAX_ENEMIES; ++j) {
                Enemy *e = &g->enemies[j];
                float dx;
                float dy;

                if (!e->active) {
                    continue;
                }
                dx = e->x - tx;
                dy = e->y - ty;
                if (dx * dx + dy * dy > radiusSq) {
                    continue;
                }
                if (apply_damage(g, e, spec->damageType, damage)) {
                    kill_enemy(g, e);
                } else {
                    e->hitFlash = CORE_HIT_FLASH_TIME;
                }
            }
            spawn_beam(g, tower, tx, ty, 1, spec->aoeRadius);
        } else {
            bool died = apply_damage(g, target, spec->damageType, damage);

            spawn_beam(g, tower, target->x, target->y, 0, 0.0f);
            if (died) {
                kill_enemy(g, target);
            } else {
                target->hitFlash = CORE_HIT_FLASH_TIME;
                apply_on_hit_effect(g, target, spec->damageType);
            }
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

/* ------------------------------------------------------------------ */
/* road (de)projection after the maze changes                         */
/* ------------------------------------------------------------------ */

/*
 * Road changes (build/sell) can leave every living enemy on a completely
 * different polyline. Move each one to the nearest point on the new road and
 * keep its progress there: it must keep marching from wherever it stands, not
 * snap back to the entrance. Segments are one cell long, so pathPos is simply
 * segment index + clamped projection factor, i.e. still a distance in cells.
 */
static float project_onto_path(const Path *path, float x, float y)
{
    float bestDistSq = 1.0e30f;
    float bestT = 0.0f;
    int i;

    if (!path || path->count < 2) {
        return 0.0f;
    }

    for (i = 0; i + 1 < path->count; ++i) {
        Vec2 a = path->pts[i];
        Vec2 b = path->pts[i + 1];
        float dx = b.x - a.x;
        float dy = b.y - a.y;
        float lenSq = dx * dx + dy * dy;
        float t;
        float px;
        float py;
        float ex;
        float ey;
        float distSq;

        if (lenSq <= 0.0f) {
            continue;
        }
        t = ((x - a.x) * dx + (y - a.y) * dy) / lenSq;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        px = a.x + dx * t;
        py = a.y + dy * t;
        ex = x - px;
        ey = y - py;
        distSq = ex * ex + ey * ey;
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            bestT = (float)i + t;
        }
    }

    if (bestT < 0.0f) bestT = 0.0f;
    if (bestT > path->length) bestT = path->length;
    return bestT;
}

static void reproject_enemies(Game *g)
{
    Vec2 pos;
    int i;

    if (g->path.count < 2) {
        return;
    }
    for (i = 0; i < MAX_ENEMIES; ++i) {
        Enemy *e = &g->enemies[i];
        if (!e->active) {
            continue;
        }
        e->pathPos = project_onto_path(&g->path, e->x, e->y);
        core_path_position(&g->path, e->pathPos, &pos);
        e->x = pos.x;
        e->y = pos.y;
    }
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
    g->selectedType = DMG_NORMAL;
    g->selectedTower = -1;
    g->speedMultiplier = 1.0f;
    g->cursorCol = 0;
    g->cursorRow = 0;
    g->rng = 0x2545F491u; /* any non-zero seed for the menu background */
    g->spawnInterval = core_wave_spawn_interval(1);
    g->puddleNext = 0;
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
    /* Kept as the run seed. Nothing consumes it during a run: enemy layer
     * composition is a pure function of it (see core_enemy_layer_types), which
     * is what lets game_wave_preview() be exact. */
    g->rng = (seed != 0u) ? seed : 0x9E3779B9u;

    /* On an empty board the BFS finds the straight row-12 corridor:
     * 26 cells, 25 segments. Every later build/sell re-runs it. */
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
    update_status_effects(g, sdt);
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

/*
 * Build a tower. Towers MAY stand on the road (that is the maze mechanic) -
 * every empty cell except the entrance and the exit is fair game - but a build
 * that would make the exit unreachable is refused and costs nothing.
 *
 * Messages (front end may show g->message): "can't build now", "can't build
 * there", "can't build on the entrance", "can't build on the exit", "cell
 * occupied", "tower limit", "that would block the path", "not enough gold".
 */
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
    if (col == CORE_START_COL && row == CORE_START_ROW) {
        set_message(g, "can't build on the entrance");
        return false;
    }
    if (col == CORE_GOAL_COL && row == CORE_GOAL_ROW) {
        set_message(g, "can't build on the exit");
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
    /* Maze rule: refuse a build that would cut the exit off. This only runs the
     * BFS on a hypothetical board, nothing is mutated yet. */
    if (!core_path_exists(g, col, row)) {
        set_message(g, "that would block the path");
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
    tower->damageType = spec->damageType;
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

    /* The road changed: rebuild it and re-project the enemies already walking.
     * core_path_exists() above guarantees this succeeds; keep the rollback
     * anyway so a future rule change can never corrupt the board. */
    if (core_build_path(g) < 2) {
        g->occupancy[row][col] = 0;
        memset(tower, 0, sizeof *tower);
        --g->towerCount;
        g->gold += spec->cost;
        g->selectedTower = -1;
        core_build_path(g);
        reproject_enemies(g);
        set_message(g, "that would block the path");
        return false;
    }
    reproject_enemies(g);

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

    /* Removing a wall can only make the road shorter, never seal it. Re-path
     * and re-project the enemies so the shortcut is actually walked. */
    core_build_path(g);
    reproject_enemies(g);

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
