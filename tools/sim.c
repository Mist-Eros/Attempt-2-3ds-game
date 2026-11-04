/*
 * Elemental Defense - headless auto-play / balance sim (v0.3).
 *
 * Drives the real portable core (no rendering, no platform code) with a simple
 * greedy scripted player and prints one line per wave plus a final verdict.
 * This is the proof that the game loop is actually playable end to end.
 *
 * Player styles:
 *
 *   default / mixed - towers beside the road only (never on it), cycling all six
 *                     damage types so the army covers HP / SHIELD / ARMOR, then
 *                     upgrades. The road stays at its initial 25 segments.
 *   --maze          - same mixed army, but first spends gold planting towers ON
 *                     the road, greedily choosing the cell whose blocking
 *                     lengthens the route the most (the intended play style).
 *   --plain         - GUN towers only (neutral 1.0x vs every layer): the
 *                     baseline that ignores the element system.
 *
 * Usage:  sim.exe [--maze] [--plain] [seed]
 * Exit code is always 0; the verdict line says VICTORY / GAMEOVER.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "game.h"
#include "core_internal.h" /* core_path_* / SPEED_SCALE accessors; tools only */

#define TOWER_RANGE_CELLS (60.0f / 28.0f) /* same conversion as the core */
#define MAX_SPOTS (GRID_COLS * GRID_ROWS)
#define SUPPORT_TOWER_TARGET 36
#define MAZE_WALL_TARGET 60
#define MAX_SIM_STEPS 4000000
#define STEP_SECONDS (1.0f / 60.0f)

typedef struct {
    int col;
    int row;
    int score; /* how many road cells this cell covers */
} Spot;

static Spot g_spots[MAX_SPOTS];
static int g_spotCount = 0;
static int g_nextSpot = 0;
static int g_cycleIndex = 0;
static int g_mazeMode = 0;
static int g_plainMode = 0;

/* Build order: mostly the four common elemental towers, with the odd SLAG
 * (damage amplifier) and BLASTER (AoE). Repeats keep all layer types covered. */
static const int kBuildCycle[] = {
    DMG_NORMAL, DMG_FIRE, DMG_ELECTRIC, DMG_CORROSIVE,
    DMG_FIRE, DMG_CORROSIVE, DMG_EXPLOSIVE, DMG_NORMAL,
    DMG_ELECTRIC, DMG_SLAG, DMG_NORMAL, DMG_FIRE
};

static int next_build_type(void)
{
    int count = (int)(sizeof kBuildCycle / sizeof kBuildCycle[0]);
    int type = kBuildCycle[g_cycleIndex % count];
    ++g_cycleIndex;
    return type;
}

/* Reset the AI build cycle between runs (main is one run per process anyway). */
static void reset_ai(void)
{
    g_nextSpot = 0;
    g_cycleIndex = 0;
}

/* Greedy "good spot" heuristic: how many CURRENT road cells are in range.
 * Road cells themselves are skipped here; --maze plants those separately. */
static void collect_spots(const Game *g)
{
    float rangeSq = TOWER_RANGE_CELLS * TOWER_RANGE_CELLS;
    int row;
    int col;

    g_spotCount = 0;
    for (row = 0; row < GRID_ROWS; ++row) {
        for (col = 0; col < GRID_COLS; ++col) {
            int score = 0;
            int i;

            if (core_path_contains(g, col, row)) {
                continue;
            }
            if (!game_can_place_at(g, col, row, 0)) {
                continue;
            }
            for (i = 0; i < g->path.count; ++i) {
                float dx = g->path.pts[i].x - ((float)col + 0.5f);
                float dy = g->path.pts[i].y - ((float)row + 0.5f);
                if (dx * dx + dy * dy <= rangeSq) {
                    ++score;
                }
            }
            if (score > 0) {
                g_spots[g_spotCount].col = col;
                g_spots[g_spotCount].row = row;
                g_spots[g_spotCount].score = score;
                ++g_spotCount;
            }
        }
    }

    /* Insertion sort: higher coverage first, then row-major. Stable and small. */
    {
        int i;
        for (i = 1; i < g_spotCount; ++i) {
            Spot key = g_spots[i];
            int j = i - 1;
            while (j >= 0 && (g_spots[j].score < key.score ||
                              (g_spots[j].score == key.score &&
                               (g_spots[j].row > key.row ||
                                (g_spots[j].row == key.row &&
                                 g_spots[j].col > key.col))))) {
                g_spots[j + 1] = g_spots[j];
                --j;
            }
            g_spots[j + 1] = key;
        }
    }
}

