# Elemental Defense — 3DS Port Specification (v4)

**Single source of truth for the C implementation (devkitARM + libctru + citro2d).**

| Item | Value |
|---|---|
| Original source | `C:\Users\Raine\Desktop\ElementalDefense\game\main.lua` (LÖVE2D, 1519 lines, 51,302 bytes) — read in full |
| Original audio | `C:\Users\Raine\Desktop\ElementalDefense\game\sounds\` (16 files, §12) |
| Current implementation | `include/game.h` (frozen core API), `include/gfx.h`, `source/core/{game,grid,data}.c`, `source/{render,ui,app,main,plat_3ds,audio}.c`, `tests/test_core.c`, `tools/sim.c`, `tools/preview/*` |
| 3DS target | top 400x240 (display), bottom 320x240 (touch), fixed 1/60 s simulation |
| This document | `Attempt-2-3ds-game/docs/design.md` |

**v4 changelog (this revision):** the **dynamic BFS maze mechanic is restored** — the road is recomputed after every build and sell, towers may stand on road cells, blocked builds are refused, and walking enemies are re-projected onto the rerouted road. Enemy movement is now **absolute cells/second** (`SPEED_SCALE 0.25`), a deliberate deviation from the Lua's path-normalised speed so mazing actually buys tower time. Everything else in this revision matches the code now in the repo (`source/core/*`, `source/render.c`, `source/ui.c`, `source/app.c`, `tests/test_core.c`). v3's hand-authored serpentine road is gone; §2, §5.3, §6, §10, §11, §14, §15 and the appendices were rewritten for the maze game. All Lua-derived content numbers are unchanged and were re-verified against `main.lua`.

**Tagging convention**

- `[LUA]` — value/behaviour copied verbatim from `main.lua`.
- `[DERIVED]` — computed exactly from a `[LUA]` formula.
- `[PORT]` — a new decision required by the 3DS/engine design; all are listed in §14.2/§14.5.
- `[CODE]` — behaviour that currently exists in the repo (file/function named).

---

## 0. Scope, locked decisions, engine contract

### 0.1 Locked decisions (authoritative)

| # | Decision | Lua original (for the record) |
|---|---|---|
| L1 | **Dynamic BFS maze restored.** A 4-connected BFS from entrance `(0,12)` to exit `(25,12)` runs on run start and after every successful build/sell. Towers may stand on road cells; the road reroutes around them. | `main.lua:63-123` BFS from `(1,13)` to `(26,13)`, recomputed on build/sell |
| L2 | **Entrance `(0,12)` and exit `(25,12)` can never be built on.** Everything else may be walled. A placement that would leave the exit unreachable is refused with `"that would block the path"`, costs nothing and does not mutate the board. | BFS seeded the start cell unconditionally, so a tower could be built on the spawn cell (enemy spawned inside it); the exit could not be built on |
| L3 | **After any reroute, living enemies are re-projected** onto the nearest point of the new road, keeping their progress (no snap back to the entrance). | Lua recomputed the world position from normalised `t` on the new path; no explicit re-projection |
| L4 | **Enemy speed is absolute cells/second**: `(20 + wave - 1) * 0.25`, elite x1.8 (`SPEED_SCALE = 0.25`). Deliberate deviation from the Lua so mazing genuinely buys tower time. | `t += dt * speed/100`, `t` normalised over the whole path -> crossing time always `100/speed` s |
| L5 | **25 waves**. Lives 20 at start; every leak (elite or not) costs exactly 1 life and pays no gold; 0 lives -> game over; clearing wave 25 -> victory. | Endless waves; no lives; leaks paid +2/+10 gold and restarted the same wave forever |
| L6 | **Explicit build phase between waves**, player-paced ("NEXT WAVE"). Building/upgrading/selling is *also* allowed during a wave (as in the Lua). No inter-wave timer. | Waves auto-chained in the same frame; no build phase |
| L7 | Damage, multipliers, costs, kill rewards, upgrade curve, enemy HP/speed formulas, spawn interval: **keep the Lua numbers** (this spec). | — |
| L8 | **Element quotas are exact** and `game_wave_preview()` is truthful. The port defines wave size as `3 + 2*wave` **including** elites (the Lua added elites on top; see §6.1 and §14.5 Q1). | `baseEnemies = 3 + wave*2`, `enemiesPerWave = base + eliteCount`, HUD quotas from `base - eliteCount` |
| L9 | Pause overlay (separate from speed), speeds 1x/2x/4x, `L/R` = tower type, `Select` = speed, `Start` = pause. | Speed levels {0, 0.5, 1, 2, 4}; L/R speed; SELECT help; no pause state |

### 0.2 Conventions

- The **core** is cell space (`include/game.h`): 1.0 = one grid cell; cell `(col,row)` is 0-based, top-left origin, centre `(col+0.5, row+0.5)`.
- The **renderer** is pixels (`include/gfx.h`): top 400x240, bottom 320x240, origin top-left, +y down.
- Convert only in the renderer: `px = BOARD_X (5) + cellX * 15` (top), bottom UI uses its own rects (§11).
- Lengths are px or cells as stated. Times are seconds. Colours are Lua floats 0..1 -> `GfxColor`.
- The Lua has two state variables: outer `gameState` ("MENU"/"PLAYING") and inner `state` ("MOVING"/"BUILDING"/"TOWER_SELECTED"). In C these are `GamePhase` plus the (cursor, selection) pair; there is no separate build-mode enum in the shipped code (§10).
- `pathPos` = distance travelled **in cells** along `Game.path` (0 .. `path.length`); `path.length == path.count - 1` because every BFS step is exactly one cell.

### 0.3 Engine contract — `include/game.h` (frozen; append-only additions already made)

`include/game.h` is the contract between the portable core and the 3DS front end. Do not reorder or rename fields; append-only additions are allowed (the core already appended two).

| Item | Meaning / value | Section |
|---|---|---|
| `GRID_COLS`, `GRID_ROWS` | 26, 13; cell centres `(col+0.5,row+0.5)` | §2 |
| `GamePhase` | `PHASE_MENU`, `PHASE_BUILD`, `PHASE_WAVE`, `PHASE_GAMEOVER`, `PHASE_VICTORY` | §1 |
| `STARTING_GOLD`, `STARTING_LIVES` | 150, 20 | §7 |
| `Game.gold/lives/wave/totalWaves/enemiesAlive/enemiesToSpawn/kills/leaks` | run state; `totalWaves = 25`; `wave` is 1-based and becomes 26 when the run is won (renderer clamps) | §1.4, §6 |
| `Game.speedMultiplier` | 1.0 / 2.0 / 4.0; `game_cycle_speed()` cycles; `paused` is separate | §1.7 |
| `Game.path` (`Path`) | live BFS route: `pts` are cell centres, `count` cells, `length = count-1` cells. Initially 26 cells / 25 segments (straight row 12). Grows when the player mazes. | §2 |
| `Game.pathBlocked` | `false` in normal play. Set `true` only if a rebuild finds no route (defensive: board changed behind the core's back). Placement rejection does not set it — rejected builds never mutate the board. | §2.4 |
| `Game.occupancy[row][col]` | `0` empty, else tower index + 1. Note **row-major** indexing in C, unlike Lua's `[col][row]`. | §2 |
| `Enemy.pathPos` | distance along the *current* path in cells; `x,y` derived from it. Re-projection (§2.6) updates it when the road changes. | §2.6, §5.3 |
| `Game.spawnQuota[3]` | appended core field: remaining regular-enemy element quota for the wave being spawned (fire/water/forest). Front end never writes it. | §6.4 |
| `Game.stepAccumulator` | appended core field: leftover real time not yet converted to fixed steps. | §1.6 |
| `TowerSpec` | `name`, `cost`, `damage`, `fireRate`, **`range` in cells (2.142857f)**, `color[3]`, `glow[3]` | §3, §8 |
| `game_tower_stats()` | damage/fireRate at level, range constant | §8 |
| `game_upgrade_cost()`, `game_sell_value()` | cost curve / 75 % refund | §7, §8 |
| `game_damage_multiplier()` | element matrix 2.0 / 1.0 / 0.5 | §4 |
| `game_wave_preview()` | exact upcoming-wave Fire/Water/Forest/Elite counts (sum = wave size) | §6.7 |
| `game_can_place_at()` | **pure legality** incl. a hypothetical BFS; does NOT check gold. Used by the renderer for tile colouring. | §2.5 |
| `game_place_tower()` | full placement (gold + path rules); may reject; emits `GAME_EVT_BUILD` | §2.5, §3.5 |
| `game_select_tower()`, `game_set_cursor()`, `game_set_selected_type()` | selection/cursor/palette | §10 |
| `game_upgrade_selected()`, `game_sell_selected()` | economy actions; sell rebuilds the road and re-projects enemies | §7.2, §8 |
| `game_start_next_wave()` | only in `PHASE_BUILD` and not paused | §1.4 |
| `game_toggle_pause()`, `game_cycle_speed()` | pause/speed; pause resets the step accumulator | §1.7 |
| `game_clear_events()`, `game_clear_message()` | platform consumes events/messages | §12, §9.7 |
| `Game.events` bits | `GAME_EVT_BUILD/SELL/UPGRADE/LASER/ENEMY_DIE/WAVE_START/GAMEOVER/VICTORY/LEAK` | §12 |
| `MAX_TOWERS 96`, `MAX_ENEMIES 128`, `MAX_BEAMS 128`, `MAX_ELITE_SPAWNS 16` | fixed pools; no dynamic allocation anywhere in the core | §15.8 |

Private core interface (`source/core/core_internal.h`, not usable by the front end): `CORE_START_COL/ROW = 0/12`, `CORE_GOAL_COL/ROW = 25/12`, `CORE_TOTAL_WAVES 25`, `CORE_MESSAGE_TIME 1.5f`, `core_build_path()`, `core_path_exists()`, `core_path_length_with_block()` (maze AI/sim helper), `core_path_contains()`, `core_path_position()`, `core_enemy_speed_cells()`, `core_enemy_speed_scale()`, wave content formulas.

### 0.4 The game in one paragraph

A 26x13 maze tower defense. Creeps enter at `(0,12)` bottom-left and walk the current shortest road to the exit at `(25,12)` bottom-right. Towers are placed anywhere except those two cells — **including on the road**, which forces the BFS to find a detour and lengthens the walk (that is the core strategy). A placement that would seal the exit is refused. Between waves the player builds/upgrades, then taps NEXT WAVE; building is also allowed mid-wave. Three elemental towers (Fire/Water/Forest — identical base stats, rock-paper-scissors multipliers), 10 levels each, hitscan attacks with a 0.15 s laser visual. Kills pay 10 gold; start 150 gold; 20 lives; 25 waves; clearing wave 25 wins.

---

## 1. Game loop & state machine

### 1.1 Original Lua update order (reference — the C `game_step` mirrors it)

The path BFS itself is **not** in the update loop in the Lua either: it runs inside the build/sell input handlers. The per-frame order is:

```
 1. if phase == MENU: update menu particles only; return
 2. dt = dt * gameSpeed
 3. laserSoundCooldown = max(0, laserSoundCooldown - dt)
 4. errorTimer = max(0, errorTimer - dt)
 5. gameTime += dt
 6. laser/beam lifetime -= dt; remove dead
 7. wave/elite bookkeeping, spawn timer -> spawn one enemy
 8. enemies: t += dt*speed/100; t >= 1 -> leak
 9. towers (column-major over grid): cooldown -= dt; nearest target in range; fire
10. wave-completion check -> retry/advance (Lua); port: BUILD/VICTORY
```

Build/sell handlers in the Lua: mutate `gridData`, run `findPath()`; on success `updatePath()` (second BFS), on failure roll back and toast `"Path blocked!"`; sell runs `updatePath()`. `[LUA] 1350-1376, 1444-1457`

### 1.2 Original states (audit)

| Layer | Lua values | Notes |
|---|---|---|
| Outer `gameState` | `"MENU"`, `"PLAYING"` | no pause, no game over, no victory, no restart |
| Inner `state` | `"MOVING"`, `"BUILDING"`, `"TOWER_SELECTED"` | cursor/build mode (`main.lua:188`) |
| Speed | `speedLevels {0, 0.5, 1, 2, 4}` | `0` is the pause hack |
| Overlay | `showControls` | SELECT/TAB toggles a controls panel |

### 1.3 Missing states in the Lua (defects fixed by the port)

No lives, no game over, no victory/final wave, no pause menu, no restart/return-to-menu, no mute, no boss type, and `gameover.wav` is loaded but never played (`main.lua:263`). Leaking both paid gold and restarted the wave forever (`main.lua:698-707`, `779-798`), so the run could neither be won nor lost.

### 1.4 3DS phase machine `[CODE]`

| Phase | Entered by | Simulation | Player actions | UI |
|---|---|---|---|---|
| `PHASE_MENU` | `game_init` | none (renderer menu particles) | tap bottom START / A / Start -> `game_new_run` | title on both screens |
| `PHASE_BUILD` | `game_new_run`; completion of a wave < 25 | no enemies/spawning; tower timers/animations still advance; message timers tick | build/upgrade/sell/type/cursor/NEXT WAVE/pause | top HUD `BUILD`; bottom `NEXT WAVE` + incoming preview |
| `PHASE_WAVE` | `game_start_next_wave` (BUILD and not paused) | full step (§1.5) | same actions as BUILD (building mid-wave is allowed) | top HUD `WAVE`; bottom wave progress + stats |
| `PHASE_GAMEOVER` | `lives` hits 0 on a leak | frozen | bottom PLAY AGAIN / MAIN MENU; A retry, B menu | result overlay |
| `PHASE_VICTORY` | wave counter passes 25 | frozen | as game over | result overlay |

Transitions `[CODE]`:

| From | Trigger | To | Side effects |
|---|---|---|---|
| MENU | START/A/Start | BUILD | `game_new_run`: gold 150, lives 20, wave 1, totalWaves 25, clear board/entities, speed 1, build BFS path, selected type 0, selectedTower -1, cursor `(0,0)` |
| BUILD | NEXT WAVE (not paused) | WAVE | `start_wave()`: quotas/elite schedule for `wave`, spawn timer 0, selected tower cleared, `GAME_EVT_WAVE_START` |
| WAVE | `enemiesToSpawn == 0 && enemiesAlive == 0` | BUILD or VICTORY | `++wave`; if `wave > totalWaves` -> VICTORY + `GAME_EVT_VICTORY`, else BUILD |
| WAVE | leak takes lives to 0 | GAMEOVER | lives clamped 0, `GAME_EVT_LEAK` + `GAME_EVT_GAMEOVER`; enemies frozen |
| GAMEOVER / VICTORY | PLAY AGAIN / RETRY (touch or A) | BUILD | fresh `game_new_run` with the same seed |
| GAMEOVER / VICTORY | MAIN MENU (touch or B) | MENU | `game_init` |
| BUILD / WAVE | Start / pause badge | same, `paused` toggled | freeze battlefield; step accumulator reset on the toggle; messages keep ticking |
| BUILD / WAVE | Select / speed badge | same | speed 1 -> 2 -> 4 -> 1 |

Wave-number semantics `[CODE]`: `wave` is the wave being played (or the next one in BUILD). `check_wave_complete()` does `++wave` and tests `wave > totalWaves`; so after clearing wave 25, `wave == 26` and phase is `PHASE_VICTORY`. The renderer clamps display to `min(wave, totalWaves)` (`shownWave`) — see §11.2. `totalWaves` is 25.

### 1.5 `game_step(dt)` order `[CODE]`

`game_update(g, realDt)` converts real time into fixed steps; `game_step` is one deterministic step (tests call it directly).

```
game_step(dt):
  clamp dt to [0, CORE_MAX_STEP_DT=0.25]
  tick_message(dt)                      # wall-clock, before pause/phase checks
  if paused: return
  if phase not in {BUILD, WAVE}: return
  sdt = dt * speedFraction              # 1, 2 or 4; anything else -> 1
  timeTotal += sdt; update_beams(sdt)   # beam.life -= sdt
  if phase == BUILD:
      update_tower_timers(sdt)          # cooldown/animPhase keep running during build
      return
  # PHASE_WAVE
  waveTime += sdt
  update_spawning(sdt)                  # spawn timer, quota draw, elite slots
  if (!update_enemies(sdt)) return      # leak -> lives; returns false on game over
  compact_enemies()
  update_towers(sdt)                    # cooldown, nearest target, damage, beams, kills
  compact_enemies()
  check_wave_complete()                 # ++wave or VICTORY
```

- There is **no path rebuild in the step loop**. The road changes only inside `game_place_tower()` / `game_sell_selected()` (§2.4).
- Message timers use real dt (they expire while paused and at 4x speed).
- Enemy cleanup preserves spawn order via compaction (`memmove`-style copy), so the nearest-target tie-break stays "earliest spawned".
- `game_update` accumulates real dt (`clamp(dt, 0, 0.25)`) and runs at most 6 fixed steps (`CORE_MAX_STEPS_PER_UPDATE`), then drops leftover time to avoid catch-up spirals.

### 1.6 Fixed timestep `[CODE]`

`CORE_FIXED_STEP = 1/60 s`; speed multiplies inside the step (at 4x one step simulates 4/60 s). `game_update` vs `game_step` is the test seam. Pause (`game_toggle_pause`) resets `stepAccumulator` so unpausing cannot burst; while paused only `tick_message` runs.

### 1.7 Pause & speed `[CODE]` (deviation D5/D6 in §14.2)

- `paused` toggled by Start or the header pause badge; handled **before** any other input in a frame, so pausing never also triggers an action.
- Speeds 1.0 / 2.0 / 4.0 via Select, the header speed badge, or `game_cycle_speed()`; shown as `1X`/`2X`/`4X`, yellow when >1x.
- The Lua's 0.5x and its `0x` pause speed are not ported: 0x is replaced by the pause overlay.
- Pausing is allowed in BUILD and WAVE only (not in MENU/GAMEOVER/VICTORY).

---

## 2. Grid & dynamic pathfinding (restored maze mechanic)

### 2.1 Grid constants

| Constant | Value | Notes |
|---|---|---|
| `GRID_COLS`, `GRID_ROWS` | 26, 13 | `[LUA]` 26x13 grid |
| Start / entrance | **`(0,12)`** = Lua `(1,13)` | spawn centre `(0.5,12.5)`; never buildable |
| Goal / exit | **`(25,12)`** = Lua `(26,13)` | leak point; never buildable |
| Initial path (empty board) | **26 cells / 25 segments**, straight along row 12, left to right | `[CODE]` `core_build_path`; matches Lua's initial path shape (translated 0-based) |
| Path storage | `Game.path`: `pts[]` cell centres, `count`, `length = count-1` cells; max `MAX_PATH_CELLS = 338` (full-board snake) | `[CODE]` |
| Cell space | 1.0 = one cell | `include/game.h` |
| Top-screen cell size | **15 px**; board origin **`(5,38)`** = 390x195 px, 38 px HUD strip above, 7 px margin below | `[CODE]` `source/render.h` |
| Original px cell | 28 px (1280x720) | used only to derive range: 60/28 = **2.142857 cells** |
| Buildable cells | the whole board except `(0,12)` and `(25,12)`; road cells included | §2.5 |

C indexing warning: `Game.occupancy` is `[row][col]` (row-major), while the Lua used `gridData[col][row]`. Tower scans in the code are row-major loops; the Lua's update loop was column-major. This only affects the order in which simultaneous shots resolve, not balance.

### 2.2 Cell <-> pixel mapping `[CODE]`

```
core:     centre = (col + 0.5, row + 0.5)                       [cells]
renderer: x = BOARD_X (5) + cellX * 15,  y = BOARD_Y (38) + cellY * 15,  cell = 15 px
range draw radius = 2.142857 * 15 = 32.14 -> 32 px
```

The road is drawn from the **live** `g->path` polyline (`render.c build_path_map()`): every path cell is filled, and 1 px shoulder lines are drawn on road edges that border buildable cells so the brown road reads against the dark board. There is no hard-coded road anywhere in the renderer.

### 2.3 BFS implementation `[CODE]` (`source/core/grid.c`)

4-connected BFS, entrance -> exit, exactly the Lua's rules (`main.lua:63-123`) with fixed arrays and no allocation:

```
bfs_route(g, blockCol, blockRow, outPath):
  visited[13][26] = false; parent[338] = -1; queue[338]; head = tail = 0
  start = index(0,12); goal = index(25,12)          # index = row*26 + col
  visited[0][12] = true; queue[tail++] = start
  while head < tail:
      current = queue[head++]
      if current == goal: found = true; break
      col = current % 26; row = current / 26
      for neighbour in EXACT ORDER: up (row-1), down (row+1), left (col-1), right (col+1):
          if out of bounds: continue
          if visited: continue
          if wall: continue                          # occupancy != 0, OR the hypothetical block cell
          visited = true; parent[n] = current; queue[tail++] = n
  if not found: return 0
  walk parent[] from goal back to start, count cells; write cell centres start..goal into *outPath
  outPath->length = count - 1
```

- Neighbour expansion order is **up, down, left, right** — identical to the Lua (`main.lua:84-89`), so tie-breaking and the chosen detour are deterministic and match the original's expansion order.
- Walls are cells with `occupancy != 0`, plus the optional hypothetical block (used by placement checks and the maze AI, without mutating the board).
- Path cells are written as **cell centres** `(col+0.5,row+0.5)`, so path segments are exactly 1 cell long; `pathPos` = segment index + interpolation factor.
- Empty board result: `(0,12),(1,12),...,(25,12)` — 26 cells, length 25 (`[CODE]` test `test_initial_path`).
- A single tower on the road at `(7,12)` yields length **27** (detour via row 11 or 13; the test accepts either, `test_build_on_road_reroutes`). A contiguous road wall of N cells adds exactly +2 to the path (up, across, down) regardless of N; long paths need mazes with multiple rows/turns.
- Worst case path = 338 cells (a Hamiltonian snake); `MAX_PATH_CELLS` covers it.

### 2.4 When the path is rebuilt `[CODE]`

| Trigger | Function | Notes |
|---|---|---|
| Run start | `game_new_run()` | builds the initial straight row-12 path; sets `pathBlocked` if it fails (cannot on an empty board) |
| Successful placement | `game_place_tower()` | after committing the tower + occupancy + gold: `core_build_path()`, then `reproject_enemies()` |
| Defensive placement rollback | `game_place_tower()` | if the post-commit BFS somehow finds no route: undo tower/occupancy/gold, rebuild the old path, re-project, message `"that would block the path"` (unreachable in practice because of the pre-check) |
| Sale | `game_sell_selected()` | removing a wall can only shorten the road; rebuild + re-project |
| Never | `game_step()` | the update loop never runs BFS |
| UI query | `game_can_place_at()` | runs a **hypothetical** BFS (no mutation) to decide if a placement would keep the exit reachable. The top-screen renderer calls it every frame for the cursor tile; it is a 338-cell BFS and is deliberately allowed to run per frame. |

`Game.pathBlocked` stays `false` in normal play; it is only set if a rebuild finds no route (e.g. a corrupted board). It is not used by the game loop.

**Renderer rule:** read `g->path.pts` / `g->path.count` fresh every frame — the road can change on any build/sell, including mid-wave. Never cache the polyline.

### 2.5 Placement rules and "blocked path" behaviour `[CODE]`

`game_place_tower(col,row)` (uses `g->selectedType`) checks in this order, first failure wins; a failure sets `g->message` (1.5 s) and returns false without mutating anything:

| # | Check | Message |
|---|---|---|
| 1 | phase is BUILD or WAVE | `"can't build now"` |
| 2 | cell in bounds | `"can't build there"` |
| 3 | cell is not the entrance `(0,12)` | `"can't build on the entrance"` |
| 4 | cell is not the exit `(25,12)` | `"can't build on the exit"` |
| 5 | cell empty (`occupancy == 0`) | `"cell occupied"` |
| 6 | `towerCount < MAX_TOWERS` (96) | `"tower limit"` |
| 7 | exit still reachable with this cell treated as a wall (`core_path_exists`) | `"that would block the path"` |
| 8 | `gold >= 20` | `"not enough gold"` |

On success: `gold -= 20`; tower created at level 1 with `invested = 20`, `cooldown = 0`; `occupancy[row][col]` set; `towerCount++`; the new tower is **auto-selected**; road rebuilt; enemies re-projected; `GAME_EVT_BUILD`.

`game_can_place_at(col,row,type)` is the pure-predicate version used by the UI: phase + bounds + entrance/exit + occupancy + tower cap + hypothetical reachability. It deliberately **does not check gold**, so the renderer can show green (legal + affordable), yellow (legal but unaffordable), white (tower) or dim (illegal: entrance/exit or would seal the exit).

Original Lua differences (all deliberate fixes, §14.1 B5):
- Lua built the tower first, then BFS-validated and **rolled back** on failure (`"Path blocked!"`, no gold spent). It ran two BFS on success. The port pre-checks with a hypothetical BFS (one BFS) and never mutates on failure.
- Lua's BFS seeded the start cell unconditionally, so the spawn cell **could** be built on (enemy spawned inside the tower). It could not be built on the exit. The port protects both and uses distinct messages.
- Message text is `"that would block the path"` (lowercase, 1.5 s) instead of `"Path blocked!"`.

### 2.6 Enemy re-projection after a reroute `[CODE]` (`source/core/game.c`)

```
project_onto_path(path, x, y) -> float:
    bestDistSq = +inf; bestT = 0
    for i in 0 .. path.count-2:
        a = path.pts[i]; b = path.pts[i+1]; d = b - a
        t = clamp(dot(p - a, d) / |d|^2, 0, 1)
        q = a + d*t
        if |p - q|^2 < bestDistSq: bestDistSq = |p - q|^2; bestT = i + t
    return clamp(bestT, 0, path.length)

reproject_enemies(g):
    for every active enemy:
        e->pathPos = project_onto_path(&g->path, e->x, e->y)
        (e->x, e->y) = path_position(&g->path, e->pathPos)
```

- Segments are exactly one cell long, so `i + t` is directly a distance in cells.
- Tie-break: the **first** segment wins (`<` strict), so the result is deterministic.
- Called after every successful build and every sale. Enemies keep walking from the nearest point on the new road instead of snapping back to the entrance; a longer road therefore starts affecting the already-walking enemies immediately (they may move backwards/forwards in path distance, but stay spatially near where they were).
- If the new path is shorter, enemies re-projected past their old progress will have advanced — this is accepted (the player removed a wall, i.e. gave the creeps a shortcut; they now walk the shortcut).

### 2.7 Original BFS quirks (documented, not reproduced)

The Lua BFS used a table queue with `table.remove(queue,1)` (O(n) shift) and allocated four neighbour tables per expanded node; it seeded `visited[startCol][startRow]` and never checked the start cell for a tower; it reconstructed with `table.insert(path,1,...)`; it ran on every build/sell. It returned `nil` when the goal was unreachable, which triggered the rollback branch. The port keeps the algorithm's semantics (4-connected, up/down/left/right order, unit steps) but uses fixed arrays and a head/tail FIFO ring. None of the Lua's failure paths remain reachable.

### 2.8 Performance note `[CODE]`

BFS allocates nothing: `visited[13][26]` (338 B), `parent[338]` and `queue[338]` as `int16_t` (~1.7 KB total) live on the stack for the call. Work is <= 338 cells x 4 neighbours. It runs once per successful build, once per sale, and once per UI cursor query (`game_can_place_at` in the top renderer, per frame). That is negligible next to drawing ~1300 primitives and is far cheaper than the Lua+LovePotion original's table-based BFS plus double validation (§13.4).

---

## 3. Towers

### 3.1 Roster `[LUA]` (`main.lua:128-156`)

All three towers are stat-identical; only element (colour + damage multiplier) differs. `glowColor`/`glow` is declared but unused by the Lua — kept in `TowerSpec` for parity, no glow behaviour.

| # | Element | Core id | Cost | Damage L1 | `fireRate` L1 (= cooldown **seconds between shots**) | Shots/s `[DERIVED]` | Range | Range (cells) `[DERIVED]` | Colour (floats) | 8-bit | glow (unused) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | Fire | `ELEMENT_FIRE` (0) | 20 | 3 | 0.8 | 1.250 | 60 px | **2.142857** | (0.95, 0.15, 0.15) | (242, 38, 38) | (0.95, 0.3, 0.1) |
| 2 | Water | `ELEMENT_WATER` (1) | 20 | 3 | 0.8 | 1.250 | 60 px | 2.142857 | (0.15, 0.4, 0.95) | (38, 102, 242) | (0.1, 0.5, 0.95) |
| 3 | Forest | `ELEMENT_FOREST` (2) | 20 | 3 | 0.8 | 1.250 | 60 px | 2.142857 | (0.15, 0.9, 0.15) | (38, 230, 38) | (0.1, 0.9, 0.3) |

`TowerSpec` (`source/core/data.c`): `name = "Fire"/"Water"/"Forest"`, `cost = 20`, `damage = 3.0f`, `fireRate = 0.8f`, `range = 2.142857f` cells, colours/glows as above.

`fireRate` semantics `[LUA] 739-740`: seconds of cooldown between shots. On fire `cooldown = fireRate(level)`; every step `cooldown = max(0, cooldown - dt)`. A new tower has `cooldown = 0` and can fire on its first step with a target in range. Range is constant at all levels (from the spec, not the tower).

### 3.2 Targeting & firing `[LUA]` (`main.lua:715-777`)

- Every tower is scanned each step; each tower scans all active enemies.
- Candidate: `distance(towerCentre, enemyCentre) <= 2.142857` cells (Euclidean, inclusive).
- Target: smallest distance; ties keep the **earliest in the enemy array** (= earliest spawned) because only a strictly smaller distance replaces.
- Damage is applied instantly (hitscan; no projectile, no travel time). The beam is pure VFX.
- Kill: `gold += 10`, `kills++`, enemy removed, `GAME_EVT_ENEMY_DIE`. A killing shot still spawns its beam.
- No line-of-sight blocking, no minimum range, no priority modes, no crits. Towers never take damage.

### 3.3 Beams `[LUA]` (`main.lua:747-755`, `991-999`)

| `Beam` field | Value |
|---|---|
| `x1,y1` | tower cell centre at fire time |
| `x2,y2` | target position at fire time (snapshot, does not follow) |
| `element` | firing tower's element (colour source) |
| `life`, `maxLife` | 0.15, 0.15 (`CORE_BEAM_LIFE`) |
| draw | alpha = `life/0.15`; 1 px line at alpha; 3 px line at `alpha*0.1` |

`MAX_BEAMS 128`; when full a shot still deals damage and the cosmetic beam is dropped.

### 3.4 Tower icon geometry `[LUA]` (`main.lua:293-390`)

Filled polygons in a local system centred on the cell centre, radius parameter `size`. The C renderer draws them as **centroid triangle fans** (`render.c poly_fill`, each glyph is star-shaped) — visually identical at these sizes. Winding order irrelevant.

**Size**: original `size = 12 + min(level,10)*0.4` px in 28 px cells. On the 15 px board: levels 1-4 -> **7 px**, 5-9 -> **8 px**, 10 -> **9 px**.

**Fire** (s = size, w = 0.8s): outer vertices `(0,-1) (-0.8,+0.6) (-0.24,+0.2) (0,+0.8) (+0.24,+0.2) (+0.8,+0.6)` * s (6 tris around centroid `(0,+0.2s)`); inner flame at white alpha 0.15: `(0,-0.6) (-0.32,+0.3) (-0.08,+0.1) (0,+0.4) (+0.08,+0.1) (+0.32,+0.3)` * s.

**Water** (s): `(0,-1) (-0.8,+0.2) (-0.5,+0.6) (0,+1) (+0.5,+0.6) (+0.8,+0.2)` * s, centroid `(0,+0.27s)`; highlight quad white alpha 0.2: `(-0.2,-0.4) (-0.4,-0.1) (-0.2,-0.1) (-0.1,-0.3)` * s.

**Forest/leaf** (s): `(0,-1) (-0.7,-0.3) (-0.8,+0.2) (-0.3,+0.6) (0,+0.4) (+0.3,+0.6) (+0.8,+0.2) (+0.7,-0.3)` * s, centroid `(0,+0.05s)`; vein 1 px line `(0,-0.5s)` -> `(0,+0.3s)` black alpha 0.15.

**Star** (elite / HUD), 10 vertices: `(0,-1) (0.3,-0.3) (1,-0.3) (0.4,0.1) (0.6,0.8) (0,0.4) (-0.6,0.8) (-0.4,0.1) (-1,-0.3) (-0.3,-0.3)` * s, centroid `(0,0)`.

**Level marker** (drawn by the top renderer): level < 10 -> level number above the icon; level 10 -> two small gold rects above it (see §11.2).

### 3.5 Action API behaviour `[CODE]`

| API | Behaviour |
|---|---|
| `game_place_tower(g,col,row)` | §2.5; uses `selectedType`; auto-selects the placed tower; emits BUILD |
| `game_can_place_at(g,col,row,type)` | pure legality incl. hypothetical BFS; no messages; no gold check |
| `game_select_tower(g,idx)` | `idx` in `[0,towerCount)` selects, anything else clears (`-1`) |
| `game_set_selected_type(g,type)` | ignores out-of-range types; does not place |
| `game_set_cursor(g,col,row)` | clamps to grid |
| `game_upgrade_selected(g)` | §8; messages `"can't upgrade now"`, `"no tower selected"`, `"max level"`, `"not enough gold"` |
| `game_sell_selected(g)` | §7.2; messages `"can't sell now"`, `"no tower selected"`; rebuilds road + re-projects |
| `game_start_next_wave(g)` | ignored unless BUILD and not paused |
| `game_tower_at(g,col,row)` | tower index or -1 |

---

## 4. Elements / combinations

### 4.1 Damage multiplier matrix `[LUA]` (`main.lua:164-179`)

| Tower \ Enemy | FIRE | WATER | FOREST |
|---|---|---|---|
| **Fire** | 1.0 | **0.5** | **2.0** |
| **Water** | **2.0** | 1.0 | **0.5** |
| **Forest** | **0.5** | **2.0** | 1.0 |

`game_damage_multiplier(towerType, enemyElement)` returns 2.0 / 1.0 / 0.5; out-of-range indices clamp to 0; default 1.0.

### 4.2 No combining mechanic `[LUA]`

- No fusion, no mixed towers, no tier names, no recipes. Tower type is one `Element`; progression is the linear level 1..10 (§8).
- `upgrade_1..6.wav` is a **random sound pool** played on any successful upgrade (`main.lua:278-282`), not six tiers or six elements.
- `EnemySpec` (Fire/Water/Forest) is colour/name data; elites are a boolean modifier (§5).

### 4.3 Roster locked

Three elements only (Fire/Water/Forest), no earth/air, no combinations — the Lead's decision, matching the Lua (the remake README's "earth, air and combinations" was aspirational, not implemented).

---

## 5. Enemies

### 5.1 Base enemies `[LUA]` (`main.lua:158-162`, `669-689`)

All three elements are mechanically identical; element only drives colour and the damage multiplier.

| Element | `EnemySpec` name | Colour floats | 8-bit | HP | Speed parameter | Icon size | Kill bounty | Behaviour |
|---|---|---|---|---|---|---|---|---|
| Fire | "Fire" | (0.95, 0.15, 0.15) | (242,38,38) | `15 + (wave-1)*3` | `20 + (wave-1)` | 10 px orig / 5 px 3DS | +10 gold | walk road; leak at end |
| Water | "Water" | (0.15, 0.4, 0.95) | (38,102,242) | same | same | same | +10 gold | same |
| Forest | "Forest" | (0.15, 0.9, 0.15) | (38,230,38) | same | same | same | +10 gold | same |

No attacks, armour, status effects, regeneration, splitting, collisions, or interaction with towers beyond being shot.

### 5.2 Elite modifier `[LUA]` (`main.lua:674-677`, `392-409`)

| Property | Normal | Elite |
|---|---|---|
| HP | `15 + (wave-1)*3` | **x 2.5** |
| Speed | `20 + (wave-1)` (Lua units) | **x 1.8** |
| Icon size | 10 px orig / 5 px 3DS | 16 px orig / 9 px 3DS |
| Health bar | 24x5 orig / 13x3 px 3DS, raised above the icon | same |
| Kill bounty | +10 gold | **+10 gold** (no extra) |
| Extra visual | — | 1 px white + gold outline rects around the icon; gold star in previews |
| Element multiplier | applies | applies |

Examples: wave 25 elite = 217.5 HP, speed 44 x 1.8 = 79.2 Lua units -> 19.8 cells/s.

### 5.3 Movement in cell space — **absolute speed** `[CODE]`, deviation D11

The Lua moved enemies with `t += dt * speed/100` where `t` was normalised over the whole road, so a longer (mazed) road took exactly the same `100/speed` seconds — mazing bought the player **no** extra tower time. The port moves enemies at an absolute speed:

```
core_enemy_speed(wave, elite) = (20 + (wave - 1)) * (elite ? 1.8f : 1.0f)   // Lua units
speed_cells_per_sec          = core_enemy_speed(...) * SPEED_SCALE
SPEED_SCALE                  = 0.25f
pathPos                     += speed_cells_per_sec * dt
```

- Wave 1 regular = **5.0 cells/s**; wave 10 = 7.25; wave 25 = 11.0; wave 25 elite = **19.8**.
- Spawn: `pathPos = 0`, position = `path.pts[0]` = entrance centre `(0.5,12.5)`.
- Position each step = `core_path_position(&g->path, pathPos)` (segment index `(int)pathPos`, interpolate).
- **Leak**: `pathPos >= path.length` -> remove enemy, `lives -= 1`, `leaks += 1`, `GAME_EVT_LEAK`, no gold; if `lives == 0` -> clamp 0, `PHASE_GAMEOVER`, `GAME_EVT_GAMEOVER`.
- Road changes re-project `pathPos` onto the new road (§2.6); enemies never snap to the entrance.

**Travel time vs road length** (`[DERIVED]`, `seconds = path_length_cells / speed_cells_per_sec`; elite in parentheses):

| Road length (cells) | Wave 1 (5.0 / 9.0) | Wave 10 (7.25 / 13.05) | Wave 25 (11.0 / 19.8) |
|---:|---|---|---|
| 25 (initial) | 5.00 (2.78) | 3.45 (1.92) | 2.27 (1.26) |
| 40 | 8.00 (4.44) | 5.52 (3.07) | 3.64 (2.02) |
| 60 | 12.00 (6.67) | 8.28 (4.60) | 5.45 (3.03) |
| 80 | 16.00 (8.89) | 11.03 (6.13) | 7.27 (4.04) |
| 100 | 20.00 (11.11) | 13.79 (7.66) | 9.09 (5.05) |

Every extra cell of detour is extra time under fire: this is the whole point of the maze mechanic. `SPEED_SCALE` is the single balance knob (§14.6).

### 5.4 Enemy visuals `[LUA]` (`main.lua:392-417`, `1001-1005`)

- Icon = element glyph (§3.4) at radius 10 (normal) / 16 (elite) original; 3DS: 5 / 9 px.
- Health bar every frame: centred on the enemy, width 24 -> 13 px, black background alpha 0.7, fill `(1-p, p, 0)` clamped `p`; height 5 -> 3 px (fill 2 px).
- Elite outline rects drawn after the icon.
- `hitFlash` (engine extra): white flash for 0.12 s after damage; cosmetic.

### 5.5 Capacity `[PORT]`

`MAX_ENEMIES 128`; peak concurrent = wave size at most (waves do not overlap): wave 25 = 53. If the array fills, spawning pauses (timer clamped) and resumes — no enemy is silently lost. If it ever stayed full, the wave would stall; impossible with the current numbers.

---

## 6. Waves (25 total)

### 6.1 Formulas `[LUA]` + `[PORT]` wave-size rule

`start_wave()` (`source/core/game.c`):

```
total     = 3 + wave * 2                       // wave size INCLUDING elites [PORT, D3]
E         = 10 if wave % 25 == 0, else
             3 if wave % 10 == 0, else
             1 if wave %  5 == 0, else 0       // precedence %25 > %10 > %5
regular   = total - E
quotaFire   = quotaWater = floor(regular / 3)
quotaForest = regular - quotaFire - quotaWater
interval  = max(0.8, 3.0 - 0.05 * wave)         // seconds
HP        = 15 + (wave - 1) * 3
speedLua  = 20 + (wave - 1)                     // before SPEED_SCALE
```

- The Lua defined `baseEnemies = 3 + wave*2` and then **added** elites (`enemiesPerWave = base + E`) while computing its HUD quota from `base - E`; that is the off-by-one the port removes: the port keeps the Lua's HUD-quota formula (`regular = total - E`, fire = water = floor(regular/3)) but makes `total` the whole wave, so preview and spawns agree exactly (`[CODE]` `core_wave_total_spawns()`; tests assert preview sum == spawned count). See §14.5 Q1; this is the one content-level deviation to confirm.
- Elite element: rolled uniformly from the RNG, **not** part of the preview quotas (the preview's elite count is exact, the element is random).
- Regular element order: a deterministic shuffled draw from the remaining quota multiset (xorshift32), so consecutive waves do not feel identical but the multiset is exact.

### 6.2 Wave table 1..25 `[DERIVED]` (authoritative port numbers)

`total` = spawns incl. elites; `F/W/Fo` = exact regular quotas; `I` = spawn interval; `HP` = regular HP; `Spd` = Lua speed parameter; `cross` = seconds to walk the **initial 25-cell road** (regular / elite); `eliteHP`; `gold` = max kill income.

| Wave | total | E | F/W/Fo | I (s) | HP | Spd | cross reg/elite (s) | eliteHP | gold |
|---:|---:|---:|---|---:|---:|---:|---|---:|---:|
| 1 | 5 | 0 | 1/1/3 | 2.95 | 15 | 20 | 5.00 / — | — | 50 |
| 2 | 7 | 0 | 2/2/3 | 2.90 | 18 | 21 | 4.76 / — | — | 70 |
| 3 | 9 | 0 | 3/3/3 | 2.85 | 21 | 22 | 4.55 / — | — | 90 |
| 4 | 11 | 0 | 3/3/5 | 2.80 | 24 | 23 | 4.35 / — | — | 110 |
| 5 | 13 | 1 | 4/4/4 | 2.75 | 27 | 24 | 4.17 / 2.31 | 67.5 | 130 |
| 6 | 15 | 0 | 5/5/5 | 2.70 | 30 | 25 | 4.00 / — | — | 150 |
| 7 | 17 | 0 | 5/5/7 | 2.65 | 33 | 26 | 3.85 / — | — | 170 |
| 8 | 19 | 0 | 6/6/7 | 2.60 | 36 | 27 | 3.70 / — | — | 190 |
| 9 | 21 | 0 | 7/7/7 | 2.55 | 39 | 28 | 3.57 / — | — | 210 |
| 10 | 23 | 3 | 6/6/8 | 2.50 | 42 | 29 | 3.45 / 1.92 | 105.0 | 230 |
| 11 | 25 | 0 | 8/8/9 | 2.45 | 45 | 30 | 3.33 / — | — | 250 |
| 12 | 27 | 0 | 9/9/9 | 2.40 | 48 | 31 | 3.23 / — | — | 270 |
| 13 | 29 | 0 | 9/9/11 | 2.35 | 51 | 32 | 3.12 / — | — | 290 |
| 14 | 31 | 0 | 10/10/11 | 2.30 | 54 | 33 | 3.03 / — | — | 310 |
| 15 | 33 | 1 | 10/10/12 | 2.25 | 57 | 34 | 2.94 / 1.63 | 142.5 | 330 |
| 16 | 35 | 0 | 11/11/13 | 2.20 | 60 | 35 | 2.86 / — | — | 350 |
| 17 | 37 | 0 | 12/12/13 | 2.15 | 63 | 36 | 2.78 / — | — | 370 |
| 18 | 39 | 0 | 13/13/13 | 2.10 | 66 | 37 | 2.70 / — | — | 390 |
| 19 | 41 | 0 | 13/13/15 | 2.05 | 69 | 38 | 2.63 / — | — | 410 |
| 20 | 43 | 3 | 13/13/14 | 2.00 | 72 | 39 | 2.56 / 1.42 | 180.0 | 430 |
| 21 | 45 | 0 | 15/15/15 | 1.95 | 75 | 40 | 2.50 / — | — | 450 |
| 22 | 47 | 0 | 15/15/17 | 1.90 | 78 | 41 | 2.44 / — | — | 470 |
| 23 | 49 | 0 | 16/16/17 | 1.85 | 81 | 42 | 2.38 / — | — | 490 |
| 24 | 51 | 0 | 17/17/17 | 1.80 | 84 | 43 | 2.33 / — | — | 510 |
| 25 | 53 | 10 | 14/14/15 | 1.75 | 87 | 44 | 2.27 / 1.26 | 217.5 | 530 |

Run totals `[DERIVED]`: **725 spawns** (18 elites, 707 regulars); max kill income **7,250 gold**; +150 start = **7,400 max lifetime gold**. Wave 25 spawn span = `(53-1)*1.75 = 91.0 s`; the last spawn crosses the initial road in 2.27 s.

### 6.3 Elite spawn schedule `[LUA]` -> net effect `[CODE]`

The Lua preloaded per-elite delays and decremented them per spawn slot (`main.lua:531-556`, `656-667`). Net effect, verified against the code: **spawn slot 1 is always regular; slots 2 .. 1+E are the E elites consecutively; all other slots are regular.** The port implements exactly that (`elitePending` + slot index), with no delay-array bookkeeping (`core_wave_elite_delays()` still exists in `data.c` for reference/sim use but is not needed by `start_wave`). Tests assert the elite appears in the second spawn slot (`test_elite_spawn_stats`).

### 6.4 Spawning internals `[CODE]`

- First spawn happens one full interval after `game_start_next_wave()` (timer starts at 0 and increments before the check). Wave 1's first creep appears at t = 2.95 s.
- One spawn per interval; the timer **subtracts** the interval (`spawnTimer -= spawnInterval`) rather than resetting to 0 (Lua reset to 0; subtracting keeps cadence accurate under variable step counts).
- Spawn pauses (timer clamped to the interval) while the enemy array is full.
- Wave start clears the selected tower, resets `waveTime`, `enemiesAlive`, quotas, elite schedule, and emits `GAME_EVT_WAVE_START`.
- Waves never overlap: `PHASE_BUILD` is required between them.

### 6.5 Completion, victory, game over `[LOCKED L5]` `[CODE]`

```
check_wave_complete():
  if phase != WAVE: return
  if enemiesToSpawn > 0 or enemiesAlive > 0: return
  ++wave
  if wave > totalWaves: phase = VICTORY; event VICTORY
  else:                 phase = BUILD
```

No wave retry (the Lua retried forever on any leak), no auto-chain, no leak gold. Lives are checked in the leak handler before completion; if the last leak takes lives to 0, GAMEOVER wins.

### 6.6 Wave counter display

- Top HUD: phase tag + `shownWave/totalWaves` where `shownWave = min(g->wave, g->totalWaves)` (so VICTORY shows 25/25, never 26/25).
- Bottom build panel: `"WAVE %d OF %d"` with `g->wave` (the wave NEXT WAVE will start).
- Bottom wave panel: `"WAVE %d IN PROGRESS"`, progress bar, `ALIVE/QUEUED/KILLS/LEAKS/TOWERS/TIME`.
- Result overlay: `"WAVE %d/%d"` with `shownWave`.

### 6.7 Preview API `[CODE]`

`game_wave_preview(g,&fire,&water,&forest,&elite)` takes `g->wave` and returns the exact composition `fire+water+forest+elite == total`. It is used by the bottom BUILD preview and by the wave progress bar denominator. Tests (`test_wave_composition`) assert waves 1/4/5/10/25 => spawns 5/11/13/23/53, elites 0/0/1/3/10, fire 1/3/4/6/14, water 1/3/4/6/14, forest 3/5/4/8/15.

### 6.8 Engine caps

`MAX_ELITE_SPAWNS 16` (max E = 10) unused by the current path; `MAX_ENEMIES 128` (max 53 alive); `MAX_BEAMS 128`; `MAX_TOWERS 96` -> message `"tower limit"`.

---

## 7. Economy & lives

### 7.1 Values `[LUA]` except lives `[LOCKED L5]`

| Item | Value | Source |
|---|---|---|
| Starting gold | **150** | `main.lua:186`; `STARTING_GOLD` |
| Starting lives | **20** | `[LOCKED]`; `STARTING_LIVES` |
| Tower cost | 20 all types | `main.lua:131/140/149` |
| Kill reward | **+10 gold** any enemy, elite or not | `main.lua:763`; `CORE_KILL_REWARD` |
| Leak | **-1 life, 0 gold**, elites too | `[LOCKED L5]`; Lua paid +2/+10 and retried |
| Blocked placement | 0 gold spent, no tower | `[CODE]` §2.5 |
| Wave bonus | none | `[LUA]` |
| Sell refund | `floor(invested * 0.75)`; `invested` = 20 + all upgrades paid | `main.lua:1448-1450`; `game_sell_value()` |
| Upgrade cost (L -> L+1) | `20 * 2^(L-1)` = 20,40,80,160,320,640,1280,2560,5120 | `main.lua:1430`; `core_upgrade_cost_for_level()` |
| Total invested at level L | `20 * 2^(L-1)` = 20..10240 | `[DERIVED]` |
| Gold negative? | no (all spends checked) | `[LUA]` |
| Lose | `lives <= 0` -> `PHASE_GAMEOVER` | `[LOCKED L5]` |
| Win | clear wave 25 -> `PHASE_VICTORY` | `[LOCKED L5]` |
| Build/upgrade/sell phases | BUILD **and** WAVE (mid-wave building matches the Lua) | `[CODE]` |

### 7.2 Sell & economy API `[CODE]`

`game_sell_value(g,i) = tower.invested * 3 / 4` (integer floor). A successful sale: `gold += refund`; tower removed and the tower array compacted; occupancy rebuilt; **road rebuilt and enemies re-projected** (removing a wall can only shorten the road); selection cleared; `GAME_EVT_SELL`. The Lua hardcoded the base cost 20 inside `getTotalCost` (`main.lua:502-508`); the port tracks real `invested`, which is correct even if tower costs ever change.

### 7.3 Income of a full 25-wave run

Max lifetime gold = 150 + 7,250 (every kill claimed) = **7,400**. One tower fully maxed costs 10,240 — more than the entire run's income — so play is about many mid-level towers, not one level-10 tower. A blocked placement costs nothing (you can probe mazes for free); a leak costs a life *and* the 10 gold bounty.

### 7.4 Win/lose

- Lose: `lives` reaches 0 on any leak (`update_enemies` sets `PHASE_GAMEOVER`, emits `GAME_EVT_GAMEOVER`; audio plays the game-over stinger once wired).
- Win: the wave counter passes `totalWaves` (25); `wave` becomes 26, phase `PHASE_VICTORY`, `GAME_EVT_VICTORY`.
- Retry from either screen: fresh run at wave 1 with the same seed; MAIN MENU returns to `PHASE_MENU`.

---

## 8. Upgrades

### 8.1 Per-level table `[LUA]` formulas + `[DERIVED]` values

On a successful upgrade (`main.lua:1425-1442`, `source/core/data.c`): cost `20*2^(level-1)` (level before upgrade), damage doubles, cooldown becomes `floor(cooldown*0.9*100)/100`, level++, `invested += cost`, gold -= cost, random upgrade sound, `GAME_EVT_UPGRADE`. Max level 10; upgrade does **not** reset the current cooldown; range never changes.

| Level | Damage `[DERIVED]` | `fireRate` cooldown (s) `[DERIVED]` | Shots/s | DPS vs neutral | Upgrade cost (L -> L+1) | Total invested | Sell value |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3 | 0.80 | 1.250 | 3.75 | 20 | 20 | 15 |
| 2 | 6 | 0.72 | 1.389 | 8.33 | 40 | 40 | 30 |
| 3 | 12 | 0.64 | 1.563 | 18.75 | 80 | 80 | 60 |
| 4 | 24 | 0.57 | 1.754 | 42.11 | 160 | 160 | 120 |
| 5 | 48 | 0.51 | 1.961 | 94.12 | 320 | 320 | 240 |
| 6 | 96 | 0.45 | 2.222 | 213.33 | 640 | 640 | 480 |
| 7 | 192 | 0.40 | 2.500 | 480.00 | 1280 | 1280 | 960 |
| 8 | 384 | 0.36 | 2.778 | 1066.67 | 2560 | 2560 | 1920 |
| 9 | 768 | 0.32 | 3.125 | 2400.00 | 5120 | 5120 | 3840 |
| 10 | 1536 | 0.28 | 3.571 | 5485.71 | — (MAX) | 10240 | 7680 |

- `damage = 3 * 2^(level-1)` exactly; the `floor` step is load-bearing (0.8*0.9^9 = 0.3099, the Lua/port value is 0.28).
- Element multipliers stack: 2x vs weak, 0.5x vs resistant.
- Level 10: `game_upgrade_cost()` returns -1; upgrade fails with `"max level"` before checking gold.
- Upgrade buttons in the bottom panel show `UPGRADE <cost>G` or `MAX LEVEL`, green frame when affordable; SELL shows the refund.

### 8.2 UI `[LUA]` -> shipped 3DS

Bottom info panel shows `name LV.n`, `DMG <damage> RNG <range> DPS <damage/fireRate>`, plus UPGRADE/SELL buttons with prices. Selecting a tower also draws its range circle on the top board (selected bright, cursor-hovered dim). The Lua's text equivalents are in §9.3; the shipped layout is §11.3.

---

## 9. UI/HUD — original LÖVE layout (provenance reference)

The original draws a 1280x720 layout (playfield band y 0..500, HUD panel y 505..720). This section is kept because it documents where the content came from and what the port should not lose. **The shipped 3DS UI is §11**; where the two conflict, §11 is authoritative. The Lua has no lives widget and no tooltip; its info panel is the de-facto tooltip.

### 9.1 Fonts `[LUA]`

Five LÖVE default-font sizes: title 36, large 26, medium 18, small 14, tiny 11 (`main.lua:1506-1510`). The 3DS uses the 8x8 bitmap font at integer scale 1 (8 px) and 2 (16 px); hierarchy is conveyed by colour/size separately (§11).

### 9.2 Game HUD `[LUA]` (`main.lua:1045-1267`)

Panel `rect(0,505,1280,215)` fill `(0.03,0.03,0.07,0.95)` + 1 px top line `(0.1,0.1,0.2,0.2)`.

| Element | Original position | Font | Colour | Content |
|---|---|---|---|---|
| "WAVE" label | `printf(30,513,100,center)` | small | (0.5,0.5,0.7,0.6) | static |
| wave number | `printf(30,538,100,center)` | title 36 | white | `wave` |
| status | `printf(30,588,100,center)` | small | READY (1,1,0.4), ACTIVE (0.2,0.9,0.2), COMPLETE (1,1,0.3), WAITING (0.5,0.5,0.5) | READY/ACTIVE/COMPLETE/WAITING |
| "LEFT" label | `printf(30,608,100,center)` | small | (0.5,0.5,0.7,0.5) | |
| enemies remaining | `printf(30,628,100,center)` | large 26 | white a=0.9 | `enemiesRemaining` |
| "WAVE ENEMIES" header | `printf(1250,513,200,right)` | small | (0.5,0.5,0.7,0.6) | **drawn off-screen right** (§14.1 B8) |
| composition rows | y=538+24k; icon (1080,y+8) size 8; name `printf(1105,y,100,left)`; count `printf(1205,y,40,right)` `"xN"` | small | element colours; elite (1,0.8,0.2) | only count>0; Fire/Water/Forest/Elite |
| coin | circle (30,677) r14 (1,0.8,0.2), highlight (29,676) r5.6 a0.6 (1,1,0.5), rim (0.8,0.6,0.1) | — | — | |
| gold | `printf(55,669,80,left)` | large 26 | (1,0.9,0.3) | `gold` |
| "SPEED" + value | `printf(150,669,60,left)` small label; value `printf(150,687,60,left)` medium | | (0.5,0.5,0.7,0.5) / white a0.8 | `PAUSED/0.5x/1x/2x/4x` |
| "BUILD:" label | `printf(250,669,50,left)` | tiny 11 | (0.5,0.5,0.7,0.4) | |
| build buttons | 3x `rect(310+100(i-1),667,88,38,r4)` | — | fill (0.08,0.08,0.14,0.6); selected tint a0.2 + outline a0.4 | icon (x+18,685) size 12; cost `printf(x+38,679,40,left)` tiny white a0.6 |
| info panel | `rect(490,511,560,60)` fill (0.04,0.04,0.08,0.6), outline (0.1,0.1,0.2,0.15) | — | — | §9.3 |
| error toast | `rect(400,530,480,35)` black a0.7; text medium 18 (1,0.2,0.2) | | | `Path blocked!` / `Not enough gold!` / `Max level reached!` |

### 9.3 Info panel by build state `[LUA]` (`main.lua:1207-1257`)

| State | Line 1 | Line 2 |
|---|---|---|
| BUILDING | name; `DAMAGE: d`; `COST: cG` | `PATH CELL - Building redirects enemies` or `Press A to build  \|  B to cancel` |
| TOWER_SELECTED | name; `LV.n`; `DMG: d`; `X: costG` (green, `MAX` -> `X: MAXG`); `Y: sellG` (red) | `Press Y again to confirm sell` or `X: Upgrade  \|  Y: Sell  \|  B: Cancel` |
| empty | — | `PATH CELL - Build here to redirect enemies` or `Press A to build a tower here` |

### 9.4 Main menu `[LUA]` (`main.lua:804-878`)

Gradient background (721 one-pixel strips, `(0.02+0.03t,0.02+0.03t,0.05+0.07t)`, t=i/720); panel `rect(290,80,700,560)` (0.04,0.04,0.10,0.95) + outline (0.15,0.15,0.30,0.4); title glow at (640,200) r100 (1,0.7,0.2) a0.15; "ELEMENTAL" title 36 (1,0.9,0.4); "DEFENSE" 26 white with shadow; "Tower Defense" 18 (0.5,0.5,0.7,0.6); divider; three element icons at x=420/640/860, y=320, size 18 + glow; labels Fire/Water/Forest; START button `rect(570,400,140,55,r28)` (0.85,0.15,0.15) + shadow/highlight + "START"; hint "Press SELECT for controls"; START touch hit box (570..710, 400..455). Menu particles: 4 spawned per frame, unbounded (B9).

### 9.5 Controls overlay `[LUA]` (`main.lua:883-918`)

Full-screen black a0.90; panel (290,70,700,580); "CONTROLS" title; seven lines at `printf(380,180+50i,500,left)` (D-Pad/Touch move, A/Tap select/build, B cancel, L/R speed, X upgrade, Y twice sell, SELECT toggle); footer "Press SELECT again to close". Toggled by SELECT/TAB in both states; keys swallowed while open. Not ported as a screen (§14.5 Q4) — the bottom UI carries the hints.

### 9.6 Playfield `[LUA]` (`main.lua:923-1040`)

Draw order: gradient bg -> grid backdrop `rect(274,76,732,368)` (0.03,0.03,0.06) + outline -> path cells (fill `(0.2+0.08t,0.12+0.04t,0.04+0.02t)`, t = path index/length; outline (0.25,0.18,0.08,0.15)) -> cell fills (empty (0.05,0.05,0.08), tower tint a0.06, outline (0.08,0.08,0.12,0.12)) -> selected-tower range circle (r60, fill a0.06, outline a0.15) -> towers -> lasers -> enemies + health bars -> cursor (white fill a0.08, four 4x4 corner blocks a0.6, outline a0.3, yellow a0.15/a0.3 when building) -> pre-game hint (unreachable). 28 px cells at offset (276,78).

### 9.7 Transient messages `[CODE]`

`Game.message` + `messageTimer` are set by failed actions; `CORE_MESSAGE_TIME = 1.5 s` wall-clock (the Lua used 2.0 s of game time and swallowed the next key). The bottom screen draws the message as a red-bordered toast at `(x,35,w,18)` centred, while `messageTimer > 0`; it never blocks input. `game_clear_message()` clears it. Message strings are lowercase and short: `can't build there`, `can't build on the entrance`, `can't build on the exit`, `cell occupied`, `tower limit`, `that would block the path`, `not enough gold`, `can't build now`, `can't upgrade now`, `can't sell now`, `no tower selected`, `max level`.

---

## 10. Input mapping

### 10.1 Original Lua controls `[LUA]` (`main.lua:1288-1458`, `1463-1488`)

| Context | Key | Behaviour |
|---|---|---|
| MENU | `select`/`tab` | toggle controls overlay |
| MENU | `a`/`return`/`space` | start game |
| any | any key while toast up | clears toast, swallows key |
| PLAYING | `l`/`[`, `r`/`]` | speed index -/+ over {0,0.5,1,2,4} |
| BUILDING | `left`/`a`, `right`/`d` | previous / next tower type |
| BUILDING | `b`/`backspace` | cancel |
| BUILDING | `a`/`return`/`space` | build; `a` unreachable (intercepted above) |
| MOVING/TOWER_SELECTED | arrows / `w`/`s`/`a`/`d` | move cursor (clamped); leaving a tower cancels selection |
| MOVING | `return`/`space` | enter BUILDING or TOWER_SELECTED; `a` moves left instead |
| TOWER_SELECTED | `b` cancel, `x` upgrade, `y` arm/confirm sell | |
| PLAYING touch | tap grid cell | sets cursor then calls `keypressed("a")` — which moves the cursor one cell left; building via touch is impossible (B1/B10) |
| MENU touch | tap START rect | start |

### 10.2 Shipped 3DS mapping `[CODE]`

The top screen is display-only on real hardware (touch exists only on the bottom screen), so the build cursor is D-pad driven and the bottom panel holds all touch controls.

| Action | Touch (bottom screen) | Buttons |
|---|---|---|
| start run (menu) | big START button `(50,124,220,64)` | A or Start |
| move cursor | — (no touch map) | D-pad (up/down/left/right); cursor clamped, selection dropped if it leaves the selected tower |
| choose tower type | one of the 3 palette buttons `(6/110/214, 34, 96x52)` | L / R cycle the type |
| place / select | — | **A** at the cursor: if a tower is there -> select it; otherwise `game_place_tower` (works on road cells if the exit stays reachable) |
| deselect tower | — | B |
| upgrade / sell | UPGRADE `(8,120,150,30)` / SELL `(164,120,148,30)` | X / Y (auto-selects the tower under the cursor first if nothing is selected) |
| start next wave | NEXT WAVE button `(6,159,190,77)` (BUILD only) | — (deliberate: no accidental A wave start) |
| speed 1x/2x/4x | header SPEED badge `(244,2,34,28)` | Select |
| pause / resume | header PAUSE badge `(282,2,34,28)`; paused: RESUME `(60,104,200,52)` | Start (handled before every other action; also resumes) |
| retry / menu (result) | PLAY AGAIN `(40,140,240,56)` / MAIN MENU `(80,204,160,28)` | A retries, B goes to menu |
| paused -> menu | MAIN MENU `(100,168,120,28)` | — |
| debug only | top-screen tap (preview harness only; moves cursor + selects tower) | — |

Input logic order (`app.c tick_play`): pause toggle -> (if paused: overlay touches + message tick only) -> Select speed -> touch on bottom -> D-pad -> L/R type -> A/X/Y/B -> `game_update`. This guarantees a pause press can never also trigger an action.

### 10.3 Touch events `[CODE]` (`ui.h` / `ui.c`)

`ui_hit_test()` maps a bottom-screen touch to one `UI_EVENT_*`: START, SELECT_TYPE(index), UPGRADE, SELL, NEXT_WAVE, CYCLE_SPEED, TOGGLE_PAUSE, RESUME, RETRY, MENU. `app.c` converts them to core calls; `ui.c` never touches the simulation. Hit boxes are exported (`ui_*_rect`) and match the drawing code so the two cannot drift.

### 10.4 UI feedback on the board `[CODE]`

The top renderer calls `game_can_place_at(cursor)` every frame and colours the cursor tile: white edge (tower), green edge (legal + affordable), yellow edge (legal, unaffordable), dim edge (illegal: entrance/exit or would seal the exit). A translucent ghost of the selected tower type is drawn on an empty legal cell. The bottom info panel prints `CURSOR C%d R%d`, `ROAD` or `EMPTY CELL`, and one hint line:
- legal + road cell: `ROAD - BUILD HERE TO REROUTE THEM` (green);
- legal + empty cell: `A BUILDS <NAME> FOR 20G`;
- illegal road cell: `ROAD - THIS CELL CAN'T BE BLOCKED`;
- illegal other: `CAN'T BUILD HERE RIGHT NOW`;
plus `L/R TYPE   A BUILD   B CLEAR` and `TAP A TYPE ABOVE TO SWITCH`.

---

## 11. 3DS layout (shipped implementation)

### 11.1 Geometry and palette `[CODE]`

Top screen 400x240: board `BOARD_CELL 15`, `BOARD_X 5`, `BOARD_Y 38` -> 390x195 px, leaving a 38 px HUD strip at the top and 7 px margin at the bottom. Background is a 24-band vertical gradient `(0.02+0.03t, 0.02+0.03t, 0.05+0.07t)` (Lua's gradient, banded for speed). Palette (`source/render.h`):

```
cell empty        (0.05,0.05,0.08)            path cell    (0.24+0.10t, 0.14+0.05t, 0.05+0.03t)
grid line         (0.14,0.14,0.22,0.30)       path edge    (0.38,0.25,0.10,0.85)
panel             (0.04,0.04,0.10,0.95)       border       (0.15,0.15,0.30,0.40)
gold              (1.00,0.80,0.20)            gold soft    (1.00,0.90,0.40)
muted             (0.50,0.50,0.70,0.65)       dim          (0.38,0.38,0.58,0.55)
red               (1.00,0.20,0.20)             green        (0.20,0.90,0.25)
build tag         (1.00,1.00,0.40)            wave tag     (0.20,0.90,0.20)
element colours identical to TowerSpec/EnemySpec (fire 0.95,0.15,0.15 etc.)
```

Top render order: gradient -> HUD strip -> board backing/board cells -> path shoulders -> tower-cell tints -> grid lines -> border -> **entrance/exit portal arrows** -> range circles (cursor + selected) -> tower icons + level markers -> beams -> enemies + health bars -> cursor/ghost -> menu/pause/result overlays.

Board drawing details `[CODE]`:
- Path cells come from `g->path` (`build_path_map` floors each `path.pts[i]` to `(col,row)`); gradient `t = pathIndex/(count-1)`.
- Road shoulder: a 1 px `COL_PATH_EDGE` line on each side of a road cell that faces a non-road cell (this is what makes the road readable at 240p).
- Tower cells get a 0.06-alpha element tint; grid is 1 px lines on the whole board (not per-cell outlines).
- **Entrance/exit markers** (`render.c draw_portal_markers`): arrows that follow the live path. Green arrow `(0.35,1.00,0.45)` at `path.pts[0]` pointing along the first segment; red arrow `(1.00,0.35,0.30)` at `path.pts[count-1]` pointing along the last segment. Each arrow = a black backing triangle (alpha 0.55, apex 7 px along, base 3 px back, half-width 5.5 px) plus the coloured triangle (apex 6 px, base 3 px back, half-width 4.5 px). Because they follow `g->path`, they move when the player reroutes the road.
- Cursor: white cell fill a0.07, 4 corner brackets, edge colour as §10.4; ghost element glyph at 30 % alpha on a legal empty cell.
- Range circles: 32 px radius for the selected tower (bright) and for the tower under the cursor (dim); selected tower cell gets an outline.
- Enemy health bars 13x3 px; beams 1 px + 3 px glow at 0.15 s life.

### 11.2 Top HUD strip (0..37 px) `[CODE]`

| Element | Position | Scale | Colour | Content |
|---|---|---|---|---|
| coin | `art_coin(14,19,6)` | — | gold/white/amber | |
| gold | `gfx_text(26,11,2)` | 2 (16 px) | gold | `%d` |
| heart | `art_heart(100,19,6)` | — | red | |
| lives | `gfx_text(112,11,2)` | 2 | white | `%d` |
| phase tag | `gfx_text(160,5,1)` | 1 | PAUSED grey; WAVE green; OVER red; WIN gold; BUILD yellow | `PAUSED`/`WAVE`/`OVER`/`WIN`/`BUILD` |
| wave counter | `gfx_text(160,17,2)` | 2 | white | `shownWave/totalWaves` (clamped to 25) |
| ALIVE / TOWERS label | `gfx_text(244,5,1)` | 1 | muted | `ALIVE` in WAVE, `TOWERS` otherwise |
| count | `gfx_text(244,17,2)` | 2 | white | `enemiesAlive` or `towerCount` |
| SPEED label | `gfx_text(330,5,1)` | 1 | muted | `SPEED` |
| speed value | `gfx_text(330,17,2)` | 2 | white, yellow if >1 | `%.0fX` |
| HUD background | `rect(0,0,400,38)` fill (0.035,0.035,0.09) + 1 px bottom line | | | |

### 11.3 Bottom screen (320x240) `[CODE]`

Vertical budget: header 0..31; tower palette 34..85; info/upgrade panel 89..154; bottom section 159..236.

| # | Element | Rect / position | Notes |
|---|---|---|---|
| B1 | header | `rect(0,0,320,32)` fill (0.035,0.035,0.09) + 1 px bottom line | |
| B2 | gold | coin `(14,16)` r6; value scale2 at `(26,8)` gold | |
| B3 | lives | heart `(100,16)` size6 red; value scale2 at `(112,8)` white | |
| B4 | wave | `"WAVE"` scale1 (160,12) muted; `"%d/%d"` scale1 (200,12) white | not clamped here |
| B5 | speed badge | `rect(244,2,34,28)`: dark fill, top highlight, 1 px border; `"%.0fX"` scale2 centered `(261,9)` | tap = cycle speed |
| B6 | pause badge | `rect(282,2,34,28)`: same frame; two 5x12 white bars at x+10/x+19, y+8 | tap = pause/resume |
| B7 | tower palette | 3 buttons `(6,34,96,52)`, `(110,34,96,52)`, `(214,34,96,52)` | selected: element tint a0.16 fill, 2 px element border, glow; icon at `(x+18,y+30)` size 10; name scale1 `(x+34,y+9)`; coin `(x+41,y+35)` r4; cost scale1 `(x+48,y+31)` gold/greyed |
| B8 | info panel | `rect(4,89,312,66)` panel fill + border | selected tower: icon `(16,99)` size 5.5 + glow; `"<name> LV.n"` scale1 `(30,92)` element colour; `"DMG %.1f   RNG %.1f   DPS %.1f"` scale1 `(10,107)` muted; cursor tower: `"CURSOR C%d R%d"` `(10,93)`, `ROAD`/`EMPTY CELL` right-aligned `(308,93)`, hint line `(10,108)` per §10.4, `"L/R TYPE   A BUILD   B CLEAR"` `(10,124)`, `"TAP A TYPE ABOVE TO SWITCH"` `(10,138)` |
| B9 | UPGRADE button | `rect(8,120,150,30)` green frame; label scale1 centred at y+11: `UPGRADE <cost>G` / `MAX LEVEL` | disabled grey when unaffordable/maxed |
| B10 | SELL button | `rect(164,120,148,30)` red-ish frame; label `SELL <refund>G` | |
| B11 | NEXT WAVE (BUILD) | `rect(6,159,190,77)`: green fill (0.13,0.45,0.20), 2 px green border, glow; `"NEXT WAVE"` scale2 at y+10; `"WAVE %d OF %d"` scale1 y+34; `"PRESS WHEN READY"` scale1 y+48 | |
| B12 | INCOMING (BUILD) | `rect(202,159,112,77)` panel with title; 4 rows at y+20/34/48/62: element icon + `x<count>` | Fire/Water/Forest/Elite (elite = gold star), exact preview |
| B13 | wave panel (WAVE) | `rect(6,159,308,77)` panel: `"WAVE %d IN PROGRESS"` `(14,163)`; progress bar `(14,174,292,16)` with orange fill `288*frac` at `(16,176,·,12)`; `"CLEARED %d%%   LEFT %d"` centred `(160,178)`; `"ALIVE %d"` `(14,196)`, `"QUEUED %d"` `(112,196)`, `"KILLS %d"` `(212,196)`; `"LEAKS %d"` `(14,208)` red if >0, `"TOWERS %d"` `(112,208)`, `"TIME %d:%02d"` `(212,208)` | LEFT = alive + queued; frac from wave size |
| B14 | toast | `rect(x,35,w,18)` red-bordered dark box, text scale1 centred `(160,40)` | `Game.message` while timer > 0 |
| B15 | menu screen | title art; big START `(50,124,220,64)`; element icons above | |
| B16 | paused overlay | dim play UI; `"PAUSED"` title; `"THE WAVE IS FROZEN"` (160,74) scale1; RESUME `(60,104,200,52)`; MAIN MENU `(100,168,120,28)` | touch only, plus Start/overlay |
| B17 | result overlay | `"GAME OVER"` (red) / `"ALL WAVES CLEARED"` (gold) title scale4 at y14; `"WAVE %d/%d"` etc.; PLAY AGAIN `(40,140,240,56)`; MAIN MENU `(80,204,160,28)` | shownWave clamped |

### 11.4 ASCII mockups (not to scale)

**A. Initial layout** — empty board, road = straight row 12, entrance left, exit right.

Top screen 400x240:

```
+--------------------------------------------------------------------------------+
| (o) 150   (v) 20    BUILD     1/25      TOWERS 0          SPEED 1X             |  HUD 0..37
+--------------------------------------------------------------------------------+
|   . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .  |
|   . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .  |
|   . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .  |
|   . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .  |
|   ... (rows 0..11: 26x13 board, 15 px cells; only row 12 is road initially) ... |
|   >############################>                                                |
|   ^ entrance (0,12) green arrow                       exit (25,12) red arrow ^  |
+--------------------------------------------------------------------------------+
| 7 px margin                                                                     |
+--------------------------------------------------------------------------------+
```

Bottom screen 320x240:

```
+--------------------------------------------------------------------------+
| (o) 150    (v) 20   WAVE 1/25             [1X]   [||]                    |  header 0..31
+--------------------------------------------------------------------------+
| [ FIRE 20G ]   [ WATER 20G ]   [ FOREST 20G ]                            |  palette 34..85
+--------------------------------------------------------------------------+
| CURSOR C0 R0                                              EMPTY CELL     |  info panel
| A BUILDS FIRE FOR 20G                                                    |  89..154
| L/R TYPE   A BUILD   B CLEAR                                             |
| TAP A TYPE ABOVE TO SWITCH                                               |
| [ UPGRADE 20G ]                          [ SELL 15G ]                    |
+--------------------------------------------------------------------------+
| [  NEXT WAVE  ]   [ WAVE 1 OF 25 ]  |  INCOMING                          |  bottom
| [     (big)   ]   [ PRESS WHEN READY]|  F x1  W x1  Fo x3  E x0           |  159..236
+--------------------------------------------------------------------------+
```

**B. Example maze** — after the player walls the bottom rows (28 towers, exactly measured with the port BFS: path length 35 cells, i.e. +10 cells of detour; `T` = tower on road/wall, `#` = current road, `S`/`G` = entrance/exit; rows 0-6 are empty build space above the maze).

```
     01234567890123456789012345
 0   ..........................
 1   ..........................
 2   ..........................
 3   ..........................
 4   ..........................
 5   ..........................
 6   ..........................
 7   ####################......
 8   #.....T.....T.....T####...
 9   #..T..T..T..T..T..T..T#...
10   #..T..T..T..T..T..T..T#...
11   #..T..T..T..T..T..T..T#...
12   S..T.....T.....T.....T###G
```

The road now runs along row 7 for 20 cells before dropping back to row 12 at col 22, so every tower in rows 8-11 gets several seconds of fire on the whole wave (wave 1: 35 cells / 5.0 cells/s = 7.0 s, vs 5.0 s on the initial road). A single tower at `(7,12)` would already force a +2-cell detour (path length 27, verified by `test_build_on_road_reroutes`).

### 11.5 Draw order and budget `[CODE]`

Top: gradient (24 bands) -> HUD -> board backing -> board cells (338 fills) -> path shoulders -> tower tints (<=96) -> grid lines (25 vertical + 12 horizontal) -> border -> portal arrows (2 pairs of triangles) -> range circles (<=2) -> towers (<=96 x ~10 tris) -> beams (<=128 x 2 lines) -> enemies (<=53 x ~8 prims) -> cursor/ghost. Bottom: header + badges, palette (3 buttons x few prims), info panel + two buttons, build or wave section, toast. Worst case well under ~1500 primitives; the 3DS build is reported at a stable 60 fps in the Azahar emulator. `game_can_place_at` (338-cell BFS) is called once or twice per frame by the top renderer; acceptable.

### 11.6 Stereo

Single 2D target (`GFX_LEFT`): the game is flat 2D; duplicating the top screen for stereo would double the draw cost for no gameplay benefit. The bottom screen is 2D touch.

---

## 12. Audio mapping

### 12.1 Current status `[CODE]`

`source/audio.c` is a **deliberate stub**: it records `GAME_EVT_*` bits (for tests/preview) but plays nothing; there is no ndsp mixer yet. The README lists sound as the next milestone. The original files are still available and should be wired through the same event bits:

| `Game.events` bit | Intended sound (original file) | When |
|---|---|---|
| `GAME_EVT_BUILD` | `build.wav` | successful placement |
| `GAME_EVT_SELL` | `sell.wav` | confirmed sale |
| `GAME_EVT_UPGRADE` | random one of `upgrade_1..6.wav` | successful upgrade (no level/element mapping) |
| `GAME_EVT_LASER` | `laser.wav` | tower fires; throttle globally to **0.08 s** game time (`laserSoundCooldown` in the Lua) |
| `GAME_EVT_ENEMY_DIE` | `enemy_die.wav` | enemy killed |
| `GAME_EVT_WAVE_START` | `wave_start.wav` | NEXT WAVE |
| `GAME_EVT_LEAK` | — (no Lua sound; optional alarm) | enemy reaches the exit |
| `GAME_EVT_GAMEOVER` | `gameover.wav` (**never played** in the Lua) | lives reach 0 |
| `GAME_EVT_VICTORY` | — (optional jingle) | wave 25 cleared |

Music in the Lua: `menu_music.ogg` loops at volume 0.5 from boot, `game_music.ogg` loops at 0.5 during play; the Lua stops menu music on run start and never pauses music. `playSound()` does `stop()` then `play()` (retrigger, no overlapping voices); `soundEnabled` is never toggled (no mute UI).

### 12.2 File inventory

| File | Bytes | Format | Est. duration (16-bit stereo 44.1 kHz assumption) | Plan |
|---|---:|---|---:|---|
| `build.wav` | 305,998 | WAV | ~1.73 s | keep |
| `enemy_die.wav` | 6,318 | WAV | ~0.04 s | keep |
| `gameover.wav` | 10,497,182 | WAV | ~59.5 s — huge | trim to 2-3 s stinger |
| `game_music.ogg` | 3,590,538 | OGG | codec-dependent | optional |
| `laser.wav` | 7,758 | WAV | ~0.04 s | keep, quiet, throttled |
| `menu_music.ogg` | 1,705,932 | OGG | codec-dependent | optional |
| `sell.wav` | 93,518 | WAV | ~0.53 s | keep |
| `upgrade_1..6.wav` | 76,110 / 78,158 / 79,694 / 71,502 / 76,622 / 63,310 | WAV | 0.36-0.45 s each | keep |
| `wave_start.wav` | 151,278 | WAV | ~0.86 s | keep |

### 12.3 3DS audio recommendations

- Convert SFX to 16-bit mono 22050 Hz (or IMA-ADPCM) and embed with `bin2s`; a 1 s mono 22.05 kHz clip is 44 KB. Whole SFX set < 400 KB.
- Trim `gameover.wav` (10.5 MB is unacceptable) to a 2 s stinger (~88 KB).
- Music: no OGG decoder in libctru. Either omit (music optional), or pre-decode a 15-30 s loop to mono 22.05 kHz PCM and loop from RAM (0.7-1.5 MB), or integrate a streaming decoder (v2). Do not decode OGG at runtime while rendering.
- `ndsp`: >= 8 SFX channels + music channels; implement the 0.08 s laser throttle exactly (one global cooldown), driven by `Game.events` + `game_clear_events()` once per frame (already wired in `app.c`).

---

## 13. What likely made the old 3DS attempt fail (evidence)

1. **Absolute 1280x720 layout, no resolution independence.** `love.conf` demands 1280x720 (`main.lua:11-18`), `love.window.setMode(1280,720,...)` (`main.lua:1495-1501`), and every UI coordinate is absolute: HUD at y 505..720, info panel x 490..1050, grid offset x=276, menu panel 700x560. On 400x240 you see the top-left corner only (part of the title; columns 1-4 of the field; the HUD is entirely below the screen). This is the README's "half the screen cut off".
2. **Per-frame immediate-mode primitive count.** `drawGradientBackground` draws **721 filled rectangles every frame** (`main.lua:437-443`); the field does 338 cells/frame with an `isPathCell()` linear scan (`main.lua:949-968`, `482-490`); icons are 6-10-vertex polygons; five TrueType fonts are rasterized. Through LovePotion on the ARM11 every primitive is a CPU draw; likely the fps killer.
3. **Unbounded per-frame allocations on the menu.** 4 particle tables per frame with no cap (`main.lua:600-615`) -> ~600 live Lua tables; plus a laser table per shot and four neighbour tables per BFS node.
4. **BFS cost/hitching.** `findPath()` allocates neighbour tables, uses `table.remove(queue,1)` (O(n) shift), and runs **twice on every successful build** (`main.lua:1362-1364`) plus once per sell. Maze building — the core strategy — is exactly when the pathfinder is stressed. The port keeps the mechanic but re-implements it with fixed arrays and a single hypothetical pre-check; the BFS is now ~1.7 KB of stack and <=338 cells, negligible against rendering.
5. **Audio memory/decode.** 10.5 MB `gameover.wav`, 3.6 + 1.7 MB OGGs; OGG streaming decode competes with rendering on the ARM11, loading dominates boot.
6. **Touch controls fought keyboard emulation (fatal).** `love.touchpressed` sets the cursor then calls `love.keypressed("a")` (`main.lua:1463-1488`). In `MOVING`, `a` means "cursor left" (`main.lua:1387-1393`), so every tap moved the cursor one cell left; entering `BUILDING` needs `return`/`space`, which touch cannot send; in `BUILDING`, `a` is intercepted to cycle type (`main.lua:1336-1340`). The game was unbuildable on touch-only hardware; only menu START worked. The port has no top-screen touch and a proper D-pad cursor + bottom-button UI.
7. **No performance escape hatches:** no batching, no atlas, no culling, no reduced-detail mode, no fixed timestep, no resolution factor.
8. **The game had no end state:** leaks paid gold and restarted the wave forever, so it could neither be won nor lost (`main.lua:779-798`) — looked broken at any frame rate.

The C rewrite removes 1-7 structurally: fixed 400x240/320x240 layout (§11), a portable deterministic core with fixed pools (§15), a cheap BFS (§2.8), a fixed-step loop, touch-native input (§10), and proper endings (§1.4).

---

## 14. Known bugs / unclear bits

### 14.1 Original defects and how the port handles them

| ID | Lua ref | Defect | Port behaviour |
|---|---|---|---|
| B1 | `main.lua:1336-1344`, `1387-1393`, `1406-1415` | `a` aliased to "cursor left"/"previous type" before the confirm branch; keyboard A never builds/selects; help text lies | A is a real confirm button; D-pad moves the cursor (§10.2) |
| B2 | `main.lua:698-707`, `779-798` | Leaks paid +2/+10 gold and retried the same wave forever; no lives/game over/victory | lives 20, -1 per leak, 0 gold, wave advances, game over at 0, victory at wave 25 |
| B3 | `main.lua:644-648`, `1031-1039`, `1295-1301` | Intended build-before-wave-1 phase is dead code (menu confirm starts a wave immediately) | `game_new_run` starts in `PHASE_BUILD`; wave 1 only on NEXT WAVE |
| B4 | `main.lua:794-798` | Waves auto-chain in the same frame; COMPLETE status never visible | player-paced BUILD phase; no auto-chain, no timer |
| B5 | `main.lua:63-69`, `1350-1376` | BFS seeded the start cell unconditionally, so a tower could be built on the spawn cell (enemy spawned inside); the Lua validated by mutating + rolling back | entrance `(0,12)` AND exit `(25,12)` rejected; hypothetical BFS pre-check; rejection costs nothing; message `"that would block the path"` |
| B6 | `main.lua:558-565` | HUD quotas computed from `base - E`; port's exact-quota rule makes the whole wave `3+2*wave` including elites (see Q1) | `game_wave_preview` sum == spawned count; tests assert it |
| B6b | `main.lua:573-576` | Spawner ignored the quotas and rolled every element randomly | exact quotas, shuffled order (§6.4) |
| B7 | `main.lua:1235-1237` | `"X: MAX" .. "G"` renders `X: MAXG` | bottom button shows `MAX LEVEL` |
| B8 | `main.lua:1103` | right-aligned header drawn off-screen (right edge 1450) | composition moved to the bottom INCOMING panel |
| B9 | `main.lua:600-615` | 4 particle tables/frame, no cap; life and maxLife rolled independently | renderer-local particle pool (`MENU_PARTICLE_COUNT`), capped; no per-frame allocation |
| B10 | `main.lua:883-918`, `1463-1488` | help overlay/toast do not block touch; overlay blocks keys only; top touch sets cursor to the left cell | no help screen; no top touch on hardware; toast never blocks input |
| B11 | `main.lua:1311-1314` | toast swallows one key press | toast is display-only |
| B12 | `main.lua:188` | global `state` vs `gameState` | `GamePhase` + cursor/selection, no build-mode globals |
| B13 | `main.lua:136`, `145`, `154`, `159-161` | `glowColor` never read | kept in specs, no behaviour |
| B14 | `main.lua:502-508` | `getTotalCost` hardcodes base cost 20 | `Tower.invested` tracked exactly |
| B15 | `main.lua:199`, `201`, `764` | `gameTime`/`totalKills` written, never displayed | `kills`, `leaks`, `timeTotal` shown in the wave panel and result screen |
| B16 | `main.lua:263` | `gameover.wav` loaded, never played | wired to `GAME_EVT_GAMEOVER` once audio exists |
| B17 | `main.lua:206`, `570`, `783-792` | `waveRetry`/`enemiesThatPassed` retry loop | removed; leaks cost lives and the wave advances |
| B18 | `main.lua:413-417` | health-bar width could go negative past 0 hp | fill fraction clamped 0..1 |
| B19 | `main.lua:217-219`, `1316-1325` | speeds {0, 0.5, 1, 2, 4}; pause is a speed hack | 1/2/4 + separate pause overlay |
| B20 | `main.lua:140-145` | multiplier 1.0 fallbacks | kept as default 1.0 |

### 14.2 Deliberate deviations from the Lua (do not "fix back")

| # | Deviation | Lua |
|---|---|---|
| D1 | Build phase between waves, player-paced NEXT WAVE, no inter-wave timer | instant same-frame auto-chain |
| D2 | 20 lives; leak = -1 life, 0 gold; game over at 0 | no lives; leak paid +2/+10 and retried forever |
| D3 | Wave size `3+2*wave` **including** elites; exact quotas so the preview is truthful (see Q1) | wave size `base+E`, HUD quota from `base-E`, random elements |
| D4 | Victory after wave 25; `totalWaves = 25`; `wave` becomes 26 on victory (display clamps) | endless, no victory |
| D5 | Pause overlay; speed 1/2/4 only (no 0.5x, no 0x speed) | speed 0/0.5/1/2/4, no pause UI |
| D6 | L/R = tower type; Select = speed; Start = pause; no help screen | L/R = speed; SELECT = help; no pause |
| D7 | Game-over/victory screens + `gameover.wav` | missing state, unused sound |
| D8 | 3DS layout 400x240/320x240, 8x8 font, board at (5,38), 24-band gradient, no top-screen touch, bottom-only touch UI | 1280x720, TrueType fonts, 721-line gradient, mouse/touch on the field |
| D9 | Toast never swallows input; message 1.5 s wall-clock (ticks while paused/4x) | 2.0 s game-time, swallows the next key |
| D10 | Fixed 1/60 s timestep | variable LÖVE dt |
| **D11** | **Enemy speed is absolute cells/s**: `(20+wave-1) * 0.25 * (elite?1.8:1)`; road length matters (`SPEED_SCALE` is the single knob) | `t += dt*speed/100` normalised over the whole path: crossing time `100/speed` s regardless of road length, so mazing bought no time |
| D12 | Entrance and exit cells are never buildable; live enemies are re-projected spatially onto the rerouted road (nearest point, keep progress) | start cell buildable (spawn-inside-tower bug); enemies' world position recomputed from normalised `t` |
| D13 | Blocked placement is pre-checked with a hypothetical BFS (one BFS, no mutation, message `"that would block the path"`) | mutate board, BFS, roll back (`"Path blocked!"`, two BFS on success) |

### 14.3 Behaviours kept intentionally

- Dynamic BFS road, towers on the road, detours, blocked-build rejection (§2).
- Hitscan towers with a 0.15 s beam visual, nearest-target tie-break by spawn order, cooldown semantics (§3).
- Damage matrix, costs, kill rewards, upgrade curve, sell refund, enemy HP/speed formulas, spawn interval, elite schedule (§4-§8).
- Building/upgrading/selling during a wave (the Lua allows it); only NEXT WAVE is build-phase-only.
- Elite spawns burst at slots 2..1+E (what the Lua actually does, §6.3).
- Element glyph geometry and palette; menu particle drift; ASCII-safe pixel art.

### 14.4 Decisions already resolved (no action needed)

| Item | Resolution |
|---|---|
| Path mechanic | **Dynamic BFS restored**; towers on road; blocked builds refused; enemies re-projected (L1-L3) |
| Enemy speed | **Absolute cells/s**, `SPEED_SCALE 0.25` (L4/D11) |
| Entrance/exit | Both unbuildable; entrance/exit messages (L2/D12) |
| Waves / victory | 25 waves, victory after 25, `totalWaves 25` (L5) |
| Lives / leaks | 20 lives, -1 per leak, no leak gold (L5) |
| Build phase | Player-paced between waves; building also allowed mid-wave (L6) |
| Element roster | Three only, no combining (§4.3) |
| Exact quotas | Preview must equal the wave; implementation uses wave size incl. elites (L8/D3, Q1 below) |
| Speed levels | 1/2/4 + pause (D5) |
| Controls | A confirm, B deselect, X/Y upgrade/sell, L/R type, Select speed, Start pause (D6/§10.2) |
| Tower cap | `MAX_TOWERS 96`, message `"tower limit"` |
| Sell refund | `floor(invested * 0.75)` (Lua-confirmed 75 %) |
| Message timer | 1.5 s wall-clock, no input swallowing (D9) |

### 14.5 Still genuinely ambiguous / needs sign-off

- **Q1 — wave size between the Lua and the port.** The Lua's actual spawn total was `(3+2w) + E` but its HUD quota was computed from `(3+2w) - E` (both wrong in different directions). The port chose total = `3+2w` including elites (`regular = total - E`), which keeps `game_wave_preview()` exactly truthful. This changes wave totals vs the Lua: wave 5 is 13 spawns (Lua 14), wave 10 is 23 (Lua 26), wave 25 is 53 (Lua 63). If the intended reading is "keep Lua spawn totals", the fix is confined to `core_wave_total_spawns()` + `start_wave()` + the §6.2 table; confirm which is wanted.
- **Q2 — entrance + exit protection.** The Lua let the entrance be built on (spawn-inside-tower bug). The port rejects both. Confirm this is the desired rule (it also makes `core_path_exists` simple).
- **Q3 — mazing strength / balance.** Absolute speed (D11) means a long maze multiplies the time under fire, but the current sim says the mechanic is **functional yet not mandatory**: both the "plain" AI (36 towers beside a 25-cell straight road) and the "maze" AI (59 towers, 95-cell road) win 25/25 with 0 leaks (3 seeds, `tools/sim.c`). `SPEED_SCALE` is a pacing knob, not a strategy separator (it only starts to separate playable-looking runs at absurd 8-16x scales where enemies tunnel through 1/60 s range checks). Making the maze necessary is a **content** decision (exponential HP growth, upgrade damage x1.5 instead of x2, or an economy squeeze) and needs the Lead's sign-off — see §14.6. Until then the maze is a strategic option/optimisation, and the initial straight road (only rows 10-11 can cover it) is the baseline.
- **Q4 — in-game help.** There is no help screen (Select is speed). The bottom UI has static hint lines. If a help overlay is wanted, use the paused overlay (it has room) — new UI work.
- **Q5 — audio.** Current `audio.c` is a stub; decide whether v1 ships SFX only (recommended) or includes a pre-decoded music loop. `gameover.wav` must be trimmed either way; the original `.wav`s are still on disk.
- **Q6 — blocked-build message text.** Port uses `"that would block the path"` (Lead's wording) rather than the Lua's `"Path blocked!"`; purely cosmetic, keep unless the original string is wanted for nostalgia.

### 14.6 Balance notes

- **Mazing is the intended core counterplay**: every extra road cell is extra time under fire at the current absolute speed (§5.3 table). A 35-cell example maze (28 towers) makes wave 1 take 7.0 s instead of 5.0 s; a 100-cell maze turns wave 1 into 20 s under fire.
- **Sim evidence (`tools/sim.c`, 3 seeds, current numbers):** both scripted players clear all 25 waves with 0 leaks — the "plain" AI (builds beside the road only, 36 towers, 25-cell road, 36 kills/wave income) and the "maze" AI (builds on the road using `core_path_length_with_block()`, 59 towers, final road 95 cells). CPU 0.03 s / 0.05 s for a whole run. So the maze mechanic works and is rewarded (less fire-time pressure, more safety margin) but is **not yet required**. Options to make mazing necessary, for sign-off (not applied): (1) exponential HP growth, e.g. `hp = 15 * 2^((wave-1)/3)` instead of linear; (2) upgrade damage multiplier x1.5 instead of x2 with a cheaper cost curve; (3) economy squeeze (e.g. kill reward 10 -> 5, tower cost 20 -> 25). `SPEED_SCALE` should stay 0.25 (or only be touched for pacing): it does not separate the strategies at playable speeds.
- The initial straight road is only reachable by towers in rows 10-11 and the road row itself (range 2.142857 cells; dy=1 covers +/-1.895 cells, dy=2 covers +/-0.769 cells, dy=3 out of range). Wave 1 with 150 gold (7 towers) is therefore tight; the player is expected to maze early.
- Kill income over a full run is 7,250 + 150 gold, while one level-10 tower costs 10,240: the intended curve is many mid-level towers, not one maxed tower.
- Wave 25 (53 spawns incl. 10 elites, interval 1.75 s, elite HP 217.5) is the intended finale; on a well-mazed board the time-under-fire is what decides it.
- Enemy HP grows linearly while tower damage doubles per level, so late-run damage is hugely overkill; the gold budget is the limiting factor, as intended.
- Leaks cost both a life and the 10-gold bounty (double punishment, D2); no wave-completion bonus exists.

---

## 15. Implementation guide (actual repo layout)

### 15.1 File responsibilities `[CODE]`

| File | Owns |
|---|---|
| `include/game.h` | frozen core API, `Game`/`Tower`/`Enemy`/`Beam`/`Path` structs, phases, events, constants. Append-only additions: `stepAccumulator`, `spawnQuota` |
| `include/gfx.h` | two-screen immediate drawing API: clear/rect/circle/line/triangle + 8x8 bitmap text |
| `source/core/core_internal.h` | private core contract: start/goal, fixed-step/message/beam constants, BFS/path queries, speed formula, wave formulas |
| `source/core/data.c` | content tables (`TowerSpec`, `EnemySpec`), damage matrix, per-level stats, upgrade cost, sell value, wave formulas, `core_enemy_speed_cells()` + `SPEED_SCALE 0.25` |
| `source/core/grid.c` | **BFS road** (`core_build_path`, `core_path_exists`, `core_path_length_with_block`), path contains/position, cell centres, placement legality (`game_can_place_at`), `game_tower_at` |
| `source/core/game.c` | lifecycle, fixed-step update, spawning/quotas, movement/leaks, **re-projection**, towers/beams, wave completion, actions (`place/upgrade/sell/start wave`), messages, RNG, economy, lives |
| `source/render.c` / `.h` | top screen: gradient, HUD strip, board + live path + shoulders + entrance/exit arrows, towers, range circles, beams, enemies, cursor/ghost, menu/pause/result overlays, shared pixel-art helpers |
| `source/ui.c` / `.h` | bottom screen: header + speed/pause badges, tower palette, info/upgrade panel, NEXT WAVE/incoming preview, wave panel, paused/result/menu screens, touch hit-testing (`UiEvent`) |
| `source/app.c` / `.h` | scene state machine (MENU/BUILD/WAVE/paused/result), input -> action mapping, particles, event dispatch to audio, draw both screens |
| `source/main.c` | `aptMainLoop`, `hidScanInput` polling, frame timing |
| `source/plat_3ds.c` | citro2d/citro3d backend for `gfx.h` |
| `source/audio.c` | audio stub (records `GAME_EVT_*`, no playback yet); the ndsp mixer slots in behind the same API |
| `tests/test_core.c` | host tests (39 tests) incl. path/BFS, maze long route, blocked placement, reprojection, speed-vs-path-length, waves, victory, fail states, events/messages, capacity |
| `tools/sim.c` | headless auto-play balance sim with two players: `sim.exe` (plain, builds beside the road) and `sim.exe --maze` (greedy mazing using `core_path_length_with_block()`); see §14.6 for results |
| `tools/preview/*` | PC software-renderer preview harness (`gfx.h` backend writing PNGs; supports top-screen click debug) |

### 15.2 Content tables to implement (exact values, `data.c`)

```
TowerSpec[3] = {
  { "Fire",   20, 3.0f, 0.8f, 2.142857f, {0.95,0.15,0.15}, {0.95,0.30,0.10} },
  { "Water",  20, 3.0f, 0.8f, 2.142857f, {0.15,0.40,0.95}, {0.10,0.50,0.95} },
  { "Forest", 20, 3.0f, 0.8f, 2.142857f, {0.15,0.90,0.15}, {0.10,0.90,0.30} },
}
EnemySpec[3] = { "Fire", "Water", "Forest" } with the same colours/glows
game_tower_stats(type, level, *damage, *fireRate, *range):
  damage   = 3 * (1 << (level-1))     // 3 .. 1536
  fireRate = {0.80,0.72,0.64,0.57,0.51,0.45,0.40,0.36,0.32,0.28}[level-1]
  range    = 2.142857f                // constant
core_upgrade_cost_for_level(L) = L >= 10 ? -1 : 20 << (L-1)
game_sell_value(g,i)           = tower.invested * 3 / 4
game_damage_multiplier(t,e)    = { {1,.5,2}, {2,1,.5}, {.5,2,1} }[t][e]
core_wave_base_enemies(w)      = 3 + 2*w
core_wave_elite_count(w)       = w%25==0 ? 10 : w%10==0 ? 3 : w%5==0 ? 1 : 0
core_wave_total_spawns(w)      = base(w)            // includes elites (D3/Q1)
core_wave_spawn_interval(w)    = max(0.8, 3.0 - 0.05*w)
core_enemy_hp(w, elite)        = (15 + (w-1)*3) * (elite ? 2.5 : 1)
core_enemy_speed(w, elite)     = (20 + (w-1)) * (elite ? 1.8 : 1)      // Lua units
core_enemy_speed_cells(w, e)   = core_enemy_speed(w,e) * 0.25f        // cells/s (D11)
```

### 15.3 Lifecycle `[CODE]`

- `game_init(g)`: `memset`, `PHASE_MENU`, wave 1, totalWaves 25, gold 150, lives 20, `selectedType = FIRE`, `selectedTower = -1`, speed 1, cursor `(0,0)`, fixed non-zero RNG, spawnInterval for wave 1.
- `game_new_run(g, seed)`: `game_init` then `PHASE_BUILD`, wave 1, gold/lives, speed 1, `seed` (0 -> fixed), `core_build_path()` (initial straight row-12 road), spawnInterval. Cursor stays `(0,0)`.
- `game_start_next_wave(g)`: ignored unless BUILD and not paused.

### 15.4 Step/update contract `[CODE]`

- `game_update(dt)`: clamp to 0.25 s; if paused only `tick_message(dt)`; else accumulate and run up to 6 `game_step(1/60)`; drop leftover on cap.
- `game_step(dt)`: §1.5 exactly; no allocation; deterministic given `rng`; `dt` clamped to 0.25 and multiplied by speed inside.
- Events/messages are state; the platform clears events each frame (`app.c` -> `audio_play_event()` -> `game_clear_events()`).

### 15.5 Action contracts `[CODE]`

| Function | Contract | Failure messages |
|---|---|---|
| `game_place_tower` | §2.5; uses `selectedType`; auto-selects new tower; rebuilds road + reprojects | `can't build now`, `can't build there`, `can't build on the entrance`, `can't build on the exit`, `cell occupied`, `tower limit`, `that would block the path`, `not enough gold` |
| `game_upgrade_selected` | level<10, gold>=cost; `invested += cost`; event UPGRADE | `can't upgrade now`, `no tower selected`, `max level`, `not enough gold` |
| `game_sell_selected` | refund `invested*3/4`, remove+compact, rebuild occupancy + road, reproject, clear selection, event SELL | `can't sell now`, `no tower selected` |
| `game_select_tower` | valid index selects, anything else clears | — |
| `game_set_cursor` | clamps | — |
| `game_set_selected_type` | ignores invalid | — |
| `game_toggle_pause` | toggles; resets step accumulator | — |
| `game_cycle_speed` | 1 -> 2 -> 4 -> 1 | — |
| `game_can_place_at` | pure predicate incl. hypothetical BFS, no gold check, no messages | — |
| `game_clear_events` / `game_clear_message` | zero the fields | — |

### 15.6 Dynamic road + re-projection reference

BFS (fixed arrays, no allocation; exact neighbour order up/down/left/right):

```
visited[13][26]=false; parent[338]=-1; queue[338]; head=tail=0
queue[tail++] = index(0,12); visited[0][12]=true
while head<tail:
    cur=queue[head++]; if cur==index(25,12): found; break
    for (dc,dr) in {(0,-1),(0,1),(-1,0),(1,0)}:
        n=(cur%26+dc, cur/26+dr)
        skip OOB / visited / wall(occupancy or hypothetical block)
        visited=true; parent[n]=cur; queue[tail++]=n
if !found return 0
walk parent from goal -> start, count cells, write centres -> Path.pts, length = count-1
```

Re-projection after every rebuild:

```
for each active enemy:
    bestT = 0; bestD2 = INF
    for i in 0..path.count-2:
        t = clamp(dot(e.xy - a, b-a)/|b-a|^2, 0, 1)      // |b-a| == 1 by construction
        d2 = |e.xy - (a + t*(b-a))|^2
        if d2 < bestD2: bestD2 = d2; bestT = i + t
    e.pathPos = clamp(bestT, 0, path.length)
    e.xy = path_position(path, e.pathPos)
```

Initial road: `(0,12),(1,12),...,(25,12)` — 26 cells / 25 segments. Example 28-tower maze (measured with this BFS): path length 35; grid in §11.4B. Single tower at `(7,12)` -> length 27 (test-verified).

### 15.7 Acceptance tests (39 host tests currently passing; the key ones this spec pins)

| Test | Expected |
|---|---|
| initial path | count 26, length 25.0, all centres `(i+0.5, 12.5)`, `pathBlocked == false`, start/exit centres `(0.5,12.5)` / `(25.5,12.5)` |
| build on road | place at `(7,12)`: `towerCount 1`, gold 130, `game_tower_at(7,12)==0`, path length 27, `(7,12)` not on road, `(7,11)` or `(7,13)` is |
| blocking placement | after fence `(24,12)`, `(25,11)` refused: no gold change, no tower, occupancy empty, message contains "block", road intact; selling the fence reopens it |
| entrance/exit | `game_can_place_at` false for `(0,12)`/`(25,12)`; messages contain "entrance"/"exit"; no gold/tower change |
| sell | selling `(7,12)` restores length 25 and refunds 15 (gold 130+15) |
| speed vs path length | wave 1 after 1 s: `pathPos == 5.0` on both a straight and a much longer mazed board (absolute speed D11) |
| re-projection | a mid-wave build/sell reroutes successfully and leaves every enemy with `0 <= pathPos <= path.length`, no teleport to entrance, no crash |
| world positions | `game_cell_center(25,12) == (25.5,12.5)` |
| wave composition | waves 1/4/5/10/25 -> spawns 5/11/13/23/53, elites 0/0/1/3/10, fire 1/3/4/6/14, water same, forest 3/5/4/8/15; preview sum == spawns; quotas staged; `enemiesToSpawn == size` |
| spawn intervals | waves 1/4/5/10/25/60 -> 2.95/2.80/2.75/2.50/1.75/0.80 |
| elite stats | wave 5 regular HP 27, elite HP 67.5, elite in spawn slot 2 |
| exact quotas | spawn everything with a tiny interval, count elements/elites == preview |
| victory | clearing waves 1..25 leaves `phase == PHASE_VICTORY`, `wave == 26`, `totalWaves == 25` |
| maze long route | scripted walls at cols 4/8/12/16 with alternating row gaps (48 towers) -> path length > 60, `pathBlocked == false`, integrity holds |
| idle player loses | no towers, auto-start every BUILD: exactly 20 leaks, lives 0, `PHASE_GAMEOVER`, `wave == 3` (5+7+8 leaked) |
| game-over freezes | GAMEOVER freezes `waveTime`/`kills`; `game_step(5.0)` and `game_start_next_wave()` do nothing |
| fixed-step accumulator | `game_update(5/240)` advances 1/60 s; `game_update(10.0)` caps at 6 steps and drops the rest; five `1/240` updates accumulate to 1/60 s |
| events | BUILD/UPGRADE/SELL/WAVE_START/LASER/ENEMY_DIE bits set and OR-accumulated until `game_clear_events` |
| messages | failure sets `message`, `messageTimer == 1.5`; self-expires (BUILD ticks it); `game_clear_message` zeroes; long text truncates safely |
| capacity | full enemy array pauses spawning (nothing lost); full beam array drops the VFX but still damages; full tower array rejects with `"tower limit"` |
| pause / speed | pause freezes the sim and resets the accumulator; `game_cycle_speed` 1 -> 2 -> 4 -> 1 |
| selection / cursor | `selectedType` ignores invalid values; cursor clamps; placing auto-selects the new tower; `game_select_tower(-1)` clears |
| placement guards | MENU/VICTORY reject placement with a message; `game_can_place_at` false there too |
| upgrade curve | costs 20..5120, `invested == 20 * 2^level` after reaching level L, max-level message, damage 1536 |
| determinism | same seed + same call sequence -> byte-identical `Game` (39 tests total) |

### 15.8 Performance / memory

- `sizeof(Game) == 12364` bytes (~12 KB) — keep the single instance static/global, not on the stack (`NOTES.md`).
- Core step: O(towers + enemies) with fixed pools; no allocations; squared-distance range checks.
- Path BFS: <=338 cells x 4 neighbours, ~1.7 KB of stack; runs on placement/sale plus a per-frame cursor legality query — trivial at 60 fps.
- Renderer: <= ~1500 primitives/frame (§11.5); 8x8 bitmap text; the game is reported at a stable 60 fps in the Azahar emulator.
- Audio: stub today; when wired, <=8 SFX channels + 1 music stream; 0.08 s laser throttle mandatory.

---

## Appendix A — constants reference

```c
/* grid / road (include/game.h + core_internal.h) */
#define GRID_COLS 26
#define GRID_ROWS 13
#define MAX_PATH_CELLS (GRID_COLS * GRID_ROWS)   /* 338 */
#define CORE_START_COL 0
#define CORE_START_ROW 12
#define CORE_GOAL_COL 25
#define CORE_GOAL_ROW 12
#define CORE_TOTAL_WAVES 25
#define CORE_KILL_REWARD 10
#define CORE_MESSAGE_TIME 1.5f
#define CORE_BEAM_LIFE 0.15f
#define CORE_HIT_FLASH_TIME 0.12f
#define CORE_FIXED_STEP (1.0f/60.0f)
#define CORE_MAX_STEPS_PER_UPDATE 6
#define CORE_MAX_UPDATE_DT 0.25f
#define CORE_MAX_STEP_DT 0.25f
#define SPEED_SCALE 0.25f                        /* cells/s per Lua speed unit */

