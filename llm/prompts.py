"""
prompts.py — Builds system+user prompts for LLM calls.
All prompts are in English.
"""

from __future__ import annotations
from typing import List, Dict, TYPE_CHECKING

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
    "You suggest an action for a character. Answer with a single word from "
    "this list: {actions}. No explanations."
)


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
    return [
        {"role": "system", "content": SYSTEM_DECISION.format(actions=", ".join(actions))},
        {"role": "user", "content": (
            f"{_agent_brief(a)}\n"
            f"Hour: {world.time_system.hour}. What is the sensible thing to do now?"
        )},
    ]
