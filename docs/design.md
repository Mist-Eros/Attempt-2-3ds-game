# Elemental Defense — 3DS Port Specification (v3)

**Single source of truth for the C rewrite (devkitARM + libctru + citro2d).**

| Item | Value |
|---|---|
| Original source | `C:\Users\Raine\Desktop\ElementalDefense\game\main.lua` (LÖVE2D, 1519 lines, 51,302 bytes) — read in full |
| Original audio | `C:\Users\Raine\Desktop\ElementalDefense\game\sounds\` (16 files, §12) |
| Engine contract | `include/game.h` (core, cell space, frozen API), `include/gfx.h` (renderer, 8x8 bitmap font) |
| 3DS target | top 400x240 (landscape, display only), bottom 320x240 (touch), 60 Hz fixed step |
| This document | `Attempt-2-3ds-game/docs/design.md` |

**v3 changelog:** locked serpentine path (76 cells / 75 segments, start `(0,1)`, end `(25,9)`) replaces the straight default; `PHASE_BUILD` is player-paced with no timer; `L/R = tower type`, `Select = speed`, `Start = pause`; module layout in §15 now matches the real repo (`source/core/{game,grid,data}.c`, `include/*.h`, `source/render.c`, `source/ui.c`, `source/plat_3ds.c`, host preview `tools/preview/*`); open questions trimmed to the genuinely ambiguous ones. **v2:** incorporated the Lead's locks and the `include/game.h` cell-space contract. All Lua-derived numbers are unchanged.

**Tagging convention**

- `[LUA]` — value/behaviour copied **verbatim** from `main.lua`.
- `[DERIVED]` — computed exactly from a `[LUA]` formula.
- `[PORT]` — a new decision (locked or required by the engine contract); every one is listed in §14.2/§14.4.
- `[LOCKED]` — a deliberate deviation the Lead has already decided; do not "fix back" to the Lua.

---

## 0. Locked design decisions, conventions, engine contract

### 0.1 Locked decisions (authoritative, from the Lead)

| # | Decision | Lua original (for the record) |
|---|---|---|
| L1 | **Fixed path; no re-pathing.** The path is a hardcoded const array (hand-authored serpentine, §2.3), built once and never recomputed. | BFS recomputed on every build/sell; building on path cells redirected enemies (`main.lua:63-123`, `1362-1364`, `1452`) |
| L2 | **Towers may only be placed on non-path cells.** No "build on the path to make a maze". | Building on path cells was allowed if a path still existed (`main.lua:1350-1376`) |
| L3 | **Explicit build phase between waves, player-paced.** After a wave resolves, phase = `PHASE_BUILD` until the player taps NEXT WAVE. No auto-chaining, no inter-wave timer. | Waves auto-chained in the same frame with no pause (`main.lua:794-798`) |
| L4 | **20 lives. Every leaked enemy costs one life** (elites included, -1). At 0 lives -> game over. No gold for leaks. | No lives at all; leaks paid **+2 gold** (+10 elite) and restarted the same wave forever (`main.lua:698-707`, `779-798`) |
| L5 | **Victory after clearing wave 25.** | Endless waves, no victory condition (`main.lua:779-798`) |
| L6 | Damage, multipliers, costs, rewards, upgrade curve, enemy HP/speed formulas: **keep the Lua numbers** (this spec). | — |
| L7 | Original's missing/looping game-over state is documented as a defect and replaced by the L3/L4/L5 flow. | `gameState` only ever `MENU`/`PLAYING`; `gameover.wav` loaded but never played (`main.lua:263`) |

### 0.2 Conventions

- Lengths inside the C core are **cells**: 1.0 = one grid cell. `include/game.h` is cell space; `include/gfx.h` is pixels. Convert only in the renderer.
- Ranges/costs/times stay in real units as stated; the original 28 px cells are only used to derive cell-space ranges: `60 px / 28 px` = **2.142857 cells**.
- Times are seconds; the simulation advances in fixed 1/60 s steps (§1.6).
- Colours `[LUA]` floats 0..1 map directly to `GfxColor` and `TowerSpec.color[]` / `EnemySpec.color[]`.
- Original screen 1280x720; 3DS top 400x240 (display only), bottom 320x240 (touch). `gfx.h`: origin top-left, +y down.
- The Lua has two state variables: outer `gameState` (screens) and inner `state` (cursor/build mode). This doc calls them **phase** and **build mode**; the C core uses `GamePhase` + `selectedTower`/`placing` (§1).
- Markers `[LUA]`, `[DERIVED]`, `[PORT]`, `[LOCKED]`.

### 0.3 Engine contract (`include/game.h`) — how this spec maps onto it

`include/game.h` is **frozen** (engine-core is already writing `source/core/{game,grid,data}.c` against it). This spec must be read with it:

| `game.h` item | Value / meaning | Spec section |
|---|---|---|
| `GRID_COLS`, `GRID_ROWS` | 26, 13; cell `(col,row)` 0-based, top-left; centre `(col+0.5,row+0.5)` | §2 |
| `GamePhase` | `PHASE_MENU`, `PHASE_BUILD`, `PHASE_WAVE`, `PHASE_GAMEOVER`, `PHASE_VICTORY` | §1 |
| `STARTING_GOLD`, `STARTING_LIVES` | 150, 20 | §7 |
| `Game.wave`, `Game.totalWaves` | `wave` = wave currently being played, or the next wave during `PHASE_BUILD`; `totalWaves = 25` | §1.4, §6 |
| `Game.speedMultiplier` | 1.0 / 2.0 / 4.0 (`game_cycle_speed`); pause is the separate `paused` flag | §1.7 |
| `TowerSpec` | `name`, `cost`, `damage`, `fireRate`, **`range` in cells** (2.142857f), `color[3]`, `glow[3]` | §3, §8 |
| `game_tower_stats` | effective `damage`, `fireRate`, `range` at a level (range constant) | §8 |
| `game_upgrade_cost`, `game_sell_value` | upgrade cost curve / 75 % refund | §8, §7 |
| `game_damage_multiplier` | element matrix returning 2.0 / 1.0 / 0.5 | §4 |
| `game_wave_preview` | next-wave Fire/Water/Forest/Elite counts (truthful -> exact quotas, §6.4) | §6 |
| `Enemy.pathPos` | distance travelled along path **in cells** (0 .. `path.length`) | §5.3 |
| `Path.length` | **75.0** cells for the locked serpentine path (76 cells, 75 segments) | §2.3 |
| `MAX_TOWERS 96`, `MAX_ENEMIES 128`, `MAX_BEAMS 128`, `MAX_ELITE_SPAWNS 16` | capacity limits; see §2.5 (tower cap message) and §6.8 | §2.5, §6.8 |
| `Game.events` bit mask | audio/feedback triggers; map in §12.1 | §12 |
| `game_init` / `game_new_run` | menu vs fresh run | §1.4 |
| `game_place_tower` / `game_upgrade_selected` / `game_sell_selected` / `game_start_next_wave` / `game_set_selected_type` / `game_select_tower` / `game_set_cursor` / `game_toggle_pause` / `game_cycle_speed` | player actions; return false + `Game.message` when invalid | §3.5, §10 |
| `Game.message` + `messageTimer` | transient UI text (toast), `MAX_MESSAGE_LEN 48` | §9.7 |
| `Game.pathBlocked` | **always false** in this design (no re-pathing); kept for API compatibility | §2.3 |
| `stepAccumulator` | real dt -> fixed 1/60 steps inside `game_update` | §1.6 |

**Contract addenda (append-only fields; `game.h` allows appending, engine-core owns it)** — needed for a clean implementation:
1. `bool placing` in `Game` — whether a tower type is armed for placement by touch (`selectedType` alone cannot express "armed vs just highlighted").
2. Wave spawn bookkeeping in `Game` (or an appended wave-context struct): `int quotaLeft[TOWER_TYPE_COUNT]; int eliteSpawned; int spawnIndex;` so exact element quotas and the elite block are state, not globals (the core must support two `Game` instances).
3. `float laserSfxCooldown` (or the platform throttles `GAME_EVT_LASER` to 0.08 s; the Lua throttles globally, §12.1).
4. Optional `float enemySpeed`; not required — velocity can be recomputed from `wave` each step (§5.3; waves never overlap).

### 0.4 The game in one paragraph

A 26x13 grid tower defense with 25 waves. Enemies enter at cell `(0,1)` and follow a **fixed hand-authored serpentine path** — right along row 1 to col 23, down to row 5, left to col 2, down to row 9, then right to the exit at `(25,9)` — and every leak costs one of 20 lives. Between waves the player builds and upgrades: three elemental towers (Fire/Water/Forest — identical base stats, rock-paper-scissors multipliers), 10 levels each, hitscan attacks with a 0.15 s laser visual. Kills pay 10 gold; start 150 gold. Waves grow linearly (spawn count, HP, speed) with elites on waves divisible by 5/10/25. Clear wave 25 for victory.

---

## 1. Game loop & state machine

### 1.1 Original Lua update order (reference — keep this simulation order)

The Lua's per-frame order is the reference for the C `game_step` (§1.5); it is kept because spawn-before-move and move-before-fire decide whether a freshly spawned enemy can be hit in its first frame.

```
 1. if phase == MENU: update menu particles only; return
 2. dt = dt * gameSpeed
 3. laserSoundCooldown = max(0, laserSoundCooldown - dt)
 4. errorTimer = max(0, errorTimer - dt)
 5. gameTime += dt
 6. beam/laser lifetime -= dt; remove dead
 7. wave start check (Lua auto-chain; replaced by L3 build phase)
 8. spawn timer: when >= interval and spawns remain -> spawn one enemy (elite schedule §6.3)
 9. enemies: pathPos += dt * speed; if t >= 1 -> leak (Lua pays gold + retries; L4: -1 life)
10. towers (column-major over grid): cooldown -= dt; find nearest enemy in range; fire -> damage
    -> beam; kill -> +10 gold; remove enemy
11. wave completion check; Lua retries or advances (replaced by L3/L5)
```

### 1.2 Original outer/inner states (audit of `main.lua`)

| Layer | Values in Lua | Notes |
|---|---|---|
| Outer, `gameState` | `"MENU"`, `"PLAYING"` only | no pause, no game over, no victory, no restart (`main.lua:184,621,1273,1289,1464`) |
| Inner, `state` | `"MOVING"`, `"BUILDING"`, `"TOWER_SELECTED"` | cursor/build mode, `main.lua:188` |
| Speed | `speedLevels {0, 0.5, 1, 2, 4}` | `0` is the "pause" hack; 1/2/4 match the engine header, 0.5 is dropped (locked, D7) |
| Controls overlay | `showControls` bool | help screen in both states (`main.lua:204`, `883-918`) |

### 1.3 Missing states in the Lua (documented defects)

| Missing | Evidence | Consequence in the original |
|---|---|---|
| lives | no such variable anywhere | leaks unpunished |
| game over | `gameState` never becomes anything else | run never ends |
| victory / final wave | no wave cap | endless |
| pause menu | only `gameSpeed = 0` via `L` twice | no overlay, no resume |
| return to menu / restart | no transition back to `"MENU"` | only LÖVE's default Escape quits |
| mute | `soundEnabled` never toggled | no mute |
| boss enemy | elites are a modifier only | — |
| game-over sound | `gameover.wav` loaded (`main.lua:263`), never played | dead asset |

The **missing/looping game-over state** is the headline defect: the run can neither be won nor lost, and leaking restarts the same wave forever while paying gold.

### 1.4 `[LOCKED]` 3DS phase machine (matches `GamePhase`)

| Phase | Entered by | Simulation | Player actions | UI |
|---|---|---|---|---|
| `PHASE_MENU` | `game_init`; quit-to-menu from overlays | none (menu particles, renderer-local) | tap START -> `game_new_run` | title art, element icons, START/CONTROLS on the touch screen |
| `PHASE_BUILD` | `game_new_run`; completion of any wave < 25 | none: no enemies, no spawning; timers idle; **player-paced, no countdown** | build / upgrade / sell / change type / move cursor / NEXT WAVE -> `PHASE_WAVE`; pause | field + HUD `BUILD`; bottom panel; wave preview of `wave` |
| `PHASE_WAVE` | `game_start_next_wave` | full §1.5 update | build/upgrade/sell still allowed (matches Lua); pause | HUD `ACTIVE`, `LEFT` counter, enemies |
| `PHASE_GAMEOVER` | `lives <= 0` after a leak | frozen | RETRY -> `game_new_run`; MAIN MENU -> `game_init` | top overlay `GAME OVER` + stats; bottom RETRY/MENU |
| `PHASE_VICTORY` | wave 25 fully resolved with `lives > 0` | frozen | RETRY / MAIN MENU | top overlay `VICTORY` + stats; bottom RETRY/MENU |

Transition table (`[LOCKED]`):

| From | Trigger | To | Side effects |
|---|---|---|---|
| `MENU` | bottom START tap | `BUILD` (`wave = 1`) | `game_new_run`: gold 150, lives 20, totalWaves 25, wave 1, clear towers/enemies/beams, cursor = `(1,0)`, `selectedType = 0`, `selectedTower = -1`, `placing = false`, `speedMultiplier = 1`, start game music |
| `BUILD` | NEXT WAVE tap / `game_start_next_wave` | `WAVE` | init wave spawn state for `wave` (§6), `GAME_EVT_WAVE_START`, `wave_start.wav` |
| `WAVE` | last enemy of the wave resolved: `enemiesToSpawn == 0 && enemiesAlive == 0` | `BUILD` or `VICTORY` | if `wave >= totalWaves` (25) -> `PHASE_VICTORY` + `GAME_EVT_VICTORY`; else `wave += 1`, `PHASE_BUILD` |
| `WAVE` | leak takes lives to 0 | `GAMEOVER` | clear/freeze; `GAME_EVT_GAMEOVER`; `gameover.wav`; stop game music |
| `GAMEOVER` | RETRY tap | `BUILD` (`wave = 1`) | same reset as `game_new_run` |
| `GAMEOVER` | MAIN MENU tap | `MENU` | `game_init`; menu music |
| `VICTORY` | RETRY / MAIN MENU | `BUILD` / `MENU` | as above |
| `BUILD`/`WAVE` | Start button / `game_toggle_pause` | same phase, `paused` toggles | freeze/unfreeze update; overlay |
| `BUILD`/`WAVE` | Select button / SPEED button / `game_cycle_speed` | same phase | `speedMultiplier` 1 -> 2 -> 4 -> 1 |

`totalWaves = 25` is set by `game_new_run`. `Game.wave` semantics: during `BUILD` it is the wave that NEXT WAVE will launch (initially 1); during `WAVE` it is the wave in progress; after the final wave resolves it stays `25` == `totalWaves` and the phase becomes `VICTORY` (matches the header comment).

### 1.5 `[LOCKED]` `game_step(dt)` order (C core)

`game_update(g, realDt)` accumulates real dt into `stepAccumulator` and calls `game_step(1/60)` until the accumulator is drained (cap the accumulator at 0.1 s to avoid a spiral of death after a hitch; drop excess). `game_step` must be deterministic and allocation-free.

```
game_step(dt):
  if phase == MENU or phase == GAMEOVER or phase == VICTORY: return (no simulation)
  if paused: return
  dt *= speedMultiplier
  messageTimer = max(0, messageTimer - dt)
  laserSfxCooldown = max(0, laserSfxCooldown - dt)
  for beams: life -= dt; deactivate when <= 0
  if phase == WAVE:
      # 1) spawning
      if enemiesToSpawn > 0:
          spawnTimer += dt
          if spawnTimer >= spawnInterval:
              spawnTimer = 0                      # Lua resets to 0, do not subtract
              spawn_one_enemy(g)
      # 2) enemy movement + leaks
      for each active enemy (index order, no reordering):
          enemy.pathPos += enemyVelocity(g, enemy) * dt
          if enemy.pathPos >= path.length: leak(g, enemy)
          else enemy.x, enemy.y = path_position(g, enemy.pathPos)
      # 3) towers, column-major: for col 0..25, for row 0..12
      for each occupied cell:
          tower.cooldown = max(0, tower.cooldown - dt)
          target = nearest active enemy with distance <= RANGE_CELLS
          if target and cooldown <= 0:
              cooldown = fireRate(level)
              dmg = damage(level) * game_damage_multiplier(type, target.element)
              target.hp -= dmg; target.hitFlash = HIT_FLASH_SEC
              spawn Beam from tower centre to target position (life 0.15)
              event LASER (platform throttles audio)
              if target.hp <= 0: gold += 10; kills += 1; remove enemy; event ENEMY_DIE
      # 4) completion
      if enemiesToSpawn == 0 and enemiesAlive == 0:
          if wave >= totalWaves: phase = VICTORY; event VICTORY
          else: wave += 1; phase = BUILD
```

Notes:
- `enemyVelocity(g, e) = path.length * (20 + (wave-1)) * (e->elite ? 1.8f : 1.0f) / 100.0f` cells/s (§5.3). Safe because waves never overlap.
- Removal from the enemy array must preserve spawn order (`memmove`/tombstone scan), so nearest-target tie-break = earliest spawned, like the Lua.
- Beam positions are snapshot cell-space coordinates at fire time (the Lua does not track the target afterwards).
- `enemiesAlive` / `enemiesToSpawn` are maintained counters (header fields).
- Leak: `lives -= 1; leaks += 1; event LEAK;` remove enemy; if `lives <= 0` -> `phase = GAMEOVER; event GAMEOVER` (L4).

### 1.6 Fixed step (`include/game.h` contract)

- `game_update(g, dt)` converts real time to fixed steps: canonical `SIM_STEP = 1.0f/60.0f`.
- Speed multiplies **inside** `game_step` (after the phase checks), matching the Lua's `dt = dt * gameSpeed` placed after the menu return.
- Pause freezes everything that uses scaled dt (spawn timer, enemy movement, cooldowns, beam life, message timer, laser-sound cooldown) — identical to the Lua's `gameSpeed = 0`.
- The renderer may run at whatever rate the 3DS loop offers; simulation stays at 60 steps/s.
- `game_step(g, dt)` is also the unit-test entry point (one step).

### 1.7 Pause & speed `[LOCKED]`

- `paused` bool (header) toggled by **Start** / `game_toggle_pause`. Pause overlay on both screens.
- `speedMultiplier`: 1.0, 2.0, 4.0 via **Select** / the SPEED button / `game_cycle_speed`; HUD shows `1x`/`2x`/`4x`, and `PAUSED` while paused.
- The Lua's 0.5x and 0x speed levels are dropped (0x is replaced by `paused`); recorded as deviation D7.

---

## 2. Grid, fixed path & cell space

### 2.1 Grid constants

| Constant | Value | Notes |
|---|---|---|
| `GRID_COLS`, `GRID_ROWS` | 26, 13 | `[LUA]` grid 26x13 |
| Core cell space | 1.0 = one cell | `include/game.h` |
| Cell `(col,row)` | 0-based, `col 0..25` left->right, `row 0..12` top->bottom | `game_cell_center = (col+0.5, row+0.5)` |
| Original px cell | 28 px (1280x720) | only used to derive range: 60/28 = 2.142857 cells |
| Top-screen px cell | **15 px** (390x195 field) | `FIELD_X=5`, `FIELD_Y=24` (§11) |
| Bottom-screen map cell | **12 px** (312x156 map) | `MAP_X=4`, `MAP_Y=4` (§11) |
| Start cell | `(0,1)` = Lua `(1,2)` | enemies spawn at centre `(0.5,1.5)` |
| End cell | `(25,9)` | leak at `pathPos >= path.length` (75) |
| Path cells | **76** | locked serpentine, §2.3 |
| Buildable cells | **262** (338 total - 76 path cells) | tower cap is 96 (§2.5) |

Index-order warning: `game.h` stores `occupancy[GRID_ROWS][GRID_COLS]`, i.e. `[row][col]`, while the Lua uses `gridData[col][row]`. The renderer/core must be consistent: **C uses `[row][col]`**; the Lua loops in §1.1 were column-major (`for col ... for row`), keep that scan order.

### 2.2 Cell <-> pixel mapping

```
core:     centre = (col + 0.5, row + 0.5)                      [cells]
renderer top:     x = FIELD_X + col * 15,      y = FIELD_Y + row * 15,  w = h = 15
renderer bottom:  x = MAP_X  + col * 12,       y = MAP_Y  + row * 12,  w = h = 12
range draw radius (top) = 2.142857 * 15 = 32.14 -> 32 px
```

### 2.3 `[LOCKED]` The fixed path: hand-authored serpentine (76 cells)

No BFS at runtime. No re-pathing. `Game.path` is a hardcoded const array built once at `game_new_run`/grid init (Appendix C). Locked geometry (0-based):

| Segment | Cells | Count |
|---|---|---|
| Row 1, left->right | `(0,1)` .. `(23,1)` | 24 |
| Col 23, downward | `(23,2)` .. `(23,5)` | 4 |
| Row 5, right->left | `(22,5)` .. `(2,5)` | 21 |
| Col 2, downward | `(2,6)` .. `(2,9)` | 4 |
| Row 9, left->right | `(3,9)` .. `(25,9)` | 23 |
| **Total** | contiguous 4-connected, no duplicates | **76 cells / 75 segments** |

- `Path.count = 76`, `Path.length = 75.0f`; start `(0,1)`, end `(25,9)`.
- Enemies spawn at `pathPos = 0` -> centre `(0.5,1.5)`; leak at `pathPos >= 75.0` (end centre `(25.5,9.5)`).
- All 76 path cells are marked in `path_cell[GRID_ROWS][GRID_COLS]`; path cells are never buildable (L2).
- `Game.pathBlocked` is always `false`; no BFS is needed. (`game.h`'s "placement makes the goal unreachable" comment is obsolete for this design — do not implement it.)
- Interpolation uses the generic §2.4 code; turns are just adjacent cell centres, as in the Lua.
- Board usage with range 2.142857 cells: rows 0–11 all contain at least one buildable cell within range of the path (230 of the 262 buildable cells are in range, verified against the locked path data); row 12 is decorative. This replaces the old straight-path limitation (only rows 10–11 usable).
- Renderer rule: **draw the actual path cells from `g->path.pts`** (both screens); never hardcode a straight road. Gradient `t = i/(count-1)` over the 76 cells.
- The Lua's own empty-grid BFS path was a straight bottom row; this port intentionally uses the hand-authored serpentine above instead — LOCKED, not derivable from BFS.

### 2.4 Path geometry helpers (C)

```
cell-centre path position:
  pos = clamp(pathPos, 0, path.length)
  i   = min((int)pos, path.count - 2)
  f   = pos - i
  x   = pts[i].x + (pts[i+1].x - pts[i].x) * f
  y   = pts[i].y + (pts[i+1].y - pts[i].y) * f
```

There is no straight-path special case: the serpentine path turns corners at cell boundaries, and the lerp above is the whole geometry. `pathPos` is clamped only for drawing; the leak test uses the raw value.

### 2.5 Placement validity `[LOCKED]` (`game_can_place_at` / `game_place_tower`)

Check in this order; first failure wins:

1. `phase == PHASE_BUILD || phase == PHASE_WAVE` (building during a wave is allowed, like the Lua; pause does not block building).
2. `col in [0,25] && row in [0,12]`.
3. `!path_cell[row][col]` -> message `"CANNOT BUILD ON THE PATH"`.
4. `occupancy[row][col] == 0` -> message `"CELL OCCUPIED"`.
5. `towerCount < MAX_TOWERS` (96) -> message `"TOWER LIMIT 96"`.
6. `gold >= 20` -> message `"NOT ENOUGH GOLD"`.

On success: `gold -= 20`; tower element = `selectedType`; level 1; `cooldown = 0`; `invested = 20`; `occupancy[row][col] = towerIndex + 1`; `towerCount++`; `GAME_EVT_BUILD`.

`game_place_tower` returns `false` and sets `message`/`messageTimer = 2.0` on failure. There is no path-blocked failure and no repositioning of enemies.

### 2.6 Original pathfinding (historical reference only — removed by L1/L2)

The Lua's BFS (`main.lua:63-109`): FIFO queue seeded with the start cell, neighbours expanded in **up, down, left, right** order, towers block cells, uniform cost 1, path reconstructed via a `parent` map; returns `nil` when unreachable. Recompute triggers: boot, every successful build (validated by a first BFS, then a second BFS via `updatePath()`), every sell. The initial empty-grid path was the straight bottom row (25 segments); the port deliberately replaces it with the hand-authored serpentine (§2.3). Building on path cells redirected the path; if no path remained the build was rolled back with the toast `"Path blocked!"`. All of this is **not ported** (L1/L2); it explains the old 3DS performance problems (§13) and is documented so nobody mistakes the fixed path for a BFS result.

---

## 3. Towers

### 3.1 Roster `[LUA]` (`main.lua:128-156`)

All three towers are stat-identical; only element (colour + damage multiplier) differs. `glowColor`/`glow` is declared but never used by the Lua — keep the data in `TowerSpec` for parity, do not implement glow behaviour.

| # | Element | Core id | Cost | Damage L1 | `fireRate` L1 (= cooldown **seconds between shots**) | Shots/s `[DERIVED]` | Range | Range (cells) `[DERIVED]` | Colour (floats) | 8-bit | glow (unused) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | Fire | `ELEMENT_FIRE` (0) | 20 | 3 | 0.8 | 1.250 | 60 px | **2.142857** | (0.95, 0.15, 0.15) | (242, 38, 38) | (0.95, 0.3, 0.1) |
| 2 | Water | `ELEMENT_WATER` (1) | 20 | 3 | 0.8 | 1.250 | 60 px | 2.142857 | (0.15, 0.4, 0.95) | (38, 102, 242) | (0.1, 0.5, 0.95) |
| 3 | Forest | `ELEMENT_FOREST` (2) | 20 | 3 | 0.8 | 1.250 | 60 px | 2.142857 | (0.15, 0.9, 0.15) | (38, 230, 38) | (0.1, 0.9, 0.3) |

`TowerSpec` values in `game.h`: `name = "Fire"/"Water"/"Forest"`, `cost = 20`, `damage = 3.0f`, `fireRate = 0.8f`, `range = 2.142857f` (cells), `color[]` as above, `glow[]` as above.

`fireRate` semantics: **seconds of cooldown between shots**. On fire `tower.cooldown = fireRate(level)`; every step `cooldown = max(0, cooldown - dt)`. A new tower has `cooldown = 0` and fires on its first step with a target in range. Range is constant at every level (it comes from the spec, not the tower).

### 3.2 Targeting & firing `[LUA]` (`main.lua:715-777`)

- Every occupied cell is scanned each step (column-major: `col 0..25`, `row 0..12`); each tower scans all active enemies.
- Candidate: `distance(towerCentre, enemyCentre) <= 2.142857` cells (Euclidean, inclusive, positions are continuous).
- Target: smallest distance; ties keep the **earliest in the enemies array** (= earliest spawned) because only a strictly smaller distance replaces.
- Damage applied instantly on the firing step (hitscan; no projectile, no travel time). Beam is pure VFX.
- Kill: `gold += 10`, `kills++`, remove enemy, `GAME_EVT_ENEMY_DIE`, play `enemy_die.wav`. A killing shot still spawns its beam.
- No line-of-sight blocking, no minimum range, no priority modes, no target leading, no crits.

### 3.3 Beams (laser VFX) `[LUA]` (`main.lua:747-755`, `991-999`)

| Field (`Beam`) | Value |
|---|---|
| `x1,y1` | tower cell centre at fire time |
| `x2,y2` | target position at fire time (snapshot; does not follow) |
| `element` | firing tower's element (colour source) |
| `life`, `maxLife` | 0.15, 0.15 |
| draw | alpha = `life/0.15`; line width **1 px** at alpha; second pass line width **3 px** at `alpha*0.1`; colour = element colour |

`MAX_BEAMS 128`: at most 96 towers can exist and cooldowns are >= 0.28 s, so live beams = towers x (0.15/cooldown) <= ~52. 128 is plenty; if full, skip creating a beam (damage still applies).

### 3.4 Tower icon geometry `[LUA]` (`main.lua:293-390`)

Original icons are filled polygons in a local system centred on `(x,y)` with radius parameter `size`. In C draw them as triangle fans (`gfx_fill_triangle`) around the stated centroid; each centroid lies inside its polygon, so the fan is visually identical at these sizes. Winding order is irrelevant (no culling).

**Size**: original `size = 12 + min(level,10) * 0.4` px in 28 px cells (12.4 .. 16.0). Top screen: `size_px = round(size * 15/28)` -> levels 1–4 **7 px**, 5–9 **8 px**, 10 **9 px**.

**Fire** (radius `s = size`), outer polygon vertices in order: `(0,-1)`, `(-0.8,+0.6)`, `(-0.24,+0.2)`, `(0,+0.8)`, `(+0.24,+0.2)`, `(+0.8,+0.6)`, all `* s`; fan centroid `(0, +0.2*s)`, 6 triangles. Inner flame at white alpha 0.15: `(0,-0.6)`, `(-0.32,+0.3)`, `(-0.08,+0.1)`, `(0,+0.4)`, `(+0.08,+0.1)`, `(+0.32,+0.3)` `* s`.

**Water** (radius `s`), 6 vertices: `(0,-1)`, `(-0.8,+0.2)`, `(-0.5,+0.6)`, `(0,+1)`, `(+0.5,+0.6)`, `(+0.8,+0.2)` `* s`; centroid `(0, +0.27*s)`; 6 triangles. Highlight quad at white alpha 0.2: `(-0.2,-0.4)`, `(-0.4,-0.1)`, `(-0.2,-0.1)`, `(-0.1,-0.3)` `* s` (2 triangles).

**Forest** (radius `s`), 8 vertices: `(0,-1)`, `(-0.7,-0.3)`, `(-0.8,+0.2)`, `(-0.3,+0.6)`, `(0,+0.4)`, `(+0.3,+0.6)`, `(+0.8,+0.2)`, `(+0.7,-0.3)` `* s`; centroid `(0, +0.05*s)`; 8 triangles. Vein: 1 px line `(0,-0.5*s)` -> `(0,+0.3*s)`, black alpha 0.15.

**Star** (elite icon / HUD elite), radius `s`, 10 vertices: `(0,-1)`, `(0.3,-0.3)`, `(1,-0.3)`, `(0.4,0.1)`, `(0.6,0.8)`, `(0,0.4)`, `(-0.6,0.8)`, `(-0.4,0.1)`, `(-1,-0.3)`, `(-0.3,-0.3)` `* s`; fan centroid `(0,0)`.

**Level marker**: level < 10: level number as an 8x8-glyph digit above the icon (top screen: centred at `(cx, cy - size_px - 9)`, white alpha 0.8). Level >= 10: two filled rects above the icon: `(cx-3, cy-size-3, 5, 3)` gold (1,1,0.3) and `(cx-2, cy-size-2, 4, 2)` (1,1,0.5) alpha 0.6.

**Tower cell tint**: Lua fills the cell with tower colour alpha 0.06. On 3DS this is imperceptible; skip it (deviation D10).

### 3.5 Action API mapping (`include/game.h`)

| API | Behaviour | Errors (message, 2.0 s) |
|---|---|---|
| `game_set_selected_type(g, t)` | clamp 0..2; sets `selectedType`; does **not** arm placement by itself (touch build button / L,R set `placing`) | — |
| `game_place_tower(g, col, row)` | §2.5; uses `selectedType`; returns success | `"CANNOT BUILD ON THE PATH"`, `"CELL OCCUPIED"`, `"TOWER LIMIT 96"`, `"NOT ENOUGH GOLD"`, `"CAN'T BUILD NOW"` (if phase wrong) |
| `game_select_tower(g, idx)` | `idx` in `[0, towerCount)` or -1; updates `selectedTower`; clears sell confirm if any | — |
| `game_upgrade_selected(g)` | see §8.1 | `"MAX LEVEL"`, `"NOT ENOUGH GOLD"`, `"NO TOWER SELECTED"` |
| `game_sell_selected(g)` | see §7.2 | `"NO TOWER SELECTED"` |
| `game_start_next_wave(g)` | only in `PHASE_BUILD`; initialises the wave and sets `PHASE_WAVE`; emits `GAME_EVT_WAVE_START` | ignores calls outside `PHASE_BUILD` |
| `game_can_place_at(g, col, row, type)` | pure predicate = §2.5 checks 2–6 (no message) | — |

---

## 4. Elements / combinations

### 4.1 Damage multiplier matrix `[LUA]` (`main.lua:164-179`)

| Tower \ Enemy | FIRE | WATER | FOREST |
|---|---|---|---|
| **Fire** | 1.0 | **0.5** | **2.0** |
| **Water** | **2.0** | 1.0 | **0.5** |
| **Forest** | **0.5** | **2.0** | 1.0 |

Rules: strong x2.0, weak x0.5, same element x1.0. `game_damage_multiplier(towerType, enemyElement)` returns 2.0/1.0/0.5; clamp out-of-range indices to 0; default 1.0 for any unknown combination.

### 4.2 No combining mechanic `[LUA]`

- No element fusion, no mixed/paired towers, no tier names, no recipe UI.
- Tower type is one `Element`; the only progression is level 1..10 (§8).
- `upgrade_1.wav`..`upgrade_6.wav` are a **random sound pool**, not six tiers: `playRandomUpgradeSound()` picks `math.random(1,#loaded)` on each successful upgrade (`main.lua:278-282`). No mapping to element or level. Port: `1 + (rand() % 6)` or the same rng; not gameplay-relevant.
- `EnemySpec` (Fire/Water/Forest) is colour/name data; elites are a bool modifier, not an element (§5).

### 4.3 Element roster locked

The remake README claims "fire, water, earth, air and a few combinations — each with its own range, damage and fire rate". The Lua has **three** elements (Fire/Water/Forest), identical stats, no combinations, no earth/air — and the Lead has locked **three only, no combining**. This spec follows the Lua. Combinations would be new design, not a port.

---

## 5. Enemies

### 5.1 Base enemies `[LUA]` (`main.lua:158-162`, `669-689`)

All three elements are mechanically identical; element only drives colour and the damage multiplier against towers.

| Element | `EnemySpec` name | Colour floats | 8-bit | HP | Speed parameter | Icon size | Bounty on kill | Behaviour |
|---|---|---|---|---|---|---|---|---|
| Fire | "Fire" | (0.95, 0.15, 0.15) | (242, 38, 38) | `15 + (wave-1)*3` | `20 + (wave-1)` | 10 px orig / 5 px 3DS | +10 gold | walk path; leak at end |
| Water | "Water" | (0.15, 0.4, 0.95) | (38, 102, 242) | same | same | same | +10 gold | same |
| Forest | "Forest" | (0.15, 0.9, 0.15) | (38, 230, 38) | same | same | same | +10 gold | same |

- No enemy attacks, no armour, no status effects, no regeneration, no splitting, no collision between enemies, no interaction with towers except being shot.
- Element per spawn: **exact quotas** (locked, D5); if `EXACT_QUOTAS 0`, uniform random per spawn (`types[rng % 3]`), matching the Lua's `math.random(1,3)`.
- HP/speed are unbounded linear functions of `wave`; wave 25 numbers in §6.2.
- Bounty is always +10 on kill (elites included) `[LOCKED: L6]`.

### 5.2 Elite modifier `[LUA]` (`main.lua:674-677`, `392-409`)

Elite is a boolean on any element.

| Property | Normal | Elite |
|---|---|---|
| HP | `15 + (wave-1)*3` | **x 2.5** |
| Speed parameter | `20 + (wave-1)` | **x 1.8** |
| Icon size | 10 px orig / 5 px 3DS | 16 px orig / 9 px 3DS |
| Health bar | width 24 orig / 13 px 3DS, offset -18 orig / -10 px 3DS | same |
| Kill bounty | +10 gold | **+10 gold** (no extra) |
| Extra visual | — | 1 px outline rect around the icon, white alpha 0.12, side `2*size+4` orig; gold alpha 0.15 second rect, side `2*size+8` orig; HUD icon = star |
| Element multiplier | applies | applies |

Examples: wave 25 elite = 217.5 HP, speed parameter 44 x 1.8 = 79.2.

### 5.3 Movement in cell space `[LOCKED: engine contract]`

The Lua increments normalized progress: `t += dt * speed/100`, crossing in `100/speed` seconds. `game.h` stores `Enemy.pathPos` as **distance along the path in cells**, so the equivalent velocity is:

```
pathLen       = Game.path.length                  // 75.0 for the locked serpentine path
speedParam(w) = 20.0f + (w - 1)                   // wave of the CURRENT wave (enemies never overlap waves)
vel(e)        = pathLen * speedParam(Game.wave) * (e->elite ? 1.8f : 1.0f) / 100.0f  // cells/s
pathPos      += vel * dt
```

- Crossing time stays `100/speedParam` seconds — exactly like the Lua — independent of path shape/length.
- Velocity examples `[DERIVED]`: wave 1 regular = `75 * 20/100` = **15.0 cells/s** (5.00 s crossing); wave 10 = 75*29/100 = 21.75 cells/s (3.45 s); wave 25 regular = **33.0 cells/s** (2.27 s); wave 25 elite = **59.4 cells/s** (1.26 s).
- On spawn: `pathPos = 0`, `x,y = game_cell_center(0,1)` = `(0.5,1.5)`.
- Position each step = §2.4 interpolation along `Game.path.pts` (cell centres); corners are sharp (no smoothing), same as the Lua.
- **Leak** `[LOCKED L4]`: `pathPos >= path.length` -> remove enemy, `lives -= 1`, `leaks += 1`, `GAME_EVT_LEAK`, no gold; if `lives == 0` -> `PHASE_GAMEOVER` + `GAME_EVT_GAMEOVER`. (Original paid +2/+10 gold and retried the wave — do not reproduce.)
- HP can go negative; nothing clamps it (the enemy is removed on `hp <= 0` in the same step).

### 5.4 Enemy visuals `[LUA]` (`main.lua:392-417`, `1001-1005`)

- Icon = the element shape from §3.4 at radius 10 (normal) / 16 (elite) in original units; 3DS: 5 / 9 px.
- Health bar drawn every frame for every enemy: centre `(e.x, e.y - 18 orig)` scaled to `-10 px` 3DS; width 24 orig -> **13 px**; background fill black alpha 0.7, height 5 orig -> 3 px; fill colour `(1-p, p, 0)`, inner width `(13-2)*p`, height 2 px; clamp `p = clamp(hp/maxHp, 0, 1)`.
- Elite extra outline rects (above) drawn after the icon.
- `hitFlash` (engine extra): renderer may tint the icon white for a few frames after damage; purely cosmetic, no gameplay effect. Original had no hit flash.

### 5.5 Capacity `[PORT]`

`MAX_ENEMIES 128`. Max concurrent = wave total spawns (waves do not overlap): wave 25 = **63**. Safe. If the array ever fills (defensive), skip the spawn and decrement `enemiesToSpawn` so the wave can still complete, and set message `"ENEMY LIMIT"` in debug builds.

---

## 6. Waves (25 total)

### 6.1 Counts, elites, interval `[LUA]` (`main.lua:513-571`)

Called when the player starts a wave (`game_start_next_wave`):

```
base        = 3 + wave * 2                         // 5, 7, 9, ...
E           = 10 if wave % 25 == 0
              3 if wave % 10 == 0
              1 if wave % 5 == 0
              0 otherwise                          // precedence %25 > %10 > %5
totalSpawns = base + E
regular     = totalSpawns - E = base               // [PORT] correct regular count
quotaFire   = floor(regular / 3)
quotaWater  = floor(regular / 3)
quotaForest = regular - quotaFire - quotaWater
interval    = max(0.8, 3.0 - 0.05 * wave)          // seconds; wave 1 = 2.95, wave 44+ = 0.80
spawnTimer  = 0
eliteSpawned = 0
HP          = 15 + (wave-1)*3
speedParam  = 20 + (wave-1)
```

Bug note: the Lua computes its HUD quotas from `base - E` (`main.lua:558`), which undercounts regular spawns by `E` on elite waves (regular spawn slots are `base`). The port uses `regular = totalSpawns - E = base`, so the HUD/preview matches the actual spawns. Lua's displayed (buggy) quotas for elite waves: w5 4/4/4, w10 6/6/8, w15 10/10/12, w20 13/13/14, w25 14/14/15 — do not use these.

### 6.2 Wave data table 1..25 `[DERIVED]` (the authoritative numbers)

`base` = regular spawn slots; `E` = elites; `total` = spawn slots; `F/W/Fo` = exact element quotas for the regular slots (port values); `I` = spawn interval (s); `HP` = regular HP; `Spd` = speed parameter; `cross` = `100/Spd` s; `eliteHP` = `HP*2.5` (— if none); `gold` = max kill income (`total*10`).

| Wave | base | E | total | F/W/Fo | I (s) | HP | Spd | cross (s) | eliteHP | gold |
|---:|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | 5 | 0 | 5 | 1/1/3 | 2.95 | 15 | 20 | 5.00 | — | 50 |
| 2 | 7 | 0 | 7 | 2/2/3 | 2.90 | 18 | 21 | 4.76 | — | 70 |
| 3 | 9 | 0 | 9 | 3/3/3 | 2.85 | 21 | 22 | 4.55 | — | 90 |
| 4 | 11 | 0 | 11 | 3/3/5 | 2.80 | 24 | 23 | 4.35 | — | 110 |
| 5 | 13 | 1 | 14 | 4/4/5 | 2.75 | 27 | 24 | 4.17 | 67.5 | 140 |
| 6 | 15 | 0 | 15 | 5/5/5 | 2.70 | 30 | 25 | 4.00 | — | 150 |
| 7 | 17 | 0 | 17 | 5/5/7 | 2.65 | 33 | 26 | 3.85 | — | 170 |
| 8 | 19 | 0 | 19 | 6/6/7 | 2.60 | 36 | 27 | 3.70 | — | 190 |
| 9 | 21 | 0 | 21 | 7/7/7 | 2.55 | 39 | 28 | 3.57 | — | 210 |
| 10 | 23 | 3 | 26 | 7/7/9 | 2.50 | 42 | 29 | 3.45 | 105.0 | 260 |
| 11 | 25 | 0 | 25 | 8/8/9 | 2.45 | 45 | 30 | 3.33 | — | 250 |
| 12 | 27 | 0 | 27 | 9/9/9 | 2.40 | 48 | 31 | 3.23 | — | 270 |
| 13 | 29 | 0 | 29 | 9/9/11 | 2.35 | 51 | 32 | 3.12 | — | 290 |
| 14 | 31 | 0 | 31 | 10/10/11 | 2.30 | 54 | 33 | 3.03 | — | 310 |
| 15 | 33 | 1 | 34 | 11/11/11 | 2.25 | 57 | 34 | 2.94 | 142.5 | 340 |
| 16 | 35 | 0 | 35 | 11/11/13 | 2.20 | 60 | 35 | 2.86 | — | 350 |
| 17 | 37 | 0 | 37 | 12/12/13 | 2.15 | 63 | 36 | 2.78 | — | 370 |
| 18 | 39 | 0 | 39 | 13/13/13 | 2.10 | 66 | 37 | 2.70 | — | 390 |
| 19 | 41 | 0 | 41 | 13/13/15 | 2.05 | 69 | 38 | 2.63 | — | 410 |
| 20 | 43 | 3 | 46 | 14/14/15 | 2.00 | 72 | 39 | 2.56 | 180.0 | 460 |
| 21 | 45 | 0 | 45 | 15/15/15 | 1.95 | 75 | 40 | 2.50 | — | 450 |
| 22 | 47 | 0 | 47 | 15/15/17 | 1.90 | 78 | 41 | 2.44 | — | 470 |
| 23 | 49 | 0 | 49 | 16/16/17 | 1.85 | 81 | 42 | 2.38 | — | 490 |
| 24 | 51 | 0 | 51 | 17/17/17 | 1.80 | 84 | 43 | 2.33 | — | 510 |
| 25 | 53 | 10 | 63 | 17/17/19 | 1.75 | 87 | 44 | 2.27 | 217.5 | 630 |

Run totals `[DERIVED]`: 743 spawns across 25 waves; max kill income 7,430 gold; plus 150 start = 7,580 max lifetime gold. Wave 25 spawn span = `(63-1)*1.75` = 108.5 s of spawning; last spawn crosses in <= 2.27 s, so the wave lasts ~110 s if nothing is killed.

### 6.3 Elite spawn schedule `[LUA]` -> net effect `[LOCKED]`

The Lua preloads `waveEvents` with delays (`i*0.3` for 10 elites, `i*0.2` for 3, `0.5` for 1), decrements every pending delay by `interval` on each spawn slot, and spawns an elite when a delay reaches `<= 0` (`main.lua:531-556`, `656-667`). Exact net effect `[DERIVED]`:

- spawn slot 1 (1-based within the wave) is always **regular**;
- spawn slots **2 .. 1+E** are the **E elites, consecutively**;
- all other slots are regulars.

The delays never spread elites through the wave — do not "fix" this into a nice schedule. **Locked implementation: drop the delay-array complexity**; use `eliteSpawned < E && spawnIndex >= 2 && spawnIndex <= 1+E`. The `eliteDelays[16]` fields in `game.h` may stay unused (or hold the same rule); they are not required.

### 6.4 Spawning details

- First spawn happens one full interval after the wave starts (`spawnTimer` starts at 0 and increments before the check) — wave 1's first enemy appears 2.95 s after NEXT WAVE.
- One spawn per interval, timer reset to 0 (Lua behaviour; do not subtract).
- Regular spawn elements: exact quotas (D5). Distribute in a fixed interleaved order (recommended: fill an array of `regular` elements as `F,W,Fo,F,W,Fo,...` respecting each quota, then spawn the next element at each regular slot); this keeps `game_wave_preview` truthful.
- Wave start clears the previous wave's state; waves never overlap (BUILD phase between them).
- `GAME_EVT_WAVE_START` + `wave_start.wav` on every `game_start_next_wave`.

### 6.5 Wave completion & transitions `[LOCKED]`

```
when phase == WAVE and enemiesToSpawn == 0 and enemiesAlive == 0:
    if wave >= totalWaves (25):  phase = PHASE_VICTORY; event VICTORY      // L5
    else:                        wave += 1;    phase = PHASE_BUILD          // L3
```

Never retry a wave (Lua retried forever when anything leaked — §14.1), never auto-chain, never pay leak gold, and there is **no inter-wave timer**: `PHASE_BUILD` persists until the player taps NEXT WAVE (player-paced). Victory requires `lives > 0`; if the last leak took lives to 0, `GAMEOVER` wins (`lives` check happens in the leak handler before completion).

### 6.6 Wave counter & HUD

- Display `WAVE <wave>/25` at all times; during `PHASE_BUILD` it is the wave that NEXT WAVE will launch; after victory the HUD shows `25/25`.
- `LEFT` counter: during `WAVE` = `enemiesToSpawn + enemiesAlive`; during `BUILD` = total spawns of the upcoming wave (`base + E`).
- Lua behaviour (for reference) displayed `enemiesRemaining` including unspawned enemies, and incremented `wave` only on a leak-free completion.

### 6.7 Preview API

`game_wave_preview(g,&fire,&water,&forest,&elite)` returns the quotas for `g->wave` (the wave that will start next in `PHASE_BUILD`, or the current wave during `WAVE`). Values match §6.2 exact quotas. Renderer shows these on the bottom strip (§11.3).

### 6.8 Engine caps

`MAX_ELITE_SPAWNS 16` (max E = 10) — safe. `MAX_ENEMIES 128` (max 63 concurrent) — safe. `MAX_BEAMS 128` — safe (§3.3). `MAX_TOWERS 96` — reachable: 262 buildable cells, cap 96 => message `"TOWER LIMIT 96"` (§2.5).

---

## 7. Economy & lives

### 7.1 Values `[LUA]` except lives `[LOCKED L4]`

| Item | Value | Source |
|---|---|---|
| Starting gold | **150** | `main.lua:186` |
| Starting lives | **20** | `[LOCKED L4]` — Lua has none |
| Tower cost | 20 (all types) | `main.lua:131/140/149` |
| Kill reward | **+10 gold** any enemy (incl. elite) | `main.lua:763` |
| Leak | **-1 life, 0 gold** (elites too — locked) | `[LOCKED L4]`; Lua paid +2/+10 and retried |
| Wave completion bonus | none | `[LUA]` |
| Sell refund | `floor(invested * 0.75)` = `15 * 2^(level-1)` at level L | `main.lua:1448-1450` |
| Upgrade cost (L -> L+1) | `20 * 2^(L-1)` | `main.lua:1430` |
| Total invested at level L | `20 * 2^(L-1)` (`invested` accumulates) | `[DERIVED]` |
| Interest / repair / partial refund | none | `[LUA]` |
| Gold can go negative | no (all spends checked) | `[LUA]` |
| Lose condition | `lives <= 0` -> `PHASE_GAMEOVER` | `[LOCKED L4]` |
| Win condition | clear wave 25 with `lives > 0` -> `PHASE_VICTORY` | `[LOCKED L5]` |

### 7.2 `[PORT]` API formulas

```
game_upgrade_cost(g, i) = (tower level >= 10) ? -1 : 20 << (level-1)      // level 1..9 -> 20..5120
game_sell_value(g, i)   = tower.invested * 3 / 4                          // integer floor
                        // equivalent to 15 << (level-1) when the tower was upgraded from level 1
```

`Tower.invested` must be incremented by the upgrade cost on every upgrade so the sell value matches the Lua's `getTotalCost` (which always uses a base of 20). Note the Lua hardcodes the base cost inside `getTotalCost` (`main.lua:502-508`); all towers cost 20 so this is correct today — `invested` is the cleaner C version (`[PORT]`).

After a successful sale: `gold += sell value`; `occupancy[row][col] = 0`; remove the tower (keep the tower array compact: swap the last tower into the hole and update the moved tower's `occupancy`; set `selectedTower = -1`); `towerCount--`; `GAME_EVT_SELL`.

### 7.3 Income sanity over the 25-wave run

- Max lifetime gold: 150 start + 7,430 kills = **7,580** (if no leak and every kill claimed).
- Full upgrades are expensive: one tower from L1 to L10 = 10,240 total, more than the whole run's income — so the run is about many mid-level towers, not maxed ones.
- Income is kill-gated: leaks cost lives *and* income, so the economy punishes the same mistakes twice (intended by L4).

---

## 8. Upgrades

### 8.1 Per-level table `[LUA]` formulas + `[DERIVED]` values

On a successful upgrade (`main.lua:1425-1442`): cost `20*2^(level-1)` (level before upgrade), `damage *= 2`, `fireRate = floor(fireRate*0.9*100)/100` (2-decimal floor), `level += 1`, `invested += cost`, gold -= cost, play a random upgrade sound. Max level 10.

| Level | Damage `[DERIVED]` | `fireRate` cooldown (s) `[DERIVED]` | Shots/s | DPS vs neutral | Upgrade cost (L -> L+1) | Total invested | Sell = floor(0.75 x total) |
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

- `damage(level) = 3 * 2^(level-1)` exactly; `fireRate` must use the table above (the `floor` step is load-bearing: 0.8 x 0.9^9 = 0.3099, but the Lua yields 0.28).
- Element multiplier stacks: DPS vs weak element = 2x, vs resistant = 0.5x.
- Upgrading does not reset the cooldown timer; the tower keeps its remaining cooldown.
- Range never changes with level.
- Level 10: `game_upgrade_cost` returns -1; `game_upgrade_selected` fails with `"MAX LEVEL"` before checking gold (matches the Lua's check order).

### 8.2 UI `[LUA]` -> 3DS §11

- Selecting a tower shows its info in the bottom tooltip strip and enables UPGRADE / SELL buttons with prices.
- Upgrade tap / X -> §8.1; sell tap twice / Y twice -> §7.2. Any other action clears the sell confirmation; no timeout (Lua has none).
- Selected tower also draws its range circle (radius 32 px on the top screen).
- The Lua shows `LV.<n>`, `DMG: <n>`, `X: <cost>G`, `Y: <sell>G`, `X: Upgrade | Y: Sell | B: Cancel`, `Press Y again to confirm sell` (`main.lua:1226-1248`). The 3DS texts are shorter (§11.2 tooltip table).

---

## 9. UI/HUD (original layout reference)

The original draws a **1280x720** layout: playfield band y 0..500, HUD panel y 505..720. This section preserves the original content/colours/fonts for fidelity, then §11 re-lays it out for the 3DS. The Lua has **no lives widget** and **no tooltip**; the info panel is the de-facto tooltip.

### 9.1 Fonts `[LUA]`

Five LÖVE default-font sizes: `fontTitle 36`, `fontLarge 26`, `fontMedium 18`, `fontSmall 14`, `fontTiny 11` (`main.lua:1506-1510`). 3DS uses `gfx.h`'s 8x8 bitmap font at scale 1 (8 px) and scale 2 (16 px); hierarchy is approximated by colour and position rather than five sizes (§11).

### 9.2 Game HUD elements `[LUA]` (`main.lua:1045-1267`)

Panel: `rect(0,505,1280,215)` fill `(0.03,0.03,0.07,0.95)`; 1 px separator at y=505 `(0.1,0.1,0.2,0.2)`.

| Element | Position / size | Font | Colour | Content / notes |
|---|---|---|---|---|
| "WAVE" label | `printf(30,513,100,center)` | small | (0.5,0.5,0.7,0.6) | static |
| wave number | `printf(30,538,100,center)` | title 36 | white | `wave` |
| status | `printf(30,588,100,center)` | small | READY (1,1,0.4) / ACTIVE (0.2,0.9,0.2) / COMPLETE (1,1,0.3) / WAITING (0.5,0.5,0.5) | READY iff `!gameStarted`; ACTIVE iff `waveActive`; COMPLETE iff `waveComplete` (never visible); WAITING unreachable |
| "LEFT" label | `printf(30,608,100,center)` | small | (0.5,0.5,0.7,0.5) | |
| enemies remaining | `printf(30,628,100,center)` | large 26 | white α0.9 | `enemiesRemaining` |
| "WAVE ENEMIES" header | `printf(1250,513,200,right)` | small | (0.5,0.5,0.7,0.6) | **off-screen right** (right edge 1450) — §14.1 B8 |
| composition rows | y = 538 + 24k; icon `(1080,y+8)` size 8; name `printf(1105,y,100,left)` α0.8; count `printf(1205,y,40,right)` `"x"..n` α0.7 | small | element colours; elite (1,0.8,0.2) | only `count>0`, order Fire/Water/Forest/Elite; icons fire/water/leaf/star; `"---"` placeholder otherwise |
| gold coin | circle `(30,677)` r14 (1,0.8,0.2) + highlight `(29,676)` r5.6 α0.6 (1,1,0.5) + rim r14 (0.8,0.6,0.1) | — | — | |
| gold value | `printf(55,669,80,left)` | large 26 | (1,0.9,0.3) | |
| "SPEED" label | `printf(150,669,60,left)` | small | (0.5,0.5,0.7,0.5) | |
| speed value | `printf(150,687,60,left)` | medium | white α0.8 | PAUSED / 0.5x / 1x / 2x / 4x |
| "BUILD:" label | `printf(250,669,50,left)` | tiny | (0.5,0.5,0.7,0.4) | |
| build buttons | 3x `rect(310+100(i-1),667,88,38,r4)` | — | fill (0.08,0.08,0.14,0.6); selected: inner fill tower α0.2 + outline α0.4 | icon `(x+18,685)` size 12; cost `printf(x+38,679,40,left)` tiny white α0.6 |
| info panel | `rect(490,511,560,60)` fill (0.04,0.04,0.08,0.6), outline (0.1,0.1,0.2,0.15) | — | — | content §9.3 |
| error toast | `rect(400,530,480,35)` black α0.7; text `printf(410,535,460,center)` medium 18 (1,0.2,0.2) | — | — | only while `errorTimer>0`; messages `Path blocked!` / `Not enough gold!` / `Max level reached!` |

### 9.3 Info panel text by build state `[LUA]` (`main.lua:1207-1257`)

| Build state | Line 1 (y+4) | Line 2 (y+35; y+20 for empty cell) |
|---|---|---|
| BUILDING | element name (x+15, element colour α0.9); `DAMAGE: <d>` (x+80); `COST: <c>G` gold (x+190) | `PATH CELL - Building redirects enemies` if path cell else `Press A to build  |  B to cancel` |
| TOWER_SELECTED | name; `LV.<n>` (x+80); `DMG: <d>` (x+145); `X: <cost>G` green (x+255) (`MAX` -> renders `X: MAXG`); `Y: <sell>G` red (x+365) | `Press Y again to confirm sell` if armed; else `X: Upgrade  |  Y: Sell  |  B: Cancel` |
| empty | — | path: `PATH CELL - Build here to redirect enemies`; else `Press A to build a tower here` |

### 9.4 Main menu `[LUA]` (`main.lua:804-878`)

Gradient background (721 one-pixel strips, `(0.02+0.03t, 0.02+0.03t, 0.05+0.07t)`, t = i/720); panel `rect(290,80,700,560,r4)` (0.04,0.04,0.10,0.95) + outline (0.15,0.15,0.30,0.4); title glow `(640,200)` r100 (1,0.7,0.2) α0.15; `"ELEMENTAL"` title 36 (1,0.9,0.4) at `printf(440,160,400,center)`; `"DEFENSE"` large 26 white at `printf(478,213,280,center)` with shadow at `(480,215)` (0.3,0.3,0.5,0.5); `"Tower Defense"` medium (0.5,0.5,0.7,0.6) at `printf(500,265,280,center)`; divider `rect(440,295,400,1)` (0.3,0.2,0.1,0.2); three element icons at x = 420/640/860, y=320, size 18 + glow r25 α0.12, labels Fire/Water/Forest below; START button `rect(570,400,140,55,r28)` (0.85,0.15,0.15) + shadow + highlight + `"START"`; hint `"Press SELECT for controls"` tiny at `(520,590)`; START touch hit box (570..710, 400..455) (`main.lua:1465`). Menu particles: 4 spawned per frame, unbounded, drift ±30 px/s, life 1..4 s, size 2..5, random colours (§14.1 B9).

### 9.5 Controls overlay `[LUA]` (`main.lua:883-918`)

Full-screen black α0.90; panel `rect(290,70,700,580,r4)`; `"CONTROLS"` title; seven lines at `printf(380,180+50i,500,left)`:

```
D-Pad / Touch : Move cursor
A / Tap       : Select / Build
B             : Cancel
L / R         : Speed control
X             : Upgrade tower
Y (twice)     : Sell tower
SELECT        : Toggle menu
```

footer `"Press SELECT again to close"`. Toggled by `select`/`tab` in both game states; while open all other keys are swallowed. The 3DS overlay does not reproduce the Lua texts (which lie about A — §14.1 B1) and instead lists the mapping in §10.2.

### 9.6 Playfield `[LUA]` (`main.lua:923-1040`)

Draw order: gradient bg -> grid backdrop `rect(274,76,732,368)` (0.03,0.03,0.06) + outline -> path cells (fill `(0.2+0.08t,0.12+0.04t,0.04+0.02t)`, t = path index/length; outline (0.25,0.18,0.08,0.15)) -> cell fills (empty (0.05,0.05,0.08), tower-cell tint α0.06, outline (0.08,0.08,0.12,0.12)) -> selected-tower range circle (r 60 px, fill α0.06, outline α0.15) -> towers -> lasers -> enemies + health bars -> cursor (white fill α0.08, four 4x4 corner blocks α0.6, outline α0.3; yellow fill α0.15 + outline α0.3 when BUILDING) -> pre-game hint text (unreachable). Cell size = 28 px, offset `(276,78)`.

### 9.7 Transient messages `[PORT]`

`game.h` provides `Game.message` + `messageTimer`; the C core sets them from failed actions (§2.5, §3.5, §8.1) with `messageTimer = 2.0`; the renderer shows the message in the bottom tooltip strip (red for errors) while the timer runs. Duration scales with speed (same as the Lua's `errorTimer`), and unlike the Lua it does **not** swallow input (§14.1 B11). Messages are short all-caps strings (<= 23 chars fits 184 px with the 8x8 font; `MAX_MESSAGE_LEN 48` gives headroom).

---

## 10. Input mapping

### 10.1 Original controls `[LUA]` (`main.lua:1288-1458`, `1463-1488`)

Key names are LÖVE strings; `a`/`d`/`w`/`s` alias the arrows.

| Context | Key | Exact Lua behaviour |
|---|---|---|
| MENU | `select`/`tab` | toggle controls overlay |
| MENU | `a`/`return`/`space` | start game: `PLAYING`, `gameStarted=true`, `startWave()` |
| any | `select`/`tab` | toggle overlay; if open, all other keys ignored |
| any | any key while toast active | clears toast, swallows key |
| PLAYING | `l`/`[`, `r`/`]` | speed index -1 / +1 over {0,0.5,1,2,4} |
| BUILDING | `left`/`a`, `right`/`d` | previous / next tower type |
| BUILDING | `b`/`backspace` | cancel |
| BUILDING | `a`/`return`/`space` | build; `a` unreachable (intercepted as "previous type") |
| MOVING / TOWER_SELECTED | arrows / `w`/`s`/`a`/`d` | move cursor, clamp; leaving TOWER_SELECTED cancels selection |
| MOVING | `a`/`return`/`space` | `a` moves cursor left; `return`/`space` enter BUILDING or TOWER_SELECTED |
| TOWER_SELECTED | `b` | cancel |
| TOWER_SELECTED | `x` | upgrade |
| TOWER_SELECTED | `y` | arm sell, second press sells |
| PLAYING touch | tap grid cell | sets cursor, then `love.keypressed("a")` -> moves cursor one cell left (§14.1 B1/B10); UI buttons not touchable |
| MENU touch | tap START rect | start |

Escape quits (LÖVE default), no confirmation.

### 10.2 `[LOCKED]` 3DS mapping (touch primary, buttons secondary)

Touch only exists on the **bottom screen**, so the bottom panel carries a 26x13 touch map of the grid (§11.3). Every action also has a button equivalent.

| Action | Touch (primary) | Buttons (secondary) | Core API |
|---|---|---|---|
| move cursor | tap a map cell (when no type armed and no tower there) | D-pad (repeat 0.25 s delay, 0.08 s rate) | `game_set_cursor` |
| choose tower type | tap one of 3 build buttons (tap again to disarm) | **L / R** cycle `selectedType` 0->1->2->0 (keeps `placing` armed) | `game_set_selected_type` |
| place tower | arm a type, then tap an empty map cell | A (with a type armed, at cursor) | `game_place_tower` |
| select tower | tap a tower cell | A when cursor on a tower and nothing armed | `game_select_tower` |
| cancel / deselect | tap armed build button again | B | `placing=false`, `game_select_tower(-1)` |
| start next wave | tap NEXT WAVE (only in build phase) | none (deliberate: no accidental A-starts) | `game_start_next_wave` |
| upgrade | tap UPGRADE (selected tower) | X | `game_upgrade_selected` |
| sell | tap SELL twice (label `SURE?`) | Y twice | `game_sell_selected` |
| cycle speed | tap SPEED (1x -> 2x -> 4x -> 1x) | **Select** | `game_cycle_speed` |
| pause / resume | tap PAUSE | **Start** | `game_toggle_pause` |
| help overlay | menu CONTROLS button | — (Select is speed now) | renderer-local |
| retry / menu | overlay buttons | — | `game_new_run` / `game_init` |

Decisions (locked):
- `A` is a true confirm (never "cursor left"); D-pad moves the cursor. Deliberately fixes the Lua's key-alias collision (§14.1 B1).
- **L/R = cycle tower type** and **Select = cycle speed**, **Start = pause** (3DS/README decision; the Lua had L/R = speed and Select = help). Documented as deviation D8.
- Wave starts only from the NEXT WAVE button (not A) to prevent accidental wave launches while building.
- The help overview is the menu CONTROLS button only; it never traps gameplay input (unlike B10).

### 10.3 Touch interaction state machine `[PORT]`

```
on map tap (bottom px -> cell):
  if paused or overlay (help) open: ignore
  if phase not in {BUILD, WAVE}: ignore
  if a tower exists at cell:
      game_select_tower(that index); placing = false;           // tap again -> deselect
  else if placing:
      ok = game_place_tower(col,row);                           // uses selectedType
      if ok: placing = false; else keep placing (retry after fixing)
  else:
      game_set_cursor(col,row)                                  // move only
```

- Build-row tap: `game_set_selected_type(i)`; `placing = (placing && selectedType == i) ? false : true` (toggle).
- UPGRADE/SELL taps require `selectedTower >= 0`; otherwise message `"NO TOWER SELECTED"`.
- SELL has a two-tap confirm stored renderer-side (`sell_confirm`); cleared by any other action or selection change (Lua has no timeout, so neither do we).
- Hit testing must clamp: `col = clamp((x-4)/12, 0, 25)`, `row = clamp((y-4)/12, 0, 12)` after integer floor of the map area only.
- Pause/GameOver/Victory overlays use their own touch buttons (§11.3) and ignore the map.

### 10.4 Edge cases

- Toast messages never swallow input on 3DS (Lua swallows one key).
- Tap outside the map/buttons: ignored.
- Action buttons are disabled-looking when inapplicable but remain tappable; tapping shows the relevant message (helps discovery).
- 3DS touch is resistive: no multi-touch logic needed; only `KEY_TOUCH` down edges trigger actions (optional drag-to-place is out of scope).
- D-pad repeat is renderer/platform-side; the core only sees discrete `game_set_cursor` calls.

---

## 11. 3DS layout (for `include/gfx.h`)

### 11.1 Scale rationale

Top screen 400x240: 26x13 grid at **15 px** cells = **390x195** field, `FIELD_X = 5`, `FIELD_Y = 24`; top HUD bar y 0..23; bottom tooltip strip y 220..239. Bottom screen 320x240: touch map at **12 px** cells = **312x156** (`MAP_X = 4`, `MAP_Y = 4`), then build row (y 164..191), action row (y 196..223), and a 16 px wave-preview strip (y 224..239).

Scaling: `S = 15/28 = 0.535714` from original px. Derived draw sizes:

| Meaning | Original | 3DS top |
|---|---|---|
| Tower range radius | 60 px | **32 px** (= 2.142857 x 15) |
| Tower icon radius | 12.4..16.0 px | **7 / 8 / 9 px** (levels 1-4 / 5-9 / 10) |
| Enemy icon radius | 10 / 16 px | **5 / 9 px** (normal / elite) |
| Enemy health bar | 24 x 5 px | **13 x 3 px** (fill 11 x 2) |
| Beam width (core/glow) | 1.5 / 5 px | **1 / 3 px** |
| Level marker text/rect | 11 px / 10x5 | **8 px glyph / 5x3 rect** |
| Cursor corner blocks | 4x4 px | **2x2 px** |

`gfx.h` has no rounded rects and a single-line 8x8 bitmap font: text scale 1 = 8 px tall, scale 2 = 16 px. Use scale 1 for all HUD/panel text, scale 2 only for menu titles, `START`, `PAUSED`, `GAME OVER`, `VICTORY`.

### 11.2 Top screen (400x240) — battlefield + HUD

Palette (float RGB, from §9): bg `(0.02,0.02,0.05)`, field frame `(0.03,0.03,0.06)`, empty cell `(0.05,0.05,0.08)`, grid line `(0.08,0.08,0.12)`, path gradient `(0.2+0.08t, 0.12+0.04t, 0.04+0.02t)`, panel `(0.03,0.03,0.07)`, label `(0.5,0.5,0.7)`, gold `(1,0.9,0.3)`, coin `(1,0.8,0.2)`, error `(1,0.2,0.2)`, status BUILD `(1,1,0.4)`, ACTIVE `(0.2,0.9,0.2)`.

| # | Element | Position / size | Colour | Notes |
|---|---|---|---|---|
| T1 | clear background | full screen | `(0.02,0.02,0.05)` | flat; Lua gradient replaced by flat + palette (D10) |
| T2 | HUD bar | `rect(0,0,400,24)` | `(0.03,0.03,0.07)` | 1 px separator `(0.1,0.1,0.2)` at y=23 |
| T3 | coin | circle `(11,12)` r5 `(1,0.8,0.2)`; highlight circle `(10,10)` r2 `(1,1,0.5)` α0.6; outline circle r5 `(0.8,0.6,0.1)` | | |
| T4 | gold text | `text(19, 8, scale=1, "%d")` | `(1,0.9,0.3)` | up to 5 digits = 40 px (ends x=59) |
| T5 | wave | `text(66, 8, 1, "W%d/%d")` | white | e.g. `W1/25`, up to 7 glyphs (56 px) |
| T6 | left | `text(130, 8, 1, "LEFT %d")` | white α0.9 | `enemiesToSpawn + enemiesAlive` |
| T7 | lives | `text(206, 8, 1, "LIVES %d")` | `(1,0.4,0.4)` | red-ish; up to 9 glyphs (72 px) |
| T8 | status | `text(290, 8, 1, ...)` | BUILD `(1,1,0.4)`, ACTIVE `(0.2,0.9,0.2)`, PAUSED `(0.8,0.8,0.8)` | `"BUILD"`, `"ACTIVE"`, `"PAUSED"` |
| T9 | speed | right-aligned so it ends at x=396, y=8, scale 1 | white α0.8 | `1x`/`2x`/`4x`/`PAUSED` |
| T10 | field frame | `rect(4,23,392,197)` | `(0.03,0.03,0.06)` | 1 px border around field |
| T11 | field cells | `rect(5,24,390,195)` | empty cell `(0.05,0.05,0.08)` | one fill |
| T12 | path cells | per cell in `g->path.pts`: `rect(FIELD_X+col*15, FIELD_Y+row*15, 15,15)` | path gradient at `t = i/(count-1)` | **76 cells from the core path — never hardcode a straight road** |
| T13 | grid lines | vertical `x = 5+15k`, k=0..26; horizontal `y = 24+15k`, k=0..13; thickness 1 | `(0.08,0.08,0.12)` | 41 lines |
| T14 | tower range (selected) | `fill_circle(cx,cy,32, element α0.06)` + `outline_circle(cx,cy,32,1, element α0.15)` | | only when `selectedTower` is this tower |
| T15 | tower icon | fan triangles centred on cell centre, radius 7/8/9 px | element colour | §3.4 |
| T16 | level marker | digit above icon at `(cx, cy - radius - 9)`; level 10: rects `(cx-3,cy-r-3,5,3)` + `(cx-2,cy-r-2,4,2)` | white α0.8 / gold | |
| T17 | beams | `line(x1,y1,x2,y2,1, colour αp)` + `line(...,3, colour αp*0.1)`, `p = life/0.15` | element colour | under enemies |
| T18 | enemies | fan icon radius 5 (9 elite) at cell-space position converted to px | element colour; elite outline rect 1 px side `2r+4` white α0.12 + gold α0.15 | convert: `px = FIELD_X + cellx*15`, `py = FIELD_Y + celly*15` |
| T19 | health bars | bg `rect(cx-6,cy-10,13,3)` black α0.7; fill `rect(cx-5,cy-9,floor(11*p),2)` `(1-p,p,0)` | | clamp p |
| T20 | cursor | cell fill white α0.08 + 4 corner 2x2 rects white α0.6 + 1 px outline white α0.3 | | always drawn |
| T21 | build preview | if `placing`: cell fill `(1,1,0.3)` α0.15 + outline α0.3 | | |
| T22 | tooltip strip | `rect(0,220,400,20)` `(0.03,0.03,0.07)`; 1 px line at y=220 `(0.1,0.1,0.2)`; text centred at y=226 scale 1 | error `(1,0.2,0.2)`; else `(0.5,0.5,0.7)` / element colour | content table below |
| T23 | pause overlay | dim full screen black α0.6; `"PAUSED"` scale 2 centred `(200,96)`; `"SPEED %s"` scale 1 `(200,124)`; `"START: RESUME"` scale 1 `(200,140)` | | |
| T24 | game-over overlay | dim black α0.75; `"GAME OVER"` scale 2 `(1,0.2,0.2)` centred y=76; `"WAVE %d/%d"` scale 1 y=104; `"KILLS %d   LEAKS %d"` scale 1 y=118 | | buttons on bottom screen |
| T25 | victory overlay | dim black α0.75; `"VICTORY"` scale 2 `(1,0.9,0.3)` centred y=76; `"ALL 25 WAVES CLEARED"` scale 1 y=104; `"KILLS %d   LIVES %d"` y=118 | | |

Tooltip strip content precedence (first match):

| Condition | Text (scale 1, centred) |
|---|---|
| `messageTimer > 0` | `Game.message` in error red |
| `placing` | e.g. `"FIRE 20G - TAP A CELL"` (element name + cost) |
| `selectedTower >= 0` | `"FIRE LV3  DMG 12  UP 80G  SELL 60G"` (`UP MAX` at level 10) |
| `PHASE_WAVE` | `"WAVE %d  LEFT %d"` |
| `PHASE_BUILD` | `"TAP NEXT WAVE WHEN READY"` |

### 11.3 Bottom screen (320x240) — touch control panel

| # | Element | Position / size | Colour | Notes |
|---|---|---|---|---|
| B1 | background | `rect(0,0,320,240)` | `(0.03,0.03,0.07)` | |
| B2 | map frame | `rect(3,3,314,158)` | `(0.02,0.02,0.05)` | 1 px border |
| B3 | map cells | `rect(4 + col*12, 4 + row*12, 12, 12)` | empty `(0.05,0.05,0.08)`; path gradient (same formula, over `g->path.pts`); tower cells element α0.25 + 1 px element outline | one fill per path/tower cell only |
| B4 | map grid lines | `x = 4+12k` k=0..26, `y = 4+12k` k=0..13, 1 px | `(0.08,0.08,0.12)` | 41 lines |
| B5 | map enemies | 2x2 rect at `(4 + cellx*12, 4 + celly*12)`; elite 3x3 + 1 px white outline | element colour | position from cell-space |
| B6 | map cursor | 12x12 1 px outline | white | tracks `cursorCol/cursorRow` |
| B7 | map selection / armed | 12x12 1 px outline on the selected tower cell (`(1,0.9,0.3)`) and on armed-type highlight | | |
| B8 | build buttons (3) | `rect(6,164,100,28)`, `(112,164,100,28)`, `(218,164,100,28)` | bg `(0.08,0.08,0.14)`; border `(0.15,0.15,0.25)`; armed: element fill α0.2 + element border | icon: fire/water/leaf fan radius 6 at `(x+14,y+14)`; name scale 1 at `(x+26,y+3)` element colour (`FIRE`/`WATER`/`FOREST`); cost scale 1 at `(x+26,y+13)` gold (`20G`) |
| B9 | action buttons (5) | `rect(6,196,60,28)`, `(68,...)`, `(130,...)`, `(192,...)`, `(254,...)` | bg `(0.08,0.08,0.14)`, border `(0.15,0.15,0.25)`; disabled text `(0.3,0.3,0.4)`; enabled text white; sell-confirm bg `(0.5,0.1,0.1)` | two centred lines scale 1 at y+3 / y+13: `UPGRADE`+cost, `SELL`+price, `NEXT`+`WAVE`, `SPEED`+`%s`, `PAUSE`/`RESUME` |
| B10 | wave preview strip | `rect(0,224,320,16)` `(0.02,0.02,0.05)`; four slots at x = 6, 86, 166, 246: icon radius 4 at `(x,y+8)` + text `"x%d"` scale 1 at `(x+12,y+4)` | element colours; elite `(1,0.8,0.2)` star | skip slots with count 0; `game_wave_preview` data |
| B11 | menu buttons | START `rect(40,84,240,56)`; CONTROLS `rect(40,152,240,32)` | START `(0.85,0.15,0.15)` + top-half highlight `(1,0.3,0.3)` α0.3; CONTROLS `(0.08,0.08,0.14)` | `"START"` scale 2 white centred; `"CONTROLS"` scale 1 |
| B12 | pause buttons | RESUME `rect(60,80,200,36)`; SPEED `rect(60,124,200,36)`; QUIT TO MENU `rect(60,168,200,36)` | same button style | scale 1 labels |
| B13 | game-over/victory buttons | RETRY `rect(60,120,200,40)`; MAIN MENU `rect(60,168,200,40)` | same | scale 1 |

Touch hit boxes (bottom px):

| Region | Rect | Action |
|---|---|---|
| map cell `(col,row)` | `(4+col*12, 4+row*12, 12, 12)`, col 0..25, row 0..12 | §10.3 |
| build 1/2/3 | `(6,164,100,28)` / `(112,164,100,28)` / `(218,164,100,28)` | select/arm element |
| UPGRADE | `(6,196,60,28)` | upgrade |
| SELL | `(68,196,60,28)` | sell (two taps) |
| NEXT WAVE | `(130,196,60,28)` | `game_start_next_wave` (build phase only) |
| SPEED | `(192,196,60,28)` | cycle 1/2/4 |
| PAUSE | `(254,196,60,28)` | pause/resume |
| menu START | `(40,84,240,56)` | `game_new_run` |
| menu CONTROLS | `(40,152,240,32)` | help overlay |
| pause RESUME/SPEED/QUIT | `(60,80,200,36)` / `(60,124,200,36)` / `(60,168,200,36)` | |
| end RETRY / MAIN MENU | `(60,120,200,40)` / `(60,168,200,40)` | `game_new_run` / `game_init` |

### 11.4 ASCII mockups (not to scale)

**Top screen (400x240).** Legend: `.` empty buildable cell, `#` path cell, `F`/`W` towers, `e` enemy, `+` cursor. Field is 26x13 @15 px (`FIELD_X=5`, `FIELD_Y=24`); path = rows 1/5/9 + shafts per §2.3.

```
+--------------------------------------------------------------------------------+
| (o) 150   W1/25   LEFT 5    LIVES 20        BUILD                    1x        |  HUD y0..23
+--------------------------------------------------------------------------------+
|+------------------------------------------------------------------------------+|
||. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . ||  row 0
||# # # # # # # # # # # # e # # # # # # # # # # # # . .                         ||  row 1  PATH
||. . . . . . . . . . . . . . . . . . . . . . . . # . .                         ||  row 2
||. . . F . . . . . . . . . . . . . . . . . . . . # . .                         ||  row 3
||. . . . . . . . . . . . . . . . . . . . . . . . # . .                         ||  row 4
||. . # # # # # # # # # # # # # # # # # # # # # # # . .                         ||  row 5  PATH
||. . . . . . . . . . . . . . . . . . . . . . . . . . .                         ||  row 6
||. . . . . . . . . . . . . . . . . . . . . . . . . . .                         ||  row 7
||. . . . . . . . . . . . . . . . . . . . . . . . . . .                         ||  row 8
||. . # # # # # # # # # # # # # # e # # # # # # # # # # #                         ||  row 9  PATH
||. . . . . . . . . . . . . . . . . . . . . . . . . . .                         ||  row 10
||. . . . . . . . . . . . . . . . . . . . . . . . . . .                         ||  row 11
||+ W . . . . . . . . . . . . . . . . . . . . . . . . . .                       ||  row 12 (W = sample)
|+------------------------------------------------------------------------------+|
+--------------------------------------------------------------------------------+
| FIRE 20G - TAP A CELL                                                            |  tooltip y220..239
+--------------------------------------------------------------------------------+
```

**Bottom screen (320x240).** Touch map = same 26x13 grid at 12 px (`MAP_X=4`, `MAP_Y=4`), then the control rows.

```
+--------------------------------------------------------------------------+
|+------------------------------------------------------------------------+|
||. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .||  touch map
||# # # # # # # # # # # # e # # # # # # # # # # # # . .                    ||  312x156, 12 px cells
||. . . . . . . . . . . . . . . . . . . . . . . . # . .                    ||  rows 0..12
||. . . F . . . . . . . . . . . . . . . . . . . . # . .                    ||
||. . . . . . . . . . . . . . . . . . . . . . . . # . .                    ||
||. . # # # # # # # # # # # # # # # # # # # # # # # . .                    ||
||. . . . . . . . . . . . . . . . . . . . . . . . . . .                    ||
||. . . . . . . . . . . . . . . . . . . . . . . . . . .                    ||
||. . . . . . . . . . . . . . . . . . . . . . . . . . .                    ||
||. . # # # # # # # # # # # # # # e # # # # # # # # # # #                    ||
||. . . . . . . . . . . . . . . . . . . . . . . . . . .                    ||
||. . . . . . . . . . . . . . . . . . . . . . . . . . .                    ||
||. . . . . . . . . . . . . . . . . . . . . . . . . . .                    ||
||. . . . . . . . . . . . . . . . . . . . . . . . . . .                    ||
|+------------------------------------------------------------------------+|
+--------------------------------------------------------------------------+
| [ FIRE 20G ]       [ WATER 20G ]       [ FOREST 20G ]                     |  build row y164..191
+--------------------------------------------------------------------------+
| [UPGRADE]  [SELL]   [NEXT]   [SPEED]   [PAUSE]                            |  action row y196..223
|   80G       60G     WAVE      1x                                          |
+--------------------------------------------------------------------------+
|  F x17     W x17     Fo x19     EL x10                                    |  preview strip y224..239
+--------------------------------------------------------------------------+
```

### 11.5 Draw order & budget

Top: clear -> HUD bar/coin/texts -> field frame -> field bg -> path cells (76 from `g->path.pts`) -> grid lines -> selected range -> towers (icon + level) -> beams -> enemies (icon + elite outline + health bar) -> cursor -> build preview -> tooltip strip -> overlays.
Bottom: clear -> map frame/bg -> map path cells (76) -> map grid lines -> map tower cells -> map enemies -> map cursor/selection -> build row -> action row -> preview strip -> overlays.

Rough primitive count during wave 25: top = 1 clear + ~12 rects + 76 path rects + 41 lines + ~1 range circle + (<=96 towers x ~7 triangles) + (<=63 enemies x ~6-8 primitives) + <=52 beams x2 + cursor 7 + tooltip ~2 = ~1300 triangles/quads at worst. Fine at 60 fps if colours are batched where trivial. Avoid per-frame text formatting for static labels; only sprintf values that change (gold, wave, left, lives).

### 11.6 Stereo / screens

The top screen is display-only and flat 2D: render one 2D target (`GFX_LEFT`). Stereo duplication doubles top-screen draw cost for no gameplay benefit; leave the 3D slider without depth content. The bottom screen is 2D touch.

---

## 12. Audio mapping

### 12.1 Event -> sound `[LUA]` + `game.h` events

| `Game.events` bit | Sound file | When | Notes |
|---|---|---|---|
| `GAME_EVT_BUILD` | `build.wav` | successful tower placement | |
| `GAME_EVT_SELL` | `sell.wav` | confirmed sale | |
| `GAME_EVT_UPGRADE` | `upgrade_1..6.wav` | successful upgrade | random one of 6; no level/element mapping |
| `GAME_EVT_LASER` | `laser.wav` | tower fires | platform must throttle globally to **0.08 s** game time (Lua `laserSoundCooldown`) |
| `GAME_EVT_ENEMY_DIE` | `enemy_die.wav` | enemy killed | |
| `GAME_EVT_WAVE_START` | `wave_start.wav` | NEXT WAVE | every wave |
| `GAME_EVT_LEAK` | — | enemy leaks | no Lua sound; optional soft "life lost" sfx |
| `GAME_EVT_GAMEOVER` | `gameover.wav` | lives reach 0 | sound exists but was **never played** in the Lua; now used |
| `GAME_EVT_VICTORY` | — (optional) | wave 25 cleared | no asset; reuse `wave_start.wav` or silent |
| `GAME_EVT_*` cleared | `game_clear_events` | after the platform consumes them each frame | |

Music `[LUA]`: `menu_music.ogg` loops at volume 0.5 from boot and stops when a run starts; `game_music.ogg` loops at 0.5 during `BUILD`/`WAVE`; on game over stop game music and play the stinger; on victory stop game music (optional victory jingle); quit-to-menu restarts menu music. Pause does not stop music in the Lua.

`playSound` semantics in Lua: `stop()` then `play()` (retrigger, no overlapping same-source voices). `soundEnabled` is never toggled — no mute UI exists.

### 12.2 Audio inventory

| File | Bytes | Format | Est. duration (16-bit stereo 44.1 kHz WAV assumption) | 3DS plan |
|---|---:|---|---:|---|
| `build.wav` | 305,998 | WAV | ~1.73 s | keep |
| `enemy_die.wav` | 6,318 | WAV | ~0.04 s | keep |
| `gameover.wav` | 10,497,182 | WAV | ~59.5 s — **huge** | trim to 2–3 s stinger |
| `game_music.ogg` | 3,590,538 | OGG | codec-dependent | optional (§12.3) |
| `laser.wav` | 7,758 | WAV | ~0.04 s | keep, quiet mix |
| `menu_music.ogg` | 1,705,932 | OGG | codec-dependent | optional |
| `sell.wav` | 93,518 | WAV | ~0.53 s | keep |
| `upgrade_1..6.wav` | 76,110 / 78,158 / 79,694 / 71,502 / 76,622 / 63,310 | WAV | 0.36–0.45 s each | keep |
| `wave_start.wav` | 151,278 | WAV | ~0.86 s | keep |

### 12.3 3DS audio recommendations `[PORT]`

- Convert SFX to 16-bit **mono 22050 Hz** (or IMA-ADPCM) and embed via `bin2s`; a 1 s mono 22.05 kHz PCM is 44 KB. Whole SFX set trimmed well under 400 KB.
- Trim `gameover.wav` (10.5 MB is unacceptable): 2 s mono stinger ~88 KB.
- Music: no OGG decoder in libctru. Options in order: (a) omit (music optional); (b) pre-decode a 15–30 s loop to mono 22.05 kHz PCM and loop from RAM (~0.7–1.5 MB); (c) integrate a streaming decoder (not v1). Do not decode OGG at runtime while rendering.
- `ndsp`: >= 8 SFX channels + separate music channels; the 0.08 s laser throttle is mandatory to avoid channel spam.
- Audio lives in the platform layer (`source/plat_3ds.c`), driven by `Game.events` + music by phase; no separate audio module exists in the repo and none is required by this spec.

---

## 13. What likely made the old 3DS attempt fail (evidence)

1. **Absolute 1280x720 layout, no resolution independence.** `love.conf` demands 1280x720 (`main.lua:11-18`), `love.window.setMode(1280,720,...)` (`main.lua:1495-1501`), and every coordinate is absolute: HUD at y 505..720, info panel x 490..1050, grid offset x=276, menu panel 700x560. On a 400x240 screen you see the top-left corner only: columns 1–4 of the field, the title partially off-screen, the whole HUD below the screen. No scale transform exists. This is the README's "half the screen cut off".
2. **Per-frame immediate-mode primitive count.** `drawGradientBackground` draws **721 filled rectangles every frame** (`main.lua:437-443`); the field does 338 cells/frame with an `isPathCell()` linear scan of the path (`main.lua:949-968`, `482-490`); icons are 6–10-vertex polygons; five TrueType fonts are rasterized. On the 3DS ARM11 through LovePotion each primitive is a CPU-tessellated draw; this is the likely fps killer.
3. **Unbounded per-frame allocations on the menu.** 4 particle tables spawned every frame with no cap (`main.lua:600-615`) -> ~600 live Lua tables at steady state, constant GC churn; a laser table per shot; four neighbour tables per BFS node.
4. **BFS cost/hitching (original maze mechanic).** `findPath()` allocates 4 neighbour tables per expanded node, uses `table.remove(queue,1)` (O(n) shift), and runs **twice per successful build** (validate + update, `main.lua:1362-1364`), once per failed build, once per sell. Maze building — the core strategy — is exactly when the pathfinder is stressed. **This is removed by the locked fixed-path decision (L1/L2)**: the port has no runtime BFS at all, just a 76-cell const path.
5. **Audio memory/decode.** 10.5 MB `gameover.wav`, 3.6 MB + 1.7 MB OGGs; OGG streaming decode on ARM11 competes with rendering and loading dominates boot.
6. **Touch controls fought keyboard emulation (fatal).** `love.touchpressed` sets the cursor then calls `love.keypressed("a")` (`main.lua:1463-1488`). In `MOVING`, `a` means "cursor left" (`main.lua:1387-1393`), so every tap moves the cursor one cell left of the tap; entering `BUILDING` needs `return`/`space`, which touch cannot send; in `BUILDING`, `a` is intercepted again to cycle tower type (`main.lua:1336-1340`). On touch-only hardware the game is unbuildable — only the menu START button worked. README's "touch controls fighting the mouse emulation" matches exactly.
7. **No performance escape hatches:** no batching, no atlas, no culling, no reduced detail, no fixed timestep, no resolution factor.
8. **The Lua game itself had no end state:** leaking restarted the same wave forever with gold rewards, so it could neither be lost nor won (`main.lua:779-798`) — looks broken at any frame rate.

The C rewrite removes 1–7 structurally: fixed 400x240/320x240 layout (§11), cell-space silent core with preallocated arrays (`game.h`), no runtime BFS (L1), fixed-step deterministic update, touch-native input (§10), bounded particle counts (renderer-local cap), and proper endings (L3/L4/L5).

---

## 14. Known bugs / unclear bits

### 14.1 Original defects relevant to the rewrite

| ID | Lua ref | Defect | Required C behaviour |
|---|---|---|---|
| B1 | `main.lua:1336-1344`, `1387-1393`, `1406-1415` | `a` is aliased to "cursor left"/"previous type" and tested before the confirm branch; keyboard A never builds or selects; help text lies | A = confirm button; D-pad = cursor (§10.2) |
| B2 | `main.lua:698-707`, `779-798` | Leaking paid +2/+10 gold and restarted the same wave forever; no lives, no game over, no victory | L3/L4/L5: build phase, 20 lives, -1 per leak, victory at 25, game over at 0 |
| B3 | `main.lua:644-648`, `1031-1039`, `1295-1301` | Intended build-before-wave-1 phase is dead code because menu confirm sets `gameStarted=true` and calls `startWave()` immediately | `game_new_run` -> `PHASE_BUILD`; wave 1 only on NEXT WAVE (L3) |
| B4 | `main.lua:794-798` | Waves auto-chain in the same frame; status `COMPLETE` never visible | `PHASE_BUILD` between waves, player-paced; HUD `BUILD` |
| B5 | `main.lua:63-69`, `1353` | BFS seeds the start cell unconditionally, so a tower could be built on the spawn cell (end cell could not) | fixed path; start/end are path cells -> never buildable (L2) |
| B6 | `main.lua:558-565` | HUD quotas are computed from `base - E` but regular spawn slots are `base`, so the displayed composition is off by `E` on elite waves | quotas from `totalSpawns - E = base` (§6.1/6.2) |
| B6b | `main.lua:573-576` | Spawner ignores quotas and picks uniformly at random | exact quotas (D5), §6.4; toggle for Lua-exact randomness |
| B7 | `main.lua:1235-1237` | `"X: " .. "MAX" .. "G"` renders `X: MAXG` | show `UP MAX` (§11.2 tooltip) |
| B8 | `main.lua:1103` | `printf(..., x=1250, limit=200, "right")` draws the header off-screen right (edge 1450) | composition moved to bottom preview strip (§11.3) |
| B9 | `main.lua:600-615` | 4 particle tables/frame, no cap, `life`/`maxLife` independent | renderer-local particle array, max 48, 1 spawn/frame, `maxLife=life` |
| B10 | `main.lua:883-918`, `1463-1488` | Help overlay and toast do not block touch; overlay only blocks keys | input gated by `help_open`/paused; map taps ignored under overlays |
| B11 | `main.lua:1311-1314` | Toast swallows one key press | toast never swallows input; display only (§9.7) |
| B12 | `main.lua:188` | global `state` (build mode) vs `gameState` | separate `GamePhase` + `selectedTower`/`placing` (§1.2) |
| B13 | `main.lua:136`, `145`, `154`, `159-161` | `glowColor` fields never read | keep in `TowerSpec.glow`/`EnemySpec.glow`, no behaviour |
| B14 | `main.lua:502-508` | `getTotalCost` hardcodes a base cost of 20 | `Tower.invested` accumulates real spend (§7.2) |
| B15 | `main.lua:199`, `201`, `764` | `gameTime` and `totalKills` written but never displayed | `kills` on the game-over/victory overlay; `kills` also used for score |
| B16 | `main.lua:263` | `gameover.wav` loaded, never played | play on `GAME_EVT_GAMEOVER` |
| B17 | `main.lua:206`, `570`, `783-792` | `waveRetry`/`enemiesThatPassed` encode a broken retry mechanic | removed; leaks cost lives, wave advances (L3/L4) |
| B18 | `main.lua:413-417` | health-bar fill can have negative width if hp < 0 before removal | clamp `p` to `[0,1]` (§5.4) |
| B19 | `main.lua:217-219`, `1316-1325` | speeds {0, 0.5, 1, 2, 4}; pause is a speed hack; half-speed level | engine contract: 1/2/4 + `paused` (§1.7); 0.5x dropped |
| B20 | `main.lua:140-145` | `getDamageMultiplier` has 1.0 fallbacks, unknown elements impossible | keep default 1.0 |

### 14.2 `[LOCKED]` deliberate deviations from the Lua (do not "fix back")

| # | Deviation | Lua |
|---|---|---|
| D1 | Fixed path, **hand-authored serpentine (76 cells, §2.3)**, computed once, never recomputed; `pathBlocked` always false | BFS on every build/sell; initial path was a straight bottom row |
| D2 | Towers only on non-path cells; no maze/redirection | build on path cells allowed |
| D3 | Build phase between waves, player-paced NEXT WAVE, no timer | instant same-frame auto-chain |
| D4 | 20 lives; leak = -1 life (elites too), 0 gold; game over at 0 | no lives; leak paid +2/+10 and retried forever |
| **D5** | **Exact element quotas for spawns (truthful `game_wave_preview`)** | uniform random spawn element |
| D6 | Victory after wave 25; `totalWaves = 25` | endless, no victory |
| D7 | Pause overlay + separate `paused`; speed 1/2/4 only (no 0.5x, no 0x) | speed 0/0.5/1/2/4, no pause UI |
| D8 | **L/R = cycle tower type; Select = cycle speed; Start = pause**; help overlay = bottom CONTROLS button | L/R = speed; SELECT = help; no pause |
| D9 | Game-over/victory screens + `gameover.wav` | missing state, unused sound |
| D10 | 3DS layout 400x240/320x240, 8x8 bitmap font, flat background (no 721-strip gradient), grid as 41 lines instead of 338 cell outlines, no tower-cell tint, renderer particles capped at 48 | 1280x720 layout, TrueType fonts, per-cell outlines, 721 rects, unbounded particles |
| D11 | Toast does not swallow input | swallows one key |
| D12 | Fixed 1/60 s timestep | LÖVE variable dt |

### 14.3 Original behaviours kept intentionally

- Hitscan towers with 0.15 s beam visual, nearest-target tie-break by spawn order, cooldown semantics (§3.2).
- Damage matrix, costs, kill rewards, upgrade curve, sell refund, enemy HP/speed formulas, wave counts/elite schedule/spawn interval (§4, §6, §7, §8).
- Towers may be built/upgraded/sold during a wave (the Lua allows it); only NEXT WAVE is restricted to `PHASE_BUILD`.
- Elite spawns burst at the start of a wave (slots 2..1+E), not spread out (this is what the Lua actually does, §6.3).
- Menu particle drift, colours, and the general pixel-art vocabulary.

### 14.4 Decisions already resolved by the Lead (no action needed)

| Was | Resolution |
|---|---|
| Path shape | **Locked**: hand-authored serpentine, 76 cells / 75 segments, start `(0,1)`, end `(25,9)` (§2.3, Appendix C). Not BFS, not the Lua's straight row. |
| Speed levels | **Locked**: 1x/2x/4x + separate pause overlay; Lua's 0.5x and 0x dropped (D7). |
| Tower cap | **Locked**: `MAX_TOWERS 96` with message `"TOWER LIMIT 96"` (§2.5). |
| Exact quotas | **Locked**: exact element quotas, truthful preview (D5, §6.4). |
| Elite spawn timing | **Locked**: first E spawns after slot 1 are the elites, consecutive (§6.3); drop delay-array logic. |
| Elite leak penalty | **Locked**: -1 life per leak regardless of elite (§5.3, §7.1). |
| Wave flow | **Locked**: no timer/intermission/countdown; after a wave resolves phase = `PHASE_BUILD` until NEXT WAVE (§1.4). |
| Victory | **Locked**: 25 waves (§6.5). |
| L/R + Select + Start | **Locked**: L/R = cycle tower type; Select = cycle speed; Start = pause (D8). |
| Sell refund | Confirmed from the Lua: `floor(totalInvested * 0.75)` (§7.2). |
| Element roster | **Locked**: three elements only, no combining (§4.3). |
| Tower limit message | **Locked**: `"TOWER LIMIT 96"`. |

### 14.5 Still genuinely ambiguous / worth a quick call

1. **Economy actions during `PHASE_WAVE`.** This spec allows build/upgrade/sell during a wave (the Lua allows it at any time); the locks only restrict NEXT WAVE to `PHASE_BUILD`. If the intent is build-phase-only economy, add one gate in `game_place_tower`/`game_upgrade_selected`/`game_sell_selected` (message `"BUILD PHASE ONLY"`). One-line change, no other numbers affected.
2. **In-game help.** Select is speed, so the Lua-style help overlay is only reachable from the menu's CONTROLS button (or the pause overlay). If in-game help is wanted, a small `?` button is needed — the bottom panel currently has no free slot; would cost 12 px of map height or a 6th action button.
3. **Default cursor cell.** Set to `(1,0)` (buildable, adjacent to the row-1 path). Purely cosmetic.
4. **Music.** Optional in the spec (§12.3): ship SFX only, or include a pre-decoded 15–30 s PCM loop? `gameover.wav` must be trimmed either way.
5. **Overlay copy.** Game-over/victory stat lines (`WAVE x/25`, `KILLS`, `LEAKS`, `LIVES`) are the spec's choice; exact wording is the UI owner's.

### 14.6 Balance notes (25-wave run, serpentine path)

- Tower damage doubles per level while enemy HP grows linearly, so late-run towers are hugely overkill: L10 = 1536 dmg / 0.28 s = 5486 DPS vs wave-25 regular HP 87. The economy caps this (full L10 tower = 10,240 > total run income 7,580), so it should be fine; verify by playtest.
- The serpentine path gives ~12 usable rows (still not all: row 12 is decorative), so there is real space for towers. Wave 1 is tight with 150 gold (7 towers) and range 2.14 cells: an enemy is in range of a given tower for roughly 0.86 s at wave-1 speed (one shot of 3 damage vs 15 HP), so early placement must cluster. If wave 1 feels like a wall, tune `STARTING_GOLD` or wave-1 HP, not the formulas.
- Wave 25 (63 spawns, 10 elites, interval 1.75 s, elite HP 217.5) is the intended finale. Expect a first clear to be winnable but not trivial around 20–30 mid-level towers.
- No income besides kills means leaks reduce both lives and income (double punishment, per L4); there is no wave-completion bonus.

---

## 15. Implementation guide (actual repo layout)

### 15.1 Responsibilities split (no new module names)

| Layer | Files | Owns |
|---|---|---|
| Core (host-testable, no 3DS headers) | `include/game.h` (frozen API), `source/core/game.c`, `source/core/grid.c`, `source/core/data.c` | grid + locked path data, economy, wave/spawn/elite schedule, combat, phases, lives, RNG, events, messages |
| Renderer 3DS | `source/render.c` (field/HUD), `source/ui.c` (menu/panels/overlays/touch hit-testing) | both screens, 15 px / 12 px layout, `gfx.h` primitives + 8x8 text, drawing from core state, particles, overlays |
| Platform 3DS | `source/plat_3ds.c` | citro2d implementation of `include/gfx.h`, `hidKeys*`/`hidTouch*` input mapped to core APIs, ndsp SFX + 0.08 s laser throttle + music by phase |
| Host preview | `tools/preview/*` | host `gfx.h` backend (software rasterizer / BMP frames) + preview harness; no gameplay logic |
| Build wiring | repo `Makefile` | adding `source/render.c`, `source/ui.c`, `source/plat_3ds.c` (and `tools/preview`) is the engine/CI owner's call; do not invent modules |

### 15.2 Content tables to fill (exact values)

`TowerSpec` (3 entries, index = Element):
```
{ "Fire",  20, 3.0f, 0.8f, 2.142857f, {0.95f,0.15f,0.15f}, {0.95f,0.3f,0.1f} }
{ "Water", 20, 3.0f, 0.8f, 2.142857f, {0.15f,0.40f,0.95f}, {0.10f,0.5f,0.95f} }
{ "Forest",20, 3.0f, 0.8f, 2.142857f, {0.15f,0.90f,0.15f}, {0.10f,0.9f,0.30f} }
```
`EnemySpec` (3 entries, index = Element): names "Fire"/"Water"/"Forest"; same colours/glows as the tower table.

`game_tower_stats(type, level, *damage, *fireRate, *range)`:
```
damage   = 3 * (1 << (level-1))                      // 3 .. 1536
fireRate = {0.80,0.72,0.64,0.57,0.51,0.45,0.40,0.36,0.32,0.28}[level-1]
range    = 2.142857f                                  // constant
```
`game_upgrade_cost(g,i)` = `-1` if level>=10 else `20 << (level-1)`; `game_sell_value(g,i)` = `tower.invested * 3 / 4`.
`game_damage_multiplier(t,e)` = matrix in §4.1.
`game_wave_preview` = §6.2 quotas for `g->wave`.
`game_path_length(g)` = `g->path.length`; `game_cell_center(c,r)` = `(c+0.5f, r+0.5f)`.

### 15.3 `game_init` / `game_new_run`

`game_init`: `memset` zero, `phase = PHASE_MENU`, `totalWaves = 25`, `selectedType = 0`, `selectedTower = -1`, `speedMultiplier = 1.0f`, `rng` seeded from a fixed constant (menu has no simulation). Renderer owns menu particles.

`game_new_run(g, seed)`: zero the run fields (keep `rng` = seed), `phase = PHASE_BUILD`, `gold = 150`, `lives = 20`, `wave = 1`, `totalWaves = 25`, `kills = leaks = 0`, clear towers/enemies/beams/occupancy, `selectedType = 0`, `selectedTower = -1`, `placing = false`, `cursor = (1,0)` (buildable cell next to the row-1 path; the Lua's cursor started top-left — cosmetic), `speedMultiplier = 1.0f` (Lua starts at 1x), `paused = false`, `path` = locked const (no BFS), `pathBlocked = false`.

### 15.4 Update/step contract

- `game_update(g, realDt)`: `stepAccumulator += clamp(realDt, 0, 0.1)`; `while (stepAccumulator >= 1/60) { game_step(g, 1/60); stepAccumulator -= 1/60; }`.
- `game_step(g, dt)`: §1.5 exactly. No allocation, no I/O, deterministic given `rng`.
- Phase checks first: MENU/GAMEOVER/VICTORY return; paused returns.
- Timers use **scaled** dt (after `dt *= speedMultiplier`).
- Events accumulate in `g->events` until the platform calls `game_clear_events`.

### 15.5 Action contracts (behaviour the renderer can rely on)

| Function | Contract |
|---|---|
| `game_set_cursor` | clamp to grid; does not change selection |
| `game_set_selected_type` | clamp 0..2; does not arm placement |
| `game_place_tower` | §2.5; on success emits BUILD; on failure sets message/timer and returns false |
| `game_select_tower(idx)` | `idx` in `[0,towerCount)` selects; `-1` clears; other values return false |
| `game_upgrade_selected` | level<10, gold>=cost; updates level/damage/fireRate via computed stats, `invested += cost`, emits UPGRADE |
| `game_sell_selected` | adds `invested*3/4`, removes tower, compacts array + occupancy, clears selection, emits SELL |
| `game_start_next_wave` | only in PHASE_BUILD; sets spawn state (§6), phase WAVE, emits WAVE_START |
| `game_toggle_pause` | toggles `paused` in BUILD/WAVE |
| `game_cycle_speed` | 1 -> 2 -> 4 -> 1 |
| `game_wave_preview` | §6.2 quotas for `g->wave` |
| `game_clear_events` / `game_clear_message` | zero fields |

### 15.6 Fixed path data (locked serpentine)

```c
// 76 cells, start (0,1), end (25,9); path.length = 75.0f
static void build_fixed_path(Path *p, bool path_cell[GRID_ROWS][GRID_COLS]) {
    int n = 0;
    #define ADD(c, r) do {                         \
        p->pts[n].x = (c) + 0.5f;                  \
        p->pts[n].y = (r) + 0.5f;                  \
        path_cell[(r)][(c)] = true;                \
        n++;                                       \
    } while (0)

    for (int c = 0; c <= 23; ++c)  ADD(c, 1);      // 24 cells, row 1 left->right
    for (int r = 2; r <= 5;  ++r)  ADD(23, r);     // 4 cells, col 23 down
    for (int c = 22; c >= 2; --c)  ADD(c, 5);      // 21 cells, row 5 right->left
    for (int r = 6; r <= 9;  ++r)  ADD(2, r);      // 4 cells, col 2 down
    for (int c = 3; c <= 25; ++c)  ADD(c, 9);      // 23 cells, row 9 left->right
    #undef ADD

    p->count  = n;                 // 76
    p->length = (float)(n - 1);    // 75.0f
}
```

Unit-test invariants: `n == 76`; consecutive points are unit 4-adjacent (`|dc|+|dr| == 1`); first point `(0.5,1.5)`, last `(25.5,9.5)`; all 76 `path_cell` entries true and no duplicates.

### 15.7 Acceptance tests (numbers must match)

| Test | Expected |
|---|---|
| `game_new_run` | PHASE_BUILD, gold 150, lives 20, wave 1, totalWaves 25, 0 towers, cursor (1,0) |
| path | 76 points, start `(0,1)`, end `(25,9)`, every consecutive pair unit-adjacent; `path.length == 75.0f`; `game_path_length == 75.0f`; all 76 cells reject placement |
| placement rules | path cell rejected `"CANNOT BUILD ON THE PATH"`; empty non-path cell accepted for 20 gold; second tower on same cell rejected `"CELL OCCUPIED"`; 97th tower rejected `"TOWER LIMIT 96"`; gold 150 -> 130 after one tower |
| wave 1 start | `enemiesToSpawn == 5`, `spawnInterval == 2.95f`, first spawn at t=2.95 s, spawn position `(0.5,1.5)`; quotas 1/1/3; no elites |
| wave 5 | 14 spawns, 1 elite at spawn slot 2, regular HP 27, elite HP 67.5; quotas 4/4/5 |
| wave 10 | 26 spawns, 3 elites (slots 2–4), elite HP 105; quotas 7/7/9 |
| wave 25 | 63 spawns, 10 elites (slots 2–11), HP 87/217.5, interval 1.75; quotas 17/17/19; clearing it -> PHASE_VICTORY |
| velocity | wave 1: 75*20/100 = 15.0 cells/s (5 s crossing); wave 25: 33.0 cells/s (2.27 s); wave 25 elite: 59.4 cells/s (1.26 s) |
| leak | lives -1, 0 gold, `GAME_EVT_LEAK`, enemy removed at pathPos 75 / centre `(25.5,9.5)`; lives 0 -> PHASE_GAMEOVER + `GAME_EVT_GAMEOVER` |
| kill | +10 gold, kills+1, enemy removed, `GAME_EVT_ENEMY_DIE` |
| tower L1 | 3 dmg, 0.8 s cooldown, range 2.142857 cells; fire when a target is in range and cooldown 0 |
| upgrade | L1->L2 costs 20, damage 6, cooldown 0.72, invested 40; L10: cost -1/message `MAX LEVEL`, damage 1536, cooldown 0.28 |
| sell | L1 sells for 15, L3 for 60, L10 for 7680 (floor of invested*0.75) |
| multipliers | Fire vs Forest 2.0; Fire vs Water 0.5; Water vs Fire 2.0; Forest vs Water 2.0; same 1.0 |
| pause | `paused` freezes spawning/movement/cooldowns/beams/message timer; actions still work |
| speed | cycle 1->2->4->1; `dt *= speedMultiplier` inside the step |
| message | failed action sets `message` and `messageTimer = 2.0` (scaled dt); renderer shows it; no input swallowing |
| determinism | two `Game` instances with the same seed and same input sequence produce identical state bit-for-bit |

### 15.8 Performance budget

- Core: O(towers + enemies) per step with small constants (96 + 128 max); no allocation; < 0.5 ms/step on ARM11 expected.
- Renderer: ~1300 primitives worst case (§11.5); use `gfx.h` primitives, avoid per-frame `snprintf` for constants; text only when values change.
- Audio: <= 8 SFX channels; 1 music stream; no runtime decode of OGG if music is enabled (pre-decoded PCM loop, §12.3).

---

## Appendix A — locked path constants and layout

```c
/* grid */
#define GRID_COLS 26
#define GRID_ROWS 13
#define PATH_CELL_COUNT 76
#define PATH_LEN_CELLS 75.0f
#define START_COL 0
#define START_ROW 1
#define END_COL 25
#define END_ROW 9

