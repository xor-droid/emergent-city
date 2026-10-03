"""
needs.py — Maslow-style needs that decay over time and motivate behavior.
"""

from __future__ import annotations
from dataclasses import dataclass

import config


@dataclass
class Needs:
    hunger: float = 1.0
    energy: float = 1.0
    safety: float = 1.0
    social: float = 1.0
    meaning: float = 1.0
    belonging: float = 1.0
    money: float = 0.0       # not 0..1; absolute

    def decay(self, hours: float) -> None:
        self.hunger = max(0.0, self.hunger - config.HUNGER_DECAY * hours)
        self.energy = max(0.0, self.energy - config.ENERGY_DECAY * hours)
        self.safety = max(0.0, min(1.0, self.safety + (0.5 - self.safety) * 0.02 * hours))
        self.social = max(0.0, self.social - config.SOCIAL_DECAY * hours)
        self.meaning = max(0.0, self.meaning - config.MEANING_DECAY * hours)
        self.belonging = max(0.0, self.belonging - config.BELONGING_DECAY * hours)

    @property
    def most_critical(self) -> str:
        candidates = {
            "hunger": self.hunger,
            "energy": self.energy,
            "safety": self.safety,
            "social": self.social,
            "meaning": self.meaning,
            "belonging": self.belonging,
        }
        return min(candidates, key=candidates.get)

    def is_critical(self) -> bool:
        """True if any 0..1 need has fallen below the critical threshold."""
        t = config.NEED_CRITICAL_THRESHOLD
        return (self.hunger < t or self.energy < t or self.safety < t
                or self.social < t or self.meaning < t or self.belonging < t)

    def is_dying(self) -> bool:
        return self.hunger <= 0.01 or self.energy <= 0.01

    def overall_wellbeing(self) -> float:
        return (self.hunger * 1.2 + self.energy + self.safety * 1.1
                + self.social * 0.9 + self.meaning + self.belonging) / 6.2

    def to_dict(self) -> dict:
        return {
            "hunger": self.hunger, "energy": self.energy, "safety": self.safety,
            "social": self.social, "meaning": self.meaning,
            "belonging": self.belonging, "money": self.money,
        }

    @classmethod
    def from_dict(cls, d: dict) -> "Needs":
        return cls(**d)
