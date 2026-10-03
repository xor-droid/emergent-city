"""
hud.py — Top status bar: in-game clock, day, population, deaths, money supply,
faction counts, LLM stats.
"""

from __future__ import annotations
import pygame
from typing import TYPE_CHECKING

import config

if TYPE_CHECKING:
    from world.world import World


class HUD:
    def __init__(self, screen: pygame.Surface, world: "World") -> None:
        self.screen = screen
        self.world = world
        self.font = pygame.font.SysFont("arial", 14, bold=True)
        self.small = pygame.font.SysFont("arial", 12)
        self.llm_calls = 0

    def draw(
        self,
        paused: bool = False,
        speed: float = 1.0,
        sim_time=None,
        population: int = 0,
        llm_calls: int = 0,
    ) -> None:
        """Main draw entry-point used by main.py."""
        self.llm_calls = llm_calls
        surf = self.screen
        world = self.world
        bar = pygame.Surface((surf.get_width(), 28), pygame.SRCALPHA)
        bar.fill((0, 0, 0, 170))
        surf.blit(bar, (0, 0))

        ts = sim_time if sim_time is not None else getattr(world, "time_system", None)
        if ts is not None:
            day = getattr(ts, "day", 0)
            hour = getattr(ts, "hour", 0)
            minute = getattr(ts, "minute", 0)
            season = getattr(ts, "season_name", "")
            time_str = f"Day {int(day)}  {int(hour):02d}:{int(minute):02d}  Season: {season}  "
        else:
            time_str = ""

        alive = population if population else sum(
            1 for a in getattr(world, "agents", []) if getattr(a, "alive", True)
        )
        dead = sum(1 for a in getattr(world, "agents", []) if not getattr(a, "alive", True))
        wanted = sum(
            1 for a in getattr(world, "agents", [])
            if getattr(a, "alive", True) and getattr(a, "wanted", False)
        )
        factions = getattr(world, "factions", None)
        n_factions = len(factions.factions) if factions and hasattr(factions, "factions") else 0

        line = (
            f"{time_str}"
            f"Population: {alive}  Deaths: {dead}  Wanted: {wanted}  "
            f"Factions: {n_factions}  "
            f"LLM: {self.llm_calls}"
        )
        if paused:
            line = "[PAUSED]  " + line
        elif speed != 1.0:
            line = f"[x{speed:.1f}]  " + line

        text = self.font.render(line, True, (240, 240, 240))
        surf.blit(text, (8, 6))

    # Backwards-compat alias for older callers
    def render(
        self,
        surf: pygame.Surface,
        world: "World",
        fps: float = 0.0,
        llm_calls: int = 0,
        paused: bool = False,
        speed: float = 1.0,
    ) -> None:
        self.screen = surf
        self.world = world
        self.llm_calls = llm_calls
        self.draw(paused=paused, speed=speed)
