# Elemental Defense — 3DS Port Specification

**Single source of truth for the C rewrite (devkitARM + libctru + citro2d).**

| Item | Value |
|---|---|
| Original source | `C:\Users\Raine\Desktop\ElementalDefense\game\main.lua` (LÖVE2D, 1519 lines, 51,302 bytes) — read in full |
| Original audio | `C:\Users\Raine\Desktop\ElementalDefense\game\sounds\` (16 files, listed in §12) |
| Original render target | 1280 x 720 window, 60 fps, default LÖVE 11 fonts |
| 3DS target | top 400x240 (landscape), bottom 320x240 touch, 60 Hz fixed step, citro2d |
| This document | `Attempt-2-3ds-game/docs/design.md` |

**Tagging convention used throughout**

- `[LUA]` — value/behaviour copied **verbatim** from `main.lua` (no rounding, no reinterpretation).
- `[DERIVED]` — computed exactly from a `[LUA]` formula (arithmetic only, shown where it matters).
- `[PORT]` — **not in the Lua**. A new decision required to make a playable 3DS game, or a deliberate fix for a broken/unimplemented part of the Lua. Every `[PORT]` decision is repeated in §14.2/§14.3 and needs the Lead's sign-off.

**The game in one paragraph.** A 26x13 grid tower defense. Creeps spawn at cell (1,13) bottom-left and walk a BFS-shortest path to (26,13) bottom-right. The player spends gold on one of three elemental towers (Fire/Water/Forest, all identical base stats, rock-paper-scissors damage multipliers) and can upgrade each tower from level 1 to 10. Towers are **hitscan** — they instantly damage the nearest enemy in a 60 px radius every `fireRate` seconds and draw a 0.15 s laser line. Building on a path cell reroutes the path (BFS is recomputed); if no path remains the build is rejected. Waves are endless, auto-generated from linear formulas, with elites on waves divisible by 5/10/25. The Lua has **no lives, no game over, no victory and no pause state** — those are added by this spec as `[PORT]` items.

---

## 0. Conventions

- Lengths are pixels unless a cell/tile unit is stated. Times are seconds. The Lua uses delta time (`dt` in seconds from LÖVE); the 3DS port uses a fixed step (§1.7).
- Colours are written as Lua floats `(r,g,b)` / `(r,g,b,a)` in `[LUA]` blocks and as `(R,G,B)` 0–255 in 3DS blocks. Conversion: `u8 = round(f * 255)` (round half up). Alpha is always 1 unless written.
- `t` = **normalized path progress**, `0.0` at the first path point (start cell centre) and `1.0` at the last (end cell centre). See §2.5. Enemy speed is therefore expressed in "1/100 path per second".
- Original screen space: 1280x720, origin top-left, y down. 3DS top screen: 400x240. 3DS bottom screen: 320x240. Both origin top-left.
- The Lua has two different "state" variables: outer `gameState` ("MENU" / "PLAYING") and inner `state` ("MOVING" / "BUILDING" / "TOWER_SELECTED"). This document calls them **game state** and **build state**; the C code must use distinct names (`game_state`, `build_state`).

---

## 1. Game loop & state machine

### 1.1 Original update order (exact, `main.lua:620-799`)

Every frame `love.update(dt)` runs in this exact order. The order matters (e.g. spawning happens before movement, firing happens after movement, completion is checked after firing).

```
1.  if gameState == "MENU": updateMenuParticles(dt); return        (no gameplay update at all)
2.  dt = dt * gameSpeed
3.  laserSoundCooldown = max(0, laserSoundCooldown - dt)
4.  if errorTimer > 0: errorTimer -= dt; clamp to 0
5.  gameTime += dt
6.  for lasers: life -= dt; remove when life <= 0
7.  wave start logic:
      if wave == 1 and not gameStarted and not waveActive: do nothing   -- "waiting for player" (see §14 B3)
      elif not waveActive and not waveComplete and gameStarted: startWave()
8.  if waveActive:
      enemySpawnTimer += dt
      if enemySpawnTimer >= enemySpawnInterval and enemiesSpawned < enemiesPerWave:
          enemySpawnTimer = 0; enemiesSpawned += 1
          decide isElite from waveEvents (exact algorithm §6.5)
          if pathPositions is non-empty:
              health = 15 + (wave-1)*3 ; speed = 20 + (wave-1)*1
              if isElite: health *= 2.5 ; speed *= 1.8
              spawn enemy at pathPositions[1] with t = 0
9.  for each enemy (reverse index):
      t += dt * (speed / 100)
      if t >= 1: remove; enemiesRemaining -= 1; enemiesThatPassed += 1
                  gold += isElite ? 10 : 2            -- leaking PAYS the player (§14 B2)
      else: x,y = getPathPosition(t)
10. for col 1..26, for row 1..13: if tower:
      cooldown = max(0, cooldown - dt)
      target = nearest enemy within towerTypes[type].range (ties keep earlier array entry)
      if target and cooldown <= 0:
          cooldown = tower.fireRate
          dmg = tower.damage * getDamageMultiplier(tower.type, target.type)
          target.health -= dmg
          spawn laser {x1=tower cx, y1=tower cy, x2=target.x, y2=target.y, tower colour, life=0.15}
          play laser sfx if global laserSoundCooldown <= 0 (then set to 0.08)
          if target.health <= 0: gold += 10; totalKills += 1; enemiesRemaining -= 1;
                                 enemy_die sfx; remove target from enemies array
11. if waveActive and enemiesSpawned >= enemiesPerWave and #enemies == 0 and enemiesRemaining <= 0:
      waveActive = false; waveComplete = true
      if enemiesThatPassed > 0: waveRetry = true
12. if waveComplete and waveRetry: waveRetry=false; waveComplete=false; startWave()   -- SAME wave (bug, §14 B2)
13. if waveComplete and not waveRetry and gameStarted:
      wave += 1; waveComplete = false; startWave()                                  -- immediate, no intermission
```

### 1.2 Outer game states `[LUA]`

Only **two** exist. There is no pause menu, no game over, no victory, no restart, no return to menu.

| game state | Entered by | Update | Draw |
|---|---|---|---|
| `"MENU"` | initial value (`main.lua:184`) | menu particles only | `drawMainMenu()` + optional `drawControlsOverlay()` |
| `"PLAYING"` | menu confirm (`a`/`return`/`space` at `main.lua:1295`, or touch on START at `main.lua:1465`) | full gameplay | `drawGameArea()` then `drawUIPanel()` + optional controls overlay |

Transitions: `MENU -> PLAYING` only. `PLAYING -> MENU` never happens. `showControls` (Select/Tab) overlays help in **both** states; it does not change the game state.

### 1.3 Inner build states `state` `[LUA]` (`main.lua:188`, `1335-1457`)

Global string, initialised to `"MOVING"` at load. Controls the cursor/UI, not enemy movement.

| build state | Meaning | Entered by | Exits |
|---|---|---|---|
| `MOVING` | cursor roams, nothing armed | initial; successful build; sell; upgrade; cancel; cursor move while selected | confirm on empty cell -> `BUILDING`; confirm on tower cell -> `TOWER_SELECTED` |
| `BUILDING` | a tower type is armed, waiting for placement confirm | `MOVING` + confirm on empty cell | confirm (`return`/`space` — **not `a`**, §14 B1) or touch tap -> place or error, then `MOVING` on success; `b`/`backspace` -> `MOVING` |
| `TOWER_SELECTED` | an existing tower under the cursor is selected | `MOVING` + confirm on a tower cell | `x` upgrade success -> `MOVING`; `y` twice -> sell -> `MOVING`; `b` -> `MOVING`; cursor move -> `MOVING` (clears `sellConfirm`) |

Build-state transition rules in exact terms:

- `MOVING + confirm`, tower present at cursor -> `TOWER_SELECTED`. Tower absent -> `BUILDING` (the previously selected `selectedTowerType` is reused).
- `BUILDING + confirm`: if `gold >= towerTypes[selected].cost`, tentatively place tower, run `findPath()`; if non-nil commit, deduct gold, play build sfx, `state="MOVING"`; if nil roll back and toast `"Path blocked!"`. If not affordable: toast `"Not enough gold!"`. On either error the build state **stays** `BUILDING`.
- While `BUILDING`, `left`/`a` and `right`/`d` cycle `selectedTowerType` (wrapping 1..3). Because `a` is tested first, it never reaches the build action (§14 B1).
- In `TOWER_SELECTED`, `x` upgrades (level < 10 and gold >= `20 * 2^(level-1)`), `y` arm-then-confirm sell, `b` cancels.
- Moving the cursor in `TOWER_SELECTED` drops back to `MOVING` and clears the tower's `sellConfirm` flag.

### 1.4 Pause / speed `[LUA]` (`main.lua:217-219`, `1316-1325`)

There is **no pause state**. Speed is a global multiplier:

`speedLevels = {0, 0.5, 1, 2, 4}`, `speedIndex = 3` (1-based) -> `gameSpeed = 1` at boot.

- `L` (or `[`): `speedIndex = max(1, speedIndex - 1)`, then `gameSpeed = speedLevels[speedIndex]`.
- `R` (or `]`): `speedIndex = min(5, speedIndex + 1)`, then `gameSpeed = speedLevels[speedIndex]`.
- Index 1 gives `gameSpeed = 0`: the whole world freezes (`dt` becomes 0 after the menu check, so cooldowns, lasers, spawn timer, error toast and laser-sound cooldown all freeze), but the mouse/keyboard **still build, upgrade and sell**. The HUD prints `PAUSED` for index 1 (`getSpeedText`, `main.lua:497-500`).
- Display strings: `PAUSED`, `0.5x`, `1x`, `2x`, `4x`.
- `gameSpeed` multiplies the same `dt` used by the error toast and laser-sound cooldown, so at 4x these run 4x faster in real time.

### 1.5 States/effects that do **not** exist in the Lua

| Missing | Evidence | Consequence in original |
|---|---|---|
| lives / HP of the base | no `lives`-like variable anywhere; grep of `lives`, `health` base, `gameover` shows nothing but the sound | leaking enemies is not punished |
| game over | `gameState` only ever becomes `"MENU"` or `"PLAYING"` | the run never ends |
| victory / final wave | no `MAX_WAVE`, no wave cap | waves are endless |
| pause menu | see §1.4 | only a speed-0 hack, two `L` presses away; no overlay, no resume confirmation |
| return to menu / restart | no transition back to `"MENU"` anywhere | LÖVE default `Escape` quit is the only exit |
| sound on/off | `soundEnabled` is never toggled by any input | cannot mute |
| boss enemy | no boss type; elites are a modifier (§5) | — |

`sfx_gameover` is loaded (`main.lua:263`) but **never played**; this is the strongest signal that a game-over screen was intended.

### 1.6 `[PORT]` 3DS state machine

Add the states the Lua is missing, keep everything else.

```
                 tap START
   +---------+  ----------->  +---------------------------+
   | GS_MENU |                | GS_PLAYING                |
   +---------+  <-----------+ |  phase = READY            |
        ^        MENU        |  phase = WAVE             |
        |                    |  phase = INTERWAVE        |
        |  GAME_OVER/MENU    |  paused (bool overlay)    |
   +----------+              +---------------------------+
   |GS_GAME_OVER| <----------- lives <= 0
   +----------+
```

| State | Sub-state / field | Behaviour | Top screen | Bottom screen |
|---|---|---|---|---|
| `GS_MENU` | — | menu particles animate; no gameplay | title art, element icons, particles | big `START`, `CONTROLS` buttons |
| `GS_PLAYING` | `phase = PHASE_READY` | before wave 1: enemies empty, `gameStarted == false`, player builds freely, START/NEXT arms wave 1 | field + HUD status `READY`, tooltip "Tap START to begin" | map + build row + action row; NEXT label `START` |
| | `PHASE_WAVE` | normal update (§1.1 steps 2–11, minus retry) | field + HUD status `ACTIVE` | NEXT disabled (`WAIT`) |
| | `PHASE_INTERWAVE` | `INTERWAVE_SEC` countdown (default 3.0 s) after a wave is fully resolved; next wave starts when it hits 0 or on NEXT tap | HUD status `NEXT 2.4s` | NEXT enabled (`NEXT`) |
| | `paused = true` | gameplay update frozen (equivalent to `gameSpeed = 0`), input overlay active | dim + `PAUSED` + current speed | `RESUME`, `SPEED x`, `QUIT TO MENU` |
| `GS_GAME_OVER` | — | gameplay frozen, `gameover.wav` plays once, game music stops | `GAME OVER`, wave reached, kills | `RETRY`, `MAIN MENU` |
| optional `GS_VICTORY` | — | `[PORT]`, only if `VICTORY_WAVE > 0` is signed off (§14.3) | `VICTORY` | `RETRY`, `MAIN MENU` |

Transitions (all `[PORT]` unless marked):

| From | Trigger | To | Side effects |
|---|---|---|---|
| `GS_MENU` | tap START (bottom `(40,84,240,56)`) | `GS_PLAYING/READY` | reset run (§7.2), stop menu music, start game music |
| `GS_MENU` | tap CONTROLS / Select | help overlay | — |
| `READY` | tap NEXT / A / Start-wave | `WAVE` (wave 1) | `gameStarted = true`, `startWave()` |
| `WAVE` | wave fully resolved with `livesLostThisWave == 0` | `INTERWAVE` | `wave += 1`, `INTERWAVE_SEC` timer (Lua: instant, §14 B4) |
| `WAVE` | wave fully resolved after leaks | `INTERWAVE` | **no wave retry** (fixes §14 B2); leaks already cost lives |
| `INTERWAVE` | timer 0 or NEXT tap | `WAVE` | `startWave()` |
| `PLAYING` | Start pressed | paused toggle | freeze/unfreeze; remembers previous non-zero speed |
| `PLAYING` | `lives <= 0` after a leak | `GS_GAME_OVER` | `play gameover.wav`, stop game music |
| `GS_GAME_OVER` | tap RETRY | `GS_PLAYING/READY` | reset run, restart game music |
| `GS_GAME_OVER` | tap MAIN MENU | `GS_MENU` | stop game music, play menu music |
| paused | QUIT TO MENU | `GS_MENU` | same as above |

`INTERWAVE_SEC = 3.0` is a port addition (Lua has 0 s, §14 B4); set it to `0.0` to reproduce the Lua exactly.

### 1.7 3DS frame loop `[PORT]`

- Fixed step: `dt = 1.0f / 60.0f`, then `dt *= gameSpeed` exactly like the Lua (step 2 of §1.1). Do **not** accumulate a real-clock dt; the Lua's only timing source is `dt`, so a fixed step preserves all numbers.
- Order per frame: read input -> apply game-state transitions -> `game_update(dt_scaled)` (steps 2–11 of §1.1, with the §1.6 replacement for 12–13) -> draw top -> draw bottom.
- Pause (`paused == true` or `gameSpeed == 0`) must freeze **everything** whose timer uses the scaled `dt`, exactly as the Lua does: spawn timer, enemy `t`, cooldowns, laser lifetimes, error timer, laser-sound cooldown.
- 60 fps target; if the frame budget is missed, keep the fixed step (game runs slower rather than becoming unstable). Never increase `dt`.
- Render both screens each frame. Render top to `GFX_LEFT` only; stereo duplication doubles draw cost for no gameplay benefit (§11.6).

---

## 2. Grid & pathfinding

### 2.1 Grid constants `[LUA]` (`main.lua:29-52`, `57-58`)

| Constant | Value | Note |
|---|---|---|
| `grid.cols` | 26 | |
| `grid.rows` | 13 | |
| `grid.cellSize` | 28 px | original 1280x720 space |
| `grid.topScreenHeight` | 500 px | layout constant, not a screen height (§9.2) |
| `startCol, startRow` | 1, 13 | bottom-left, enemies spawn here |
| `endCol, endRow` | 26, 13 | bottom-right, leaking enemies leave here |
| `grid.offsetX` | 276 | `[DERIVED]` `(1280 - 26*28)/2` |
| `grid.offsetY` | 78 | `[DERIVED]` `((500 - 13*28)/2) + 10` |
| Field pixel rect | x 276..1004, y 78..442 | 728 x 364 px |

`gridData[col][row]` is `nil` or a tower table; column-major indexing `[col][row]` (this matters for porting loops).

Cell -> pixel helpers (`main.lua:448-452`):

```
cell origin: (offsetX + (col-1)*cellSize, offsetY + (row-1)*cellSize)
cell centre: (originX + cellSize/2,       originY + cellSize/2)
```

### 2.2 The BFS exactly as implemented `[LUA]` (`main.lua:63-109`)

```
findPath():
  queue   = [ {col=1,row=13} ]          -- FIFO, start cell is seeded unconditionally
  visited[startCol][startRow] = true
  parent  = {}
  while #queue > 0:
      current = table.remove(queue, 1)                 -- O(n) shift
      if current == end cell:
          walk parent[] backwards from current, table.insert(path, 1, node) until node == nil
          return path                                  -- array of {col,row}, start..end inclusive
      neighbors = { up (row-1), down (row+1), left (col-1), right (col+1) }   -- EXACT order
      for each neighbor in that order:
          if 1 <= col <= 26 and 1 <= row <= 13:
              if not visited[col][row]:
                  if gridData[col][row] == nil:        -- tower blocks
                      visited[col][row] = true
                      parent[col][row]  = current
                      push neighbor
  return nil                                           -- unreachable
