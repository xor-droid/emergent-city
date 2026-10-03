"""
prompts.py — Builds system+user prompts for LLM calls.
All prompts are in English.
"""

from __future__ import annotations
from typing import List, Dict, TYPE_CHECKING

import config

if TYPE_CHECKING:
    from agents.agent import Agent
    from world.world import World


SYSTEM_ROLEPLAY = (
    "You are a resident of an emergent city. Reply briefly, in English, "
    "in the first person. One or two sentences, no more than 25 words. "
    "Stay true to the character's personality and current state."
)

SYSTEM_THOUGHT = (
    "You are the inner monologue of a city resident. One short thought "
    "(up to 12 words) in English, without quotation marks."
)

SYSTEM_DECISION = (
    "You are the instinct of a city resident, choosing their next action.\n"
    "A need only counts as URGENT when it is low (below ~0.40); a need near 0 "
    "overrides everything. If EVERY need is above 0.40 the person is "
    "comfortable, so do NOT pick by tiny need differences — instead decide by "
    "time of day and their personality/goals.\n"
    "Priority: 1) satisfy any low (<0.40) need; else 2) fit the time of day "
    "(daytime -> work/shop/socialize; evening -> socialize/drink_at_bar; "
    "night -> sleep/go_home); and 3) fit personality/traits (religious->pray, "
    "criminal or broke->commit_crime, lazy->wander/go_home, extravert->"
    "socialize).\n"
    "Need->action guide: eat/shop=hunger, sleep/go_home=energy, "
    "socialize/drink_at_bar=social, work=money+meaning, pray=meaning+belonging, "
    "commit_crime=money (risky), flee=danger, wander=restless.\n"
    "Never default to 'eat' unless hunger is actually low. Reply with EXACTLY "
    "ONE word from this list and nothing else: {actions}."
)


_LOW_NEED = 0.40  # a need below this is genuinely urgent


def _needs_summary(n: "object") -> str:
    """Flag only genuinely-low needs; otherwise say the person is comfortable.

    Avoids implying that a marginally-lowest-but-high need (e.g. hunger=0.98 at
    game start) is urgent, which was making the model always choose 'eat'.
    """
    core = [
        ("hunger", n.hunger), ("energy", n.energy), ("safety", n.safety),
        ("social", n.social), ("meaning", n.meaning), ("belonging", n.belonging),
    ]
    allvals = ", ".join(f"{k}={v:.2f}" for k, v in core) + f", money={n.money:.0f}"
    low = [f"{k}={v:.2f}" for k, v in core if v < _LOW_NEED]
    if low:
        return f"URGENT low needs: {', '.join(low)}.\nAll needs: {allvals}"
    return f"No urgent needs — this person is comfortable.\nAll needs: {allvals}"


def _agent_brief(a: "Agent") -> str:
    pers = a.personality
    n = a.needs
    traits = ", ".join(pers.unique_traits) or "—"
    return (
        f"Name: {a.name}, age {a.age}.\n"
        f"Personality: O={pers.openness:.2f} C={pers.conscientiousness:.2f} "
        f"E={pers.extraversion:.2f} A={pers.agreeableness:.2f} N={pers.neuroticism:.2f}. "
        f"Traits: {traits}.\n"
        f"State: hunger={n.hunger:.2f}, energy={n.energy:.2f}, "
        f"safety={n.safety:.2f}, social={n.social:.2f}, "
        f"meaning={n.meaning:.2f}, money={n.money:.0f}."
    )


def build_dialogue_prompt(a: "Agent", other: "Agent", context: str = "") -> List[Dict[str, str]]:
    return [
        {"role": "system", "content": SYSTEM_ROLEPLAY},
        {"role": "user", "content": (
            f"{_agent_brief(a)}\n"
            f"You are talking to a person named {other.name}.\n"
            f"Context: {context or 'a chance meeting on the street'}.\n"
            "What do you say to them?"
        )},
    ]


def build_thought_prompt(a: "Agent", world: "World") -> List[Dict[str, str]]:
    hour = world.time_system.hour
    return [
        {"role": "system", "content": SYSTEM_THOUGHT},
        {"role": "user", "content": (
            f"{_agent_brief(a)}\n"
            f"It is now {int(hour):02d}:00. You are doing: {a.current_action}.\n"
            "What thought is going through your head right now?"
        )},
    ]


def build_decision_prompt(a: "Agent", world: "World", actions: List[str]) -> List[Dict[str, str]]:
    pers = a.personality
    hour = int(world.time_system.hour)
    night = hour < config.DAYTIME_START_HOUR or hour >= config.NIGHTTIME_START_HOUR
    tod = "night (most people sleep)" if night else "daytime"
    traits = ", ".join(pers.unique_traits) or "—"
    return [
        {"role": "system", "content": SYSTEM_DECISION.format(actions=", ".join(actions))},
        {"role": "user", "content": (
            f"{a.name}, age {a.age}. Traits: {traits}.\n"
            f"Personality: O={pers.openness:.2f} C={pers.conscientiousness:.2f} "
            f"E={pers.extraversion:.2f} A={pers.agreeableness:.2f} N={pers.neuroticism:.2f}.\n"
            f"Needs (0=critical, 1=satisfied): {_needs_summary(a.needs)}.\n"
            f"Time: {hour:02d}:00, {tod}.\n"
            "Their one best action right now (one word):"
        )},
    ]
