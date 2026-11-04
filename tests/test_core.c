/*
 * Host unit tests for the portable Elemental Defense core.
 *
 * Plain C99, no framework, no test runner dependency: every test prints
 * PASS/FAIL and the process exits non-zero if anything failed.
 *
 * Build/run:  make -C tests            (add CC=... for a non-default compiler)
 * or:         make -C tests test
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "game.h"

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

/* Put an enemy into a slot by hand. x/y are refreshed by the next step, so we
 * only need the path position. */
static void put_enemy(Game *g, int slot, int element, float pathPos, float hp)
{
    Enemy *e = &g->enemies[slot];
    memset(e, 0, sizeof *e);
    e->active = true;
    e->element = (uint8_t)element;
    e->elite = false;
    e->maxHp = hp;
    e->hp = hp;
    e->pathPos = pathPos;
}

/*
 * One level-1 tower on (0,0), wave 1 started, no queued spawns, no enemies.
 * Clear the enemy array afterwards with put_enemy() if a test wants exactly one.
 */
static void setup_single_tower(Game *g)
{
    game_new_run(g, 7u);
    game_place_tower(g, 0, 0); /* centre (0.5,0.5); the top leg is row 1 */
    game_start_next_wave(g);
    g->enemiesToSpawn = 0;
    g->elitePending = 0;
    g->eliteDelayCount = 0;
    g->enemiesAlive = 0;
    g->towers[0].cooldown = 0.0f;
}

/* ------------------------------------------------------------------ */
/* content tables / stats                                             */
/* ------------------------------------------------------------------ */

static void test_tower_specs(void)
{
    int type;

    for (type = 0; type < TOWER_TYPE_COUNT; ++type) {
        const TowerSpec *spec = game_tower_spec(type);
        CHECK(spec != NULL);
        if (!spec) {
            continue;
        }
        CHECK(spec->name != NULL);
        CHECK(spec->cost == 20);
        CHECK_NEAR(spec->damage, 3.0f, 1e-6f);
        CHECK_NEAR(spec->fireRate, 0.8f, 1e-6f);
        CHECK_NEAR(spec->range, 60.0f / 28.0f, 1e-5f);
        CHECK(spec->color[0] >= 0.0f && spec->color[0] <= 1.0f);
    }
    CHECK(strcmp(game_tower_spec(ELEMENT_FIRE)->name, "Fire") == 0);
    CHECK(strcmp(game_tower_spec(ELEMENT_WATER)->name, "Water") == 0);
    CHECK(strcmp(game_tower_spec(ELEMENT_FOREST)->name, "Forest") == 0);
    CHECK(game_tower_spec(-1) == NULL);
    CHECK(game_tower_spec(TOWER_TYPE_COUNT) == NULL);

    CHECK(game_enemy_spec(0) != NULL);
    CHECK(game_enemy_spec(2) != NULL);
    CHECK(game_enemy_spec(-1) == NULL);
    CHECK(game_enemy_spec(3) == NULL);
}

static void test_tower_stats_per_level(void)
{
    static const float expectedDamage[TOWER_MAX_LEVEL] = {
        3.0f, 6.0f, 12.0f, 24.0f, 48.0f, 96.0f, 192.0f, 384.0f, 768.0f,
        1536.0f
    };
    static const float expectedRate[TOWER_MAX_LEVEL] = {
        0.80f, 0.72f, 0.64f, 0.57f, 0.51f, 0.45f, 0.40f, 0.36f, 0.32f,
        0.28f
    };
    int level;

    for (level = 1; level <= TOWER_MAX_LEVEL; ++level) {
        float damage = 0.0f;
        float fireRate = 0.0f;
        float range = 0.0f;
        game_tower_stats(ELEMENT_WATER, level, &damage, &fireRate, &range);
        CHECK_NEAR(damage, expectedDamage[level - 1], 1e-4f);
        CHECK_NEAR(fireRate, expectedRate[level - 1], 1e-6f);
        CHECK_NEAR(range, 60.0f / 28.0f, 1e-5f); /* range never upgrades */
    }

    /* Level is clamped to 1..TOWER_MAX_LEVEL by contract. */
    {
        float damage = 0.0f;
        game_tower_stats(ELEMENT_FIRE, 0, &damage, NULL, NULL);
        CHECK_NEAR(damage, 3.0f, 1e-6f);
        game_tower_stats(ELEMENT_FIRE, 99, &damage, NULL, NULL);
        CHECK_NEAR(damage, 1536.0f, 1e-6f);
    }
}

static void test_damage_multiplier_matrix(void)
{
    /* tower rows: fire, water, forest; enemy columns: fire, water, forest */
    static const float expected[TOWER_TYPE_COUNT][TOWER_TYPE_COUNT] = {
        { 1.0f, 0.5f, 2.0f }, /* fire tower   vs fire/water/forest */
        { 2.0f, 1.0f, 0.5f }, /* water tower  */
        { 0.5f, 2.0f, 1.0f }  /* forest tower */
    };
    int tower;
    int enemy;

    for (tower = 0; tower < TOWER_TYPE_COUNT; ++tower) {
        for (enemy = 0; enemy < TOWER_TYPE_COUNT; ++enemy) {
            CHECK_NEAR(game_damage_multiplier(tower, enemy),
                       expected[tower][enemy], 1e-6f);
        }
    }
    CHECK_NEAR(game_damage_multiplier(-1, 0), 1.0f, 1e-6f); /* clamps to fire */
    CHECK_NEAR(game_damage_multiplier(99, 99), 1.0f, 1e-6f);
}

