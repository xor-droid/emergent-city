# Emergent City — C port

A full POSIX-C port of the simulation. Ports the real logic from the Python sim:
**Needs decay, Personality (Big Five + traits + all derived weights), UtilityAI,
Relationships (friends/rivals + decay), Memory, Crime + Wanted + Jail (with
severity-scaled hunts/sentences), police hunting + lie-low, Factions (gangs &
cults), Economy (cost of living), an Event feed, greedy pathing, and the
World/agent/time loop** — deterministic PCG32 RNG, binary save/load, two
renderers (raylib window + notcurses terminal), and **LLM-driven decisions**
(libcurl + cJSON against the local Qwen server, on a background thread so the
sim never blocks). Feature parity with the Python sim.

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

`--blit` (notcurses only): `sextant` (default) `|quad|half|braille|ascii|pixel|auto`
(flag > `CSIM_NCBLIT` env). Default is **sextant** (2x3 sub-cell, high-res, and
composes safely with the HUD/panels). `pixel` is **opt-in/experimental**: true
terminal pixel graphics (Kitty/Sixel) look best but share the text plane, which
aborts on some terminals — use it only if your terminal renders it cleanly.
`auto` lets notcurses negotiate the best blitter.

Backend dependencies:
- **raylib** — fetched+built by CMake (no apt package); needs GL/X11 dev headers
  (`libgl1-mesa-dev xorg-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev`).
- **notcurses** — `sudo apt install libnotcurses-dev`.
  - **Known issue (upstream notcurses input parser):** on startup notcurses
    interrogates the terminal (sends DA1 + capability queries) and **blocks with
    no timeout** until it parses the DA1 reply (`inputlayer_get_responses`,
    `in.c`). On some terminals its escape-sequence automaton overflows while
    parsing the batched query responses (`process_escape`, `in.c`:
    `amata.used <= buflen`). In a **debug** build that aborts; in a **Release**
    build (`NDEBUG`) the assert is skipped but the reply is silently mis-parsed,
    so the DA1 handshake never completes and init **hangs forever**. Seen on
    Kitty-protocol terminals (Kitty/Ghostty/WezTerm) and on some **WSL / Windows
    Terminal** setups.
  - **What we do about it:** `backend_notcurses.c` runs `notcurses_init` on a
    worker thread with a **4 s watchdog** (`CSIM_NC_INIT_TIMEOUT` to override). If
    the handshake stalls it restores the terminal and exits with a message
    instead of freezing — it never leaves you with a dead blank screen.
  - **If notcurses won't start in your terminal:** this is an upstream limitation
    we can't fix from the app. Use the GPU backend, which has full feature parity:
    ```sh
    ./build/csim --backend raylib
    ```
    Or try a terminal whose query replies notcurses parses cleanly (a plain
    `xterm` under WSLg often works; Kitty-protocol terminals and this WSL/Windows
    Terminal combo do not). Building notcurses from source in Release avoids the
    *abort* but not the *hang*:
    ```sh
    sudo apt install libunistring-dev libdeflate-dev
    cmake -B build -S . -DCSIM_FETCH_NOTCURSES=ON && cmake --build build -j1
    ```
  - **Diagnostics:** `CSIM_NCLOG=/tmp/nclog.txt ./build/csim --backend notcurses`
    logs terminal geometry, the init result (or `TIMED OUT`), per-frame
    blit/render return codes, and every key received.

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
