# Elemental Defense v0.3 — Borderlands-style damage model

This is the design sheet for the v0.3 combat overhaul: five Borderlands 2 damage
elements plus "normal", layered enemies, and status effects. It is the source of
truth for the core, the 3DS front end and the docs. Numbers here are deliberate
and can be tuned in `source/core/data.c` afterwards.

## 1. Enemies: concentric layers

An enemy is a stack of **1 to 3 concentric rings** (layers). Each layer is one of:

| Layer | Colour | Notes |
|---|---|---|
| **HP** | red (0.95, 0.18, 0.18) | plain health |
| **SHIELD** | blue (0.28, 0.55, 1.00) | typical energy shield |
| **ARMOR** | yellow (0.95, 0.78, 0.18) | plated armour |

- `layers[0]` is the **outermost** ring and takes damage first. Damage that
  depletes a layer carries the excess into the next layer, with that layer's own
  multiplier applied to the carry-over.
- Layer count by wave: **waves 1–3 = 1 layer, waves 4–8 = 2 layers, waves 9+ =
  3 layers**. Elites get one extra layer (capped at 3).
- Layer types: 1-layer enemies get a random type; 2-layer enemies get two
  *different* types; 3-layer enemies get all three types in random order.
- Total HP for the wave is `(15 + (wave - 1) * 3) * (elite ? 2.5 : 1)` and is
  split equally across the layers.
- Movement is unchanged: `(20 + (wave - 1)) * 0.25` cells/second, elite ×1.8.
  Slagged enemies move at 65%; stunned enemies do not move at all.

Renderer: draw the layers outside-in as concentric circles with radii
**6.0 / 4.2 / 2.4 board pixels** (board cell = 15 px), ring thickness ~1.5 px,
alpha `0.35 + 0.65 * hpFraction`; destroyed layers are not drawn. Elites get an
extra white outline ring; slagged enemies a pulsing purple ring; stunned enemies
flash white.

## 2. Damage types & tower roster

Six towers, one damage type each. Same range for all: **2.142857 cells** (60
original pixels). Upgrades double damage per level (levels 1–10), use the
existing cooldown table, and cost the existing `20 * 2^(L-1)` curve.

| Tower | Type | Cost | DMG (L1) | Cooldown | Effect |
|---|---|---|---|---|---|
| **GUN** | normal | 20 | 3 | 0.80 s | none |
| **FIRE** | fire | 20 | 3 | 0.80 s | adds a burn stack |
| **SHOCK** | electric | 20 | 3 | 0.80 s | stuns for 0.30 s |
| **CORROSIVE** | corrosive | 20 | 3 | 0.80 s | leaves an acid puddle |
| **SLAG** | slag | 15 | 1 | 0.80 s | applies slag (see below) |
| **BLASTER** | explosive | 30 | 9 | 2.40 s (3× slower) | AoE blast, radius 1.3 cells |

### Multipliers (damage type × layer type)

| | HP | SHIELD | ARMOR |
|---|---|---|---|
| **normal** | 1.0 | 1.0 | 1.0 |
| **fire** | **2.0** | 0.5 | 0.5 |
| **electric** | 0.5 | **2.0** | 0.5 |
| **corrosive** | 0.5 | 0.5 | **2.0** |
| **slag** | 1.0 | 1.0 | 1.0 (low base damage) |
| **explosive** | 1.0 | 1.0 | 1.0 |

Slagged enemies take **×1.5 damage from everything** (direct hits, burn, acid,
blast). Multiply layer multiplier by the slag multiplier.

### Status effects

- **Burn (fire):** every fire hit adds one stack (max **5**), refreshes
  `burnTimer = 2.5 s`. Each stack deals **1.0 damage/second** to the current
  outermost layer, using fire multipliers. Stacks and timer are per enemy.
- **Stun (electric):** `stunTimer = max(stunTimer, 0.30 s)`; while > 0 the enemy
  does not advance along the road. Stun does not otherwise change damage.
- **Acid puddle (corrosive):** every corrosive hit drops a puddle at the
  enemy's position: radius **0.7 cells**, lifetime **5 s**, deals **2.0
  damage/second** to every enemy inside (applied with corrosive multipliers to
  their current outer layer). Max 16 live puddles, oldest replaced.
- **Slag:** every slag hit sets `slagTimer = 4 s` (refresh). While slagged:
  movement ×0.65 and damage taken ×1.5.
- All effects are refreshed on hit, not stacked, except burn stacks.

### Targeting

Nearest enemy in range is the default. Elemental towers prefer targets whose
**outermost layer** matches their strength:

| Tower | Prefers outer layer |
|---|---|
| GUN / SLAG / BLASTER | nearest (no preference) |
| FIRE | HP |
| SHOCK | SHIELD |
| CORROSIVE | ARMOR |

If no enemy in range has the preferred outer layer, fall back to nearest. Ties
go to the oldest spawned enemy (lowest index).

### Explosive (BLASTER) specifics

Picks a target like the others, then damages **every enemy within 1.3 cells of
the target's position** for its damage (normal multipliers vs each layer, slag
amplifier applied). Visual: expanding explosion ring rather than a beam.

## 3. UI / rendering requirements

- **Tower palette:** six buttons (2 rows × 3, or 6 across if it fits) with a
  distinct icon, name and cost per damage type. Selected one highlighted.
- Icons: GUN = grey barrel/bullet, FIRE = flame, SHOCK = lightning bolt,
  CORROSIVE = dripping acid, SLAG = purple crystal, BLASTER = bomb.
- **Selected tower panel:** name, level, DMG/RNG/DPS, and the effect line, e.g.
  `BURNS · 2x HP`, `STUNS · 2x SHIELD`, `ACID PUDDLE · 2x ARMOR`,
  `SLAG: +50% DMG TAKEN`, `AOE 1.3`.
- **Incoming preview:** counts of HP / SHIELD / ARMOR layers in the next wave
  (with coloured dots), plus elite count.
- **Enemy rendering:** see §1; puddles drawn as translucent green blobs with a
  brighter rim; burn shown as small orange motes on the enemy; stun as a white
  flash; slag as a purple outline.
- Beam colours: normal off-white, fire orange, electric cyan, corrosive acid
  green, slag purple, explosive amber explosion ring.
- HUD/menus otherwise unchanged.

## 4. Balance guardrails for the core dev

- Keep 25 waves, 20 lives, 150 starting gold, current kill rewards, spawn
  counts, and sell/upgrade rules.
- The headless sim must stay winnable with a mixed-tower AI; if the layer
  system makes it too hard, adjust enemy HP/speed first (not the damage table).
- `SPEED_SCALE` stays 0.25 unless the sim says otherwise.

## 5. API summary (see `include/game.h`)

`DamageType` (6) replaces the old `Element`; enemies use `EnemyLayer layers[3]`
with `layerCount`; `Game` gained `Puddle puddles[16]`, `puddleNext`, and
`layerQuota[3]`; `Beam` gained `kind` and `radius`; `TowerSpec` gained
`damageType` and `aoeRadius`; `game_wave_preview()` now reports HP/SHIELD/ARMOR
layer counts; `game_damage_multiplier(damageType, layerType)`.
