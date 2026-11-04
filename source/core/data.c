/*
 * Elemental Defense - content tables and balance formulas (v0.3).
 *
 * Source of truth for all numbers: docs/elements-spec.md (Borderlands-style
 * damage model) for the six towers / multipliers / status effects, and the
 * original LÖVE game (main.lua) for the wave, economy and upgrade numbers that
 * the spec kept. The simulation in game.c reads everything through these
 * accessors; it never hard-codes a balance number itself.
 *
 * STATUS-EFFECT CONSTANTS: deliberately defined here as extern consts (not in
 * the public header) so the whole balance sheet lives in one file.
 */
#include <stddef.h>

#include "game.h"
#include "core_internal.h"

/*
 * Tower range: the original worked in pixels (60 px, cell = 28 px). The core
 * works in cells, so range is 60/28 cells for every damage type (spec section 2).
 */
#define TOWER_RANGE_CELLS (60.0f / 28.0f)
#define TOWER_BASE_COOLDOWN 0.80f /* the level-1 cooldown all towers share */

/* ---- status effects (elements-spec.md section 2) ---- */

const int   core_burn_max_stacks = 5;
const float core_burn_duration = 2.5f;
const float core_burn_dps_per_stack = 1.0f;
const float core_stun_duration = 0.30f;
const float core_puddle_radius = 0.7f;
const float core_puddle_life = 5.0f;
const float core_puddle_dps = 2.0f;
const float core_slag_duration = 4.0f;
const float core_slag_damage_amp = 1.5f;
const float core_slag_speed_mult = 0.65f;
const float core_explosion_life = 0.30f;

/*
 * The six towers, one damage type each (elements-spec.md section 2).
 * Colours are the element identities the renderer/UI use for icons, beams and
 * the palette; they are data, the front end only reads them.
 */
static const TowerSpec kTowerSpecs[TOWER_TYPE_COUNT] = {
    {
        /* GUN */ "GUN", 20, 3.0f, TOWER_BASE_COOLDOWN, TOWER_RANGE_CELLS,
        DMG_NORMAL, 0.0f,
        { 0.75f, 0.75f, 0.78f }, { 0.45f, 0.45f, 0.50f }
    },
    {
        /* FIRE */ "FIRE", 20, 3.0f, TOWER_BASE_COOLDOWN, TOWER_RANGE_CELLS,
        DMG_FIRE, 0.0f,
        { 0.95f, 0.45f, 0.12f }, { 0.95f, 0.30f, 0.05f }
    },
    {
        /* SHOCK */ "SHOCK", 20, 3.0f, TOWER_BASE_COOLDOWN, TOWER_RANGE_CELLS,
        DMG_ELECTRIC, 0.0f,
        { 0.25f, 0.80f, 1.00f }, { 0.10f, 0.60f, 1.00f }
    },
    {
        /* CORROSIVE */ "CORROSIVE", 20, 3.0f, TOWER_BASE_COOLDOWN,
        TOWER_RANGE_CELLS, DMG_CORROSIVE, 0.0f,
        { 0.45f, 0.85f, 0.15f }, { 0.30f, 0.80f, 0.10f }
    },
    {
        /* SLAG - low damage, marks targets (spec section 2) */
        "SLAG", 15, 1.0f, TOWER_BASE_COOLDOWN, TOWER_RANGE_CELLS,
        DMG_SLAG, 0.0f,
        { 0.70f, 0.25f, 0.90f }, { 0.60f, 0.15f, 0.85f }
    },
    {
        /* BLASTER - explosive AoE, 3x slower, 3x damage (spec section 2) */
        "BLASTER", 30, 9.0f, TOWER_BASE_COOLDOWN * 3.0f, TOWER_RANGE_CELLS,
        DMG_EXPLOSIVE, 1.3f,
        { 1.00f, 0.70f, 0.15f }, { 1.00f, 0.50f, 0.05f }
    }
};

/* Damage type x layer type (elements-spec.md section 2). Rows: DamageType,
 * columns: LayerType (HP / SHIELD / ARMOR). 2.0 strong, 0.5 weak. */