/* screen layout (render.h / ui.c) */
#define BOARD_CELL 15.0f
#define BOARD_X 5.0f
#define BOARD_Y 38.0f
#define BOARD_W 390.0f
#define BOARD_H 195.0f
#define HUD_H 38.0f
/* bottom-screen strips: header 32, palette y34 h52, panel x4 y89 w312 h66,
   bottom y159 h77; buttons per §11.3 */

/* economy */
#define STARTING_GOLD 150
#define STARTING_LIVES 20
#define TOWER_COST 20
#define MAX_LEVEL 10
#define MAX_TOWERS 96
#define MAX_ENEMIES 128
#define MAX_BEAMS 128

/* per-level tables */
static const int   UPGRADE_COST[10] = {20,40,80,160,320,640,1280,2560,5120,0};
static const float FIRE_RATE[10]    = {0.80f,0.72f,0.64f,0.57f,0.51f,0.45f,0.40f,0.36f,0.32f,0.28f};
static const int   DAMAGE[10]       = {3,6,12,24,48,96,192,384,768,1536};
/* invested = 20 << (level-1); sell = invested * 3 / 4 */
```

## Appendix B — renderer cheat-sheet

```
cell -> top px:     px = 5 + cellX * 15;  py = 38 + cellY * 15;  cell = 15 px
range radius:       2.142857 * 15 = 32.14 -> 32 px
road:               for i in 0..path.count-1: fill cell floor(path.pts[i]);
                    gradient (0.24+0.10t, 0.14+0.05t, 0.05+0.03t), t = i/(count-1);
                    1 px (0.38,0.25,0.10,0.85) shoulder where road borders buildable
