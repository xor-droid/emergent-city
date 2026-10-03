"""
event_feed.py — Right-side feed showing recent world events.

The world owns an EventBus (world.events). This widget pulls the most recent
event strings/objects and renders them in a scrollable-style log. Toggle with
Tab.
"""

from __future__ import annotations
from typing import TYPE_CHECKING

import pygame

import config

if TYPE_CHECKING:
    from world.world import World


class EventFeed:
    WIDTH = 320
    PAD = 10
    LINE_HEIGHT = 16
    MAX_LINES = 24

    def __init__(self, screen: pygame.Surface, world: "World") -> None:
        self.screen = screen
        self.world = world
        self.font = pygame.font.SysFont("arial", 12)
        self.title_font = pygame.font.SysFont("arial", 13, bold=True)
        self.visible: bool = True

    def toggle_visible(self) -> None:
        self.visible = not self.visible

    def _recent_events(self) -> list:
        bus = getattr(self.world, "events", None)
        if bus is None:
            return []
        # Preferred: the bus exposes a recent(n) accessor.
        recent = getattr(bus, "recent", None)
        if callable(recent):
            try:
                return list(recent(self.MAX_LINES))
            except Exception:
                pass
        # Fallback: pull from a list/deque-style attribute.
        for attr in ("log", "items", "events", "_events"):
            data = getattr(bus, attr, None)
            if data is not None and not callable(data):
                try:
                    return list(data)[-self.MAX_LINES:]
                except TypeError:
                    continue
        return []

    @staticmethod
    def _format_event(ev) -> str:
        if isinstance(ev, str):
            return ev
        if isinstance(ev, dict):
            txt = ev.get("text") or ev.get("message") or ev.get("description") or ""
            who = ev.get("who") or ev.get("actor") or ""
            return f"{who}: {txt}" if who else str(txt or ev)
        text = getattr(ev, "text", None) or getattr(ev, "message", None)
        if text:
            return str(text)
        return str(ev)

    def draw(self) -> None:
        if not self.visible:
            return
        w = self.WIDTH
        h = self.screen.get_height()
        x = self.screen.get_width() - w
        y = 0
        # Panel background
        panel = pygame.Surface((w, h), pygame.SRCALPHA)
        panel.fill((*config.PALETTE.ui_bg, 220))
        self.screen.blit(panel, (x, y))
        # Title
        title = self.title_font.render("EVENT FEED  [Tab]", True, config.PALETTE.ui_accent)
        self.screen.blit(title, (x + self.PAD, y + self.PAD))
        # Lines
        events = self._recent_events()
        line_y = y + self.PAD + 24
        for ev in reversed(events):  # newest first
            if line_y > h - self.LINE_HEIGHT:
                break
            text = self._format_event(ev)[: w // 7]
            surf = self.font.render(text, True, config.PALETTE.ui_text)
            self.screen.blit(surf, (x + self.PAD, line_y))
            line_y += self.LINE_HEIGHT
