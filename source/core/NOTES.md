# Core technical notes

Portable game core (`source/core/`, pure C99, no 3DS headers). The front end talks
to it only through `include/game.h`. This file records decisions, deviations from
the LÖVE original, and the little bits of behaviour the front end needs to know.

Status: implemented, 39/39 host unit tests passing, playable in the headless sim,
and linked into the 3DS build.

```
source/core/core_internal.h   private shared interface (front end must not include)
source/core/data.c            content tables: tower/enemy specs, damage matrix,
                              per-level stats, upgrade costs, sell value, wave formulas,
                              absolute enemy speed (SPEED_SCALE)
source/core/grid.c            dynamic BFS maze road, reachability, placement legality
source/core/game.c            lifecycle, fixed-step update, spawning, movement,
                              targeting, damage, economy, lives, phases, actions, RNG,
                              enemy re-projection after every road change
tests/test_core.c             host unit tests (39 tests)
tests/Makefile                make -C tests CC="D:/tools/zig/zig.exe cc" test
tools/sim.c, tools/Makefile   headless auto-play / balance sim (plain and --maze)
```

Host build (verified with Zig 0.17.1 / clang 22, warning-free with
`-std=c99 -Wall -Wextra -pedantic`):

```
make -C tests CC="D:/tools/zig/zig.exe cc" test
make -C tools CC="D:/tools/zig/zig.exe cc" run          # plain AI
make -C tools CC="D:/tools/zig/zig.exe cc" run MAZE=1   # not wired; use ./tools/sim.exe --maze
```

---

## 1. Contract additions (append-only; nothing existing renamed or removed)

`include/game.h`:

1. `Game.stepAccumulator` (float, appended at the end) - leftover real time that
   `game_update()` has not converted into fixed steps yet. Kept in the struct so two
   `Game` values in one process never share timing state (host tests rely on this).
2. `Game.spawnQuota[TOWER_TYPE_COUNT]` (uint8_t, appended after it) - remaining
   regular-enemy element quota for the wave being spawned. Internal bookkeeping for
   exact-quota spawning; the front end never needs to write it.
3. `float game_damage_multiplier(int towerType, int enemyElement);` - the 3x3
   elemental damage matrix as a query (2.0 / 1.0 / 0.5). Handy for HUD "strong vs"
   hints and used by the tests. Invalid type/element clamp to 0.

**No additions were needed for the maze mechanic** (the rebuilt `g->path` is the
same public `Path` type the renderer already draws).

**Front end rule:** never reorder or insert fields above the appended ones. Appending
new fields/functions is fine, editing existing ones is not. Only the *core* should
add fields; coordinate with the Lead first.

`sizeof(Game)` is 12364 bytes (~12 KB). On the 3DS keep the single instance in
`.bss`/static or heap, not in a 12 KB stack local.

---

## 2. Frame model

- `game_update(g, dt)`: clamp `dt` to `CORE_MAX_UPDATE_DT` (0.25 s), accumulate, run
  at most 6 fixed steps of `1/60 s`; if the cap is hit the remaining time is dropped
  (no catch-up spiral). Call this once per real frame with the frame delta.
- `game_step(g, dt)`: exactly one deterministic step, no accumulation. Tests and the
  headless sim call this directly. Internally it multiplies `dt` by
  `speedMultiplier` (1x / 2x / 4x), so at 4x each fixed step simulates 4/60 s.
- `paused == true` freezes the battlefield and the fixed-step accumulator. UI message
  timers are wall-clock and keep ticking while paused.
- Phases: `MENU -> BUILD <-> WAVE`, plus terminal `GAMEOVER` / `VICTORY`. Only BUILD
  and WAVE simulate; terminal phases freeze. `game_start_next_wave()` only works in
  BUILD (and not while paused). The front end decides when to call it (there is no
  inter-wave timer in the core).
- `game_new_run(g, seed)`: fresh run in BUILD, wave 1, 150 gold, 20 lives, road built.
  `seed == 0` is replaced by a fixed non-zero seed (xorshift32 cannot run at zero).

Timing in one fixed step: spawn -> move enemies (leaks) -> towers target/fire ->
cleanup -> wave-complete check. This mirrors the original `love.update` order.

