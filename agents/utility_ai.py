"""
utility_ai.py — Scores candidate actions for an agent given personality, needs,
relationships, world state, and time. The action with the highest utility is
chosen.

This file deliberately uses ONLY rule-based logic. The LLM layer can override
the choice via the DecisionRouter when an interesting situation arises.
"""

from __future__ import annotations
from dataclasses import dataclass
from typing import List, TYPE_CHECKING

import config

if TYPE_CHECKING:
    from agents.agent import Agent
    from world.world import World


@dataclass
class ActionScore:
    name: str
    score: float
    reason: str = ""


class UtilityAI:
    """Stateless scorer."""

    ACTIONS = (
        "eat", "sleep", "work", "socialize", "drink_at_bar",
        "go_home", "wander", "pray", "shop", "commit_crime",
        "patrol", "flee",
    )

    @classmethod
    def best_action(cls, agent: "Agent", world: "World") -> str:
        scores = cls.score_all(agent, world)
        scores.sort(key=lambda s: s.score, reverse=True)
        return scores[0].name

    @classmethod
    def score_all(cls, agent: "Agent", world: "World") -> List[ActionScore]:
        n = agent.needs
        p = agent.personality
        hour = world.time_system.hour
        is_night = world.time_system.is_night

        scores: List[ActionScore] = []

        # ── Eat
        eat = (1.0 - n.hunger) ** 2 * 2.0
        if n.hunger < config.NEED_CRITICAL_THRESHOLD:
            # Starving: eating is life-or-death and must beat even the sleep
            # emergency boost. A charity meal is available even if broke, so
            # don't suppress it for lack of money here.
            eat += 3.0
        elif n.money < config.MEAL_PRICE * 0.5:
            eat *= 0.4  # can't really afford it (and not yet desperate)
        scores.append(ActionScore("eat", eat, "hunger"))

        # ── Sleep
        sleep = (1.0 - n.energy) ** 2 * 2.5
        if is_night:
            sleep *= 1.5
        if hour < 6 or hour > 23:
            sleep *= 1.3
        # When genuinely exhausted, sleep must dominate everything (incl. work)
        # so agents recover instead of collapsing from exhaustion.
        if n.energy < config.NEED_CRITICAL_THRESHOLD:
            sleep += 2.0
        scores.append(ActionScore("sleep", sleep, "tired"))

        # ── Work
        work = 0.0
        if 8 <= hour <= 18 and agent.workplace_id != -1:
            work = 0.8 + p.work_ethic() * 0.6
            if n.money < config.LOW_MONEY_THRESHOLD:
                work += 0.6
        scores.append(ActionScore("work", work, "work hours"))

        # ── Socialize
        social = (1.0 - n.social) * (0.7 + p.social_drive() * 0.6)
        if is_night and hour > 21:
            social *= 0.6
        scores.append(ActionScore("socialize", social, "lonely"))

        # ── Drink at bar
        drink = 0.0
        if 18 <= hour <= 24 or hour < 2:
            drink = (1.0 - n.social) * 0.6 + p.extraversion * 0.4
            if p.has("alcoholic"):
                drink += 0.6
            if n.money < config.DRINK_PRICE:
                drink *= 0.2
        scores.append(ActionScore("drink_at_bar", drink, "evening"))

        # ── Pray
        pray = 0.0
        if (1.0 - n.meaning) > 0.4 or n.safety < 0.4:
            pray = (1.0 - n.meaning) * p.faith() * 1.2
        scores.append(ActionScore("pray", pray, "spiritual"))

        # ── Shop
        shop = 0.0
        if n.money > config.LUXURY_PRICE and n.belonging < 0.7:
            shop = 0.35 + (0.7 - n.belonging) * 0.5
        scores.append(ActionScore("shop", shop, "wants stuff"))

        # ── Go home
        home = 0.0
        if is_night:
            home = 0.7
        if n.energy < 0.3:
            home += 0.5
        scores.append(ActionScore("go_home", home, "rest"))

        # ── Commit crime
        crime = 0.0
        propensity = p.crime_propensity()
        if n.money < config.LOW_MONEY_THRESHOLD and n.hunger < 0.5:
            crime = propensity * 1.2 * (1.0 - n.money / max(1.0, config.LOW_MONEY_THRESHOLD))
        if agent.faction_id != -1 and is_night:
            crime += 0.3 * propensity
        if n.safety > 0.7:
            crime *= 0.7
        scores.append(ActionScore("commit_crime", crime, "desperate"))

        # ── Patrol (police only)
        patrol = 0.0
        if getattr(agent, "is_police", False) and 6 <= hour <= 22:
            patrol = 1.5
        scores.append(ActionScore("patrol", patrol, "duty"))

        # ── Flee
        flee = 0.0
        if n.safety < 0.3:
            flee = (0.3 - n.safety) * 4.0
        scores.append(ActionScore("flee", flee, "scared"))

        # ── Wander (fallback)
        scores.append(ActionScore("wander", 0.05, "idle"))

        return scores
