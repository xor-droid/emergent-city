# Emergent City — C port (proof of concept)

A C port of the sim's data-oriented core, to gauge the effort of a full POSIX-C
port with a portable graphics UI. It ports the real logic — **Needs decay,
Personality (Big Five + traits and all derived weights), and the UtilityAI
action scorer** — plus a minimal world/agent/time loop, deterministic PRNG, and
a raylib renderer.

## Layout
```
csim/
  src/rng.h             PCG32 deterministic PRNG (header-only)
  src/sim.h / sim.c     the ported core (no graphics deps)
  src/viz.h / viz.c     shared render helpers (tile colors, HUD string)
  src/main.c            entry point + --backend dispatch
  src/main_headless.c   runs the core and prints a report (plain gcc, no deps)
  src/backend_raylib.c  raylib renderer (GPU window)
  src/backend_notcurses.c  notcurses terminal renderer (pixel/sextant/ascii)
  CMakeLists.txt        builds raylib (fetched) + notcurses (if installed)
```

## Rendering backends (one binary, pick at runtime)
Two best-of-breed renderers: a GPU window (raylib) and a terminal renderer
(notcurses) that spans ascii -> sextant -> true pixel graphics.
```sh
./build/csim --backend raylib                    # GPU window (default)
./build/csim --backend notcurses                 # terminal, auto pixel/sextant
./build/csim --backend notcurses --blit pixel    # force TRUE pixel graphics (Kitty/Sixel/iTerm2)
./build/csim --backend notcurses --blit sextant  # force 2x3 sub-cell blocks (works in tmux)
./build/csim --backend notcurses --blit ascii    # plain ASCII cells (works anywhere)
./build/csim --help                              # lists backends + blit modes
```
Controls (raylib): drag/arrows pan, wheel zoom, Space pause, 1/2/3 speed.
notcurses: arrows pan, Space pause, 1/2/3 speed, q quit.

`--blit` (notcurses only): `default|pixel|sextant|quad|half|braille|ascii`
(flag > `CSIM_NCBLIT` env > `default`). True pixel graphics need a capable
terminal **outside tmux** (Kitty/WezTerm/foot/recent Windows Terminal); tmux
falls back to sextants.

Backend dependencies:
- **raylib** — fetched+built by CMake (no apt package); needs GL/X11 dev headers
  (`libgl1-mesa-dev xorg-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev`).
- **notcurses** — `sudo apt install libnotcurses-dev`.

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
