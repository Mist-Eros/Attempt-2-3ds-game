# Elemental Defense — 3DS Port Specification (v5)

**Single source of truth for the C implementation (devkitARM + libctru + citro2d).**

| Item | Value |
|---|---|
| Original LÖVE game | `C:\Users\Raine\Desktop\ElementalDefense\game\main.lua` (LÖVE2D, 1519 lines) — waves, economy, upgrades, maze/path behaviour |
| Combat design sheet | `docs/elements-spec.md` (v0.3 Borderlands-2-style damage model) |
| Current implementation | `include/game.h`, `include/gfx.h`, `source/core/{game,grid,data}.c`, `source/{render,ui,app,main,plat_3ds,audio}.c`, `tests/test_core.c`, `tools/sim.c`, `tools/preview/*` |
| 3DS target | top 400x240 (display), bottom 320x240 (touch), fixed 1/60 s simulation |
| This document | `Attempt-2-3ds-game/docs/design.md` |

**v5 changelog (this revision):** the combat model is completely replaced by the v0.3 **Borderlands-2-inspired** design: six damage types (normal, fire, electric, corrosive, slag, explosive), enemies made of 1–3 concentric **HP / SHIELD / ARMOR layers**, damage carried layer-to-layer with per-layer multipliers, status effects (burn, stun, acid puddle, slag), explosive AoE, and a six-tower roster. The LÖVE original's three-element rock-paper-scissors and `EnemySpec` are gone. The **maze/BFS game and all wave/economy/upgrade rules are unchanged** (v0.2 locks still hold). One deliberate balance deviation from `elements-spec.md`: a global enemy-HP multiplier of **2.0** (`kEnemyHpScale`, allowed by the spec's balance guardrail) so that mixed element armies matter — see §5.2 and §14.2. Every number below was checked against the shipped `source/core/data.c` + `game.c`, `tests/test_core.c` (46 tests) and the sim results in `source/core/NOTES.md`.

**Tagging convention**

- `[LUA]` — from the original LÖVE game (`main.lua`).
- `[SPEC]` — from `docs/elements-spec.md`.
- `[CODE]` — verified against the shipped C in this repo (file/function/constant named).
- `[BALANCE]` — deliberate tuning deviation, signed off by the balance guardrails in elements-spec §4.
- `[DERIVED]` — computed arithmetically from the above.

The Borderlands 2 comparison is inspiration only: **these numbers are our own design**, they were not extracted from BL2.

---

## 0. Scope, locked decisions, engine contract

### 0.1 Locked decisions (authoritative)

| # | Decision | Source |
|---|---|---|
| L1 | **Dynamic BFS maze restored.** 4-connected BFS from entrance `(0,12)` to exit `(25,12)` on run start and after every build/sell. Towers may stand on road cells; the road reroutes around them. | `[CODE]` `core_build_path()`; LÖVE original `main.lua:63-123` |
| L2 | **Entrance `(0,12)` and exit `(25,12)` are never buildable.** A build that leaves the exit unreachable is refused with `"that would block the path"`, costs nothing, mutates nothing. | `[CODE]` `game_place_tower()` / `game_can_place_at()` |
| L3 | **Enemies are re-projected** onto the new road after any reroute (nearest point, keep progress). | `[CODE]` `reproject_enemies()` |
| L4 | **Enemy movement is absolute cells/second**: `(20 + wave-1) * 0.25`, elite ×1.8. Longer mazes give real extra time under fire. | `[BALANCE]`/`[CODE]` `SPEED_SCALE`; deviation from the Lua's normalised speed |
| L5 | **25 waves**, 20 lives, every leak = −1 life and 0 gold, 0 lives -> game over, clear wave 25 -> victory. | `[CODE]`; v0.2 locks |
| L6 | **Build phase between waves**, player-paced NEXT WAVE; build/upgrade/sell also allowed mid-wave. No inter-wave timer. | `[CODE]` |
| L7 | **Borderlands-style combat** `[SPEC]`: six damage types (normal/fire/electric/corrosive/slag/explosive), enemies are 1–3 concentric layers of HP/SHIELD/ARMOR, per-layer multipliers, damage carry-over, status effects, explosive AoE. | `docs/elements-spec.md`; `[CODE]` |
| L8 | **Layer composition is deterministic and the preview is exact.** Layer count by wave, layer types from a hash of `(seed, wave, spawn slot)`; `game_wave_preview()` reports exactly what will spawn. | `[CODE]` `core_enemy_layer_types()` / `game_wave_preview()` |
| L9 | **Balance guardrail applied**: global enemy HP ×**2.0** (`kEnemyHpScale`), damage table/economy untouched. | `[BALANCE]`; evidence §14.6 |
| L10 | Speed levels 1x/2x/4x + separate pause overlay; L/R = tower type, Select = speed, Start = pause. | `[CODE]` |
| L11 | The six-tower/damage-type system is **our design**, inspired by BL2; the LÖVE original had only Fire/Water/Forest with identical stats. | `[LUA]` vs `[SPEC]` |

### 0.2 Conventions

- **Core is cell space** (`include/game.h`): 1.0 = one grid cell; cell `(col,row)` is 0-based, centre `(col+0.5, row+0.5)`.
- **Renderer is pixels** (`include/gfx.h`): top 400x240, bottom 320x240, top-left origin, +y down.
- Range: original 60 px / 28 px cell = **2.142857 cells**, constant for every tower and level.
- `pathPos` = distance travelled along `Game.path` in cells (0 .. `path.length`); every BFS step is one cell.
- Layer order: `layers[0]` is the **outermost** ring and takes damage first; damage carries inward.
- Status timers/stack counts are per enemy; puddles are per game (`Game.puddles`).
- Numbers are rounded only where stated; the C uses full `float` precision.

### 0.3 Engine contract — `include/game.h` (frozen; no API changes needed for v0.3)

| Item | Meaning / value |
|---|---|
| `TOWER_TYPE_COUNT` | **6** — the six damage-type towers are also the tower "types" |
| `DamageType` | `DMG_NORMAL=0`, `DMG_FIRE=1`, `DMG_ELECTRIC=2`, `DMG_CORROSIVE=3`, `DMG_SLAG=4`, `DMG_EXPLOSIVE=5` |
| `LayerType` | `LAYER_HP=0`, `LAYER_SHIELD=1`, `LAYER_ARMOR=2`; `MAX_LAYERS 3` |
| `Tower` | `damageType`, `level` 1..10, `col,row`, `cooldown` (s), `invested`, `animPhase`. No element/tower-name field — the damage type is the identity |
| `EnemyLayer` | `{ uint8_t type; float hp, maxHp; }` |
| `Enemy` | `elite`, `layerCount` 1..3, `layers[MAX_LAYERS]` (outermost first), `pathPos`, `x,y`, `hitFlash`, `burnStacks` 0..5, `burnTimer`, `stunTimer`, `slagTimer` |
| `Beam` | `damageType`, `kind` (0 = line beam, 1 = explosion ring), `x1,y1,x2,y2`, `radius` (cells, explosion only), `life/maxLife` |
| `Puddle` | `{ active, x, y, radius, life, maxLife, dps }`; `Game.puddles[MAX_PUDDLES=16]`, `Game.puddleNext` round-robin slot |
| `TowerSpec` | `name`, `cost`, `damage` (L1), `fireRate` (L1 cooldown seconds), `range` (cells), **`damageType`**, **`aoeRadius`** (0 = single-target), `color[3]`, `glow[3]` |
| `game_tower_stats()` | effective `damage`/`fireRate`/`range` at a level (range constant) |
| `game_damage_multiplier(damageType, layerType)` | 2.0 strong / 1.0 neutral / 0.5 weak; out-of-range clamps to 0 |
| `game_wave_preview(g, &hpLayers, &shieldLayers, &armorLayers, &elites)` | **total layer counts across the whole wave** (all spawns, elites included) + elite count; exact |
| `Game.layerQuota[3]` | staged at `game_start_next_wave()` from the same function as the preview; **informational only**, nothing in the sim reads it |
| `Game.rng` | the run seed (set by `game_new_run`) and **never advanced during a run** — layer composition is a pure function of it, which is what makes the preview exact |
| Removed from v0.2 | `Element`, `EnemySpec`, `game_enemy_spec()`. There is no enemy "element" any more; only layers |
| `MAX_TOWERS 96`, `MAX_ENEMIES 128`, `MAX_BEAMS 128`, `MAX_PUDDLES 16` | fixed pools, no dynamic allocation |
| `sizeof(Game)` | **18,960 bytes** (`[DERIVED]` host compile) — keep the single instance static/global, not on the stack |

Private core interface (`source/core/core_internal.h`): maze constants/queries (`CORE_START_*`, `CORE_GOAL_*`, `core_build_path`, `core_path_exists`, `core_path_length_with_block`, `core_path_contains`, `core_path_position`), status-effect consts (`core_burn_max_stacks = 5`, `core_burn_duration = 2.5`, `core_burn_dps_per_stack = 1.0`, `core_stun_duration = 0.30`, `core_puddle_radius = 0.7`, `core_puddle_life = 5.0`, `core_puddle_dps = 2.0`, `core_slag_duration = 4.0`, `core_slag_damage_amp = 1.5`, `core_slag_speed_mult = 0.65`, `core_explosion_life = 0.30`), wave formulas, `core_enemy_layer_count`, `core_spawn_slot_is_elite`, `core_enemy_layer_types`, `core_enemy_speed_cells`.

### 0.4 The game in one paragraph

A 26x13 maze tower defense with 25 waves and 20 lives. Creeps enter at `(0,12)` and walk the current BFS-shortest road to the exit at `(25,12)`; towers may be built **on** the road, forcing a detour (mazing is the core strategy). Every enemy is a stack of one to three concentric **HP / SHIELD / ARMOR** layers (waves 1–3: one layer, 4–8: two, 9+: three; elites get one extra). Six damage-type towers — **GUN** (normal), **FIRE** (burn), **SHOCK** (stun), **CORROSIVE** (acid puddle), **SLAG** (amplifier/slow), **BLASTER** (explosive AoE) — each is strong vs one layer type (2x), weak vs one (0.5x), neutral otherwise; slag makes everything take +50 % damage. Kills pay 10 gold; start 150 gold; towers upgrade 10 levels (damage doubles per level). Clear wave 25 to win.

---

## 1. Game loop & state machine

### 1.1 Step order `[CODE]` (`game_step`)

`game_update(g, realDt)` clamps real time to 0.25 s, accumulates and runs at most 6 fixed `1/60 s` steps; leftover is dropped (no catch-up spiral). `game_step(dt)` is one deterministic step and multiplies `dt` by `speedMultiplier` (1/2/4).

```
game_step(dt):
  clamp dt to [0, 0.25]
  tick_message(dt)                      # wall-clock; expires while paused/at 4x
  if paused: return
  if phase not in {BUILD, WAVE}: return
  sdt = dt * speedMultiplier
  timeTotal += sdt; update_beams(sdt)
  if phase == BUILD:
      update_tower_timers(sdt)          # cooldowns/animations tick; puddles freeze
      return
  # PHASE_WAVE
  waveTime += sdt
  1. update_spawning(sdt)               # queue -> enemies (layer composition hash)
  2. update_status_effects(sdt)         # burn ticks, slag timer, puddle lifetime+ticks
  3. update_enemies(sdt)                # stun freezes, slag slows, leaks cost a life
  4. compact_enemies(); update_towers(sdt)   # target, fire, AoE, on-hit statuses
  5. compact_enemies(); check_wave_complete()
```

Notes:
- Status damage (burn, acid) can kill, and kill rewards/events fire the same as direct hits.
- Puddles only simulate in WAVE; they are frozen during BUILD/game over/victory.
- No path rebuild happens in the step loop; only build/sell can change the road (§2).
- Enemy compaction preserves spawn order, which is the targeting tie-break order ("oldest first").

### 1.2 Original LÖVE behaviour (reference)

The LÖVE update order was `dt*gameSpeed -> spawn -> move -> towers fire -> completion`; wave auto-chained, no lives, leaks paid gold (`main.lua:617-799`). The port keeps the order and replaces the end-state logic (L5/L6).

### 1.3 Phase machine `[CODE]`

| Phase | Entered by | Simulation | Player actions |
|---|---|---|---|
| `PHASE_MENU` | `game_init` | none (renderer menu particles) | A/Start/tap START -> `game_new_run` |
| `PHASE_BUILD` | `game_new_run`; wave < 25 resolved | no enemies; tower timers/animations tick; puddles frozen | build/upgrade/sell/type/cursor/NEXT WAVE/pause |
| `PHASE_WAVE` | NEXT WAVE (BUILD, not paused) | full step | same actions (mid-wave building allowed) |
| `PHASE_GAMEOVER` | lives hit 0 on a leak | frozen | A/tap PLAY AGAIN -> new run; B/tap MAIN MENU -> menu |
| `PHASE_VICTORY` | wave counter passes 25 | frozen | same result screen |