/* ------------------------------------------------------------------ */
/* path and grid                                                      */
/* ------------------------------------------------------------------ */

static void test_fixed_path(void)
{
    Game g;
    int i;
    int8_t seen[GRID_ROWS][GRID_COLS];
    float expectedWaypoints[][2] = {
        { 0.5f, 1.5f },  { 23.5f, 1.5f }, { 23.5f, 2.5f },
        { 23.5f, 5.5f }, { 22.5f, 5.5f }, { 2.5f, 5.5f },
        { 2.5f, 6.5f },  { 2.5f, 9.5f },  { 3.5f, 9.5f },
        { 25.5f, 9.5f }
    };
    int waypointCells[] = { 0, 23, 24, 27, 28, 48, 49, 52, 53, 75 };

    begin(&g, 1234u);

    CHECK(g.pathBlocked == false);
    CHECK(g.path.count == 76);
    CHECK_NEAR(game_path_length(&g), 75.0f, 1e-6f);
    CHECK(g.path.count <= MAX_PATH_CELLS);

    memset(seen, 0, sizeof seen);
    for (i = 0; i < g.path.count; ++i) {
        int col = (int)(g.path.pts[i].x - 0.5f);
        int row = (int)(g.path.pts[i].y - 0.5f);
        CHECK(g.path.pts[i].x == (float)col + 0.5f);
        CHECK(g.path.pts[i].y == (float)row + 0.5f);
        CHECK(col >= 0 && col < GRID_COLS);
        CHECK(row >= 0 && row < GRID_ROWS);
        if (col >= 0 && col < GRID_COLS && row >= 0 && row < GRID_ROWS) {
            CHECK(!seen[row][col]); /* no cell repeats */
            seen[row][col] = 1;
        }
    }

    /* Start, end and the corners of the serpentine. */
    CHECK_NEAR(g.path.pts[0].x, 0.5f, 1e-6f);
    CHECK_NEAR(g.path.pts[0].y, 1.5f, 1e-6f);
    CHECK_NEAR(g.path.pts[g.path.count - 1].x, 25.5f, 1e-6f);
    CHECK_NEAR(g.path.pts[g.path.count - 1].y, 9.5f, 1e-6f);

    for (i = 0; i < 10; ++i) {
        int cell = waypointCells[i];
        CHECK_NEAR(g.path.pts[cell].x, expectedWaypoints[i][0], 1e-6f);
        CHECK_NEAR(g.path.pts[cell].y, expectedWaypoints[i][1], 1e-6f);
    }

    /* Every consecutive pair must be 4-connected. */
    for (i = 1; i < g.path.count; ++i) {
        int dx = (int)(g.path.pts[i].x - g.path.pts[i - 1].x);
        int dy = (int)(g.path.pts[i].y - g.path.pts[i - 1].y);
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        CHECK(dx + dy == 1);
    }

    /* Path cells are blocked for building, off-path cells are not. */
    CHECK(game_can_place_at(&g, 0, 1, 0) == false);
    CHECK(game_can_place_at(&g, 23, 5, 0) == false);
    CHECK(game_can_place_at(&g, 2, 9, 0) == false);
    CHECK(game_can_place_at(&g, 25, 9, 0) == false);
    CHECK(game_can_place_at(&g, 0, 0, 0) == true);
    CHECK(game_can_place_at(&g, 12, 2, 0) == true);
    CHECK(game_can_place_at(&g, 25, 12, 0) == true);
}

