"""
relationships.py — Per-agent social ledger: friends, rivals, lovers.
"""

from __future__ import annotations
from dataclasses import dataclass, field
from typing import Dict, List

import config


@dataclass
class RelationshipBook:
    """
    affinity in [-1, 1]: hate ↔ love.
    familiarity in [0, 1]: how much they actually know each other.
    """
    affinity: Dict[int, float] = field(default_factory=dict)
    familiarity: Dict[int, float] = field(default_factory=dict)
    romantic_partner: int = -1
    rivals: List[int] = field(default_factory=list)
    # Others we've already announced a friendship with (prevents re-announcing
    # when affinity dips below the threshold and crosses back). Transient.
    announced_friends: set = field(default_factory=set)
    announced_rivals: set = field(default_factory=set)

    def adjust(self, other_id: int, d_affinity: float, d_fam: float = 0.05) -> None:
        a = self.affinity.get(other_id, 0.0) + d_affinity
        self.affinity[other_id] = max(-1.0, min(1.0, a))
        f = self.familiarity.get(other_id, 0.0) + d_fam
        self.familiarity[other_id] = max(0.0, min(1.0, f))

        if a >= config.RIVALRY_AFFINITY_THRESHOLD * -1 and other_id in self.rivals:
            self.rivals.remove(other_id)
        elif a <= config.RIVALRY_AFFINITY_THRESHOLD and other_id not in self.rivals:
            self.rivals.append(other_id)

    def likes(self, other_id: int) -> bool:
        return self.affinity.get(other_id, 0.0) >= config.FRIENDSHIP_AFFINITY_THRESHOLD

    def hates(self, other_id: int) -> bool:
        return self.affinity.get(other_id, 0.0) <= config.RIVALRY_AFFINITY_THRESHOLD

    def best_friend(self) -> int:
        if not self.affinity:
            return -1
        return max(self.affinity, key=self.affinity.get)

    def to_dict(self) -> dict:
        return {
            "aff": dict(self.affinity),
            "fam": dict(self.familiarity),
            "partner": self.romantic_partner,
            "rivals": list(self.rivals),
        }

    @classmethod
    def from_dict(cls, d: dict) -> "RelationshipBook":
        rb = cls()
        rb.affinity = {int(k): v for k, v in d["aff"].items()}
        rb.familiarity = {int(k): v for k, v in d["fam"].items()}
        rb.romantic_partner = d["partner"]
        rb.rivals = list(d["rivals"])
        return rb