/* top-screen layout (pixels) */
#define FIELD_X 5
#define FIELD_Y 24
#define CELL_PX 15
#define FIELD_W (GRID_COLS * CELL_PX)   /* 390 */
#define FIELD_H (GRID_ROWS * CELL_PX)   /* 195 */
#define HUD_H 24
#define TOOLTIP_Y 220
#define TOOLTIP_H 20
#define RANGE_DRAW_PX 32
#define TOWER_ICON_PX_LOW 7
#define TOWER_ICON_PX_MID 8
#define TOWER_ICON_PX_MAX 9
#define ENEMY_ICON_PX 5
#define ELITE_ICON_PX 9
#define HPBAR_W 13
#define HPBAR_H 3

/* bottom-screen layout (pixels) */
#define BOT_W 320
#define BOT_H 240
#define MAP_X 4
#define MAP_Y 4
#define MAP_CELL 12
#define MAP_W (GRID_COLS * MAP_CELL)    /* 312 */
#define MAP_H (GRID_ROWS * MAP_CELL)    /* 156 */

/* core gameplay (cells / seconds / gold) */
#define RANGE_CELLS 2.142857f
#define TOWER_COST 20
#define STARTING_GOLD 150
#define STARTING_LIVES 20
#define KILL_GOLD 10
#define LEAK_LIVES 1
#define TOTAL_WAVES 25
#define MAX_LEVEL 10
#define SIM_STEP (1.0f / 60.0f)
#define BEAM_LIFE 0.15f
#define BEAM_SFX_COOLDOWN 0.08f
#define MESSAGE_TIME 2.0f
#define ELITE_HP_MULT 2.5f
#define ELITE_SPEED_MULT 1.8f
#define ENEMY_HP_BASE 15.0f
#define ENEMY_HP_PER_WAVE 3.0f
#define ENEMY_SPEED_BASE 20.0f
#define ENEMY_SPEED_PER_WAVE 1.0f
#define SPAWN_BASE 3.0f
#define SPAWN_PER_WAVE 0.05f
#define SPAWN_MIN 0.8f
#define EXACT_QUOTAS 1