- `game_start_next_wave()` is ignored outside BUILD or while paused.
- `check_wave_complete()`: when `enemiesToSpawn == 0 && enemiesAlive == 0`, `++wave`; if `wave > totalWaves` -> VICTORY + `GAME_EVT_VICTORY`, else BUILD. After clearing wave 25 `wave == 26`; renderers clamp display to `min(wave, 25)`.
- `game_new_run(g, seed)`: fresh run in BUILD, wave 1, gold 150, lives 20, totalWaves 25, speed 1, BFS road built, `selectedType = DMG_NORMAL`, `selectedTower = -1`, cursor `(0,0)`, `rng = seed` (0 -> fixed non-zero).
- Pause: `game_toggle_pause()` freezes the battlefield, resets `stepAccumulator` (no unpause burst); message timers keep ticking. Pause is allowed in BUILD/WAVE only.
- Speed: `game_cycle_speed()` 1 -> 2 -> 4 -> 1; shown `1X`/`2X`/`4X`, yellow when >1x.

---

## 2. Grid & dynamic pathfinding (maze mechanic — unchanged)

### 2.1 Grid constants

| Constant | Value |
|---|---|
| `GRID_COLS`, `GRID_ROWS` | 26, 13; cell centres `(col+0.5,row+0.5)` |
| Entrance / spawn | `(0,12)` = LÖVE `(1,13)`; spawn centre `(0.5,12.5)` |
| Exit / leak point | `(25,12)` = LÖVE `(26,13)`; exit centre `(25.5,12.5)` |
| Initial path (empty board) | **26 cells / 25 segments**, straight row 12, left to right |
| Path storage | `Game.path.pts[]` cell centres, `count`, `length = count-1` cells; max `MAX_PATH_CELLS = 338` |
| Top screen cell size | 15 px; board origin `(5, 38)` -> 390x195 px; 38 px HUD strip above, 7 px margin below |
| Range | 2.142857 cells; drawn radius 32 px |

### 2.2 BFS `[CODE]` (`source/core/grid.c`)

4-connected BFS entrance -> exit, exactly the LÖVE rules: FIFO queue, neighbour order **up, down, left, right**, uniform cost 1, occupied cells are walls, visited at enqueue, parent walk from goal back to start. Path cells are written as cell centres, so every segment is exactly 1 cell long.

- Empty board: `(0,12),(1,12),...,(25,12)` — 26 cells / length 25.
- A single tower on the road at `(7,12)` -> length **27** (detour via row 11/13).
- Deterministic: same board -> same path; `MAX_PATH_CELLS 338` covers a full-board snake.
- `game_can_place_at()` runs the same BFS on a **hypothetical** board (candidate cell treated as a wall) without mutating the game, so the UI can colour the cursor tile every frame.

### 2.3 Rebuild triggers `[CODE]`

| Trigger | Where |
|---|---|
| Run start | `game_new_run()` -> `core_build_path()` |
| Successful placement | `game_place_tower()`: commit tower/gold/occupancy, then rebuild + re-project |
| Defensive rollback | if the post-commit rebuild somehow fails: undo tower/gold/occupancy, rebuild, re-project, message `"that would block the path"` |
| Sale | `game_sell_selected()`: compact towers, rebuild occupancy, rebuild road, re-project |
| Never | the step loop — no BFS during gameplay ticks |
| Cursor UI | `game_can_place_at()` per frame (hypothetical only) |

`Game.pathBlocked` stays `false` in normal play; it is only set if a rebuild finds no route (corrupted board), and nothing in the game loop reads it.

### 2.4 Placement rules `[CODE]`

Check order (first failure wins; failure sets the 1.5 s `Game.message` and changes nothing):

1. phase BUILD/WAVE -> `"can't build now"`
2. in bounds -> `"can't build there"`
3. not entrance `(0,12)` -> `"can't build on the entrance"`
4. not exit `(25,12)` -> `"can't build on the exit"`
5. cell empty -> `"cell occupied"`
6. `towerCount < MAX_TOWERS (96)` -> `"tower limit"`
7. exit still reachable with the candidate as a wall -> `"that would block the path"`
8. `gold >= cost` -> `"not enough gold"`

Success: gold deducted, tower created at level 1 (`invested = cost`, `cooldown = 0`), occupancy set, tower auto-selected, road rebuilt, enemies re-projected, `GAME_EVT_BUILD`.

`game_can_place_at()` is the pure version (checks 1–7 but **not** gold) used for the cursor tile: green = legal + affordable, yellow = legal but unaffordable, white = tower, dim = blocked.

### 2.5 Enemy re-projection `[CODE]` (`source/core/game.c`)

After every rebuild, each live enemy is moved to the nearest point on the new polyline: per-segment clamped projection, `pathPos = segmentIndex + t`, then `x,y = path_position(pathPos)`. Tie-break: first segment (strict `<`). Enemies keep walking from where they stand; never snapped back to the entrance. A shorter road (sell) re-projects them forward along the shortcut.

### 2.6 Renderer rule

The road can change on any build/sell, including mid-wave: **read `g->path.pts` / `g->path.count` every frame; never cache the polyline.** Entrance/exit arrows are drawn from `path.pts[0]` / `path.pts[count-1]` and move when the road reroutes.

### 2.7 Performance

BFS uses stack arrays only: `visited[13][26]` + `parent[338]` + `queue[338]` (~1.7 KB), <=338 cells x 4 neighbours. It runs once per build/sale plus the per-frame cursor query — negligible against rendering, and far cheaper than the Lua original's table BFS + double validation.

---

## 3. Towers (six damage types)

### 3.1 Roster `[SPEC]` + `[CODE]` (`source/core/data.c`)

All towers share range **2.142857 cells**. Damage doubles per upgrade level; the cooldown follows the shared per-level table, except BLASTER which is **3x** that table. `TowerSpec.name` is the internal name; the bottom-screen UI displays the damage-type name (see §9.5: UI shows `EXPLOSIVE`, not `BLASTER`).

| # | Spec name | Damage type | Cost | DMG L1 | Cooldown L1 | Effect | AoE | Colour `(r,g,b)` | Glow |
|---|---|---|---|---|---|---|---|---|---|
| 0 | GUN | normal | 20 | 3 | 0.80 s | none | — | (0.75,0.75,0.78) | (0.45,0.45,0.50) |
| 1 | FIRE | fire | 20 | 3 | 0.80 s | +1 burn stack (max 5, 2.5 s, 1 dps/stack) | — | (0.95,0.45,0.12) | (0.95,0.30,0.05) |
| 2 | SHOCK | electric | 20 | 3 | 0.80 s | stun 0.30 s | — | (0.25,0.80,1.00) | (0.10,0.60,1.00) |
| 3 | CORROSIVE | corrosive | 20 | 3 | 0.80 s | acid puddle (r 0.7, 5 s, 2 dps) | — | (0.45,0.85,0.15) | (0.30,0.80,0.10) |
| 4 | SLAG | slag | 15 | 1 | 0.80 s | slag 4 s: +50 % damage taken, ×0.65 speed | — | (0.70,0.25,0.90) | (0.60,0.15,0.85) |
| 5 | BLASTER | explosive | 30 | 9 | **2.40 s** (3x) | AoE blast radius **1.3 cells** | 1.3 | (1.00,0.70,0.15) | (1.00,0.50,0.05) |

Single-target DPS at L1 (neutral): GUN/FIRE/SHOCK/CORROSIVE/BLASTER = **3.75**, SLAG = **1.25**. At L10 (neutral): 5,485.7 for the 3-damage towers and BLASTER, SLAG 1,828.6 (`[DERIVED]`).

### 3.2 Firing `[CODE]`

- A tower fires when `cooldown <= 0` and a target is in range; on firing `cooldown = fireRate(level)`.
- Damage is hitscan (instant); each shot spawns a `Beam`:
  - `kind 0` (GUN/FIRE/SHOCK/CORROSIVE/SLAG): line from tower centre to the target position, `damageType` set for colour, life 0.15 s (`CORE_BEAM_LIFE`).
  - `kind 1` (BLASTER): explosion ring centred on the impact point, `radius = aoeRadius` (1.3), life 0.30 s (`core_explosion_life`).
- `MAX_BEAMS 128`; if full, a shot still deals damage and the cosmetic beam is dropped.
- BLASTER damages **every** enemy within 1.3 cells of the target's position (including the target) for its full damage; there is no falloff. Each victim applies its own layer multipliers + slag amp.
- On-hit status effects are applied **only to the surviving target of a direct hit**. Burn/acid ticks never re-apply statuses; explosions apply none (their damage type has no on-hit effect).
- `GAME_EVT_LASER` is set for every shot (the audio layer will rate-limit; §12).

### 3.3 Targeting `[CODE]` (`find_target`)

Default is the **nearest** enemy in range. Elemental towers prefer a target whose **outermost layer** matches their strength; if none in range has that outer layer, they fall back to nearest. Ties (strict `<`) go to the **oldest** enemy (lowest array index = earliest spawned).

| Tower | Prefers outer layer |
|---|---|
| FIRE | HP (red) |
| SHOCK | SHIELD (blue) |
| CORROSIVE | ARMOR (yellow) |
| GUN / SLAG / BLASTER | nearest (no preference) |

Only the outermost layer counts for preference; an inner layer matching the element does not attract targeting.

### 3.4 Per-level scaling `[CODE]` (`game_tower_stats`)

```
damage   = spec.damage * (1 << (level-1))          // doubles per level
fireRate = cooldownTable[level-1] * (spec.fireRate / 0.80)   // BLASTER scale = 3, others 1
range    = spec.range                              // 60/28 cells, never upgrades
```

Shared cooldown table (seconds): **0.80, 0.72, 0.64, 0.57, 0.51, 0.45, 0.40, 0.36, 0.32, 0.28**; BLASTER = ×3 (2.40 … 0.84).

| Level | Damage × | Damage (3-dmg towers) | Cooldown (normal) | DPS (normal) | Damage (SLAG) | Damage (BLASTER) | Cooldown (BLASTER) | Upgrade cost | Total invested | Sell (75 %) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 3 | 0.80 | 3.75 | 1 | 9 | 2.40 | 20 | 20 | 15 |
| 2 | 2 | 6 | 0.72 | 8.33 | 2 | 18 | 2.16 | 40 | 40 | 30 |
| 3 | 4 | 12 | 0.64 | 18.75 | 4 | 36 | 1.92 | 80 | 80 | 60 |
| 4 | 8 | 24 | 0.57 | 42.11 | 8 | 72 | 1.71 | 160 | 160 | 120 |
| 5 | 16 | 48 | 0.51 | 94.12 | 16 | 144 | 1.53 | 320 | 320 | 240 |
| 6 | 32 | 96 | 0.45 | 213.33 | 32 | 288 | 1.35 | 640 | 640 | 480 |
| 7 | 64 | 192 | 0.40 | 480.00 | 64 | 576 | 1.20 | 1280 | 1280 | 960 |
| 8 | 128 | 384 | 0.36 | 1066.67 | 128 | 1152 | 1.08 | 2560 | 2560 | 1920 |
| 9 | 256 | 768 | 0.32 | 2400.00 | 256 | 2304 | 0.96 | 5120 | 5120 | 3840 |
| 10 | 512 | 1536 | 0.28 | 5485.71 | 512 | 4608 | 0.84 | — (MAX) | 10240 | 7680 |

An upgrade does **not** reset the tower's current cooldown, and range never changes.

### 3.5 Tower glyphs `[CODE]` (renderer, `art_tower_icon`)

Six pixel-art glyphs, drawn in the damage-type colour at radius 7 (L1–4) / 8 (L5–9) / 9 (L10), with an element glow and a 1 px level bar at the bottom of the cell. GUN = grey bullet with a driving band; FIRE = flame polygon (same shape as the original game); SHOCK = Z-shaped lightning bolt; CORROSIVE = dripping acid blob; SLAG = purple crystal shard; BLASTER = dark bomb with a lit fuse. At level 10 the tower gets a small gold cap instead of the normal level indicator styling.

### 3.6 Action API `[CODE]`

| API | Behaviour / errors |
|---|---|
| `game_place_tower` | uses `selectedType`; auto-selects the placed tower; BUild event; errors as in §2.4 |
| `game_can_place_at` | legality incl. hypothetical BFS; no gold check; no messages |
| `game_upgrade_selected` | `"can't upgrade now"` / `"no tower selected"` / `"max level"` / `"not enough gold"`; adds cost to `invested`, emits UPGRADE |
| `game_sell_selected` | `"can't sell now"` / `"no tower selected"`; refund `invested*3/4`; removes/compacts tower, rebuilds occupancy + road, re-projects, clears selection, emits SELL |
| `game_select_tower`, `game_set_selected_type`, `game_set_cursor` | as named; invalid values ignored/clamped |

---

## 4. Damage model & status effects

