"""
world.py — Central simulation aggregator: ties together time, tiles, buildings,
economy, agents, factions, events, and ticks them in lockstep.
"""

from __future__ import annotations
from typing import List, Dict, Optional
import random
import logging

import config
from world.tile_map import TileMap, District
from world.buildings import BuildingRegistry, BuildingType
from world.economy import Economy
from world.events import EventBus, WorldEvent
from world.time_system import TimeSystem
from agents.agent import Agent
from agents.factions import FactionSystem
from agents.crime import CrimeSystem

log = logging.getLogger(__name__)


class World:
    """Owns everything in the simulation."""

    def __init__(self, width: int, height: int, decision_router=None):
        self.width = width
        self.height = height
        self.rng = random.Random(config.RANDOM_SEED)

        self.time_system = TimeSystem()
        self.events = EventBus()
        self.tile_map = TileMap(width=width, height=height)
        self.buildings = BuildingRegistry(self.tile_map, self.rng)
        self.economy = Economy(rng=self.rng)
        self.factions = FactionSystem()
        self.crime = CrimeSystem(self.events)

        self.agents: List[Agent] = []
        self._agents_by_id: Dict[int, Agent] = {}
        self._next_agent_id = 0

        self.decision_router = decision_router

        # Internal accumulators for daily ticks
        self._day_seen = self.time_system.day

        self.buildings.populate()
        log.info("Placed %d buildings", len(self.buildings.buildings))

    # ── Population ────────────────────────────────────────────────────────────
    def populate(self, n: int) -> None:
        residences = self.buildings.residences()
        if not residences:
            log.error("No residential buildings; cannot populate")
            return

        for _ in range(n):
            home = self.rng.choice(residences)
            if len(home.residents) >= home.capacity:
                # try a few more
                tries = 0
                while len(home.residents) >= home.capacity and tries < 30:
                    home = self.rng.choice(residences)
                    tries += 1
                if len(home.residents) >= home.capacity:
                    continue
            agent = Agent.spawn(
                aid=self._next_agent_id,
                home=home,
                rng=self.rng,
                world=self,
            )
            home.residents.append(agent.id)
            self.agents.append(agent)
            self._agents_by_id[agent.id] = agent
            self._next_agent_id += 1

        log.info("Spawned %d agents into %d residences", len(self.agents), len(residences))

    def get_agent(self, aid: int) -> Optional[Agent]:
        return self._agents_by_id.get(aid)

    def spawn_agent(self, x: Optional[int] = None, y: Optional[int] = None) -> Optional[Agent]:
        """Spawn and register a single new citizen (used by God Mode).

        Picks a random residence as the agent's home; if (x, y) is given and
        walkable, the agent starts there instead of at the home perimeter.
        Returns the new Agent, or None if there's nowhere to house them.
        """
        residences = self.buildings.residences()
        if not residences:
            return None
        home = self.rng.choice(residences)
        agent = Agent.spawn(aid=self._next_agent_id, home=home, rng=self.rng, world=self)
        if (x is not None and y is not None
                and self.tile_map.in_bounds(int(x), int(y))
                and self.tile_map.is_walkable(int(x), int(y))):
            agent.x, agent.y = int(x), int(y)
        home.residents.append(agent.id)
        self.agents.append(agent)
        self._agents_by_id[agent.id] = agent
        self._next_agent_id += 1
        self.events.post(WorldEvent(
            kind="birth", actor_id=agent.id, location=(agent.x, agent.y),
            importance=0.4, text=f"{agent.name} appeared in the city",
        ))
        return agent

    # ── Tick ──────────────────────────────────────────────────────────────────
    def tick(self, dt_seconds: float) -> None:
        prev_day = self.time_system.day
        self.time_system.advance(dt_seconds)

        # Tick agents
        for agent in self.agents:
            if agent.alive:
                agent.tick(dt_seconds, self)

        # Police patrols & crime resolution
        self.crime.tick(dt_seconds, self)

        # Faction dynamics
        self.factions.tick(dt_seconds, self)

        # Daily updates
        if self.time_system.day != prev_day:
            self.economy.daily_tick()
            self.factions.daily_tick(self)
            for agent in self.agents:
                if agent.alive:
                    agent.daily_tick(self)

    # ── Persistence ───────────────────────────────────────────────────────────
    def snapshot(self) -> dict:
        return {
            "time": self.time_system.to_dict(),
            "economy": self.economy.to_dict(),
            "agents": [a.to_dict() for a in self.agents if a.alive],
            "next_agent_id": self._next_agent_id,
            "factions": self.factions.to_dict(),
        }

    def restore(self, data: dict) -> None:
        self.time_system = TimeSystem.from_dict(data["time"])
        self.economy = Economy.from_dict(data["economy"], rng=self.rng)
        self._next_agent_id = data["next_agent_id"]
        self.agents.clear()
        self._agents_by_id.clear()
        for ad in data["agents"]:
            a = Agent.from_dict(ad, world=self)
            self.agents.append(a)
            self._agents_by_id[a.id] = a
        self.factions.from_dict_inplace(data["factions"])