static const float kDamageMultiplier[TOWER_TYPE_COUNT][3] = {
    /* vs HP  vs SHIELD  vs ARMOR */
    { 1.0f,   1.0f,      1.0f }, /* DMG_NORMAL    */
    { 2.0f,   0.5f,      0.5f }, /* DMG_FIRE      */
    { 0.5f,   2.0f,      0.5f }, /* DMG_ELECTRIC  */
    { 0.5f,   0.5f,      2.0f }, /* DMG_CORROSIVE */
    { 1.0f,   1.0f,      1.0f }, /* DMG_SLAG      */
    { 1.0f,   1.0f,      1.0f }  /* DMG_EXPLOSIVE */
};

/* Cooldown table: base cooldown per level, the same for every tower.
 * BLASTER's spec cooldown is 3x this (see game_tower_stats). */
static const float kCooldownByLevel[TOWER_MAX_LEVEL] = {
    0.80f, 0.72f, 0.64f, 0.57f, 0.51f, 0.45f, 0.40f, 0.36f, 0.32f, 0.28f
};

/* main.lua:1430 - cost to go from the level in the index to the next level is
 * 20 * 2^(level-1) for every tower. The last slot is unused (level 10 is cap). */
static const int kUpgradeCostByLevel[TOWER_MAX_LEVEL] = {
    20, 40, 80, 160, 320, 640, 1280, 2560, 5120, 0
};

/* ------------------------------------------------------------------ */

const TowerSpec *game_tower_spec(int type)
{
    if (type < 0 || type >= TOWER_TYPE_COUNT) {
        return NULL;
    }
    return &kTowerSpecs[type];
}

float game_damage_multiplier(int damageType, int layerType)
{
    if (damageType < 0) damageType = 0;
    if (damageType >= TOWER_TYPE_COUNT) damageType = TOWER_TYPE_COUNT - 1;
    if (layerType < 0) layerType = 0;
    if (layerType > LAYER_ARMOR) layerType = LAYER_ARMOR;
    return kDamageMultiplier[damageType][layerType];
}

void game_tower_stats(int type, int level,
                      float *damage, float *fireRate, float *range)
{
    const TowerSpec *spec;
    float cooldownScale;

    if (type < 0) type = 0;
    if (type >= TOWER_TYPE_COUNT) type = TOWER_TYPE_COUNT - 1;
    if (level < 1) level = 1;
    if (level > TOWER_MAX_LEVEL) level = TOWER_MAX_LEVEL;

    spec = &kTowerSpecs[type];
    cooldownScale = spec->fireRate / TOWER_BASE_COOLDOWN;

    if (damage) {
        /* Spec: upgrades double damage per level (levels 1-10). */
        *damage = spec->damage * (float)(1 << (level - 1));
    }
    if (fireRate) {
        /* Spec: existing cooldown table; BLASTER's is 3x the table. */
        *fireRate = kCooldownByLevel[level - 1] * cooldownScale;
    }
    /* The original never improves range on upgrade; it is the level-1 range. */
    if (range) *range = spec->range;
}

int core_upgrade_cost_for_level(int level)
{
    if (level < 1 || level >= TOWER_MAX_LEVEL) {
        return -1;
    }
    return kUpgradeCostByLevel[level - 1];
}

int game_upgrade_cost(const Game *g, int towerIndex)
{
    if (!g || towerIndex < 0 || towerIndex >= g->towerCount) {
        return -1;
    }
    if (!g->towers[towerIndex].active) {
        return -1;
    }
    return core_upgrade_cost_for_level(g->towers[towerIndex].level);
}

int game_sell_value(const Game *g, int towerIndex)
{
    if (!g || towerIndex < 0 || towerIndex >= g->towerCount) {
        return 0;
    }
    if (!g->towers[towerIndex].active) {
        return 0;
    }
    /* main.lua:1449 - floor(getTotalCost(tower) * 0.75). */
    return (g->towers[towerIndex].invested * 3) / 4;
}

int game_selected_upgrade_cost(const Game *g)
{
    return game_upgrade_cost(g, g ? g->selectedTower : -1);
}

