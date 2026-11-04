# Core technical notes

Portable game core (`source/core/`, pure C99, no 3DS headers). The front end talks
to it only through `include/game.h`. This file records decisions, deviations from
the LÖVE original, and the little bits of behaviour the front end needs to know.

Status: implemented, unit-tested on host, playable in the headless sim.

```
source/core/core_internal.h   private shared interface (front end must not include)
source/core/data.c            content tables: tower/enemy specs, damage matrix,
                              per-level stats, upgrade costs, sell value, wave formulas
source/core/grid.c            fixed serpentine road, path queries, placement legality
source/core/game.c            lifecycle, fixed-step update, spawning, movement,
                              targeting, damage, economy, lives, phases, actions, RNG
tests/test_core.c             host unit tests (32 tests)
tests/Makefile                make -C tests CC="D:/tools/zig/zig.exe cc" test
tools/sim.c, tools/Makefile   headless auto-play / balance sim
```

Host build (verified with Zig 0.17.1 / clang 22, warning-free with
`-std=c99 -Wall -Wextra -pedantic`):

```
make -C tests CC="D:/tools/zig/zig.exe cc" test
make -C tools CC="D:/tools/zig/zig.exe cc" run
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
- `game_new_run(g, seed)`: fresh run in BUILD, wave 1, 150 gold, 20 lives, path built.
  `seed == 0` is replaced by a fixed non-zero seed (xorshift32 cannot run at zero).

Timing in one fixed step: spawn -> move enemies (leaks) -> towers target/fire ->
cleanup -> wave-complete check. This mirrors the original `love.update` order.

---

## 3. The fixed road (deliberate change vs the original)

The LÖVE game re-ran BFS after every build so towers placed on the path rerouted the
enemies, and a build that blocked the goal was rejected ("Path blocked!"). **That
mechanic is gone.** The core hard-codes ONE serpentine road in `grid.c`:

    row 1:  (0,1) -> (23,1)   24 cells, left to right   (start = (0,1))
    col 23: (23,2) -> (23,5)   4 cells, down
    row 5:  (22,5) -> (2,5)   21 cells, right to left
    col 2:  (2,6) -> (2,9)     4 cells, down
    row 9:  (3,9) -> (25,9)   23 cells, left to right   (goal = (25,9))

`path.count == 76`, `path.length == 75.0` cells, every consecutive pair is
4-connected. `path.pts` holds cell centres `(col+0.5, row+0.5)`; the renderer draws
the polyline through them. Towers cannot be placed on road cells at all, so the path
is immutable for the whole run and `pathBlocked` stays false.

Consequences: enemy movement is
`speed_cells_per_sec = (20 + (wave-1)) * (elite ? 1.8 : 1) * 75 / 100`
(equivalently the original `t += dt*speed/100` over the whole path). Wave 1 = 15
cells/s -> 5.0 s to cross; wave 25 = 33 cells/s -> 2.27 s; wave 25 elite = 2.27 s.
Enemies leak when `pathPos >= path.length`.

This segment table is NOT in `docs/design.md` (that document still describes the
straight BFS path); it implements the Lead's later hand-authored-road decision so the
26x13 board is actually usable: tower range is only 60/28 ~= 2.14 cells, so a
straight line would be reachable from just two rows.

---

## 4. Balance / content (numbers from main.lua unless noted)

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
  tracked exactly (20 + every upgrade paid).
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
  consecutively, then regulars. This is the Lead's exact-quota rule: `game_wave_preview()`
  is exactly what the wave will throw at you. (The Lua computed those quotas for the
  HUD but then rolled every spawn randomly; the Lua also added elites on top of the
  base count while subtracting them from the quota - `docs/design.md` 6.6 copies that
  off-by-one. Ported wave totals are therefore `3 + 2*wave`, not `3 + 2*wave + E`.)
- Enemy element rolls/shuffles and elite element rolls are the only RNG users, all
  through the seeded xorshift32, so a run is fully reproducible.

---

## 5. Actions / UI semantics

- `game_place_tower()` / `game_upgrade_selected()` / `game_sell_selected()` work in
  both BUILD and WAVE (the original allowed building mid-wave, and mid-wave upgrades
  are a deliberate quality-of-life keep). They are rejected in MENU/GAMEOVER/VICTORY
  with a message.
- `game_place_tower()` uses `g->selectedType`, ignores the cursor, and auto-selects
  the tower it just placed. On success: gold deducted, BUILD event, returns true.
  Failure messages (lowercase, ~1.5 s): "can't build there", "can't build on the
  path", "cell occupied", "tower limit", "not enough gold", "can't build now".
- `game_can_place_at()` answers "could a tower legally go here": bounds, phase,
  road, occupancy, tower cap. It deliberately does NOT check gold, so the UI can show
  a red tile vs an "not enough gold" toast. `game_tower_at()` returns -1 for empty.
- `game_upgrade_selected()` failure messages: "no tower selected", "max level",
  "not enough gold". `game_sell_selected()`: refund added, tower removed, occupancy
  rebuilt, selection cleared, SELL event.
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

---

## 6. Capacity / robustness

- `MAX_TOWERS 96` (balance AI uses 36), `MAX_ENEMIES 128` (peak alive in a real wave
  is a handful), `MAX_BEAMS 128` (cosmetic). Enemy spawning pauses while the enemy
  array is full and resumes (no enemy is silently lost); tower placement is rejected
  at the cap with "tower limit"; beams may be dropped.
- No dynamic allocation anywhere, no libc math at all (squared distances are used
  for range checks), no static mutable state: everything lives in the `Game` value.
  The same sources compile warning-free with devkitARM
  `arm-none-eabi-gcc -std=c99 -march=armv6k` (verified).
- Two `Game` values can run side by side (host tests compare them byte for byte for
  determinism).

---

## 7. Determinism

`game_new_run(g, seed)` fixes the RNG. Same seed + same API call sequence + same dt
sequence => identical `Game` bytes (there is no floating-point randomness, no time
source, no pointer use in the state). Tests cover this with an FNV hash and memcmp.
Different seeds produce different enemy element orders (and thus different runs).
