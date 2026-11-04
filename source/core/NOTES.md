# Core technical notes (v0.3 combat model)

Portable game core (`source/core/`, pure C99, no 3DS headers). The front end talks
to it only through `include/game.h`. This file records decisions, deviations from
the design sheets, and the little bits of behaviour the front end needs to know.

Status: implemented, 46/46 host unit tests passing, winnable in the headless sim
(mixed-tower AI and maze AI), 3DS-clean.

Design sources of truth:
- `docs/elements-spec.md` - v0.3 damage types, layers, towers, multipliers, effects.
- LÖVE original `main.lua` - waves, economy, upgrades, path/maze behaviour.
- The Lead's v0.2/v0.3 decisions (maze mechanic, exact layer quotas, victory at 25).

```
source/core/core_internal.h   private shared interface (front end must not include)
source/core/data.c            all content: 6 TowerSpecs, damage matrix, cooldown
                              curve, upgrade costs, status-effect constants, wave
                              numbers, absolute speed (SPEED_SCALE), enemy HP
                              multiplier (kEnemyHpScale), layer composition hash
source/core/grid.c            dynamic BFS maze road, reachability, placement legality
source/core/game.c            lifecycle, fixed-step update, spawning, movement,
                              targeting, layered damage, status effects, economy,
                              lives, phases, actions, enemy re-projection
tests/test_core.c             host unit tests (46 tests)
tests/Makefile                make -C tests CC="D:/tools/zig/zig.exe cc" test
tools/sim.c, tools/Makefile   headless auto-play: mixed / --maze / --plain
```

Host build (verified warning-free with Zig 0.17.1 / clang 22,
`-std=c99 -O2 -Wall -Wextra -pedantic`):

```
make -C tests CC="D:/tools/zig/zig.exe cc" test
make -C tools CC="D:/tools/zig/zig.exe cc" run              # mixed army
tools/sim.exe --maze                                        # mixed + maze walls
tools/sim.exe --plain                                       # GUN-only baseline
```

---

## 1. Public contract notes

There are **no API additions** in this round: the v0.3 header already carries
everything (`DamageType`, `LayerType`, `EnemyLayer`, `Puddle`, `Beam.kind/radius`,
`TowerSpec.damageType/aoeRadius`, `Game.layerQuota`).

- `Game.rng` is the run seed. **Nothing consumes it during a run**: enemy layer
  composition is a pure function of `(g->rng, wave, spawn slot)`. That is exactly
  what lets `game_wave_preview()` be truthful without extra state. If future code
  ever advances the RNG, the preview must be changed together with it.
- `Game.layerQuota[]` is filled at `game_start_next_wave()` with the truthul
  HP/SHIELD/ARMOR layer totals of the wave being spawned (same function as the
  preview). Purely informational; nothing in the simulation reads it back.
- `game_wave_preview(g, &hpLayers, &shieldLayers, &armorLayers, &elites)` reports
  totals across **all spawns of the wave currently in `g->wave`** (during BUILD
  that is the upcoming wave, during WAVE the current one).
- `sizeof(Game)` is now ~17 KB (added layers/puddles). On 3DS keep the single
  instance in `.bss`/static or heap, not on the stack.

---

## 2. Frame model

- `game_update(g, dt)`: clamp `dt` to 0.25 s, accumulate, run at most 6 fixed
  `1/60 s` steps; drop the rest if the cap is hit (no catch-up spiral).
- `game_step(g, dt)`: exactly one deterministic step, no accumulation; multiplies
  `dt` by `speedMultiplier` (1x / 2x / 4x).
- `paused`: freezes the battlefield and accumulator; UI message timers keep ticking.
- Phases: `MENU -> BUILD <-> WAVE`, plus terminal `GAMEOVER` / `VICTORY`.
  `game_start_next_wave()` only works in BUILD and not while paused.
- Step order in WAVE:
  1. spawn queued enemies
  2. status effects: burn ticks, slag timer, acid puddle lifetime + ticks
  3. movement: stunned enemies freeze, slagged move at 65%, leaks cost 1 life
  4. compact, towers pick targets / fire / AoE / apply on-hit effects
  5. compact, wave-complete check
- BUILD: tower cooldowns and animations tick; beams fade; nothing else moves.

---

## 3. Layered enemies (elements-spec.md section 1)

- `layers[0]` is outermost. Layer count: waves 1-3 = 1, waves 4-8 = 2, waves 9+ =
  3; elites +1 capped at 3.
- Types, per spawn slot:
  - 1 layer: one random type
  - 2 layers: two different types, random pair and order
  - 3 layers: all three, random order
  The randomness is a deterministic hash of `(seed = g->rng, wave, spawn slot)`.
- Total HP `= (15 + (wave-1)*3) * (elite?2.5:1) * kEnemyHpScale`, split equally
  across the layers. `kEnemyHpScale = 2.0f` is the v0.3 balance tune (section 9).