---

## 3. The maze road (the whole point of the game)

`core_build_path(g)` runs a **4-connected breadth-first search from the entrance
(0,12) to the exit (25,12)** every time the board changes. Towers occupy cells and
BFS treats them as walls; neighbours are expanded up, down, left, right, in the
original `main.lua:63-109` order, which keeps the result deterministic. On an empty
board the road is the straight row-12 corridor: **26 cells, 25 segments**.

- Towers MAY be built on road cells - that is how the player redirects enemies onto a
  longer detour. Only the entrance (0,12) and the exit (25,12) are off limits.
- `game_can_place_at()` / `game_place_tower()` refuse a build that would make the exit
  unreachable (`"that would block the path"`), and a refused build costs no gold and
  leaves no tower. The check runs a BFS on a hypothetical board, so it is safe to call
  for the cursor every frame and it never mutates anything.
- Every successful build or sell rebuilds `g->path`, then **re-projects every live
  enemy onto the new polyline**: per-segment clamped projection to the nearest point,
  `pathPos = segmentIndex + t`, then `x,y` from the path. Enemies keep walking from
  where they stand; they never teleport back to the entrance. The goal end is
  unchanged, so a shorter route (sell) also re-projects and lets them run.
- `g->pathBlocked` is not used by the game loop any more (kept per the contract);
  `core_build_path()` only sets it if the board was somehow corrupted behind the
  core's back.
- `path.length == count - 1` cells. `path.pts[]` are cell centres `(col+0.5,row+0.5)`;
  the renderer draws the polyline through them. The road can change every build, so
  the front end must redraw `g->path` each frame (it already reads the array).
- `game_wave_preview()` and the wave formulas are independent of the road length.

---

## 4. Enemy speed - deliberate deviation from the original

The LÖVE original moved enemies with `t += dt*speed/100` where `t` was normalised
over the whole path. That means a longer (mazed) path was walked in exactly the same
`100/speed` seconds: **mazing bought the player no extra tower time at all.** The port
uses an absolute speed instead:

```
speed_cells_per_second = (20 + (wave - 1)) * SPEED_SCALE      (elite: * 1.8)
```

`SPEED_SCALE` is one named constant in `source/core/data.c`
(`kEnemySpeedScale = 0.25f`). Rationale for 0.25:

- Wave 1 = 5 cells/s -> the initial 25-cell straight road is crossed in 5.0 s, exactly
  the original crossing time.
- Wave 25 = 11 cells/s -> 2.27 s across the straight road, same as the original's
  normalised wave-25 crossing time.
- A 60-cell maze at wave 1 takes 12 s; the 95-cell maze the sim AI builds takes 19 s.
  Extra road is now real, measurable extra time under fire.

If playtesting wants a snappier or slower game, `kEnemySpeedScale` is the only knob -
do not touch the HP/economy numbers for pacing.

---

## 5. Balance / content (numbers from main.lua unless noted)

- Towers: cost 20, base damage 3, base fireRate 0.8 s, range 60 px = 60/28 cells.
  All three elements start identical; only the damage matrix differs.
- Damage matrix (tower row -> enemy column): fire 1.0 / 0.5 / 2.0 (vs fire/water/
  forest), water 2.0 / 1.0 / 0.5, forest 0.5 / 2.0 / 1.0.
