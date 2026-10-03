# Emergent City — C port (proof of concept)

A C port of the sim's data-oriented core, to gauge the effort of a full POSIX-C
port with a portable graphics UI. It ports the real logic — **Needs decay,
Personality (Big Five + traits and all derived weights), and the UtilityAI
action scorer** — plus a minimal world/agent/time loop, deterministic PRNG, and
a raylib renderer.

## Layout
```
csim/
  src/rng.h            PCG32 deterministic PRNG (header-only)
  src/sim.h / sim.c    the ported core (no graphics deps)
  src/main_headless.c  runs the core and prints a report (plain gcc, no deps)
  src/main.c           raylib renderer (tiles + agents + HUD, pan/zoom)
  CMakeLists.txt       builds both; fetches raylib if not installed
```

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
