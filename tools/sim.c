/*
 * Elemental Defense - headless auto-play / balance sim (v0.7: 1000 waves).
 *
 * Drives the real portable core (no rendering, no platform code) with a simple
 * greedy scripted player and prints a compact campaign report: one row every
 * 100 waves, the wave-time curve, and a final verdict/summary.
 * This is the proof that the game loop is actually playable end to end.
 *
 * Player styles:
 *
 *   default / mixed - towers beside the road only (never on it), cycling all six
 *                     damage types so the army covers HP / SHIELD / ARMOR, then
 *                     upgrades. The road stays at its initial 25 segments.
 *                     It also keeps expanding the tower count as income allows
 *                     (the board has no tower cap any more), up to the number of
 *                     cells that actually cover the road.
 *   --maze          - same mixed army, but also spends gold planting towers ON
 *                     the road, greedily choosing the cell whose blocking
 *                     lengthens the route the most (the intended play style);
 *                     a longer road also feeds more towers.
 *   --plain         - GUN towers only (neutral 1.0x vs every layer): the
 *                     baseline that ignores the element system.
 *
 * Usage:  sim.exe [--maze] [--plain] [--1x] [seed]
 *   Default speed is 4x internally (steps cost 4 game-seconds each) so a whole
 *   1000-wave run takes a few seconds of host CPU; wave durations are reported
 *   in 1x game seconds either way.
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
#define MAX_SUPPORT_TARGET 336 /* buildable cells: 338 - entrance - exit */
#define MAZE_WALL_TARGET 60
#define MAX_SIM_STEPS 8000000
#define STEP_SECONDS (1.0f / 60.0f)
#define MAX_WAVES 1001

typedef struct {
    int col;
    int row;
    int score; /* how many road cells this cell covers */
} Spot;

static Spot g_spots[MAX_SPOTS];
static int g_spotCount = 0;
static int g_cycleIndex = 0;
static int g_mazeMode = 0;
static int g_plainMode = 0;
static int g_oneX = 0;
static float g_waveTimes[MAX_WAVES]; /* 1-based, seconds at 1x game speed */
static int g_wavesCompleted = 0;

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

