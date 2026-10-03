"""
god_mode.py — Debug/cheat overlay. Toggle with G.

While active, pick a tool with number keys 1..6 and left-click on the map to
apply it. All interventions post to the world event bus, so they also appear
in the event feed.

Tools:
  1 Smite    — kill the agent under the cursor
  2 Bless    — restore all of an agent's needs (and give money)
  3 Starve   — drain an agent's hunger & energy to critical
  4 Incite   — make an agent attempt a crime right now
  5 Spawn    — create a new citizen at the clicked tile
  6 Gang     — raise a gang from the agents near the click
"""

from __future__ import annotations
from typing import TYPE_CHECKING, Optional, Tuple

import pygame

import config
from world.events import WorldEvent

if TYPE_CHECKING:
    from world.world import World
    from rendering.camera import Camera
    from agents.agent import Agent


# Tool ids
SMITE, BLESS, STARVE, INCITE, SPAWN, GANG = range(6)

TOOLS = [
    ("1", "Smite", (230, 90, 80)),
    ("2", "Bless", (120, 220, 130)),
    ("3", "Starve", (220, 180, 90)),
    ("4", "Incite", (230, 140, 80)),
    ("5", "Spawn", (120, 180, 230)),
    ("6", "Gang", (200, 120, 220)),
]

# Click hit-test radius in tiles (matches the agent panel's generosity).
_PICK_RADIUS_SQ = 5.0 * 5.0
# Radius within which "Gang" recruits agents.
_GANG_RADIUS = 8.0


