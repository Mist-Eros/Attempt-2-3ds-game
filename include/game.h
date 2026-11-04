/*
 * Elemental Defense - portable game core API
 *
 * This header is the contract between the platform-independent simulation
 * (source/core/, built and unit-tested on a PC) and the 3DS front end
 * (source/*.c, built with devkitARM + citro2d).
 *
 * Rules for the core:
 *   - never include <3ds.h> or any citro2d/citro3d header here
 *   - keep everything in "cell space": 1.0 == one grid cell. The renderer
 *     scales cell space to pixels and knows nothing about simulation rules.
 *   - no dynamic allocation, no libc math beyond sinf/cosf/sqrtf/fabsf
 *
 * Coordinates: col 0..GRID_COLS-1 (left to right), row 0..GRID_ROWS-1 (top to
 * bottom). Cell (col,row) center is (col + 0.5, row + 0.5).
 */
#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <stdint.h>

#define GRID_COLS 26
#define GRID_ROWS 13
#define MAX_PATH_CELLS (GRID_COLS * GRID_ROWS)
#define TOWER_TYPE_COUNT 3
#define TOWER_MAX_LEVEL 10

#define MAX_TOWERS 96
#define MAX_ENEMIES 128
#define MAX_BEAMS 128
#define MAX_ELITE_SPAWNS 16

#define STARTING_GOLD 150
#define STARTING_LIVES 20

#define MAX_MESSAGE_LEN 48

typedef enum {
    ELEMENT_FIRE = 0,
    ELEMENT_WATER = 1,
    ELEMENT_FOREST = 2
} Element;

typedef enum {
    PHASE_MENU = 0,     /* title screen */
    PHASE_BUILD,        /* wave not started, player builds/upgrades, taps "next wave" */
    PHASE_WAVE,         /* spawning and combat in progress */
    PHASE_GAMEOVER,     /* shown as an overlay; player can restart */
    PHASE_VICTORY       /* all waves cleared; overlay; player can restart */
} GamePhase;

typedef struct {
    float x, y;
} Vec2;

typedef struct {
    bool active;
    float r, g, b;      /* 0..1 */
} Particle;

typedef struct {
    bool active;
    uint8_t element;    /* Element */
    uint8_t level;      /* 1..TOWER_MAX_LEVEL */
    int16_t col, row;
    float cooldown;     /* seconds until the next shot */
    int invested;       /* total gold spent, for the sell price */
    float animPhase;    /* idle animation timer, renderer may use */
} Tower;

typedef struct {
    bool active;
    uint8_t element;    /* Element */
    bool elite;
    float hp, maxHp;
    float pathPos;      /* distance travelled along the path, in cells */
    float x, y;         /* cell-space position (derived from pathPos) */
    float hitFlash;     /* seconds of white flash left after damage */
} Enemy;

/* Instant-hit "laser" visual, like the original LÖVE version. */
typedef struct {
    bool active;
    uint8_t element;
    float x1, y1, x2, y2;   /* cell space */
    float life, maxLife;    /* seconds */
} Beam;

typedef struct {
    Vec2 pts[MAX_PATH_CELLS];
    int count;          /* number of grid cells in the path */
    float length;       /* path length in cells (count - 1) */
} Path;