static int total_tower_levels(const Game *g)
{
    int i;
    int sum = 0;
    for (i = 0; i < g->towerCount; ++i) {
        if (g->towers[i].active) {
            sum += g->towers[i].level;
        }
    }
    return sum;
}

/* Try to place a tower of `type` at (col,row); returns true on success. */
static bool try_place(Game *g, int col, int row, int type)
{
    const TowerSpec *spec = game_tower_spec(type);
    if (!spec || g->gold < spec->cost) {
        return false;
    }
    game_set_selected_type(g, type);
    return game_place_tower(g, col, row);
}

/*
 * --maze: plant one tower on the road on the cell whose blocking makes the
 * route longest. Returns true when a wall was built.
 */
static bool maze_plant_one(Game *g)
{
    float bestGain = 0.5f; /* only build when it really lengthens the road */
    int bestCol = -1;
    int bestRow = -1;
    int i;

    for (i = 0; i < g->path.count; ++i) {
        int col = (int)(g->path.pts[i].x - 0.5f);
        int row = (int)(g->path.pts[i].y - 0.5f);
        float newLength;

        if (col == CORE_START_COL && row == CORE_START_ROW) {
            continue;
        }
        if (col == CORE_GOAL_COL && row == CORE_GOAL_ROW) {
            continue;
        }
        newLength = core_path_length_with_block(g, col, row);
        if (newLength <= g->path.length) {
            continue; /* would not lengthen the road (or would seal it) */
        }
        if (newLength - g->path.length > bestGain) {
            bestGain = newLength - g->path.length;
            bestCol = col;
            bestRow = row;
        }
    }

    if (bestCol < 0) {
        return false;
    }
    /* Mazes don't need the expensive BLASTER; use the next cheap mixed type. */
    {
        int type = next_build_type();
        while (game_tower_spec(type) && game_tower_spec(type)->cost > g->gold) {
            type = next_build_type();
        }
        return try_place(g, bestCol, bestRow, type);
    }
}

static void ai_build_phase(Game *g)
{
    int guard;

    if (g_mazeMode) {
        for (guard = 0; guard < 400; ++guard) {
            if (g->gold < 15 || g->towerCount >= MAZE_WALL_TARGET) {
                break;
            }
            if (!maze_plant_one(g)) {
                break;
            }
        }
    }

    /* Spread normal towers over the board, never on the current road. */
    for (guard = 0; guard < MAX_SPOTS; ++guard) {
        int col;
        int row;

        if (g->towerCount >= SUPPORT_TOWER_TARGET || g->gold < 15 ||
            g_nextSpot >= g_spotCount) {
            break;
        }
        col = g_spots[g_nextSpot].col;
        row = g_spots[g_nextSpot].row;
        if (core_path_contains(g, col, row)) {
            ++g_nextSpot; /* the maze has grown over this spot */
            continue;
        }
        {
            int type = g_plainMode ? DMG_NORMAL : next_build_type();
            if (!try_place(g, col, row, type)) {
                ++g_nextSpot;
                continue;
            }
        }
        ++g_nextSpot;
    }

    /* Then sink everything else into the cheapest available upgrade. */
    for (guard = 0; guard < 20000; ++guard) {
        int best = -1;
        int bestCost = 1 << 30;
        int i;

        for (i = 0; i < g->towerCount; ++i) {
            int cost = game_upgrade_cost(g, i);
            if (cost < 0 || cost > g->gold) {
                continue;
            }
            if (cost < bestCost ||
                (cost == bestCost && best >= 0 &&
                 g->towers[i].level < g->towers[best].level)) {
                best = i;
                bestCost = cost;
            }
        }
        if (best < 0) {
            break;
        }
        game_select_tower(g, best);
        if (!game_upgrade_selected(g)) {
            break;
        }
    }
    game_select_tower(g, -1);
    game_clear_message(g);
}

static const char *mode_name(void)
{
    if (g_plainMode) {
        return "plain-gun";
    }
    return g_mazeMode ? "maze-mixed" : "mixed";
}

