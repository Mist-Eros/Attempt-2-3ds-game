# PC preview harness — screenshots & how to reproduce (scratch, not committed)

Host-only tooling: compiles the game's platform-independent `App` / `render.c` /
`ui.c` / `core` code against the software `gfx.h` backend (`plat_host.c`) and
dumps both 3DS framebuffers as PNGs. Fully headless — no emulator, no window.

> **v0.7.0 release-candidate round:** bottom screen is the redesigned toy UI
> (28 px stat-chip row, 2×3 rounded palette, mascot; top screen is a pure
> battlefield with no HUD), the campaign is **1000 waves**, towers have **no
> cap** (`MAX_TOWERS` is a storage bound of 400 for 336 buildable cells), and
> engine-3ds added compact `K`/`M` number formatting so late-game gold
> (real 1000-wave sim ends around **253,060**) and kills stay inside their
> chips. The `late` scenario now has **4 captures**. Current set:
> **126 PNGs (63 pairs)**, byte-identical between runs.

## Build & run

```
D:/devkitPro/msys2/usr/bin/make.exe -C tools/preview CC="D:/tools/zig/zig.exe cc"
tools/preview/preview.exe --scenario all     --out tools/preview/out
tools/preview/preview.exe --scenario late    --out tools/preview/out   # 1000-wave HUD only
```

Options: `--scenario menu|build|maze|wave|status|selected|gameover|victory|late|all`
(default `all`), `--frames N` (per-scenario cap, default 1800),
`--capture-every K`, `--out DIR`, `--seed S` (default 1337), `--help`.
`N` in `frame_<N>_top.png` / `frame_<N>_bottom.png` is the global capture index.

## Frame map (`--scenario all`)

| Capture # | Scenario | Frames (scenario-local) | What it shows |
|---|---|---|---|
| 0000–0005 | menu | 0/30/60/90/120/150 | Comic title screen, element cards, waving mascot; six animated moments |
| 0006–0010 | build | 5/24/42/55/59 | Six-type palette + stat chips; one tower of each type placed; pause overlay |
| 0011–0020 | maze | 7 … 984 | Straight road → tower on the road → BFS reroute → full six-type wall (path.length 25 → 39) → wave creeps on the detour |
| 0021–0040 | wave | 41 … 1545 | Forced wave 9: layered enemies, beams, explosion rings, wave progress panel |
| 0041–0049 | status | 38 … 310 | Layered enemy, stun flash, burn motes, acid puddle, slag outline, explosive ring |
| 0050–0056 | selected | 24 … 63 | Per-type effect lines (BURNS / ACID PUDDLE / SLAG / AOE / STUNS) + upgrade |
| 0057 | gameover | 22 | GAME OVER overlay (both screens) |
| 0058 | victory | 22 | VICTORY overlay (both screens) |
| **0059** | **late** | **9** | **Late-1 BUILD @ wave 900/1000, gold 172,360:** gold chip shows compact **`172K`** (no collision with the lives chip), `WAVE 900/1000`, `TOWERS 312`, `WAVE 900 OF 1000` on NEXT WAVE, `INCOMING HP ×80 / SHIELD ×80 / ARMOR ×80 / ELITE ×10`, packed 312-tower board, selected **GUN LV.9** panel with **`UPGRADE 5120G`** (the L9→L10 max-level upgrade label) |
| **0060** | **late** | **12** | **Late-2 BUILD @ wave 1000/1000, real end-of-run gold 253,060:** gold chip **`253K`**, `WAVE 1000/1000`, `TOWERS 312`, `WAVE 1000 OF 1000`, full INCOMING preview, `UPGRADE 5120G` |
| **0061** | **late** | **495** | **Late-3 WAVE @ wave 1000/1000:** big `WAVE 1000/1000` title, progress bar `CLEARED 23% LEFT 62`, `ALIVE 22 QUEUED 40 KILLS 52K` (compact kill counter), `LEAKS 6 TIME 391:05`, enemies on the packed board |
| **0062** | **late** | **498** | **Late-4 VICTORY:** internal `wave = 1001`, `totalWaves = 1000` → both screens clamp to `WAVE 1000/1000` (never 1001/1000); result overlay shows full-width `KILLS 52000`, `GOLD 253060`, `TIME 390:56` |

(`--scenario late` alone writes the same four pairs as frame_0000..0003.)

## Late-game (1000-wave) HUD verification

Hand-built fixture (scenario writes `Game`/`App` state directly; no campaign
play required), deterministic, with two realistic late-campaign balances:

- **Late-1:** `wave = 900`, `totalWaves = 1000`, `gold = 172360`, kills 48000,
  leaks 4, TIME 335:23, `lives = 20`, **312 towers**, selected tower **GUN LV.9**
  (occupancy/cursor at cell (0,0); `game_upgrade_cost()` = 5120).