static void test_cell_center(void)
{
    Vec2 p = game_cell_center(0, 0);
    CHECK_NEAR(p.x, 0.5f, 1e-6f);
    CHECK_NEAR(p.y, 0.5f, 1e-6f);
    p = game_cell_center(25, 12);
    CHECK_NEAR(p.x, 25.5f, 1e-6f);
    CHECK_NEAR(p.y, 12.5f, 1e-6f);
    p = game_cell_center(3, 7);
    CHECK_NEAR(p.x, 3.5f, 1e-6f);
    CHECK_NEAR(p.y, 7.5f, 1e-6f);
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
    CHECK(game_tower_at(&g, 0, 0) == -1);
    CHECK(game_can_place_at(&g, 0, 0, 0) == true);
    CHECK(game_can_place_at(&g, -1, 0, 0) == false);
    CHECK(game_can_place_at(&g, 0, -1, 0) == false);
    CHECK(game_can_place_at(&g, GRID_COLS, 0, 0) == false);
    CHECK(game_can_place_at(&g, 0, GRID_ROWS, 0) == false);
    CHECK(game_can_place_at(&g, 0, 0, -1) == false);
    CHECK(game_can_place_at(&g, 0, 0, TOWER_TYPE_COUNT) == false);

    /* Valid placement: accepted, gold deducted, grid updated. */
    CHECK(game_place_tower(&g, 0, 0) == true);
    CHECK(g.gold == STARTING_GOLD - 20);
    CHECK(g.towerCount == 1);
    CHECK(g.occupancy[0][0] == 1);
    CHECK(game_tower_at(&g, 0, 0) == 0);
    CHECK(g.towers[0].active);
    CHECK(g.towers[0].level == 1);
    CHECK(g.towers[0].element == ELEMENT_FIRE);
    CHECK(g.towers[0].invested == 20);
    CHECK(g.selectedTower == 0);

    /* Occupied cell rejected. */
    CHECK(game_place_tower(&g, 0, 0) == false);
    CHECK(text_contains(g.message, "occupied"));
    CHECK(g.towerCount == 1);

    /* Path cell rejected with a specific message. */
    game_clear_message(&g);
    CHECK(game_place_tower(&g, 5, 1) == false);
    CHECK(text_contains(g.message, "path"));
    CHECK(g.towerCount == 1);
    CHECK(g.gold == STARTING_GOLD - 20);

    /* Out of bounds rejected. */
    game_clear_message(&g);
    CHECK(game_place_tower(&g, -1, 0) == false);
    CHECK(g.message[0] != '\0');
    CHECK(game_place_tower(&g, 0, GRID_ROWS) == false);
    CHECK(g.towerCount == 1);

    /* Tower selection from the grid. */
    CHECK(game_tower_at(&g, 0, 1) == -1);
    CHECK(game_tower_at(&g, -1, 0) == -1);
    game_select_tower(&g, 0);
    CHECK(g.selectedTower == 0);
    game_select_tower(&g, 7);
    CHECK(g.selectedTower == -1);
    game_select_tower(&g, -1);
    CHECK(g.selectedTower == -1);
}

static void test_placement_affordability(void)
{
    Game g;

    begin(&g, 2u);
    g.gold = 5;
    /* Legality does not depend on gold... */
    CHECK(game_can_place_at(&g, 0, 0, 0) == true);
    /* ...but the action must reject it. */
    CHECK(game_place_tower(&g, 0, 0) == false);
    CHECK(text_contains(g.message, "not enough gold"));
    CHECK(g.gold == 5);
    CHECK(g.towerCount == 0);
    CHECK(game_tower_at(&g, 0, 0) == -1);

    /* Exactly enough gold is fine. */
    g.gold = 20;
    CHECK(game_place_tower(&g, 0, 0) == true);
    CHECK(g.gold == 0);
    CHECK(g.towerCount == 1);
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

    CHECK(game_upgrade_cost(&g, 0) == 20);
    CHECK(game_selected_upgrade_cost(&g) == 20);

    for (level = 1; level < TOWER_MAX_LEVEL; ++level) {
        int costBefore = game_upgrade_cost(&g, 0);
        CHECK(costBefore == expectedCost[level - 1]);
        CHECK(game_upgrade_selected(&g) == true);
        CHECK(g.towers[0].level == level + 1);
        CHECK(g.towers[0].invested == 20 * (1 << level));
    }
    CHECK(g.towers[0].level == TOWER_MAX_LEVEL);

    /* Max level: no further upgrade, -1 cost, "max level" message. */
    game_clear_message(&g);
    CHECK(game_upgrade_cost(&g, 0) == -1);
    CHECK(game_selected_upgrade_cost(&g) == -1);
    CHECK(game_upgrade_selected(&g) == false);
    CHECK(text_contains(g.message, "max level"));
    CHECK(g.towers[0].level == TOWER_MAX_LEVEL);

    /* Invalid tower indices never crash. */
    CHECK(game_upgrade_cost(&g, -1) == -1);
    CHECK(game_upgrade_cost(&g, 5) == -1);
    CHECK(game_sell_value(&g, -1) == 0);

    /* Damage at level 10 is 3 * 2^9 = 1536. */
    {
        float damage = 0.0f;
        game_tower_stats(g.towers[0].element, g.towers[0].level, &damage, NULL,
                         NULL);
        CHECK_NEAR(damage, 1536.0f, 1e-4f);
    }
}

static void test_upgrade_affordability(void)
{
    Game g;

    begin(&g, 4u);
    CHECK(game_place_tower(&g, 0, 0) == true); /* gold 130 */
    game_select_tower(&g, 0);
    g.gold = 19;
    game_clear_message(&g);
    CHECK(game_upgrade_selected(&g) == false);
    CHECK(text_contains(g.message, "not enough gold"));
    CHECK(g.towers[0].level == 1);
    CHECK(g.gold == 19);

    /* Upgrade with no selection. */
    game_select_tower(&g, -1);
    game_clear_message(&g);
    CHECK(game_upgrade_selected(&g) == false);
    CHECK(text_contains(g.message, "no tower"));

    /* Sell with no selection. */
    CHECK(game_sell_selected(&g) == false);
    CHECK(text_contains(g.message, "no tower"));
}