```

Facts the C port must reproduce bit-for-bit:

- 4-directional movement, uniform cost 1 per cell, no diagonals.
- Neighbour expansion order is **up, down, left, right** (this fixes tie-breaking and therefore the exact path shape when several shortest paths exist).
- Towers block a cell; the **start cell is never checked** for occupancy (it is seeded into the queue regardless — §14 B5); the **end cell is checked** like any other neighbour, so a tower there makes the path unreachable.
- `visited` is checked at enqueue time (not dequeue time), as in the code.
- Path includes both start and end cells: 26..338 cells (338 = 26*13 worst case for a maze).
- BFS cost: 338 nodes, 4 neighbours each; `table.remove(queue,1)` is the only super-linear part.

Initial path with an empty grid: **straight along row 13**, 26 cells, 25 segments. (Every cell `(c,13)` is discovered from `(c-1,13)` because BFS reaches depth `c-1` on the bottom row before any detour at depth `c+`. Start centre = (290,428), end centre = (990,428) in original space.)

### 2.3 When the path is recomputed `[LUA]`

| Trigger | Code | Notes |
|---|---|---|
| Boot | `updatePath()` at `main.lua:123` | after grid init |
| Successful build | `testPath = findPath()` at `main.lua:1362`; if non-nil `updatePath()` (which calls `findPath()` **again**) | two full BFS per successful build; a failed build costs one BFS |
| Sell | `updatePath()` at `main.lua:1452` | unconditional; cannot fail |
| Anything else? | no | path is **not** recomputed per frame; `isPathCell` is called per cell per frame for rendering |

The C port must run exactly one BFS per placement: place tentatively, BFS, commit or roll back. On sell, run one BFS. Path may also need recomputation on run reset (`[PORT]`).

### 2.4 What happens when the path would be blocked `[LUA]` (`main.lua:1350-1376`)

Exact sequence for a build attempt:

1. `if gold < cost` -> `showError("Not enough gold!")`; no placement; build state stays `BUILDING`.
2. Tentatively write the tower into `gridData[cursor.col][cursor.row]`.
3. `testPath = findPath()`.
4. `if testPath == nil`: `gridData[...] = nil` (rollback), `showError("Path blocked!")`, gold **not** deducted, build state stays `BUILDING`. The error toast shows for `errorTimer = 2.0` seconds.
5. If non-nil: `updatePath()` (second BFS), subtract gold, `state = "MOVING"`, play `build.wav`.

Additional behaviour of the error toast: any key press while `errorTimer > 0` sets `errorTimer = 0` and **returns**, so the key is swallowed (the next key does the real action). Touch input does **not** clear the error.

So "path blocked" is a rejected placement, not a state the game can enter — **except** via the start-cell loophole and defensive holes listed in §14 B5. If `currentPath` ever becomes `nil`, `updatePath()` empties `pathPositions`, then `getPathPosition()` returns `{x=0,y=0}` for every query, so enemies would sit at the top-left pixel and never leak, and the wave would never complete (softlock). Guard against it (§2.6).

### 2.5 Path positions and normalized progress `[LUA]` (`main.lua:111-121`, `458-480`)

`updatePath()` builds `pathPositions[i] = {x = cell centre of currentPath[i]}` for `i = 1..#currentPath`.

`getPathPosition(t)` is O(1):

```
if pathPositions is nil or #pathPositions < 2: return (0,0)
totalSegments = #pathPositions - 1
segmentLength = 1 / totalSegments
segmentIndex  = floor(t / segmentLength) + 1
localT        = (t % segmentLength) / segmentLength
if segmentIndex >= #pathPositions: return last position
return lerp(pathPositions[segmentIndex], pathPositions[segmentIndex+1], localT)
```

Consequences (they are all real behaviours, not bugs to "fix" silently):

- Every segment is traversed in the same **fraction of total time**, so long and short segments get the same `dt` budget; since every cell is the same size here, world speed is uniform except for the diagonal-free grid turns.
- An enemy's world position is recomputed from `t` and the **current** path every frame. When the path changes (build/sell), every live enemy instantly slides to its new position on the new path; `t` is preserved. Shortening the path teleports enemies **forward**, lengthening it teleports them **backward**. Keep this behaviour in C (store `t` only; recompute x,y when needed).
- `t == 1` is the leak threshold; the position at exactly `t==1` is the end centre.
- Spawn position is `pathPositions[1]` (start cell centre), `t = 0`.

### 2.6 `[PORT]` C implementation rules

- Fixed arrays, zero allocation in the update loop: `path_cells[MAX_PATH_CELLS]` (338), `BFS queue[338]` as a ring with head/tail indices (no shifting), `visited[26][13]` as bytes, `parent[26][13]` as small struct, `path_positions[338]` floats. Reuse all buffers every BFS.
- Keep the neighbour order U, D, L, R and the enqueue-time visited test — paths will then match the Lua exactly.
- Validate placement by (a) cell in bounds, (b) cell empty, (c) **not the start cell and not the end cell** (fixes §14 B5), (d) BFS non-nil, (e) `gold >= cost`. Only then commit.
- Keep an occupancy fast path: `tower_grid[26][13]` of `int16` tower index or -1.
- Never let `currentPath` be `NULL` during `PHASE_WAVE`; if a BFS ever returns unreachable during a *removal* (impossible) or reset, fall back to the previous known-good path and assert in debug builds.
- Iterate `for col 1..26 for row 1..13` (column-major) when scanning towers to match Lua ordering (affects which tower fires first when gold/sfx timing is inspected, not gameplay balance).

---

## 3. Towers

### 3.1 Tower roster `[LUA]` (`main.lua:128-156`)

All three towers are **stat-identical**; only the element (colour and damage multiplier) differs. `glowColor` is declared but **never read** — do not rely on it (kept in the table for completeness).

| # | Name | Element id (C) | Cost | Damage | `fireRate` (seconds **between** shots) | Shots/s `[DERIVED]` | Range (px) | Range (cells) `[DERIVED]` | Colour `(r,g,b)` floats | 8-bit | `glowColor` (unused) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | Fire | `ELEM_FIRE` | 20 | 3 | 0.8 | 1.250 | 60 | 2.14 | `(0.95, 0.15, 0.15)` | (242, 38, 38) | `(0.95, 0.3, 0.1)` |
| 2 | Water | `ELEM_WATER` | 20 | 3 | 0.8 | 1.250 | 60 | 2.14 | `(0.15, 0.4, 0.95)` | (38, 102, 242) | `(0.1, 0.5, 0.95)` |
| 3 | Forest | `ELEM_FOREST` | 20 | 3 | 0.8 | 1.250 | 60 | 2.14 | `(0.15, 0.9, 0.15)` | (38, 230, 38) | `(0.1, 0.9, 0.3)` |

Semantics of `fireRate` (`main.lua:739-740`): it is a **cooldown in seconds**, not shots per second. On fire: `tower.cooldown = tower.fireRate`. Every frame, while idle, `cooldown = max(0, cooldown - dt)`. A tower built with `cooldown = 0` can fire on its very first frame with a target in range. There is no "aiming"/rotation time.

Range: constant 60 px for **all** tower types and **all** levels; it is read from `towerTypes[tower.type].range` each frame (`main.lua:722`), never from the tower instance, so upgrades cannot change range in the current code.

### 3.2 Targeting and firing `[LUA]` (`main.lua:715-777`)

- Every tower scans **all** enemies each frame, column-major over the grid (338 cells checked per frame regardless of tower count).
- A candidate must satisfy `distance(towerCentre, enemyCentre) <= range` (Euclidean, inclusive).
- Target = the candidate with the **smallest distance**; ties keep the first candidate encountered, i.e. the one earliest in the `enemies` array = **earliest spawned**.
- No priority modes, no "first/last/strongest" targeting, no line-of-sight blocking, no minimum range.
- Damage is applied **instantly** on the frame the tower fires (hitscan). The laser is purely visual.
- The target's health is reduced; if `health <= 0` on that same frame: `gold += 10`, `totalKills += 1`, `enemiesRemaining -= 1`, `enemy_die.wav`, remove from enemy array by identity (order of survivors preserved).
- A killing shot still spawns its laser first.
- Damage is `tower.damage * multiplier`; the multiplier is computed from the tower's element and the target's element (§4.1).
- Multiple towers can hit the same enemy in one frame; the enemy is removed the moment it dies, so later towers in the same frame re-scan the updated list.

### 3.3 Laser visual `[LUA]` (`main.lua:747-755`, `991-999`)

| Field | Value |
|---|---|
| `x1,y1` | tower cell centre at fire time |
| `x2,y2` | target position at fire time (does **not** follow the enemy afterwards) |
| `color` | the tower's colour (same float triple as §3.1) |
| `life`, `maxLife` | 0.15, 0.15 |
| Draw | alpha = `life/maxLife`; line width 1.5 at that alpha; second pass line width 5 at `alpha * 0.1` (glow) |

On the 3DS (scaled): width 1 at `alpha`, width 3 at `alpha*0.1`, colour = tower colour, life 0.15 s, cap 256 concurrent lasers. The laser is drawn **under** the enemies (Lua draw order: lasers, then enemies, §9.9).

### 3.4 Tower icon geometry `[LUA]` (`main.lua:293-390`)

All icons are `love.graphics.polygon("fill", ...)` with the vertices below, in a local coordinate system where `x,y` is the icon centre and `size` is the "radius" parameter. 3DS draws them as triangle fans around the stated centroid (citro2d `C2D_DrawTriangle`); the centroid lies inside each polygon so the fan is visually identical at <= 18 px. No culling is enabled, so winding order is irrelevant.

**Icon size**: `size = 12 + min(level, 10) * 0.4` (12.4 at level 1, 16.0 at level 10). 3DS: `size_px = round(size * 15/28)` -> levels 1–4: **7 px**, 5–9: **8 px**, 10: **9 px**.

**Fire** (w = 0.8*size), outer polygon, vertices in order:

| # | vertex |
|---|---|
| 1 | (0, -1.0) * size |
| 2 | (-0.8, +0.6) * size |
| 3 | (-0.24, +0.2) * size |
| 4 | (0, +0.8) * size |
| 5 | (+0.24, +0.2) * size |
| 6 | (+0.8, +0.6) * size |

fan centroid **(0, +0.2*size)**; 6 triangles. Then an inner flame at 0.15 white alpha, vertices `(0,-0.6) (-0.32,0.3) (-0.08,0.1) (0,0.4) (0.08,0.1) (0.32,0.3)` * size, same centroid.

**Water** (s = size), outer polygon:

| # | vertex |
|---|---|
| 1 | (0, -1.0) * s |
| 2 | (-0.8, +0.2) * s |
| 3 | (-0.5, +0.6) * s |
| 4 | (0, +1.0) * s |
| 5 | (+0.5, +0.6) * s |
| 6 | (+0.8, +0.2) * s |

fan centroid **(0, +0.27*s)**; 6 triangles. Highlight quad at white alpha 0.2: `(-0.2,-0.4) (-0.4,-0.1) (-0.2,-0.1) (-0.1,-0.3)` * s (two triangles).

**Forest / leaf** (s = size), outer polygon:

| # | vertex |
|---|---|
| 1 | (0, -1.0) * s |
| 2 | (-0.7, -0.3) * s |
| 3 | (-0.8, +0.2) * s |
| 4 | (-0.3, +0.6) * s |
| 5 | (0, +0.4) * s |
| 6 | (+0.3, +0.6) * s |
| 7 | (+0.8, +0.2) * s |
| 8 | (+0.7, -0.3) * s |

fan centroid **(0, +0.05*s)**; 8 triangles. Vein: 1 px line (0,-0.5*s) -> (0,+0.3*s) at black alpha 0.15.

**Star** (elite icon and HUD elite icon), s = size, 10 vertices:

`(0,-1) (0.3,-0.3) (1,-0.3) (0.4,0.1) (0.6,0.8) (0,0.4) (-0.6,0.8) (-0.4,0.1) (-1,-0.3) (-0.3,-0.3)` * s, fan centroid (0,0), 10 triangles.

**Level marker** on towers (`drawTower`, `main.lua:380-389`):
- level < 10: the level number as text, `fontTiny` (11 px), centred 10 px wide, baseline `y - size - 6`. 3DS: 6 px text centred at `(cx, cy - size - 4)`, white alpha 0.8.
- level >= 10: two filled rects above the icon instead of text: `(cx-5, cy-size-5, 10, 5)` colour `(1,1,0.3)` then `(cx-4, cy-size-4, 8, 3)` colour `(1,1,0.5,0.6)`. 3DS: `(cx-3, cy-size-3, 5, 3)` (255,255,77) then `(cx-2, cy-size-2, 4, 2)` (255,255,102, alpha 153).