/* upgrade tables (level index 0..9) */
static const int   UPGRADE_COST[10] = {20,40,80,160,320,640,1280,2560,5120,0};
static const float FIRE_RATE[10]    = {0.80f,0.72f,0.64f,0.57f,0.51f,0.45f,0.40f,0.36f,0.32f,0.28f};
static const int   DAMAGE[10]       = {3,6,12,24,48,96,192,384,768,1536};
/* invested == 20 << (level-1); sell == invested * 3 / 4 */
```

## Appendix B — renderer conversion cheat-sheet

```
cell -> top px:        px = FIELD_X + cellx * 15;  py = FIELD_Y + celly * 15
cell -> bottom px:     px = MAP_X  + cellx * 12;  py = MAP_Y  + celly * 12
top range radius:      RANGE_CELLS * 15 = 32.14 -> 32 px
bottom range radius:   RANGE_CELLS * 12 = 25.7  -> 26 px (optional, not drawn)
tower icon radius px:  round((12 + min(level,10)*0.4) * 15/28)  -> 7/8/9
enemy icon radius px:  5 (normal), 9 (elite)
health bar:            bg (cx-6, cy-10, 13, 3) black a=0.7
                       fill (cx-5, cy-9, floor(11p), 2) rgb(1-p, p, 0)