### 4.1 Damage types `[SPEC]` `[CODE]`

| Type | Id | Flavour | Effect (on direct hits) | Strong vs | Weak vs |
|---|---|---|---|---|---|
| normal | 0 | GUN | none | — (1.0 everywhere) | — |
| fire | 1 | FIRE | +1 burn stack (max 5), burn 1 dps/stack, 2.5 s, fire multipliers | HP | SHIELD, ARMOR |
| electric | 2 | SHOCK | stun 0.30 s (enemy stops moving) | SHIELD | HP, ARMOR |
| corrosive | 3 | CORROSIVE | drops an acid puddle at the target (0.7 cells, 5 s, 2 dps, corrosive multipliers) | ARMOR | HP, SHIELD |
| slag | 4 | SLAG | slag 4 s: damage taken ×1.5 and speed ×0.65 | — (low base damage) | — |
| explosive | 5 | BLASTER | AoE 1.3 cells, neutral multipliers; 3x slower cooldown, 3x damage | — | — |

### 4.2 Damage multiplier matrix `[SPEC]` `[CODE]` (`kDamageMultiplier`)

| damage type \ layer | HP | SHIELD | ARMOR |
|---|---|---|---|
| normal | 1.0 | 1.0 | 1.0 |
| fire | **2.0** | 0.5 | 0.5 |
| electric | 0.5 | **2.0** | 0.5 |
| corrosive | 0.5 | 0.5 | **2.0** |
| slag | 1.0 | 1.0 | 1.0 |
| explosive | 1.0 | 1.0 | 1.0 |

Slag amplification: while `slagTimer > 0`, every damage source (direct, burn tick, acid tick, explosion) is multiplied by **1.5** on top of the layer multiplier. `game_damage_multiplier()` returns the base matrix only; the amp is applied inside `apply_damage()`.

### 4.3 Damage resolution `[CODE]` (`apply_damage`)

```
pool = raw damage; amp = slag ? 1.5 : 1.0
while pool > 0 and layers remain:
    mult = game_damage_multiplier(type, layers[0].type) * amp
    if pool * mult < layers[0].hp:
        layers[0].hp -= pool * mult; pool = 0
    else:
        pool -= layers[0].hp / mult     # unspent RAW damage carries inward
        destroy layers[0]; shift the layer stack inward (layerCount--)
if layerCount == 0: enemy dies (+10 gold, kills++, ENEMY_DIE event)
```

Consequences:
- Carry-over is computed in **raw** damage, so the next layer applies **its own** multiplier (e.g. a fire hit that breaks an HP layer then hits SHIELD at 0.5x).
- The slag ×1.5 applies to every layer and every source, including the slag tower's own hits while the target is already slagged.
- Killing a layer shows the next layer immediately (outer ring disappears); the health bar/ring fraction is per current outer layer only.

Worked examples `[CODE]` tests:
- GUN 3 raw into `[HP 5, ARMOR 100]`: first shot leaves HP 2; second shot spends 2 raw to break HP and carries 1 raw -> ARMOR takes 1 (`99`).
- FIRE 3 raw into `[HP 3, SHIELD 100]`: HP layer absorbs `3*2.0 = 6` scaled but only had 3 hp, so it eats `3/2.0 = 1.5` raw; the other 1.5 carries and does `1.5*0.5 = 0.75` to SHIELD -> hp 99.25; the surviving target gets its first burn stack.
- Slagged `[HP 3]` hit by GUN 3: multiplier 1.0*1.5 -> 4.5 damage -> kill and +10 gold.

### 4.4 Status effects `[SPEC]` `[CODE]`

| Effect | Trigger | Numbers | Tick / interaction |
|---|---|---|---|
| Burn | every FIRE direct hit on a surviving target | stacks `min(5, +1)`, `burnTimer = 2.5 s` (refresh, not stack timer) | Each step: `stacks * 1.0 dps * dt` **fire** damage to the outer layer (fire multipliers + slag amp), then `burnTimer -= dt`; at 0 stacks clear |
| Stun | every SHOCK direct hit | `stunTimer = max(stunTimer, 0.30)` | While > 0 the enemy does not advance at all; no other damage effect |
| Acid puddle | every CORROSIVE direct hit | radius 0.7 cells, life 5 s, 2.0 dps **corrosive**; max 16 live | Ticks in the status phase against every enemy inside (own layer multiplier + slag amp); when full the round-robin `puddleNext` slot is replaced |
| Slag | every SLAG direct hit | `slagTimer = 4 s` (refresh) | Damage taken ×1.5 from all sources, movement ×0.65 |

Effective status DPS vs each outer layer (before slag amp):

| Source | vs HP | vs SHIELD | vs ARMOR |
|---|---|---|---|
| Burn, 1 stack (1.0 raw dps) | 2.0 | 0.5 | 0.5 |
| Burn, 5 stacks (5.0 raw dps) | **10.0** | 2.5 | 2.5 |
| Acid puddle (2.0 raw dps) | 1.0 | 1.0 | **4.0** |

Slag amp multiplies all of these by 1.5. Stun freezes movement for 0.30 s: about 1.5 cells skipped at wave 1 (5 cells/s), 3.3 cells at wave 25 regular (11 cells/s), 5.94 cells for a wave-25 elite (19.8 cells/s).

`[CODE]` details:
- `burnStacks` is `uint8_t`; per-hit cap 5, independent of burn timer.
- Statuses are attached to the enemy, not the tower, and are cleared when the enemy dies.
- Burn/acid ticks can kill; kill rewards/events are identical to direct kills.
- All status timers tick in the status phase, i.e. **before** movement and before towers fire in the same step.

### 4.5 Explosive (BLASTER) `[SPEC]` `[CODE]`

- Cooldown 3x the shared table (2.40 s at L1), damage 3x a normal L1 tower (9 at L1), both scaling with level (damage doubles, cooldown uses the 3x table).
- Picks a target with the normal rules (nearest), then damages **every enemy within 1.3 cells of the target position** for full damage; each enemy applies its own outer-layer multipliers and slag amp. AoE damage is applied before the beam/visual is created.
- Visual: expanding explosion ring (kind 1) centred on the impact point.
- Burst/DPS choice `[BALANCE]`: single-target DPS is deliberately equal to a 3-damage tower (3.75 at L1) — the BLASTER pays for its AoE with cooldown, not with lower throughput; its value is hitting clumps and layer-stacked waves.

### 4.6 Design note (BL2 inspiration)

The six damage types, three defensive layer types, "strong/weak" multipliers, slag amplify/slow and elemental DoT are directly inspired by Borderlands 2, but **all values here are our own** (chosen for a 25-wave tower defense on a 26x13 board). Nothing is copied from BL2's tables.

---

## 5. Enemies: layered health

### 5.1 Layers `[SPEC]` `[CODE]`

An enemy is `layerCount` concentric layers, `layers[0]` outermost. Each layer is one of:

| Layer | Colour (renderer) | Meaning |
|---|---|---|
| HP | red (0.95,0.18,0.18) | plain health |
| SHIELD | blue (0.28,0.55,1.00) | energy shield |
| ARMOR | yellow (0.95,0.78,0.18) | plated armour |

- **Layer count schedule** (`core_enemy_layer_count`): waves **1–3 = 1 layer**, waves **4–8 = 2 layers**, waves **9+ = 3 layers**; **elites get +1 layer, capped at 3**. So elite layer counts: waves 1–3 = 2, waves 4–8 = 3, waves 9+ = 3.
- **Layer types** (`core_enemy_layer_types`): a deterministic hash of `(seed = Game.rng, wave, spawnSlot)`:
  - 1 layer: one type from `hash % 3`;
  - 2 layers: one of the 6 **ordered pairs of distinct types** (`hash % 6`);
  - 3 layers: one of the 6 permutations of all three types (`hash % 6`).
  Distinctness is guaranteed by construction: 2-layer enemies never repeat a type, 3-layer enemies always have exactly one of each.
- Spawn slots: `core_spawn_slot_is_elite(slot, E)` = `slot >= 1 && slot <= E` (slot 0 regular, the E elites are consecutive, then regulars) — the same net effect as the LÖVE game.
- `Game.rng` stays equal to the run seed: **nothing consumes the RNG during a run**, which is why `game_wave_preview()` can be exact without touching the simulation.

### 5.2 HP `[SPEC]` + `[BALANCE]` `[CODE]`

```
totalHP(wave, elite) = (15 + (wave-1)*3) * 2.0 * (elite ? 2.5 : 1.0)
perLayerHP           = totalHP / layerCount          // split equally
```

`kEnemyHpScale = 2.0f` in `data.c` is the one deliberate balance deviation from `elements-spec.md` (which specifies the un-scaled formula). It is explicitly allowed by the spec's §4 guardrail ("adjust enemy HP/speed first, not the damage table"); evidence and the rejected alternatives are in §14.6. Setting it to `1.0f` restores the literal spec numbers.

Examples (`[DERIVED]`):

| Wave | Layers reg/elite | Regular total HP / per layer | Elite total HP / per layer |
|---|---|---|---|
| 1 | 1 / 2 | 30.0 / **30.00** | 75.0 / 37.50 |
| 4 | 2 / 3 | 48.0 / **24.00** | 120.0 / 40.00 |
| 5 | 2 / 3 | 54.0 / **27.00** | 135.0 / 45.00 |
| 9 | 3 / 3 | 78.0 / **26.00** | 195.0 / 65.00 |
| 10 | 3 / 3 | 84.0 / **28.00** | 210.0 / 70.00 |
| 20 | 3 / 3 | 144.0 / **48.00** | 360.0 / 120.00 |
| 25 | 3 / 3 | 174.0 / **58.00** | 435.0 / 145.00 |

Elite HP multiplier is applied before splitting, so an elite's layers are 2.5x a regular's per layer.

### 5.3 Movement `[CODE]`

- Absolute speed: `speed_cells = (20 + (wave-1)) * 0.25`, elite ×1.8 (`SPEED_SCALE = 0.25`). Wave 1 = 5 cells/s, wave 10 = 7.25, wave 25 = 11.0, wave-25 elite = 19.8 cells/s.
- Slagged: speed ×**0.65**; stunned: speed 0 (no `pathPos` change at all; stun timer decays).
- Leak at `pathPos >= path.length`: enemy removed, lives −1, leaks++, no gold, `GAME_EVT_LEAK`; at 0 lives -> GAMEOVER + `GAME_EVT_GAMEOVER`.
- Road changes re-project `pathPos` (§2.5); enemies never snap to the entrance.
- Travel time vs road length (mazing): `seconds = path length in cells / speed` — 25 cells: 5.00/3.45/2.27 s at waves 1/10/25 regular (elite 2.78/1.92/1.26); 60 cells: 12.00/8.28/5.45 s; 100 cells: 20.00/13.79/9.09 s. Every extra cell of detour is extra time under fire.

### 5.4 Enemy rendering `[CODE]` (`render.c draw_enemies`)

- Concentric rings **outside-in** at radii **6.5 / 4.6 / 2.7 px** (15 px cell), thickness **2 px**, 12 segments each, colour = layer colour with alpha `0.45 + 0.55*hpFraction`; destroyed layers are not drawn.
- A filled core dot (r 2.3 px) in the current outer layer's colour marks the active layer.
- Elite: extra gold glow (r 14) + white outline ring r 7.5, alpha 0.64.
- Hit flash: white filled circle r 6.2, alpha up to 0.80, decays with `hitFlash` (0.12 s).
- Slag: pulsing purple outline ring r 8.4 (alpha 0.45–0.80, sine).
- Stun: pulsing white-blue filled circle r 6.5 (alpha 0.16–0.42).
- Burn: up to 5 orange 2x2 px motes orbiting the enemy (one per stack).
- The design-spec's 6.0/4.2/2.4 radii were nudged to 6.5/4.6/2.7 with 2 px thickness because 1.5 px rings read poorly on the 240p LCD; layer colours are exactly the spec's.

### 5.5 Kill / leak rewards

Kill: +10 gold, `kills++`, `enemiesAlive--`, `GAME_EVT_ENEMY_DIE`, beam/status cleanup by removal. Leak: −1 life, no gold. Both remove the enemy from the array preserving order.

### 5.6 Capacity

`MAX_ENEMIES 128`; maximum wave size is 53 spawns (wave 25), waves never overlap, so no overflow. If the array is full, spawning pauses (timer clamped) and resumes; no enemy is silently lost. `peek` of layers doesn't change this.

---

## 6. Waves (25)

### 6.1 Formulas `[CODE]` (`data.c`)