entrance/exit:      green arrow (0.35,1.00,0.45) at path.pts[0] along pts[1]-pts[0];
                    red arrow (1.00,0.35,0.30) at last point along last-prev;
                    triangle apex 6 px, base 3 px back, half-width 4.5 px, black backing
cursor tiles:       white = tower; green = legal+affordable; yellow = legal, no gold;
                    dim = illegal (entrance/exit or would seal the road)
HUD (top 400x38):   coin(14,19,6)+gold(26,11,s2); heart(100,19,6)+lives(112,11,s2);
                    phase tag(160,5,s1); wave(160,17,s2 "%d/%d" clamped);
                    ALIVE/TOWERS(244,5,s1)+count(244,17,s2); SPEED(330,5,s1)+value(330,17,s2)
tower icon radius:  round((12 + min(level,10)*0.4) * 15/28) -> 7/8/9 px
enemy icon radius:  5 normal / 9 elite; health bar 13 x 3 (fill 11 x 2)
beam:               1 px at a=life/0.15, 3 px at a*0.1
text:               8x8 font, scale 1 = 8 px, scale 2 = 16 px; (x,y) top-left
button text:        scale 1 fits 40 chars across 320 px
```

## Appendix C — dynamic road reference data

**Initial road (empty board):** `(0,12) -> (25,12)`, 26 cells, `length = 25.0`, path centres `(i+0.5, 12.5)`.

**Test fixtures:**
- One tower at `(7,12)` -> path length **27**; detour through row 11 (test also accepts row 13; BFS neighbour order picks row 11 first).
- Fence: tower at `(24,12)` legal; then `(25,11)` refused ("would block the path") because the exit would have no free neighbour; selling `(24,12)` reopens it.
- Example maze (28 towers, path length **35**) — see the §11.4B grid.

**BFS algorithm/order (must match Lua for deterministic detours):** FIFO from exactly `(0,12)` to `(25,12)`; neighbours expanded **up, down, left, right**; walls = occupied cells (+ hypothetical block); visited at enqueue; parent walk from goal back to start; path written start -> goal.

## Appendix D — Lua reference index

| Topic | `main.lua` lines |
|---|---|
| Window config / screen constants / grid + offsets | 11-52 |
| Pathfinding (BFS, updatePath) | 54-123 |
| Tower types / enemy types / damage matrix | 125-179 |
| Global state variables | 181-226 |
| Sounds + helpers | 228-287 |
| Pixel-art icon functions | 289-435 |
| Helpers (grid pos, distance, path position, isPathCell, error, speed text, getTotalCost) | 445-508 |
| `startWave` / random enemy type | 510-576 |
| Menu particles | 578-615 |
| `love.update` (full simulation) | 617-799 |
| Main menu / controls overlay | 801-918 |
| Game area drawing | 920-1040 |
| HUD panel drawing | 1042-1267 |
| `love.draw` | 1269-1283 |
| `love.keypressed` (input, build/validate/rollback, economy) | 1285-1458 |
| `love.touchpressed` | 1460-1488 |
| `love.load` | 1490-1519 |
