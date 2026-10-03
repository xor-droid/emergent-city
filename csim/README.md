# Emergent City — C port

A full POSIX-C port of the simulation. Ports the real logic from the Python sim:
**Needs decay, Personality (Big Five + traits + all derived weights), UtilityAI,
Relationships (friends/rivals + decay), Memory, Crime + Wanted + Jail (with
severity-scaled hunts/sentences), police hunting + lie-low, Factions (gangs &
cults), Economy (cost of living), an Event feed, greedy pathing, and the
World/agent/time loop** — deterministic PCG32 RNG, binary save/load, a raylib
GPU-window renderer, and **LLM-driven decisions** (libcurl + cJSON against the
local Qwen server, on a background thread so the sim never blocks). Feature
parity with the Python sim.

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
  src/sim.c             core leaf systems (needs/personality/utility/rels/mem/events/pathing/buildings)
  src/systems.c         crime+wanted+jail, factions, economy
  src/world.c           worldgen, population, tick orchestration, save/load
  src/sim.h             full data model + API (no graphics deps)
  src/viz.h / viz.c     render helpers (tile colors, legend, HUD string)
  src/gfx.h             gfx abstraction (drawing + input + backend vtable)
  src/ui.c              the whole UI, written against gfx.h
  src/gfx_raylib.c      the raylib backend (GPU window; TTF UI font)
  src/main.c            entry point
  src/main_headless.c   runs the core and prints a report (plain gcc, no deps)
  CMakeLists.txt        builds raylib (fetched)
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
`g` god mode · `1`-`8` tool · `j` jail · `f` factions · `l` legend · `a` ASCII
(Dwarf-Fortress) mode · `Tab` feed · arrows/PgUp/PgDn browse list · Space pause ·
`1`/`2`/`3` speed · `q` or Esc to quit. Zoom in and buildings show a type glyph: `H` home,
`$` shop, `O` office, `B` bar, `+` church, `P` police. Small UI text uses an
antialiased TTF (DejaVu Sans) for legibility, scaled for the display.

**ASCII mode** (`a`, or `CSIM_ASCII=1`): a Dwarf-Fortress-style render — each
tile a colored character on black (`,` grass, `.` road, `~` water, `"` park,
`H/$/O/B/+/P` buildings) and citizens as `@` (`P` police).

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
Proves the port works with just a C compiler + libm:
```sh
cc -O2 -o csim_headless src/main_headless.c src/sim.c -lm
./csim_headless
```

## Build & run the graphical version (raylib)
raylib is **not** in the Ubuntu 26.04 apt repos, so you don't install it — CMake
fetches and builds it automatically (the GL/X11 dev headers it needs are already
present). Requires network on first configure:
```sh
cmake -B build -S . && cmake --build build -j
./build/csim
```
Controls: drag/arrows pan · wheel zoom · Space pause · 1/2/3 speed · Esc quit.

If you'd rather vendor raylib yourself:
```sh
git clone --depth 1 -b 5.5 https://github.com/raysan5/raylib.git
cd raylib/src && make PLATFORM=PLATFORM_DESKTOP && sudo make install
```

## What this demonstrates
- The logic layer ports to C almost mechanically (dataclasses → structs,
  pure scoring functions → C functions). Same behavior, far faster.
- Graphics is a thin layer: pygame *is* SDL2, and raylib (or SDL2) gives the
  same rect/text drawing in portable C.

## What a full port would still add
- A* pathfinding (here movement is a greedy step) — direct to port.
- The crime/wanted/jail/faction/relationship systems — all pure logic.
- LLM integration: **libcurl** + **cJSON** (both already installed) replacing
  `requests`, with a pthread pool for the async, non-blocking calls.
- Save/load via cJSON; `.env` via getenv.
