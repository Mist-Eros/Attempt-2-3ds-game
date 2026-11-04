/*
 * Elemental Defense - portable game core API
 *
 * This header is the contract between the platform-independent simulation
 * (source/core/, built and unit-tested on a PC) and the 3DS front end
 * (source / main.c, plat_3ds.c, built with devkitARM + citro2d).
 *
 * Rules for the core:
 *   - never include <3ds.h> or any citro2d/citro3d header here
 *   - keep everything in "cell space": 1.0 == one grid cell. The renderer
 *     scales cell space to pixels and knows nothing about simulation rules.
 *   - no dynamic allocation, no libc math beyond sinf/cosf/sqrtf/fabsf
 *
 * Coordinates: col 0..GRID_COLS-1 (left to right), row 0..GRID_ROWS-1 (top to
 * bottom). Cell (col,row) center is (col + 0.5, row + 0.5).
 *
 * v0.3 combat model (Borderlands 2 style):
 *   - enemies are stacks of 1..3 concentric layers, each layer is one of
 *     HP (red), SHIELD (blue) or ARMOR (yellow); layers are destroyed outside-in
 *   - towers deal one of six damage types: normal, fire, electric, corrosive,
 *     slag, explosive; each type has its own multipliers vs layer types and
 *     its own status effect
 */
#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <stdint.h>

#define GRID_COLS 26
#define GRID_ROWS 13
#define MAX_PATH_CELLS (GRID_COLS * GRID_ROWS)

/* one per damage type below */
#define TOWER_TYPE_COUNT 6
#define TOWER_MAX_LEVEL 10

#define MAX_LAYERS 3
#define MAX_PUDDLES 16

/*
 * No tower cap: the 26x13 board has 338 cells (336 buildable - the entrance and
 * exit are reserved), so 400 is a pure storage bound the player can never hit.
 * The compile-time check below keeps MAX_TOWERS strictly above the board size.
 */
#define MAX_TOWERS 400
typedef char game_max_towers_exceeds_board[
    (MAX_TOWERS > GRID_ROWS * GRID_COLS) ? 1 : -1];
#define MAX_ENEMIES 128
#define MAX_BEAMS 128
#define MAX_ELITE_SPAWNS 16

#define STARTING_GOLD 150
#define STARTING_LIVES 20

#define MAX_MESSAGE_LEN 48

/* what a tower's shots are made of */
typedef enum {
    DMG_NORMAL = 0,     /* same damage to every layer type */
    DMG_FIRE,           /* strong vs HP, weak otherwise; stacking burn DoT */
    DMG_ELECTRIC,       /* strong vs SHIELD, weak otherwise; brief stun */
    DMG_CORROSIVE,      /* strong vs ARMOR, weak otherwise; acid puddle on the ground */
    DMG_SLAG,           /* low damage; target takes +50% damage and is slowed */
    DMG_EXPLOSIVE       /* normal damage to every layer, but AoE and 3x slower */
} DamageType;

typedef enum {
    LAYER_HP = 0,       /* red   */
    LAYER_SHIELD,       /* blue  */
    LAYER_ARMOR         /* yellow */
} LayerType;

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
    uint8_t damageType; /* DamageType */
    uint8_t level;      /* 1..TOWER_MAX_LEVEL */
    int16_t col, row;
    float cooldown;     /* seconds until the next shot */
    int invested;       /* total gold spent, for the sell price */
    float animPhase;    /* idle animation timer, renderer may use */
} Tower;

/* one concentric ring of an enemy */
typedef struct {
    uint8_t type;       /* LayerType */
    float hp;
    float maxHp;
} EnemyLayer;

typedef struct {
    bool active;
    bool elite;
    uint8_t layerCount;             /* 1..MAX_LAYERS, [0] is the outermost */
    EnemyLayer layers[MAX_LAYERS];
    float pathPos;                  /* distance travelled along the path, in cells */
    float x, y;                     /* cell-space position (derived from pathPos) */
    float hitFlash;                 /* seconds of white flash left after damage */

    /* status effects */
    uint8_t burnStacks;             /* fire: 0..CORE_BURN_MAX_STACKS */
    float burnTimer;
    float stunTimer;                /* electric: movement fully stopped */
    float slagTimer;                /* slag: +damage taken, slower movement */
} Enemy;

