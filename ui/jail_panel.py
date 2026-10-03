"""
jail_panel.py — Toggleable roster of jailed agents (hotkey J).

Lists every agent currently serving time: name, the crime they were convicted
of, their sentence length, and how much they've served. Sentence/served are in
ticks (the sim's native unit) plus a percentage so it reads at a glance.
"""

from __future__ import annotations
from typing import TYPE_CHECKING

import pygame

import config

if TYPE_CHECKING:
    from world.world import World


class JailPanel:
    WIDTH = 580
    PAD = 14
    ROW_H = 20
    MAX_ROWS = 22

    def __init__(self, screen: pygame.Surface, world: "World") -> None:
        self.screen = screen
        self.world = world
        self.font = pygame.font.SysFont("arial", 13)
        self.small = pygame.font.SysFont("arial", 11)
        self.title_font = pygame.font.SysFont("arial", 15, bold=True)
        self.visible: bool = False

    def toggle_visible(self) -> None:
        self.visible = not self.visible

    def _inmates(self) -> list:
        inmates = [a for a in getattr(self.world, "agents", [])
                   if getattr(a, "alive", True) and getattr(a, "arrested_ticks", 0) > 0]
        # Longest-remaining first (most serious / freshest sentences on top).
        inmates.sort(key=lambda a: a.arrested_ticks, reverse=True)
        return inmates

    def draw(self) -> None:
        if not self.visible:
            return
        inmates = self._inmates()
        shown = inmates[: self.MAX_ROWS]

        sw, sh = self.screen.get_width(), self.screen.get_height()
        h = self.PAD * 2 + 30 + 22 + max(1, len(shown)) * self.ROW_H + 18
        w = self.WIDTH
        x = sw // 2 - w // 2
        y = sh // 2 - h // 2

        panel = pygame.Surface((w, h), pygame.SRCALPHA)
        panel.fill((*config.PALETTE.ui_bg, 240))
        self.screen.blit(panel, (x, y))
        pygame.draw.rect(self.screen, config.PALETTE.ui_accent, (x, y, w, h), 1)

        cur_y = y + self.PAD
        title = self.title_font.render(f"JAIL ROSTER  ({len(inmates)})   [J]",
                                       True, config.PALETTE.ui_accent)
        self.screen.blit(title, (x + self.PAD, cur_y))
        cur_y += 30

        # Column headers
        cols = [(self.PAD, "NAME"), (190, "CRIME"), (320, "SERVED / SENTENCE"), (510, "%")]
        for cx, label in cols:
            self.screen.blit(self.small.render(label, True, config.PALETTE.ui_text_dim),
                             (x + cx, cur_y))
        cur_y += 20
        pygame.draw.line(self.screen, config.PALETTE.ui_text_dim,
                         (x + self.PAD, cur_y), (x + w - self.PAD, cur_y), 1)
        cur_y += 4

        if not shown:
            self.screen.blit(self.font.render("No one is in jail.", True, config.PALETTE.ui_text_dim),
                             (x + self.PAD, cur_y + 2))
        for a in shown:
            total = max(1, getattr(a, "sentence_total", 0))
            remaining = a.arrested_ticks
            served = max(0, total - remaining)
            pct = int(served / total * 100)
            crime = getattr(a, "jailed_for", "") or "—"
            row = [
                (self.PAD, a.name[:24], config.PALETTE.ui_text),
                (190, crime, config.PALETTE.ui_text),
                (320, f"{served} / {total}", config.PALETTE.ui_text),
                (510, f"{pct}%", config.PALETTE.ui_text_good if pct >= 66 else config.PALETTE.ui_text),
            ]
            for cx, text, color in row:
                self.screen.blit(self.font.render(str(text), True, color), (x + cx, cur_y))
            cur_y += self.ROW_H

        if len(inmates) > self.MAX_ROWS:
            self.screen.blit(
                self.small.render(f"...and {len(inmates) - self.MAX_ROWS} more",
                                  True, config.PALETTE.ui_text_dim),
                (x + self.PAD, cur_y))
