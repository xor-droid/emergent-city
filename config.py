"""
config.py — All tunable parameters in one place.

Change values here to retune the simulation without touching gameplay code.
"""

from __future__ import annotations
from dataclasses import dataclass, field
from typing import Tuple


# ──────────────────────────────────────────────────────────────────────────────
# Display
# ──────────────────────────────────────────────────────────────────────────────

WINDOW_WIDTH = 1600
WINDOW_HEIGHT = 900
TARGET_FPS = 60
WINDOW_TITLE = "Emergent City"

# Pixels per tile at zoom = 1.0
TILE_SIZE = 24

# Camera zoom limits
MIN_ZOOM = 0.4
MAX_ZOOM = 3.0
DEFAULT_ZOOM = 1.0


# ──────────────────────────────────────────────────────────────────────────────
# World
# ──────────────────────────────────────────────────────────────────────────────

WORLD_WIDTH_TILES = 120
WORLD_HEIGHT_TILES = 90

# How many citizens spawn at start
INITIAL_POPULATION = 150
MAX_POPULATION = 250

# How many seconds of real time = one in-game hour at speed 1x
SECONDS_PER_HOUR = 30.0  # one full day = 12 minutes at 1x (slowed from 12s/hour)

# TIME_SCALE: in-game seconds per real second at speed 1x.
# 1 in-game hour = SECONDS_PER_HOUR real seconds, so
# TIME_SCALE = 3600 / SECONDS_PER_HOUR.
TIME_SCALE = 3600.0 / SECONDS_PER_HOUR

# Day/night
DAYTIME_START_HOUR = 6
NIGHTTIME_START_HOUR = 20

# Speed multipliers
SPEED_MULTIPLIERS = (1.0, 5.0, 20.0)


# ──────────────────────────────────────────────────────────────────────────────
# Agents — Movement
# ──────────────────────────────────────────────────────────────────────────────

# Tiles an agent walks per REAL second at speed 1x.
# Previously: 1 tile per tick (60 tiles/sec at 60fps) — way too fast.
# Now: ~2 tiles/sec at 1x → realistic human walking pace.
AGENT_SPEED_TILES_PER_SECOND = 2.0

# Seconds an agent spends on an action before re-deciding.
ACTION_BASE_DURATION_SECONDS = 6.0

# Ticks an agent sleeps per sleep action (each tick = one World.tick call).
SLEEP_TICKS = 400

# How many recent events an agent remembers (bounded deque).
AGENT_MEMORY_SIZE = 20


# ──────────────────────────────────────────────────────────────────────────────
# Agents — Needs decay rates (per in-game hour)
# ──────────────────────────────────────────────────────────────────────────────

# Need values range 0..1 (1 = fully satisfied, 0 = critical)
HUNGER_DECAY = 0.045
ENERGY_DECAY = 0.035       # sleep restores
SAFETY_DECAY = 0.005       # rises in dangerous tiles, decays naturally
SOCIAL_DECAY = 0.020
MEANING_DECAY = 0.015      # boredom; jobs, hobbies, factions restore
MONEY_DECAY = 0.0          # money doesn't decay; it's spent/earned
BELONGING_DECAY = 0.012

# When a need drops below this, the agent considers it CRITICAL
NEED_CRITICAL_THRESHOLD = 0.20
NEED_LOW_THRESHOLD = 0.40


# ──────────────────────────────────────────────────────────────────────────────
# Agents — Personality
# ──────────────────────────────────────────────────────────────────────────────

# Distribution of Big Five traits (mean, stddev), values clipped to [0,1]
BIG_FIVE_MEAN = 0.5
BIG_FIVE_STDDEV = 0.18

UNIQUE_TRAITS = [
"ambitious", "lazy", "kind", "cruel", "paranoid", "trusting", "vain",
"humble", "religious", "skeptical", "romantic", "cynical", "creative",
"rigid", "loyal", "treacherous", "addicted", "disciplined", "naive",
"manipulative", "generous", "stingy", "brave", "cowardly", "vengeful",
"forgiving", "curious", "incurious", "charismatic", "awkward",
]


# ──────────────────────────────────────────────────────────────────────────────
# Economy
# ──────────────────────────────────────────────────────────────────────────────

STARTING_MONEY_MEAN = 100.0
STARTING_MONEY_STDDEV = 50.0

# Below this balance an agent is considered poor (drives work/crime decisions).
LOW_MONEY_THRESHOLD = 20.0

GOOD_BASE_PRICES = {
"food": 5.0,
"tool": 25.0,
"luxury": 80.0,
}