static void test_sell_refund(void)
{
    Game g;

    begin(&g, 5u);
    CHECK(game_place_tower(&g, 0, 0) == true); /* gold 130, invested 20 */
    game_select_tower(&g, 0);
    CHECK(game_sell_value(&g, 0) == 15); /* floor(20 * 0.75) */
    CHECK(game_selected_sell_value(&g) == 15);

    g.events = 0;
    CHECK(game_sell_selected(&g) == true);
    CHECK(g.gold == 130 + 15);
    CHECK(g.towerCount == 0);
    CHECK(g.occupancy[0][0] == 0);
    CHECK(game_tower_at(&g, 0, 0) == -1);
    CHECK(g.selectedTower == -1);
    CHECK((g.events & GAME_EVT_SELL) != 0u);

    /* Upgraded tower: invested 20 + 20 = 40, refund 30. */
    CHECK(game_place_tower(&g, 0, 0) == true); /* gold 145-20 = 125 */
    game_select_tower(&g, 0);
    CHECK(game_upgrade_selected(&g) == true); /* gold 105, invested 40 */
    CHECK(g.towers[0].invested == 40);
    CHECK(game_sell_value(&g, 0) == 30);
    CHECK(game_sell_selected(&g) == true);
    CHECK(g.gold == 105 + 30);
    CHECK(g.towerCount == 0);

    /* The cell is buildable again after selling. */
    CHECK(game_can_place_at(&g, 0, 0, 0) == true);
}

/* ------------------------------------------------------------------ */
/* waves                                                              */
/* ------------------------------------------------------------------ */

static void test_wave_composition(void)
{
    static const int waves[] = { 1, 4, 5, 10, 25 };
    static const int expectedSpawns[] = { 5, 11, 13, 23, 53 };
    static const int expectedElites[] = { 0, 0, 1, 3, 10 };
    static const int expectedFire[] = { 1, 3, 4, 6, 14 };
    static const int expectedWater[] = { 1, 3, 4, 6, 14 };
    static const int expectedForest[] = { 3, 5, 4, 8, 15 };
    int i;

    for (i = 0; i < 5; ++i) {
        Game g;
        int fire = -1;
        int water = -1;
        int forest = -1;
        int elite = -1;

        begin(&g, 100u + (uint32_t)i);
        g.wave = waves[i];

        game_wave_preview(&g, &fire, &water, &forest, &elite);
        CHECK(fire == expectedFire[i]);
        CHECK(water == expectedWater[i]);
        CHECK(forest == expectedForest[i]);
        CHECK(elite == expectedElites[i]);
        /* The preview must describe the whole wave exactly. */
        CHECK(fire + water + forest + elite == expectedSpawns[i]);

        game_start_next_wave(&g);
        CHECK(g.phase == PHASE_WAVE);
        CHECK(g.enemiesToSpawn == expectedSpawns[i]);
        CHECK(g.enemiesAlive == 0);

        /* Regular element quotas are staged for exact spawning. */
        CHECK(g.spawnQuota[ELEMENT_FIRE] == expectedFire[i]);
        CHECK(g.spawnQuota[ELEMENT_WATER] == expectedWater[i]);
        CHECK(g.spawnQuota[ELEMENT_FOREST] == expectedForest[i]);
        CHECK(g.elitePending == expectedElites[i]);
    }

    /* game_start_next_wave() must not restart a running wave. */
    {
        Game g;
        begin(&g, 77u);
        game_start_next_wave(&g);
        CHECK(g.phase == PHASE_WAVE);
        g.enemiesToSpawn = 1;
        game_start_next_wave(&g);
        CHECK(g.phase == PHASE_WAVE);
        CHECK(g.enemiesToSpawn == 1);
    }
}

static void test_spawn_intervals(void)
{
    static const int waves[] = { 1, 4, 5, 10, 25, 60 };
    static const float expected[] = { 2.95f, 2.80f, 2.75f, 2.50f, 1.75f, 0.80f };
    int i;

    for (i = 0; i < 6; ++i) {
        Game g;
        begin(&g, 55u + (uint32_t)i);
        g.wave = waves[i];
        game_start_next_wave(&g);
        CHECK_NEAR(g.spawnInterval, expected[i], 1e-5f);
    }
}

static void test_elite_spawn_stats(void)
{
    Game g;
    int i;
    int foundElite = 0;
    int foundRegular = 0;

    /* Wave 5: one elite, in the second spawn slot. No towers, so just watch it
     * appear and inspect the frozen stats: hp 27 normal, 67.5 elite. */
    begin(&g, 202u);
    g.wave = 5;
    game_start_next_wave(&g);
    CHECK_NEAR(g.spawnInterval, 2.75f, 1e-5f);

    for (i = 0; i < 400 && !foundElite; ++i) {
        int j;
        game_step(&g, 1.0f / 60.0f);
        for (j = 0; j < MAX_ENEMIES; ++j) {
            Enemy *e = &g.enemies[j];
            if (!e->active || !e->elite) {
                continue;
            }
            foundElite = 1;
            CHECK_NEAR(e->maxHp, 67.5f, 1e-4f);
            CHECK_NEAR(e->hp, 67.5f, 1e-4f);
            CHECK(e->pathPos < 1.5f); /* freshly spawned this step */
        }
    }
    CHECK(foundElite == 1);

    /* And a regular one on the same wave. */
    begin(&g, 203u);
    g.wave = 5;
    game_start_next_wave(&g);
    for (i = 0; i < 400 && !foundRegular; ++i) {
        int j;
        game_step(&g, 1.0f / 60.0f);
        for (j = 0; j < MAX_ENEMIES; ++j) {
            Enemy *e = &g.enemies[j];
            if (!e->active || e->elite) {
                continue;
            }
            foundRegular = 1;
            CHECK_NEAR(e->maxHp, 27.0f, 1e-4f);
        }
    }
    CHECK(foundRegular == 1);
}

