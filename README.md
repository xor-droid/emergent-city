# 🏙️ Emergent City

> A living-city simulation. 150 citizens with personalities, needs, families, jobs, faiths and rap sheets — dropped into a procedural city where **nothing is scripted**. You watch the stories happen: marriages and murders, drug empires and turf wars, immigrant families, gang feuds, crackdowns, and citizens who grow up, grow old, and die.

![Language](https://img.shields.io/badge/core-C-blue)
![Renderer](https://img.shields.io/badge/renderer-raylib-green)
![Deterministic](https://img.shields.io/badge/deterministic-yes-brightgreen)
![License](https://img.shields.io/badge/license-MIT-purple)

![Emergent City](docs/city.png)

*Day 6 of a 150-citizen city. The feed on the right is unscripted — drug deals, new friendships and rivalries, a death from injuries, five marriages in one morning.*

---

## What is this?

Each citizen is a little agent with a **Big-Five personality + traits**, a set of **needs** (hunger, energy, safety, social, meaning, belonging, money), a **memory**, and a **web of relationships**. A fast utility-AI drives their moment-to-moment choices; an optional LLM is consulted only for rare narrative turning points. Everything else — the crime, the economy, the demographics — **emerges** from those agents interacting.

You just watch. (Unless you flip on **God Mode**.)

This repo has two implementations:

- **`csim/` — a from-scratch C port, and the primary, actively-developed version.** Dependency-light, deterministic, fast (150 agents at hundreds of FPS), rendered in a raylib GPU window. **Everything below describes `csim`.**
- **Python / pygame prototype** (repo root: `main.py`, `agents/`, `world/`, …) — the original it was ported from.

---

## What's simulated

A city's worth of interacting systems, all emergent from the agents:

- **🧠 Agents & AI** — Big-Five personality + unique traits, seven needs, memory, a relationship graph (affinity + familiarity → friends, rivals, marriages). Utility-AI for ~85% of behavior; optional **Qwen/OpenRouter LLM** for the narrative 15%.
- **🔪 Crime underworld** — career criminals, street **dealers**, **kingpins**, and rare latent **serial killers**. A real drug economy (kingpin import → wholesale → street sales to addicts), **turf wars** and retaliation when dealers poach, career **escalation** (theft → burglary → robbery → arson), `wanted` heat, arrests, and **jail gangs** that settle scores behind bars. Assaults cause **injuries** you can die of or pay to **treat**.
- **⚔️ Factions & warfare** — gangs and cults recruit members and hold **turf**. Rival factions **declare war**, their soldiers hunt each other through a shared **combat** system, casualties mount, and truces are called. (Warfare is strictly faction-vs-faction — it's a city, not a nation.)
- **⚖️ Law** — police patrol and arrest; a crime surge triggers a temporary **crackdown**; repeat offenders get **longer sentences**.
- **💰 Economy** — occupations, wages that track the cost of living, **goods-price inflation**, a **landlord class** that owns homes and collects **rent**, and **credit** (micro-loans, interest, debt that drags down your status).
- **🏅 Reputation & status** — a civic standing that work and worship raise and crime lowers, feeding into social **tiers and titles** (Destitute → Magnate, Officer, Kingpin, Notorious…).
- **🌍 Culture** — religions, cultural groups, languages and **education**. Friendships form along cultural lines (a shared tongue bonds, a language barrier strains), the devout draw meaning from worship, and culture **spreads by contact**: the young get schooled, searching souls convert to a friend's faith, minorities assimilate into the common tongue.
- **👶 Life cycle** — courtship → **marriage** → **children** who inherit the family's culture and faith; citizens **age** through life stages (Child → Youth → Adult → Elder) and eventually **die of old age**. Immigration brings young families so the city's age pyramid stays healthy. The whole pace is tunable.
- **🎲 Determinism** — the core is a deterministic PCG32 sim with a fixed timestep; a run is **bit-for-bit reproducible from its seed** (headless, or the GUI with `--fixed-step`, LLM off). Binary save/load snapshots the whole world.

![City dashboard](docs/dashboard.png)

*The city dashboard (press `E`): population, economy, culture and governance at a glance — here an elder has just died of old age at 69.*

---

## Build & run

`csim` builds with CMake and fetches raylib automatically. You need a C compiler and the usual GL/X11 dev headers; `libcurl` + `libcjson` are optional (for the LLM).

```bash
# dev headers (Debian/Ubuntu): raylib needs these
sudo apt install build-essential cmake libgl1-mesa-dev xorg-dev \
     libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
# optional, for LLM consults:
sudo apt install libcurl4-openssl-dev libcjson-dev

cd csim
cmake -B build -S .
cmake --build build

./build/csim            # the GPU window
./build/csim --help     # every flag, env var and control, with examples
./build/csim_headless   # run the core and print a report (no graphics, no deps)
```

> On **WSL**, run GUI apps with `export DISPLAY=:0` (WSLg provides the X server).

---

## Controls

| | |
|---|---|
| **drag / wheel** | pan + zoom |
| **click a citizen** | open the inspector (needs, family, job, rap sheet, culture…) |
| **1–6** | sim speed 1×–6× |
| **Space** | pause |
| **E** | city dashboard |
| **T** | live tuning panel (change settings while it runs) |
| **O** | cycle map overlays: crime-heat, faction-turf + war lines, culture |
| **F / J / C** | factions / jail roster / crime watch |
| **Tab / L** | event feed / legend |
| **G** | God Mode (`1`–`8` select a tool: smite, bless, spawn, incite riot…) |
| **a** | Dwarf-Fortress ASCII render mode |
| **q / Esc** | quit |

![Crime-heat overlay](docs/crime-heat.png)

*Crime-heat overlay (`O`): hotspots glow where crime concentrates. Other overlays tint gang turf (with pulsing war lines between warring factions) or recolor citizens by cultural group.*

---

## Configuration

Most sim parameters are flags **and** environment variables **and** live-adjustable in the `T` panel while running:

```bash
./build/csim --pop-target 250 --years-per-day 4      # bigger city, faster life cycle
./build/csim --fixed-step                            # deterministic, reproducible run
./build/csim --family-share 0.6 --family-kids-max 5  # more children
CSIM_WARMDAYS=12 ./build/csim --pop-target 250       # open on an already-grown city
CSIM_SHOT=out.png ./build/csim                       # render one frame to ./out.png and exit
./build/csim --tileset dawnlike                      # sprite tileset instead of blocks
```

Run `./build/csim --help` for the full, categorized list (aging pace, population target, family demographics, timestep/determinism, rendering, screenshots, LLM, …).

### Reproducibility

The simulation is deterministic given a seed. **Headless** runs are bit-identical by default; the **GUI** is real-time-paced (variable timestep) by default, so pass **`--fixed-step`** to make it reproducible and frame-rate-independent. In both cases the **LLM must be off** for strict reproducibility (it's an async network call), and floating-point identity holds within one build/machine.

### LLM (optional)

Set `OPENROUTER_API_KEY` / `OPENROUTER_BASE_URL` (e.g. in a `.env`) to let citizens occasionally consult a model (Qwen works well) for big narrative decisions. Without a key, the rule-based AI handles everything.

---

## How it stays cheap

Calling an LLM for every agent every tick would be ruinous, so behavior is a **hybrid**: a free, deterministic **utility-AI** scores every agent's actions each tick, and the **LLM is consulted only on rare, important moments** (a betrayal, a conversion, a crisis) with rate-limiting. The result is a city that runs at hundreds of FPS and still has narrative texture.

---

## License

MIT.

## Credits

A C port and heavy expansion of the original Python *Emergent City* concept — built to watch a city live, feud, and grow old on its own. Inspired by Dwarf Fortress, The Sims, RimWorld, and every emergent-narrative game.