# Wages per shift, by job
WAGES = {
"unemployed": 0.0,
"laborer": 12.0,
"shopkeeper": 18.0,
"factory_worker": 16.0,
"criminal": 30.0,      # high risk, high reward
"preacher": 14.0,
"artist": 10.0,
}

# Default per-action prices/wages used by Agent execution
MEAL_PRICE = 5.0
DRINK_PRICE = 8.0
LUXURY_PRICE = 30.0
WAGE_PER_SHIFT = 15.0

# Inflation / scarcity
PRICE_VOLATILITY = 0.05    # per-day random walk
SUPPLY_PRICE_SENSITIVITY = 0.8


# ──────────────────────────────────────────────────────────────────────────────
# Crime & Factions
# ──────────────────────────────────────────────────────────────────────────────

# Probability per in-game day that a low-status, low-agreeableness agent
# considers committing a crime
CRIME_CONSIDERATION_RATE = 0.05

# Witnesses & arrest
WITNESS_RADIUS = 5
ARREST_DURATION_TICKS = 600
DANGER_DECAY_PER_SECOND = 0.001

# Gangs form when N agents with sympathetic traits cluster
GANG_FORMATION_MIN_MEMBERS = 4
CULT_FORMATION_MIN_MEMBERS = 3

# Police presence (tiles patrolled per tick)
POLICE_PATROL_INTENSITY = 6


# ──────────────────────────────────────────────────────────────────────────────
# Relationships & Social
# ──────────────────────────────────────────────────────────────────────────────

# Max remembered relationships per agent (LRU)
MAX_RELATIONSHIPS = 40

# Daily relationship decay if no interaction (toward 0)
RELATIONSHIP_DECAY = 0.005

# Affinity gain per pleasant interaction
INTERACTION_AFFINITY_DELTA = 0.04

# Affinity (roughly -1..1) at/above which two agents count as friends,
# and at/below which they count as rivals.
FRIENDSHIP_AFFINITY_THRESHOLD = 0.30
RIVALRY_AFFINITY_THRESHOLD = -0.30


# ──────────────────────────────────────────────────────────────────────────────
# LLM (OpenAI-compatible: OpenRouter cloud OR a local llama.cpp server)
# ──────────────────────────────────────────────────────────────────────────────

# Master switch for LLM-driven decisions.
# A local llama.cpp server (e.g. Qwen3-8B) has no per-token cost, so this is on.
LLM_ENABLED = True

# Chat-completions endpoint (OpenAI-compatible). Hard-defaulted to our own
# Qwen docker container on the LAN so we never silently hit a cloud provider
# (openrouter.ai / api.openai.com). OPENROUTER_BASE_URL in .env can override it,
# but if nothing is set we still point at the local container, not the cloud.
LLM_BASE_URL = "http://10.0.0.134:9090/v1/chat/completions"

# Qwen3 (and similar) default to a verbose "thinking" mode that leaves the
# answer empty when the token budget is small. Append the `/no_think` soft
# switch so replies land in `content` and stay short. Set False for cloud
# models that don't understand it (it's harmless, just unnecessary).
LLM_DISABLE_THINKING = True

# Threads available for concurrent, non-blocking LLM calls from the game loop.
LLM_POOL_SIZE = 4

# Fallback model list, tried in order, when no per-call/instance model is set.
# Normally the model comes from OPENROUTER_MODEL (.env); this is a safety net.
LLM_MODELS = ("Qwen/Qwen3-8B-GGUF:Q4_K_M",)

# Only call the LLM when an event of this importance or higher occurs
LLM_IMPORTANCE_THRESHOLD = 0.7

# Hard cap on LLM calls per real-world minute (budget / throughput guard).
# Ramping up slowly while watching the local Qwen container's load:
# 30 -> 90 (2026-10-02). Raise further only after confirming headroom.
LLM_RATE_LIMIT_PER_MINUTE = 90
LLM_MAX_CALLS_PER_MINUTE = LLM_RATE_LIMIT_PER_MINUTE  # name used by DecisionRouter

# Ceiling on the per-agent "consult the LLM" probability each decision.
LLM_MAX_CONSULT_PROBABILITY = 0.8

# Cache identical decisions for this many seconds
LLM_CACHE_TTL_SECONDS = 600

# Per-request budget
LLM_MAX_TOKENS = 220
LLM_TEMPERATURE = 0.9
LLM_TIMEOUT_SECONDS = 12.0