- **Late-2:** `wave = 1000`, `gold = 253060` (the value the real 1000-wave sim
  ends with), kills 52000, leaks 5, TIME 390:56.
- `game_wave_preview()` at both wave 900 and 1000 prints exactly
  **hp=80 shield=80 armor=80 elites=10** (80 spawns = `3+2*wave` capped at 80,
  of which 10 are elites; all 3-layer, so 240 layers split evenly).
- WAVE capture fast-forwards ~8 s of the real wave-1000 simulation (temporary
  999-lives fixture value so the demo can't hit GAME OVER), then shows plausible
  mid-wave values; `progress_total = 80` comes from `App.waveEnemyTotal`, so
  `CLEARED 23% / LEFT 62` is self-consistent.

**Gold overflow fix verified.** `format_compact()` renders ≥1000 as `K`/`M`:
`172360 → "172K"`, `253060 → "253K"` (`4321 → "4.3K"`). Each compact string is
32 px scale 1, centred at x=36 in the 52 px gold chip (x 4–55) → **ends 4 px
inside the chip, 8 px before the lives chip at x=59. No collision, no clipping.**
4× crops: `out_zoom_late/chips_253K_4x.png`.

**UPGRADE button label fits.** Selected tower is GUN **LV.9**, i.e. the most
expensive upgrade (L9→L10). Current `kUpgradeCostByLevel[8] = 5120`, so the
button reads **`UPGRADE 5120G`** (13 glyphs = 104 px) inside the 150 px button
with ~23 px padding each side. (Note: the task brief expected 10,240G, but the
shipped curve is `20·2^(L-1)` → L9→L10 = **5120G**; 10,240 would be a
non-existent L10→L11 step. Even `UPGRADE 10240G` = 112 px would still fit the
same button, so the layout is safe either way.) 4× crop:
`out_zoom_late/upgrade_sell_4x.png`.

Other fit checks at the widest values — **no clipping or overlap found:**

- `WAVE 1000/1000` chip: 72 px text in the 88 px chip; `TOWERS 312`: 24 px in
  the 50 px chip; `WAVE 1000 OF 1000` on NEXT WAVE: 136 px in the 160 px button.
- INCOMING rows `HP ×80` / `SHIELD ×80` / `ARMOR ×80` / `ELITE ×10` all fit the
  112 px bracket (widest row `SHIELD x80` ends 22 px before the edge).
- Wave panel: title `WAVE 1000/1000` scale 2 = 224 px inside the 276 px panel;
  `CLEARED 23% LEFT 62`, `ALIVE 22 QUEUED 40 KILLS 52K`, `LEAKS 6 TIME 391:05`
  all inside the panel.
- VICTORY: both top and bottom clamp `shownWave = min(wave, totalWaves)` →
  `WAVE 1000/1000`; `KILLS 52000`, `GOLD 253060`, `TIME 390:56` fit the result
  panel/overlay (full numbers, not compacted there).

Verification output of the final run:

```
[late] BUILD wave=900/1000 towers=312 gold=172360; INCOMING hp=80 shield=80 armor=80 elites=10; selected GUN LV.9 upgrade 5120G
[late] BUILD wave=1000/1000 towers=312 gold=253060; INCOMING hp=80 shield=80 armor=80 elites=10
[late] WAVE wave=1000/1000 alive=23 queued=40 kills=52000 leaks=5 progress_total=80
[late] VICTORY internal wave=1001 totalWaves=1000 gold=253060 (expect clamp to 1000/1000)
--- scenario late: 4 capture(s), 0 failure(s), 0 identical-to-previous, 499 frame(s) simulated
9 scenarios: 0 capture failures, 126 PNGs, byte-identical on a second run (126/126)
```

## Harness changes (this round)

- `late` fixture updated: 4 captures (added BUILD @ wave 900 / gold 172,360),
  gold values 172,360 and 253,060, selected tower changed to **LV.9** so the
  panel shows the max-level upgrade label. Appended last in `--scenario all`;
  existing indices 0000–0058 unchanged.
- Rebuilt after engine-3ds's compact-number save (`ui.c` 01:49:03); source
  hashes checked before/between/after the two full capture runs (stable).
- No engine files touched; headless `preview.exe` only (no Azahar/GUI, no git).

## Backend notes

- `plat_host.c` implements every `gfx.h` function with a software RGBA8
  rasterizer (source-over alpha, strict clipping, pixel-snapped rects, lines,
  triangles, circles, 8x8 font). `gfx_present()` dumps the PNG pair via
  `stb_image_write.h` when a capture is armed; `host.h` is the driver-only
  capture/diagnostic side channel.
- Linking: `main.c plat_host.c ../../source/app.c ../../source/audio.c
  ../../source/render.c ../../source/ui.c ../../source/core/*.c`
  with `-I../../include -I../../source -I. -std=c99 -Wall -Wextra -O2 -lm`.
  `source/main.c`, `source/plat_3ds.c`, `source/audio_3ds.c` are excluded.