/* Speed up spawning and count every enemy of a wave to prove the element
 * quotas and elite slots are exactly what the preview promised. */
static void test_spawn_quotas_exact(void)
{
    static const int waves[] = { 1, 4, 5, 10, 25 };
    static const int expectedWaveElites[] = { 0, 0, 1, 3, 10 };
    int w;

    for (w = 0; w < 5; ++w) {
        Game g;
        int fire = 0;
        int water = 0;
        int forest = 0;
        int elites = 0;
        int total = 0;
        int i;
        int guard;

        begin(&g, 300u + (uint32_t)w);
        g.wave = waves[w];
        game_start_next_wave(&g);
        g.spawnInterval = 0.02f; /* white box: spawn everything long before the
                                  * first enemy can cross the road */

        for (guard = 0; guard < 5000 && g.enemiesToSpawn > 0; ++guard) {
            game_step(&g, 1.0f / 60.0f);
        }
        CHECK(g.enemiesToSpawn == 0);
        CHECK(g.phase == PHASE_WAVE);

        for (i = 0; i < MAX_ENEMIES; ++i) {
            Enemy *e = &g.enemies[i];
            if (!e->active) {
                continue;
            }
            ++total;
            if (e->elite) {
                ++elites;
            } else if (e->element == ELEMENT_FIRE) {
                ++fire;
            } else if (e->element == ELEMENT_WATER) {
                ++water;
            } else {
                ++forest;
            }
        }
        CHECK(total == 3 + 2 * waves[w]);
        CHECK(elites == expectedWaveElites[w]);
        CHECK(fire == (3 + 2 * waves[w] - elites) / 3);
        CHECK(water == (3 + 2 * waves[w] - elites) / 3);
        CHECK(forest == (3 + 2 * waves[w] - elites) - 2 * fire);

        /* Elite spawn slots are 2..1+E; the array preserves spawn order. */
        {
            int slot;
            int seenElite = 0;
            for (slot = 0; slot < MAX_ENEMIES; ++slot) {
                if (!g.enemies[slot].active) {
                    continue;
                }
                if (slot == 0) {
                    CHECK(g.enemies[slot].elite == false);
                } else if (g.enemies[slot].elite) {
                    ++seenElite;
                }
            }
            CHECK(seenElite == elites);
        }
    }
}

/* ------------------------------------------------------------------ */
/* combat                                                             */
/* ------------------------------------------------------------------ */

static void test_damage_application(void)
{
    int element;

    for (element = 0; element < TOWER_TYPE_COUNT; ++element) {
        Game g;
        float multiplier;

        setup_single_tower(&g);
        put_enemy(&g, 0, element, 0.0f, 100.0f);
        g.enemiesAlive = 1;
        g.towers[0].cooldown = 0.0f;

        multiplier = game_damage_multiplier(ELEMENT_FIRE, element);
        game_step(&g, 1.0f / 60.0f);

        CHECK(g.enemies[0].active);
        CHECK_NEAR(g.enemies[0].hp, 100.0f - 3.0f * multiplier, 1e-4f);
        CHECK(g.events & GAME_EVT_LASER);
    }
}

static void test_targeting_nearest(void)
{
    Game g;

    setup_single_tower(&g);
    put_enemy(&g, 0, ELEMENT_FIRE, 0.10f, 100.0f); /* ends up closer */
    put_enemy(&g, 1, ELEMENT_FIRE, 0.50f, 100.0f);
    g.enemiesAlive = 2;
    g.towers[0].cooldown = 0.0f;

    game_step(&g, 1.0f / 60.0f);

    CHECK(g.enemies[0].hp < 100.0f);
    CHECK_NEAR(g.enemies[1].hp, 100.0f, 1e-6f);
}

static void test_beam_lifetime(void)
{
    Game g;
    int i;
    int beamsAfterShot = 0;

    setup_single_tower(&g);
    put_enemy(&g, 0, ELEMENT_WATER, 0.0f, 100.0f);
    g.enemiesAlive = 1;
    g.towers[0].cooldown = 0.0f;

    game_step(&g, 1.0f / 60.0f);
    for (i = 0; i < MAX_BEAMS; ++i) {
        if (g.beams[i].active) {
            ++beamsAfterShot;
            CHECK_NEAR(g.beams[i].life, 0.15f, 1e-5f);
            CHECK_NEAR(g.beams[i].maxLife, 0.15f, 1e-5f);
            CHECK(g.beams[i].element == ELEMENT_FIRE);
        }
    }
    CHECK(beamsAfterShot >= 1);

    /* 0.15 s of life: it must be gone after 10 fixed steps (0.166 s). */
    run_steps(&g, 10);
    beamsAfterShot = 0;
    for (i = 0; i < MAX_BEAMS; ++i) {
        if (g.beams[i].active) {
            ++beamsAfterShot;
        }
    }
    CHECK(beamsAfterShot == 0);
}

