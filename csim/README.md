# Emergent City — C core

A POSIX-C reimplementation of the Python/pygame original that has since grown **far
beyond it**. It keeps the original's skeleton — Needs, Big-Five Personality, Utility-AI,
Relationships, Memory, Events, Factions, a basic Economy and the world/agent/time loop —
and adds a city's worth of systems on top (see the [top-level README](../README.md) for
the full tour):

- **Crime underworld** — career escalation, a wholesale→dealer→user **drug trade** with
  turf/customer retaliation, extortion, serial killers, wanted/jail (severity-scaled
  hunts + sentences), jail gangs, injuries/treatment.
- **Factions & warfare**, **law & justice** (crackdowns, trials/acquittals, corruption +
  oversight, funding-scaled police force & catch rate), a deeper **economy** (occupations,
  wages, landlords/rent with **inheritance on death**, credit/debt, craft skill).
- **Governance & balance (default on)** — progressive income + wealth (stock) taxes, a
  subsistence welfare floor, source caps (rent cap + debt-interest cap), economic
  stabilizers (interest/price dampers; survival-benefit grant off by default), and
  always-on stability guardrails: a city that counterweights its own inequality while
  keeping a real poor class. Disable any piece with `--no-taxation` / `--no-source-caps`
  / `--no-justice` / `--no-stabilizers`.
- **Knowledge/tech**, **culture** (faith/language/education), a full **life cycle**
  (marriage→birth→aging→death, family tree).
- **Perception** (field-of-view + hearing), **A\*+JPS pathfinding** + flow fields,
  **FastNoiseLite** worldgen.
- **Socioeconomics** (households, occupation pay tiers, rich/poor neighbourhoods,
  wealth-scaled crime, police-bias), **weather** (deterministic temp/rain/fog with
  crime-cover + heating effects).
- A **metrics CSV + live web dashboard**, and **session recording → OpenSearch →
  byte-identical replay** with a GUI pause-to-edit fork.

Deterministic PCG32 RNG (bit-reproducible from the seed), binary save/load, a raylib
GPU-window renderer, and optional **LLM-driven decisions** (libcurl + cJSON on a
background thread so the sim never blocks). The **governance/balance stack** (progressive
taxation, welfare, source caps, justice, funding-scaled policing) and organic noise worldgen
are **on by default** — the baseline is a governed, balanced city; everything is a tunable
knob and disable-able (`--no-taxation`, `--no-source-caps`, `--no-justice`, …). The remaining
newer systems (perception, weather, biomes, neighbourhoods, …) are **opt-in, default off**.
Every configuration is recorded in the manifest, so any run stays **byte-identical** on replay.

## LLM decisions
Set `OPENROUTER_API_KEY` (any value locally) to enable Qwen consults; the client
reads `OPENROUTER_BASE_URL` (default the local Qwen server) and `OPENROUTER_MODEL`.
A worker thread does the HTTP calls; results are applied to agents on later ticks
(deferred override), rate-limited and gated to "interesting" agents — the HUD
shows the running call count. Needs `libcurl` + `libcjson` at build time
(auto-detected); without them the sim runs purely rule-based.

## Layout
```
csim/
  src/rng.h             PCG32 deterministic PRNG (header-only)
  src/sim.c             leaf systems + all config knobs (needs/personality/utility/rels/mem/
                        events/pathing; occupation pay, weather accessors, …)
  src/systems.c         crime/underworld, factions, law, economy, knowledge, culture, perception
  src/world.c           worldgen (+FastNoiseLite), population, tick loop, households, weather,
                        metrics export, save/load
  src/sim.h             full data model + API (no graphics deps)
  src/viz.h / viz.c     god-mode actions + render helpers (now in the core, used by replay)
  src/record.c/.h       session recording + byte-identical replay + GUI player/fork
  src/os_client.c/.h    OpenSearch archive (via Data Prepper) + session recall (libcurl/cJSON)
  src/harness.c         native common-random-numbers harness (csim_harness)
  src/llm.c/.h          background LLM decision client (libcurl + cJSON) or no-op stubs
  src/FastNoiseLite.h   vendored MIT noise header (worldgen + weather)
  src/gfx.h             gfx abstraction (drawing + input + backend vtable)
  src/ui.c              the whole UI, written against gfx.h
  src/gfx_raylib.c      the raylib backend (GPU window; TTF UI font)
  src/main.c            GUI entry point
  src/main_headless.c   headless run / report + --metrics / --replay-session / --rerun
  CMakeLists.txt        builds csim (raylib, fetched), csim_headless, csim_harness
```

## Rendering (raylib GPU window)
The UI lives in `ui.c`, written against the small `gfx.h` interface and rendered
by the raylib backend. (The abstraction stays so another backend could be
re-added just by providing a `GfxBackend` vtable.)
```sh
./build/csim            # run
./build/csim --help     # controls + env vars
```
Controls: drag (right-mouse) pan · wheel zoom · click a citizen to inspect ·
`E` city dashboard · `T` tuning panel · `K` family tree · `O` map overlay · `g` god mode
(`1`-`8` tool) · `j` jail · `f` factions · `c` crime-watch · `l` legend · `a` ASCII
(Dwarf-Fortress) mode · `Tab` feed · Space pause · `1`-`6` speed (1x–6x) · `q`/Esc quit. Zoom in and buildings show a type glyph: `H` home,
`$` shop, `O` office, `B` bar, `+` church, `P` police. Small UI text uses an
antialiased TTF (DejaVu Sans) for legibility, scaled for the display.