# ──────────────────────────────────────────────────────────────────────────────
# Rendering — Palette (Disco Elysium / RimWorld inspired)
# ──────────────────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Palette:
    background: Tuple[int, int, int] = (18, 16, 22)
    grass: Tuple[int, int, int] = (52, 78, 58)
    grass_lush: Tuple[int, int, int] = (68, 102, 70)
    road: Tuple[int, int, int] = (48, 44, 52)
    road_marking: Tuple[int, int, int] = (210, 200, 160)
    sidewalk: Tuple[int, int, int] = (95, 92, 100)
    water: Tuple[int, int, int] = (45, 70, 100)

    house_wall: Tuple[int, int, int] = (162, 122, 96)
    house_roof: Tuple[int, int, int] = (110, 60, 50)
    house_wealthy_wall: Tuple[int, int, int] = (200, 180, 150)
    house_wealthy_roof: Tuple[int, int, int] = (60, 70, 90)
    slum_wall: Tuple[int, int, int] = (95, 78, 70)
    slum_roof: Tuple[int, int, int] = (70, 50, 45)

    shop_wall: Tuple[int, int, int] = (170, 140, 80)
    shop_roof: Tuple[int, int, int] = (130, 80, 50)
    factory_wall: Tuple[int, int, int] = (90, 90, 95)
    factory_roof: Tuple[int, int, int] = (55, 55, 60)
    park_tree: Tuple[int, int, int] = (40, 80, 50)

    window_lit: Tuple[int, int, int] = (255, 215, 130)
    window_dark: Tuple[int, int, int] = (40, 40, 50)

    mood_happy: Tuple[int, int, int] = (120, 220, 130)
    mood_content: Tuple[int, int, int] = (200, 220, 160)
    mood_neutral: Tuple[int, int, int] = (210, 210, 210)
    mood_sad: Tuple[int, int, int] = (110, 140, 220)
    mood_angry: Tuple[int, int, int] = (230, 90, 80)
    mood_afraid: Tuple[int, int, int] = (180, 130, 220)
    mood_numb: Tuple[int, int, int] = (130, 130, 140)

    night_overlay: Tuple[int, int, int] = (10, 12, 30)

    ui_bg: Tuple[int, int, int] = (24, 22, 30)
    ui_panel: Tuple[int, int, int] = (32, 30, 38)
    ui_accent: Tuple[int, int, int] = (212, 175, 90)
    ui_text: Tuple[int, int, int] = (230, 225, 215)
    ui_text_dim: Tuple[int, int, int] = (150, 145, 135)
    ui_text_danger: Tuple[int, int, int] = (230, 90, 80)
    ui_text_good: Tuple[int, int, int] = (120, 220, 130)


PALETTE = Palette()


# ──────────────────────────────────────────────────────────────────────────────
# Districts (used by procedural map generation — Manhattan-style grid)
# ──────────────────────────────────────────────────────────────────────────────

# Each district maps to a wealth score (used by economy/agent placement).
DISTRICT_LAYOUT = {
"downtown":     {"radius": 0.18, "wealth": 0.95},   # Financial District
"midtown":      {"radius": 0.40, "wealth": 0.80},   # Midtown
"residential":  {"radius": 0.65, "wealth": 0.55},   # Brownstones
"industrial":   {"radius": 0.85, "wealth": 0.35},   # Docks/warehouses
"slums":        {"radius": 1.00, "wealth": 0.15},   # Outer edges
}


# ──────────────────────────────────────────────────────────────────────────────
# Debug
# ──────────────────────────────────────────────────────────────────────────────

DEBUG_DRAW_PATHS = False
DEBUG_DRAW_NEEDS = False
DEBUG_DRAW_DISTRICTS = False
LOG_LLM_CALLS = True
RANDOM_SEED = 1337   # set to None for non-deterministic runs


# ──────────────────────────────────────────────────────────────────────────────
# Rendering aliases (used by camera/renderer/UI modules)
# ──────────────────────────────────────────────────────────────────────────────

# Some modules expect SCREEN_* / TILE_WIDTH / TILE_HEIGHT names — alias them.
SCREEN_WIDTH = WINDOW_WIDTH
SCREEN_HEIGHT = WINDOW_HEIGHT
TILE_WIDTH = TILE_SIZE
TILE_HEIGHT = TILE_SIZE
CAMERA_MIN_ZOOM = MIN_ZOOM
CAMERA_MAX_ZOOM = MAX_ZOOM
BUILDING_HEIGHT_PX = 12

# Save file location
SAVE_FILE_PATH = "emergent_city_save.json"