```
spawns(w)    = 3 + 2*w                                   // INCLUDING elites [v0.2 lock]
E(w)         = 10 if w%25==0; 3 if w%10==0; 1 if w%5==0; else 0
interval(w)  = max(0.8, 3.0 - 0.05*w)                    // seconds
totalHP(w,e) = (15 + (w-1)*3) * 2.0 * (e ? 2.5 : 1)      // kEnemyHpScale = 2.0
layers(w,e)  = (w<=3 ? 1 : w<=8 ? 2 : 3) + (e ? 1 : 0), capped at 3
perLayerHP   = totalHP / layers
speed(w,e)   = (20 + (w-1)) * (e ? 1.8 : 1) * 0.25        // cells/s
```

- Spawn slot 0 regular; slots 1..E elites; the rest regular.
- First spawn one interval after NEXT WAVE; one spawn per interval (the timer subtracts the interval).
- Layer types/HP are pure functions of `(seed, wave, slot, elite)` — preview is exact (§6.6).

### 6.2 Wave table `[DERIVED]` (`HP scale 2.0` applied)

`E` = elites; `layers` = per-enemy layer count (regular/elite); `totLayers` = total layer count of the wave; `HP reg/elite` = total HP per enemy; `perLayer reg/elite`; `waveHP` = total HP of all spawns; `gold` = max kill income (10/spawn).

| Wave | spawns | E | layers reg/elite | totLayers | HP reg/elite | perLayer reg/elite | waveHP | gold |
|---:|---:|---:|---|---:|---|---|---|---:|
| 1 | 5 | 0 | 1/2 | 5 | 30.0 / 75.0 | 30.00 / 37.50 | 150 | 50 |
| 2 | 7 | 0 | 1/2 | 7 | 36.0 / 90.0 | 36.00 / 45.00 | 252 | 70 |
| 3 | 9 | 0 | 1/2 | 9 | 42.0 / 105.0 | 42.00 / 52.50 | 378 | 90 |
| 4 | 11 | 0 | 2/3 | 22 | 48.0 / 120.0 | 24.00 / 40.00 | 528 | 110 |
| 5 | 13 | 1 | 2/3 | 27 | 54.0 / 135.0 | 27.00 / 45.00 | 783 | 130 |
| 6 | 15 | 0 | 2/3 | 30 | 60.0 / 150.0 | 30.00 / 50.00 | 900 | 150 |
| 7 | 17 | 0 | 2/3 | 34 | 66.0 / 165.0 | 33.00 / 55.00 | 1122 | 170 |
| 8 | 19 | 0 | 2/3 | 38 | 72.0 / 180.0 | 36.00 / 60.00 | 1368 | 190 |
| 9 | 21 | 0 | 3/3 | 63 | 78.0 / 195.0 | 26.00 / 65.00 | 1638 | 210 |
| 10 | 23 | 3 | 3/3 | 69 | 84.0 / 210.0 | 28.00 / 70.00 | 2310 | 230 |
| 11 | 25 | 0 | 3/3 | 75 | 90.0 / 225.0 | 30.00 / 75.00 | 2250 | 250 |
| 12 | 27 | 0 | 3/3 | 81 | 96.0 / 240.0 | 32.00 / 80.00 | 2592 | 270 |
| 13 | 29 | 0 | 3/3 | 87 | 102.0 / 255.0 | 34.00 / 85.00 | 2958 | 290 |
| 14 | 31 | 0 | 3/3 | 93 | 108.0 / 270.0 | 36.00 / 90.00 | 3348 | 310 |
| 15 | 33 | 1 | 3/3 | 99 | 114.0 / 285.0 | 38.00 / 95.00 | 3933 | 330 |
| 16 | 35 | 0 | 3/3 | 105 | 120.0 / 300.0 | 40.00 / 100.00 | 4200 | 350 |
| 17 | 37 | 0 | 3/3 | 111 | 126.0 / 315.0 | 42.00 / 105.00 | 4662 | 370 |
| 18 | 39 | 0 | 3/3 | 117 | 132.0 / 330.0 | 44.00 / 110.00 | 5148 | 390 |
| 19 | 41 | 0 | 3/3 | 123 | 138.0 / 345.0 | 46.00 / 115.00 | 5658 | 410 |
| 20 | 43 | 3 | 3/3 | 129 | 144.0 / 360.0 | 48.00 / 120.00 | 6840 | 430 |
| 21 | 45 | 0 | 3/3 | 135 | 150.0 / 375.0 | 50.00 / 125.00 | 6750 | 450 |
| 22 | 47 | 0 | 3/3 | 141 | 156.0 / 390.0 | 52.00 / 130.00 | 7332 | 470 |
| 23 | 49 | 0 | 3/3 | 147 | 162.0 / 405.0 | 54.00 / 135.00 | 7938 | 490 |
| 24 | 51 | 0 | 3/3 | 153 | 168.0 / 420.0 | 56.00 / 140.00 | 8568 | 510 |
| 25 | 53 | 10 | 3/3 | 159 | 174.0 / 435.0 | 58.00 / 145.00 | 11832 | 530 |

Run totals `[DERIVED]`: **725 spawns**, **2,059 layers**, **725 kills -> 7,250 gold**, **93,438 total wave HP**; +150 start = 7,400 max lifetime gold. Wave 25 spawn span = `(53-1)*1.75 = 91 s`.

### 6.3 Layer-type composition and preview

- Waves 1–3 (1 layer): each enemy is a single random type (hash). Per-type wave totals vary with seed but sum to the spawn count.
- Waves 4–8 (2 layers): each enemy has two different types; per-type totals vary with seed and sum to twice the spawn count.
- Waves 9+ (3 layers): every enemy is HP+SHIELD+ARMOR in some order, so per-type totals are **exactly the spawn count each** (e.g. wave 9: 21/21/21; wave 25: 53/53/53 + 10 elites).
- Elites: same type rules for their layer count (elites are capped at 3, so all elite layers are one of each from wave 9+; in waves 1–8 an elite has more layers than its regular wave-mates — e.g. wave 5 elite = 3 layers vs 2 for regulars).
- `game_wave_preview()` returns total HP/SHIELD/ARMOR layer counts + elite count across the whole wave (`layerCount` summed over every spawn). It is a pure function of `(Game.rng, wave)` and exactly matches what the spawner creates; `Game.layerQuota[]` is filled with the same numbers at wave start for convenience (informational only).
- Example preview for the deterministic preview seed `0x5EED1234` (the preview harness default) `[DERIVED]`:

| Wave | HP | SHIELD | ARMOR | ELITE | sum |
|---:|---:|---:|---:|---:|---:|
| 1 | 2 | 2 | 1 | 0 | 5 |
| 3 | 5 | 1 | 3 | 0 | 9 |
| 4 | 7 | 7 | 8 | 0 | 22 |
| 5 | 8 | 10 | 9 | 1 | 27 |
| 8 | 12 | 14 | 12 | 0 | 38 |
| 9 | 21 | 21 | 21 | 0 | 63 |
| 10 | 23 | 23 | 23 | 3 | 69 |
| 15 | 33 | 33 | 33 | 1 | 99 |
| 20 | 43 | 43 | 43 | 3 | 129 |
| 25 | 53 | 53 | 53 | 10 | 159 |

Real runs use their own seed (entropy from the 3DS clock via `main.c`), so 1/2-layer wave totals vary run to run; the 3-layer waves (9+) are always evenly split, and the preview is always exactly what will spawn.

### 6.4 Spawn internals `[CODE]`

- Wave start: phase WAVE, `waveTime = 0`, `spawnTimer = 0`, `enemiesToSpawn = spawns(w)`, `spawnInterval = interval(w)`, `elitePending = E`, `eliteDelayCount = E`, `enemiesAlive = 0`, selected tower cleared, `layerQuota` staged from the preview, `GAME_EVT_WAVE_START`.
- Each fixed step in WAVE: `spawnTimer += sdt`; while `spawnTimer >= interval` and spawns remain: subtract interval, compute `spawnSlot = spawns(w) - enemiesToSpawn`, `elite = slot in 1..E`, spawn (layer composition hash, HP split), decrement counter.
- If `enemiesAlive >= MAX_ENEMIES`, spawning pauses (timer clamped to one interval) and resumes later.
- Waves never overlap; there is no inter-wave timer (BUILD is player-paced).

### 6.5 Completion / victory / game over `[CODE]`

`check_wave_complete()`: if WAVE and `enemiesToSpawn == 0 && enemiesAlive == 0`: `++wave`; if `wave > totalWaves` -> VICTORY + `GAME_EVT_VICTORY`, else BUILD. Leaks are resolved in `update_enemies` before this; lives hitting 0 sets GAMEOVER first. Old LÖVE retry-on-leak behaviour is gone.

### 6.6 HUD wave counter

Top HUD shows `shownWave = min(wave, totalWaves)` / `25`; bottom BUILD panel shows `WAVE %d OF %d` for the upcoming wave; bottom WAVE panel shows `WAVE %d` + progress; result screen shows the clamped value.

### 6.7 Wave preview API

`game_wave_preview(g, &hpLayers, &shieldLayers, &armorLayers, &elites)` — see §6.3; used by the bottom INCOMING panel and by tests (`test_wave_composition`, `test_layer_composition_truthful`).

### 6.8 Capacity

`MAX_ELITE_SPAWNS 16` (unused by the current path; E <= 10), `MAX_ENEMIES 128` (max 53), `MAX_BEAMS 128`, `MAX_PUDDLES 16`, `MAX_TOWERS 96` -> `"tower limit"`.

---

## 7. Economy & lives (unchanged from v0.2)

| Item | Value | Source |
|---|---|---|
| Starting gold / lives | **150 / 20** | `[CODE]` `STARTING_GOLD`, `STARTING_LIVES` |
| Tower costs | GUN/FIRE/SHOCK/CORROSIVE 20, **SLAG 15**, **BLASTER 30** | `[SPEC]`/`[CODE]` |
| Kill reward | **+10 gold**, any enemy, elite or not | `[LUA]` `CORE_KILL_REWARD` |
| Leak | **−1 life, 0 gold** | `[CODE]` |
| Blocked placement | 0 gold, no tower | `[CODE]` |
| Sell refund | `floor(invested * 0.75)` (75 %) | `[LUA]`/`[CODE]` |
| Upgrade cost | `20 * 2^(L-1)`: 20,40,80,160,320,640,1280,2560,5120 | `[LUA]`/`[CODE]` |
| Max lifetime gold | 150 + 7,250 = **7,400** | `[DERIVED]` |
| Win / lose | clear wave 25 / 0 lives | `[CODE]` |

- Status/effect damage still awards the normal kill reward.
- There is no income besides kills; no wave-completion bonus; no leak gold.
- Building on the road costs 20/15/30 gold like any tower — a wall is a tower, and it still shoots.

---

## 8. Upgrades (unchanged curve; effect line added to the UI)

- Max level 10; damage **doubles per level** `3 * 2^(L-1)` for the 3-damage towers (SLAG `1*2^(L-1)`, BLASTER `9*2^(L-1)`).
- Cooldown from the shared table (0.80 → 0.28), multiplied by 3 for BLASTER (2.40 → 0.84).
- Cost `20 * 2^(L-1)`; `invested` accumulates; sell = `floor(invested*0.75)`; upgrade does not reset the current cooldown; range constant.
- Full per-level numbers in §3.4.
- UI: bottom info panel shows `DMG / RNG / DPS` plus an **effect line** (see §9.6), and UPGRADE/SELL buttons with prices (`UPGRADE <cost>G` / `MAX LEVEL`, `SELL +<refund>G`).

---

## 9. UI / HUD content (shipped 3DS implementation)

The LÖVE original's 1280x720 HUD is superseded; the maze/economy content was re-laid out in v0.2 and the combat additions below are the v0.3 UI. Positions are top-screen (400x240) and bottom-screen (320x240) pixels; text uses the 8x8 font at scale 1 (8 px) or 2 (16 px).

### 9.1 Top HUD strip (0..37 px, top screen)

| Element | Position | Scale | Colour | Content |
|---|---|---|---|---|
| Background | `rect(0,0,400,38)` | — | (0.035,0.035,0.09) | 1 px bottom line (0.20,0.20,0.38,0.45) |
| Coin | `art_coin(14,19,6)` | — | gold/white/amber | |
| Gold | `text(26,11)` | 2 | gold (1.00,0.82,0.24) | `%d` |
| Heart | `art_heart(100,19,6)` | — | red (1.00,0.40,0.36) | |
| Lives | `text(112,11)` | 2 | white | `%d` |
| Phase tag | `text(160,5)` | 1 | PAUSED grey; WAVE green; OVER red; WIN gold; BUILD yellow | `PAUSED`/`WAVE`/`OVER`/`WIN`/`BUILD` |
| Wave counter | `text(160,17)` | 2 | white | `min(wave,25)/25` |
| ALIVE / TOWERS label | `text(244,5)` | 1 | light secondary (0.86,0.88,0.96) | `ALIVE` in WAVE else `TOWERS` |
| Count | `text(244,17)` | 2 | white | `enemiesAlive` / `towerCount` |
| SPEED label | `text(330,5)` | 1 | secondary | `SPEED` |
| Speed value | `text(330,17)` | 2 | white, yellow if >1x | `%.0fX` |