- Elite spawn slots are 1..eliteCount (slot 0 regular), exactly like v0.2.
- Enemies are stored in spawn order (compaction preserves it), which is also the
  targeting tie-break order ("oldest first").

### Damage resolution (single funnel: `apply_damage()`)

All damage - direct hits, burn ticks, acid ticks, explosions - goes through:

```
remaining = raw damage; amp = slag ? 1.5 : 1.0
while remaining > 0 and layers left:
    mult = game_damage_multiplier(type, layers[0].type) * amp
    if remaining * mult < layers[0].hp:  layers[0].hp -= remaining*mult; stop
    else: remaining -= layers[0].hp / mult; pop layer   // carry raw damage
```

So a destroyed layer carries its **unspent raw damage** into the next layer, which
applies its own multiplier (spec section 1). Slag's x1.5 applies to every layer and
every damage source. When the last layer dies the enemy is killed: +10 gold,
`kills++`, `GAME_EVT_ENEMY_DIE`, `enemiesAlive--`.

## 4. Towers (elements-spec.md section 2)

| index | Damagetype | Cost | DMG L1 | Cooldown L1 | Effect |
|---|---|---|---|---|---|
| 0 | DMG_NORMAL (GUN) | 20 | 3 | 0.80 s | none |
| 1 | DMG_FIRE | 20 | 3 | 0.80 s | burn stack (max 5, 2.5 s, 1 dps/stack) |
| 2 | DMG_ELECTRIC (SHOCK) | 20 | 3 | 0.80 s | stun 0.30 s |
| 3 | DMG_CORROSIVE | 20 | 3 | 0.80 s | acid puddle (0.7 cells, 5 s, 2 dps) |
| 4 | DMG_SLAG | 15 | 1 | 0.80 s | slag 4 s: x1.5 damage taken, x0.65 speed |
| 5 | DMG_EXPLOSIVE (BLASTER) | 30 | 9 | 2.40 s | AoE radius 1.3 cells |

- Damage doubles per level; cooldown follows the existing per-level table
  (0.80, 0.72, 0.64, 0.57, 0.51, 0.45, 0.40, 0.36, 0.32, 0.28 s); BLASTER uses 3x
  that table. Range is a constant 60/28 cells for every tower and level.
- Upgrade costs stay `20 * 2^(L-1)`; sell refund stays `floor(invested * 0.75)`.
- Multiplier matrix (damageType x layerType): normal/slag/explosive = 1.0/1.0/1.0;
  fire = 2.0/0.5/0.5; electric = 0.5/2.0/0.5; corrosive = 0.5/0.5/2.0.
- Targeting: nearest in range, except FIRE prefers outer HP, SHOCK outer SHIELD,
  CORROSIVE outer ARMOR; fallback nearest; ties go to the oldest enemy.
- Explosive: picks a target, then damages **every** enemy within 1.3 cells of the
  target position (each with its own layer multipliers + slag amp). Beams:
  `kind=0` line beam (life 0.15 s) from tower to target, with `damageType` set to
  the tower's type; `kind=1` explosion ring centred on the target with
  `radius = aoeRadius`, life 0.30 s. `Beam.damageType` lets the renderer map
  colours (normal off-white, fire orange, electric cyan, corrosive acid green,
  slag purple, explosive amber).
- On-hit statuses are applied only to surviving targets of **direct** hits;
  burn/acid ticks and explosions never re-apply them.

### Status effects (constants in data.c as extern consts)

- Burn: each fire hit `burnStacks = min(5, +1)` and `burnTimer = 2.5 s`. Each step:
  damage `stacks * 1.0 dps * dt` of type FIRE to the outer layer (fire multipliers),
  then timer -= dt; at <= 0 stacks are cleared. Refresh, not stack, on hits.
- Stun: `stunTimer = max(stunTimer, 0.30)`; while > 0 the enemy does not advance.
- Slag: every slag hit sets `slagTimer = 4 s`; while > 0 damage taken x1.5 (all
  sources) and movement x0.65.
- Acid puddles: every corrosive direct hit drops one at the target position;
  radius 0.7, life 5 s, 2 dps of type CORROSIVE to every enemy inside (corrosive
  multipliers, own slag amp). At most `MAX_PUDDLES` (16); when full the round-robin
  `puddleNext` slot (oldest) is replaced. Puddles tick in the status phase.
- Puddles keep simulating only in WAVE (like everything else); they freeze in BUILD.

---

## 5. Maze road (unchanged from v0.2)

- `core_build_path()` = 4-connected BFS from entrance (0,12) to exit (25,12) with
  occupied cells as walls; initial empty board = 26 cells / 25 segments (straight
  row 12). Neighbour order up/down/left/right keeps it deterministic.
- Towers MAY be built on road cells (maze mechanic); only entrance/exit are off
  limits, and a build that would seal the exit is refused
  (`"that would block the path"`, no gold spent, no tower left).
