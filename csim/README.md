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
  src/ui.c              the whole UI, written ONCE against gfx.h
  src/gfx_raylib.c      backend: raylib
  src/gfx_sdl3.c        backend: SDL3 (SDL_Renderer + debug text)
  src/gfx_glfw.c        backend: GLFW + legacy OpenGL (+ stb_easy_font)
  src/stb_*.h           vendored single-header libs (font, PNG write)
  src/main.c            entry point + --backend selection
  src/main_headless.c   runs the core and prints a report (plain gcc, no deps)
  CMakeLists.txt        builds raylib (fetched) + sdl3/glfw (if installed)
```

## Rendering (one UI, three windowed backends)
The entire UI lives in `ui.c`, written once against the `gfx.h` interface, so
the backends can't drift. Pick one at runtime:
```sh
./build/csim                     # raylib (default)
./build/csim --backend sdl3      # SDL3
./build/csim --backend glfw      # GLFW + OpenGL
./build/csim --help              # lists compiled-in backends + controls
```
Controls: drag (right-mouse) pan · wheel zoom · click a citizen to inspect ·
`g` god mode · `1`-`8` tool · `j` jail · `f` factions · `l` legend · `Tab`
feed · arrows/PgUp/PgDn browse list · Space pause · `1`/`2`/`3` speed · Esc.
Zoom in and buildings show a type glyph: `H` home, `$` shop, `O` office,
`B` bar, `+` church, `P` police.

Env: `CSIM_ZOOM=N` initial zoom · `CSIM_DEMO=1` open panels · `CSIM_SHOT=path`
dump a screenshot.

Dependencies:
- **raylib** (always on, default) — fetched + built by CMake; needs GL/X11 dev
  headers (`libgl1-mesa-dev xorg-dev libxrandr-dev libxinerama-dev
  libxcursor-dev libxi-dev`).
- **SDL3** — `sudo apt install libsdl3-dev` (enables `--backend sdl3`).
- **GLFW** — `sudo apt install libglfw3-dev` (enables `--backend glfw`).

CMake compiles in whichever are present; missing ones are simply unavailable at
runtime. Under WSL, run GUI apps with `export DISPLAY=:0` (WSLg provides the
X server).

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