class GodMode:
    def __init__(self, world: "World") -> None:
        self.world = world
        self.active: bool = False
        self.tool: int = SMITE
        self.font = pygame.font.SysFont("arial", 13, bold=True)
        self.small = pygame.font.SysFont("arial", 12)
        self._flash: str = ""          # transient confirmation message
        self._flash_until: float = 0.0

    # ── State ───────────────────────────────────────────────────────────────────
    def toggle(self) -> None:
        self.active = not self.active

    def select_tool(self, index: int) -> bool:
        """Select a tool by 0-based index. Returns True if it was a valid tool."""
        if 0 <= index < len(TOOLS):
            self.tool = index
            name = TOOLS[index][1]
            self._notify(f"Tool: {name}")
            return True
        return False

    def _notify(self, msg: str) -> None:
        self._flash = msg
        self._flash_until = pygame.time.get_ticks() / 1000.0 + 2.5

    # ── Picking ──────────────────────────────────────────────────────────────────
    def _agent_at(self, tx: float, ty: float, require_alive: bool = True) -> Optional["Agent"]:
        best: Optional["Agent"] = None
        best_d = _PICK_RADIUS_SQ
        for a in self.world.agents:
            if require_alive and not a.alive:
                continue
            d = (a.x - tx) ** 2 + (a.y - ty) ** 2
            if d <= best_d:
                best_d = d
                best = a
        return best

    # ── Apply ────────────────────────────────────────────────────────────────────
    def apply_click(self, pos: Tuple[int, int], camera: "Camera") -> None:
        if not self.active:
            return
        tx, ty = camera.screen_to_tile(*pos)
        world = self.world

        if self.tool == SPAWN:
            agent = world.spawn_agent(int(tx), int(ty))
            self._notify(f"Spawned {agent.name}" if agent else "No housing available")
            return

        if self.tool == GANG:
            self._raise_gang(tx, ty)
            return

        # Remaining tools act on an agent under the cursor.
        agent = self._agent_at(tx, ty)
        if agent is None:
            self._notify("No one there")
            return

        if self.tool == SMITE:
            agent.die(world, reason="smited by the hand of god")
            self._notify(f"Smote {agent.name}")

        elif self.tool == BLESS:
            n = agent.needs
            n.hunger = n.energy = n.safety = 1.0
            n.social = n.meaning = n.belonging = 1.0
            n.money += 100.0
            world.events.post(WorldEvent(
                kind="blessing", actor_id=agent.id, location=(agent.x, agent.y),
                importance=0.3, text=f"{agent.name} was blessed",
            ))
            self._notify(f"Blessed {agent.name}")

        elif self.tool == STARVE:
            agent.needs.hunger = 0.03
            agent.needs.energy = 0.03
            world.events.post(WorldEvent(
                kind="affliction", actor_id=agent.id, location=(agent.x, agent.y),
                importance=0.4, text=f"{agent.name} is wracked by hunger",
            ))
            self._notify(f"Starved {agent.name}")

        elif self.tool == INCITE:
            target = agent._pick_crime_target(world)
            kind = "assault" if target is not None else "theft"
            world.crime.attempt_crime(agent, world, kind=kind, target=target)
            self._notify(f"{agent.name} turns to crime")

    def _raise_gang(self, tx: float, ty: float) -> None:
        world = self.world
        nearby = [
            a for a in world.agents
            if a.alive and a.faction_id == -1
            and (a.x - tx) ** 2 + (a.y - ty) ** 2 <= _GANG_RADIUS * _GANG_RADIUS
        ]
        if len(nearby) < 2:
            self._notify("Not enough recruits nearby")
            return
        fs = world.factions
        idx = world.rng.randrange(len(fs.GANG_NAMES))
        name = fs.GANG_NAMES[idx]
        color = fs.GANG_COLORS[idx % len(fs.GANG_COLORS)]
        faction = fs._create("gang", name, color, ideology="power in the streets")
        for a in nearby:
            fs.recruit(faction.id, a.id)
            a.faction_id = faction.id
        leader = world.get_agent(faction.leader_id)
        world.events.post(WorldEvent(
            kind="faction_formed", actor_id=faction.leader_id,
            location=(int(tx), int(ty)), importance=0.8,
            text=f"Gang '{name}' formed with {len(nearby)} members",
        ))
        self._notify(f"Raised {name} ({len(nearby)} members)")

    # ── Draw ─────────────────────────────────────────────────────────────────────
    def draw_indicator(self, screen: pygame.Surface) -> None:
        if not self.active:
            return

        # Top badge.
        text = self.font.render("✦ GOD MODE ✦", True, config.PALETTE.ui_accent)
        x = screen.get_width() // 2 - text.get_width() // 2
        y = 8
        pad = 8
        bg = pygame.Surface((text.get_width() + pad * 2, text.get_height() + pad), pygame.SRCALPHA)
        bg.fill((*config.PALETTE.ui_bg, 200))
        screen.blit(bg, (x - pad, y - pad // 2))
        screen.blit(text, (x, y))

        # Bottom toolbar.
        self._draw_toolbar(screen)

        # Transient confirmation message.
        now = pygame.time.get_ticks() / 1000.0
        if self._flash and now < self._flash_until:
            msg = self.font.render(self._flash, True, config.PALETTE.ui_text)
            mx = screen.get_width() // 2 - msg.get_width() // 2
            screen.blit(msg, (mx, 34))

    def _draw_toolbar(self, screen: pygame.Surface) -> None:
        cell_w, cell_h, gap = 92, 30, 6
        total = len(TOOLS) * cell_w + (len(TOOLS) - 1) * gap
        start_x = screen.get_width() // 2 - total // 2
        y = screen.get_height() - cell_h - 12

        # Backing strip.
        strip = pygame.Surface((total + 24, cell_h + 16), pygame.SRCALPHA)
        strip.fill((*config.PALETTE.ui_bg, 210))
        screen.blit(strip, (start_x - 12, y - 8))

        for i, (key, name, color) in enumerate(TOOLS):
            cx = start_x + i * (cell_w + gap)
            selected = (i == self.tool)
            cell = pygame.Surface((cell_w, cell_h), pygame.SRCALPHA)
            cell.fill((*color, 230) if selected else (*config.PALETTE.ui_panel, 220))
            screen.blit(cell, (cx, y))
            if selected:
                pygame.draw.rect(screen, config.PALETTE.ui_accent, (cx, y, cell_w, cell_h), 2)
            label_color = (20, 18, 24) if selected else config.PALETTE.ui_text
            label = self.small.render(f"{key}  {name}", True, label_color)
            screen.blit(label, (cx + 8, y + (cell_h - label.get_height()) // 2))