int main(int argc, char **argv)
{
    Game g;
    unsigned seed = 1337u;
    int steps = 0;
    int guard;
    int lastPrintedWave = -1;
    int waveCount;
    clock_t cpuStart;
    int argi;

    for (argi = 1; argi < argc; ++argi) {
        if (strcmp(argv[argi], "--maze") == 0) {
            g_mazeMode = 1;
        } else if (strcmp(argv[argi], "--plain") == 0) {
            g_plainMode = 1;
        } else {
            seed = (unsigned)strtoul(argv[argi], NULL, 10);
            if (seed == 0u) {
                seed = 1337u;
            }
        }
    }

    reset_ai();
    game_new_run(&g, (uint32_t)seed);
    collect_spots(&g);
    waveCount = g.totalWaves;
    cpuStart = clock();

    printf("Elemental Defense - headless balance sim (v0.3)\n");
    printf("mode=%s  seed=%u  SPEED_SCALE=%.2f  support spots=%d  waves=%d\n\n",
           mode_name(), seed, (double)core_enemy_speed_scale(), g_spotCount,
           waveCount);
    printf("%4s | %5s | %5s | %5s | %5s | %6s | %4s | %3s | %3s | %3s | %3s | %5s\n",
           "wave", "gold", "lives", "kills", "leaks", "towers", "lvlsum",
           "HP", "SH", "AR", "EL", "pathLen");
    printf("%4s | %5s | %5s | %5s | %5s | %6s | %4s | %3s | %3s | %3s | %3s | %5s\n",
           "", "", "", "", "", "", "", "lay", "lay", "lay", "ite", "");

    for (guard = 0; guard < MAX_SIM_STEPS; ++guard) {
        if (g.phase == PHASE_VICTORY || g.phase == PHASE_GAMEOVER) {
            break;
        }
        if (g.phase == PHASE_BUILD) {
            int hpLayers = 0;
            int shieldLayers = 0;
            int armorLayers = 0;
            int elites = 0;

            ai_build_phase(&g);
            if (g.wave != lastPrintedWave) {
                game_wave_preview(&g, &hpLayers, &shieldLayers, &armorLayers,
                                  &elites);
                printf("%4d | %5d | %5d | %5d | %5d | %6d | %4d | %3d | %3d | %3d | %3d | %5.1f\n",
                       g.wave, g.gold, g.lives, g.kills, g.leaks,
                       g.towerCount, total_tower_levels(&g), hpLayers,
                       shieldLayers, armorLayers, elites,
                       (double)g.path.length);
                lastPrintedWave = g.wave;
            }
            game_start_next_wave(&g);
        }
        game_step(&g, STEP_SECONDS);
        ++steps;
    }

    printf("\n");
    if (g.phase == PHASE_VICTORY) {
        printf("RESULT: VICTORY - cleared all %d waves\n", waveCount);
    } else if (g.phase == PHASE_GAMEOVER) {
        printf("RESULT: GAMEOVER - died on wave %d\n", g.wave);
    } else {
        printf("RESULT: TIMEOUT after %d steps (phase %d)\n", steps,
               (int)g.phase);
    }

    {
        int towerCount = g.towerCount;
        int levels = total_tower_levels(&g);
        double simSeconds = (double)g.timeTotal;
        double cpuSeconds = (double)(clock() - cpuStart) / (double)CLOCKS_PER_SEC;
        int typeCounts[TOWER_TYPE_COUNT] = { 0, 0, 0, 0, 0, 0 };
        int i;

        for (i = 0; i < towerCount; ++i) {
            if (g.towers[i].active &&
                g.towers[i].damageType < TOWER_TYPE_COUNT) {
                ++typeCounts[g.towers[i].damageType];
            }
        }

        printf("waves reached : %d/%d\n", (g.wave > waveCount) ? waveCount
                                                                : g.wave,
               waveCount);
        printf("gold          : %d\n", g.gold);
        printf("lives         : %d/%d\n", g.lives, STARTING_LIVES);
        printf("kills/leaks   : %d / %d\n", g.kills, g.leaks);
        printf("towers        : %d (level sum %d)\n", towerCount, levels);
        printf("army          : GUN %d, FIRE %d, SHOCK %d, CORROSIVE %d, "
               "SLAG %d, BLASTER %d\n",
               typeCounts[DMG_NORMAL], typeCounts[DMG_FIRE],
               typeCounts[DMG_ELECTRIC], typeCounts[DMG_CORROSIVE],
               typeCounts[DMG_SLAG], typeCounts[DMG_EXPLOSIVE]);
        printf("final road    : %.0f cells\n", (double)g.path.length);
        printf("sim time      : %.1f s over %d fixed steps\n", simSeconds,
               steps);
        printf("host cpu time : %.2f s\n", cpuSeconds);
    }
    return 0;
}
