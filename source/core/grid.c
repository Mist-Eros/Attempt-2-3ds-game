/*
 * Elemental Defense - grid and the fixed road.
 *
 * History: the LÖVE original ran a breadth-first search after every build, so
 * blocking the path rerouted the enemies (main.lua:63-123). We deliberately
 * dropped that whole mechanic. This port has ONE hand-authored serpentine road
 * and towers can never be built on it, so the path is fixed for a whole run:
 * no re-pathing, no "path blocked" rejection state, no enemy re-mapping.
 *
 * Why a serpentine road: tower range is 60 original px = 60/28 ~ 2.14 cells.
 * A straight line across row 12 could only ever be covered by two rows of
 * towers, which makes the 26x13 board pointless. The road below snakes across
 * five rows instead, so most of the board is useful build space.
 *
 * The road (start at the left end of the top leg, goal at the right end of the
 * bottom leg), 76 cells / 75 segments:
 *
 *   row 1:  (0,1) -> (23,1)     24 cells, left to right
 *   col 23: (23,2) -> (23,5)     4 cells, down
 *   row 5:  (22,5) -> (2,5)     21 cells, right to left
 *   col 2:  (2,6) -> (2,9)       4 cells, down
 *   row 9:  (3,9) -> (25,9)     23 cells, left to right
 *
 * Every consecutive pair of cells is 4-connected, so enemies simply walk the
 * cell centres and the renderer draws the polyline through them.
 */
#include <string.h>

#include "game.h"
#include "core_internal.h"

Vec2 game_cell_center(int col, int row)
{
    Vec2 p;
    p.x = (float)col + 0.5f;
    p.y = (float)row + 0.5f;
    return p;
}

float game_path_length(const Game *g)
{
    return g ? g->path.length : 0.0f;
}

static void push_path_cell(Path *path, int *count, int col, int row)
{
    if (*count >= MAX_PATH_CELLS) {
        return;
    }
    path->pts[*count] = game_cell_center(col, row);
    ++(*count);
}

int core_build_path(Game *g)
{
    Path *path;
    int count;
    int col;
    int row;

    if (!g) {
        return 0;
    }

    path = &g->path;
    path->count = 0;
    path->length = 0.0f;
    count = 0;

    /* Top leg, left to right. */
    for (col = 0; col <= 23; ++col) {
        push_path_cell(path, &count, col, 1);
    }
    /* Down the right-hand connector. */
    for (row = 2; row <= 5; ++row) {
        push_path_cell(path, &count, 23, row);
    }
    /* Second leg, right to left. */
    for (col = 22; col >= 2; --col) {
        push_path_cell(path, &count, col, 5);
    }
    /* Down the left-hand connector. */
    for (row = 6; row <= 9; ++row) {
        push_path_cell(path, &count, 2, row);
    }
    /* Bottom leg, left to right, ending at the goal (25,9). */
    for (col = 3; col <= 25; ++col) {
        push_path_cell(path, &count, col, 9);
    }

    if (count != CORE_PATH_CELLS) {
        /* Only possible if the table above was edited without updating the
         * constant; fail closed instead of exposing a half-built road. */
        path->count = 0;
        return 0;
    }

    path->count = count;
    path->length = (float)(count - 1);
    return count;
}

bool core_path_contains(const Game *g, int col, int row)
{
    Vec2 c;
    int i;

    if (!g || g->path.count <= 0) {
        return false;
    }
    c = game_cell_center(col, row);
    for (i = 0; i < g->path.count; ++i) {
        /* Exact comparison is fine: both sides are (integer + 0.5) floats,
         * which are exactly representable for this grid size. */
        if (g->path.pts[i].x == c.x && g->path.pts[i].y == c.y) {
            return true;
        }
    }
    return false;
}

void core_path_position(const Path *path, float t, Vec2 *out)
{
    int index;
    float frac;
    Vec2 a;
    Vec2 b;

    if (!out) {
        return;
    }
    if (!path || path->count <= 0) {
        out->x = 0.0f;
        out->y = 0.0f;
        return;
    }
    if (path->count == 1 || t <= 0.0f) {
        *out = path->pts[0];
        return;
    }
    if (t >= path->length) {
        *out = path->pts[path->count - 1];
        return;
    }

    index = (int)t; /* t >= 0 so the cast is a floor */
    if (index >= path->count - 1) {
        index = path->count - 2;
    }
    frac = t - (float)index;
    a = path->pts[index];
    b = path->pts[index + 1];
    out->x = a.x + (b.x - a.x) * frac;
    out->y = a.y + (b.y - a.y) * frac;
}

int game_tower_at(const Game *g, int col, int row)
{
    int slot;

    if (!g || col < 0 || col >= GRID_COLS || row < 0 || row >= GRID_ROWS) {
        return -1;
    }
    slot = (int)(unsigned char)g->occupancy[row][col];
    return (slot > 0) ? slot - 1 : -1;
}

/*
 * Placement legality only: bounds, road, occupancy, tower cap. It does NOT
 * look at gold, so the UI can distinguish "nothing can go here" (red tile)
 * from "not affordable" (message on tap). game_place_tower() checks gold.
 */
bool game_can_place_at(const Game *g, int col, int row, int type)
{
    if (!g) {
        return false;
    }
    if (type < 0 || type >= TOWER_TYPE_COUNT) {
        return false;
    }
    if (g->phase != PHASE_BUILD && g->phase != PHASE_WAVE) {
        return false;
    }
    if (col < 0 || col >= GRID_COLS || row < 0 || row >= GRID_ROWS) {
        return false;
    }
    if (g->path.count < 2) {
        return false;
    }
    if (core_path_contains(g, col, row)) {
        return false;
    }
    if (g->occupancy[row][col] != 0) {
        return false;
    }
    if (g->towerCount >= MAX_TOWERS) {
        return false;
    }
    return true;
}
