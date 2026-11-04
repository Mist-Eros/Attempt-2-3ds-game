/*
 * Elemental Defense - private interface shared by the core .c files.
 *
 * Nothing in here is part of the frozen public contract in include/game.h.
 * The 3DS front end must never include this header: it only sees game.h.
 */
#ifndef CORE_INTERNAL_H
#define CORE_INTERNAL_H

#include "game.h"

/* ---- simulation constants (not content, just plumbing) ---- */
#define CORE_FIXED_STEP          (1.0f / 60.0f) /* seconds per fixed simulation step */
#define CORE_MAX_STEPS_PER_UPDATE 6             /* game_update() catch-up budget */
#define CORE_MAX_UPDATE_DT       0.25f          /* real time clamp handed to game_update() */
#define CORE_MAX_STEP_DT         0.25f          /* defensive clamp inside game_step() */
#define CORE_MESSAGE_TIME        1.5f           /* seconds a UI message stays up */
#define CORE_BEAM_LIFE           0.15f          /* laser beam visual lifetime */
#define CORE_HIT_FLASH_TIME      0.12f          /* enemy white-flash on hit */

/* Content constants that the original game hard-coded. */
#define CORE_TOTAL_WAVES         25             /* original was endless; 25 = boss wave */
#define CORE_KILL_REWARD         10             /* gold per kill, elite or not (main.lua:763) */

/* The maze entrance and exit. Towers may stand on the road everywhere else. */
#define CORE_START_COL           0
#define CORE_START_ROW           12
#define CORE_GOAL_COL            25
#define CORE_GOAL_ROW            12

/* ---- data.c: content tables and formulas ---- */

/* Wave composition. All of these are 1-based waves. */
int   core_wave_base_enemies(int wave);       /* 3 + wave*2, before elites */
int   core_wave_elite_count(int wave);        /* 10 on %25, 3 on %10, 1 on %5, else 0 */
int   core_wave_total_spawns(int wave);       /* wave size including elites */
float core_wave_spawn_interval(int wave);     /* max(0.8, 3.0 - wave*0.05) */
float core_enemy_hp(int wave, bool elite);    /* (15 + (wave-1)*3) * 2.5 if elite */
float core_enemy_speed(int wave, bool elite); /* original "speed" units, *1.8 if elite */

/*
 * Enemy movement speed in CELLS PER SECOND. Deliberate deviation from the
 * LÖVE original: the original did t += dt*speed/100 with t normalised over the
 * whole path, so a longer (mazed) path did not buy the towers any extra time.
 * Here speed is absolute, so every extra cell of detour is extra tower time -
 * which is the whole point of the maze mechanic. SPEED_SCALE (data.c) is the
 * single tuning knob; see NOTES.md.
 */
float core_enemy_speed_cells(int wave, bool elite);
float core_enemy_speed_scale(void);

/* Cost in gold to take a tower from `level` to `level+1`; -1 when maxed. */
int core_upgrade_cost_for_level(int level);

/* ---- grid.c: dynamic BFS road and grid queries ---- */

/*
 * Rebuild g->path with a breadth-first search from the entrance to the exit,
 * treating every occupied cell as a wall. Returns the number of cells (0 if the
 * exit is unreachable). Placement/sell use core_path_exists() before changing
 * the board, so a build can never actually seal the route.
 */
int  core_build_path(Game *g);

/* True when the exit is still reachable if (blockCol,blockRow) also became a
 * wall. Pass -1,-1 for "just the current occupancy". Does not mutate the Game. */
bool core_path_exists(const Game *g, int blockCol, int blockRow);

/*
 * Path length in cells if (blockCol,blockRow) became a wall, without touching
 * g->path. Returns -1.0f when that would seal the exit. Used by the maze AI.
 */
float core_path_length_with_block(const Game *g, int blockCol, int blockRow);

/* True when (col,row) is one of the cells of the current g->path. */
bool core_path_contains(const Game *g, int col, int row);

/* Cell-space position at path distance `t` (0 .. path.length). */
void core_path_position(const Path *path, float t, Vec2 *out);

#endif /* CORE_INTERNAL_H */