### 9.2 Board rendering (top screen)

- Board 26x13 at 15 px cells, origin `(5,38)`; 2 px backing lip + border.
- Road drawn from the live `g->path` (`build_path_map`): each path cell is filled with the brown gradient `(0.24+0.10t, 0.14+0.05t, 0.05+0.03t)`, `t = pathIndex/(count-1)`; 1 px lighter shoulder `(0.40,0.27,0.11,0.90)` where road meets a non-road cell.
- Buildable cells (0.05,0.05,0.08); 1 px grid lines `(0.14,0.14,0.22,0.30)`.
- Tower cells get a 0.06-alpha tint of the tower's damage-type colour.
- **Entrance/exit arrows**: green `(0.35,1.00,0.45)` at `path.pts[0]` pointing along the first segment; red `(1.00,0.35,0.30)` at the last point pointing along the last segment (black backing triangle + coloured triangle). They follow the live road.
- Selected-tower range circle: 32 px radius (2.142857 × 15), fill alpha 0.06 + 1 px outline alpha 0.32 in the damage-type colour, plus a 2 px cell outline; the tower under the cursor (when different) gets a dim fill alpha 0.04.
- Cursor: white fill alpha 0.07, 2 px corner brackets, edge colour by legality: white = tower, green = legal+affordable, yellow = legal but no gold, dim = blocked (entrance/exit or would seal the road); a faint filled circle + outline ghost in the selected tower's colour appears on an empty legal cell.

### 9.3 Enemies / status visuals

See §5.4 for exact radii/alpha/colours. Draw order: board -> puddles -> portal arrows -> range circles -> towers -> beams -> enemies -> cursor.

### 9.4 Puddles, beams, explosions

- **Puddle**: filled circle radius `0.7 cells * 15 = 10.5 px` in (0.40,0.85,0.14) alpha `0.10+0.18*lifeFraction`, 1 px rim (0.78,1.00,0.40) alpha `0.30+0.45*lifeFraction`; drawn **under** towers/enemies just above the board.
- **Line beam** (all non-explosive towers): 3 px pass in the damage-type colour alpha `life/0.15 * 0.14`, then 1 px core alpha `life/0.15`, then 1 px white alpha `*0.55`.
- **Explosion ring** (BLASTER): radius grows `aoeRadius * 15 * (1 - life/maxLife)`; 2 px damage-coloured ring alpha `0.90 * lifeFraction` (14 segments) + inner 1 px white ring at 0.62x radius alpha `0.40 * lifeFraction` (12 segments); life 0.30 s.
- Damage-type colours: normal off-white (0.92,0.92,0.88), fire orange (1.00,0.45,0.12), electric cyan (0.25,0.85,1.00), corrosive acid green (0.55,1.00,0.20), slag purple (0.78,0.35,1.00), explosive amber (1.00,0.65,0.10). Text variants are lightened for legibility.
- Layer colours: HP red (0.95,0.18,0.18), SHIELD blue (0.28,0.55,1.00), ARMOR yellow (0.95,0.78,0.18); text variants (1.00,0.60,0.56), (0.62,0.80,1.00), (1.00,0.90,0.52). Layer names `HP`, `SHIELD`, `ARMOR`.

### 9.5 Bottom screen: header + six-tower palette (0..87 px)

