/*
 * Elemental Defense - grid and the maze road.
 *
 * The road is NOT fixed: this is the maze-building game. Exactly like the LÖVE
 * original (main.lua:63-123), a 4-connected breadth-first search from the
 * entrance (0,12) to the exit (25,12) runs whenever the board changes. Towers
 * may stand on the road, and a tower that would make the exit unreachable is a
 * refused placement - so the player builds walls on the road to force the
 * enemies around a longer detour. The BFS is nearest-cells-first and expands
 * neighbours up, down, left, right in that order, matching the original, which
 * also makes the result deterministic (ties resolve the same way every time).
 *
 * The found cells are stored as cell centres in g->path; path.length is
 * count-1 cells. game.c re-projects live enemies onto the new polyline after
 * every successful build/sell so they keep walking from where they stand
 * instead of snapping back to the entrance.
 */
#include <string.h>

#include "game.h"
#include "core_internal.h"

#define CELL_INDEX(col, row) ((row) * GRID_COLS + (col))
#define CELL_COL(index)      ((index) % GRID_COLS)
#define CELL_ROW(index)      ((index) / GRID_COLS)
#define CELL_COUNT           (GRID_COLS * GRID_ROWS)

#define NO_BLOCK (-1)

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

/* A cell is a wall when a tower stands there, or when the caller asks us to
 * pretend one does (that is how placement legality / the maze AI are checked
 * without mutating the board). */
static bool cell_is_wall(const Game *g, int col, int row,
                         int blockCol, int blockRow)
{
    if (col == blockCol && row == blockRow) {
        return true;
    }
    return g->occupancy[row][col] != 0;
}

/*
 * Breadth-first search from the entrance to the exit. Returns the number of
 * cells on the shortest route (0 when the exit is unreachable). When outPath is
 * non-NULL the route is written into it, entrance first.
 */
static int bfs_route(const Game *g, int blockCol, int blockRow, Path *outPath)
{
    int8_t visited[GRID_ROWS][GRID_COLS];
    int16_t parent[CELL_COUNT];
    int16_t queue[CELL_COUNT];
    int head;
    int tail;
    int start;
    int goal;
    int found;
    int i;

    for (i = 0; i < CELL_COUNT; ++i) {
        parent[i] = -1;
    }
    memset(visited, 0, sizeof visited);

    start = CELL_INDEX(CORE_START_COL, CORE_START_ROW);
    goal = CELL_INDEX(CORE_GOAL_COL, CORE_GOAL_ROW);

    head = 0;
    tail = 0;
    queue[tail++] = (int16_t)start;
    visited[CORE_START_ROW][CORE_START_COL] = 1;
    found = 0;

    while (head < tail) {
        int16_t current = queue[head++];
        int col;
        int row;
        int n;

        if (current == goal) {
            found = 1;
            break;
        }

        col = CELL_COL(current);
        row = CELL_ROW(current);

        /* Same expansion order as main.lua:84-89: up, down, left, right. */
        for (n = 0; n < 4; ++n) {
            int nc = col;
            int nr = row;

            if (n == 0) nr = row - 1;
            else if (n == 1) nr = row + 1;
            else if (n == 2) nc = col - 1;
            else nc = col + 1;

            if (nc < 0 || nc >= GRID_COLS || nr < 0 || nr >= GRID_ROWS) {
                continue;
            }
            if (visited[nr][nc]) {
                continue;
            }
            if (cell_is_wall(g, nc, nr, blockCol, blockRow)) {
                continue;
            }

            visited[nr][nc] = 1;
            parent[CELL_INDEX(nc, nr)] = current;
            queue[tail++] = (int16_t)CELL_INDEX(nc, nr);
        }
    }

    if (!found) {
        return 0;
    }

    /* Walk parents back from the goal; that gives goal -> start, so fill the
     * output from the back. */
    {
        int16_t walk = (int16_t)goal;
        int count = 0;

        while (walk >= 0 && count < MAX_PATH_CELLS) {
            ++count;
            if (walk == start) {
                break;
            }
            walk = parent[walk];
        }
        if (walk != start || count > MAX_PATH_CELLS) {
            return 0; /* cannot happen on a valid board, but do not trust it */
        }

        if (outPath) {
            walk = (int16_t)goal;
            outPath->count = count;
            for (i = count - 1; i >= 0; --i) {
                outPath->pts[i] = game_cell_center(CELL_COL(walk), CELL_ROW(walk));
                if (walk == start) {
                    break;
                }
                walk = parent[walk];
            }
            outPath->length = (float)(count - 1);
        }
        return count;
    }
}

int core_build_path(Game *g)
{
    int count;

    if (!g) {
        return 0;
    }

    count = bfs_route(g, NO_BLOCK, NO_BLOCK, &g->path);
    if (count < 2) {
        /* Only possible if the board was changed behind the core's back. Keep
         * the game renderable instead of pretending there is a road. */
        g->path.count = 0;
        g->path.length = 0.0f;
        g->pathBlocked = true;
        return 0;
    }
    g->pathBlocked = false;
    return count;
}

bool core_path_exists(const Game *g, int blockCol, int blockRow)
{
    if (!g) {
        return false;
    }
    /* The entrance and the exit can never be built on, so never "blocked". */
    if (blockCol == CORE_START_COL && blockRow == CORE_START_ROW) {
        return false;
    }
    if (blockCol == CORE_GOAL_COL && blockRow == CORE_GOAL_ROW) {
        return false;
    }
    if (blockCol < 0 || blockCol >= GRID_COLS || blockRow < 0 ||
        blockRow >= GRID_ROWS) {
        return bfs_route(g, NO_BLOCK, NO_BLOCK, NULL) >= 2;
    }
    return bfs_route(g, blockCol, blockRow, NULL) >= 2;
}

float core_path_length_with_block(const Game *g, int blockCol, int blockRow)
{
    int count;

    if (!g) {
        return -1.0f;
    }
    count = bfs_route(g, blockCol, blockRow, NULL);
    return (count >= 2) ? (float)(count - 1) : -1.0f;
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

static bool is_entrance_or_exit(int col, int row)
{
    return (col == CORE_START_COL && row == CORE_START_ROW) ||
           (col == CORE_GOAL_COL && row == CORE_GOAL_ROW);
}

/*
 * Placement legality only: bounds, entrance/exit, occupancy, tower cap and the
 * maze rule (a build may never seal the exit). It does NOT look at gold, so the
 * UI can distinguish "nothing can go here" (red tile) from "not affordable"
 * (message on tap). game_place_tower() checks gold.
 *
 * The reachability test runs a BFS with the candidate cell treated as a wall;
 * the real board is not touched, so this is safe to call for the cursor every
 * frame.
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
    if (is_entrance_or_exit(col, row)) {
        return false;
    }
    if (g->occupancy[row][col] != 0) {
        return false;
    }
    if (g->towerCount >= MAX_TOWERS) {
        return false;
    }
    return core_path_exists(g, col, row);
}