int game_selected_sell_value(const Game *g)
{
    if (!g) {
        return 0;
    }
    return game_sell_value(g, g->selectedTower);
}

/* ---- wave composition (main.lua:513-571 startWave) ---- */

int core_wave_base_enemies(int wave)
{
    if (wave < 1) {
        wave = 1;
    }
    return 3 + wave * 2;
}

int core_wave_elite_count(int wave)
{
    if (wave < 1) {
        return 0;
    }
    if (wave % 25 == 0) {
        return 10; /* boss wave */
    }
    if (wave % 10 == 0) {
        return 3;
    }
    if (wave % 5 == 0) {
        return 1;
    }
    return 0;
}

int core_wave_total_spawns(int wave)
{
    /* Wave size is 3 + 2*wave INCLUDING the elites: regulars are
     * (base - eliteCount) and the elite count is part of the wave (Lead's
     * exact-quota rule from v0.2; the preview is exact because of it). */
    return core_wave_base_enemies(wave);
}

float core_wave_spawn_interval(int wave)
{
    if (wave < 1) {
        wave = 1;
    }
    /* main.lua:567 - max(0.8, 3.0 - wave*0.05). */
    float interval = 3.0f - (float)wave * 0.05f;
    return (interval < 0.8f) ? 0.8f : interval;
}

/*
 * v0.3 balance: global enemy HP multiplier (single tuning knob, allowed by the
 * guardrails in elements-spec.md section 4; the damage table and economy are
 * untouched).
 *
 * Why it is not 1.0: with the raw spec formula a mixed AI clears 25/25 waves
 * with 0 leaks AND a pure-GUN army also clears 25/25 with 0 leaks - enemies die
 * in one volley, so the layer/element system never matters. At 2.0x the mixed
 * AI clears 25/25 with 1 leak over the whole run, while the pure-GUN army dies
 * on wave 3 with zero kills: that is the intended "bring the right elements"
 * gameplay. Set back to 1.0f for the literal spec numbers.
 */
static const float kEnemyHpScale = 2.0f;

float core_enemy_hp(int wave, bool elite)
{
    if (wave < 1) {
        wave = 1;
    }
    float hp = (float)(15 + (wave - 1) * 3) * kEnemyHpScale; /* main.lua:671 */
    if (elite) {
        hp *= 2.5f;                                          /* main.lua:675 */
    }
    return hp;
}

float core_enemy_speed(int wave, bool elite)
{
    if (wave < 1) {
        wave = 1;
    }
    float speed = (float)(20 + (wave - 1)); /* main.lua:672 */
    if (elite) {
        speed *= 1.8f;                       /* main.lua:676 */
    }
    return speed;
}

/*
 * Enemy movement speed in cells per second - DELIBERATE DEVIATION from the Lua.
 *
 * The original moved enemies with t += dt*speed/100 where t was normalised over
 * the whole path, so a longer (mazed) path was walked in exactly the same
 * 100/speed seconds: mazing bought the player no extra tower time. That defeats
 * the maze-building mechanic, so the port moves enemies at an absolute speed
 * and a detour really is extra time under fire.
 *
 * kEnemySpeedScale is the single balance knob:
 *   0.25 -> wave 1 = 5 cells/s, i.e. 5 s across the initial 25-cell straight
 *           road (same crossing time the original had), and a 60-cell maze
 *           costs 12 s at wave 1.
 */
static const float kEnemySpeedScale = 0.25f;

float core_enemy_speed_cells(int wave, bool elite)
{
    return core_enemy_speed(wave, elite) * kEnemySpeedScale;
}

float core_enemy_speed_scale(void)
{
    return kEnemySpeedScale;
}

/* ---- layered enemies (elements-spec.md section 1) ---- */

int core_enemy_layer_count(int wave, bool elite)
{
    int layers;

    if (wave < 1) {
        wave = 1;
    }
    if (wave <= 3) {
        layers = 1;
    } else if (wave <= 8) {
        layers = 2;
    } else {
        layers = 3;
    }
    if (elite) {
        ++layers; /* one extra layer, capped at MAX_LAYERS */
    }
    if (layers > MAX_LAYERS) {
        layers = MAX_LAYERS;
    }
    return layers;
}

