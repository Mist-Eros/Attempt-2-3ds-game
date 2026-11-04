/*
 * Elemental Defense - content tables and balance formulas.
 *
 * Every number in this file was taken from the original LOVE2D game
 * (ElementalDefense/game/main.lua); the line references point at the Lua that
 * it came from. Keep the tables data-only: the simulation in game.c reads
 * them through the accessors, it never hard-codes a balance number.
 */
#include <stddef.h>

#include "game.h"
#include "core_internal.h"

/*
 * The original worked in pixels: one grid cell was 28 px, tower range was
 * 60 px. The core works in cells, so range becomes 60/28 (~2.14 cells).
 */
#define TOWER_RANGE_CELLS (60.0f / 28.0f)

/* main.lua:128-156 - all three towers start identical, elements differ in the
 * damage matrix only. Colours are the LÖVE colours, already 0..1. */
static const TowerSpec kTowerSpecs[TOWER_TYPE_COUNT] = {
    {
        "Fire", 20, 3.0f, 0.8f, TOWER_RANGE_CELLS,
        { 0.95f, 0.15f, 0.15f },
        { 0.95f, 0.30f, 0.10f }
    },
    {
        "Water", 20, 3.0f, 0.8f, TOWER_RANGE_CELLS,
        { 0.15f, 0.40f, 0.95f },
        { 0.10f, 0.50f, 0.95f }
    },
    {
        "Forest", 20, 3.0f, 0.8f, TOWER_RANGE_CELLS,
        { 0.15f, 0.90f, 0.15f },
        { 0.10f, 0.90f, 0.30f }
    }
};

/* main.lua:158-162 - enemies use the same elemental colours as the towers. */
static const EnemySpec kEnemySpecs[TOWER_TYPE_COUNT] = {
    { "Fire",   { 0.95f, 0.15f, 0.15f }, { 0.95f, 0.30f, 0.10f } },
    { "Water",  { 0.15f, 0.40f, 0.95f }, { 0.10f, 0.50f, 0.95f } },
    { "Forest", { 0.15f, 0.90f, 0.15f }, { 0.10f, 0.90f, 0.30f } }
};

/* main.lua:164-179 getDamageMultiplier(): rows are tower types, columns are
 * enemy elements. Asymmetric on purpose - fire melts forest but splashes off
 * water, water douses fire but is drunk by forest, forest drains water but
 * burns to fire. */
static const float kDamageMultiplier[TOWER_TYPE_COUNT][TOWER_TYPE_COUNT] = {
    /* vs FIRE  vs WATER  vs FOREST */
    {  1.0f,     0.5f,     2.0f }, /* FIRE tower   */
    {  2.0f,     1.0f,     0.5f }, /* WATER tower  */
    {  0.5f,     2.0f,     1.0f }  /* FOREST tower */
};

/* main.lua:1434 - every upgrade doubles the damage. */
static const float kDamageByLevel[TOWER_MAX_LEVEL] = {
    3.0f, 6.0f, 12.0f, 24.0f, 48.0f, 96.0f, 192.0f, 384.0f, 768.0f, 1536.0f
};

/* main.lua:1435 - fireRate = floor(fireRate*0.9*100)/100, applied once per
 * upgrade. Precomputed so the table cannot drift with float rounding. */
static const float kFireRateByLevel[TOWER_MAX_LEVEL] = {
    0.80f, 0.72f, 0.64f, 0.57f, 0.51f, 0.45f, 0.40f, 0.36f, 0.32f, 0.28f
};

/* main.lua:1430 - cost to go from the level in the index to the next level is
 * 20 * 2^(level-1). The last slot is unused (level 10 is the cap). */
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

const EnemySpec *game_enemy_spec(int element)
{
    if (element < 0 || element >= TOWER_TYPE_COUNT) {
        return NULL;
    }
    return &kEnemySpecs[element];
}

