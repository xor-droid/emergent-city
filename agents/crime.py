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
        civilian_witnesses = [w for w in witnesses if not getattr(w, "is_police", False)]

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
            # Got away, but seen by civilians -> now wanted.
            if civilian_witnesses:
                self._mark_wanted(perpetrator, kind, world)
        elif police_nearby:
            # Caught red-handed by police at the scene.
            perpetrator.needs.safety = max(0.0, perpetrator.needs.safety - 0.3)
            self._jail(perpetrator, kind)
            self.events.post(WorldEvent(
                kind="arrest",
                actor_id=perpetrator.id,
                target_id=target.id if target else -1,
                location=(x, y),
                importance=importance + 0.15,
                text=f"{perpetrator.name} was caught in the act and arrested for {kind}{against}",
                payload={"kind": kind, "how": "in_the_act"},
            ))
        else:
            # Botched, no police — but civilians may have seen them flee.
            perpetrator.needs.safety = max(0.0, perpetrator.needs.safety - 0.3)
            self.events.post(WorldEvent(
                kind="crime_failed",
                actor_id=perpetrator.id,
                target_id=target.id if target else -1,
                location=(x, y),
                importance=importance + 0.1,
                text=f"{perpetrator.name} botched {kind}{against} and fled",
                payload={"kind": kind, "police": police_nearby},
            ))
            if civilian_witnesses:
                self._mark_wanted(perpetrator, kind, world)

        # Witnesses lose safety
        for w in witnesses:
            if not getattr(w, "is_police", False):
                w.needs.safety = max(0.0, w.needs.safety - 0.15)
                # Tile danger rises
                world.tile_map.get(w.x, w.y).danger = min(1.0,
                    world.tile_map.get(w.x, w.y).danger + 0.08)

    # ── Wanted lifecycle ─────────────────────────────────────────────────────
    @staticmethod
    def _severity(kind: str) -> float:
        return config.CRIME_SEVERITY.get(kind, 1.0)

    def _sentence(self, kind: str) -> int:
        """Jail time for a crime, scaled by its severity."""
        return int(config.ARREST_DURATION_TICKS * self._severity(kind))

    def _jail(self, a, kind: str) -> None:
        """Put an agent in jail for `kind`, recording the sentence for the roster."""
        sentence = self._sentence(kind)
        a.arrested_ticks = sentence
        a.sentence_total = sentence
        a.jailed_for = kind
        self._clear_wanted(a)

    def _mark_wanted(self, a, kind: str, world: "World") -> None:
        # More serious crimes are hunted longer. Keep the longer of any existing
        # heat and this crime's, so a serious crime can't be "downgraded".
        heat = int(config.WANTED_DURATION_TICKS * self._severity(kind))
        if a.wanted:
            if heat > a.wanted_ticks:
                a.wanted_ticks = heat
                a.wanted_for = kind
            return
        a.wanted = True
        a.wanted_for = kind
        a.wanted_ticks = heat
        self.events.post(WorldEvent(
            kind="wanted", actor_id=a.id, location=(a.x, a.y), importance=0.5,
            text=f"{a.name} is now wanted for {kind}",
            payload={"kind": kind},
        ))

    @staticmethod
    def _clear_wanted(a) -> None:
        a.wanted = False
        a.wanted_ticks = 0
        a.wanted_for = ""

    def tick(self, dt: float, world: "World") -> None:
        # Slow decay of tile danger
        decay = config.DANGER_DECAY_PER_SECOND * dt
        for x in range(world.tile_map.width):
            for y in range(world.tile_map.height):
                t = world.tile_map.tiles[x][y]
                if t.danger > 0.0:
                    t.danger = max(0.0, t.danger - decay)

        # Police hunt wanted agents; uncaught heat cools off ("lying low").
        police = [a for a in world.agents
                  if a.alive and getattr(a, "is_police", False) and a.arrested_ticks == 0]
        r = config.POLICE_ARREST_RADIUS
        for a in world.agents:
            if not (a.alive and a.wanted and a.arrested_ticks == 0):
                continue
            cop = next((p for p in police
                        if abs(p.x - a.x) <= r and abs(p.y - a.y) <= r), None)
            if cop is not None and world.rng.random() < config.POLICE_ARREST_CHANCE:
                kind = a.wanted_for or "a crime"
                self._jail(a, kind)
                self.events.post(WorldEvent(
                    kind="arrest", actor_id=a.id, target_id=cop.id,
                    location=(a.x, a.y), importance=0.7,
                    text=f"{a.name} was tracked down and arrested by {cop.name} (wanted for {kind})",
                    payload={"kind": kind, "how": "patrol"},
                ))
                continue
            # Lie low: heat decays; once it runs out they're no longer wanted.
            a.wanted_ticks -= 1
            if a.wanted_ticks <= 0:
                kind = a.wanted_for or "a crime"
                self._clear_wanted(a)
                self.events.post(WorldEvent(
                    kind="laid_low", actor_id=a.id, location=(a.x, a.y), importance=0.3,
                    text=f"{a.name} lay low and is no longer wanted for {kind}",
                    payload={"kind": kind},
                ))