**ASCII mode** (`a`, or `CSIM_ASCII=1`): a Dwarf-Fortress-style render — each
cell a dark tile-tinted background with a brighter CP437 glyph: `,` grass,
`.` road, `≈` water, `♣` park, `⌂` home, `$` shop, `O` office, `B` bar,
`+` church, `P` police; citizens are `☺` (`☻` police).

**Image tilesets** (`--tileset NAME`, or a path to a `.png`): replace the font
glyphs with a tileset image (implies ASCII/tile view). Assets are **not
bundled** (licensing) — fetch the CC0 ones with `tools/fetch-tilesets.sh` (into
`csim/tilesets/`, gitignored); each name resolves to a file under the tileset
dir (`CSIM_TILESET_DIR`, default `./tilesets`), and a missing file prints where
to get it and falls back to font glyphs.
- **CC0, CP437 grid:** `camashu` — [DorfFortressTileSet](https://github.com/Camashu/DorfFortressTileSet)
- **DF-wiki CP437 (verify each license):** `curses` `phoebus` `anikki` — [tileset repository](https://dwarffortresswiki.org/Tileset_repository). Download the tileset *sheet* image (a 16×16 glyph grid), not a screenshot.
- **CC-BY 4.0, per-type sprites:** `dawnlike` — [DawnLike](https://opengameart.org/content/dawnlike-16x16-universal-rogue-like-tileset-v181) by DawnBringer & DragonDePlatino. The fetch script composites one tile per type into `dawnlike.png`. **Attribution required** if you distribute.
- **CC0, sprites (approx mapping):** `kenney` `kenney-indoor` `kenney-caves` `kenney-1bit` — [Kenney via OpenGameArt](https://opengameart.org/content/roguelikerpg-pack-1700-tiles)

**Cell geometry is native/automatic.** CP437 sheets are always a 16×16 grid, so
the cell size (even non-square, e.g. Camashu's 20×24) is derived from the image.
Semantic sheets use their per-set cell + 1px spacing. Override anything with
`CSIM_TILESET_CELL` / `CSIM_TILESET_SPACE` / `CSIM_TILESET_MARGIN`.

CP437 sheets map each tile to its code-page-437 glyph (tinted) — the authentic
DF look, **recommended** (`camashu`). `dawnlike` gives real **per-type sprites**
(doors/jars/trees/characters) via a composited atlas. The Kenney sheets are
*environment* tilesets with no canonical per-type tile and no person sprite, so
their mapping is approximate.

Env: `CSIM_UI=N` UI scale (default auto from monitor height — ~2.0 on 4K) ·
`CSIM_ZOOM=N` initial zoom · `CSIM_ASCII=1` start in ASCII mode ·
`CSIM_DEMO=1` open panels · `CSIM_SHOT=path` screenshot · `CSIM_BENCH=N`
uncapped N-frame FPS benchmark.

Dependency: **raylib** is fetched + built by CMake (no apt package); it needs
GL/X11 dev headers (`libgl1-mesa-dev xorg-dev libxrandr-dev libxinerama-dev
libxcursor-dev libxi-dev`). Under WSL, run GUI apps with `export DISPLAY=:0`
(WSLg provides the X server).

Note: build single-threaded in this project — `cmake --build build -j1`.

## Build & run the headless core (no dependencies)
Runs the full core with just a C compiler + libm (LLM/OpenSearch compile as no-op stubs
without libcurl/cJSON):
```sh
cc -O2 -o csim_headless src/main_headless.c src/sim.c src/systems.c src/world.c \
   src/viz.c src/record.c src/os_client.c src/llm.c -lm
./csim_headless
./csim_headless --days 40 --metrics m.csv            # 40-day run, balance CSV
./csim_headless --replay-session run.sess            # byte-identical replay (checksum-verified)
```

## Build & run the graphical version (raylib)
raylib is **not** in the Ubuntu 26.04 apt repos, so you don't install it — CMake
fetches and builds it automatically (the GL/X11 dev headers it needs are already
present). Requires network on first configure:
```sh
cmake -B build -S . && cmake --build build -j
./build/csim
```
Controls: drag pan · wheel zoom · Space pause · 1-6 speed · Esc quit.

If you'd rather vendor raylib yourself:
```sh
git clone --depth 1 -b 5.5 https://github.com/raysan5/raylib.git
cd raylib/src && make PLATFORM=PLATFORM_DESKTOP && sudo make install
```

## Relationship to the Python original
The Python/pygame sim at the repo root is the **reference**; this C core reimplements its
concepts and then goes well past them. Nothing links or runs the Python — it's a clean
reimplementation (dataclasses → structs, scoring functions → C functions), far faster, and
everything the original left as "future work" (A\*, the crime/jail/faction systems, LLM via
libcurl+cJSON, save/load) is **done**, alongside the many systems listed up top that the
original never had.

## Determinism & reproducibility
The core is bit-reproducible from its seed (headless always; GUI with `--fixed-step`; LLM
off). `--record <file>` captures a session (seed + config + tick-stamped interventions) for
**byte-identical replay** (`--replay-session`, self-verified by a World checksum), optionally
archived to OpenSearch via Data Prepper and recalled with `--replay-session os:<id>`. See the
[top-level README](../README.md) for the feature tour and flags, and
[`tools/dashboard/`](../tools/dashboard/) for the live metrics dashboard.
