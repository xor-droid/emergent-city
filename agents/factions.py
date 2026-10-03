"""
factions.py — Gangs, cults, civic groups. Recruitment, rivalry, hierarchy.
"""

from __future__ import annotations
from dataclasses import dataclass, field
from typing import List, Dict, TYPE_CHECKING
import random

import config

if TYPE_CHECKING:
    from world.world import World


@dataclass
class Faction:
    id: int
    name: str
    kind: str            # "gang" | "cult" | "civic"
    color: tuple
    leader_id: int = -1
    members: List[int] = field(default_factory=list)
    territory: List[tuple] = field(default_factory=list)
    rivalries: List[int] = field(default_factory=list)
    treasury: float = 200.0
    ideology: str = ""

    def to_dict(self) -> dict:
        return {
            "id": self.id, "name": self.name, "kind": self.kind,
            "color": list(self.color), "leader_id": self.leader_id,
            "members": list(self.members), "territory": [list(t) for t in self.territory],
            "rivalries": list(self.rivalries), "treasury": self.treasury,
            "ideology": self.ideology,
        }

    @classmethod
    def from_dict(cls, d: dict) -> "Faction":
        f = cls(id=d["id"], name=d["name"], kind=d["kind"],
                color=tuple(d["color"]), leader_id=d["leader_id"])
        f.members = list(d["members"])
        f.territory = [tuple(t) for t in d["territory"]]
        f.rivalries = list(d["rivalries"])
        f.treasury = d["treasury"]
        f.ideology = d["ideology"]
        return f


class FactionSystem:
    """All factions in the world."""

    GANG_NAMES = ("Scarlet Hounds", "Black Bay", "The Needles", "Northern Wolves",
                  "Factory Shadows", "The Twenty-Thirds")
    CULT_NAMES = ("Children of the Dawn", "Silence of the Moon", "Order of the Ninth Hour",
                  "Circle of Ash", "Sisters of the Water")
    GANG_COLORS = ((200, 50, 50), (40, 40, 40), (170, 80, 200),
                   (100, 100, 220), (160, 110, 50), (220, 180, 60))
    CULT_COLORS = ((230, 200, 70), (180, 180, 240), (120, 60, 160),
                   (90, 90, 90), (60, 160, 200))

    def __init__(self) -> None:
        self.factions: Dict[int, Faction] = {}
        self._next_id = 0
        self._seeded = False

    def seed(self, rng: random.Random) -> None:
        if self._seeded:
            return
        for name, color in zip(self.GANG_NAMES, self.GANG_COLORS):
            self._create("gang", name, color, ideology="power in the streets")
        for name, color in zip(self.CULT_NAMES, self.CULT_COLORS):
            self._create("cult", name, color, ideology="revelation")
        # Mutual rivalries between same-kind factions
        gangs = [f for f in self.factions.values() if f.kind == "gang"]
        for f in gangs:
            for g in gangs:
                if g.id != f.id and rng.random() < 0.5:
                    if g.id not in f.rivalries:
                        f.rivalries.append(g.id)
        self._seeded = True

    def _create(self, kind: str, name: str, color: tuple, ideology: str = "") -> Faction:
        f = Faction(id=self._next_id, name=name, kind=kind, color=color, ideology=ideology)
        self.factions[self._next_id] = f
        self._next_id += 1
        return f

    def recruit(self, faction_id: int, agent_id: int) -> None:
        f = self.factions.get(faction_id)
        if f and agent_id not in f.members:
            f.members.append(agent_id)
            if f.leader_id == -1:
                f.leader_id = agent_id

    def leave(self, faction_id: int, agent_id: int) -> None:
        f = self.factions.get(faction_id)
        if f and agent_id in f.members:
            f.members.remove(agent_id)
            if f.leader_id == agent_id:
                f.leader_id = f.members[0] if f.members else -1

    def get(self, fid: int):
        return self.factions.get(fid)

    def tick(self, dt: float, world: "World") -> None:
        if not self._seeded:
            self.seed(world.rng)

    def daily_tick(self, world: "World") -> None:
        for f in self.factions.values():
            # Treasury bleed/income
            if f.kind == "gang":
                f.treasury += len(f.members) * 5 - 10
            elif f.kind == "cult":
                f.treasury += len(f.members) * 3
            # Recruit drift: occasional new member from desperate citizens
            if world.rng.random() < 0.25 and world.agents:
                cand = world.rng.choice(world.agents)
                if (cand.alive and cand.faction_id == -1
                        and cand.needs.belonging < 0.4
                        and cand.personality.crime_propensity() > 0.5
                        and f.kind == "gang"):
                    self.recruit(f.id, cand.id)
                    cand.faction_id = f.id
                elif (cand.alive and cand.faction_id == -1
                      and cand.personality.faith() > 0.7
                      and cand.needs.meaning < 0.4
                      and f.kind == "cult"):
                    self.recruit(f.id, cand.id)
                    cand.faction_id = f.id

    def to_dict(self) -> dict:
        return {
            "next_id": self._next_id,
            "seeded": self._seeded,
            "factions": [f.to_dict() for f in self.factions.values()],
        }

    def from_dict_inplace(self, d: dict) -> None:
        self._next_id = d["next_id"]
        self._seeded = d["seeded"]
        self.factions = {f["id"]: Faction.from_dict(f) for f in d["factions"]}
