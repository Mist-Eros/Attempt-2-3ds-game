/*
 * Host unit tests for the portable Elemental Defense core (v0.7: 1000 waves, no tower cap).
 *
 * Plain C99, no framework: every test prints PASS/FAIL and the process exits
 * non-zero if anything failed.
 *
 * Build/run:  make -C tests            (add CC=... for a non-default compiler)
 * or:         make -C tests test
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "core_internal.h" /* wave-scaling accessors; host tests only */

/* ------------------------------------------------------------------ */
/* tiny test harness                                                  */
/* ------------------------------------------------------------------ */

static int g_failures = 0;
static int g_tests = 0;
static int g_tests_failed = 0;
static int g_current_test_failed = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("      check failed: %s (%s:%d)\n", #cond, __FILE__,      \
                   __LINE__);                                                \
            ++g_failures;                                                    \
            ++g_current_test_failed;                                         \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(actual, expected, eps)                                    \
    do {                                                                     \
        float a_ = (float)(actual);                                          \
        float e_ = (float)(expected);                                        \
        if (fabsf(a_ - e_) > (float)(eps)) {                                 \
            printf("      check failed: %s ~= %s (got %.6f want %.6f, "      \
                   "%.6f) (%s:%d)\n",                                        \
                   #actual, #expected, (double)a_, (double)e_,              \
                   (double)(a_ - e_), __FILE__, __LINE__);                   \
            ++g_failures;                                                    \
            ++g_current_test_failed;                                         \
        }                                                                    \
    } while (0)