static void test_enemy_movement_speed(void)
{
    Game g;

    begin(&g, 11u);
    game_start_next_wave(&g);
    g.enemiesToSpawn = 0;
    g.elitePending = 0;
    g.eliteDelayCount = 0;
    put_enemy(&g, 0, ELEMENT_FIRE, 0.0f, 1000.0f);
    g.enemiesAlive = 1;

    /* Wave 1: speed 20, path length 75 -> 15 cells per second. */
    run_steps(&g, 60);
    CHECK_NEAR(g.enemies[0].pathPos, 15.0f, 0.1f);
    CHECK_NEAR(g.enemies[0].x, 15.5f, 0.1f);
    CHECK_NEAR(g.enemies[0].y, 1.5f, 1e-5f);
}

static void test_leak_costs_life(void)
{
    Game g;

    begin(&g, 12u);
    game_start_next_wave(&g);
    g.enemiesToSpawn = 0;
    g.elitePending = 0;
    g.eliteDelayCount = 0;
    put_enemy(&g, 0, ELEMENT_FIRE, 75.0f - 0.1f, 1000.0f);
    g.enemiesAlive = 1;
    g.events = 0;

    game_step(&g, 1.0f / 60.0f);

    CHECK(g.leaks == 1);
    CHECK(g.lives == STARTING_LIVES - 1);
    CHECK(g.kills == 0);
    CHECK(g.gold == STARTING_GOLD); /* no gold for leaks */
    CHECK((g.events & GAME_EVT_LEAK) != 0u);
    CHECK(g.enemiesAlive == 0);
    /* The wave is now empty, so it advanced to the build phase for wave 2. */
    CHECK(g.phase == PHASE_BUILD);
    CHECK(g.wave == 2);
}

/* ------------------------------------------------------------------ */
/* fail states                                                        */
/* ------------------------------------------------------------------ */

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
    CHECK(g.leaks == STARTING_LIVES); /* exactly 20 leaks to lose */
    CHECK(g.kills == 0);
    CHECK(g.wave == 3); /* 5 + 7 + 8 leaked = 20 lives */
    CHECK((g.events & GAME_EVT_GAMEOVER) != 0u);
}

static void test_gameover_freezes_simulation(void)
{
    Game g;
    float frozenWaveTime;
    int frozenKills;

    begin(&g, 43u);
    g.lives = 1;
    game_start_next_wave(&g);
    run_steps(&g, 1200); /* first enemy crosses after ~8 s, so this ends it */

    CHECK(g.phase == PHASE_GAMEOVER);
    CHECK(g.lives == 0);

    frozenWaveTime = g.waveTime;
    frozenKills = g.kills;
    game_step(&g, 5.0f);
    run_steps(&g, 60);
    CHECK_NEAR(g.waveTime, frozenWaveTime, 1e-6f);
    CHECK(g.kills == frozenKills);

    /* Starting a wave after a loss must not do anything. */
    game_start_next_wave(&g);
    CHECK(g.phase == PHASE_GAMEOVER);
}

static void test_victory_after_25_waves(void)
{
    Game g;
    int guard;
    int expectedLeaks = 0;
    int wave;

    begin(&g, 2024u);
    g.lives = 100000; /* survive leaks: this test is about wave progression */
    /* 4x speed keeps the test fast; the step count is still bounded. */
    game_cycle_speed(&g);
    game_cycle_speed(&g);
    CHECK_NEAR(g.speedMultiplier, 4.0f, 1e-6f);

    for (wave = 1; wave <= 25; ++wave) {
        expectedLeaks += 3 + 2 * wave;
    }

    for (guard = 0; guard < 2000000; ++guard) {
        if (g.phase == PHASE_BUILD) {
            game_start_next_wave(&g);
        }
        if (g.phase == PHASE_VICTORY || g.phase == PHASE_GAMEOVER) {
            break;
        }
        game_step(&g, 1.0f / 60.0f);
    }

    CHECK(g.phase == PHASE_VICTORY);
    CHECK(g.wave == 26); /* past the last wave */
    CHECK(g.totalWaves == 25);
    CHECK(g.kills == 0);
    CHECK(g.leaks == expectedLeaks);
    CHECK(g.lives == 100000 - expectedLeaks);
    CHECK((g.events & GAME_EVT_VICTORY) != 0u);
}

/* ------------------------------------------------------------------ */
/* determinism / timing                                               */
/* ------------------------------------------------------------------ */

static void run_script(Game *g, uint32_t seed)
{
    begin(g, seed);
    game_place_tower(g, 0, 0);
    game_set_selected_type(g, ELEMENT_FOREST);
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
    CHECK(a.kills == b.kills);
    CHECK_NEAR(a.waveTime, b.waveTime, 1e-6f);
    CHECK_NEAR(a.timeTotal, b.timeTotal, 1e-6f);
}

