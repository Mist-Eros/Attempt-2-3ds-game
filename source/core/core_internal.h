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

/* The fixed hand-authored road is 76 cells long (75 segments). See grid.c. */
#define CORE_PATH_CELLS          76

/* ---- data.c: content tables and formulas ---- */

/* Wave composition. All of these are 1-based waves. */
int   core_wave_base_enemies(int wave);       /* 3 + wave*2, before elites */
int   core_wave_elite_count(int wave);        /* 10 on %25, 3 on %10, 1 on %5, else 0 */
int   core_wave_total_spawns(int wave);       /* base + elites */
float core_wave_spawn_interval(int wave);     /* max(0.8, 3.0 - wave*0.05) */
float core_enemy_hp(int wave, bool elite);    /* (15 + (wave-1)*3) * 2.5 if elite */
float core_enemy_speed(int wave, bool elite); /* original "speed" units, *1.8 if elite */

/*
 * Fill out[] with the elite spawn delays of a wave (the original keeps a list
 * of per-elite delays and decrements them once per spawn attempt). Returns the
 * number written, at most max. The delay layout mirrors main.lua startWave().
 */
int core_wave_elite_delays(int wave, float *out, int max);

/* Cost in gold to take a tower from `level` to `level+1`; -1 when maxed. */
int core_upgrade_cost_for_level(int level);

/* ---- grid.c: fixed path and grid queries ---- */

/* Build the one fixed BFS path (start -> goal) into g->path. Returns cell count. */
int  core_build_path(Game *g);

/* True when (col,row) is one of the cells of g->path. */
bool core_path_contains(const Game *g, int col, int row);

/* Cell-space position at path distance `t` (0 .. path.length). */
void core_path_position(const Path *path, float t, Vec2 *out);

#endif /* CORE_INTERNAL_H */