/*
 * Instant-hit shot visual. kind 0 = beam from tower to target, kind 1 =
 * explosion ring centred on (x2,y2) (explosive towers only).
 */
typedef struct {
    bool active;
    uint8_t damageType;             /* DamageType */
    uint8_t kind;
    float x1, y1, x2, y2;           /* cell space */
    float radius;                   /* explosion radius for kind 1, cells */
    float life, maxLife;            /* seconds */
} Beam;

/* corrosive acid puddle on the ground */
typedef struct {
    bool active;
    float x, y;                     /* cell space centre */
    float radius;                   /* cells */
    float life, maxLife;
    float dps;
} Puddle;

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
    /*
     * 0 = empty, else tower index + 1. int16_t because the board can hold more
     * than 127 towers (the old int8_t would wrap and corrupt the maze BFS).
     */
    int16_t occupancy[GRID_ROWS][GRID_COLS];
    Path path;
    bool pathBlocked;       /* defensive only: live board has no route; placement
                             * rejections are pre-checked in game_can_place_at() */

    /* dynamic entities */
    Enemy enemies[MAX_ENEMIES];
    Beam beams[MAX_BEAMS];
    Puddle puddles[MAX_PUDDLES];
    int puddleNext;                         /* round-robin replacement slot */

    /* wave spawning internals */
    float spawnTimer;
    float spawnInterval;
    int elitePending;
    float eliteDelays[MAX_ELITE_SPAWNS];
    int eliteDelayCount;

    /* player selection / controls */
    int selectedType;       /* damage-type tower selected in the build palette */
    int selectedTower;      /* tower index or -1 */
    int cursorCol, cursorRow;   /* D-pad cursor */

    /* audio / feedback events, see GAME_EVT_*; platform clears them */
    uint32_t events;

    uint32_t rng;           /* deterministic xorshift state */

    /*
     * Leftover real time that game_update() has not turned into fixed steps
     * yet. Kept in the struct (instead of a static) so two Game instances in
     * the same process never share simulation state - the host tests rely on
     * that to compare two runs side by side.
     */
    float stepAccumulator;

    /*
     * Remaining layer-type quota for the wave being spawned (indexed by
     * LayerType). Waves spawn an exact number of HP / SHIELD / ARMOR layers
     * instead of pure random rolls, so the bottom-screen preview is truthful;
     * the spawn order is still shuffled by the RNG.
     */
    uint8_t layerQuota[3];
} Game;

/* ---- content table (implemented in the core) ---- */
typedef struct {
    const char *name;
    int cost;
    float damage;       /* level 1 */
    float fireRate;     /* seconds between shots, level 1 */
    float range;        /* cells, level 1 */
    uint8_t damageType; /* DamageType */
    float aoeRadius;    /* cells; 0 = single target */
    float color[3];
    float glow[3];
} TowerSpec;

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

/* effective stats at a given level (level clamped 1..TOWER_MAX_LEVEL) */
void game_tower_stats(int type, int level,
                      float *damage, float *fireRate, float *range);

int game_upgrade_cost(const Game *g, int towerIndex);   /* -1 if maxed */
int game_sell_value(const Game *g, int towerIndex);
int game_tower_at(const Game *g, int col, int row);     /* tower index or -1 */
bool game_can_place_at(const Game *g, int col, int row, int type);

/*
 * What the upcoming wave will throw at you: total number of HP / SHIELD /
 * ARMOR layers across all spawns (elites included), plus the elite count.
 */
void game_wave_preview(const Game *g, int *hpLayers, int *shieldLayers,
                       int *armorLayers, int *elites);

/*
 * Damage multiplier of a damage type against a layer type. 2.0 = strong,
 * 1.0 = neutral, 0.5 = weak. Out-of-range values are clamped to 0.
 */
float game_damage_multiplier(int damageType, int layerType);

float game_path_length(const Game *g);      /* in cells */
Vec2 game_cell_center(int col, int row);    /* cell-space center */
int game_selected_upgrade_cost(const Game *g);
int game_selected_sell_value(const Game *g);

#endif /* GAME_H */