**Tower cell tint** beneath the icon: `tower.color` at alpha 0.06 filling the cell. `[PORT]` may be skipped on 3DS (imperceptible; saves one quad per tower).

---

## 4. Elements / combinations

### 4.1 Damage multiplier matrix `[LUA]` (`main.lua:164-179`)

`getDamageMultiplier(towerType, enemyType)`, towerType is 1/2/3, enemyType is `"FIRE"`/`"WATER"`/`"FOREST"`:

| Tower \ Enemy | FIRE | WATER | FOREST |
|---|---|---|---|
| **Fire** (1) | 1.0 | **0.5** | **2.0** |
| **Water** (2) | **2.0** | 1.0 | **0.5** |
| **Forest** (3) | **0.5** | **2.0** | 1.0 |

Rules: strong = x2.0, weak = x0.5, same element = x1.0. This is the only element interaction in the game.

### 4.2 There is **no** combining mechanic `[LUA]`

- No element fusion, no mixed/elemental-pair towers, no tier names, no recipe UI.
- Tower type is a single integer 1..3; the only per-tower progression is the linear level 1..10 upgrade (§8).
- `upgrade_1.wav` … `upgrade_6.wav` are a **random sound pool**, not six upgrade tiers/paths: `playRandomUpgradeSound()` picks `math.random(1, #sfx_upgrade)` on every successful upgrade (`main.lua:278-282`). There is no mapping from sound number to element or level. C can pick `1 + rand() % 6`; a `[PORT]` alternative (deterministic by level, e.g. `upgrade_(level-1)`) is a free choice but not what the Lua does — note it in the commit if changed.
- `enemyTypes` (FIRE/WATER/FOREST) exists only to give enemies a colour table and a name; elites are a boolean modifier, not an element (§5).

### 4.3 Ambiguity — README says five elements

`Attempt-2-3ds-game/README.md:15-17` describes "fire, water, earth, air and a few combinations … each with its own range, damage and fire rate". **None of that exists in `main.lua`**: 3 elements, identical stats, no combinations, no air/earth. This spec follows the code (the stated single source of truth). If the Lead wants combinations in the remake, it is new design work, not a port; see §14.3 Q1.

---

## 5. Enemies

### 5.1 Base enemy types `[LUA]` (`main.lua:158-162`, `669-689`)

An enemy is generated from a random element; the three types are mechanically identical apart from colour/name. There is no separate "boss" type.

| Enemy element | Internal name | Colour floats | 8-bit | Base HP | Base speed | Size (px) | Bounty on kill | Behaviour |
|---|---|---|---|---|---|---|---|---|
| Fire | `FIRE` | (0.95, 0.15, 0.15) | (242, 38, 38) | `15 + (wave-1)*3` | `20 + (wave-1)*1` | 10 | +10 gold | walk path, leak at t>=1 |
| Water | `WATER` | (0.15, 0.4, 0.95) | (38, 102, 242) | same | same | 10 | +10 gold | same |
| Forest | `FOREST` | (0.15, 0.9, 0.15) | (38, 230, 38) | same | same | 10 | +10 gold | same |

- Type is chosen uniformly at random per spawn: `types[math.random(1,3)]` (`main.lua:573-576`). There are no per-type stat differences.
- `glowColor` in `enemyTypes` is unused (same as towers).
- Enemies have **no** attack, no blocking, no armour, no slow/status effects, no regeneration, no splitting. They do not interact with each other or with towers except through targeted damage.
- Movement speed uses `t` (normalized): `t += dt * speed/100` -> crossing time = `100/speed` seconds **regardless of path length**. `[DERIVED]` world speed on the initial straight path (700 px) = `7 * speed` px/s; on the 3DS path (375 px) = `3.75 * speed` px/s. Time-per-path is the balance-relevant number; see §6.6.
- Spawn position: `pathPositions[1]`, `t = 0`; enemies are appended to the `enemies` array (spawn order preserved; removal shifts the array, so order is still spawn order).
- Leak (`t >= 1`): enemy removed, `enemiesRemaining -= 1`, `enemiesThatPassed += 1`, gold `+2` normal / `+10` elite (yes, leaking pays — §14 B2).
- HP may go negative; nothing clamps it.

### 5.2 Elite modifier `[LUA]` (`main.lua:674-677`, `392-409`)

Elites are **not** a type: any random element can be elite. Only present on waves divisible by 5 (`§6.2`).

| Property | Normal | Elite |
|---|---|---|
| HP | `15 + (wave-1)*3` | **x2.5** of that |
| Speed | `20 + (wave-1)*1` | **x1.8** of that |
| HP/speed coupling | — | both multipliers applied to the base values |
| Icon size | 10 px | 16 px |
| Health bar | width 24, y-offset -18 | same |
| Kill bounty | +10 gold | **+10 gold** (same as normal — the Lua gives no elite bonus for killing) |
| Leak bounty | +2 gold | **+10 gold** |
| Visual extra | — | two 1 px outline rects around the icon, colour white alpha 0.12 at `(x-size-3, y-size-3, size*2+6, size*2+6)` and gold `(1,0.8,0.2)` alpha 0.15 at `(x-size-4, y-size-4, size*2+8, size*2+8)`; `drawPixelStar` is used for the elite icon in the HUD composition list |
| Element multiplier | applies normally | applies normally |

### 5.3 Enemy visuals `[LUA]` (`main.lua:392-417`, `1001-1005`)

- Icon = same element shape as the tower icon (§3.4) at `size = 10` (16 if elite). No rotation, no animation.
- Health bar drawn for **every** enemy at every frame:
  - `x = enemy.x`, `y = enemy.y - 18`, `width = 24` (all px, original space).
  - background: filled rect `(x-12, y-6, 24, 5)` black alpha 0.7.
  - fill: `(1-percent, percent, 0)` where `percent = health/maxHealth`, rect `(x-12+1, y-5, (24-2)*percent, 3)`.
- Elite outline rects listed in §5.2 are drawn after the icon.
- 3DS scale: bar width 13 px, height 3 (bg) / 2 (fill), offsets `(cx-6, cy-10)` and `(cx-5, cy-9)`; elite outline 1 px rect side `2*size+4`.

### 5.4 Enemy cap `[PORT]`

No cap exists in the Lua. Max concurrent enemies = `enemiesPerWave` (spawns are the only source and all must resolve before a wave completes): wave 200 = 413. Use `MAX_ENEMIES 512` and cap the wave counter at `MAX_WAVE 200` (§6.7); hard-clamp spawns if the array is full (decrement `enemiesRemaining` so the wave can still complete) and log it.

---

## 6. Waves

### 6.1 Counts and elite wave formula `[LUA]` (`main.lua:513-571`)

Called by `startWave()` (on run start, on every retry, and on every successful wave advance):

```
baseEnemies = 3 + wave * 2                       -- 5, 7, 9, 11, ...
if wave % 25 == 0:      eliteCount = 10
elif wave % 10 == 0:    eliteCount = 3
elif wave % 5 == 0:     eliteCount = 1
else:                   eliteCount = 0
enemiesPerWave   = baseEnemies + eliteCount
enemiesRemaining = enemiesPerWave
enemySpawnInterval = max(0.8, 3.0 - wave * 0.05)
enemySpawnTimer  = 0
enemiesSpawned   = 0
enemiesThatPassed = 0
waveRetry = false
```

- Precedence is `%25` > `%10` > `%5`; wave 25 has **10** elites (not 10+3+1), wave 10 has 3, wave 5 has 1.
- Enemy stat formulas per spawn: `health = 15 + (wave-1)*3`, `speed = 20 + (wave-1)*1`, then elite multipliers (§5.2).
- **No wave cap and no victory condition.** Wave counter starts at 1 and increments forever.

### 6.2 Composition: what is displayed vs what actually spawns `[LUA]` (`main.lua:558-565`, `573-576`)

The Lua computes a per-element quota for the HUD:

```
regularCount = baseEnemies - eliteCount          -- note: elites subtracted from the base, then added back as spawns
fireCount   = floor(regularCount / 3)
waterCount  = floor(regularCount / 3)
forestCount = regularCount - fireCount - waterCount
waveEnemyCounts = {FIRE, WATER, FOREST, ELITE}
```

…but the **spawner ignores those quotas** and picks `math.random(1,3)` for every non-elite spawn. So the "WAVE ENEMIES" HUD list is a prediction, not the actual composition: expected counts are `regularCount/3` each, with binomial variance. The C port should either (a) keep random spawning and relabel the HUD, or (b) spawn exact quotas (recommended for fairness, `[PORT]`, see §14 B6). Default in this spec: **spawn exactly the computed quotas** for FIRE/WATER/FOREST in that order, interleaved with elites as §6.4 describes — this makes the HUD truthful and removes wave-to-wave luck. If exact Lua behaviour is required, flip a single `#define EXACT_QUOTAS 0/1`.

### 6.3 Spawn interval and wave timing `[LUA]` / `[DERIVED]`

- `enemySpawnInterval = max(0.8, 3.0 - 0.05*wave)`; wave 1 = 2.95 s, wave 44 = 0.8 s, clamped 0.8 s thereafter.
- The **first** enemy appears one full interval after `startWave()` (spawn timer starts at 0 and increments before the check), i.e. 2.95 s after wave 1 begins.
- Exactly `enemiesPerWave` spawn slots per wave; last spawn at `(N-1) * interval`.
- Minimum wave duration (nothing killed) = `(N-1)*interval + 100/speed` (the last spawn must cross the path). `[DERIVED]` wave 1 ≈ 11.80 + 5.00 = 16.8 s; wave 25 ≈ 108.5 + 2.27 = 110.8 s; wave 44+ ≈ 0.8*(N-1) + 1.27 s.
- No wave-completion bonus, no countdown UI in the Lua beyond the `LEFT` counter and `ACTIVE/COMPLETE` status.

### 6.4 Elite spawn algorithm `[LUA]` (`main.lua:656-667`)

`startWave()` preloads `waveEvents` as an array of `{type="ELITE", delay=d}`:

| Wave class | events | delay values |
|---|---|---|
| `wave % 25 == 0` | 10 | `i * 0.3` for i = 1..10 -> 0.3, 0.6, … 3.0 |
| `wave % 10 == 0` | 3 | `i * 0.2` -> 0.2, 0.4, 0.6 |
| `wave % 5 == 0` | 1 | `0.5` |

On every spawn slot (before creating the enemy), the Lua runs this loop (indices scanned **from the end down to 1**):

```
isElite = false
for i = #waveEvents, 1, -1:
    e = waveEvents[i]
    if e.delay <= 0:
        isElite = true
        remove waveEvents[i]
        break                      -- at most one elite per spawn slot
    e.delay = e.delay - enemySpawnInterval
```

Net effect (`[DERIVED]`, verified by hand for all three wave classes): **slot 1 is always a regular enemy; all `eliteCount` elites spawn consecutively in spawn slots 2 .. 1+eliteCount; the remaining slots are regulars.** The stored delays only reshuffle which event fires first; they never spread elites through the wave. For the C port, implement it as that net effect (spawn slot index 2..1+E are elite) with a comment, or copy the loop verbatim if you prefer bug-compatibility. Do not implement "delays" thinking they space elites out — they do not.

Example wave 25: 63 spawn slots, slots 2–11 are the 10 elites, slots 1 and 12–63 are regulars.

### 6.5 Wave completion & counter advance `[LUA]` (`main.lua:779-798`)

```
complete iff  waveActive
          and enemiesSpawned >= enemiesPerWave
          and #enemies == 0
          and enemiesRemaining <= 0
```

On completion:

1. `waveActive = false`, `waveComplete = true`.
2. If `enemiesThatPassed > 0` -> `waveRetry = true`.
3. If retrying: `waveRetry=false; waveComplete=false; startWave()` — **the same wave restarts forever** while any enemy leaks. No lives are lost, and the player is paid +2/+10 gold per leak (§14 B2).
4. Else: `wave += 1; waveComplete = false; startWave()` — the next wave begins **in the same frame**, with no intermission. Wave 1 -> 2 never shows a `COMPLETE` status frame.

`[PORT]` replacement (implemented in §1.6): leaks deduct lives, the wave advances regardless, and there is a `INTERWAVE_SEC` (3.0 s default) gap before the next wave. `waveComplete` status text is never visible in the Lua because of step 4; the 3DS shows the intermission countdown instead.

### 6.6 Wave data table `[DERIVED]` from §6.1/§6.3

`I` = spawn interval, `HP` = base HP, `Spd` = base speed, `cross` = `100/Spd` seconds, `span` = `(total-1)*I`, `gold` = total spawns x 10 (max kill income).

| Wave | base | elites | total spawns | F/W/Fo quotas | I (s) | HP | Spd | cross (s) | span (s) | gold |
|---:|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | 5 | 0 | 5 | 1/1/3 | 2.95 | 15 | 20 | 5.00 | 11.80 | 50 |
| 2 | 7 | 0 | 7 | 2/2/3 | 2.90 | 18 | 21 | 4.76 | 17.40 | 70 |
| 3 | 9 | 0 | 9 | 3/3/3 | 2.85 | 21 | 22 | 4.55 | 22.80 | 90 |
| 4 | 11 | 0 | 11 | 3/3/5 | 2.80 | 24 | 23 | 4.35 | 28.00 | 110 |
| 5 | 13 | 1 | 14 | 4/4/4 | 2.75 | 27 | 24 | 4.17 | 35.75 | 140 |
| 6 | 15 | 0 | 15 | 5/5/5 | 2.70 | 30 | 25 | 4.00 | 37.80 | 150 |
| 7 | 17 | 0 | 17 | 5/5/7 | 2.65 | 33 | 26 | 3.85 | 42.40 | 170 |
| 8 | 19 | 0 | 19 | 6/6/7 | 2.60 | 36 | 27 | 3.70 | 46.80 | 190 |
| 9 | 21 | 0 | 21 | 7/7/7 | 2.55 | 39 | 28 | 3.57 | 51.00 | 210 |
| 10 | 23 | 3 | 26 | 6/6/8 | 2.50 | 42 | 29 | 3.45 | 62.50 | 260 |
| 11 | 25 | 0 | 25 | 8/8/9 | 2.45 | 45 | 30 | 3.33 | 58.80 | 250 |
| 12 | 27 | 0 | 27 | 9/9/9 | 2.40 | 48 | 31 | 3.23 | 62.40 | 270 |
| 15 | 33 | 1 | 34 | 10/10/12 | 2.25 | 57 | 34 | 2.94 | 74.25 | 340 |
| 20 | 43 | 3 | 46 | 13/13/14 | 2.00 | 72 | 39 | 2.56 | 90.00 | 460 |
| 24 | 51 | 0 | 51 | 17/17/17 | 1.80 | 84 | 43 | 2.33 | 90.00 | 510 |
| 25 | 53 | 10 | 63 | 14/14/15 | 1.75 | 87 | 44 | 2.27 | 108.50 | 630 |
| 26 | 55 | 0 | 55 | 18/18/19 | 1.70 | 90 | 45 | 2.22 | 91.80 | 550 |
| 30 | 63 | 3 | 66 | 20/20/20 | 1.50 | 102 | 49 | 2.04 | 97.50 | 660 |
| 40 | 83 | 3 | 86 | 26/26/28 | 1.00 | 132 | 59 | 1.69 | 85.00 | 860 |
| 44 | 91 | 0 | 91 | 30/30/31 | 0.80 | 144 | 63 | 1.59 | 72.00 | 910 |
| 45 | 93 | 1 | 94 | 30/30/32 | 0.80 | 147 | 64 | 1.56 | 74.40 | 940 |
| 50 | 103 | 10 | 113 | 31/31/31 | 0.80 | 162 | 69 | 1.45 | 89.60 | 1130 |
| 100 | 203 | 10 | 213 | 64/64/65 | 0.80 | 312 | 119 | 0.84 | 169.60 | 2130 |
| 200 | 403 | 10 | 413 | 131/131/131 | 0.80 | 612 | 219 | 0.46 | 329.60 | 4130 |

