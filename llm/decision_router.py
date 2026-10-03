"""
decision_router.py — Decides WHEN to spend an LLM call on a given agent.
Most ticks should be 0 calls; only ~15-20% of decisions go to the LLM.
"""

from __future__ import annotations
import time
from typing import Optional, TYPE_CHECKING

import config
from llm.openrouter_client import OpenRouterClient
from llm.prompts import build_decision_prompt, build_dialogue_prompt, build_thought_prompt

if TYPE_CHECKING:
    from agents.agent import Agent
    from world.world import World


class DecisionRouter:
    def __init__(self, client: OpenRouterClient) -> None:
        self.client = client
        self.last_call_time = 0.0
        self.calls_this_minute = 0
        self._minute_window_start = time.time()

    # ── Budget ────────────────────────────────────────────────────────────────
    def _under_budget(self) -> bool:
        now = time.time()
        if now - self._minute_window_start > 60.0:
            self._minute_window_start = now
            self.calls_this_minute = 0
        return self.calls_this_minute < config.LLM_MAX_CALLS_PER_MINUTE

    # ── Gating ────────────────────────────────────────────────────────────────
    def should_consult(self, agent: "Agent", world: "World") -> bool:
        if self.client is None or not self.client.enabled or not self._under_budget():
            return False

        # Interestingness heuristic
        score = 0.0
        n = agent.needs
        if n.is_critical():
            score += 0.5
        if abs(n.social - 0.5) > 0.35:
            score += 0.2
        if agent.relationships.rivals:
            score += 0.15
        if agent.faction_id != -1:
            score += 0.15
        if agent.personality.openness > 0.7:
            score += 0.10

        # Roll
        return world.rng.random() < min(score, config.LLM_MAX_CONSULT_PROBABILITY)

    def consult(self, agent: "Agent", world: "World", current_action: str) -> Optional[str]:
        if not self._under_budget():
            return None
        prompt = build_decision_prompt(agent, world, list(
            ("eat", "sleep", "work", "socialize", "drink_at_bar",
             "go_home", "wander", "pray", "shop", "commit_crime", "flee")
        ))
        self.calls_this_minute += 1
        fut = self.client.submit(prompt, max_tokens=8, temperature=0.7)
        try:
            resp = fut.result(timeout=config.LLM_GAME_WAIT_SECONDS)
        except Exception:  # noqa: BLE001
            return None
        if resp.error or not resp.text:
            return None
        word = resp.text.strip().lower().split()[0] if resp.text.strip() else ""
        word = word.strip(".,!? ").strip()
        valid = {"eat","sleep","work","socialize","drink_at_bar","go_home",
                 "wander","pray","shop","commit_crime","flee"}
        return word if word in valid else None

    def dialogue(self, agent: "Agent", other: "Agent", context: str = "") -> str:
        if self.client is None or not self.client.enabled or not self._under_budget():
            return ""
        self.calls_this_minute += 1
        prompt = build_dialogue_prompt(agent, other, context)
        fut = self.client.submit(prompt, max_tokens=80, temperature=0.9)
        try:
            resp = fut.result(timeout=config.LLM_GAME_WAIT_SECONDS)
        except Exception:  # noqa: BLE001
            return ""
        return "" if resp.error else resp.text

    def thought(self, agent: "Agent", world: "World") -> str:
        if self.client is None or not self.client.enabled or not self._under_budget():
            return ""
        self.calls_this_minute += 1
        prompt = build_thought_prompt(agent, world)
        fut = self.client.submit(prompt, max_tokens=30, temperature=0.95)
        try:
            resp = fut.result(timeout=config.LLM_GAME_WAIT_SECONDS)
        except Exception:  # noqa: BLE001
            return ""
        return "" if resp.error else resp.text
