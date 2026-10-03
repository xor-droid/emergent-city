"""
crime.py — Crime resolution. Listens for "crime_attempt" events, decides
outcomes based on witnesses, police presence, and victim resistance.
"""

from __future__ import annotations
from typing import TYPE_CHECKING
import math

import config
from world.events import WorldEvent

if TYPE_CHECKING:
    from world.world import World


class CrimeSystem:
    def __init__(self, events) -> None:
        self.events = events

    def attempt_crime(self, perpetrator, world: "World", kind: str = "theft", target=None) -> None:
        """Called from Agent when they decide to commit a crime."""
        x, y = perpetrator.x, perpetrator.y

        # Find witnesses within radius
        witness_radius = config.WITNESS_RADIUS
        witnesses = [
            a for a in world.agents
            if a.alive and a.id != perpetrator.id
            and abs(a.x - x) <= witness_radius
            and abs(a.y - y) <= witness_radius
        ]
        police_nearby = any(getattr(a, "is_police", False) for a in witnesses)

        # Probability of success
        success_chance = 0.65
        success_chance -= 0.10 * len([w for w in witnesses if not getattr(w, "is_police", False)])
        success_chance -= 0.55 if police_nearby else 0.0
        success_chance += perpetrator.personality.crime_propensity() * 0.15
        success_chance = max(0.05, min(0.95, success_chance))

        success = world.rng.random() < success_chance

        importance = 0.55 if kind == "theft" else 0.85
        against = f" against {target.name}" if target is not None else ""

        if success:
            loot = world.rng.uniform(5.0, 40.0)
            perpetrator.needs.money += loot
            if target is not None and hasattr(target, "needs"):
                target.needs.money = max(0.0, target.needs.money - loot)
                target.needs.safety = max(0.0, target.needs.safety - 0.4)
            self.events.post(WorldEvent(
                kind="crime",
                actor_id=perpetrator.id,
                target_id=target.id if target else -1,
                location=(x, y),
                importance=importance,
                text=f"{perpetrator.name} committed {kind}{against} (${loot:.0f})",
                payload={"kind": kind, "loot": loot, "witnesses": len(witnesses)},
            ))
        else:
            # Caught
            perpetrator.needs.safety = max(0.0, perpetrator.needs.safety - 0.3)
            caught_by = " — caught by police" if police_nearby else " — but failed"
            self.events.post(WorldEvent(
                kind="crime_failed",
                actor_id=perpetrator.id,
                target_id=target.id if target else -1,
                location=(x, y),
                importance=importance + 0.1,
                text=f"{perpetrator.name} attempted {kind}{against}{caught_by}",
                payload={"kind": kind, "police": police_nearby},
            ))
            if police_nearby:
                perpetrator.arrested_ticks = config.ARREST_DURATION_TICKS

        # Witnesses lose safety
        for w in witnesses:
            if not getattr(w, "is_police", False):
                w.needs.safety = max(0.0, w.needs.safety - 0.15)
                # Tile danger rises
                world.tile_map.get(w.x, w.y).danger = min(1.0,
                    world.tile_map.get(w.x, w.y).danger + 0.08)

    def tick(self, dt: float, world: "World") -> None:
        # Slow decay of tile danger
        decay = config.DANGER_DECAY_PER_SECOND * dt
        for x in range(world.tile_map.width):
            for y in range(world.tile_map.height):
                t = world.tile_map.tiles[x][y]
                if t.danger > 0.0:
                    t.danger = max(0.0, t.danger - decay)