- Header 0..31 px: same content/layout as §9.1 but 320 px wide: coin (14,16,6) + gold text (26,8,scale2); heart (100,16,6) + lives (112,8,scale2); `"WAVE"` scale1 (160,12) + `"%d/%d"` white (200,12); SPEED badge `rect(244,2,34,28)` with `%.0fX` scale2; PAUSE badge `rect(282,2,34,28)` with a two-bar pause icon (5x12 bars at x+10/x+19, y+8).
- **Palette: six buttons in 2 rows x 3**, each `96 x 25` px:
  - rows at y = 34 and 62; x = 6, 110, 214 (`ui_tower_button_rect`);
  - default fill (0.07,0.07,0.13), 1 px border (0.22,0.22,0.42,0.70); selected: damage-colour fill alpha 0.18, 2 px damage-colour border, glow;
  - icon at `(x+12, y+13)` radius 7, glow behind; display name at `(x+21, y+4)` scale 1 (selected white, else light secondary); coin at `(x+26,y+19)` r 2.5; cost at `(x+31,y+15)` scale 1 (gold if affordable, muted gold otherwise);
  - display names (UI): **`GUN`, `FIRE`, `SHOCK`, `CORROSIVE`, `SLAG`, `EXPLOSIVE`** (the last is `TowerSpec.name = "BLASTER"`'s damage-type display name; UI shows `EXPLOSIVE`).
  - tapping a palette button selects that damage type for building (`UI_EVENT_SELECT_TYPE`).

### 9.6 Bottom screen: info / upgrade panel (y 89..155)

Panel `rect(4,89,312,66)`.

**Tower selected (the explicitly selected tower wins; otherwise the tower under the D-pad cursor):**
- icon `(15,96)` radius 5.5 + glow; name `"{TYPE} LV.{n}"` at `(28,91)` scale 1 in the damage-type text colour;
- stats line `"DMG %.1f   RNG %.1f   DPS %.1f"` at `(10,104)` scale 1 (DPS = damage/cooldown);
- **effect line** at `(10,115)` in the text colour, exact strings:
  - FIRE: `BURNS - 2x vs HP`
  - SHOCK: `STUNS - 2x vs SHIELD`
  - CORROSIVE: `ACID PUDDLE - 2x vs ARMOR`
  - SLAG: `SLAG: +50% DMG TAKEN`
  - EXPLOSIVE: `AOE 1.3 - 3x SLOWER`
  - GUN: `NO SPECIAL EFFECT`
- UPGRADE button `rect(8,126,150,25)`: `UPGRADE <cost>G` or `MAX LEVEL` (green frame, dimmed if unaffordable/maxed); SELL button `rect(164,126,148,25)`: `SELL +<refund>G` (red frame).

**Cursor on an empty/road cell (no tower selected):**
- `"CURSOR C%d R%d"` at `(10,92)` scale 1 secondary; `ROAD` (gold) or `EMPTY CELL` (light) right-aligned at `(308,92)`;
- hint at `(10,106)` scale 1:
  - legal build on road: `ROAD - BUILD HERE TO REROUTE THEM` (green);
  - legal build elsewhere: `A BUILDS <TYPE> FOR <cost>G` in that damage type's text colour;
  - illegal road cell: `ROAD - THIS CELL CAN'T BE BLOCKED` (secondary);
  - other illegal: `CAN'T BUILD HERE RIGHT NOW` (secondary);
- `"L/R TYPE (6)   A BUILD   B CLEAR"` at `(10,121)`; `"TAP A TYPE ABOVE TO SWITCH"` at `(10,135)` (both light secondary).

### 9.7 Bottom screen: INCOMING preview (BUILD phase, bottom band y 159..236)

- NEXT WAVE button `rect(6,159,190,77)`: green fill (0.13,0.45,0.20), 2 px green border, glow, top highlight; `"NEXT WAVE"` scale 2 at y+10; `"WAVE %d OF %d"` scale 1 at y+34; `"PRESS WHEN READY"` scale 1 at y+48.
- INCOMING panel `rect(202,159,112,77)`: title `INCOMING`; four rows at y+20/34/48/62, each = 3 px coloured layer dot + `"HP x%d"` / `"SHIELD x%d"` / `"ARMOR x%d"` (layer text colours) and a gold star + `"ELITE x%d"`. Counts are the **total layers across the wave** from `game_wave_preview()`.

### 9.8 Bottom screen: WAVE phase panel

Panel `rect(6,159,308,77)`:
- `"WAVE %d"` scale 2 at `(14,162)` white; `"IN PROGRESS"` scale 1 right-aligned `(306,168)`;
- progress bar `rect(14,181,292,15)` dark, fill `(16,183,288*frac,11)` orange (0.95,0.60,0.15), 1 px outline light blue-grey; `"CLEARED %d%%   LEFT %d"` centred `(160,184)` white (alpha 0.96);
- rows: `"ALIVE %d"` (14,200), `"QUEUED %d"` (112,200), `"KILLS %d"` (212,200); `"LEAKS %d"` (14,212, red if >0), `"TOWERS %d"` (112,212), `"TIME %d:%02d"` (212,212);
- footer `"START: PAUSE    SELECT: SPEED"` centred `(160,224)`.
- The progress denominator is `App.waveEnemyTotal`: the largest `(enemiesAlive + enemiesToSpawn)` observed since `Game.wave` last changed (reset on wave change). Frame-rate independent; the core does not expose the wave spawn count directly.

### 9.9 Menus and overlays

**Bottom menu (`PHASE_MENU`)**: glow + layered title `ELEMENTAL` (scale 3 at y14, shadow y17), `DEFENSE` (scale 2 y44), `TOWER DEFENSE` (scale 1 y66); three representative icons `FIRE/ELECTRIC/SLAG` at x=80/160/240, y≈92, with glow, labels at y110; red START button `rect(50,124,220,64)` (shadow, highlight, 1 px outline) with `"START"` scale 3 at y+12 and `"TAP OR PRESS A"` scale 1 at y+44; hints at y198 `D-PAD TO BUILD - TOUCH FOR BUTTONS`, y214 `SELECT SPEED - START PAUSE`, y228 `A NATIVE 3DS REMAKE`.

**Top menu**: gradient + drifting particles, title `ELEMENTAL` (scale 4, y26 with shadow), `DEFENSE` (scale 3, y70 with shadow), `TOWER DEFENSE` (scale 1, y103), divider at y116, three representative icons FIRE/ELECTRIC/SLAG at x=140/200/260, y≈148, labels y168; `"PRESS A OR TAP START"` prompt box y190–208; `"A NATIVE 3DS REMAKE"` y222.

**Paused overlay (both screens)**: dim the play UI; top: panel `rect(96,72,208,92)`, `"PAUSED"` scale 3 y86, `"START TO RESUME"` y120, `"SELECT CHANGES SPEED"` y136. Bottom: panel `rect(50,24,220,196)`, `"PAUSED"` scale 3 y40, `"THE WAVE IS FROZEN"` y74, RESUME `rect(60,104,200,52)` (green, `RESUME` scale 2), MAIN MENU `rect(100,168,120,28)`, `"START ALSO RESUMES"` y208.

**Result (GAME OVER / VICTORY)**: bottom: title scale 4 at y14 (`GAME OVER` red / `VICTORY!` gold), subtitle y52 (`THE PATH WAS BREACHED` / `ALL WAVES CLEARED`), stats panel `rect(30,64,260,66)` with `"WAVE %d/%d"` y71, `"KILLS %d     LEAKS %d"` y87, `"TIME %d:%02d     GOLD %d"` y103; PLAY AGAIN `rect(40,140,240,56)` green (label scale 2 y+11, `TAP TO RETRY` y+38); MAIN MENU `rect(80,204,160,28)`. Top overlay: dim 0.78 + panel `rect(60,56,280,136)`, `"VICTORY!"`/`"GAME OVER"` scale 3 y68, `"WAVE %d/%d"` y108, `"KILLS %d   LEAKS %d"` y122, `"TIME %d:%02d   GOLD %d"` y136, `"A: PLAY AGAIN    B: MENU"` y164.

### 9.10 Toast

Failed actions set `Game.message` + `messageTimer = 1.5 s` (real time; ticks while paused). The bottom screen draws a centred dark red-bordered box at y35 (height 18) with the message at scale 1, fading out over the last 0.4 s; messages never block input. The core clears the text when the timer expires.

### 9.11 Palette (`source/render.h`)

Text colours are tuned bright for the glossy 240p LCD: white (1,1,1); panel (0.05,0.05,0.12,0.96); border (0.22,0.22,0.42,0.65); border-hi (0.45,0.45,0.70,0.80); gold (1.00,0.82,0.24); gold-soft (1.00,0.92,0.45); secondary text (0.86,0.88,0.96,0.95); tertiary text (0.80,0.82,0.92,0.92); red (1.00,0.40,0.36); green (0.35,1.00,0.42); path (0.26,0.15,0.06); path edge (0.40,0.27,0.11,0.90); cell (0.05,0.05,0.08); gridline (0.14,0.14,0.22,0.30); yellow (1.00,1.00,0.35); build tag (1.00,1.00,0.45); wave tag (0.50,1.00,0.55). Damage/layer/puddle/burn/stun colours in §4.4/§5.4/§9.4.

---

## 10. Input mapping

### 10.1 Original LÖVE controls (reference)

Keys: arrow keys / WASD move the build cursor; A/Return/Space build or select; B/Backspace cancel; X upgrade; Y sell (twice to confirm); L/R change speed {0,0.5,1,2,4}; SELECT/TAB toggle a controls overlay. Touch was mapped through `keypressed("a")` and was effectively broken (cursor moved left instead of building). The mouse never existed; the game was designed for a D-pad + touchscreen Vita/3DS.

### 10.2 Shipped 3DS mapping `[CODE]`

Top screen is display-only on hardware; all placement is D-pad + A, and the bottom screen is touch.

| Action | Touch (bottom) | Buttons |
|---|---|---|
| start run | START `(50,124,220,64)` on menu | A or Start |
| move cursor | — | D-pad (clamped; leaving a tower clears its selection) |
| choose tower type | one of the 6 palette buttons `(6/110/214, 34/62, 96x25)`; `UI_EVENT_SELECT_TYPE` | L / R cycle through all **6** types |
| place / select | — | **A** at cursor: tower present -> select; empty -> `game_place_tower` (road allowed if exit stays reachable) |
| clear selection | — | B (`game_select_tower(-1)`) |
| upgrade / sell | UPGRADE `(8,126,150,25)` / SELL `(164,126,148,25)` | X / Y (auto-selects the tower under the cursor first) |
| start next wave | NEXT WAVE `(6,159,190,77)` (BUILD only) | — |
| speed | header SPEED badge `(244,2,34,28)` | Select |
| pause / resume | header PAUSE badge `(282,2,34,28)`; paused: RESUME `(60,104,200,52)` | Start (handled before all other actions; also resumes) |
| retry / menu | PLAY AGAIN `(40,140,240,56)` / MAIN MENU `(80,204,160,28)` | A retry, B menu |
| paused -> menu | MAIN MENU `(100,168,120,28)` | — |
| debug | top-screen click (preview harness only) | — |

Input order per frame: Start (pause) first -> paused overlay touches only -> Select (speed) -> bottom touch hit-test -> D-pad -> L/R type -> A/X/Y/B -> `game_update`. A pause press can never also trigger an action.

### 10.3 Touch events `[CODE]`

`ui_hit_test()` -> `UI_EVENT_*` (`START`, `SELECT_TYPE(index 0..5)`, `UPGRADE`, `SELL`, `NEXT_WAVE`, `CYCLE_SPEED`, `TOGGLE_PAUSE`, `RESUME`, `RETRY`, `MENU`); `app.c` maps them to core calls. Hit boxes are the exported `ui_*_rect()` functions used by both drawing and hit-testing (no drift).

### 10.4 UI feedback

Cursor tile edge colours: white = tower, green = can build + affordable, yellow = legal but unaffordable, dim = blocked. A translucent ghost marks a legal empty cell. The info panel hint line tells the player whether a road cell is a good build (reroute) or blocked; selected towers show their effect line. No help overlay exists (Select is speed); the pause overlay carries the control hints.

---

## 11. Layout reference & mockups

### 11.1 Geometry

| Element | Value |
|---|---|
| Top screen | 400x240; board `BOARD_CELL 15`, `BOARD_X 5`, `BOARD_Y 38` -> 390x195; 38 px HUD strip; 7 px bottom margin |
| Top HUD | §9.1 positions |
| Bottom screen | 320x240: header 0..31, palette rows y34/y62 (96x25, x 6/110/214), info panel (4,89,312,66), bottom band y159 height 77 |
| Board hit test | `floor((px-5)/15)`, `floor((py-38)/15)` clamped to grid |

### 11.2 ASCII mockups (not to scale)

**A. BUILD phase** (seed default, wave 9 incoming: HP x21, SHIELD x21, ARMOR x21, ELITE x0):

```
TOP  400x240
+--------------------------------------------------------------------------------+
| (o) 150   (v) 20   BUILD    9/25      TOWERS 5            SPEED 1X             |  HUD 0..37
+--------------------------------------------------------------------------------+
|   . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .  |
|   . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .  |
|   ... 26x13 board @15 px; towers drawn as coloured glyphs, road from g->path ... |
|   >#############################>                                                |  road row 12
|   ^ entrance green arrow                              exit red arrow ^           |
+--------------------------------------------------------------------------------+
BOTTOM 320x240
+--------------------------------------------------------------------------+
| (o)150   (v)20   WAVE 9/25                            [1X]  [||]         |  header
+--------------------------------------------------------------------------+
| [ GUN 20 ]  [ FIRE 20 ]  [ SHOCK 20 ]                                    |  palette row 1
| [ CORROSIVE 20 ] [ SLAG 15 ] [ EXPLOSIVE 30 ]                            |  palette row 2
+--------------------------------------------------------------------------+
| CURSOR C3 R11                                            ROAD             |
| ROAD - BUILD HERE TO REROUTE THEM                                        |
| L/R TYPE (6)   A BUILD   B CLEAR                                         |
| TAP A TYPE ABOVE TO SWITCH                                               |
| [ UPGRADE 20G ]                       [ SELL +15G ]                      |
+--------------------------------------------------------------------------+
| [   NEXT WAVE   ]  WAVE 9 OF 25    | INCOMING                            |
| [ PRESS WHEN READY ]               | * HP x21   * SHIELD x21              |
|                                    | * ARMOR x21  * ELITE x0             |
+--------------------------------------------------------------------------+
```

**B. WAVE phase** — layered enemies in progress:

```
TOP
+--------------------------------------------------------------------------------+
| (o) 150   (v) 20   WAVE     9/25      ALIVE 3            SPEED 2X             |
+--------------------------------------------------------------------------------+
|   ... road filled from g->path; enemies are concentric red/blue/yellow rings:   |
|   ... outer ring = current layer (hp fraction), inner rings = still-armoured,   |
|   ... orange burn motes, purple slag ring, white stun flash as applicable.      |
+--------------------------------------------------------------------------------+
BOTTOM
+--------------------------------------------------------------------------+
| (o)150   (v)20   WAVE 9/25                            [2X]  [||]         |
+--------------------------------------------------------------------------+
| [ GUN 20 ]  [ FIRE 20 ]  [ SHOCK 20 ]                                    |
| [ CORROSIVE 20 ] [ SLAG 15 ] [ EXPLOSIVE 30 ]                            |
+--------------------------------------------------------------------------+
| FIRE LV.3                                                        |
| DMG 12.0   RNG 2.1   DPS 18.8                                    |
| BURNS - 2x vs HP                                                 |
| [ UPGRADE 80G ]                       [ SELL +48G ]                       |
+--------------------------------------------------------------------------+
| WAVE 9                                                   IN PROGRESS     |
| [########################--------------]  CLEARED 62%   LEFT 8           |
| ALIVE 3    QUEUED 5    KILLS 13                                          |
| LEAKS 0    TOWERS 5    TIME 3:24                                        |
| START: PAUSE    SELECT: SPEED                                            |
+--------------------------------------------------------------------------+
```

**C. Example maze** (28 towers, measured with the port BFS: road length 35 cells; `T` = tower on the road/wall, `#` = current road, `S`/`G` = entrance/exit; rows 0–6 empty build space):

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

The road runs along row 7 for 20 cells before dropping back to row 12 at col 22. Wave 1 then takes 35/5.0 = 7.0 s to cross vs 5.0 s on the initial 25-cell road — more time under every tower's fire, on top of the maze being walled by shooting towers.

### 11.3 Draw order & budget `[CODE]`

Top: background gradient (24 bands) -> HUD -> board backing/cells -> road shoulders -> tower tints -> grid lines -> border -> puddles -> entrance/exit arrows -> range circles -> towers -> beams -> enemies -> cursor.
Bottom: gradient -> (menu | result | paused overlays) or header -> palette -> info panel -> toast -> build or wave band -> paused overlay.

Rough worst case (wave 25: 53 enemies, up to 96 towers, 16 puddles, 128 beams): board 338 + ~40 road shoulders + 96 tower glyphs (~6 prims each) + 53 enemies x (up to 3 rings x 12 line segments + core + status overlays) is the dominant cost (~3000 line segments at worst); the 3DS build is reported stable at 60 fps in the Azahar emulator. Ring drawing is segment-based (`draw_ring`, 12–14 segments) specifically to stay inside the citro2d object budget. No image assets; all primitives.

### 11.4 Stereo

Single 2D target (`GFX_LEFT`); the game is flat 2D, so duplicating the top screen for stereo would double cost for no gameplay benefit.

---

## 12. Audio mapping

### 12.1 Status `[CODE]`

`source/audio.c` is still a **stub**: it records `GAME_EVT_*` bits for tests/preview but plays nothing (README lists audio as a later milestone). The original `.wav`/`.ogg` assets are still on disk; the event mapping below is the intended wiring. Damage types do not change audio, only beam/impact visuals.

| `Game.events` bit | Intended sound | When |
|---|---|---|
| `GAME_EVT_BUILD` | `build.wav` | successful placement |
| `GAME_EVT_SELL` | `sell.wav` | confirmed sale |
| `GAME_EVT_UPGRADE` | random one of `upgrade_1..6.wav` | successful upgrade |
| `GAME_EVT_LASER` | `laser.wav` | any tower fires; throttle globally to **0.08 s** (LÖVE `laserSoundCooldown`) |
| `GAME_EVT_ENEMY_DIE` | `enemy_die.wav` | enemy killed (direct hit, burn tick, acid tick, explosion) |
| `GAME_EVT_WAVE_START` | `wave_start.wav` | NEXT WAVE |
| `GAME_EVT_LEAK` | — (optional alarm) | enemy reaches the exit |
| `GAME_EVT_GAMEOVER` | `gameover.wav` (never played in the LÖVE game) | lives reach 0 |
| `GAME_EVT_VICTORY` | — (optional jingle) | wave 25 cleared |

Menu music (`menu_music.ogg`, loop, volume 0.5) on boot; game music (`game_music.ogg`, loop, 0.5) during play; LÖVE stops menu music on run start and never pauses music.

### 12.2 Files

| File | Bytes | Format | Plan |
|---|---:|---|---|
| `build.wav` | 305,998 | WAV | keep |
| `enemy_die.wav` | 6,318 | WAV | keep |
| `gameover.wav` | 10,497,182 | WAV | trim to a 2-3 s stinger |
| `game_music.ogg` | 3,590,538 | OGG | optional |
| `laser.wav` | 7,758 | WAV | keep, throttled |
| `menu_music.ogg` | 1,705,932 | OGG | optional |
| `sell.wav` | 93,518 | WAV | keep |
| `upgrade_1..6.wav` | 63–80 KB each | WAV | keep |
| `wave_start.wav` | 151,278 | WAV | keep |

### 12.3 3DS recommendations

- SFX to 16-bit mono 22050 Hz (or IMA-ADPCM) embedded with `bin2s` (<400 KB total); `gameover.wav` trimmed to ~2 s.
- Music: no OGG decoder in libctru — omit, or pre-decode a 15–30 s loop to mono 22.05 kHz PCM and loop from RAM. Never decode OGG while rendering.
- `ndsp` with >=8 SFX channels + music channels; implement the 0.08 s laser throttle, driven by `Game.events` and `game_clear_events()` once per frame (already wired in `app.c`).

---

## 13. What likely made the old 3DS attempt fail (historical evidence)

1. **Absolute 1280x720 layout, no resolution independence** (`main.lua:11-18`, `1495-1501`; HUD at y505..720, grid at x276, menu panel 700x560). On a 400x240 screen only the top-left corner is visible — the README's "half the screen cut off".
2. **Per-frame immediate-mode primitives**: 721 rects for the gradient every frame, 338 cells/frame with an `isPathCell()` linear scan, polygon glyphs, five TrueType fonts. On the ARM11 via LovePotion that was the likely fps killer.
3. **Unbounded menu allocations** (4 particle tables/frame, no cap), plus a laser table per shot and four neighbour tables per BFS node.
4. **BFS cost/hitching**: table-queue `table.remove(...,1)` and double BFS per build. The port keeps the mechanic but re-implements it with fixed stack arrays and a single hypothetical pre-check (~1.7 KB of stack, <=338 cells).
5. **Audio memory/decode**: 10.5 MB gameover.wav + 5.3 MB OGGs.
6. **Touch controls fought keyboard emulation (fatal)**: `touchpressed` called `keypressed("a")`, which in MOVING state meant "cursor left" — on touch-only hardware the game was unbuildable. The port has no top-screen touch and a proper D-pad + bottom-button UI.
7. **No performance escape hatches** (no batching, culling, fixed timestep, resolution factor).
8. **No end state**: leaks paid gold and restarted the wave forever; the game could neither be won nor lost.

The C rewrite removes 1–7 structurally (fixed 400x240/320x240 layout, deterministic fixed-pool core, cheap BFS, fixed-step loop, touch-native UI) and gives it proper endings. The LÖVE original's three-element combat did not survive at all — it was replaced by the BL2-inspired v0.3 model documented above.

---

## 14. Known bugs / unclear bits

### 14.1 Original LÖVE defects and how the port handles them

| ID | Lua ref | Defect | Port behaviour |
|---|---|---|---|
| B1 | `main.lua:1336-1344`, `1387-1393`, `1406-1415` | `a` aliased to "cursor left"/"previous type" before the confirm branch; A never builds; help text lies | A is a real confirm; D-pad moves the cursor (§10.2) |
| B2 | `main.lua:698-707`, `779-798` | Leaks paid +2/+10 gold and retried forever; no lives/game over/victory | 20 lives, −1 per leak, wave advances, victory at 25 |
| B3 | `main.lua:644-648`, `1031-1039` | Dead pre-wave build phase (menu starts wave immediately) | `game_new_run` starts in BUILD; wave 1 only on NEXT WAVE |
| B4 | `main.lua:794-798` | Auto-chain waves; COMPLETE never visible | Player-paced BUILD phase |
| B5 | `main.lua:63-69`, `1350-1376` | BFS seeded the start cell, so towers could be built on the spawn cell; validate-by-mutate-then-rollback | entrance AND exit protected; hypothetical pre-check; rejection costs nothing |
| B6 | `main.lua:558-565` | HUD composition quota computed from `base - E` while the wave was `base + E` | v0.2 exact-quota rule; v0.3 layer preview is exact (§6.3) |
| B7 | `main.lua:1235-1237` | `"X: MAX" .. "G"` -> `MAXG` | bottom button shows `MAX LEVEL` |
| B8 | `main.lua:1103` | right-aligned HUD header drawn off-screen | composition moved to bottom INCOMING panel |
| B9 | `main.lua:600-615` | 4 particle tables/frame, no cap | renderer-local capped particle pool |
| B10 | `main.lua:883-918`, `1463-1488` | help overlay/toast don't block touch; top touch fires `keypressed("a")` | no top touch; no help screen; toast never blocks input |
| B11 | `main.lua:1311-1314` | toast swallows one key | display-only toast |
| B12 | `main.lua:188` | global `state` vs `gameState` | `GamePhase` + cursor/selection; no build-mode globals |
| B13 | `main.lua:136-161` | `glowColor` fields never read | now used: towers/enemies/palette draw with spec glow (§3.1/§9) |
| B14 | `main.lua:502-508` | `getTotalCost` hardcoded base cost 20 (all towers cost 20) | `Tower.invested` tracks the actual cost (SLAG 15 / BLASTER 30 included) |
| B15 | `main.lua:199-201`, `764` | `gameTime`/`totalKills` written, never displayed | kills/leaks/time shown in wave panel + result |
| B16 | `main.lua:263` | `gameover.wav` loaded, never played | wired to `GAME_EVT_GAMEOVER` (when audio lands) |
| B17 | `main.lua:206`, `570`, `783-792` | `waveRetry`/`enemiesThatPassed` retry loop | removed; leaks cost lives |
| B18 | `main.lua:413-417` | health-bar width could go negative | layer health fraction clamped 0..1 before drawing |
| B19 | `main.lua:217-219`, `1316-1325` | speeds {0,0.5,1,2,4}; pause is a speed hack | 1/2/4 + separate pause overlay |
| B20 | `main.lua:140-145` | multiplier fallback 1.0 | `game_damage_multiplier` clamps out-of-range to 1.0/normal |
| B21 | `main.lua:164-179` (old model) | three identical-stat elements with a rock-paper-scissors table | replaced by the six damage types / three layers; the old matrix is gone |

### 14.2 Deliberate deviations from the LÖVE original and `elements-spec.md`

| # | Deviation | Source/why |
|---|---|---|
| D1 | Build phase between waves, player-paced NEXT WAVE | v0.2 lock |
| D2 | 20 lives; leak = −1 life, 0 gold; game over at 0 | v0.2 lock |
| D3 | Wave size `3+2*wave` INCLUDING elites; exact composition preview | v0.2 exact-quota lock |
| D4 | Victory after wave 25; `wave` becomes 26, display clamps to 25 | v0.2 lock |
| D5 | Pause overlay; speed 1/2/4 only | v0.2 lock |
| D6 | L/R = damage type, Select = speed, Start = pause; no help screen | v0.2 lock |
| D7 | Game-over/victory screens + gameover.wav | v0.2 |
| D8 | 3DS layout/rendering: 400x240/320x240, 8x8 font, board at (5,38), segment rings, no top touch | v0.2 + v0.3 |
| D9 | Toast never swallows input; 1.5 s wall-clock timer | v0.2 |
| D10 | Fixed 1/60 s timestep | v0.2 |
| D11 | Absolute enemy speed (`SPEED_SCALE 0.25`) instead of path-normalised | v0.2; makes mazing meaningful |
| D12 | Entrance and exit unbuildable; enemies re-projected after reroutes | v0.2 |
| D13 | Blocked placement pre-checked with a hypothetical BFS (no mutate/rollback) | v0.2 |
| **D14** | **Enemy HP ×2.0** (`kEnemyHpScale`) vs the literal spec formula. Single constant, allowed by elements-spec §4. Without it, mixed and GUN-only armies both trivially clear 25/25; with it, GUN-only dies on wave 3 and a mixed army clears 25/25 with 0–1 leaks. Set to 1.0f for literal spec values. | `[BALANCE]` §14.6 |
| D15 | Layer composition from a **deterministic hash of (run seed, wave, spawn slot)** rather than free per-spawn RNG rolls. Same distribution; makes `game_wave_preview()` exact and runs reproducible. | `[CODE]` §5.1 |
| D16 | On-hit statuses only apply to the surviving target of a **direct** hit; explosions never apply statuses (they have none anyway); burn/acid ticks never retrigger. Burn stack count is capped and refreshed; the timer is refreshed, not stacked. | `[CODE]` §4.4 |
| D17 | BLASTER cooldown is **3x the shared cooldown table at every level** (2.40 s at L1, 0.84 s at L10); the spec only pins the L1 2.40 s value. | `[CODE]` interpretation |
| D18 | Slag amplification applies to the slag tower's own hit too if the target is already slagged (single damage funnel) | `[CODE]` |
| D19 | Acid puddles: 16-slot pool; when full the round-robin `puddleNext` slot is replaced (not strictly the oldest-created puddle) | `[CODE]` |
| D20 | UI displays the damage-type names (`GUN/FIRE/SHOCK/CORROSIVE/SLAG/EXPLOSIVE`) while `TowerSpec.name` for the last is `"BLASTER"` | `[CODE]` `ui.c` |
| D21 | LÖVE original's three-element model (Fire/Water/Forest, identical stats) is not ported at all; the combat model is new design (BL2-inspired, our numbers). | `[SPEC]`/v0.3 |
| D22 | `hitFlash` (0.12 s white flash) is a renderer-only addition, not in the LÖVE game | `[CODE]` |

### 14.3 Behaviours kept intentionally

- Dynamic BFS maze road, towers on the road, blocked-build rejection, enemy re-projection (§2).
- Hitscan attacks with instant damage; nearest-target default with "oldest wins ties"; fixed 0.15 s beam visuals.
- Wave counts/intervals, economy, upgrade curve, sell refund, lives/victory rules (§6–§8).
- Building/upgrading/selling mid-wave; only NEXT WAVE is build-phase-only.
- Elite slot rule (slot 0 regular, elites consecutive); kill reward flat +10.
- Pixel-art vocabulary: element glyph shapes for towers, `font8x8` text, palette identity colours.

### 14.4 Decisions already resolved (no action needed)

| Item | Resolution |
|---|---|
| Combat model | BL2-inspired six damage types x three layers, statuses, AoE (`elements-spec.md`) |
| Maze mechanic | dynamic BFS restored; towers on road; blocked builds refused; enemies re-projected |
| Speed | absolute cells/s, `SPEED_SCALE 0.25` |
| Layers | 1/2/3 by wave, elites +1 capped 3; deterministic type hash; exact preview |
| HP | spec total HP formula × `kEnemyHpScale 2.0`, split equally across layers |
| Damage resolution | single funnel `apply_damage()`; raw-damage carry-over; slag ×1.5 amp |
| Statuses | burn 5x1 dps/2.5 s; stun 0.30 s; acid puddle 0.7/5 s/2 dps/16 slots; slag 4 s ×1.5 dmg/×0.65 speed |
| Targets | nearest; FIRE→HP, SHOCK→SHIELD, CORROSIVE→ARMOR outer-layer preference; fallback nearest; ties oldest |
| Explosive | BLASTER 3x cooldown/damage, AoE 1.3, neutral multipliers, explosion-ring visual |
| Economy | costs 20/20/20/20/15/30; kill +10; leak −1 life/0 gold; sell 75 %; upgrades 20·2^(L−1) |
| Victory | wave 25 cleared; lives 0 -> game over |
| Controls | D-pad + A place/select, L/R type (6), X upgrade, Y sell, Select speed, Start pause |

### 14.5 Open questions / things to confirm

- **Q1 — HP scale sign-off.** Shipped `kEnemyHpScale = 2.0f` (deviation D14). The sim evidence supports it (§14.6), but it is the one number that departs from `elements-spec.md`; changing it to 1.0 is a one-line edit (and makes the element system cosmetic again).
- **Q2 — "BLASTER" vs "EXPLOSIVE" naming.** `TowerSpec.name` says `BLASTER`; the UI/effect line says `EXPLOSIVE`. Cosmetic; pick one before localisation.
- **Q3 — BLASTER cooldown curve.** 3x the whole cooldown table (current) vs only the L1 value pinned by the spec. Current choice is documented (D17); confirm.
- **Q4 — puddle replacement semantics.** Round-robin slot replacement works but is not literally "oldest". Fine for gameplay; note if strict oldest matters.
- **Q5 — deterministic layer hash vs free RNG.** Hash gives exact preview and reproducibility; if a future change starts consuming `Game.rng`, `game_wave_preview()` must be updated together with it (`NOTES.md` §1).
- **Q6 — audio/music** still stubbed; decide SFX-only vs a pre-decoded music loop, trim `gameover.wav`.
- **Q7 — no in-game help screen** (Select = speed); the pause overlay only has hints. Add a help panel only if desired.
- **Q8 — README still describes the old three-element model** (`README.md` lines about Fire/Water/Forest) — docs outside this file are stale and should be updated by their owners.

### 14.6 Balance evidence (`tools/sim.c` + `NOTES.md` §9)

Global enemy HP multiplier sweep (damage table/economy untouched):

| enemy HP | mixed AI (cycles damage types) | plain-GUN AI (neutral only) |
|---|---|---|
| ×1.0 (literal spec) | VICTORY 25/25, 0 leaks | VICTORY 25/25, 0 leaks |
| **×2.0 (shipped)** | **VICTORY 25/25, 1 leak** | **GAMEOVER wave 3, 0 kills, 20 leaks** |
| ×3.0 | VICTORY 25/25, 1 leak | GAMEOVER wave 3, 0 kills |
| ×4.0 | GAMEOVER wave 4 | GAMEOVER wave 3 |

Final runs at HP×2.0, SPEED_SCALE 0.25, seeds 1337/1/424242/999999 (identical verdicts): mixed `VICTORY 25/25`, 19–20 lives, 0–1 leaks, 724–725 kills, 36 towers, road 25 cells (~0.1 s CPU); maze variant `VICTORY 25/25`, 20/20 lives, 0 leaks, 725 kills, 36 towers, road 49 cells (~0.13 s); plain-GUN `GAMEOVER wave 3`, 0 lives, 20 leaks, 0 kills, 7 towers.

Interpretation: the maze/element system now matters — bringing the right damage types is required, mazing adds a safety margin, and ignoring elements loses by wave 3. `SPEED_SCALE` remains the pacing knob; `kEnemyHpScale` is the difficulty knob. All other numbers are as specified.

---

## 15. Implementation guide

### 15.1 File responsibilities `[CODE]`

| File | Owns |
|---|---|
| `include/game.h` | frozen core API: `DamageType`, `LayerType`, `EnemyLayer`, `Puddle`, `Beam.kind/radius`, `TowerSpec.damageType/aoeRadius`, `Game.puddles/puddleNext/layerQuota`, preview multiplier APIs |
| `include/gfx.h` | two-screen immediate drawing API (clear/rect/circle/line/triangle + 8x8 text) |
| `source/core/core_internal.h` | private constants: status-effect extern consts, maze constants, wave formulas, layer composition API |
| `source/core/data.c` | **all content**: 6 `TowerSpec`s, damage matrix, cooldown/upgrade tables, status constants, wave formulas, `kEnemyHpScale`, `SPEED_SCALE`, layer count/type hash, `game_wave_preview` |
| `source/core/grid.c` | dynamic BFS road, reachability, placement legality, path queries |
| `source/core/game.c` | lifecycle, fixed-step update, spawning, layered `apply_damage`, status effects, puddles, targeting/AoE, wave completion, economy, lives, actions, RNG (seed only), re-projection |
| `source/render.c` / `.h` | top screen: background, HUD strip, board + live road + shoulders + entrance/exit arrows, range circles, towers (6 glyphs), beams/explosion rings, enemy layer rings + status overlays, puddles, cursor, menu/pause/result overlays; shared pixel-art helpers + colour tables |
| `source/ui.c` / `.h` | bottom screen: header + badges, 2x3 tower palette, info/effect/upgrade panel, NEXT WAVE + INCOMING layer preview, wave progress panel, menu/paused/result screens, touch hit-testing |
| `source/app.c` / `.h` | scene state machine, input -> action mapping, menu particles, `waveEnemyTotal` progress bookkeeping, event dispatch to audio |
| `source/main.c` | `aptMainLoop`, `hidScanInput` polling, frame timing, run seed entropy |
| `source/plat_3ds.c` | citro2d backend for `gfx.h` |
| `source/audio.c` | audio stub (records events, no playback yet) |
| `tests/test_core.c` | 46 host tests: towers/matrix/upgrades, maze/path/projection, wave composition + layer counts + truthful preview, all damage types, layer carry, burn/stun/puddle/slag, AoE, targeting, beams, movement/leaks, fail states, victory, determinism, speed/pause, events/messages, capacity |
| `tools/sim.c` | headless balance sim: mixed army (default), `--maze`, `--plain` GUN baseline |
| `tools/preview/*` | PC software-renderer preview harness (`gfx.h` backend, PNG frames) |

### 15.2 Content tables (`data.c`, exact values)

```
TowerSpec kTowerSpecs[6] = {
  {"GUN",       20, 3.0f, 0.80f, 60.0f/28.0f, DMG_NORMAL,    0.0f, {0.75,0.75,0.78},{0.45,0.45,0.50}},
  {"FIRE",      20, 3.0f, 0.80f, 60.0f/28.0f, DMG_FIRE,      0.0f, {0.95,0.45,0.12},{0.95,0.30,0.05}},
  {"SHOCK",     20, 3.0f, 0.80f, 60.0f/28.0f, DMG_ELECTRIC,  0.0f, {0.25,0.80,1.00},{0.10,0.60,1.00}},
  {"CORROSIVE", 20, 3.0f, 0.80f, 60.0f/28.0f, DMG_CORROSIVE, 0.0f, {0.45,0.85,0.15},{0.30,0.80,0.10}},
  {"SLAG",      15, 1.0f, 0.80f, 60.0f/28.0f, DMG_SLAG,      0.0f, {0.70,0.25,0.90},{0.60,0.15,0.85}},
  {"BLASTER",   30, 9.0f, 2.40f, 60.0f/28.0f, DMG_EXPLOSIVE, 1.3f, {1.00,0.70,0.15},{1.00,0.50,0.05}},
};
damage multiplier matrix (rows damage type, cols HP/SHIELD/ARMOR):
  normal {1,1,1}  fire {2,.5,.5}  electric {.5,2,.5}  corrosive {.5,.5,2}  slag {1,1,1}  explosive {1,1,1}
cooldown table: {0.80,0.72,0.64,0.57,0.51,0.45,0.40,0.36,0.32,0.28}   // BLASTER x3
upgrade cost:   {20,40,80,160,320,640,1280,2560,5120,0}
status: burn max 5 / 2.5s / 1 dps/stack; stun 0.30s; puddle r0.7 / 5s / 2 dps / 16 max;
        slag 4s / x1.5 dmg / x0.65 speed; explosion visual 0.30s
balance: kEnemyHpScale = 2.0f; SPEED_SCALE = 0.25f
wave: spawns = 3+2w; E = 10/3/1 on %25/%10/%5; interval = max(0.8, 3.0-0.05w);
      totalHP = (15+(w-1)*3) * kEnemyHpScale * (elite?2.5:1); layers as §5.1
```

### 15.3 Layer composition hash (`core_enemy_layer_types`, exact)

```
h = seed
h ^= wave * 0x9E3779B1
h ^= (spawnSlot+1) * 0x85EBCA77
h ^= h >> 16
h *= 0x7FEB352D; h ^= h >> 15
h *= 0x846CA68B; h ^= h >> 16
count = layer_count(wave, elite)
1 layer  -> type = h % 3
2 layers -> ordered pair from 6: {(HP,SH),(SH,HP),(HP,AR),(AR,HP),(SH,AR),(AR,SH)}[h % 6]
3 layers -> permutation from 6: {(HP,SH,AR),(HP,AR,SH),(SH,HP,AR),(SH,AR,HP),(AR,HP,SH),(AR,SH,HP)}[h % 6]
```

`seed` is `Game.rng` = the run seed; nothing advances it during a run. `game_wave_preview()` iterates all spawn slots of `g->wave` with the same function, so it is exact by construction. If a future feature starts consuming the RNG, the preview must be changed to use the same state.

### 15.4 Action / event contracts

- Placement errors in §2.4 order (lowercase, 1.5 s toast): `can't build now`, `can't build there`, `can't build on the entrance`, `can't build on the exit`, `cell occupied`, `tower limit`, `that would block the path`, `not enough gold`.
- Upgrade: `can't upgrade now`, `no tower selected`, `max level`, `not enough gold`.
- Sell: `can't sell now`, `no tower selected`.
- Events: BUILD/SELL/UPGRADE on success; LASER per shot; ENEMY_DIE per kill (any source); WAVE_START per wave; LEAK per leak; GAMEOVER/VICTORY on transitions. OR-accumulated until `game_clear_events()`; platform clears once per frame after dispatching audio.
- `game_clear_message()` zeroes message + timer.

### 15.5 Step/update contract

- `game_update(dt)`: clamp 0.25 s; paused -> tick message only; else accumulate and run <=6 fixed `1/60 s` steps, drop the rest on cap.
- `game_step(dt)`: §1.1 exactly; no allocations; deterministic from seed; dt clamped and speed-scaled inside.
- `paused` freezes battlefield + accumulator; message timers keep ticking.
- `game_toggle_pause()` resets the accumulator; `game_cycle_speed()` 1->2->4->1.

### 15.6 Perf / memory

- `sizeof(Game) = 18,960` bytes; `sizeof(Enemy) = 72`, `Tower = 20`, `Puddle = 28`, `Beam = 32`. Keep the single `Game` static — 19 KB stack is too much for the 3DS main thread.
- No dynamic allocation anywhere in the core; no libc math (squared distances); all pools fixed.
- BFS: <=338 cells x 4 neighbours, ~1.7 KB stack, once per build/sale plus the per-frame cursor legality query.
- Renderer worst case ~3000 line segments (wave 25, 53 layered enemies with status overlays) plus board/towers/UI; `draw_ring` uses 12–14 segments to stay inside the citro2d object budget; no image assets.
- Audio stub today; when wired, <=8 SFX channels + 1 music stream, 0.08 s laser throttle.

---

## Appendix A — constants reference

```c
/* grid / road */
#define GRID_COLS 26
#define GRID_ROWS 13
#define MAX_PATH_CELLS (GRID_COLS*GRID_ROWS)   /* 338 */
#define CORE_START_COL 0
#define CORE_START_ROW 12
#define CORE_GOAL_COL 25
#define CORE_GOAL_ROW 12

/* v0.3 combat pools */
#define TOWER_TYPE_COUNT 6
#define MAX_LAYERS 3
#define MAX_PUDDLES 16
#define MAX_TOWERS 96
#define MAX_ENEMIES 128
#define MAX_BEAMS 128

/* timing / plumbing */
#define CORE_FIXED_STEP (1.0f/60.0f)
#define CORE_MAX_STEPS_PER_UPDATE 6
#define CORE_MAX_UPDATE_DT 0.25f
#define CORE_MAX_STEP_DT 0.25f
#define CORE_MESSAGE_TIME 1.5f
#define CORE_BEAM_LIFE 0.15f
#define CORE_HIT_FLASH_TIME 0.12f
#define CORE_TOTAL_WAVES 25
#define CORE_KILL_REWARD 10

/* content / balance */
#define TOWER_RANGE_CELLS (60.0f/28.0f)        /* 2.142857 */
#define TOWER_BASE_COOLDOWN 0.80f
static const float kEnemyHpScale = 2.0f;       /* [BALANCE] D14 */
static const float kEnemySpeedScale = 0.25f;   /* D11 */
/* status: burn 5 / 2.5s / 1.0 dps-stack; stun 0.30s; puddle 0.7 / 5.0s / 2.0 dps;
   slag 4.0s / 1.5 amp / 0.65 speed; explosion visual 0.30s */

/* screen layout */
#define BOARD_CELL 15.0f
#define BOARD_X 5.0f
#define BOARD_Y 38.0f
#define HUD_H 38.0f
/* bottom bands: header 32, palette y34/y62 (96x25 at x 6/110/214),
   info panel x4 y89 w312 h66, bottom band y159 h77 */
```

## Appendix B — renderer cheat-sheet

```
cell -> top px:      px = 5 + cellX*15;  py = 38 + cellY*15;  cell 15 px
range radius:        2.142857*15 = 32.14 -> 32 px
road:                from g->path (live); fill each path cell with
                     (0.24+0.10t, 0.14+0.05t, 0.05+0.03t), t = i/(count-1);
                     1 px (0.40,0.27,0.11,0.90) shoulder where road borders buildable
entrance/exit:       green arrow (0.35,1.00,0.45) at pts[0]; red (1.00,0.35,0.30) at last;
                     apex 6 px, base 3 px back, half-width 4.5 px + black backing
damage colours:      normal (0.92,0.92,0.88) fire (1.00,0.45,0.12) electric (0.25,0.85,1.00)
                     corrosive (0.55,1.00,0.20) slag (0.78,0.35,1.00) explosive (1.00,0.65,0.10)
layer colours:       HP (0.95,0.18,0.18) SHIELD (0.28,0.55,1.00) ARMOR (0.95,0.78,0.18)
enemy rings:         radii 6.5/4.6/2.7 px, 2 px thick, 12 segments, alpha 0.45+0.55*frac,
                     outside-in, destroyed layers skipped; core dot r2.3 current layer colour
elite:               gold glow r14 + white ring r7.5 a0.64
hit flash:           white circle r6.2, alpha <=0.80 decayed by hitFlash
slag:                purple ring r8.4 pulsing; stun: white-blue circle r6.5 pulsing;
burn:                up to 5 orange 2x2 motes orbiting
puddle:              r = 0.7*15 = 10.5 px; fill alpha 0.10+0.18*frac, rim alpha 0.30+0.45*frac
line beam:           3 px colour a*0.14, 1 px colour a, 1 px white a*0.55 (life 0.15)
explosion:           ring radius = 1.3*15*(1-life/maxLife), 2 px colour a*0.9 (14 seg)
                     + inner 1 px white at 0.62x radius a*0.4 (12 seg), life 0.30
tower glyph radius:  7 / 8 / 9 px by level; level bar 11 px at cell bottom; gold cap at L10
HUD (top 400x38):    coin(14,19,6)+gold(26,11,s2); heart(100,19,6)+lives(112,11,s2);
                     phase tag(160,5,s1); wave(160,17,s2 clamped); ALIVE/TOWERS(244,5,s1)+count(244,17,s2);
                     SPEED(330,5,s1)+value(330,17,s2)
bottom palette:      96x25 at (6/110/214, 34/62); icon (x+12,y+13) r7; name (x+21,y+4) s1;
                     coin (x+26,y+19) r2.5; cost (x+31,y+15) s1
text:                8x8 font, scale 1 = 8 px, scale 2 = 16 px
```

## Appendix C — dynamic road + layer reference

**Initial road (empty board):** `(0,12) -> (25,12)`, 26 cells, length 25.0, centres `(i+0.5,12.5)`.

**Road fixtures (test-verified):** one tower at `(7,12)` -> length 27 (row-11 detour); scripted walls at cols 4/8/12/16 with alternating row gaps (48 towers) -> length > 60; every build/sell re-projects live enemies without teleports.

**Layer fixtures:** wave 1/3 -> 1 layer, wave 4/8 -> 2 layers, wave 9 -> 3; wave 5 = 12 two-layer regulars + 1 three-layer elite (total max HP 54 and 135 respectively); wave 10 -> 23 three-layer enemies (3 elites). `game_wave_preview` totals for waves 1/4/5/9/10/25 = 5/22/27/63/69/159 layers; 3-layer waves split evenly (spawns each per type).

**BFS order (must match for deterministic detours):** FIFO from `(0,12)` to `(25,12)`; neighbours up, down, left, right; walls = occupied cells; visited at enqueue; parent walk from goal back to start.

## Appendix D — LÖVE reference index (combat model does not come from here)

| Topic | `main.lua` lines |
|---|---|
| Window config / screen constants / grid + offsets | 11-52 |
| Pathfinding (BFS, updatePath) | 54-123 |
| Old three-element tower/enemy tables + damage matrix (superseded by v0.3) | 125-179 |
| Global state variables | 181-226 |
| Sounds + helpers | 228-287 |
| Pixel-art icon functions (fire/water/leaf shapes reused for FIRE/CORROSIVE bases) | 289-435 |
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
