"""
inspector.py — Right-hand side panel showing the selected agent's
personality, needs, top relationships, recent memories, current action.
"""

from __future__ import annotations
import pygame
from typing import TYPE_CHECKING, Optional

if TYPE_CHECKING:
    from agents.agent import Agent
    from world.world import World


class Inspector:
    def __init__(self, screen_w: int, screen_h: int) -> None:
        self.font = pygame.font.SysFont("arial", 13)
        self.bold = pygame.font.SysFont("arial", 14, bold=True)
        self.w = 280
        self.screen_w = screen_w
        self.screen_h = screen_h

    def render(self, surf: pygame.Surface, agent: Optional["Agent"], world: "World") -> None:
        if agent is None:
            return
        x = surf.get_width() - self.w
        panel = pygame.Surface((self.w, surf.get_height() - 28), pygame.SRCALPHA)
        panel.fill((0, 0, 0, 200))
        surf.blit(panel, (x, 28))

        y = 36
        lines = self._agent_lines(agent, world)
        for line, is_header in lines:
            font = self.bold if is_header else self.font
            color = (255, 220, 120) if is_header else (235, 235, 235)
            txt = font.render(line, True, color)
            surf.blit(txt, (x + 10, y))
            y += 18

    def _agent_lines(self, a: "Agent", world: "World") -> list:
        p, n = a.personality, a.needs
        lines = [
            (f"{a.name} ({a.age})", True),
            (f"Action: {a.current_action}", False),
            (f"Position: ({a.x},{a.y})", False),
            ("Personality", True),
            (f"O {p.openness:.2f}  C {p.conscientiousness:.2f}", False),
            (f"E {p.extraversion:.2f}  A {p.agreeableness:.2f}", False),
            (f"N {p.neuroticism:.2f}", False),
        ]
        if p.unique_traits:
            lines.append((f"Traits: {', '.join(p.unique_traits[:3])}", False))
        lines += [
            ("Needs", True),
            (f"Hunger     {n.hunger:.2f}", False),
            (f"Energy     {n.energy:.2f}", False),
            (f"Safety     {n.safety:.2f}", False),
            (f"Social     {n.social:.2f}", False),
            (f"Belonging  {n.belonging:.2f}", False),
            (f"Meaning    {n.meaning:.2f}", False),
            (f"Money      {n.money:.0f}", False),
        ]

        # Top relationships
        top_rels = a.relationships.top_friends(3)
        if top_rels:
            lines.append(("Relationships", True))
            for other_id, rel in top_rels:
                other = next((o for o in world.agents if o.id == other_id), None)
                if other is None:
                    continue
                lines.append((f"{other.name}: {rel.affinity:+.2f}", False))

        # Recent memories
        mems = a.memory.top_memories(3)
        if mems:
            lines.append(("Memory", True))
            for m in mems:
                lines.append((f"• {m.text[:34]}", False))

        if a.last_thought:
            lines.append(("Thought", True))
            lines.append((a.last_thought[:38], False))

        return lines