static void test_speed_multiplier(void)
{
    Game normal;
    Game fast;

    begin(&normal, 21u);
    begin(&fast, 21u);
    game_start_next_wave(&normal);
    game_start_next_wave(&fast);
    game_cycle_speed(&fast); /* 1x -> 2x */
    CHECK_NEAR(fast.speedMultiplier, 2.0f, 1e-6f);

    run_steps(&normal, 300);
    run_steps(&fast, 300);

    CHECK_NEAR(normal.waveTime, 5.0f, 1e-4f);
    CHECK_NEAR(fast.waveTime, 10.0f, 1e-4f);
    CHECK_NEAR(fast.waveTime, normal.waveTime * 2.0f, 1e-4f);
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

    /* Paused and unpaused are frozen by game_update() too. */
    game_update(&g, 0.5f);
    CHECK_NEAR(g.waveTime, frozenWaveTime, 1e-6f);

    /* UI messages still expire while paused. */
    game_place_tower(&g, 5, 1); /* path cell -> message */
    CHECK(g.message[0] != '\0');
    run_steps(&g, 120); /* 2 s of message timer */
    CHECK(g.message[0] == '\0');
    CHECK(g.messageTimer <= 0.0f);

    game_toggle_pause(&g);
    CHECK(g.paused == false);
    run_steps(&g, 60);
    CHECK_NEAR(g.waveTime, 2.0f, 1e-4f);

    /* game_update() also ticks messages while paused. */
    game_place_tower(&g, 5, 1);
    CHECK(g.messageTimer > 0.0f);
    game_toggle_pause(&g);
    {
        int i;
        for (i = 0; i < 4; ++i) {
            game_update(&g, 0.5f); /* clamped to 0.25 each call = 1 s total */
        }
    }
    CHECK(g.messageTimer < 0.75f);
    game_toggle_pause(&g);
}