bool core_spawn_slot_is_elite(int spawnSlot, int eliteCount)
{
    /* Match the original's net effect: slot 0 regular, all elites consecutively
     * in slots 1..eliteCount, then regulars again. */
    return spawnSlot >= 1 && spawnSlot <= eliteCount;
}

static uint32_t layer_hash(uint32_t seed, int wave, int spawnSlot)
{
    uint32_t h = seed;
    h ^= (uint32_t)wave * 0x9E3779B1u;
    h ^= (uint32_t)(spawnSlot + 1) * 0x85EBCA77u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

int core_enemy_layer_types(uint32_t seed, int wave, bool elite, int spawnSlot,
                           uint8_t *out)
{
    /* Every ordered pair of distinct types, and every permutation of the three
     * layer types: picking one with the hash gives "two different types" and
     * "all three in random order" from spec section 1. */
    static const uint8_t pairs[6][2] = {
        { LAYER_HP, LAYER_SHIELD }, { LAYER_SHIELD, LAYER_HP },
        { LAYER_HP, LAYER_ARMOR },  { LAYER_ARMOR, LAYER_HP },
        { LAYER_SHIELD, LAYER_ARMOR }, { LAYER_ARMOR, LAYER_SHIELD }
    };
    static const uint8_t permutations[6][3] = {
        { LAYER_HP, LAYER_SHIELD, LAYER_ARMOR },
        { LAYER_HP, LAYER_ARMOR, LAYER_SHIELD },
        { LAYER_SHIELD, LAYER_HP, LAYER_ARMOR },
        { LAYER_SHIELD, LAYER_ARMOR, LAYER_HP },
        { LAYER_ARMOR, LAYER_HP, LAYER_SHIELD },
        { LAYER_ARMOR, LAYER_SHIELD, LAYER_HP }
    };
    int count = core_enemy_layer_count(wave, elite);
    uint32_t h = layer_hash(seed, wave, spawnSlot);
    int i;

    if (!out) {
        return count;
    }
    if (count == 1) {
        out[0] = (uint8_t)(h % 3u);
    } else if (count == 2) {
        const uint8_t *pair = pairs[h % 6u];
        out[0] = pair[0];
        out[1] = pair[1];
    } else {
        const uint8_t *perm = permutations[h % 6u];
        for (i = 0; i < 3; ++i) {
            out[i] = perm[i];
        }
    }
    return count;
}

/*
 * Layer totals for an entire wave (elements-spec.md section 3 "Incoming
 * preview"). Pure function of (seed, wave), so the preview is exactly what the
 * spawner will produce.
 */
static void wave_layer_totals(uint32_t seed, int wave, int *hpLayers,
                              int *shieldLayers, int *armorLayers, int *elites)
{
    int counts[3] = { 0, 0, 0 };
    int eliteCount;
    int spawns;
    int slot;

    if (wave < 1) {
        wave = 1;
    }
    eliteCount = core_wave_elite_count(wave);
    spawns = core_wave_total_spawns(wave);
    for (slot = 0; slot < spawns; ++slot) {
        uint8_t types[MAX_LAYERS];
        bool elite = core_spawn_slot_is_elite(slot, eliteCount);
        int count = core_enemy_layer_types(seed, wave, elite, slot, types);
        int i;
        for (i = 0; i < count; ++i) {
            ++counts[types[i]];
        }
    }
    if (hpLayers) *hpLayers = counts[LAYER_HP];
    if (shieldLayers) *shieldLayers = counts[LAYER_SHIELD];
    if (armorLayers) *armorLayers = counts[LAYER_ARMOR];
    if (elites) *elites = eliteCount;
}

void game_wave_preview(const Game *g, int *hpLayers, int *shieldLayers,
                       int *armorLayers, int *elites)
{
    int wave = g ? g->wave : 1;
    uint32_t seed = g ? g->rng : 0u;

    if (wave < 1) {
        wave = 1;
    }
    wave_layer_totals(seed, wave, hpLayers, shieldLayers, armorLayers, elites);
}