/* Reset the AI state between runs (main is one run per process anyway). */
static void reset_ai(void)
{
    g_cycleIndex = 0;
    g_spotCount = 0;
    memset(g_waveTimes, 0, sizeof g_waveTimes);
    g_wavesCompleted = 0;
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

/* Support-tower budget: start with a solid 48-tower army, then let the AI
 * expand as waves (and income) grow. There is no tower cap in the core; the
 * board coverage is the real limit. */
static int support_target(const Game *g)
{
    int target = 48 + g->wave / 2;
    if (target > MAX_SUPPORT_TARGET) {
        target = MAX_SUPPORT_TARGET;
    }
    return target;
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

/* Build support towers on the freshly re-collected road-coverage spots. */
static void ai_build_support(Game *g)
{
    int target = support_target(g);
    int i;

    collect_spots(g);
    for (i = 0; i < g_spotCount && g->towerCount < target; ++i) {
        int col = g_spots[i].col;
        int row = g_spots[i].row;
        int type = g_plainMode ? DMG_NORMAL : next_build_type();
        const TowerSpec *spec = game_tower_spec(type);

        if (!spec || g->gold < spec->cost) {
            break; /* cheapest affordable type is unknown: stop this pass */
        }
        try_place(g, col, row, type);
    }
}

static void ai_upgrade(Game *g)
{
    int guard;

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
    ai_build_support(g);
    ai_upgrade(g);
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

static void print_row(const Game *g, float waveSeconds)
{
    int hpLayers = 0;
    int shieldLayers = 0;
    int armorLayers = 0;
    int elites = 0;
    char waveSecondsText[16];

    game_wave_preview(g, &hpLayers, &shieldLayers, &armorLayers, &elites);
    if (waveSeconds < 0.0f) {
        strcpy(waveSecondsText, "  -");
    } else {
        snprintf(waveSecondsText, sizeof waveSecondsText, "%6.1f",
                 (double)waveSeconds);
    }
    printf("%4d | %6d | %5d | %6d | %5d | %6d | %5d | %5.1f | %4d | %3d | "
           "%3d | %3d | %s\n",
           g->wave, g->gold, g->lives, g->kills, g->leaks, g->towerCount,
           total_tower_levels(g), (double)g->path.length, hpLayers,
           shieldLayers, armorLayers, elites, waveSecondsText);
}

static void print_curve(void)
{
    int wave;

    printf("\nwave-time curve (1x game seconds, every 100 waves):\n");
    printf("%6s | %9s\n", "wave", "duration");
    for (wave = 1; wave <= g_wavesCompleted; ++wave) {
        if (wave == 1 || wave == g_wavesCompleted || wave % 100 == 0) {
            printf("%6d | %9.2f\n", wave, (double)g_waveTimes[wave]);
        }
    }
}

static void find_wave_time_extremes(int *peakWave, float *peakTime,
                                    float *meanTime)
{
    int wave;
    int peak = 1;
    float total = 0.0f;
    int count = 0;

    for (wave = 1; wave <= g_wavesCompleted; ++wave) {
        if (g_waveTimes[wave] > g_waveTimes[peak]) {
            peak = wave;
        }
        total += g_waveTimes[wave];
        ++count;
    }
    *peakWave = (count > 0) ? peak : 0;
    *peakTime = g_waveTimes[(count > 0) ? peak : 0];
    *meanTime = (count > 0) ? (total / (float)count) : 0.0f;
}

int main(int argc, char **argv)
{
    Game g;
    unsigned seed = 1337u;
    int steps = 0;
    int guard;
    int printedWave = 0;
    int waveCount;
    int peakWave = 0;
    float peakTime = 0.0f;
    float meanTime = 0.0f;
    clock_t cpuStart;
    int argi;

    for (argi = 1; argi < argc; ++argi) {
        if (strcmp(argv[argi], "--maze") == 0) {
            g_mazeMode = 1;
        } else if (strcmp(argv[argi], "--plain") == 0) {
            g_plainMode = 1;
        } else if (strcmp(argv[argi], "--1x") == 0) {
            g_oneX = 1;
        } else {
            seed = (unsigned)strtoul(argv[argi], NULL, 10);
            if (seed == 0u) {
                seed = 1337u;
            }
        }
    }

    reset_ai();
    game_new_run(&g, (uint32_t)seed);
    if (!g_oneX) {
        game_cycle_speed(&g);
        game_cycle_speed(&g); /* 4x internally; wave times stay 1x game time */
    }
    waveCount = g.totalWaves;
    cpuStart = clock();

    printf("Elemental Defense - headless balance sim (v0.7: %d waves)\n",
           waveCount);
    printf("mode=%s  seed=%u  SPEED_SCALE=%.2f  sim_speed=%dx  waves=%d\n\n",
           mode_name(), seed, (double)core_enemy_speed_scale(),
           (int)g.speedMultiplier, waveCount);
    printf("%4s | %6s | %5s | %6s | %5s | %6s | %5s | %5s | %4s | %3s | "
           "%3s | %3s | %6s\n",
           "wave", "gold", "lives", "kills", "leaks", "towers", "lvlsum",
           "path", "HPlay", "SH", "AR", "EL", "waveS");
    printf("%4s | %6s | %5s | %6s | %5s | %6s | %5s | %5s | %4s | %3s | "
           "%3s | %3s | %6s\n",
           "", "", "", "", "", "", "", "", "lay", "lay", "lay", "ite", "");

    for (guard = 0; guard < MAX_SIM_STEPS; ++guard) {
        if (g.phase == PHASE_VICTORY || g.phase == PHASE_GAMEOVER) {
            break;
        }
        if (g.phase == PHASE_BUILD) {
            /* The wave that just ended is g.wave-1; its length is waveTime. */
            if (g.wave > 1 && g_wavesCompleted < g.wave - 1) {
                g_waveTimes[g.wave - 1] = g.waveTime;
                g_wavesCompleted = g.wave - 1;
            }
            ai_build_phase(&g);
            if (g.wave == 1 || g.wave % 100 == 0 || g.wave == waveCount) {
                if (g.wave != printedWave) {
                    print_row(&g, (g.wave > 1) ? g.waveTime : -1.0f);
                    printedWave = g.wave;
                }
            }
            game_start_next_wave(&g);
        }
        game_step(&g, STEP_SECONDS);
        ++steps;
    }

    if (g.phase == PHASE_VICTORY) {
        g_waveTimes[waveCount] = g.waveTime;
        g_wavesCompleted = waveCount;
    }
    find_wave_time_extremes(&peakWave, &peakTime, &meanTime);

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
        double cpuSeconds = (double)(clock() - cpuStart) /
                            (double)CLOCKS_PER_SEC;
        int typeCounts[TOWER_TYPE_COUNT] = { 0, 0, 0, 0, 0, 0 };
        int i;

        for (i = 0; i < towerCount; ++i) {
            if (g.towers[i].active &&
                g.towers[i].damageType < TOWER_TYPE_COUNT) {
                ++typeCounts[g.towers[i].damageType];
            }
        }

        printf("waves reached : %d/%d\n",
               (g.wave > waveCount) ? waveCount : g.wave, waveCount);
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
        printf("peak wave time: wave %d, %.2f s at 1x\n", peakWave,
               (double)peakTime);
        printf("mean wave time: %.2f s at 1x\n", (double)meanTime);
        printf("sim time      : %.1f s over %d fixed steps\n", simSeconds,
               steps);
        printf("host cpu time : %.2f s\n", cpuSeconds);
    }

    print_curve();
    return 0;
}