- Upgrades: max level 10. Cost from level L to L+1 = `20 * 2^(L-1)` = 20, 40, 80,
  160, 320, 640, 1280, 2560, 5120. Damage doubles per level: `3 * 2^(L-1)`.
  Fire rate per level (cooldown seconds, matching the Lua's floor-after-*0.9):
  `0.80 0.72 0.64 0.57 0.51 0.45 0.40 0.36 0.32 0.28`. Range NEVER upgrades.
  An upgrade does not reset the tower's cooldown.
- Sell refund: `floor(invested * 0.75)` (the Lua uses 75%, not 50%). `invested` is
  tracked exactly (20 + every upgrade paid). Selling re-paths the maze.
- Waves: `base = 3 + wave*2` enemies INCLUDING elites.
  Elite count: wave%25 -> 10, else wave%10 -> 3, else wave%5 -> 1, else 0.
  Spawn interval `max(0.8, 3.0 - wave*0.05)` seconds; the first enemy of a wave
  spawns one full interval after `game_start_next_wave()`.
  HP `15 + (wave-1)*3`, speed `20 + (wave-1)` in Lua units; elites x2.5 HP / x1.8
  speed. Kill reward 10 gold (elite or not).
- 25 waves total; clearing wave 25 sets wave = 26 and PHASE_VICTORY.
- Lives: 20 at start. Every leak (elite or not) costs exactly 1 life and pays no
  gold. At 0 lives -> PHASE_GAMEOVER.
- Enemy elements are quotas, not random: `regular = base - elites`,
  `fire = water = regular/3`, `forest = regular - 2*(regular/3)`; elites roll their
  element from the RNG. Spawn slots: slot 1 regular, slots 2..1+E elites
  consecutively, then regulars. This is the Lead's exact-quota rule:
  `game_wave_preview()` is exactly what the wave will throw at you.
- RNG is only used for elite elements and for the deterministic shuffle of the
  regular-enemy element order; a run is fully reproducible from its seed.

### Balance status (sim evidence, see section 9)

At `SPEED_SCALE = 0.25` both simulated players clear all 25 waves with 0 leaks:
the maze AI builds a 95-cell serpentine road (59 towers), the plain AI ignores the
maze and stacks 36 towers along the straight 25-cell road. The maze mechanic is
functional and rewarded, but **it is not yet required** - the original content is
generous enough that a straight line of towers kills everything. Making the maze
mandatory needs HP/economy tuning, not SPEED_SCALE; see section 9.

---

## 6. Actions / UI semantics

- `game_place_tower()` / `game_upgrade_selected()` / `game_sell_selected()` work in
  both BUILD and WAVE (the original allowed building mid-wave; keep it - mid-wave
  mazing is legitimate play). They are rejected in MENU/GAMEOVER/VICTORY.
- `game_place_tower()` uses `g->selectedType`, ignores the cursor, and auto-selects
  the tower it just placed. On success: gold deducted, BUILD event, road rebuilt,
  enemies re-projected, returns true.
- Placement failure messages (lowercase, ~1.5 s), in check order:
  `"can't build now"` (phase), `"can't build there"` (out of bounds),
  `"can't build on the entrance"`, `"can't build on the exit"`, `"cell occupied"`,
  `"tower limit"`, `"that would block the path"`, `"not enough gold"`.
- `game_can_place_at()` answers "could a tower legally go here": bounds, phase,
  entrance/exit, occupancy, tower cap, and the no-blocking rule. It deliberately does
  NOT check gold, so the UI can show a red tile vs an "not enough gold" toast.
  `game_tower_at()` returns -1 for empty.
- `game_upgrade_selected()` failure messages: `"no tower selected"`, `"max level"`,
  `"not enough gold"`. `game_sell_selected()`: refund added, tower removed, occupancy
  rebuilt, road rebuilt, enemies re-projected, selection cleared, SELL event.
- `game_select_tower(-1)` clears; invalid indices are treated as -1.
  `game_set_selected_type()` ignores out-of-range types. `game_set_cursor()` clamps.
- `game_cycle_speed()`: 1 -> 2 -> 4 -> 1. `game_toggle_pause()` resets the step
  accumulator so unpausing cannot burst.
- `game_clear_events()` zeroes the event bits after the audio/feedback layer consumed
  them; events are OR-accumulated between clears.
- `game_tower_spec()` / `game_enemy_spec()` return NULL for out-of-range indices;
  `game_tower_stats()` clamps type and level instead of failing (void return).

Messages: `g->message` + `g->messageTimer`. Display while `messageTimer > 0`; the
core clears the text itself when the timer expires. Timer is 1.5 s (Lua was 2.0 s;
Lead's UI spec says ~1.5 s).

Renderer-facing values maintained by the core: `Tower.animPhase` (seconds, advanced
in sim), `Tower.cooldown` (seconds to next shot), `Enemy.hitFlash` (0.12 s white flash
on damage), `Beam.life/maxLife` (0.15 s lasers). Beams are cosmetic: if the 128-slot
beam array is full a shot still deals damage and the beam is dropped.

**The road can change at any build/sell, including mid-wave. The renderer must read
`g->path.pts` / `g->path.count` every frame - never cache the polyline.**

---

## 7. Capacity / robustness

- `MAX_TOWERS 96` (sim maze AI uses 59, plain 36), `MAX_ENEMIES 128` (peak alive in a
  real wave is a handful), `MAX_BEAMS 128` (cosmetic). Enemy spawning pauses while
  the enemy array is full and resumes (no enemy is silently lost); tower placement is
  rejected at the cap with "tower limit"; beams may be dropped.
- The BFS runs on every placement check and every build/sell. Board is 338 cells, so
  this is a few thousand operations - fine to call per frame for the cursor, and it
  allocates nothing (fixed local arrays: queue/parent/visited ~1.7 KB of stack).
- No dynamic allocation anywhere, no libc math at all (squared distances are used
  for range checks), no static mutable state: everything lives in the `Game` value.
  The same sources compile warning-free with devkitARM
  `arm-none-eabi-gcc -std=c99 -march=armv6k` (verified).
- Two `Game` values can run side by side (host tests compare them byte for byte for
  determinism).

---

## 8. Determinism

`game_new_run(g, seed)` fixes the RNG. Same seed + same API call sequence + same dt
sequence => identical `Game` bytes (there is no floating-point randomness, no time
source, no pointer use in the state; BFS ties resolve with the fixed neighbour order).
Tests cover this with an FNV hash and memcmp.

---

## 9. Sim / balance notes (`tools/sim.c`)

Two scripted players at fixed 1/60 s steps:

- `sim.exe` (plain): builds towers only beside the road (never on it), then upgrades.
  The road stays at 25 segments, so this is the "ignore the maze" baseline.
- `sim.exe --maze`: every build phase, plants towers ON the current road, greedily
  choosing the cell whose blocking lengthens the route the most (using
  `core_path_length_with_block()`), then builds support towers / upgrades.

Measured at `SPEED_SCALE = 0.25f` (3 seeds, same verdicts):

| mode | result | lives | leaks | kills | towers | final road | CPU |
|---|---|---|---|---|---|---|---|
| plain | VICTORY 25/25 | 20/20 | 0 | 725 | 36 | 25 cells | 0.03 s |
| maze | VICTORY 25/25 | 20/20 | 0 | 725 | 59 | 95 cells | 0.05 s |

**The maze mechanic works but is not mandatory yet.** SPEED_SCALE sweeps show why:
at playable speeds (0.25-3.0) both AIs win with 0 leaks, because a plain tower line
already delivers one volley from every covering tower, and that volley damage
(~36 towers x level-5 damage) massively exceeds enemy HP at every wave. The requested
"plain struggles, maze wins" only appears at absurd speeds (scale 8: plain 8 leaks vs
maze 1; scale 12: plain dies on wave 25 vs maze wins with 5 leaks; scale 16: plain
dies wave 4), where enemies cross the road in well under a second and the fixed 1/60 s
step lets them tunnel between tower range checks - a simulation artifact, not
gameplay. Rejected.

To make mazing necessary (for a design sign-off, not applied silently), the content
needs one of:
1. **HP growth that outruns the one-volley kill**: e.g. exponential HP
   `hp = 15 * 2^((wave-1)/3)` instead of `15 + (wave-1)*3`, so a short straight road
   no longer provides enough shots, while a long maze does.
2. **Upgrade damage multiplier down** from x2 to ~x1.5 (plus a cheaper cost curve),
   so stacking 36 towers along one road cannot one-shot waves; long time-under-fire
   becomes the only way through.
3. **Tower cost / kill income retune** (e.g. kill reward 10 -> 5, tower cost 20 -> 25)
   so a plain player cannot afford full road coverage; maze walls double as towers,
   so they need fewer dedicated shooters. Probe: kill reward 5 alone gave plain 1
   leak / maze 0 - right direction but far too weak; a bigger economy squeeze would
   be needed.

SPEED_SCALE should stay 0.25 (or be changed only for pacing); it does not separate
the two strategies at playable values.