beam:                  1 px at a=p, 3 px at a=0.1p (glow), p = life/0.15
path drawing:          iterate g->path.pts (76 cells) for BOTH screens; never hardcode a road
text:                  gfx 8x8; scale 1 = 8 px, scale 2 = 16 px; (x,y) = top-left
                       scale 1: 1 glyph = 8 px wide; screen width 400 = 50 glyphs
```

## Appendix C — locked serpentine path (authoritative)

Locked by the Lead; this is the only path (no straight-row fallback). 76 cells / 75 segments, start `(0,1)`, end `(25,9)`:

| Segment | Cells | Count |
|---|---|---|
| Row 1, left->right | `(0,1)` .. `(23,1)` | 24 |
| Col 23, downward | `(23,2)` .. `(23,5)` | 4 |
| Row 5, right->left | `(22,5)` .. `(2,5)` | 21 |
| Col 2, downward | `(2,6)` .. `(2,9)` | 4 |
| Row 9, left->right | `(3,9)` .. `(25,9)` | 23 |

Build code and invariants: §15.6. Properties: contiguous, 4-connected, no duplicates, no self-intersection; enemy traversal time is `100/speedParam` seconds regardless of path length (§5.3); ~12 of 13 grid rows have at least one buildable cell within 2.142857 cells of the path.

## Appendix D — Lua reference index

| Topic | `main.lua` lines |
|---|---|
| Window config / screen constants / grid + offsets | 11-52 |
| Pathfinding (BFS, updatePath) — **not ported** | 54-123 |
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
| `love.keypressed` (input, build state, economy) | 1285-1458 |
| `love.touchpressed` | 1460-1488 |
| `love.load` | 1490-1519 |
| Sound loading / helpers | 243-287 |