static void test_fixed_step_accumulator(void)
{
    Game g;
    float start;

    begin(&g, 24u);
    game_start_next_wave(&g);
    start = g.waveTime;

    /* One update of 5/240 s is just over one fixed step. */
    game_update(&g, 5.0f / 240.0f);
    CHECK_NEAR(g.waveTime - start, 1.0f / 60.0f, 1e-5f);

    /* Huge frame: clamped and capped at 6 fixed steps. */
    begin(&g, 25u);
    game_start_next_wave(&g);
    game_update(&g, 10.0f);
    CHECK_NEAR(g.waveTime, 6.0f / 60.0f, 1e-5f);
    CHECK_NEAR(g.stepAccumulator, 0.0f, 1e-6f);

    /* Sub-step frames accumulate instead of being dropped. */
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

/* ------------------------------------------------------------------ */
/* events / messages / controls                                       */
/* ------------------------------------------------------------------ */

static void test_events(void)
{
    Game g;
    int i;

    begin(&g, 31u);
    CHECK(g.events == 0u);

    game_place_tower(&g, 0, 0);
    CHECK((g.events & GAME_EVT_BUILD) != 0u);

    game_clear_events(&g);
    CHECK(g.events == 0u);
    game_select_tower(&g, 0);
    game_upgrade_selected(&g);
    CHECK((g.events & GAME_EVT_UPGRADE) != 0u);

    game_clear_events(&g);
    game_sell_selected(&g);
    CHECK((g.events & GAME_EVT_SELL) != 0u);

    game_clear_events(&g);
    game_place_tower(&g, 0, 0);
    game_start_next_wave(&g);
    CHECK((g.events & GAME_EVT_WAVE_START) != 0u);

    /* A level-10 tower one-shots wave-1 enemies: laser + death events. */
    g.towers[0].level = TOWER_MAX_LEVEL;
    g.events = 0;
    for (i = 0; i < 300 && (g.events & GAME_EVT_ENEMY_DIE) == 0; ++i) {
        game_step(&g, 1.0f / 60.0f);
    }
    CHECK((g.events & GAME_EVT_LASER) != 0u);
    CHECK((g.events & GAME_EVT_ENEMY_DIE) != 0u);
    CHECK(g.kills >= 1);
    CHECK(g.towers[0].cooldown > 0.0f);
}

static void test_messages(void)
{
    Game g;

    begin(&g, 32u);
    CHECK(g.message[0] == '\0');
    CHECK(g.messageTimer == 0.0f);

    CHECK(game_place_tower(&g, 0, 1) == false); /* path cell */
    CHECK(g.message[0] != '\0');
    CHECK_NEAR(g.messageTimer, 1.5f, 1e-6f);
    CHECK(text_contains(g.message, "path"));

    game_clear_message(&g);
    CHECK(g.message[0] == '\0');
    CHECK(g.messageTimer == 0.0f);

    /* Messages expire on their own (BUILD phase still ticks the timer). */
    CHECK(game_place_tower(&g, 0, 1) == false);
    run_steps(&g, 100); /* 1.667 s */
    CHECK(g.message[0] == '\0');
    CHECK(g.messageTimer <= 0.0f);

    /* Long message would be truncated safely, never overflow. */
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
    CHECK(g.selectedType == ELEMENT_FIRE);
    CHECK(g.selectedTower == -1);

    game_set_selected_type(&g, ELEMENT_FOREST);
    CHECK(g.selectedType == ELEMENT_FOREST);
    game_set_selected_type(&g, -1); /* ignored */
    CHECK(g.selectedType == ELEMENT_FOREST);
    game_set_selected_type(&g, 3); /* ignored */
    CHECK(g.selectedType == ELEMENT_FOREST);

    game_set_cursor(&g, 4, 5);
    CHECK(g.cursorCol == 4);
    CHECK(g.cursorRow == 5);
    game_set_cursor(&g, -3, 99);
    CHECK(g.cursorCol == 0);
    CHECK(g.cursorRow == GRID_ROWS - 1);
    game_set_cursor(&g, GRID_COLS, -1);
    CHECK(g.cursorCol == GRID_COLS - 1);
    CHECK(g.cursorRow == 0);

    /* Selecting a tower through the API marks it. */
    game_place_tower(&g, 0, 0);
    CHECK(g.selectedTower == 0);
    game_select_tower(&g, 0);
    CHECK(g.selectedTower == 0);
    game_select_tower(&g, -1);
    CHECK(g.selectedTower == -1);
}

/* ------------------------------------------------------------------ */
/* capacity safety                                                    */
/* ------------------------------------------------------------------ */

static void test_capacity_limits(void)
{
    Game g;
    int i;

    /* Enemy array full: spawning waits instead of overflowing. */
    begin(&g, 41u);
    game_start_next_wave(&g);
    for (i = 0; i < MAX_ENEMIES; ++i) {
        put_enemy(&g, i, i % TOWER_TYPE_COUNT, 0.0f, 1000.0f);
    }
    g.enemiesAlive = MAX_ENEMIES;
    g.enemiesToSpawn = 5;
    run_steps(&g, 5);
    CHECK(g.enemiesToSpawn == 5); /* no spawn was lost */
    CHECK(g.enemiesAlive == MAX_ENEMIES);
    CHECK(g.enemiesAlive <= MAX_ENEMIES);

    /* Beam array full: firing drops the cosmetic beam, never overflows. */
    setup_single_tower(&g);
    for (i = 0; i < MAX_BEAMS; ++i) {
        g.beams[i].active = true;
        g.beams[i].life = 0.15f;
        g.beams[i].maxLife = 0.15f;
    }
    put_enemy(&g, 0, ELEMENT_FIRE, 0.0f, 100.0f);
    g.enemiesAlive = 1;
    g.towers[0].cooldown = 0.0f;
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
    CHECK(g.enemies[0].hp < 100.0f); /* the tower still dealt damage */

    /* Tower array full: placement is rejected with a message. */
    begin(&g, 44u);
    g.towerCount = MAX_TOWERS;
    game_clear_message(&g);
    CHECK(game_place_tower(&g, 0, 0) == false);
    CHECK(text_contains(g.message, "tower limit"));
    CHECK(g.towerCount == MAX_TOWERS);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("Elemental Defense - portable core unit tests\n");
    printf("============================================\n");

    RUN_TEST(test_tower_specs);
    RUN_TEST(test_tower_stats_per_level);
    RUN_TEST(test_damage_multiplier_matrix);
    RUN_TEST(test_fixed_path);
    RUN_TEST(test_cell_center);
    RUN_TEST(test_placement_rules);
    RUN_TEST(test_placement_affordability);
    RUN_TEST(test_placement_phase_guard);
    RUN_TEST(test_upgrade_cost_curve);
    RUN_TEST(test_upgrade_affordability);
    RUN_TEST(test_sell_refund);
    RUN_TEST(test_wave_composition);
    RUN_TEST(test_spawn_intervals);
    RUN_TEST(test_elite_spawn_stats);
    RUN_TEST(test_spawn_quotas_exact);
    RUN_TEST(test_damage_application);
    RUN_TEST(test_targeting_nearest);
    RUN_TEST(test_beam_lifetime);
    RUN_TEST(test_enemy_movement_speed);
    RUN_TEST(test_leak_costs_life);
    RUN_TEST(test_idle_player_loses);
    RUN_TEST(test_gameover_freezes_simulation);
    RUN_TEST(test_victory_after_25_waves);
    RUN_TEST(test_determinism);
    RUN_TEST(test_speed_multiplier);
    RUN_TEST(test_cycle_speed);
    RUN_TEST(test_pause);
    RUN_TEST(test_fixed_step_accumulator);
    RUN_TEST(test_events);
    RUN_TEST(test_messages);
    RUN_TEST(test_selection_and_cursor);
    RUN_TEST(test_capacity_limits);

    printf("============================================\n");
    printf("%d/%d tests passed, %d checks failed\n", g_tests - g_tests_failed,
           g_tests, g_failures);
    if (g_failures == 0) {
        printf("RESULT: PASS\n");
        return 0;
    }
    printf("RESULT: FAIL\n");
    return 1;
}
