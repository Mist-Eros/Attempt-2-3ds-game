# Attempt 2: Elemental Defense for the 3DS

A while back I made a tower defense game called **Elemental Defense** in LÖVE2D and tried to run it
on my 3DS with LovePotion. It technically booted. Then it ran at what felt like 4 fps with half the
screen cut off, and the touch controls were fighting the mouse emulation the whole time. Not fun.

So this is attempt two: the same game, rewritten from scratch in C for the 3DS. Native, no Lua
interpreter in the middle.

Old attempt for reference: [tower-defense-3ds-psvita-project-attempt](https://github.com/Mist-Eros/tower-defense-3ds-psvita-project-attempt)

## What the game is

A grid tower defense on a 26x13 board. Creeps walk from one side to the other, you build towers to
stop them. Towers are elemental — fire, water, earth, air and a few combinations — each with its own
range, damage and fire rate. Waves get bigger and meaner, money comes from kills, money buys more
towers and upgrades.

Touchscreen on the bottom for building and upgrading, top screen is the battlefield. That's the plan.

## Status

Work in progress, just started. Nothing playable yet — this repo is basically me setting up a
workbench. I'll update this section as it comes together.

- [x] repo, toolchain, build setup
- [ ] portable C game core (grid, pathfinding, towers, waves, economy)
- [ ] 3DS rendering with citro2d (top screen field + bottom screen UI)
- [ ] touch controls and D-pad fallback
- [ ] sound effects
- [ ] menus, game over, victory
- [ ] prebuilt `.3dsx` release so you don't have to compile it

## Controls (planned)

- **Touchscreen** — tap a cell to place a tower, tap a built tower to select it, then upgrade or sell
  from the bottom panel
- **D-pad** — move the grid cursor
- **A** — place / confirm
- **X** — upgrade selected tower
- **Y** — sell selected tower
- **L / R** — switch tower type
- **Start** — pause

## Building

You need devkitPro with the 3DS development packages (that's `libctru`, `citro3d` and `citro2d`):

```
pacman -S 3ds-dev
make
```

That spits out `ElementalDefense.3dsx`. Copy it to `/3ds/` on your SD card and launch it from the
Homebrew Launcher. Once the game is further along I'll attach a built `.3dsx` to the releases page so
nobody has to set up devkitPro just to try it.

## Why rewrite instead of fixing the Lua one

The Lua version has all of the actual game design in it, and I'm keeping that — the tower stats, wave
counts, upgrade costs, the whole thing. What I'm not keeping is the rendering and input layer. LÖVE on
the 3DS was never going to run a 1280x720 canvas full of stuff at a decent framerate, and I'd rather
spend my time writing the game once in C than fighting a compatibility layer.

The old code is still on my desktop and in the other repo if I need to compare numbers.