#define RUN_TEST(fn)                                                         \
    do {                                                                     \
        g_current_test_failed = 0;                                           \
        ++g_tests;                                                           \
        fn();                                                                \
        if (g_current_test_failed == 0) {                                    \
            printf("PASS  %s\n", #fn);                                       \
        } else {                                                             \
            printf("FAIL  %s\n", #fn);                                       \
            ++g_tests_failed;                                                \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static void begin(Game *g, uint32_t seed)
{
    game_new_run(g, seed);
}

static void run_steps(Game *g, int steps)
{
    int i;
    for (i = 0; i < steps; ++i) {
        game_step(g, 1.0f / 60.0f);
    }
}

static bool text_contains(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

static uint32_t hash_game(const Game *g)
{
    const unsigned char *bytes = (const unsigned char *)g;
    uint32_t hash = 2166136261u;
    size_t i;

    for (i = 0; i < sizeof *g; ++i) {
        hash ^= (uint32_t)bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static float total_layer_hp(const Enemy *e)
{
    float total = 0.0f;
    int i;
    for (i = 0; i < e->layerCount; ++i) {
        total += e->layers[i].hp;
    }
    return total;
}

static float total_layer_max_hp(const Enemy *e)
{
    float total = 0.0f;
    int i;
    for (i = 0; i < e->layerCount; ++i) {
        total += e->layers[i].maxHp;
    }
    return total;
}

/* Put a fully specified layered enemy into a slot. */
static void put_layered_enemy(Game *g, int slot, float pathPos, int layerCount,
                              const int *types, const float *hps)
{
    Enemy *e = &g->enemies[slot];
    int i;
    int index;
    float frac;
    Vec2 a;
    Vec2 b;

    memset(e, 0, sizeof *e);
    e->active = true;
    e->layerCount = (uint8_t)layerCount;
    for (i = 0; i < layerCount; ++i) {
        e->layers[i].type = (uint8_t)types[i];
        e->layers[i].hp = hps[i];
        e->layers[i].maxHp = hps[i];
    }
    e->pathPos = pathPos;

    /* Seed x/y from the road so stun tests (which never move) are in range. */
    if (g->path.count < 2) {
        e->x = 0.0f;
        e->y = 0.0f;
        return;
    }
    index = (int)pathPos;
    if (index >= g->path.count - 1) {
        index = g->path.count - 2;
    }
    if (index < 0) {
        index = 0;
    }
    frac = pathPos - (float)index;
    a = g->path.pts[index];
    b = g->path.pts[index + 1];
    e->x = a.x + (b.x - a.x) * frac;
    e->y = a.y + (b.y - a.y) * frac;
}

static void put_simple_enemy(Game *g, int slot, float pathPos, int layerType,
                             float hp)
{
    int types[1];
    float hps[1];

    types[0] = layerType;
    hps[0] = hp;
    put_layered_enemy(g, slot, pathPos, 1, types, hps);
}

/* One level-1 tower of `damageType` beside the road at (0,11), wave 1 started,
 * no queued spawns, no enemies, tower ready to fire. */
static void setup_single_tower(Game *g, int damageType)
{
    game_new_run(g, 7u);
    game_set_selected_type(g, damageType);
    game_place_tower(g, 0, 11);
    game_start_next_wave(g);
    g->enemiesToSpawn = 0;
    g->elitePending = 0;
    g->eliteDelayCount = 0;
    g->enemiesAlive = 0;
    g->towers[0].cooldown = 0.0f;
}

/* Is (col,row) one of the cells of the current road? */
static bool path_has_cell(const Game *g, int col, int row)
{
    int i;
    for (i = 0; i < g->path.count; ++i) {
        if (g->path.pts[i].x == (float)col + 0.5f &&
            g->path.pts[i].y == (float)row + 0.5f) {
            return true;
        }
    }
    return false;
}

/*
 * Assert the current road is legal: entrance -> exit, 4-connected, no repeated
 * cell, no tower standing on it, and length == count-1.
 */
static void check_path_integrity(const Game *g)
{
    int8_t seen[GRID_ROWS][GRID_COLS];
    int i;

    CHECK(g->path.count >= 2);
    CHECK(g->path.count <= MAX_PATH_CELLS);
    CHECK_NEAR(g->path.length, (float)(g->path.count - 1), 1e-6f);
    CHECK_NEAR(g->path.pts[0].x, 0.5f, 1e-6f);
    CHECK_NEAR(g->path.pts[0].y, 12.5f, 1e-6f);
    CHECK_NEAR(g->path.pts[g->path.count - 1].x, 25.5f, 1e-6f);
    CHECK_NEAR(g->path.pts[g->path.count - 1].y, 12.5f, 1e-6f);

    memset(seen, 0, sizeof seen);
    for (i = 0; i < g->path.count; ++i) {
        int col = (int)(g->path.pts[i].x - 0.5f);
        int row = (int)(g->path.pts[i].y - 0.5f);

        CHECK(col >= 0 && col < GRID_COLS);
        CHECK(row >= 0 && row < GRID_ROWS);
        if (col < 0 || col >= GRID_COLS || row < 0 || row >= GRID_ROWS) {
            continue;
        }
        CHECK(g->path.pts[i].x == (float)col + 0.5f);
        CHECK(g->path.pts[i].y == (float)row + 0.5f);
        CHECK(seen[row][col] == 0);
        seen[row][col] = 1;
        CHECK(g->occupancy[row][col] == 0);
        if (i > 0) {
            int dx = (int)(g->path.pts[i].x - g->path.pts[i - 1].x);
            int dy = (int)(g->path.pts[i].y - g->path.pts[i - 1].y);
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            CHECK(dx + dy == 1);
        }
    }
}

/* Count spawned/active enemies and their layers (used by composition tests). */
typedef struct {
    int enemies;
    int layerCounts[MAX_LAYERS + 1];
    int layerTypeTotals[3];
    int elites;
} Composition;

static Composition count_composition(const Game *g)
{
    Composition c;
    int i;

    memset(&c, 0, sizeof c);
    for (i = 0; i < MAX_ENEMIES; ++i) {
        const Enemy *e = &g->enemies[i];
        int l;
        if (!e->active) {
            continue;
        }
        ++c.enemies;
        if (e->layerCount <= MAX_LAYERS) {
            ++c.layerCounts[e->layerCount];
        }
        if (e->elite) {
            ++c.elites;
        }
        for (l = 0; l < e->layerCount; ++l) {
            ++c.layerTypeTotals[e->layers[l].type];
        }
    }
    return c;
}

/* Spawn the whole wave quickly and count every active enemy's layers. */
static void spawn_whole_wave(Game *g, Composition *out)
{
    int guard;

    game_start_next_wave(g);
    g->spawnInterval = 0.02f; /* white box: outrun the first enemy */
    for (guard = 0; guard < 20000 && g->enemiesToSpawn > 0; ++guard) {
        game_step(g, 1.0f / 60.0f);
    }
    CHECK(g->enemiesToSpawn == 0);
    *out = count_composition(g);
}

/* ------------------------------------------------------------------ */
/* content tables / stats                                             */
/* ------------------------------------------------------------------ */

static void test_tower_specs(void)
{
    static const char *expectedNames[TOWER_TYPE_COUNT] = {
        "GUN", "FIRE", "SHOCK", "CORROSIVE", "SLAG", "BLASTER"
    };
    static const int expectedCost[TOWER_TYPE_COUNT] = { 20, 20, 20, 20, 15, 30 };
    static const float expectedDamage[TOWER_TYPE_COUNT] = {
        3.0f, 3.0f, 3.0f, 3.0f, 1.0f, 9.0f
    };
    static const float expectedCooldown[TOWER_TYPE_COUNT] = {
        0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 2.4f
    };
    int type;

    for (type = 0; type < TOWER_TYPE_COUNT; ++type) {
        const TowerSpec *spec = game_tower_spec(type);
        CHECK(spec != NULL);
        if (!spec) {
            continue;
        }
        CHECK(strcmp(spec->name, expectedNames[type]) == 0);
        CHECK(spec->cost == expectedCost[type]);
        CHECK_NEAR(spec->damage, expectedDamage[type], 1e-6f);
        CHECK_NEAR(spec->fireRate, expectedCooldown[type], 1e-6f);
        CHECK_NEAR(spec->range, 60.0f / 28.0f, 1e-5f);
        CHECK(spec->damageType == type);
        CHECK(spec->color[0] >= 0.0f && spec->color[0] <= 1.0f);
        if (type == DMG_EXPLOSIVE) {
            CHECK_NEAR(spec->aoeRadius, 1.3f, 1e-6f);
        } else {
            CHECK_NEAR(spec->aoeRadius, 0.0f, 1e-6f);
        }
    }
    CHECK(game_tower_spec(-1) == NULL);
    CHECK(game_tower_spec(TOWER_TYPE_COUNT) == NULL);
}

static void test_tower_stats_per_level(void)
{
    static const float cooldownTable[TOWER_MAX_LEVEL] = {
        0.80f, 0.72f, 0.64f, 0.57f, 0.51f, 0.45f, 0.40f, 0.36f, 0.32f, 0.28f
    };
    int level;

    for (level = 1; level <= TOWER_MAX_LEVEL; ++level) {
        float damage = 0.0f;
        float fireRate = 0.0f;
        float range = 0.0f;

        /* GUN: 3 damage, base cooldown table. */
        game_tower_stats(DMG_NORMAL, level, &damage, &fireRate, &range);
        CHECK_NEAR(damage, 3.0f * (float)(1 << (level - 1)), 1e-4f);
        CHECK_NEAR(fireRate, cooldownTable[level - 1], 1e-5f);
        CHECK_NEAR(range, 60.0f / 28.0f, 1e-5f);

        /* SLAG: 1 base damage, same cooldown curve. */
        game_tower_stats(DMG_SLAG, level, &damage, &fireRate, NULL);
        CHECK_NEAR(damage, 1.0f * (float)(1 << (level - 1)), 1e-4f);
        CHECK_NEAR(fireRate, cooldownTable[level - 1], 1e-5f);

        /* BLASTER: 9 base damage, 3x the cooldown table. */
        game_tower_stats(DMG_EXPLOSIVE, level, &damage, &fireRate, NULL);
        CHECK_NEAR(damage, 9.0f * (float)(1 << (level - 1)), 1e-4f);
        CHECK_NEAR(fireRate, cooldownTable[level - 1] * 3.0f, 1e-5f);
    }

    /* Level is clamped to 1..TOWER_MAX_LEVEL. */
    {
        float damage = 0.0f;
        game_tower_stats(DMG_NORMAL, 0, &damage, NULL, NULL);
        CHECK_NEAR(damage, 3.0f, 1e-6f);
        game_tower_stats(DMG_NORMAL, 99, &damage, NULL, NULL);
        CHECK_NEAR(damage, 1536.0f, 1e-5f);
    }
}

static void test_damage_multiplier_matrix(void)
{
    /* rows: DamageType, columns: HP / SHIELD / ARMOR */
    static const float expected[TOWER_TYPE_COUNT][3] = {
        { 1.0f, 1.0f, 1.0f }, /* normal    */
        { 2.0f, 0.5f, 0.5f }, /* fire      */
        { 0.5f, 2.0f, 0.5f }, /* electric  */
        { 0.5f, 0.5f, 2.0f }, /* corrosive */
        { 1.0f, 1.0f, 1.0f }, /* slag      */
        { 1.0f, 1.0f, 1.0f }  /* explosive */
    };
    int damageType;
    int layerType;

    for (damageType = 0; damageType < TOWER_TYPE_COUNT; ++damageType) {
        for (layerType = 0; layerType < 3; ++layerType) {
            CHECK_NEAR(game_damage_multiplier(damageType, layerType),
                       expected[damageType][layerType], 1e-6f);
        }
    }
    CHECK_NEAR(game_damage_multiplier(-1, 0), 1.0f, 1e-6f);
    CHECK_NEAR(game_damage_multiplier(99, 99), 1.0f, 1e-6f);
}

static void test_upgrade_cost_curve(void)
{
    static const int expectedCost[9] = {
        20, 40, 80, 160, 320, 640, 1280, 2560, 5120
    };
    Game g;
    int level;

    begin(&g, 3u);
    g.gold = 1000000;
    CHECK(game_place_tower(&g, 0, 0) == true);
    game_select_tower(&g, 0);

    for (level = 1; level < TOWER_MAX_LEVEL; ++level) {
        CHECK(game_upgrade_cost(&g, 0) == expectedCost[level - 1]);
        CHECK(game_upgrade_selected(&g) == true);
        CHECK(g.towers[0].level == level + 1);
        CHECK(g.towers[0].invested == 20 * (1 << level));
    }
    CHECK(g.towers[0].level == TOWER_MAX_LEVEL);
    CHECK(game_upgrade_cost(&g, 0) == -1);

    game_clear_message(&g);
    CHECK(game_upgrade_selected(&g) == false);
    CHECK(text_contains(g.message, "max level"));

    /* Upgrades cost the same curve for the cheap and expensive towers. */
    begin(&g, 4u);
    g.gold = 1000000;
    game_set_selected_type(&g, DMG_SLAG);
    CHECK(game_place_tower(&g, 0, 0) == true);
    CHECK(g.towers[0].invested == 15);
    game_select_tower(&g, 0);
    CHECK(game_upgrade_cost(&g, 0) == 20);
}

static void test_sell_refund(void)
{
    Game g;

    begin(&g, 5u);
    CHECK(game_place_tower(&g, 0, 0) == true); /* GUN, cost 20 */
    CHECK(g.gold == STARTING_GOLD - 20);
    game_select_tower(&g, 0);
    CHECK(game_sell_value(&g, 0) == 15); /* floor(20 * 0.75) */
    CHECK(game_sell_selected(&g) == true);
    CHECK(g.gold == STARTING_GOLD - 20 + 15);
    CHECK(g.towerCount == 0);

    /* BLASTER costs 30 -> refund floor(22.5) = 22. */
    begin(&g, 6u);
    game_set_selected_type(&g, DMG_EXPLOSIVE);
    CHECK(game_place_tower(&g, 0, 0) == true);
    game_select_tower(&g, 0);
    CHECK(game_sell_value(&g, 0) == 22);
    CHECK(game_sell_selected(&g) == true);
    CHECK(g.gold == STARTING_GOLD - 30 + 22);

    /* Upgraded tower: invested grows, refund is 75% of it. */
    begin(&g, 7u);
    CHECK(game_place_tower(&g, 0, 0) == true);
    game_select_tower(&g, 0);
    g.gold = 1000;
    CHECK(game_upgrade_selected(&g) == true); /* invested 40 */
    CHECK(game_sell_value(&g, 0) == 30);
    CHECK(game_sell_selected(&g) == true);
    CHECK(g.towerCount == 0);
    CHECK(game_can_place_at(&g, 0, 0, 0) == true);
}

/* ------------------------------------------------------------------ */
/* placement / economy                                                */
/* ------------------------------------------------------------------ */

static void test_placement_rules(void)
{
    Game g;

    begin(&g, 1u);
    CHECK(g.gold == STARTING_GOLD);
    CHECK(g.phase == PHASE_BUILD);
    CHECK(g.selectedType == DMG_NORMAL);
    CHECK(game_tower_at(&g, 0, 0) == -1);
    CHECK(game_can_place_at(&g, 0, 0, 0) == true);
    CHECK(game_can_place_at(&g, -1, 0, 0) == false);
    CHECK(game_can_place_at(&g, 0, -1, 0) == false);
    CHECK(game_can_place_at(&g, GRID_COLS, 0, 0) == false);
    CHECK(game_can_place_at(&g, 0, GRID_ROWS, 0) == false);
    CHECK(game_can_place_at(&g, 0, 0, -1) == false);
    CHECK(game_can_place_at(&g, 0, 0, TOWER_TYPE_COUNT) == false);
    CHECK(game_can_place_at(&g, 0, 12, 0) == false); /* entrance */
    CHECK(game_can_place_at(&g, 25, 12, 0) == false); /* exit */

    CHECK(game_place_tower(&g, 0, 0) == true);
    CHECK(g.gold == STARTING_GOLD - 20);
    CHECK(g.towerCount == 1);
    CHECK(g.occupancy[0][0] == 1);
    CHECK(g.towers[0].damageType == DMG_NORMAL);
    CHECK(g.towers[0].level == 1);
    CHECK(g.towers[0].invested == 20);
    CHECK(g.selectedTower == 0);

    CHECK(game_place_tower(&g, 0, 0) == false);
    CHECK(text_contains(g.message, "occupied"));

    /* Road cells are buildable (maze mechanic) and reroute the road. */
    game_clear_message(&g);
    game_set_selected_type(&g, DMG_FIRE);
    CHECK(game_place_tower(&g, 5, 12) == true);
    CHECK(g.towerCount == 2);
    CHECK(g.towers[1].damageType == DMG_FIRE);
    CHECK(path_has_cell(&g, 5, 12) == false);
    check_path_integrity(&g);

    /* Entrance / exit are the only road cells that reject. */
    game_clear_message(&g);
    CHECK(game_place_tower(&g, 0, 12) == false);
    CHECK(text_contains(g.message, "entrance"));
    CHECK(game_place_tower(&g, 25, 12) == false);
    CHECK(text_contains(g.message, "exit"));

    game_clear_message(&g);
    CHECK(game_place_tower(&g, -1, 0) == false);
    CHECK(g.message[0] != '\0');
    CHECK(game_place_tower(&g, 0, GRID_ROWS) == false);

    game_select_tower(&g, 0);
    CHECK(g.selectedTower == 0);
    game_select_tower(&g, 7);
    CHECK(g.selectedTower == -1);
}

static void test_placement_affordability(void)
{
    Game g;

    begin(&g, 2u);
    g.gold = 5;
    CHECK(game_can_place_at(&g, 0, 0, 0) == true);
    CHECK(game_place_tower(&g, 0, 0) == false);
    CHECK(text_contains(g.message, "not enough gold"));
    CHECK(g.towerCount == 0);

    /* SLAG costs 15, BLASTER 30: check the per-type price is enforced. */
    g.gold = 20;
    game_set_selected_type(&g, DMG_SLAG);
    CHECK(game_place_tower(&g, 0, 0) == true);
    CHECK(g.gold == 5);
    game_set_selected_type(&g, DMG_EXPLOSIVE);
    CHECK(game_place_tower(&g, 1, 0) == false);
    CHECK(text_contains(g.message, "not enough gold"));
}

static void test_placement_phase_guard(void)
{
    Game g;

    game_init(&g); /* PHASE_MENU */
    CHECK(game_place_tower(&g, 0, 0) == false);
    CHECK(text_contains(g.message, "build"));
    CHECK(game_can_place_at(&g, 0, 0, 0) == false);

    g.phase = PHASE_VICTORY;
    CHECK(game_place_tower(&g, 0, 0) == false);
    CHECK(game_can_place_at(&g, 0, 0, 0) == false);
}

/* ------------------------------------------------------------------ */
/* path and maze                                                      */
/* ------------------------------------------------------------------ */

static void test_initial_path(void)
{
    Game g;
    int i;

    begin(&g, 1234u);

    CHECK(g.pathBlocked == false);
    CHECK(g.path.count == 26);
    CHECK_NEAR(game_path_length(&g), 25.0f, 1e-6f);
    for (i = 0; i < g.path.count; ++i) {
        CHECK_NEAR(g.path.pts[i].x, (float)i + 0.5f, 1e-6f);
        CHECK_NEAR(g.path.pts[i].y, 12.5f, 1e-6f);
    }
    check_path_integrity(&g);
}

static void test_build_on_road_reroutes(void)
{
    Game g;

    begin(&g, 11u);
    CHECK(game_path_length(&g) == 25.0f);
    CHECK(game_can_place_at(&g, 7, 12, 0) == true);
    CHECK(game_place_tower(&g, 7, 12) == true);
    CHECK(g.towerCount == 1);
    CHECK(g.gold == STARTING_GOLD - 20);
    CHECK(game_path_length(&g) == 27.0f);
    CHECK(path_has_cell(&g, 7, 12) == false);
    check_path_integrity(&g);
}

static void test_blocking_placement_refused(void)
{
    Game g;

    begin(&g, 12u);
    CHECK(game_place_tower(&g, 24, 12) == true);
    CHECK(g.gold == STARTING_GOLD - 20);

    CHECK(game_can_place_at(&g, 25, 11, 0) == false);
    game_clear_message(&g);
    CHECK(game_place_tower(&g, 25, 11) == false);
    CHECK(text_contains(g.message, "block"));
    CHECK(g.gold == STARTING_GOLD - 20);
    CHECK(g.towerCount == 1);
    CHECK(g.occupancy[11][25] == 0);
    check_path_integrity(&g);

    game_select_tower(&g, 0);
    CHECK(game_sell_selected(&g) == true);
    CHECK(game_can_place_at(&g, 25, 11, 0) == true);
}

static void test_entrance_exit_rejected(void)
{
    Game g;

    begin(&g, 13u);
    game_clear_message(&g);
    CHECK(game_place_tower(&g, 0, 12) == false);
    CHECK(text_contains(g.message, "entrance"));
    game_clear_message(&g);
    CHECK(game_place_tower(&g, 25, 12) == false);
    CHECK(text_contains(g.message, "exit"));
    CHECK(g.towerCount == 0);
    CHECK(g.gold == STARTING_GOLD);
    check_path_integrity(&g);
}

static void test_sell_shortens_path(void)
{
    Game g;

    begin(&g, 14u);
    CHECK(game_place_tower(&g, 7, 12) == true);
    CHECK(game_path_length(&g) == 27.0f);
    game_select_tower(&g, 0);
    CHECK(game_sell_selected(&g) == true);
    CHECK(game_path_length(&g) == 25.0f);
    CHECK(path_has_cell(&g, 7, 12) == true);
    check_path_integrity(&g);
}

static void test_maze_long_route(void)
{
    Game g;
    int row;

    begin(&g, 15u);
    g.gold = 1000000;
    for (row = 12; row >= 1; --row) {
        CHECK(game_place_tower(&g, 4, row) == true);
    }
    for (row = 0; row <= 11; ++row) {
        CHECK(game_place_tower(&g, 8, row) == true);
    }
    for (row = 12; row >= 1; --row) {
        CHECK(game_place_tower(&g, 12, row) == true);
    }
    for (row = 0; row <= 11; ++row) {
        CHECK(game_place_tower(&g, 16, row) == true);
    }
    CHECK(g.towerCount == 48);
    CHECK(g.pathBlocked == false);
    check_path_integrity(&g);
    CHECK(game_path_length(&g) > 60.0f);
}

static void test_speed_independent_of_path_length(void)
{
    Game straight;
    Game maze;
    int row;

    begin(&straight, 16u);
    game_start_next_wave(&straight);
    straight.enemiesToSpawn = 0;
    straight.elitePending = 0;
    straight.eliteDelayCount = 0;
    put_simple_enemy(&straight, 0, 0.0f, LAYER_HP, 100000.0f);
    straight.enemiesAlive = 1;
    run_steps(&straight, 60);
    CHECK_NEAR(straight.enemies[0].pathPos, 5.0f, 0.05f);

    begin(&maze, 17u);
    maze.gold = 1000000;
    for (row = 1; row <= 12; ++row) {
        CHECK(game_place_tower(&maze, 7, row) == true);
    }
    CHECK(game_path_length(&maze) > 25.0f);
    game_start_next_wave(&maze);
    maze.enemiesToSpawn = 0;
    maze.elitePending = 0;
    maze.eliteDelayCount = 0;
    put_simple_enemy(&maze, 0, 0.0f, LAYER_HP, 100000.0f);
    maze.enemiesAlive = 1;
    run_steps(&maze, 60);

    CHECK_NEAR(maze.enemies[0].pathPos, 5.0f, 0.05f);
    CHECK_NEAR(maze.enemies[0].pathPos, straight.enemies[0].pathPos, 0.05f);
    CHECK(maze.path.length > straight.path.length + 20.0f);
}

static void test_enemy_reprojection_on_build(void)
{
    Game g;
    int i;
    int guard;
    int activeBefore;

    begin(&g, 18u);
    game_start_next_wave(&g);
    for (guard = 0; guard < 3000 && g.enemiesAlive < 2; ++guard) {
        game_step(&g, 1.0f / 60.0f);
    }
    CHECK(g.enemiesAlive >= 2);

    activeBefore = 0;
    for (i = 0; i < MAX_ENEMIES; ++i) {
        if (g.enemies[i].active) {
            ++activeBefore;
        }
    }
    CHECK(activeBefore >= 2);

    CHECK(game_place_tower(&g, 18, 12) == true);
    CHECK(game_path_length(&g) > 25.0f);
    check_path_integrity(&g);
    CHECK(g.enemiesAlive == activeBefore);

    for (i = 0; i < MAX_ENEMIES; ++i) {
        Enemy *e = &g.enemies[i];
        if (!e->active) {
            continue;
        }
        CHECK(isfinite(e->x));
        CHECK(isfinite(e->y));
        CHECK(e->pathPos >= 0.0f);
        CHECK(e->pathPos <= g.path.length);
    }

    game_select_tower(&g, 0);
    CHECK(game_sell_selected(&g) == true);
    CHECK(game_path_length(&g) == 25.0f);
    check_path_integrity(&g);
    CHECK(game_place_tower(&g, 12, 12) == true);
    check_path_integrity(&g);

    for (guard = 0; guard < 60000 && g.phase == PHASE_WAVE; ++guard) {
        game_step(&g, 1.0f / 60.0f);
        for (i = 0; i < MAX_ENEMIES; ++i) {
            if (!g.enemies[i].active) {
                continue;
            }
            CHECK(isfinite(g.enemies[i].x));
            CHECK(isfinite(g.enemies[i].y));
            CHECK(g.enemies[i].pathPos <= g.path.length + 0.01f);
        }
    }
    CHECK(g.phase == PHASE_BUILD);
}

static void test_cell_center(void)
{
    Vec2 p = game_cell_center(0, 0);
    CHECK_NEAR(p.x, 0.5f, 1e-6f);
    CHECK_NEAR(p.y, 0.5f, 1e-6f);
    p = game_cell_center(25, 12);
    CHECK_NEAR(p.x, 25.5f, 1e-6f);
    CHECK_NEAR(p.y, 12.5f, 1e-6f);
}

/* ------------------------------------------------------------------ */
/* waves / layers                                                     */
/* ------------------------------------------------------------------ */

static void test_wave_composition(void)
{
    static const int waves[] = { 1, 4, 5, 9, 10, 25 };
    static const int expectedSpawns[] = { 5, 11, 13, 21, 23, 53 };
    static const int expectedElites[] = { 0, 0, 1, 0, 3, 10 };
    static const int expectedLayerTotal[] = { 5, 22, 27, 63, 69, 159 };
    int i;

    for (i = 0; i < 6; ++i) {
        Game g;
        int hp = -1;
        int shield = -1;
        int armor = -1;
        int elites = -1;

        begin(&g, 100u + (uint32_t)i);
        g.wave = waves[i];

        game_wave_preview(&g, &hp, &shield, &armor, &elites);
        CHECK(elites == expectedElites[i]);
        CHECK(hp + shield + armor == expectedLayerTotal[i]);

        /* waves 9/10/25 are all 3-layer: exactly one of each per spawn. */
        if (waves[i] >= 9) {
            CHECK(hp == expectedSpawns[i]);
            CHECK(shield == expectedSpawns[i]);
            CHECK(armor == expectedSpawns[i]);
        }
        /* One-layer waves can only ever total the spawn count. */
        if (waves[i] <= 3) {
            CHECK(hp + shield + armor == expectedSpawns[i]);
        }

        game_start_next_wave(&g);
        CHECK(g.enemiesToSpawn == expectedSpawns[i]);
        CHECK(g.enemiesAlive == 0);
        CHECK(g.layerQuota[LAYER_HP] == hp);
        CHECK(g.layerQuota[LAYER_SHIELD] == shield);
        CHECK(g.layerQuota[LAYER_ARMOR] == armor);
    }

    /* Trying to start a wave twice does nothing. */
    {
        Game g;
        begin(&g, 77u);
        game_start_next_wave(&g);
        g.enemiesToSpawn = 1;
        game_start_next_wave(&g);
        CHECK(g.enemiesToSpawn == 1);
    }
}

static void test_spawn_intervals(void)
{
    static const int waves[] = { 1, 4, 5, 10, 25, 44, 55, 56, 60, 1000 };
    static const float expected[] = {
        2.95f, 2.80f, 2.75f, 2.50f, 1.75f, 0.80f, 0.25f, 0.20f, 0.20f, 0.20f
    };
    int i;

    for (i = 0; i < 10; ++i) {
        Game g;
        begin(&g, 55u + (uint32_t)i);
        g.wave = waves[i];
        game_start_next_wave(&g);
        CHECK_NEAR(g.spawnInterval, expected[i], 1e-5f);
    }
}

/*
 * v0.7 long-run scaling: the wave curve must be bounded, monotonic and finite
 * all the way to wave 1000 (spawn count cap, spawn-interval floor, late HP ramp,
 * speed cap) and the wave-1000 preview must be exact.
 */
static void test_long_run_scaling(void)
{
    int wave;

    CHECK(CORE_TOTAL_WAVES == 1000);
    CHECK(core_wave_total_spawns(1) == 5);
    CHECK(core_wave_total_spawns(38) == 79);
    CHECK(core_wave_total_spawns(39) == 80);
    CHECK(core_wave_total_spawns(1000) == 80);
    CHECK(core_spawn_slot_is_elite(0, core_wave_elite_count(1000)) == false);
    CHECK(core_spawn_slot_is_elite(1, core_wave_elite_count(1000)) == true);
    CHECK(core_wave_elite_count(1000) == 10); /* boss wave (%25) */

    /* Spawn count: non-decreasing, capped, never above the cap. */
    for (wave = 1; wave <= 1000; ++wave) {
        int current = core_wave_total_spawns(wave);
        CHECK(current > 0);
        CHECK(current <= 80);
        if (wave > 1) {
            CHECK(current >= core_wave_total_spawns(wave - 1));
        }
    }

    /* Spawn interval: non-increasing, bounded by [0.20, 2.95]. */
    for (wave = 1; wave <= 1000; ++wave) {
        float interval = core_wave_spawn_interval(wave);
        CHECK(interval >= 0.20f - 1e-6f);
        CHECK(interval <= 3.0f + 1e-6f);
        if (wave > 1) {
            CHECK(interval <= core_wave_spawn_interval(wave - 1) + 1e-6f);
        }
    }
    CHECK_NEAR(core_wave_spawn_interval(1000), 0.20f, 1e-6f);

    /* HP: finite, positive, non-decreasing; elites exactly 2.5x; bounded at the
     * end of the campaign (< 1e6 keeps every float sum sane on the 3DS). */
    for (wave = 1; wave <= 1000; ++wave) {
        float hp = core_enemy_hp(wave, false);
        float eliteHp = core_enemy_hp(wave, true);
        CHECK(isfinite(hp));
        CHECK(hp > 0.0f);
        CHECK_NEAR(eliteHp, hp * 2.5f, 1e-3f);
        if (wave > 1) {
            CHECK(hp >= core_enemy_hp(wave - 1, false) - 1e-4f);
        }
    }
    CHECK_NEAR(core_enemy_hp(1, false), 30.0f, 1e-4f);
    CHECK_NEAR(core_enemy_hp(60, false), 384.0f, 1e-4f); /* early curve */
    CHECK(core_enemy_hp(1000, false) > core_enemy_hp(100, false));
    CHECK(core_enemy_hp(1000, false) < 1000000.0f);

    /* Speed: original wave-1 speed, saturating at 10 cells/s (18 elite). */
    CHECK_NEAR(core_enemy_speed_cells(1, false), 5.0f, 1e-6f);
    CHECK_NEAR(core_enemy_speed_cells(2, false), 5.25f, 1e-6f);
    CHECK_NEAR(core_enemy_speed_cells(21, false), 10.0f, 1e-6f);
    CHECK_NEAR(core_enemy_speed_cells(1000, false), 10.0f, 1e-6f);
    CHECK_NEAR(core_enemy_speed_cells(1000, true), 18.0f, 1e-6f);
    for (wave = 1; wave <= 1000; ++wave) {
        float speed = core_enemy_speed_cells(wave, false);
        CHECK(speed > 0.0f && speed <= 10.0f + 1e-6f);
        CHECK_NEAR(core_enemy_speed_cells(wave, true), speed * 1.8f, 1e-4f);
    }

    /* Layer composition thresholds are unchanged: wave 1000 is 3-layer. */
    CHECK(core_enemy_layer_count(1, false) == 1);
    CHECK(core_enemy_layer_count(1000, false) == 3);
    CHECK(core_enemy_layer_count(1000, true) == 3);

    /* Wave-1000 preview: 80 spawns, 10 elites, all 3 layers -> 240 layers, one
     * per type; layer quota fits the uint8_t staging fields. */
    {
        Game g;
        int hp = -1;
        int shield = -1;
        int armor = -1;
        int elites = -1;

        begin(&g, 4242u);
        g.wave = 1000;
        game_wave_preview(&g, &hp, &shield, &armor, &elites);
        CHECK(elites == 10);
        CHECK(hp == 80);
        CHECK(shield == 80);
        CHECK(armor == 80);
        CHECK(hp + shield + armor == 240);
        CHECK(hp <= 255 && shield <= 255 && armor <= 255);

        game_start_next_wave(&g);
        CHECK(g.enemiesToSpawn == 80);
        CHECK(g.layerQuota[LAYER_HP] == (uint8_t)hp);
        CHECK(g.layerQuota[LAYER_SHIELD] == (uint8_t)shield);
        CHECK(g.layerQuota[LAYER_ARMOR] == (uint8_t)armor);

        /* Verify the spawner against the pure composition function slot by slot
         * (fast spawn: at wave 1000 the fastest elites would leak after ~1.4 s,
         * so compare the first 12 spawns before any can reach the exit). */
        {
            int guard;
            int spawned;
            int expectedHp = 0;
            int expectedSh = 0;
            int expectedAr = 0;
            int expectedElites = 0;
            int slot;
            Composition c;

            g.spawnInterval = 0.02f;
            for (guard = 0; guard < 500 && g.enemiesAlive < 12; ++guard) {
                game_step(&g, 1.0f / 60.0f);
            }
            spawned = 80 - g.enemiesToSpawn;
            CHECK(spawned >= 12);
            CHECK(g.leaks == 0);
            for (slot = 0; slot < spawned; ++slot) {
                uint8_t types[MAX_LAYERS];
                bool elite = core_spawn_slot_is_elite(
                    slot, core_wave_elite_count(1000));
                int count = core_enemy_layer_types(g.rng, 1000, elite, slot,
                                                   types);
                int i;
                if (elite) {
                    ++expectedElites;
                }
                for (i = 0; i < count; ++i) {
                    if (types[i] == LAYER_HP) ++expectedHp;
                    if (types[i] == LAYER_SHIELD) ++expectedSh;
                    if (types[i] == LAYER_ARMOR) ++expectedAr;
                }
            }
            c = count_composition(&g);
            CHECK(c.enemies == spawned);
            CHECK(c.elites == expectedElites);
            CHECK(c.layerTypeTotals[LAYER_HP] == expectedHp);
            CHECK(c.layerTypeTotals[LAYER_SHIELD] == expectedSh);
            CHECK(c.layerTypeTotals[LAYER_ARMOR] == expectedAr);

            /* In-game HP follows the wave-1000 formula for both regulars and
             * the (2.5x) elites. */
            {
                int i;
                for (i = 0; i < MAX_ENEMIES; ++i) {
                    const Enemy *e = &g.enemies[i];
                    float expected;
                    if (!e->active) {
                        continue;
                    }
                    expected = core_enemy_hp(1000, e->elite);
                    CHECK_NEAR(total_layer_max_hp(e), expected, 1e-2f);
                }
            }
        }
    }

    /* A wave-1000 enemy really moves at the capped speed (60 steps = 1 s). */
    {
        Game g;

        begin(&g, 4243u);
        g.wave = 1000;
        game_start_next_wave(&g);
        g.enemiesToSpawn = 0;
        g.elitePending = 0;
        g.eliteDelayCount = 0;
        put_simple_enemy(&g, 0, 0.0f, LAYER_HP, 100000.0f);
        g.enemiesAlive = 1;
        run_steps(&g, 60);
        CHECK_NEAR(g.enemies[0].pathPos, 10.0f, 0.05f);
    }
}

/*
 * No tower cap: the 26x13 board itself is the limit, and occupancy indices
 * above 127 (the old int8_t overflow) must work across build / read / sell.
 */
static void test_no_tower_cap(void)
{
    Game g;
    int row;
    int col;
    int built = 0;

    begin(&g, 606u);
    g.gold = 1000000;

    /* Fill every cell above the road (rows 0..11 = 26*12 = 312 towers). The
     * straight row-12 road is untouched, so every one of these is legal. */
    for (row = 0; row < 12; ++row) {
        for (col = 0; col < GRID_COLS; ++col) {
            bool placed = game_place_tower(&g, col, row);
            CHECK(placed);
            if (placed) {
                ++built;
            }
            CHECK(!text_contains(g.message, "tower limit"));
        }
    }
    CHECK(built == 312);
    CHECK(g.towerCount == 312);
    CHECK(g.towerCount > MAX_TOWERS / 4); /* well past the old 96 cap */
    check_path_integrity(&g);
    CHECK_NEAR(game_path_length(&g), 25.0f, 1e-6f);

    /* Occupancy index 313 (old int8_t would wrap to 57) reads back correctly. */
    CHECK(g.occupancy[11][25] == 312);
    CHECK(game_tower_at(&g, 25, 11) == 311);
    CHECK(game_tower_at(&g, 0, 7) == 7 * GRID_COLS); /* row-major order */
    CHECK(g.towers[311].col == 25 && g.towers[311].row == 11);

    /* The board is full above the road: the only placements left are road
     * cells, which must be refused by the maze rule (not by a tower cap). */
    game_clear_message(&g);
    CHECK(game_place_tower(&g, 5, 12) == false);
    CHECK(text_contains(g.message, "block"));

    /* Selling a high-index tower compacts the array; the rebuilt occupancy
     * (int16_t) still points at the right towers, including indices > 127. */
    CHECK(g.towers[200].col == 18 && g.towers[200].row == 7);
    game_select_tower(&g, 200);
    CHECK(game_sell_selected(&g) == true);
    CHECK(g.towerCount == 311);
    CHECK(g.occupancy[7][18] == 0); /* tower 200 was (18,7), now empty */
    CHECK(game_tower_at(&g, 19, 7) == 200); /* old tower 201 moved down */
    CHECK(g.occupancy[7][19] == 201);
    check_path_integrity(&g);

    /* ...and the freed cell can be built on again (no hidden cap). */
    CHECK(game_place_tower(&g, 18, 7) == true);
    CHECK(g.towerCount == 312);
    CHECK(game_tower_at(&g, 18, 7) == 311);
}

static void test_spawn_layer_counts_by_wave(void)
{
    static const int waves[] = { 1, 3, 4, 8, 9 };
    static const int expectedLayers[] = { 1, 1, 2, 2, 3 };
    int i;

    for (i = 0; i < 5; ++i) {
        Game g;
        Composition c;

        begin(&g, 200u + (uint32_t)i);
        g.wave = waves[i];
        spawn_whole_wave(&g, &c);
        CHECK(c.enemies == 3 + 2 * waves[i]);
        CHECK(c.layerCounts[expectedLayers[i]] == c.enemies);
        CHECK(c.elites == 0);

        /* Distinctness rules: 2 layers are two different types, 3 layers are
         * all three types. */
        {
            int j;
            for (j = 0; j < MAX_ENEMIES; ++j) {
                const Enemy *e = &g.enemies[j];
                if (!e->active) {
                    continue;
                }
                if (e->layerCount == 2) {
                    CHECK(e->layers[0].type != e->layers[1].type);
                } else if (e->layerCount == 3) {
                    CHECK(e->layers[0].type != e->layers[1].type);
                    CHECK(e->layers[0].type != e->layers[2].type);
                    CHECK(e->layers[1].type != e->layers[2].type);
                }
            }
        }
    }

    /* Elites: one extra layer, capped at 3. Wave 5: 12 two-layer regulars and
     * one three-layer elite. Wave 10: everything is three layers. */
    {
        Game g;
        Composition c;
        int j;

        begin(&g, 210u);
        g.wave = 5;
        spawn_whole_wave(&g, &c);
        CHECK(c.enemies == 13);
        CHECK(c.elites == 1);
        CHECK(c.layerCounts[2] == 12);
        CHECK(c.layerCounts[3] == 1);
        for (j = 0; j < MAX_ENEMIES; ++j) {
            const Enemy *e = &g.enemies[j];
            if (!e->active) {
                continue;
            }
            if (e->elite) {
                CHECK(e->layerCount == 3);
                CHECK_NEAR(total_layer_max_hp(e), 135.0f, 1e-3f);
            } else {
                CHECK(e->layerCount == 2);
                CHECK_NEAR(total_layer_max_hp(e), 54.0f, 1e-3f);
            }
        }
    }
    {
        Game g;
        Composition c;
        begin(&g, 211u);
        g.wave = 10;
        spawn_whole_wave(&g, &c);
        CHECK(c.enemies == 23);
        CHECK(c.elites == 3);
        CHECK(c.layerCounts[3] == 23); /* elites capped at 3 too */
    }
}

/* The preview must be exactly what spawns (same numbers, same types). */
static void test_layer_composition_truthful(void)
{
    static const int waves[] = { 1, 4, 5, 9, 10, 25 };
    int i;

    for (i = 0; i < 6; ++i) {
        Game g;
        int hp = 0;
        int shield = 0;
        int armor = 0;
        int elites = 0;
        Composition c;

        begin(&g, 300u + (uint32_t)i);
        g.wave = waves[i];
        game_wave_preview(&g, &hp, &shield, &armor, &elites);
        spawn_whole_wave(&g, &c);
        CHECK(c.layerTypeTotals[LAYER_HP] == hp);
        CHECK(c.layerTypeTotals[LAYER_SHIELD] == shield);
        CHECK(c.layerTypeTotals[LAYER_ARMOR] == armor);
        CHECK(c.elites == elites);
    }

    /* Composition for one seed is deterministic... */
    {
        Game a;
        Game b;
        begin(&a, 4242u);
        begin(&b, 4242u);
        a.wave = 9;
        b.wave = 9;
        {
            int ahp, ash, aar, ael, bhp, bsh, bar, bel;
            game_wave_preview(&a, &ahp, &ash, &aar, &ael);
            game_wave_preview(&b, &bhp, &bsh, &bar, &bel);
            CHECK(ahp == bhp && ash == bsh && aar == bar && ael == bel);
        }
    }
    /* ...and different seeds shuffle the composition of 1-layer waves. */
    {
        Game a;
        Game b;
        Composition ca;
        Composition cb;

        begin(&a, 1u);
        begin(&b, 999u);
        a.wave = 1;
        b.wave = 1;
        spawn_whole_wave(&a, &ca);
        spawn_whole_wave(&b, &cb);
        CHECK(ca.layerTypeTotals[LAYER_HP] != cb.layerTypeTotals[LAYER_HP] ||
              ca.layerTypeTotals[LAYER_SHIELD] != cb.layerTypeTotals[LAYER_SHIELD] ||
              ca.layerTypeTotals[LAYER_ARMOR] != cb.layerTypeTotals[LAYER_ARMOR]);
    }
}

/* ------------------------------------------------------------------ */
/* combat: layers                                                     */
/* ------------------------------------------------------------------ */

static void test_damage_application_all_types(void)
{
    int type;

    for (type = 0; type < TOWER_TYPE_COUNT; ++type) {
        Game g;
        Enemy *e;
        float baseDamage = (type == DMG_SLAG) ? 1.0f
                          : (type == DMG_EXPLOSIVE) ? 9.0f
                          : 3.0f;
        float expectedDamage = baseDamage *
                               game_damage_multiplier(type, LAYER_HP);

        setup_single_tower(&g, type);
        put_simple_enemy(&g, 0, 0.0f, LAYER_HP, 100.0f);
        g.enemiesAlive = 1;
        game_step(&g, 1.0f / 60.0f);

        e = &g.enemies[0];
        CHECK(e->active);
        CHECK(e->layerCount == 1);
        CHECK_NEAR(e->layers[0].hp, 100.0f - expectedDamage, 1e-4f);
        CHECK(e->hitFlash > 0.0f);
    }
}

static void test_layer_depletion_and_carry(void)
{
    Game g;
    Enemy *e;

    /* GUN (1.0x everywhere): outer HP 5, inner ARMOR 100. */
    {
        int types[2] = { LAYER_HP, LAYER_ARMOR };
        float hps[2] = { 5.0f, 100.0f };

        setup_single_tower(&g, DMG_NORMAL);
        put_layered_enemy(&g, 0, 0.0f, 2, types, hps);
        g.enemiesAlive = 1;

        game_step(&g, 1.0f / 60.0f); /* 3 damage: outer has 2 left */
        e = &g.enemies[0];
        CHECK(e->layerCount == 2);
        CHECK_NEAR(e->layers[0].hp, 2.0f, 1e-4f);

        g.towers[0].cooldown = 0.0f; /* fire again immediately */
        game_step(&g, 1.0f / 60.0f); /* 3 damage: 2 breaks HP, 1 carries */
        CHECK(e->layerCount == 1);
        CHECK(e->layers[0].type == LAYER_ARMOR);
        CHECK_NEAR(e->layers[0].hp, 99.0f, 1e-4f);
    }

    /* FIRE (2.0x vs HP, 0.5x vs SHIELD): carry-over must re-apply the multiplier
     * of the next layer. 3 raw against a 3 hp HP layer eats 1.5 raw, the other
     * 1.5 carries and does 1.5*0.5 = 0.75 to the shield. */
    {
        int types[2] = { LAYER_HP, LAYER_SHIELD };
        float hps[2] = { 3.0f, 100.0f };

        setup_single_tower(&g, DMG_FIRE);
        put_layered_enemy(&g, 0, 0.0f, 2, types, hps);
        g.enemiesAlive = 1;

        game_step(&g, 1.0f / 60.0f);
        e = &g.enemies[0];
        CHECK(e->layerCount == 1);
        CHECK(e->layers[0].type == LAYER_SHIELD);
        CHECK_NEAR(e->layers[0].hp, 99.25f, 1e-4f);
        CHECK(e->burnStacks == 1); /* on-hit effect applied to the survivor */
    }

    /* Slag amplifier carries too: 3 raw, 3 hp outer, slagged => 1.5x. */
    {
        int types[1] = { LAYER_HP };
        float hps[1] = { 3.0f };

        setup_single_tower(&g, DMG_NORMAL);
        put_layered_enemy(&g, 0, 0.0f, 1, types, hps);
        g.enemies[0].slagTimer = 4.0f;
        g.enemiesAlive = 1;

        game_step(&g, 1.0f / 60.0f);
        CHECK(g.kills == 1);
        CHECK(g.gold == STARTING_GOLD - 20 + 10);
    }
}

static void test_layer_death_rewards(void)
{
    Game g;

    setup_single_tower(&g, DMG_NORMAL);
    put_simple_enemy(&g, 0, 0.0f, LAYER_HP, 2.0f);
    g.enemiesAlive = 1;
    g.events = 0;
    game_step(&g, 1.0f / 60.0f);

    CHECK(g.kills == 1);
    CHECK(g.enemiesAlive == 0);
    CHECK(g.gold == STARTING_GOLD - 20 + 10);
    CHECK((g.events & GAME_EVT_ENEMY_DIE) != 0u);
}

/* ------------------------------------------------------------------ */
/* combat: status effects                                             */
/* ------------------------------------------------------------------ */

static void test_burn_stacks_and_ticks(void)
{
    Game g;
    Enemy *e;
    float hpAfterHits;
    int i;

    setup_single_tower(&g, DMG_FIRE);
    put_simple_enemy(&g, 0, 0.0f, LAYER_HP, 10000.0f);
    g.enemiesAlive = 1;

    /* Force a hit every step to stack the burn. */
    for (i = 0; i < 6; ++i) {
        g.towers[0].cooldown = 0.0f;
        game_step(&g, 1.0f / 60.0f);
    }
    e = &g.enemies[0];
    CHECK(e->burnStacks == 5); /* capped at 5 */
    CHECK_NEAR(e->burnTimer, 2.5f, 1e-5f);

    /* Stop shooting; 5 stacks * 1 dps * 2 (fire vs HP) = 10 hp/s. */
    g.towers[0].cooldown = 1000.0f;
    hpAfterHits = e->layers[0].hp;
    run_steps(&g, 60);
    CHECK_NEAR(hpAfterHits - e->layers[0].hp, 10.0f, 0.2f);

    /* Burn expires 2.5 s after the last hit and clears the stacks. */
    run_steps(&g, 120);
    CHECK(e->burnStacks == 0);
    CHECK_NEAR(e->burnTimer, 0.0f, 1e-6f);
}

static void test_stun_stops_movement(void)
{
    Game g;
    float frozenPos;

    setup_single_tower(&g, DMG_ELECTRIC);
    put_simple_enemy(&g, 0, 0.0f, LAYER_HP, 10000.0f);
    g.enemiesAlive = 1;

    game_step(&g, 1.0f / 60.0f); /* tower fires, stun applied */
    CHECK(g.enemies[0].stunTimer > 0.0f);
    CHECK_NEAR(g.enemies[0].stunTimer, 0.3f, 0.02f);
    frozenPos = g.enemies[0].pathPos;

    run_steps(&g, 10); /* still within the 0.3 s stun */
    CHECK_NEAR(g.enemies[0].pathPos, frozenPos, 1e-6f);

    run_steps(&g, 15); /* stun over, movement resumes */
    CHECK(g.enemies[0].pathPos > frozenPos + 0.1f);
}

static void test_puddle_spawn_and_tick(void)
{
    Game g;
    Enemy *e;
    float hpAfterHit;
    int activePuddles = 0;
    int i;

    setup_single_tower(&g, DMG_CORROSIVE);
    put_simple_enemy(&g, 0, 0.0f, LAYER_ARMOR, 100.0f);
    g.enemiesAlive = 1;

    game_step(&g, 1.0f / 60.0f); /* hit + puddle at (0.5,12.5) */
    for (i = 0; i < MAX_PUDDLES; ++i) {
        if (g.puddles[i].active) {
            ++activePuddles;
            CHECK_NEAR(g.puddles[i].radius, 0.7f, 1e-6f);
            CHECK_NEAR(g.puddles[i].dps, 2.0f, 1e-6f);
            CHECK_NEAR(g.puddles[i].life, 5.0f, 0.02f);
        }
    }
    CHECK(activePuddles == 1);

    /* Next step: the puddle ticks 2 dps * 2 (corrosive vs ARMOR); the tower is
     * on cooldown so nothing else hits. */
    e = &g.enemies[0];
    hpAfterHit = e->layers[0].hp;
    game_step(&g, 1.0f / 60.0f);
    CHECK_NEAR(hpAfterHit - e->layers[0].hp, 2.0f * 2.0f / 60.0f, 1e-4f);
}

static void test_puddle_lifetime_and_cap(void)
{
    Game g;
    int i;
    int activePuddles;

    /* Lifetime: freeze the enemy far from the goal so the wave stays in WAVE and
     * let the puddle run out. */
    setup_single_tower(&g, DMG_CORROSIVE);
    put_simple_enemy(&g, 0, 0.0f, LAYER_ARMOR, 100000.0f);
    g.enemies[0].stunTimer = 100.0f;
    g.enemiesAlive = 1;

    game_step(&g, 1.0f / 60.0f);
    CHECK(g.puddles[0].active);
    run_steps(&g, 290); /* ~4.83 s */
    CHECK(g.puddles[0].active);
    run_steps(&g, 20); /* past 5 s */
    CHECK(!g.puddles[0].active);

    /* Cap: fire 20 times at a stunned enemy; only MAX_PUDDLES may be live. */
    begin(&g, 8u);
    game_set_selected_type(&g, DMG_CORROSIVE);
    g.gold = 100000;
    CHECK(game_place_tower(&g, 0, 11) == true);
    game_start_next_wave(&g);
    g.enemiesToSpawn = 0;
    g.elitePending = 0;
    g.eliteDelayCount = 0;
    put_simple_enemy(&g, 0, 0.0f, LAYER_ARMOR, 100000.0f);
    g.enemies[0].stunTimer = 100.0f;
    g.enemiesAlive = 1;
    for (i = 0; i < 20; ++i) {
        g.towers[0].cooldown = 0.0f;
        game_step(&g, 1.0f / 60.0f);
    }
    activePuddles = 0;
    for (i = 0; i < MAX_PUDDLES; ++i) {
        if (g.puddles[i].active) {
            ++activePuddles;
        }
    }
    CHECK(activePuddles == MAX_PUDDLES);
    CHECK(g.puddleNext > 0); /* round-robin replacement happened */
}

static void test_slag_effect(void)
{
    Game g;
    Enemy *e;
    float posBefore;

    begin(&g, 9u);
    game_set_selected_type(&g, DMG_SLAG);
    CHECK(game_place_tower(&g, 0, 11) == true);   /* tower 0: slag */
    game_set_selected_type(&g, DMG_NORMAL);
    CHECK(game_place_tower(&g, 1, 11) == true);   /* tower 1: gun */
    game_start_next_wave(&g);
    g.enemiesToSpawn = 0;
    g.elitePending = 0;
    g.eliteDelayCount = 0;
    put_simple_enemy(&g, 0, 1.0f, LAYER_HP, 10000.0f);
    g.enemiesAlive = 1;
    g.towers[0].cooldown = 0.0f;
    g.towers[1].cooldown = 0.0f;

    /* Slag hits first (tower array order) and the gun in the same step already
     * benefits from the +50% damage taken. */
    game_step(&g, 1.0f / 60.0f);
    e = &g.enemies[0];
    CHECK(e->slagTimer > 0.0f);
    CHECK_NEAR(e->slagTimer, 3.983f, 0.02f);
    /* 10000 - 1 (slag) - 4.5 (gun 3 * 1.0 * 1.5) = 9994.5 */
    CHECK_NEAR(total_layer_hp(e), 9994.5f, 1e-3f);

    /* Movement is slowed to 65% while slagged. */
    g.towers[0].cooldown = 1000.0f;
    g.towers[1].cooldown = 1000.0f;
    posBefore = e->pathPos;
    run_steps(&g, 60);
    CHECK_NEAR(e->pathPos - posBefore, 5.0f * 0.65f, 0.1f);
    CHECK(e->slagTimer > 0.0f);
}

/* ------------------------------------------------------------------ */
/* combat: explosive + targeting + beams                              */
/* ------------------------------------------------------------------ */

static void test_explosive_aoe(void)
{
    Game g;
    int beamKind = -1;
    float beamRadius = 0.0f;
    int i;

    setup_single_tower(&g, DMG_EXPLOSIVE);
    put_simple_enemy(&g, 0, 0.0f, LAYER_HP, 100.0f); /* target */
    put_simple_enemy(&g, 1, 0.4f, LAYER_HP, 100.0f); /* close: hit */
    put_simple_enemy(&g, 2, 4.0f, LAYER_HP, 100.0f); /* far: not hit */
    g.enemiesAlive = 3;

    game_step(&g, 1.0f / 60.0f);

    CHECK_NEAR(g.enemies[0].layers[0].hp, 91.0f, 1e-4f);
    CHECK_NEAR(g.enemies[1].layers[0].hp, 91.0f, 1e-4f);
    CHECK_NEAR(g.enemies[2].layers[0].hp, 100.0f, 1e-4f);
    CHECK_NEAR(g.towers[0].cooldown, 2.4f, 1e-4f); /* 3x slower */

    for (i = 0; i < MAX_BEAMS; ++i) {
        if (g.beams[i].active && g.beams[i].kind == 1) {
            beamKind = (int)g.beams[i].kind;
            beamRadius = g.beams[i].radius;
        }
    }
    CHECK(beamKind == 1);
    CHECK_NEAR(beamRadius, 1.3f, 1e-6f);
}

static void test_targeting_preferences(void)
{
    Game g;

    /* FIRE prefers an HP outer layer even if the SHIELD enemy is closer. */
    setup_single_tower(&g, DMG_FIRE);
    put_simple_enemy(&g, 0, 0.0f, LAYER_SHIELD, 100.0f);
    put_simple_enemy(&g, 1, 1.0f, LAYER_HP, 100.0f);
    g.enemiesAlive = 2;
    game_step(&g, 1.0f / 60.0f);
    CHECK_NEAR(g.enemies[0].layers[0].hp, 100.0f, 1e-4f);
    CHECK_NEAR(g.enemies[1].layers[0].hp, 100.0f - 3.0f * 2.0f, 1e-4f);

    /* SHOCK prefers SHIELD. */
    setup_single_tower(&g, DMG_ELECTRIC);
    put_simple_enemy(&g, 0, 0.0f, LAYER_HP, 100.0f);
    put_simple_enemy(&g, 1, 1.0f, LAYER_SHIELD, 100.0f);
    g.enemiesAlive = 2;
    game_step(&g, 1.0f / 60.0f);
    CHECK_NEAR(g.enemies[0].layers[0].hp, 100.0f, 1e-4f);
    CHECK_NEAR(g.enemies[1].layers[0].hp, 100.0f - 3.0f * 2.0f, 1e-4f);

    /* CORROSIVE prefers ARMOR. */
    setup_single_tower(&g, DMG_CORROSIVE);
    put_simple_enemy(&g, 0, 0.0f, LAYER_HP, 100.0f);
    put_simple_enemy(&g, 1, 1.0f, LAYER_ARMOR, 100.0f);
    g.enemiesAlive = 2;
    game_step(&g, 1.0f / 60.0f);
    CHECK_NEAR(g.enemies[0].layers[0].hp, 100.0f, 1e-4f);
    CHECK_NEAR(g.enemies[1].layers[0].hp, 100.0f - 3.0f * 2.0f, 1e-4f);

    /* GUN has no preference: it hits the nearest. */
    setup_single_tower(&g, DMG_NORMAL);
    put_simple_enemy(&g, 0, 0.0f, LAYER_SHIELD, 100.0f);
    put_simple_enemy(&g, 1, 1.0f, LAYER_HP, 100.0f);
    g.enemiesAlive = 2;
    game_step(&g, 1.0f / 60.0f);
    CHECK_NEAR(g.enemies[0].layers[0].hp, 97.0f, 1e-4f);
    CHECK_NEAR(g.enemies[1].layers[0].hp, 100.0f, 1e-4f);

    /* Fallback: no preferred outer layer in range -> nearest. */
    setup_single_tower(&g, DMG_FIRE);
    put_simple_enemy(&g, 0, 0.0f, LAYER_SHIELD, 100.0f);
    put_simple_enemy(&g, 1, 1.0f, LAYER_ARMOR, 100.0f);
    g.enemiesAlive = 2;
    game_step(&g, 1.0f / 60.0f);
    CHECK_NEAR(g.enemies[0].layers[0].hp, 100.0f - 3.0f * 0.5f, 1e-4f);
    CHECK_NEAR(g.enemies[1].layers[0].hp, 100.0f, 1e-4f);

    /* Ties go to the oldest enemy (lowest index). */
    setup_single_tower(&g, DMG_NORMAL);
    put_simple_enemy(&g, 0, 1.0f, LAYER_HP, 100.0f);
    put_simple_enemy(&g, 1, 1.0f, LAYER_HP, 100.0f);
    g.enemiesAlive = 2;
    game_step(&g, 1.0f / 60.0f);
    CHECK_NEAR(g.enemies[0].layers[0].hp, 97.0f, 1e-4f);
    CHECK_NEAR(g.enemies[1].layers[0].hp, 100.0f, 1e-4f);
}

static void test_normal_beam_visual(void)
{
    Game g;
    int found = 0;
    int i;

    setup_single_tower(&g, DMG_FIRE);
    put_simple_enemy(&g, 0, 0.0f, LAYER_HP, 100.0f);
    g.enemiesAlive = 1;
    game_step(&g, 1.0f / 60.0f);

    for (i = 0; i < MAX_BEAMS; ++i) {
        if (!g.beams[i].active) {
            continue;
        }
        ++found;
        CHECK(g.beams[i].kind == 0);
        CHECK(g.beams[i].damageType == DMG_FIRE);
        CHECK_NEAR(g.beams[i].life, 0.15f, 1e-5f);
        CHECK_NEAR(g.beams[i].radius, 0.0f, 1e-6f);
        CHECK_NEAR(g.beams[i].x1, 0.5f, 1e-6f); /* tower centre (0,11) */
        CHECK_NEAR(g.beams[i].y1, 11.5f, 1e-6f);
    }
    CHECK(found >= 1);
}

/* ------------------------------------------------------------------ */
/* enemies / waves framework                                          */
/* ------------------------------------------------------------------ */

static void test_enemy_movement_speed(void)
{
    Game g;

    begin(&g, 11u);
    game_start_next_wave(&g);
    g.enemiesToSpawn = 0;
    g.elitePending = 0;
    g.eliteDelayCount = 0;
    put_simple_enemy(&g, 0, 0.0f, LAYER_HP, 1000.0f);
    g.enemiesAlive = 1;

    run_steps(&g, 60);
    CHECK_NEAR(g.enemies[0].pathPos, 5.0f, 0.05f);
    CHECK_NEAR(g.enemies[0].x, 5.5f, 0.05f);
    CHECK_NEAR(g.enemies[0].y, 12.5f, 1e-5f);
}

static void test_leak_costs_life(void)
{
    Game g;

    begin(&g, 12u);
    game_start_next_wave(&g);
    g.enemiesToSpawn = 0;
    g.elitePending = 0;
    g.eliteDelayCount = 0;
    put_simple_enemy(&g, 0, g.path.length - 0.01f, LAYER_HP, 1000.0f);
    g.enemiesAlive = 1;
    g.events = 0;

    game_step(&g, 1.0f / 60.0f);

    CHECK(g.leaks == 1);
    CHECK(g.lives == STARTING_LIVES - 1);
    CHECK(g.gold == STARTING_GOLD);
    CHECK((g.events & GAME_EVT_LEAK) != 0u);
    CHECK(g.enemiesAlive == 0);
    CHECK(g.phase == PHASE_BUILD);
    CHECK(g.wave == 2);
}

static void test_idle_player_loses(void)
{
    Game g;
    int guard;

    begin(&g, 42u);
    g.events = 0;

    for (guard = 0; guard < 200000; ++guard) {
        if (g.phase == PHASE_BUILD) {
            game_start_next_wave(&g);
        }
        if (g.phase == PHASE_GAMEOVER) {
            break;
        }
        game_step(&g, 1.0f / 60.0f);
    }

    CHECK(g.phase == PHASE_GAMEOVER);
    CHECK(g.lives == 0);
    CHECK(g.leaks == STARTING_LIVES);
    CHECK(g.kills == 0);
    CHECK(g.wave == 3);
    CHECK((g.events & GAME_EVT_GAMEOVER) != 0u);
}

static void test_gameover_freezes_simulation(void)
{
    Game g;
    float frozenWaveTime;

    begin(&g, 43u);
    g.lives = 1;
    game_start_next_wave(&g);
    run_steps(&g, 1200);

    CHECK(g.phase == PHASE_GAMEOVER);
    CHECK(g.lives == 0);

    frozenWaveTime = g.waveTime;
    game_step(&g, 5.0f);
    run_steps(&g, 60);
    CHECK_NEAR(g.waveTime, frozenWaveTime, 1e-6f);
    game_start_next_wave(&g);
    CHECK(g.phase == PHASE_GAMEOVER);
}

static void test_victory_after_1000_waves(void)
{
    Game g;
    int guard;
    int expectedLeaks = 0;
    int wave;

    begin(&g, 2024u);
    /* No towers: every enemy leaks. 78,556 spawns over 1000 waves must not
     * kill a run with 100,000 lives, so the only possible ending is VICTORY. */
    g.lives = 100000;
    game_cycle_speed(&g);
    game_cycle_speed(&g);
    CHECK_NEAR(g.speedMultiplier, 4.0f, 1e-6f);

    for (wave = 1; wave <= 1000; ++wave) {
        expectedLeaks += core_wave_total_spawns(wave);
    }
    CHECK(expectedLeaks == 78556);

    for (guard = 0; guard < 4000000; ++guard) {
        if (g.phase == PHASE_BUILD) {
            game_start_next_wave(&g);
        }
        if (g.phase == PHASE_VICTORY || g.phase == PHASE_GAMEOVER) {
            break;
        }
        game_step(&g, 1.0f / 60.0f);
    }

    CHECK(g.phase == PHASE_VICTORY);
    CHECK(g.wave == 1001);
    CHECK(g.totalWaves == 1000);
    CHECK(g.leaks == expectedLeaks);
    CHECK(g.kills == 0);
    CHECK((g.events & GAME_EVT_VICTORY) != 0u);
}

static void run_script(Game *g, uint32_t seed)
{
    begin(g, seed);
    game_place_tower(g, 0, 0);
    game_set_selected_type(g, DMG_CORROSIVE);
    game_place_tower(g, 1, 0);
    game_select_tower(g, 0);
    game_upgrade_selected(g);
    game_start_next_wave(g);
    run_steps(g, 600);
}

static void test_determinism(void)
{
    Game a;
    Game b;
    Game c;

    run_script(&a, 1234u);
    run_script(&b, 1234u);
    run_script(&c, 9999u);

    CHECK(memcmp(&a, &b, sizeof a) == 0);
    CHECK(hash_game(&a) == hash_game(&b));
    CHECK(hash_game(&a) != hash_game(&c));
    CHECK_NEAR(a.waveTime, b.waveTime, 1e-6f);
}

static void test_speed_multiplier(void)
{
    Game normal;
    Game fast;

    begin(&normal, 21u);
    begin(&fast, 21u);
    game_start_next_wave(&normal);
    game_start_next_wave(&fast);
    game_cycle_speed(&fast);
    CHECK_NEAR(fast.speedMultiplier, 2.0f, 1e-6f);

    run_steps(&normal, 300);
    run_steps(&fast, 300);

    CHECK_NEAR(normal.waveTime, 5.0f, 1e-4f);
    CHECK_NEAR(fast.waveTime, 10.0f, 1e-4f);
}

static void test_cycle_speed(void)
{
    Game g;

    begin(&g, 22u);
    CHECK_NEAR(g.speedMultiplier, 1.0f, 1e-6f);
    game_cycle_speed(&g);
    CHECK_NEAR(g.speedMultiplier, 2.0f, 1e-6f);
    game_cycle_speed(&g);
    CHECK_NEAR(g.speedMultiplier, 4.0f, 1e-6f);
    game_cycle_speed(&g);
    CHECK_NEAR(g.speedMultiplier, 1.0f, 1e-6f);
}

static void test_pause(void)
{
    Game g;
    float frozenWaveTime;

    begin(&g, 23u);
    game_start_next_wave(&g);
    run_steps(&g, 60);
    CHECK_NEAR(g.waveTime, 1.0f, 1e-4f);

    game_toggle_pause(&g);
    CHECK(g.paused == true);
    frozenWaveTime = g.waveTime;
    run_steps(&g, 120);
    CHECK_NEAR(g.waveTime, frozenWaveTime, 1e-6f);
    game_update(&g, 0.5f);
    CHECK_NEAR(g.waveTime, frozenWaveTime, 1e-6f);

    game_place_tower(&g, 0, 12); /* entrance -> message */
    CHECK(g.message[0] != '\0');
    run_steps(&g, 120);
    CHECK(g.message[0] == '\0');

    game_toggle_pause(&g);
    run_steps(&g, 60);
    CHECK_NEAR(g.waveTime, 2.0f, 1e-4f);
}

static void test_fixed_step_accumulator(void)
{
    Game g;
    float start;

    begin(&g, 24u);
    game_start_next_wave(&g);
    start = g.waveTime;
    game_update(&g, 5.0f / 240.0f);
    CHECK_NEAR(g.waveTime - start, 1.0f / 60.0f, 1e-5f);

    begin(&g, 25u);
    game_start_next_wave(&g);
    game_update(&g, 10.0f);
    CHECK_NEAR(g.waveTime, 6.0f / 60.0f, 1e-5f);
    CHECK_NEAR(g.stepAccumulator, 0.0f, 1e-6f);

    begin(&g, 26u);
    game_start_next_wave(&g);
    {
        int i;
        for (i = 0; i < 5; ++i) {
            game_update(&g, 1.0f / 240.0f);
        }
    }
    CHECK_NEAR(g.waveTime, 1.0f / 60.0f, 1e-5f);
}

static void test_events(void)
{
    Game g;
    int i;

    begin(&g, 31u);
    CHECK(g.events == 0u);

    game_place_tower(&g, 0, 11);
    CHECK((g.events & GAME_EVT_BUILD) != 0u);

    game_clear_events(&g);
    game_select_tower(&g, 0);
    game_upgrade_selected(&g);
    CHECK((g.events & GAME_EVT_UPGRADE) != 0u);

    game_clear_events(&g);
    game_sell_selected(&g);
    CHECK((g.events & GAME_EVT_SELL) != 0u);

    game_clear_events(&g);
    game_place_tower(&g, 0, 11);
    game_start_next_wave(&g);
    CHECK((g.events & GAME_EVT_WAVE_START) != 0u);

    /* A high-level tower one-shots wave-1 enemies: laser + death events. */
    g.towers[0].level = TOWER_MAX_LEVEL;
    g.events = 0;
    for (i = 0; i < 300 && (g.events & GAME_EVT_ENEMY_DIE) == 0; ++i) {
        game_step(&g, 1.0f / 60.0f);
    }
    CHECK((g.events & GAME_EVT_LASER) != 0u);
    CHECK((g.events & GAME_EVT_ENEMY_DIE) != 0u);
    CHECK(g.kills >= 1);
}

static void test_messages(void)
{
    Game g;

    begin(&g, 32u);
    CHECK(g.message[0] == '\0');

    CHECK(game_place_tower(&g, 0, 12) == false);
    CHECK(g.message[0] != '\0');
    CHECK_NEAR(g.messageTimer, 1.5f, 1e-6f);
    CHECK(text_contains(g.message, "entrance"));
    game_clear_message(&g);
    CHECK(g.message[0] == '\0');

    CHECK(game_place_tower(&g, 0, 12) == false);
    run_steps(&g, 100);
    CHECK(g.message[0] == '\0');

    g.messageTimer = 99.0f;
    memset(g.message, 'x', MAX_MESSAGE_LEN - 1);
    g.message[MAX_MESSAGE_LEN - 1] = '\0';
    run_steps(&g, 1);
    CHECK(strlen(g.message) < MAX_MESSAGE_LEN);
}

static void test_selection_and_cursor(void)
{
    Game g;

    begin(&g, 33u);
    CHECK(g.selectedType == DMG_NORMAL);
    game_set_selected_type(&g, DMG_SLAG);
    CHECK(g.selectedType == DMG_SLAG);
    game_set_selected_type(&g, -1);
    CHECK(g.selectedType == DMG_SLAG);
    game_set_selected_type(&g, TOWER_TYPE_COUNT);
    CHECK(g.selectedType == DMG_SLAG);

    game_set_cursor(&g, 4, 5);
    CHECK(g.cursorCol == 4);
    CHECK(g.cursorRow == 5);
    game_set_cursor(&g, -3, 99);
    CHECK(g.cursorCol == 0);
    CHECK(g.cursorRow == GRID_ROWS - 1);
    game_set_cursor(&g, GRID_COLS, -1);
    CHECK(g.cursorCol == GRID_COLS - 1);
    CHECK(g.cursorRow == 0);

    game_place_tower(&g, 0, 0);
    CHECK(g.selectedTower == 0);
    game_select_tower(&g, -1);
    CHECK(g.selectedTower == -1);
}

static void test_capacity_pools(void)
{
    Game g;
    int i;

    /* Enemy array full: spawning waits instead of overflowing. */
    begin(&g, 41u);
    game_start_next_wave(&g);
    for (i = 0; i < MAX_ENEMIES; ++i) {
        put_simple_enemy(&g, i, 0.0f, LAYER_HP, 1000.0f);
    }
    g.enemiesAlive = MAX_ENEMIES;
    g.enemiesToSpawn = 5;
    run_steps(&g, 5);
    CHECK(g.enemiesToSpawn == 5);
    CHECK(g.enemiesAlive == MAX_ENEMIES);

    /* Beam array full: firing drops the cosmetic beam, never overflows. */
    setup_single_tower(&g, DMG_NORMAL);
    for (i = 0; i < MAX_BEAMS; ++i) {
        g.beams[i].active = true;
        g.beams[i].life = 0.15f;
        g.beams[i].maxLife = 0.15f;
    }
    put_simple_enemy(&g, 0, 0.0f, LAYER_HP, 100.0f);
    g.enemiesAlive = 1;
    game_step(&g, 1.0f / 60.0f);
    {
        int activeBeams = 0;
        for (i = 0; i < MAX_BEAMS; ++i) {
            if (g.beams[i].active) {
                ++activeBeams;
            }
            CHECK(g.beams[i].life >= 0.0f);
        }
        CHECK(activeBeams <= MAX_BEAMS);
    }
    CHECK(g.enemies[0].layers[0].hp < 100.0f);

    /* First-wave sanity: a fully built board still has no tower-count limit. */
    begin(&g, 44u);
    g.gold = 1000000;
    for (i = 0; i < 12; ++i) {
        CHECK(game_place_tower(&g, i, 0) == true);
    }
    CHECK(g.towerCount == 12);
    CHECK(!text_contains(g.message, "tower limit"));
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("Elemental Defense - portable core unit tests (v0.7)\n");
    printf("===================================================\n");

    RUN_TEST(test_tower_specs);
    RUN_TEST(test_tower_stats_per_level);
    RUN_TEST(test_damage_multiplier_matrix);
    RUN_TEST(test_upgrade_cost_curve);
    RUN_TEST(test_sell_refund);
    RUN_TEST(test_placement_rules);
    RUN_TEST(test_placement_affordability);
    RUN_TEST(test_placement_phase_guard);
    RUN_TEST(test_initial_path);
    RUN_TEST(test_build_on_road_reroutes);
    RUN_TEST(test_blocking_placement_refused);
    RUN_TEST(test_entrance_exit_rejected);
    RUN_TEST(test_sell_shortens_path);
    RUN_TEST(test_maze_long_route);
    RUN_TEST(test_speed_independent_of_path_length);
    RUN_TEST(test_enemy_reprojection_on_build);
    RUN_TEST(test_cell_center);
    RUN_TEST(test_wave_composition);
    RUN_TEST(test_spawn_intervals);
    RUN_TEST(test_long_run_scaling);
    RUN_TEST(test_spawn_layer_counts_by_wave);
    RUN_TEST(test_layer_composition_truthful);
    RUN_TEST(test_damage_application_all_types);
    RUN_TEST(test_layer_depletion_and_carry);
    RUN_TEST(test_layer_death_rewards);
    RUN_TEST(test_burn_stacks_and_ticks);
    RUN_TEST(test_stun_stops_movement);
    RUN_TEST(test_puddle_spawn_and_tick);
    RUN_TEST(test_puddle_lifetime_and_cap);
    RUN_TEST(test_slag_effect);
    RUN_TEST(test_explosive_aoe);
    RUN_TEST(test_targeting_preferences);
    RUN_TEST(test_normal_beam_visual);
    RUN_TEST(test_enemy_movement_speed);
    RUN_TEST(test_leak_costs_life);
    RUN_TEST(test_idle_player_loses);
    RUN_TEST(test_gameover_freezes_simulation);
    RUN_TEST(test_victory_after_1000_waves);
    RUN_TEST(test_determinism);
    RUN_TEST(test_speed_multiplier);
    RUN_TEST(test_cycle_speed);
    RUN_TEST(test_pause);
    RUN_TEST(test_fixed_step_accumulator);
    RUN_TEST(test_events);
    RUN_TEST(test_messages);
    RUN_TEST(test_selection_and_cursor);
    RUN_TEST(test_no_tower_cap);
    RUN_TEST(test_capacity_pools);

    printf("===================================================\n");
    printf("%d/%d tests passed, %d checks failed\n", g_tests - g_tests_failed,
           g_tests, g_failures);
    if (g_failures == 0) {
        printf("RESULT: PASS\n");
        return 0;
    }
    printf("RESULT: FAIL\n");
    return 1;
}
