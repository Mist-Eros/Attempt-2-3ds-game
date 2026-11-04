# Attempt 2: Elemental Defense for the 3DS

A tower defense game I originally wrote in LÖVE2D, rebuilt from scratch in C so it actually runs
properly on a Nintendo 3DS.

The first attempt ran the LÖVE version through LovePotion. It booted! And then crawled at a few
frames per second with half the playfield off-screen and touch controls fighting LÖVE's mouse
emulation. Rather than keep patching a compatibility layer that was never going to behave, this repo
is the native rewrite: same game, same numbers, drawn with `citro2d`.

The old attempt for posterity: [tower-defense-3ds-psvita-project-attempt](https://github.com/Mist-Eros/tower-defense-3ds-psvita-project-attempt)

![Elemental Defense title screen](docs/screenshots/menu.png)

## What the game is

A grid tower defense on a 26x13 board. Creeps crawl along a road from the left edge to the right, you
build towers to stop them, and every kill pays for the next tower.

The important bit: **you build on the road, not just next to it.** Every tower you drop on the path
makes the creeps recalculate their route around it, so the game is about walling off the short way and
forcing them through a long winding detour that keeps them in range of more towers. The route is a BFS
shortest path and gets recomputed after every build and every sell; creeps already on the road follow
the new route. Block the road completely and the game won't let you place the tower.

Three elemental towers — **Fire**, **Water**, **Forest** — with a rock-paper-scissors damage table:

| | vs Fire | vs Water | vs Forest |
|---|---|---|---|
| **Fire** | x1 | x0.5 | **x2** |
| **Water** | **x2** | x1 | x0.5 |
| **Forest** | x0.5 | **x2** | x1 |

Fire melts forest, water douses fire, forest drinks water. Enemies come in the same three elements,
mixed into each wave, plus elite versions on every 5th wave. Towers upgrade ten levels, damage doubles
with every level, and the cooldown tightens as you go. You have 20 lives: a creep that reaches the end
of the road takes one with it. Clear 25 waves and you win.

Top screen is the battlefield, bottom screen is the touch panel. The game is paused between waves so
you can build as long as you like before calling the next one in.

## Screenshots

Top screen above, touchscreen below. The menu is a capture of the actual `.3dsx` running in Azahar;
the two gameplay shots come from the game's PC preview harness, which renders through the same
`game.h`/`gfx.h` frame code as the 3DS backend (so the layout is exactly what the console draws).

| Menu (in Azahar) | Build phase, roadway rerouted | Creeps walking the detour |
|---|---|---|
| ![menu](docs/screenshots/menu.png) | ![build phase](docs/screenshots/build.png) | ![combat](docs/screenshots/wave.png) |

## Playing it

Grab `ElementalDefense.3dsx` from the [releases page](https://github.com/Mist-Eros/Attempt-2-3ds-game/releases), drop it in `/3ds/` on your SD
card, launch it from the Homebrew Launcher. Needs a 3DS with homebrew (Luma3DS/CFW or any entry
point that can run `.3dsx` files).

### Controls

| Input | Action |
|---|---|
| Touchscreen | all the buttons on the bottom screen (tower type, upgrade, sell, next wave, pause) |
| D-pad | move the build cursor on the battlefield |
| A | place the selected tower |
| X / Y | upgrade / sell the selected tower |
| L / R | cycle tower type |
| Start | pause |
| Select | change game speed (1x / 2x / 4x) |

Tapping a placed tower with your finger on the bottom screen isn't a thing yet (the battlefield is on
the top screen, which isn't touch) — the D-pad cursor is how you select one, or the panel shows the
tower you placed most recently.

Playing under an emulator (I use Azahar)? I map **WASD** to the d-pad, **U/H/J/K** to X/Y/B/A and
**Q/I** to L/R there; Start/Select are M/N.

## Status

Playable end to end: maze building on the road, 25 waves, all three towers, upgrades, lives, game
over, victory. It boots at a stable 60 fps in the Azahar emulator, which is where all my testing so far
happened — real-hardware input and GPU output are the next thing I'll verify on the actual console.
What's still missing:

- [ ] sound effects and music (the audio layer is stubbed, the original `.wav`s are still around)
- [ ] real-hardware playtest tuning — balance is currently the old LÖVE numbers on a hand-built road
- [ ] general polish: another pass on the menus, and the bottom panel starts to feel cramped at 320px

## Building it

You need [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the 3DS dev packages. On a
normal devkitPro install:

```sh
pacman -S 3ds-dev
cd Attempt-2-3ds-game
make
```

That produces `ElementalDefense.3dsx`, `ElementalDefense.elf` and the `.smdh` metadata. Copy the
`.3dsx` to your SD card and you're away. Every push, and every `v*` tag, also builds it in GitHub
Actions — the release attached here was produced by that workflow. (The Actions tab likes to claim
the runs are "queued" forever on this repo; they're not, that's just GitHub being weird about new
repos — check the artifacts.)

## How it's put together

The point of the rewrite was to not repeat the LÖVE mistake of tangling rendering, input and game rules
into one file. So:

```
source/core/     portable C99 game simulation - no 3DS headers, runs on a PC
source/plat_3ds.c citro2d backend for the two screens
source/render.c  top screen: board, towers, enemies, effects
source/ui.c      bottom screen: build panel, upgrade/sell, wave control
source/app.c     the scene state machine and input → action mapping
source/main.c    aptMainLoop / hid polling / frame timing
tests/           the core's unit tests (32 of them), built and run on the host
tools/sim.c      headless auto-play balance sim
tools/preview/   a tiny PC software renderer that dumps PNG frames - my "emulator"
```

The core is plain C99 with no dynamic allocation, everything in fixed pools. It's deterministic from
a seed, which makes the tests and the sim reproducible. `tests/` and `tools/sim.c` build with any
normal C compiler; `tools/preview` re-implements `gfx.h` with a software rasteriser so I can iterate
on layout without touching hardware. The 3DS build uses the same `game.h` + `gfx.h` contracts.

Run the tests:

```sh
make -C tests CC=cc test     # any C compiler
```

## Why rewrite instead of fixing the Lua one

The LÖVE version has all the actual game design in it — tower stats, wave composition, upgrade costs —
and I kept that (there's a full port spec in [`docs/design.md`](docs/design.md) with the original
numbers extracted line by line). What I didn't keep is the rendering and input layer. LÖVE on the 3DS
was never going to push a 1280x720 canvas of shapes at a decent framerate, and the touch/keyboard
mismatch meant the game was unplayable on the console it was made for. Writing the game once in C
against libctru is less work than fighting a compatibility layer forever.

The original Lua game also had a few... creative ideas: there was no lose condition, enemies reaching
the end actually *paid you* gold, and waves auto-chained with no build phase. Those are fixed here
(see `docs/design.md` §14 for the full list of deliberate changes).

## Credits

- [devkitPro](https://devkitpro.org/), libctru, citro3d and citro2d — without these, no 3DS homebrew.
- [font8x8](https://github.com/dhepper/font8x8) by Daniel Hepper (public domain) for the bitmap font.
- The original Elemental Defense was my own LÖVE2D project.