float game_damage_multiplier(int towerType, int enemyElement)
{
    if (towerType < 0) towerType = 0;
    if (towerType >= TOWER_TYPE_COUNT) towerType = TOWER_TYPE_COUNT - 1;
    if (enemyElement < 0) enemyElement = 0;
    if (enemyElement >= TOWER_TYPE_COUNT) enemyElement = TOWER_TYPE_COUNT - 1;
    return kDamageMultiplier[towerType][enemyElement];
}

void game_tower_stats(int type, int level,
                      float *damage, float *fireRate, float *range)
{
    if (type < 0) type = 0;
    if (type >= TOWER_TYPE_COUNT) type = TOWER_TYPE_COUNT - 1;
    if (level < 1) level = 1;
    if (level > TOWER_MAX_LEVEL) level = TOWER_MAX_LEVEL;

    if (damage) *damage = kDamageByLevel[level - 1];
    if (fireRate) *fireRate = kFireRateByLevel[level - 1];
    /* The original never improves range on upgrade; it is the level-1 range. */
    if (range) *range = kTowerSpecs[type].range;
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
    /* main.lua:1449 - floor(getTotalCost(tower) * 0.75). "invested" tracks the
     * same sum (20 for the tower plus each upgrade's cost). */
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
     * exact-quota rule). The Lua instead added elites on top of baseEnemies
     * while computing the HUD quota from baseEnemies - eliteCount, so its HUD
     * undercounted the wave by eliteCount (design.md 6.6 copies that
     * off-by-one; the preview here is exact instead). */
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

float core_enemy_hp(int wave, bool elite)
{
    if (wave < 1) {
        wave = 1;
    }
    float hp = (float)(15 + (wave - 1) * 3); /* main.lua:671 */
    if (elite) {
        hp *= 2.5f;                          /* main.lua:675 */
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
 * 100/speed seconds: mazing bought the player literally no extra tower time.
 * That defeats the maze-building mechanic, so the port moves enemies at an
 * absolute speed and a detour really is extra time under fire.
 *
 * kEnemySpeedScale is the single balance knob:
 *   0.25 -> wave 1 = 5 cells/s, i.e. 5 s across the initial 25-cell straight
 *           road (same crossing time the original had), and a 60-cell maze
 *           costs 12 s at wave 1. Tune this and nothing else.
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

int core_wave_elite_delays(int wave, float *out, int max)
{
    int count = 0;
    int i;

    if (!out || max <= 0 || wave < 1) {
        return 0;
    }

    /* main.lua:534-556: the delay list is a little quirky. %25 blocks the %10
     * and %5 branches (elseif chain), so a wave is never two kinds of elite
     * wave at once. */
    if (wave % 25 == 0) {
        for (i = 1; i <= 10 && count < max; ++i) {
            out[count++] = (float)i * 0.3f;
        }
    } else if (wave % 10 == 0) {
        for (i = 1; i <= 3 && count < max; ++i) {
            out[count++] = (float)i * 0.2f;
        }
    } else if (wave % 5 == 0) {
        out[count++] = 0.5f;
    }
    return count;
}

void game_wave_preview(const Game *g, int *fire, int *water, int *forest, int *elite)
{
    int wave = g ? g->wave : 1;
    int base;
    int elites;
    int regular;
    int fireCount;
    int waterCount;

    if (wave < 1) {
        wave = 1;
    }

    base = core_wave_base_enemies(wave);
    elites = core_wave_elite_count(wave);
    regular = base - elites;
    if (regular < 0) {
        regular = 0;
    }

    /* main.lua:558-565. Note: these are the *planned* counts the original UI
     * displayed. Actual spawns roll the element from the RNG (see NOTES.md). */
    fireCount = regular / 3;
    waterCount = regular / 3;

    if (fire) *fire = fireCount;
    if (water) *water = waterCount;
    if (forest) *forest = regular - fireCount - waterCount;
    if (elite) *elite = elites;
}