typedef struct {
    GamePhase phase;
    bool paused;

    /* run state */
    int gold;
    int lives;
    int wave;               /* 1-based, == totalWaves after the last one */
    int totalWaves;
    int enemiesAlive;
    int enemiesToSpawn;     /* still queued for this wave */
    int kills;
    int leaks;
    float waveTime;
    float speedMultiplier;   /* 1.0, 2.0 or 4.0 */
    float timeTotal;

    /* transient UI message ("not enough gold", ...) */
    char message[MAX_MESSAGE_LEN];
    float messageTimer;

    /* board */
    Tower towers[MAX_TOWERS];
    int towerCount;
    int8_t occupancy[GRID_ROWS][GRID_COLS];  /* 0 = empty, else tower index + 1 */
    Path path;
    bool pathBlocked;       /* set if a placement would make the goal unreachable */

    /* dynamic entities */
    Enemy enemies[MAX_ENEMIES];
    Beam beams[MAX_BEAMS];

    /* wave spawning internals */
    float spawnTimer;
    float spawnInterval;
    int elitePending;
    float eliteDelays[MAX_ELITE_SPAWNS];
    int eliteDelayCount;

    /* player selection / controls */
    int selectedType;       /* tower type selected in the build palette */
    int selectedTower;      /* tower index or -1 */
    int cursorCol, cursorRow;   /* D-pad cursor */

    /* audio / feedback events, see GAME_EVT_*; platform clears them */
    uint32_t events;

    uint32_t rng;           /* deterministic xorshift state */
} Game;

/* ---- content tables (implemented in the core) ---- */
typedef struct {
    const char *name;
    int cost;
    float damage;       /* level 1 */
    float fireRate;     /* seconds between shots, level 1 */
    float range;        /* cells, level 1 */
    float color[3];
    float glow[3];
} TowerSpec;

typedef struct {
    const char *name;
    float color[3];
    float glow[3];
} EnemySpec;

/* bits for Game.events */
enum {
    GAME_EVT_BUILD      = 1u << 0,
    GAME_EVT_SELL       = 1u << 1,
    GAME_EVT_UPGRADE    = 1u << 2,
    GAME_EVT_LASER      = 1u << 3,
    GAME_EVT_ENEMY_DIE  = 1u << 4,
    GAME_EVT_WAVE_START = 1u << 5,
    GAME_EVT_GAMEOVER   = 1u << 6,
    GAME_EVT_VICTORY    = 1u << 7,
    GAME_EVT_LEAK       = 1u << 8
};

/* ---- lifecycle ---- */
void game_init(Game *g);                     /* clear to PHASE_MENU */
void game_new_run(Game *g, uint32_t seed);   /* fresh run in PHASE_BUILD */
void game_update(Game *g, float dt);         /* real-time entry, fixed-steps internally */
void game_step(Game *g, float dt);           /* exactly one simulation step (tests) */

/* ---- player actions; return false and set g->message when invalid ---- */
bool game_place_tower(Game *g, int col, int row);          /* uses g->selectedType */
bool game_upgrade_selected(Game *g);
bool game_sell_selected(Game *g);
void game_start_next_wave(Game *g);                        /* PHASE_BUILD -> PHASE_WAVE */
void game_set_selected_type(Game *g, int type);
void game_select_tower(Game *g, int towerIndex);           /* -1 clears */
void game_set_cursor(Game *g, int col, int row);
void game_toggle_pause(Game *g);
void game_cycle_speed(Game *g);                            /* 1x -> 2x -> 4x -> 1x */
void game_clear_events(Game *g);
void game_clear_message(Game *g);

/* ---- queries for UI / renderer ---- */
const TowerSpec *game_tower_spec(int type);
const EnemySpec *game_enemy_spec(int element);

/* effective stats at a given level (level clamped 1..TOWER_MAX_LEVEL) */
void game_tower_stats(int type, int level,
                      float *damage, float *fireRate, float *range);

int game_upgrade_cost(const Game *g, int towerIndex);   /* -1 if maxed */
int game_sell_value(const Game *g, int towerIndex);
int game_tower_at(const Game *g, int col, int row);     /* tower index or -1 */
bool game_can_place_at(const Game *g, int col, int row, int type);

/* what the upcoming wave will throw at you (for the bottom-screen preview) */
void game_wave_preview(const Game *g, int *fire, int *water, int *forest, int *elite);

float game_path_length(const Game *g);      /* in cells */
Vec2 game_cell_center(int col, int row);    /* cell-space center */
int game_selected_upgrade_cost(const Game *g);
int game_selected_sell_value(const Game *g);

#endif /* GAME_H */