Elite HP/speed examples: wave 25 elite = 217.5 HP, 79.2 speed (cross 1.26 s); wave 50 elite = 405 HP, 124.2 speed; wave 100 elite = 780 HP, 214.2 speed.

`[PORT]` engine cap: `MAX_WAVE = 200` (fits `MAX_ENEMIES 512`); at the cap clamp `wave` and keep re-running wave 200 until the player dies or the optional victory condition fires.

### 6.7 Inter-wave / spawn timing summary `[LUA]` + `[PORT]`

| Event | Lua | 3DS port |
|---|---|---|
| wave 1 start | immediately when menu START is pressed | when player taps START on the READY screen (better matches the Lua's unreachable "BUILD YOUR DEFENSE" intent, §14 B3) |
| first spawn of a wave | one `enemySpawnInterval` after wave start | same |
| wave completion detection | same frame as last enemy dies/leaks | same |
| gap between waves | **0 s** (same frame) | `INTERWAVE_SEC = 3.0` countdown; NEXT button skips it; set 0 to match Lua |
| wave counter | `+1` on completion | `+1` on completion |
| retry on leak | same wave restarts forever | removed; leaks cost lives (§7.3) |

---

## 7. Economy & lives

### 7.1 Original economy `[LUA]`

| Item | Value | Code |
|---|---|---|
| Starting gold | **150** | `main.lua:186` |
| Tower cost | 20 (all three types) | `main.lua:131/140/149` |
| Kill reward | **+10 gold**, any enemy, any wave (including elites) | `main.lua:763` |
| Leak reward | **+2 gold** normal, **+10 gold** elite (leaking *pays*) | `main.lua:703-707` |
| Wave completion bonus | none | — |
| Wave / boss bonus | none | — |
| Sell refund | `floor(totalInvested * 0.75)` | `main.lua:1448-1450` |
| `totalInvested(level)` | `20 * 2^(level-1)` (equals `getTotalCost`, which hardcodes a base cost of 20) | `main.lua:502-508` |
| Upgrade cost | from level L to L+1: `20 * 2^(L-1)` | `main.lua:1430` |
| Gold can go negative? | no (all spends are affordability-checked) | — |
| Interest / repair / partial refunds | none | — |
| Starting lives | **none — the concept does not exist in the Lua** | — |
| Lose condition | **none** | — |
| Win condition | **none** | — |

Economy path: `gold(150) -> 7 towers (140) -> kills pay 10`. Max gross income for a fully cleared wave = 10 x total spawns (§6.6). There is no other income source.

### 7.2 `[PORT]` 3DS economy & lives

Keep every original number. Add only the minimum needed for a playable loop:

| New field | Value | Rationale |
|---|---|---|
| `lives` | **20** at run start | HUD needs a fail meter; `gameover.wav` exists unused |
| normal leak | **-1 life** | replaces the +2-gold leak bug |
| elite leak | **-3 lives** | elites are 2.5x HP / 1.8x speed and worth nothing extra on kill |
| leak gold | **0** (remove the Lua's +2/+10 leak reward) | leaking must be a penalty |
| `lives <= 0` | -> `GS_GAME_OVER` | once per run |
| run reset (RETRY / new run) | `gold = 150`, `lives = 20`, `wave = 1`, `gameStarted = false`, `build_state = MOVING`, `selectedTowerType = 1`, clear all towers, clear enemies/lasers, clear `sellConfirm`, recompute path | fresh state = initial globals of §0/§7.1 |
| victory | none by default (endless). Optional `VICTORY_WAVE` (0 = off; e.g. 50) | §14.3 Q2 |

### 7.3 Lose / win conditions

- **Original:** neither. The run literally cannot end. The only "failure" is the infinite same-wave retry when enemies leak.
- **Port (recommended, `[PORT]`):** lose when `lives <= 0` (checked after each leak resolution) -> `GS_GAME_OVER`; no win condition by default; optional victory when a signed-off `VICTORY_WAVE` has been cleared. Game-over screen shows wave reached and `totalKills` (the Lua increments `totalKills` but never displays it).

---

## 8. Upgrades

### 8.1 Per-level table `[LUA]` formulas + `[DERIVED]` values

Upgrade is linear: level 1 -> 10, no branching, no alternate paths, no range increase, no element change. On each successful upgrade (`main.lua:1425-1442`):

```
cost      = 20 * 2^(level-1)                       -- level BEFORE upgrade, 1..9
tower.damage  = tower.damage * 2                   -- doubles
tower.fireRate = floor(tower.fireRate * 0.9 * 100) / 100   -- 2-decimal floor, cooldown shrinks
tower.level  += 1
state = "MOVING"; playRandomUpgradeSound(); gold -= cost
```

Max level is 10; pressing X at level 10 -> `showError("Max level reached!")` (checked before cost).

| Level | Damage `[DERIVED]` | `fireRate` cooldown (s) `[DERIVED]` | Shots/s | DPS vs neutral | Upgrade cost (L -> L+1) | Total invested at L | Sell = floor(0.75 x total) |
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

Notes:

- `fireRate` is *seconds between shots*; the `*0.9` reduces the cooldown. The `math.floor(...*100)/100` truncation is load-bearing: level 10 = **0.28 s**, not `0.8*0.9^9 = 0.3099`. Use the table above (or replicate the floor step).
- Damage uses exact powers of two: `damage = 3 * 2^(level-1)`; total invested `= 20 * 2^(level-1)`; sell `= 15 * 2^(level-1)` (all integers, no rounding issues).
- Element multiplier stacks on top: effective DPS vs weak element = 2x, vs resistant = 0.5x (e.g. level 3 Fire vs Forest = 37.5 DPS; vs Water = 9.375 DPS).
- Upgrading does **not** reset `cooldown`; a tower mid-cooldown keeps its remaining timer and then fires at the new rate.
- All tower types have identical upgrade curves; cost is element-independent.
- `getTotalCost(tower)` in the Lua hardcodes a base of 20 instead of reading the tower type's cost — correct today because all types cost 20. In C, derive from `tower.cost_paid` or `20 * 2^(level-1)`.

### 8.2 UI for upgrading `[LUA]` (`main.lua:1226-1248`)

- Select a tower (`MOVING` + confirm on its cell) -> `TOWER_SELECTED`; the info panel shows: element name, `LV.<level>`, `DMG: <damage>`, green `X: <cost>G` (or literally `X: MAXG` at level 10 — string-concatenation bug, §14 B7), red `Y: <sellPrice>G`, and the prompt line `X: Upgrade  |  Y: Sell  |  B: Cancel` (or `Press Y again to confirm sell` once armed).
- Range circle (radius 60 px, fill alpha 0.06, outline alpha 0.15 in the tower colour) is drawn around the selected tower.
- `X` upgrade: if `gold < cost` -> toast `"Not enough gold!"`; success -> `state = "MOVING"` (deselects), plays a random upgrade sound, no particle effect.
- `Y` sell: first press sets `tower.sellConfirm = true`; the prompt changes; second press sells for `floor(total*0.75)`, adds gold, clears the cell, recomputes the path, plays `sell.wav`, `state = "MOVING"`. `sellConfirm` is cleared by moving the cursor or cancelling. No timeout.

### 8.3 upgrade_1..6 sounds `[LUA]`

Not tied to any level, element, or tier. `sfx_upgrade` is built by loading `upgrade_1.wav` … `upgrade_6.wav` in order (missing files skipped); each successful upgrade plays a uniformly random one. There is no "upgrade path" concept beyond the single level axis.

---

## 9. UI / HUD (original 1280x720 layout)

This section documents **exactly what the Lua draws**, with coordinates, so the 3DS layout in §11 can be checked against it. All coordinates are original px; fonts are the five LÖVE default-font sizes created in `love.load` (`main.lua:1506-1510`): `fontTitle 36`, `fontLarge 26`, `fontMedium 18`, `fontSmall 14`, `fontTiny 11`. The original has **no lives widget** and **no tooltip**; the bottom info panel is the de-facto tooltip.

### 9.1 Screen regions

| Region | Rect | Contents |
|---|---|---|
| Playfield band | y 0..500, full width | gradient bg + grid (offset 276,78, 728x364) |
| HUD panel | y 505..720 (215 px tall, full width) | wave/left, wave-enemy composition, gold, speed, build buttons, tower info, error toast |
| menu, controls overlay | full screen | see §9.4/§9.5 |

The grid's `offsetY=78` and `topScreenHeight=500` are unrelated to the real screen height; they just centre the field in the top 500 px.

### 9.2 Game HUD element table `[LUA]` (`main.lua:1045-1267`)

Panel background: `rect(0, 505, 1280, 215)` fill `(0.03,0.03,0.07,0.95)`; 1 px separator line `(0,505,1280,1)` fill `(0.1,0.1,0.2,0.2)`.

| Element | Position / size | Font | Colour | Content / notes |
|---|---|---|---|---|
| "WAVE" label | `printf(30, 513, w=100, center)` | small 14 | `(0.5,0.5,0.7,0.6)` | static label |
| wave number | `printf(30, 538, w=100, center)` | title 36 | white | `tostring(wave)` (LÖVE coerces the number) |
| wave status | `printf(30, 588, w=100, center)` | small 14 | READY `(1,1,0.4)` / ACTIVE `(0.2,0.9,0.2)` / COMPLETE `(1,1,0.3)` / WAITING `(0.5,0.5,0.5)` | READY iff `not gameStarted`; ACTIVE iff `waveActive`; COMPLETE iff `waveComplete`; else WAITING (unreachable — §14 B4) |
| "LEFT" label | `printf(30, 608, w=100, center)` | small 14 | `(0.5,0.5,0.7,0.5)` | |
| enemies remaining | `printf(30, 628, w=100, center)` | large 26 | white alpha 0.9 | `enemiesRemaining` = total spawned-or-not minus resolved |
| "WAVE ENEMIES" header | `printf(1250, 513, w=200, right)` | small 14 | `(0.5,0.5,0.7,0.6)` | **right edge is 1450, off-screen** — practically invisible (§14 B8) |
| composition row | starts `y=538`, row pitch 24 px; icon at `(1080, y+8)` size 8; name `printf(1105, y, w=100, left)` in item colour alpha 0.8; count `printf(1205, y, w=40, right)` `"x"..count` white alpha 0.7 | small 14 | fire/water/forest/elite colours, elite = `(1,0.8,0.2)` | rows only for `count > 0`, in order Fire, Water, Forest, Elite; icons fire/water/leaf/star; if nothing shows, a `"---"` placeholder at `(1250, y+8, w=80, center)` (also off-screen) |
| gold coin | circle centre `(30, 677)` r=14 gold `(1,0.8,0.2)` + highlight circle `(29,676)` r=5.6 alpha 0.6 `(1,1,0.5)` + rim circle r=14 `(0.8,0.6,0.1)` | — | — | |
| gold value | `printf(55, 669, w=80, left)` | large 26 | `(1,0.9,0.3)` | `gold` |
| "SPEED" label | `printf(150, 669, w=60, left)` | small 14 | `(0.5,0.5,0.7,0.5)` | |
| speed value | `printf(150, 687, w=60, left)` | medium 18 | white alpha 0.8 | `PAUSED` / `0.5x` / `1x` / `2x` / `4x` |
| "BUILD:" label | `printf(250, 669, w=50, left)` | tiny 11 | `(0.5,0.5,0.7,0.4)` | |
| build buttons | 3x `rect(310 + (i-1)*100, 667, 88, 38, r=4)` | — | base fill `(0.08,0.08,0.14,0.6)`; when selected: inner fill `(x+2,y+2,84,34)` tower colour alpha 0.2 + 1 px outline alpha 0.4 | icon at `(x+18, 685)` size 12; cost `printf(x+38, 679, w=40, left)` tiny 11 white alpha 0.6 |
| info panel | `rect(490, 511, 560, 60)` fill `(0.04,0.04,0.08,0.6)`, 1 px outline `(0.1,0.1,0.2,0.15)` | — | — | content depends on build state (§9.3) |
| error toast | `rect(400, 530, 480, 35)` fill black alpha 0.7; text `printf(410, 535, w=460, center)` medium 18 `(1,0.2,0.2)` | — | — | shown while `errorTimer > 0`, message `Path blocked!` / `Not enough gold!` / `Max level reached!` |

### 9.3 Info-panel text by build state `[LUA]` (`main.lua:1207-1257`)

| Build state | Line 1 (y+4) | Line 2 (y+35, except empty-cell case y+20) |
|---|---|---|
| `BUILDING` | element name at `x+15` in tower colour alpha 0.9; `DAMAGE: <d>` at `x+80`; `COST: <c>G` gold at `x+190` | if cursor on a path cell: `PATH CELL - Building redirects enemies`; else `Press A to build  |  B to cancel` (double spaces around the pipe in the Lua) |
| `TOWER_SELECTED` | element name `x+15`; `LV.<level>` at `x+80`; `DMG: <damage>` at `x+145`; `X: <cost>G` green `(0.3,0.9,0.3,0.6)` at `x+255` (cost = `"MAX"` at level 10 -> renders `X: MAXG`); `Y: <sell>G` red `(0.9,0.3,0.3,0.6)` at `x+365` | `Press Y again to confirm sell` if `sellConfirm`; else `X: Upgrade  |  Y: Sell  |  B: Cancel` |
| empty cell | — | if path cell: `PATH CELL - Build here to redirect enemies`; else `Press A to build a tower here` (both at y+20, label colour `(0.4,0.4,0.6,0.6)`) |

Note the Lua says "Press A…" but `A` cannot actually do it (§14 B1). Keep the text semantics in the port but use the real 3DS buttons (§11 texts).

### 9.4 Main menu `[LUA]` (`main.lua:804-878`)

| Element | Position | Detail |
|---|---|---|
| gradient background | full screen | 721 horizontal 1 px strips: `i=0..720`, `t=i/720`, colour `(0.02+0.03t, 0.02+0.03t, 0.05+0.07t)` |
| menu particles | full screen | 4 spawned per frame (unbounded, §14 B9); random pos, `vx,vy` uniform +-30 px/s, life `1+random*3`, `maxLife` independently random (so alpha can exceed 1/0 — cosmetic), size 2..5, colour each channel `0.2..0.8`; bounces off screen edges; drawn as glow (4 circles) + filled square |
| panel | `rect(290, 80, 700, 560, r=4)` | fill `(0.04,0.04,0.10,0.95)`, 1 px outline `(0.15,0.15,0.30,0.4)` |
| title glow | `(640,200)` r=100 | gold `(1,0.7,0.2)` alpha 0.15 |
| "ELEMENTAL" | `printf(440, 160, w=400, center)` fontTitle 36 | `(1,0.9,0.4)` |
| "DEFENSE" | `printf(478, 213, w=280, center)` fontLarge 26 white; shadow copy at `(480,215)` colour `(0.3,0.3,0.5,0.5)` | |
| "Tower Defense" | `printf(500, 265, w=280, center)` fontMedium 18 | `(0.5,0.5,0.7,0.6)` |
| divider | `rect(440, 295, 400, 1)` | `(0.3,0.2,0.1,0.2)` |
| element icons | x = 420, 640, 860; y = 320; size 18 | fire/water/leaf + glow r=25 alpha 0.12; labels Fire/Water/Forest at `(x-25, 348, w=50, center)` fontSmall 14 `(0.4,0.4,0.6,0.5)` |
| START button | `rect(570, 400, 140, 55, r=28)` | shadow `(574,404,140,55)` black alpha 0.3; fill `(0.85,0.15,0.15)`; highlight `(573,403,134,27.5,r=25)` `(1,0.3,0.3,0.3)`; label `"START"` fontLarge 26 at `printf(570, 414, w=140, center)` white |
| hint | `printf(520, 590, w=240, center)` fontTiny 11 | `(0.3,0.3,0.5,0.4)`: "Press SELECT for controls" |

Menu touch hit box for START: `x in [570,710], y in [400,455]` (`main.lua:1465`).

### 9.5 Controls overlay `[LUA]` (`main.lua:883-918`)

Full-screen black alpha 0.90; panel `rect(290,70,700,580,r=4)` same colours as menu panel; `"CONTROLS"` title at `printf(540,100,w=200,center)` fontTitle 36 white; seven lines fontLarge 26 `(0.8,0.8,0.9,0.9)` at `printf(380, 180 + (i-1)*50, w=500, left)`:

```
D-Pad / Touch : Move cursor
A / Tap       : Select / Build
B             : Cancel
L / R         : Speed control
X             : Upgrade tower
Y (twice)     : Sell tower
SELECT        : Toggle menu
```

footer `printf(540,580,w=200,center)` fontSmall 14 `(0.5,0.5,0.7,0.4)`: "Press SELECT again to close". Toggled by `select`/`tab` in any game state; while shown, all other keys are swallowed. Touch input still works through the overlay in the Lua (§14 B10).

### 9.6 Playfield rendering `[LUA]` (`main.lua:923-1040`)

Draw order: gradient bg -> grid backdrop -> path cells -> grid cells -> tower range (selected only) -> towers -> lasers -> enemies + health bars -> cursor -> "BUILD YOUR DEFENSE" hint (only while `not gameStarted`, effectively unreachable).

| Element | Detail |
|---|---|
| gradient bg | as menu (§9.4) |
| grid backdrop | `rect(274, 76, 732, 368)` fill `(0.03,0.03,0.06)`; 1 px outline `(0.1,0.1,0.2,0.3)` |
| path cells | per cell `i` on path: fill colour `(0.2+0.08t, 0.12+0.04t, 0.04+0.02t)` where `t = i / #currentPath`; 1 px outline `(0.25,0.18,0.08,0.15)` |
| normal cells | non-path only: empty `(0.05,0.05,0.08)` fill; tower cell = tower colour alpha 0.06 fill; every cell gets 1 px outline `(0.08,0.08,0.12,0.12)` |
| selected-tower range | fill circle radius 60 in tower colour alpha 0.06 + 1 px circle outline alpha 0.15 |
| towers | icon + level marker (§3.4) |
| lasers | §3.3 |
| enemies | §5.3 |
| cursor | cell fill white alpha 0.08; 4 corner squares 4x4 white alpha 0.6 at the cell corners; 1 px cell outline white alpha 0.3; if `BUILDING`, additional yellow `(1,1,0.3)` fill alpha 0.15 + outline alpha 0.3 |
| pre-game text | `printf(490,100,w=300,center)` large 26 `(1,1,0.6,0.8)` "BUILD YOUR DEFENSE"; `printf(520,140,w=240,center)` + `printf(480,170,w=320,center)` medium 18 alpha 0.4 "Press A to start Wave 1" / "Build on path cells to redirect enemies" — only when `not gameStarted`, which never happens in practice |

---

## 10. Input mapping

### 10.1 Original controls `[LUA]` (`main.lua:1288-1458`, `1463-1488`)

Key names are LÖVE `key` strings; note the aliases.

| Context | Key | Exact behaviour |
|---|---|---|
| MENU | `select` / `tab` | toggle controls overlay, return |
| MENU | `a` / `return` / `space` | `gameState="PLAYING"`, stop menu music, play game music, `gameStarted=true`, `startWave()` |
| any | `select` / `tab` | toggle controls overlay, return; if overlay is open, every other key is ignored (returns before gameplay input) |
| any | any key while `errorTimer > 0` | clear the toast, swallow the key |
| PLAYING | `l` / `[` | speed index -1 (`max 1`) |
| PLAYING | `r` / `]` | speed index +1 (`min 5`) |
| pre-start (dead branch) | `a`/`return`/`space` | would set `gameStarted=true; startWave()` — unreachable because starting from the menu already sets `gameStarted=true` |
| BUILDING | `left` / `a` | previous tower type (wrap 3->1); **catches `a` first** |
| BUILDING | `right` / `d` | next tower type (wrap 1->3) |
| BUILDING | `b` / `backspace` | cancel -> `MOVING` |
| BUILDING | `a` / `return` / `space` | build at cursor. `a` is unreachable here (see above); only `return`/`space` work |
| MOVING / TOWER_SELECTED | `up` / `w`, `down` / `s`, `left` / `a`, `right` / `d` | move cursor one cell, clamped to the grid; any move from `TOWER_SELECTED` returns to `MOVING` and clears `sellConfirm`; `moved` returns before the confirm logic |
| MOVING | `a` / `return` / `space` | `a` is caught by the movement alias (cursor left); `return`/`space` enter `BUILDING` (empty cell) or `TOWER_SELECTED` (tower) |
| TOWER_SELECTED | `b` / `backspace` | cancel -> `MOVING` |
| TOWER_SELECTED | `x` | upgrade (see §8); level 10 -> toast; insufficient gold -> toast |
| TOWER_SELECTED | `y` | first press arms `sellConfirm`; second press sells |
| PLAYING | any other key | ignored |
| MENU touch | tap in (570..710, 400..455) | same as `a` menu confirm |
| PLAYING touch | tap anywhere in the grid | sets `cursor` to the tapped cell, then calls `love.keypressed("a")` — which in `MOVING` moves the cursor **one cell left**; in `BUILDING` builds at the tapped cell; in `TOWER_SELECTED` does nothing. Touch on UI buttons is not handled at all |

Escape quits the app (LÖVE default) with no confirmation.

### 10.2 `[PORT]` 3DS input design (touchscreen primary, buttons secondary)

Touch is only available on the **bottom** screen, so the bottom screen carries an exact 26x13 touch map of the field (§11.3). The top screen is display-only. All actions have a button equivalent so the game is fully playable without touch.

| Action | Touch (primary) | Buttons (secondary) | Lua analogue |
|---|---|---|---|
| move cursor | tap a map cell (when no tower type armed) | D-pad up/down/left/right (repeat: 0.25 s delay, 0.08 s rate) | arrow keys |
| arm build / select tower type | tap one of the 3 build buttons (tap again to disarm) | while `BUILDING`: D-pad left/right cycles type | A/D in BUILDING (fixed, §14 B1) |
| confirm / place / select | tap the map cell with a type armed -> place; tap a tower cell -> select it | A | `return`/`space` |
| cancel / deselect | tap empty cell / tap armed button again | B | `b`/`backspace` |
| upgrade selected tower | tap `UPGRADE` | X | `x` |
| sell selected tower | tap `SELL` twice (label becomes `SURE?`) | Y twice | `y` twice |
| start wave 1 / next wave | tap `START` / `NEXT` | A (when cursor on nothing? see below) or Start?? — use NEXT only; A confirms | `a`/`return` in menu; `startWave()` |
| speed down / up | tap `SPEED` (cycles 0.5 -> 1 -> 2 -> 4 -> 0.5) | L / R (L = down, R = up, includes 0x = pause) | `l`/`r` |
| pause / resume | tap `PAUSE` | Start | none in Lua (new) |
| controls/help | bottom menu `CONTROLS`, or Select | Select | `select`/`tab` |
| back out of help | tap anywhere on top of overlay? use bottom `CLOSE` | B or Select | `select`/`tab` |

Decisions this mapping resolves:

- **A is a real confirm button** (does not move the cursor; the D-pad does). The Lua's `a`-alias collision (§14 B1) is not reproduced.
- **L/R = speed** (faithful to the Lua help text), **not** tower-type switching as the remake README currently claims. Tower-type switching is touch (build buttons) or D-pad left/right while arming. This is the one direct conflict between README and source; §14.3 Q3 asks for sign-off. If L/R must switch tower type per README, use Select for speed instead and drop the help-overlay binding (one-line change).
- In `BUILDING`, D-pad up/down does nothing (matches Lua, where the camera/cursor cannot move while arming) and left/right cycles the element; A commits at the current cursor cell.
- Start = pause (README/port), while `0x` speed is still reachable with L three times from 1x; pause is the user-friendly way.

### 10.3 Touch interaction state machine `[PORT]`

```
MAP TAP -> cell (col,row) = map hit test (§11.3)
  if paused or help open: ignore map taps
  if a tower exists at cell:
       select it (build_state = TOWER_SELECTED); disarm build; sell_confirm = false; cursor = cell
       if it was already the selected tower: deselect (build_state = MOVING)
  else if build armed (armed_type != 0):
       attempt place armed_type at cell (same validation as §2.6):
           success -> gold -= 20; play build.wav; build_state = MOVING; armed_type = 0; BFS committed
           fail    -> toast Path blocked! / Not enough gold!; armed_type stays set (retry allowed)
  else:
       cursor = cell (no state change)
```

Button semantics keep the Lua's build-state machine (§1.3) with the A-confirm fix.

Every touch/button action that changes the selected tower or cursor must clear `sell_confirm` (matching the Lua's cursor-move reset). Sell confirmation has no timeout (matching Lua); any other action cancels it.

### 10.4 Edge cases to implement

- Toast (`error_timer > 0`): do **not** swallow input on 3DS (Lua swallows one key, §14 B10); just render the toast for 2 s of *game* time (so it freezes at 0x speed, matching Lua).
- A tap on a map cell outside the grid area: ignore. Clamp to avoid off-by-one at cell borders: `col = clamp((x - 4) / 12 + 1, 1, 26)`, `row = clamp((y - 4) / 12 + 1, 1, 13)` using integer floor; treat the 1 px grid lines as belonging to the cell on their right/bottom.
- Buttons must have hit boxes >= 28 px tall for the 3DS resistive touchscreen; the §11.3 layout already respects this.
- Build buttons are disabled-looking but still tappable when gold < 20? Lua allows arming any type; placement then fails with "Not enough gold!". Match that.
- Upgrade/Sell taps with no tower selected -> toast `Select a tower first` (new, because there is no cursor-less Lua equivalent).

---

## 11. 3DS layout proposal

### 11.1 Scaling rationale

Original cell 28 px on a 1280x720 canvas. Top screen is 400x240, so the largest square-cell field that leaves a HUD strip is `15 px` -> **390 x 195 px** field (26x13 cells). Scaling factor `S = 15/28 = 0.535714`.

- 15 px cells are 2 cells = 30 px for the 60 px range diameter; a range circle radius 32 px is visually clear.
- Tower icons 7–9 px, enemies 5/9 px, health bars 13x3 px — all drawable with citro2d primitives.
- The remaining 45 px vertically is split into a 24 px HUD bar (top) and a 20 px tooltip strip (bottom) + 1 px gap.
- The grid is **not** rotated or offset; start is still bottom-left, end bottom-right, path row 13 is the bottom row of the field.

Field constants: `FIELD_X = 5`, `FIELD_Y = 24`, `CELL = 15`, `FIELD_W = 390`, `FIELD_H = 195`. Cell origin `(5 + (col-1)*15, 24 + (row-1)*15)`; cell centre `(origin + 7.5, origin + 7.5)` (use floats). Initial straight path runs from `(12.5, 211.5)` to `(387.5, 211.5)`, length 375 px.

### 11.2 Top screen (400x240) — battlefield + HUD

| # | Element | Rect / position | Colour | Notes |
|---|---|---|---|---|
| T1 | background | full screen | 8 horizontal bands interpolating `(5,5,13)` -> `(13,13,31)` (same formula as §9.4, banded instead of 721 lines) | draw first |
| T2 | HUD bar | `(0,0,400,24)` | fill `(8,8,18)`; bottom 1 px `(26,26,51)` | |
| T3 | coin | solid circle centre `(11,12)` r=5 `(255,204,51)`; highlight circle `(10,10)` r=2 `(255,255,160)` alpha 179; 1 px rim circle r=5 `(204,153,26)` | | draw after T2 |
| T4 | gold value | 10 px text, left at `(19,7)` | `(255,230,77)` | `"%d"` |
| T5 | WAVE label | 6 px text at `(66,2)` | `(128,128,179)` | `"WAVE"` |
| T6 | wave value | 10 px text at `(66,10)` | white | `"%d"` |
| T7 | LEFT label | 6 px text at `(104,2)` | `(128,128,179)` | `"LEFT"` |
| T8 | enemies remaining | 10 px text at `(104,10)` | white | `"%d"` |
| T9 | LIVES label | 6 px text at `(144,2)` | `(128,128,179)` | `"LIVES"` |
| T10 | lives value | 10 px text at `(144,10)` | `(255,102,102)` | `"%d"` |
| T11 | status | 8 px text, centre-aligned, centre `(200,12)` | `READY` `(255,255,102)`; `ACTIVE` `(51,230,51)`; `NEXT 2.4s` `(255,230,77)`; `PAUSED` `(230,230,230)` | `PHASE_READY` / `PHASE_WAVE` / `PHASE_INTERWAVE` / paused |
| T12 | speed | 8 px text, right-aligned at right edge x=396, y=8 | `(204,204,204)` | `PAUSED` / `0.5x` / `1x` / `2x` / `4x` |
| T13 | field frame | rect `(4,23,392,197)` | `(8,8,15)` | 1 px border around the field |
| T14 | field background | rect `(5,24,390,195)` | `(13,13,20)` | |
| T15 | path cells | 15x15 rect per path cell `i` | `(51+20*t, 31+10*t, 10+5*t)` with `t = i/N`, `N = path length` | filled before grid lines |
| T16 | grid lines | vertical x = `5 + 15k` k=0..26, horizontal y = `24 + 15k` k=0..13, 1 px | `(24,24,38)` | 41 lines total, cheaper than 338 cell outlines |
| T17 | tower icon | centred on cell, size per §3.4 (7/8/9 px), fan triangles | element colour | |
| T18 | tower level marker | text 6 px at `(cx, cy - size - 4)` centred; level >= 10 -> two gold rects (§3.4) | white alpha 204 / gold | |
| T19 | selected range | solid circle centre `(cx,cy)` r=32, tower colour alpha 15; 1 px circle outline r=32 alpha 38 | | only when `build_state==TOWER_SELECTED && cursor on that tower` |
| T20 | lasers | 1 px line at alpha `life/0.15`, plus 3 px line at alpha `(life/0.15)*0.1` | tower colour | under enemies |
| T21 | enemies | icon size 5 (normal) / 9 (elite) centred at enemy position | element colour; elite extra 1 px rect outline side `2*size+4` white alpha 64 | |
| T22 | enemy health bar | bg rect `(cx-6, cy-10, 13, 3)` black alpha 178; fill `(cx-5, cy-9, round(11*p), 2)` colour `(round(255*(1-p)), round(255*p), 0)` | | skip fill if `p <= 0` |
| T23 | cursor | 15x15 fill white alpha 20; 2x2 corner squares white alpha 153 at 4 corners; 1 px outline white alpha 76 | | always drawn (touch or D-pad) |
| T24 | build preview | 15x15 fill `(255,255,102)` alpha 38 + 1 px outline alpha 76; optional: range circle r=32 in armed element colour alpha 20 | | only in `BUILDING` |
| T25 | tooltip strip | rect `(0,220,400,20)` fill `(8,8,18)` alpha 242; top 1 px `(26,26,51)`; 8 px text centred at `(200,226)` | error `(255,51,51)`; BUILDING = element colour; TOWER_SELECTED = white; idle = `(128,128,179)` | content precedence: error > BUILDING > TOWER_SELECTED > READY/INTERWAVE hint > WAVE info |
| T26 | pause overlay | full-screen dim black alpha 150; `"PAUSED"` 16 px at centre `(200,96)`; 8 px speed + hint lines; `[PORT]` | | covers T1–T25 (draw last) |
| T27 | game-over overlay | full-screen dim black alpha 190; `"GAME OVER"` 16 px `(255,51,51)` at `(200,80)`; `"WAVE %d   KILLS %d"` 8 px white at `(200,110)`; hint 6 px at `(200,140)` | | |
| T28 | victory overlay (optional) | `"VICTORY"` 16 px gold; same layout as T27 | | only if `VICTORY_WAVE > 0` |

Tooltip strip strings `[PORT]`:

| State | Text (8 px, centred) |
|---|---|
| error toast active | `Path blocked!` / `Not enough gold!` / `Max level reached!` / `Select a tower first` / `Wave in progress` |
| `BUILDING` | `FIRE  20G  DMG 3  — TAP A CELL` (element name/colour changes) |
| `TOWER_SELECTED` | `FIRE  LV.3  DMG 12  UP 80G  SELL 60G` (`UP MAX` at level 10) |
| `READY` | `TAP START TO SEND THE FIRST WAVE` |
| `INTERWAVE` | `NEXT WAVE IN 2.4s` |
| wave active | `WAVE 3 ACTIVE  —  LEFT 12` |

### 11.3 Bottom screen (320x240) — touch control panel

The bottom screen is the only touch surface, therefore it contains a compact **touch map of the same grid** (12 px cells) plus all buttons. The mini-map is a control surface, not a second camera; it shows the same path cells, towers and enemies as the top screen (cheap: one fill per path/tower cell, 2x2 px enemy dots).

| # | Element | Rect / position | Colour | Notes |
|---|---|---|---|---|
| B1 | background | `(0,0,320,240)` | `(8,8,18)` | |
| B2 | map frame | `(3,3,314,158)` | `(5,5,13)` | 1 px border |
| B3 | map background | `(4,4,312,156)` | `(13,13,20)` | |
| B4 | map path cells | cell `(col,row)` -> rect `(4+(col-1)*12, 4+(row-1)*12, 12, 12)` | same gradient as T15 | |
| B5 | map towers | same cell rect | tower colour alpha 64 fill + 1 px border tower colour alpha 153 | no icon detail |
| B6 | map grid lines | vertical x = `4+12k` k=0..26; horizontal y = `4+12k` k=0..13, 1 px | `(20,20,31)` | |
| B7 | map enemies | 2x2 rect at `(4 + (x_top-5)*0.8, 4 + (y_top-24)*0.8)`; elite: 3x3 + 1 px white outline | element colour | top-field px -> map px scale = 12/15 = 0.8 |
| B8 | map cursor | 12x12 1 px outline white alpha 255 | | tracks cursor from touch or D-pad |
| B9 | map selected tower | 12x12 1 px outline `(255,230,77)` | | when `TOWER_SELECTED` |
| B10 | map build-armed highlight | 12x12 1 px outline `(255,255,102)` | | when a type is armed |
| B11 | build buttons (3) | `(6,164,100,28)`, `(112,164,100,28)`, `(218,164,100,28)` | base `(20,20,36)`, 1 px border `(38,38,64)`; armed: fill tower colour alpha 51 + 1 px border tower colour | icon 10 px centred at `(x+16, y+14)`; name 8 px at `(x+28,y+3)` in tower colour (`FIRE`/`WATER`/`FOREST`); cost 8 px at `(x+28,y+13)` `(255,230,77)` (`20G`) |
| B12 | action buttons (5) | `(6,196,60,28)`, `(68,196,60,28)`, `(130,196,60,28)`, `(192,196,60,28)`, `(254,196,60,28)` | base `(20,20,36)`/border `(38,38,64)`; disabled text `(76,76,102)`; enabled text `(230,230,245)`; sell-confirm fill `(128,26,26)` | labels (6 px, 2 lines max): `UPGRADE`/cost, `SELL`/price, `NEXT` or `START` or `WAIT`, `SPEED`/`1x`, `PAUSE`/`RESUME` |
| B13 | wave-enemies strip | `(0,224,320,16)` fill `(5,5,13)`; four slots at x = 8, 88, 168, 248: 8 px icon at `(x, y+4)` + 8 px `"x%d"` at `(x+12, y+4)` | fire/water/forest colours, elite `(255,204,51)` star | skip slots with count 0; this is the Lua's "WAVE ENEMIES" HUD section |
| B14 | menu buttons | START `(40,84,240,56)`; CONTROLS `(40,152,240,32)` | START fill `(217,38,38)` + highlight `(255,77,77)` alpha 76 top half; CONTROLS fill `(20,20,36)` | 16 px `START` white; 8 px `CONTROLS` |
| B15 | pause buttons | RESUME `(60,80,200,36)`; SPEED `(60,124,200,36)`; QUIT TO MENU `(60,168,200,36)` | same as B14 style | |
| B16 | game-over buttons | RETRY `(60,120,200,40)`; MAIN MENU `(60,168,200,40)` | same | |

Touch hit boxes (bottom screen, x/y/w/h):

| Region | Rect | Action |
|---|---|---|
| map cell (col,row) | `(4+(col-1)*12, 4+(row-1)*12, 12, 12)` for col 1..26, row 1..13 | §10.3 |
| build button 1/2/3 | `(6,164,100,28)` / `(112,164,100,28)` / `(218,164,100,28)` | arm/disarm type 1/2/3 |
| UPGRADE | `(6,196,60,28)` | upgrade selected tower |
| SELL | `(68,196,60,28)` | arm/sell selected tower |
| NEXT | `(130,196,60,28)` | start wave 1 / skip intermission (disabled during a wave) |
| SPEED | `(192,196,60,28)` | cycle 0.5 / 1 / 2 / 4 |
| PAUSE | `(254,196,60,28)` | pause/resume |
| menu START | `(40,84,240,56)` | begin run |
| menu CONTROLS | `(40,152,240,32)` | help overlay |
| pause resume | `(60,80,200,36)` | unpause |
| pause speed | `(60,124,200,36)` | cycle speed |
| pause quit | `(60,168,200,36)` | back to menu |
| game-over retry | `(60,120,200,40)` | reset run |
| game-over menu | `(60,168,200,40)` | back to menu |

### 11.4 ASCII mockups (not to scale)

**Top screen (400x240)** — battlefield + HUD + tooltip.

```
 +--------------------------------------------------------------------------------+
 | (o) 150   WAVE 1   LEFT 5   LIVES 20        ACTIVE                    1x        |  T2..T12  y 0..23
 +--------------------------------------------------------------------------------+
 | +----------------------------------------------------------------------------+ |  T13
 | |. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . | |  row 1
 | |. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . | |  row 2
 | |. . . . . . . . F . . . . . . . . . . . . . . . . . . . . . . . . . . . . . | |  row 3  (F = Fire tower)
 | |. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . | |  ...
 | |. . . . . . . . . . . . . W . . . . . . . . . . . . . . . . . . . . . . . . | |  (W = Water)
 | |. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . | |  26 cols x 13 rows
 | |. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . | |  cells 15x15 px
 | |. . . . . . . . . . . . . . . . . . e . . . . . . . . . . . . . . . . . . . | |  (e = enemy)
 | |. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . | |
 | |. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . | |
 | |. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . | |
 | |[ ] . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . | |  ([ ] = cursor)
 | |# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # | |  row 13 = PATH  y 204..219
 | +----------------------------------------------------------------------------+ |
 +--------------------------------------------------------------------------------+
 | FIRE LV.3  DMG 12  UP 80G  SELL 60G                       TAP A CELL TO BUILD  |  T25  y 220..239
 +--------------------------------------------------------------------------------+
```

**Bottom screen (320x240)** — touch map + controls.

```
 +--------------------------------------------------------------------------+
 | +----------------------------------------------------------------------+ |
 | | . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .  | |  B2..B10 y 4..160
 | | . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .  | |  map 312x156, 12 px cells
 | | . . . . . F . . . . . . . . . . . . . . . . . . . . . . . . . . . .  | |
 | | . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .  | |
 | | . . . . . . . . . . . . . . . . . . e . . . . . . . . . . . . . . .  | |
 | | . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .  | |
 | | . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .  | |
 | | . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .  | |
 | | . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .  | |
 | | . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .  | |
 | | . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .  | |
 | | . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .  | |
 | | # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # | |  path row 13
 | +----------------------------------------------------------------------+ |
 +--------------------------------------------------------------------------+
 | [ FIRE 20G ]        [ WATER 20G ]        [ FOREST 20G ]                    |  B11  y 164..192
 +--------------------------------------------------------------------------+
 | [UPGRADE]   [SELL]   [NEXT]    [SPEED]    [PAUSE]                          |  B12  y 196..224
 |    80G       60G      WAIT       1x                                         |
 +--------------------------------------------------------------------------+
 |  F x4        W x4        Fo x4        EL x1                                |  B13  y 224..239
 +--------------------------------------------------------------------------+
```

### 11.5 Draw order & frame budget `[PORT]`

Draw order matches the Lua: background bands, HUD bar, field frame, field bg, path cells, grid lines, tower range (if selected), towers, lasers, enemies + health bars, cursor, build preview, tooltip, overlays. Bottom: bg, map frame/bg, map grid lines, map path cells, map towers, map enemies, map cursor/selection, buttons, wave strip, overlays.

Budget estimate (typical gameplay, ~25 towers, ~30 enemies, ~20 lasers):

| Layer | Primitives |
|---|---|
| top background + HUD | ~40 quads + ~8 text runs |
| top field: path cells + grid lines + towers + enemies + lasers + cursor | ~100 path quads + 41 lines + ~25*6 triangles + 30*(4–6 tris) + ~20 lines + 5 quads ≈ 500 tris/quads |
| bottom map | ~100 path cells + 41 lines + 25 tower cells + 30 enemy dots ≈ 200 |
| buttons/text | ~40 quads + ~20 text runs |

Total < 1000 primitives/frame. citro2d handles this comfortably at 60 fps; avoid per-cell outlines (use grid lines) and avoid drawing empty cells (single background fill). Do not clear the whole framebuffer per screen more than needed; batch all rectangles of one colour where convenient (citro2d is immediate-mode; a few hundred quads is fine).

### 11.6 Stereo note

Render the top screen to `C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT)` only. The design is a flat 2D board; duplicating it to `GFX_RIGHT` doubles top-screen draw cost and requires eye-separation tuning. Leave the 3D slider without stereo content (or, if desired, render the HUD at zero parallax and the field at a small `+/-1` px offset — do that only after the 2D version hits 60 fps).

---

## 12. Audio mapping

### 12.1 Event -> file `[LUA]` (`main.lua:243-287`)

| Event | Sound | Type / loop / volume | Trigger frequency |
|---|---|---|---|
| menu opens / app boot | `sounds/menu_music.ogg` | stream, loop, volume 0.5 | once on boot; stops on run start |
| gameplay starts | `sounds/game_music.ogg` | stream, loop, volume 0.5 | once on menu confirm; never paused/stopped by pause or game over in the Lua |
| tower built successfully | `sounds/build.wav` | static | per successful placement |
| tower sold | `sounds/sell.wav` | static | per confirmed sale |
| tower fires | `sounds/laser.wav` | static | per shot, but throttled globally: only if `laserSoundCooldown <= 0`, then set to **0.08** game-seconds |
| wave starts | `sounds/wave_start.wav` | static | every `startWave()` call (including retries in the Lua) |
| enemy dies | `sounds/enemy_die.wav` | static | per kill |
| tower upgraded | `sounds/upgrade_1.wav` … `upgrade_6.wav` | static | random one per successful upgrade (`math.random(1,#loaded)`); no level/element mapping |
| game over | `sounds/gameover.wav` | static | loaded but **never played** in the Lua; use it on `GS_GAME_OVER` in the port |
| error toast | — | — | no sound |
| button/touch UI | — | — | no sound |

All sfx use `stop()` then `play()` (retrigger, no overlap mixing of the same source). `soundEnabled` is always true; there is no mute UI. Missing/failed-to-load sounds are silently ignored (`pcall`).

### 12.2 Audio file inventory (on-disk)

| File | Bytes | Format (by extension) | Est. duration `[DERIVED]` (16-bit stereo 44.1 kHz WAV assumption) | 3DS use |
|---|---:|---|---:|---|
| `build.wav` | 305,998 | WAV | ~1.73 s | keep |
| `enemy_die.wav` | 6,318 | WAV | ~0.04 s | keep |
| `gameover.wav` | 10,497,182 | WAV | ~59.5 s — **huge** | trim to first 2–3 s stinger |
| `game_music.ogg` | 3,590,538 | OGG | depends on codec | optional, see §12.3 |
| `laser.wav` | 7,758 | WAV | ~0.04 s | keep, mix quietly |
| `menu_music.ogg` | 1,705,932 | OGG | depends | optional |
| `sell.wav` | 93,518 | WAV | ~0.53 s | keep |
| `upgrade_1.wav` | 76,110 | WAV | ~0.43 s | keep |
| `upgrade_2.wav` | 78,158 | WAV | ~0.44 s | keep |
| `upgrade_3.wav` | 79,694 | WAV | ~0.45 s | keep |
| `upgrade_4.wav` | 71,502 | WAV | ~0.41 s | keep |
| `upgrade_5.wav` | 76,622 | WAV | ~0.43 s | keep |
| `upgrade_6.wav` | 63,310 | WAV | ~0.36 s | keep |
| `wave_start.wav` | 151,278 | WAV | ~0.86 s | keep |

Raw sizes above are containers; actual decoded PCM is what matters on 3DS (e.g. `gameover.wav` as stereo 44.1 kHz PCM = ~10.5 MB linear, plus decoding/streaming machinery).

### 12.3 3DS audio recommendations `[PORT]`

- Convert SFX to **16-bit mono 22050 Hz** or **IMA-ADPCM** and embed as binary (`.bin`) via `bin2s`; a 1 s mono 22.05 kHz PCM = 44 KB. Total SFX set trimmed to < 350 KB.
- `gameover.wav` must be trimmed (10.5 MB is unacceptable); a 2 s mono stinger is ~88 KB.
- Music: libctru has no built-in OGG decoder. Options in order of effort: (a) omit music (spec says music is optional); (b) pre-decode a 15–30 s loop to mono 22.05 kHz PCM and loop it from RAM (~0.7–1.5 MB) with `ndsp` wave buffers; (c) integrate `tremor`/`stb_vorbis` to stream from SD (not recommended for v1). Do not load the full OGGs into RAM and decode them at runtime while rendering.
- Use `ndsp` with at least 8 channels for SFX; reserve a separate channel set for music. Implement the 0.08 s global laser cooldown exactly as the Lua (it prevents audio spam).
- Menu music starts at boot; game music replaces it on run start; game-over: stop game music, play `gameover`; retry/restart game music; menu return: menu music. No fades in the Lua — hard cut is acceptable, a 0.2 s fade is a `[PORT]` nicety.

---

## 13. What likely made the 3DS attempt fail (evidence-based)

1. **Absolute 1280x720 layout, no resolution independence.** `love.conf` demands `minwidth=1280, minheight=720` (`main.lua:11-18`), `love.load` calls `love.window.setMode(1280, 720, ...)` (`main.lua:1495-1501`), and every UI coordinate is an absolute 1280x720 value: HUD panel at y 505..720, info panel at x 490..1050, grid offset x=276 (`main.lua:38-43`), menu panel 700x560 at (290,80). On a 400x240 screen the visible top-left corner contains: part of the menu title at x 440 (off-screen), and during play the grid's left 124 px (columns 1–4) plus the HUD at y>505 (off-screen). This alone explains the README's "half the screen cut off". No scaling transform exists anywhere in the file.

2. **Per-frame primitive count too high for LovePotion's immediate-mode drawing.** `drawGradientBackground()` issues `SCREEN_H + 1 = 721` filled rectangles **every frame** (`main.lua:437-443`); the field loop does 338 cells with an `isPathCell()` linear scan over the current path (O(up to 338)) **every frame** (`main.lua:949-968`, `482-490`); towers and enemies are filled polygons with 6–10 vertices each (`main.lua:293-409`); five TrueType fonts are rasterized and used. On the 3DS ARM11 the Lua->LovePotion render path for hundreds/thousands of individual draw calls per frame is the likely fps killer.

3. **Unbounded per-frame table allocation on the menu.** `updateMenuParticles` spawns **4 new particle tables per frame** with no cap (`main.lua:600-615`), lifetime 1–4 s -> steady state roughly 240/s x 2.5 s ≈ 600 live tables, continuously churning the Lua GC while the title screen is up. The same allocation style appears elsewhere (a laser table per shot, 4 neighbour tables per BFS node).

4. **BFS implementation cost and hitching.** `findPath()` allocates 4 neighbour tables per expanded node, uses `table.remove(queue, 1)` (O(n) shift) for every pop, and is run **twice per successful build** (validation + update, `main.lua:1362-1364`) plus once per failed attempt and once per sell. Building a maze — the core strategy — is exactly when the player is stressing the pathfinder.

5. **Audio memory/decode.** `gameover.wav` is 10.5 MB, `game_music.ogg` 3.6 MB and `menu_music.ogg` 1.7 MB. LÖVE/LovePotion would decode/stream these on an ARM11 with no audio DSP assist; simply loading them dominates boot time, and streaming OGG while rendering competes for the same CPU.

6. **Touch controls fought the keyboard emulation (fatal).** `love.touchpressed` (`main.lua:1463-1488`) sets `cursor` and then calls `love.keypressed("a")`. But in `MOVING` state, `a` is an alias for "move cursor left" (`main.lua:1387-1393`) and the function returns before the confirm logic, so **every tap moves the cursor one cell left of the tapped cell**. Entering `BUILDING` requires `return`/`space`, which a touch-only device cannot send; and in `BUILDING`, `a` is intercepted again to cycle tower type (`main.lua:1336-1340`), so even with a keyboard `a` never builds. Net effect on a touch-only 3DS: the only working touch interaction is the menu START button; the game is unbuildable. This matches the README's "touch controls fighting the mouse emulation the whole time".

7. **No performance escape hatches.** No sprite atlas, no canvas/offscreen caching, no batching, no culling, no reduced-detail mode, no fixed-timestep; every visual is recomputed from primitives each frame at full 1280x720 design density. There is also no resolution-independent scale factor to change once the screen size is known.

8. **The Lua game itself has no end state.** Even when it ran, leaking enemies restarted the same wave forever (`main.lua:779-798`) and leaking paid gold, so the game could not be lost or finished — a LovePotion port would have looked broken regardless of frame rate.

The C rewrite removes items 1–7 structurally: fixed 400x240/320x240 layout (§11), preallocated arrays and allocation-free update (§2.6/§15), one BFS per action, banded background instead of 721 rects, no per-cell outlines (grid lines), touch-native input (§10), and bounded particle counts.

---

## 14. Known bugs / unclear bits

The C port should **not** reproduce the following unless explicitly marked bug-compatible. Each row gives the Lua reference, the visible symptom, and the required C behaviour.

### 14.1 Bug table

| ID | Lua ref | Bug / ambiguity | Symptom in Lua | Required C behaviour |
|---|---|---|---|---|
| B1 | `main.lua:1336-1344` vs `1350-1355`, `1387-1393` vs `1406-1415` | `a` is tested first as "cursor left" / "previous type"; the real confirm branch is unreachable via `a` | keyboard `A` never selects or builds; only `return`/`space` work; help text lies | A is the confirm button on 3DS (§10.2); D-pad moves the cursor. |
| B2 | `main.lua:698-707`, `779-798` | Leaking pays gold (+2/+10) and restarts the same wave forever; no lives | infinite farm / soft-locked progression; game cannot end | leaks cost 1 or 3 lives, pay nothing, wave advances (§7.2); `GS_GAME_OVER` at 0. |
| B3 | `main.lua:644-648`, `1031-1039`, `1295-1301` | Intended pre-wave-1 build phase is dead: menu confirm sets `gameStarted=true` and calls `startWave()` at once | "BUILD YOUR DEFENSE / Press A to start Wave 1" never visible; player gets 2.95 s to build | `PHASE_READY`: build freely, wave 1 starts on START/NEXT (§1.6). |
| B4 | `main.lua:779-798` | Waves auto-chain in the same frame; `waveComplete` status never visible | no intermission, never shows COMPLETE | `INTERWAVE_SEC = 3.0` (0 = Lua-exact) with countdown + NEXT skip; status shows the countdown. |
| B5 | `main.lua:63-69`, `1353` | Start cell is seeded into BFS even if a tower occupies it; end cell cannot be built on | tower can be built on the spawn cell (enemy spawns inside it); inconsistent rules | disallow building on start **and** end cells; validate bounds, empty cell, BFS. |
| B6 | `main.lua:558-576` | HUD composition quotas computed but spawner picks uniformly at random | displayed wave composition is a prediction, not reality (binomial variance) | spawn exact quotas (default) or relabel the HUD; one `#define` toggle. |
| B7 | `main.lua:1235-1237` | `"X: " .. cost .. "G"` with `cost = "MAX"` | info panel shows `X: MAXG` | show `UP MAX` / `MAX` correctly. |
| B8 | `main.lua:1103` | `printf` right-aligned at x=1250 with limit 200 -> right edge 1450 | "WAVE ENEMIES" header and the `"---"` placeholder are drawn off-screen | 3DS layout puts the composition strip on the bottom screen (§11.3 B13). |
| B9 | `main.lua:600-615` | 4 particle tables spawned per frame, no cap; `life` and `maxLife` rolled independently | GC churn; alpha can exceed 1 or go negative | cap `MAX_PARTICLES 64`; one particle spawn/frame; `maxLife = life`; menu only. |
| B10 | `main.lua:1463-1488`, `883-918` | Touch bypasses the controls overlay and the error-toast swallow; overlay doesn't block touch | tapping while help is open still moves the cursor/builds | input is gated by `help_open` / `paused`; toast never swallows input. |
| B11 | `main.lua:1288-1458` | Error toast swallows one key press | first key after an error is lost | show toast for 2 s, do not swallow input. |
| B12 | `main.lua:188`, `144`, etc. | Global `state` used for build mode, `gameState` for screens | naming confusion | `build_state` enum + `game_state` enum in C. |
| B13 | `main.lua:158-162`, `164-179` | `enemyTypes[].glowColor` and tower `glowColor` are never read | dead data | keep or drop; no visual effect. |
| B14 | `main.lua:186`, `150` | `getTotalCost` hardcodes base cost 20 rather than reading the type | refund formula silently breaks if costs change | store `cost_paid` or derive `20 * 2^(level-1)` (all types cost 20 today). |
| B15 | `main.lua:858-877`, `1495-1501` | Window size hardcoded 1280x720 and asserted as min size | entire UI layout is resolution-locked | full layout spec in §11; no scaling transform needed. |
| B16 | `main.lua:199`, `201`, `764` | `gameTime` and `totalKills` are written but never read/displayed | dead counters | `totalKills` shown on the game-over screen; `gameTime` optional stat. |
| B17 | `main.lua:263` | `gameover.wav` loaded, never played | dead sound | play on `GS_GAME_OVER`. |
| B18 | `main.lua:206`, `570`, `783-792` | `waveRetry`/`enemiesThatPassed` designed around a retry mechanic that loops forever | infinite same-wave retry (see B2) | remove both; lives handle leaks. |
| B19 | `main.lua:494`, `41-42` | `errorMessage` cleared by any key; `errorTimer` scaled by `gameSpeed` | toast duration changes with speed | keep scaled by game speed (freezes at 0x) but don't consume input. |
| B20 | `main.lua:140-145` | `getDamageMultiplier` returns 1.0 for unknown type/element; no other elements exist | dead fallbacks | keep as `return 1.0f` default. |
| B21 | `main.lua:413-417` | Health bar fill width = `(width-2)*percent`; percent can be >1? no, but negative health -> negative width | harmless flicker | clamp `p` to `[0,1]`. |
| B22 | `main.lua:1380-1402` | Cursor moves are clamped (`math.max/min`) but touch-set cursor isn't validated beyond the cell loop | fine today | clamp in the touch hit test. |
| B23 | `main.lua:1463-1488` | Touch cannot reach upgrade/sell/speed/pause at all | game unplayable on touch-only hardware | all actions have touch targets (§11.3). |
| B24 | `main.lua:370` | Tower icon size grows with level but range/health bar etc. do not scale | purely cosmetic | keep scaled size (7/8/9 px). |

### 14.2 Intentional `[PORT]` deviations (kept minimal)

| # | Deviation | Default | Lua behaviour |
|---|---|---|---|
| D1 | `PHASE_READY` before wave 1 instead of instant start | on | instant `startWave()` on menu confirm |
| D2 | `INTERWAVE_SEC = 3.0` between waves | 3.0 s | 0 s (same-frame chain) |
| D3 | lives (20), leak penalties (-1 / -3), no leak gold | on | no lives; leak pays +2/+10 |
| D4 | wave retry removed | on | infinite retry |
| D5 | exact element quotas spawning (HUD truthfulness) | on (`EXACT_QUOTAS 1`) | uniform random |
| D6 | `MAX_WAVE 200` engine cap | on | unbounded (numerically unbounded wave count, HP/speed grow forever) |
| D7 | game-over / pause / victory screens + `gameover.wav` playback | on | none |
| D8 | START/NEXT button to launch waves | on | A/Return only |
| D9 | toast does not swallow input | on | swallows one key |
| D10 | tower-cell tint skipped; grid drawn as 41 lines instead of 338 outlines; background banded (8 rects) | on | per-cell outlines, 721 rect gradient |
| D11 | menu particles capped at 64 | on | unbounded |
| D12 | fixed 60 Hz timestep | on | LÖVE variable `dt` |

### 14.3 Open questions / ambiguities needing sign-off

- **Q1 — Elements.** The remake README claims five elements + combinations; the Lua has three, identical, no combining. This spec follows the Lua. If combinations are wanted, treat it as new design (needs stats, costs, UI) — not a port. **Blocking for anything beyond 3 towers.**
- **Q2 — Victory condition.** None in the Lua. Options: (a) endless, game over only (default in this spec); (b) `VICTORY_WAVE = 50` -> `GS_VICTORY`. Pick one before the game-over/victory screen is implemented; the screen is cheap either way.
- **Q3 — L/R buttons.** Source of truth is speed control (`main.lua` help overlay, `1316-1325`); the remake README says "L/R switch tower type". Spec follows the source (L/R = speed). Confirm, or swap to README and use Select for speed.
- **Q4 — Lives value / penalties.** 20 lives, -1 normal, -3 elite, 0 leak gold are `[PORT]` values with no Lua basis; needs a tuning pass after the game is playable.
- **Q5 — Inter-wave gap.** 3.0 s default; 0.0 reproduces the Lua exactly. Pick after the first playtest.
- **Q6 — Bug-compatibility level.** This spec fixes B1/B2/B3/B4/B5. If any must stay bug-compatible (e.g. for nostalgia/streaming), the fix is isolated per §14.1.

### 14.4 Balance observations (not bugs, but unverified)

- Tower damage doubles per level (exponential) while enemy HP/speed grow linearly, so late waves trivialise once one tower reaches level 8–10: level 10 = 1536 damage / 0.28 s = 5486 DPS vs wave-100 elite HP 780. Balance collapses after ~wave 30.
- Conversely wave 1 is tight: 150 gold = 7 towers; a tower in the row above the path covers only ~3.8 cells of path (chord 106 px), and at wave-1 speed an enemy is in range 0.76 s, enough for ~1 shot of 3 damage vs 15 HP. The intended counter-play is maze-building (towers on path cells force a detour, §2.4); range 60 px makes rows 11–13 the only useful rows for the straight path. If the 3DS playtest feels impossible, tune `START_GOLD` or wave-1 HP, not the pathfinder.
- No income besides kills means a bad early build order can soft-lock progress (especially with lives now penalising leaks). Consider a small wave-completion bonus as a `[PORT]` tuning lever (default 0 = faithful).

---

## 15. Implementation guide for the C engineer

### 15.1 Suggested module split

| File | Contents |
|---|---|
| `source/core/consts.h` | Appendix A constants, enums, structs |
| `source/core/grid.c/.h` | `tower_grid[26][13]`, cell<->px mapping, occupancy |
| `source/core/path.c/.h` | BFS, path cells, `path_positions`, `path_position_at(t)`, `path_is_cell()` |
| `source/core/game.c/.h` | update loop, build state machine, economy, lives, waves state |
| `source/core/waves.c/.h` | `start_wave()`, spawn logic, elite block, completion, intermission |
| `source/core/towers.c/.h` | tower stats, level tables, damage matrix, targeting, firing |
| `source/core/enemies.c/.h` | enemy array, spawn/despawn, movement, leak/kill resolution |
| `source/3ds/main.c` | `aptMainLoop`, `hidScanInput`, touch, frame loop, citro2d init |
| `source/3ds/render_top.c` | §11.2 |
| `source/3ds/render_bottom.c` | §11.3 |
| `source/3ds/audio.c` | §12.3 ndsp wrapper, laser cooldown, music |
| `source/3ds/text.c` | C2D_TextBuf helpers + 5 text sizes |

The Makefile already lists `SOURCES := source source/core` and `INCLUDES := include` (repo Makefile), so adding `source/3ds` requires adding it to `SOURCES` (or moving `main.c` to `source/`). Coordinate with the Lead before editing the Makefile.

### 15.2 Data structures (all static, no malloc in play)

```c
#define GRID_COLS 26
#define GRID_ROWS 13
#define MAX_PATH_CELLS 338
#define MAX_TOWERS 338
#define MAX_ENEMIES 512
#define MAX_LASERS 256
#define MAX_PARTICLES 64
#define MAX_WAVE_EVENTS 10
#define MAX_LEVEL 10
#define MAX_WAVE 200

typedef enum { ELEM_FIRE = 0, ELEM_WATER = 1, ELEM_FOREST = 2 } Element;
typedef enum { GS_MENU, GS_PLAYING, GS_GAME_OVER, GS_VICTORY } GameState;
typedef enum { PHASE_READY, PHASE_WAVE, PHASE_INTERWAVE } WavePhase;
typedef enum { BUILD_MOVING, BUILD_PLACING, BUILD_SELECTED } BuildState;

typedef struct { u8 type; u8 level; float damage; float fire_rate; float cooldown; bool sell_confirm; } Tower;
typedef struct { float t, hp, max_hp, speed, x, y; u8 type; bool elite; } Enemy;
typedef struct { float x1, y1, x2, y2; u8 r, g, b; float life, max_life; } Laser;
typedef struct { i16 col, row; } PathCell;

typedef struct {
    GameState game_state; WavePhase phase; BuildState build_state; bool paused;
    float game_speed; int speed_index;
    float dt_scaled;                    // dt * game_speed for this frame
    u32 gold; int lives; int wave;
    int enemy_spawned, enemies_per_wave, enemies_remaining;
    float spawn_timer, spawn_interval;
    bool wave_active; bool game_started;
    int kills; int lives_lost_this_wave;
    float interwave_timer;
    int cursor_col, cursor_row; int selected_type; int armed_type;
    const char* error_msg; float error_timer;
    float laser_sfx_cooldown;
    Enemy enemies[MAX_ENEMIES]; int enemy_count;
    Laser lasers[MAX_LASERS]; int laser_count;
    Tower towers[MAX_TOWERS];            // dense list, grid stores index+1 or -1
    i16 tower_grid[GRID_COLS][GRID_ROWS];
    PathCell path[MAX_PATH_CELLS]; int path_len;
    float path_px[MAX_PATH_CELLS][2];
} Game;
```

Enemy/laser removal must preserve array order (use `memmove`, not swap-remove) so the nearest-target tie-break stays "earliest spawned" like the Lua. `MAX_ENEMIES` never exceeded for `wave <= 200` (max 413).

### 15.3 Per-level static tables (generate at compile time or hardcode)

```c
// upgrade cost from level L (1..9) to L+1
static const u16 UPGRADE_COST[10] = {20,40,80,160,320,640,1280,2560,5120,0};
// damage at level L (index L-1)
static const u16 TOWER_DAMAGE[10] = {3,6,12,24,48,96,192,384,768,1536};
// cooldown in seconds at level L
static const float TOWER_FIRE_RATE[10] = {0.80f,0.72f,0.64f,0.57f,0.51f,0.45f,0.40f,0.36f,0.32f,0.28f};
// total invested / sell value: 20<<(L-1) and 15<<(L-1)
```

### 15.4 Order-sensitive update (copy §1.1 exactly, with §1.6 replacements)

1. if `game_state == GS_MENU`: menu particles; return.
2. `dt_scaled = (1/60) * (paused ? 0 : game_speed)`.
3. decay laser-sound cooldown, error timer, laser lives.
4. if `PHASE_WAVE`: spawn timer/elites; move enemies; resolve leaks (lives, game over check).
5. fire towers (column-major grid scan, nearest target), apply damage, resolve kills.
6. wave completion -> phase transition / intermission countdown -> next wave.
7. draw.

Do not reorder steps 4–6: spawn-before-move and move-before-fire change whether a newly spawned enemy can be hit this frame, matching the Lua exactly.

### 15.5 Performance / memory budget `[PORT]`

- RAM: `Game` struct ≈ `512*32 B + 256*32 B + 338*32 B + 338*16 B + 338*8 B` ≈ 50 KB. Trivial for 128 MB FCRAM. Static `Game` in BSS, not stack.
- Frame: < 1000 draw primitives (§11.5); no allocation; no `printf` in the loop (cache formatted HUD strings, update only when values change).
- Text: one `C2D_TextBuf` of e.g. 8192 glyphs, cleared and re-filled each frame; keep the number of `snprintf` calls small and reuse buffers.
- Keep the 0.08 s laser-sfx cooldown exactly (prevents dozens of overlapping channels).
- Enable `-O2`; the repo Makefile already uses `-O2 -march=armv6k`.

### 15.6 Acceptance tests (numbers must match exactly)

| Test | Expected |
|---|---|
| Boot path | 26 cells straight along row 13; first position `(12.5, 211.5)` on 3DS, `(290,428)` in original units |
| Wave 1 | 5 spawn slots, interval 2.95 s, first spawn 2.95 s after wave start; no elites; HP 15, speed 20 |
| Wave 5 | 14 spawns, 1 elite; elite spawns at slot 2; elite HP 67.5, speed 43.2 |
| Wave 10 | 26 spawns, 3 elites (slots 2–4); quotas 6/6/8 |
| Wave 25 | 63 spawns, 10 elites (slots 2–11); quotas 14/14/15; interval 1.75 s |
| Spawn interval | wave 43 = 0.85 s, wave 44+ = 0.80 s |
| Build on path | 7-tower wall forces BFS detour; total path length increases; enemies' world positions jump to the new path preserving t |
| Blocked build | enclosing the end cell or blocking all routes -> `findPath() == NULL` -> placement rolled back, gold unchanged, toast `Path blocked!` |
| Start/end cells | placement rejected on both (B5 fix) |
| Tower level 1 | 3 dmg, 0.80 s cooldown, range 60 |
| Level table | L10 = 1536 dmg, 0.28 s, total 10240, sell 7680; verify all of §8.1 |
| Multipliers | Fire vs Forest x2, Fire vs Water x0.5, Water vs Fire x2, Forest vs Water x2 |
| Kill | +10 gold, `enemiesRemaining--`, `kills++` |
| Sell | `floor(total*0.75)`, path recomputed |
| Pause | `gameSpeed = 0` freezes spawn timer, enemy `t`, cooldowns, lasers, toast; input still works |
| Toast | exactly 2.0 s of scaled game time |
| Laser | life 0.15 s, global sfx cooldown 0.08 s |

---

## Appendix A — constant block for the C port

```c
/* screens */
#define TOP_W 400
#define TOP_H 240
#define BOT_W 320
#define BOT_H 240

/* grid / field (top screen) */
#define GRID_COLS 26
#define GRID_ROWS 13
#define CELL_PX 15
#define FIELD_X 5
#define FIELD_Y 24
#define FIELD_W (GRID_COLS * CELL_PX)   /* 390 */
#define FIELD_H (GRID_ROWS * CELL_PX)   /* 195 */
#define START_COL 1
#define START_ROW 13
#define END_COL 26
#define END_ROW 13

/* touch map (bottom screen) */
#define MAP_X 4
#define MAP_Y 4
#define MAP_CELL 12

/* original-space values kept for reference / gameplay math */
#define ORIG_CELL 28
#define ORIG_OFFSET_X 276
#define ORIG_OFFSET_Y 78
#define ORIG_TOP_REGION 500

/* scale from original units to top-screen px */
#define S3D (15.0f / 28.0f)             /* 0.535714 */
#define RANGE_PX 60.0f                  /* gameplay range; draw radius = 32 px */
#define RANGE_DRAW_PX 32
#define ENEMY_ICON_PX 5
#define ELITE_ICON_PX 9
#define HPBAR_W 13
#define HPBAR_H 3

/* economy */
#define START_GOLD 150
#define START_LIVES 20                  /* [PORT] */
#define TOWER_COST 20
#define KILL_GOLD 10
#define LIVES_LOST_NORMAL 1             /* [PORT] */
#define LIVES_LOST_ELITE 3              /* [PORT] */
#define SELL_NUM 3
#define SELL_DEN 4                      /* floor(total * 0.75) */
#define MAX_LEVEL 10

/* speed */
static const float SPEED_LEVELS[5] = {0.0f, 0.5f, 1.0f, 2.0f, 4.0f};
#define SPEED_INDEX_DEFAULT 2           /* 0-based -> 1.0x */
#define SPEED_LEVEL_COUNT 5

/* waves */
#define SPAWN_BASE 3.0f
#define SPAWN_PER_WAVE 0.05f
#define SPAWN_MIN 0.8f
#define ENEMIES_BASE 3
#define ENEMIES_PER_WAVE 2
#define ENEMY_HP_BASE 15.0f
#define ENEMY_HP_PER_WAVE 3.0f
#define ENEMY_SPEED_BASE 20.0f
#define ENEMY_SPEED_PER_WAVE 1.0f
#define ELITE_HP_MULT 2.5f
#define ELITE_SPEED_MULT 1.8f
#define ELITE_DELAY_STEP_25 0.3f
#define ELITE_DELAY_STEP_10 0.2f
#define ELITE_DELAY_STEP_5 0.5f
#define MAX_WAVE 200
#define INTERWAVE_SEC 3.0f              /* [PORT], 0.0 = Lua-exact */
#define EXACT_QUOTAS 1                  /* [PORT] */

/* combat */
#define DMG_STRONG 2.0f
#define DMG_WEAK 0.5f
#define LASER_LIFE 0.15f
#define LASER_SFX_COOLDOWN 0.08f
#define ERROR_TOAST_SEC 2.0f
#define TOWER_HITSCAN 1

/* tower base stats (all types identical) */
#define TOWER_DAMAGE_L1 3.0f
#define TOWER_FIRE_RATE_L1 0.8f
#define TOWER_RANGE 60.0f

/* colours (C2D_Color32 = ABGR) */
#define C_FIRE        C2D_Color32(242, 38, 38, 255)
#define C_WATER       C2D_Color32(38, 102, 242, 255)
#define C_FOREST      C2D_Color32(38, 230, 38, 255)
#define C_FIRE_GLOW   C2D_Color32(242, 77, 26, 255)
#define C_WATER_GLOW  C2D_Color32(26, 128, 242, 255)
#define C_FOREST_GLOW C2D_Color32(26, 230, 77, 255)
#define C_GOLD        C2D_Color32(255, 230, 77, 255)
#define C_COIN        C2D_Color32(255, 204, 51, 255)
#define C_ERROR       C2D_Color32(255, 51, 51, 255)
#define C_PANEL       C2D_Color32(8, 8, 18, 255)
#define C_CELL        C2D_Color32(13, 13, 20, 255)
#define C_FIELD_FRAME C2D_Color32(8, 8, 15, 255)
#define C_GRIDLINE    C2D_Color32(24, 24, 38, 255)
#define C_LABEL       C2D_Color32(128, 128, 179, 255)
#define C_READY       C2D_Color32(255, 255, 102, 255)
#define C_ACTIVE      C2D_Color32(51, 230, 51, 255)
#define C_COMPLETE    C2D_Color32(255, 255, 77, 255)
#define C_LIVES       C2D_Color32(255, 102, 102, 255)
#define C_BTN_BG      C2D_Color32(20, 20, 36, 255)
#define C_BTN_BORDER  C2D_Color32(38, 38, 64, 255)
#define C_SELL_CONFIRM C2D_Color32(128, 26, 26, 255)
/* path gradient: t = i/path_len, 0..1
   r = 51 + 20*t, g = 31 + 10*t, b = 10 + 5*t (alpha 255) */
```

## Appendix B — Lua reference index

| Topic | `main.lua` lines |
|---|---|
| Window config / screen constants | 11-24 |
| Grid + offset calculation | 26-52 |
| Pathfinding (BFS, updatePath) | 54-123 |
| Tower types / enemy types / damage matrix | 125-179 |
| Global state variables | 181-226 |
| Sounds + helpers | 228-287 |
| Pixel-art icon functions | 289-435 |
| Helpers (grid pos, distance, getPathPosition, isPathCell, showError, speed text, getTotalCost) | 445-508 |
| startWave / random enemy type | 510-576 |
| Menu particles | 578-615 |
| love.update (full simulation) | 617-799 |
| Main menu drawing | 801-878 |
| Controls overlay | 880-918 |
| Game area drawing (grid, path, towers, lasers, enemies, cursor) | 920-1040 |
| HUD panel drawing | 1042-1267 |
| love.draw | 1269-1283 |
| love.keypressed (input + build state machine + economy) | 1285-1458 |
| love.touchpressed | 1460-1488 |
| love.load (window, fonts, audio, seed) | 1490-1519 |

| Sound | `main.lua` |
|---|---|
| loadSounds | 243-269 |
| playSound / playRandomUpgradeSound | 271-282 |
| menu/game music helpers | 284-287 |