- Every build/sell rebuilds `g->path` and re-projects all live enemies onto the new
  polyline (nearest point, segment index + t); no teleports back to the entrance.
  The renderer must re-read `g->path` every frame.
- `game_can_place_at()` runs the reachability BFS without mutating the game; safe
  to call per frame and it does not check gold.

---

## 6. Enemy speed (deviation from the LÖVE original, kept)

`speed_cells_per_second = (20 + (wave-1)) * 0.25`, elite x1.8. Absolute, not
normalised to path length, so a longer maze buys real tower time. Wave 1 = 5
cells/s = 5 s across the initial 25-cell road; wave 25 = 11 cells/s = 2.27 s.
`SPEED_SCALE` (`kEnemySpeedScale = 0.25f`, data.c) is the only pacing knob.

---

## 7. Economy / waves / upgrades (unchanged rules)

- 25 waves, 20 lives, 150 starting gold. Wave size `3 + 2*wave` including elites.
  Elites: wave%25 -> 10, %10 -> 3, %5 -> 1. Spawn interval
  `max(0.8, 3.0 - wave*0.05)`; first spawn one interval after wave start.
- Kill reward 10 gold (elite or not). Leak costs exactly 1 life, pays nothing.
- Victory after clearing wave 25 (wave becomes 26, PHASE_VICTORY). 0 lives ->
  PHASE_GAMEOVER.
- Upgrade curve `20 * 2^(L-1)`, max level 10; damage x2 per level; sell 75%.

---

## 8. Messages (front end copy)

Placement failures, in check order: `"can't build now"`, `"can't build there"`,
`"can't build on the entrance"`, `"can't build on the exit"`, `"cell occupied"`,
`"tower limit"`, `"that would block the path"`, `"not enough gold"`.
Upgrade/sell: `"no tower selected"`, `"max level"`, `"not enough gold"`,
`"can't build now"` / `"can't upgrade now"` / `"can't sell now"` in terminal phases.
Message timer is 1.5 s; the core clears the text itself when it expires.

Renderer-facing per-entity data: `Tower.damageType/level/cooldown/animPhase`,
`Enemy.layers[]` (type/hp/maxHp, outermost first) + `hitFlash` + `burnStacks/
burnTimer/stunTimer/slagTimer`, `Beam.kind/radius/damageType/life`,
`Puddle.x/y/radius/life/maxLife/dps`. Layer colours and ring radii are in
elements-spec.md section 1/3 (renderer side).

---

## 9. Sim / balance evidence (tools/sim.c)

Three scripted players at fixed 1/60 s steps:
- mixed (default): cycles GUN/FIRE/SHOCK/CORROSIVE + occasional SLAG/BLASTER,
  builds 36 support towers beside the road, upgrades the cheapest.
- `--maze`: same army, but first plants walls on the road (maximum-lengthening
  placement) until 60 towers / gold runs out - the intended maze play.
- `--plain`: GUN-only army (neutral damage vs every layer) - the "ignore elements"
  baseline.

**HP tuning: `kEnemyHpScale = 2.0f`** (data.c, single constant, damage table and
economy untouched). With the literal spec HP, both mixed and plain-GUN clear
25/25 with 0 leaks, i.e. the whole layer/element system is cosmetic. Probe results:

| enemy HP | mixed AI | plain-GUN AI |
|---|---|---|
| x1.0 (spec) | VICTORY 25/25, 0 leaks | VICTORY 25/25, 0 leaks |
| **x2.0 (shipped)** | **VICTORY 25/25, 1 leak** | **GAMEOVER wave 3, 0 kills, 20 leaks** |
| x3.0 | VICTORY 25/25, 1 leak | GAMEOVER wave 3, 0 kills |
| x4.0 | GAMEOVER wave 4 | GAMEOVER wave 3 |

Final results at `kEnemyHpScale = 2.0f`, `SPEED_SCALE = 0.25f`, 4 seeds
(1337 / 1 / 424242 / 999999), all identical verdicts:

| mode | result | lives | leaks | kills | towers | road | CPU |
|---|---|---|---|---|---|---|---|
| mixed | VICTORY 25/25 | 19-20/20 | 0-1 | 724-725 | 36 | 25 | ~0.1 s |
| maze | VICTORY 25/25 | 20/20 | 0 | 725 | 36 | 49 | ~0.13 s |
| plain-GUN | GAMEOVER wave 3 | 0/20 | 20 | 0 | 7 | 25 | <0.01 s |

So: a reasonable mixed army clears 25 waves with a small leak margin; the maze
variant clears perfectly (extra road time); ignoring the element system loses in
wave 3. Setting `kEnemyHpScale` back to `1.0f` restores the literal spec numbers
(and makes the game trivially winnable with GUNs only).

The mixed AI currently goes all-in on a straight-road cover of 36 towers and does
not build a maze; that is why its margin is one leak worse than the maze run. No
other tuning was applied: spawn counts, kill rewards, starting gold, upgrade costs,
damage table and status numbers are exactly the spec.
