"""
god_mode.py — Debug/cheat overlay. Toggle with G.

While active, pick a tool with number keys 1..9 and left-click on the map to
apply it. All interventions post to the world event bus, so they also appear
in the event feed.

Tools:
  1 Smite    — kill the agent under the cursor
  2 Bless    — restore all of an agent's needs (and give money)
  3 Starve   — drain an agent's hunger & energy to critical
  4 Incite   — make an agent attempt a crime right now
  5 Spawn    — create a new citizen at the clicked tile
  6 Gang     — raise a gang from the agents near the click
  7 Cult     — raise a cult from the agents near the click
  8 Riot     — ignite a riot: nearby agents turn violent
  9 Needs    — open a slider editor to set an agent's needs by hand
"""

from __future__ import annotations
from typing import TYPE_CHECKING, Optional, Tuple, List

import pygame

import config
from world.events import WorldEvent

if TYPE_CHECKING:
    from world.world import World
    from rendering.camera import Camera
    from agents.agent import Agent


# Tool ids
SMITE, BLESS, STARVE, INCITE, SPAWN, GANG, CULT, RIOT, NEEDS = range(9)

TOOLS = [
    ("1", "Smite", (230, 90, 80)),
    ("2", "Bless", (120, 220, 130)),
    ("3", "Starve", (220, 180, 90)),
    ("4", "Incite", (230, 140, 80)),
    ("5", "Spawn", (120, 180, 230)),
    ("6", "Gang", (200, 120, 220)),
    ("7", "Cult", (230, 200, 70)),
    ("8", "Riot", (230, 70, 60)),
    ("9", "Needs", (120, 200, 210)),
]

# Click hit-test radius in tiles (matches the agent panel's generosity).
_PICK_RADIUS_SQ = 5.0 * 5.0
# Radius within which "Gang"/"Cult" recruit and "Riot" spreads.
_FACTION_RADIUS = 8.0
_RIOT_RADIUS = 10.0
_RIOT_MAX = 12  # cap rioters so one click can't storm the whole map

# Need-editor layout (fixed screen coords so hit-testing needs no screen size).
_ED_X, _ED_Y, _ED_W = 340, 60, 300
_ED_ROW_H = 26
_ED_BAR_X = _ED_X + 92
_ED_BAR_W = 180
# (need name, max value) — money is absolute, the rest are 0..1.
_NEED_SPECS: List[Tuple[str, float]] = [
    ("hunger", 1.0), ("energy", 1.0), ("safety", 1.0), ("social", 1.0),
    ("meaning", 1.0), ("belonging", 1.0), ("money", 500.0),
]


class GodMode:
    def __init__(self, world: "World") -> None:
        self.world = world
        self.active: bool = False
        self.tool: int = SMITE
        self.font = pygame.font.SysFont("arial", 13, bold=True)
        self.small = pygame.font.SysFont("arial", 12)
        self._flash: str = ""          # transient confirmation message
        self._flash_until: float = 0.0
        self.editor_agent_id: int = -1  # agent whose needs are being edited
        self._drag_need: Optional[str] = None

    # ── State ───────────────────────────────────────────────────────────────────
    def toggle(self) -> None:
        self.active = not self.active
        if not self.active:
            self.close_editor()

    def select_tool(self, index: int) -> bool:
        """Select a tool by 0-based index. Returns True if it was a valid tool."""
        if 0 <= index < len(TOOLS):
            self.tool = index
            if index != NEEDS:
                self.close_editor()
            self._notify(f"Tool: {TOOLS[index][1]}")
            return True
        return False

    def close_editor(self) -> None:
        self.editor_agent_id = -1
        self._drag_need = None

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

    # ── Mouse API (called from main.py when God Mode is active) ───────────────────
    def on_mouse_down(self, pos: Tuple[int, int], camera: "Camera") -> None:
        if not self.active:
            return
        # Needs editor takes priority if it's open and the click lands on a slider.
        if self.editor_agent_id != -1:
            need = self._slider_at(pos)
            if need is not None:
                self._drag_need = need
                self._set_need_from_x(need, pos[0])
                return
        tx, ty = camera.screen_to_tile(*pos)
        if self.tool == NEEDS:
            agent = self._agent_at(tx, ty)
            if agent is None:
                self.close_editor()
                self._notify("No one there")
            else:
                self.editor_agent_id = agent.id
                self._notify(f"Editing {agent.name}")
            return
        self._apply_tool(tx, ty)

    def on_mouse_motion(self, pos: Tuple[int, int]) -> None:
        if self._drag_need is not None:
            self._set_need_from_x(self._drag_need, pos[0])

    def on_mouse_up(self) -> None:
        self._drag_need = None

    # ── Tool application ──────────────────────────────────────────────────────────
    def _apply_tool(self, tx: float, ty: float) -> None:
        world = self.world

        if self.tool == SPAWN:
            agent = world.spawn_agent(int(tx), int(ty))
            self._notify(f"Spawned {agent.name}" if agent else "No housing available")
            return
        if self.tool == GANG:
            self._raise_faction("gang", tx, ty)
            return
        if self.tool == CULT:
            self._raise_faction("cult", tx, ty)
            return
        if self.tool == RIOT:
            self._ignite_riot(tx, ty)
            return

        # Remaining tools act on a single agent under the cursor.
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

    def _nearby(self, tx: float, ty: float, radius: float, unfactioned: bool = False) -> List["Agent"]:
        r2 = radius * radius
        out = []
        for a in self.world.agents:
            if not a.alive:
                continue
            if unfactioned and a.faction_id != -1:
                continue
            if (a.x - tx) ** 2 + (a.y - ty) ** 2 <= r2:
                out.append(a)
        return out

    def _raise_faction(self, kind: str, tx: float, ty: float) -> None:
        world = self.world
        recruits = self._nearby(tx, ty, _FACTION_RADIUS, unfactioned=True)
        if len(recruits) < 2:
            self._notify("Not enough recruits nearby")
            return
        fs = world.factions
        names = fs.GANG_NAMES if kind == "gang" else fs.CULT_NAMES
        colors = fs.GANG_COLORS if kind == "gang" else fs.CULT_COLORS
        ideology = "power in the streets" if kind == "gang" else "revelation"
        idx = world.rng.randrange(len(names))
        name = names[idx]
        color = colors[idx % len(colors)]
        faction = fs._create(kind, name, color, ideology=ideology)
        for a in recruits:
            fs.recruit(faction.id, a.id)
            a.faction_id = faction.id
            a.needs.belonging = min(1.0, a.needs.belonging + 0.3)
        label = "Gang" if kind == "gang" else "Cult"
        world.events.post(WorldEvent(
            kind="faction_formed", actor_id=faction.leader_id,
            location=(int(tx), int(ty)), importance=0.8,
            text=f"{label} '{name}' formed with {len(recruits)} members",
        ))
        self._notify(f"Raised {name} ({len(recruits)} members)")

    def _ignite_riot(self, tx: float, ty: float) -> None:
        world = self.world
        crowd = self._nearby(tx, ty, _RIOT_RADIUS)
        rioters = [a for a in crowd if not getattr(a, "is_police", False)]
        if len(rioters) < 2:
            self._notify("No crowd to incite")
            return
        world.rng.shuffle(rioters)
        rioters = rioters[:_RIOT_MAX]
        acted = 0
        for a in rioters:
            a.needs.safety = max(0.0, a.needs.safety - 0.4)
            if world.rng.random() < 0.7:
                target = a._pick_crime_target(world)
                world.crime.attempt_crime(a, world, kind="riot", target=target)
                acted += 1
        world.events.post(WorldEvent(
            kind="riot", actor_id=rioters[0].id, location=(int(tx), int(ty)),
            importance=0.95, text=f"A riot erupts — {acted} rioters rampage",
        ))
        self._notify(f"Riot! {acted} rioters")

    # ── Need editor ───────────────────────────────────────────────────────────────
    def _editor_agent(self) -> Optional["Agent"]:
        if self.editor_agent_id == -1:
            return None
        a = self.world.get_agent(self.editor_agent_id)
        if a is None or not a.alive:
            self.close_editor()
            return None
        return a

    def _slider_at(self, pos: Tuple[int, int]) -> Optional[str]:
        mx, my = pos
        if not (_ED_BAR_X - 6 <= mx <= _ED_BAR_X + _ED_BAR_W + 6):
            return None
        for i, (name, _max) in enumerate(_NEED_SPECS):
            bar_y = _ED_Y + 34 + i * _ED_ROW_H
            if bar_y - 6 <= my <= bar_y + 14:
                return name
        return None

    def _set_need_from_x(self, need: str, mx: int) -> None:
        agent = self._editor_agent()
        if agent is None:
            return
        frac = (mx - _ED_BAR_X) / _ED_BAR_W
        frac = max(0.0, min(1.0, frac))
        max_val = dict(_NEED_SPECS)[need]
        setattr(agent.needs, need, frac * max_val)

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

        self._draw_toolbar(screen)
        if self.editor_agent_id != -1:
            self._draw_editor(screen)

        now = pygame.time.get_ticks() / 1000.0
        if self._flash and now < self._flash_until:
            msg = self.font.render(self._flash, True, config.PALETTE.ui_text)
            mx = screen.get_width() // 2 - msg.get_width() // 2
            screen.blit(msg, (mx, 34))

    def _draw_toolbar(self, screen: pygame.Surface) -> None:
        cell_w, cell_h, gap = 84, 30, 6
        total = len(TOOLS) * cell_w + (len(TOOLS) - 1) * gap
        start_x = screen.get_width() // 2 - total // 2
        y = screen.get_height() - cell_h - 12

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
            label = self.small.render(f"{key} {name}", True, label_color)
            screen.blit(label, (cx + 8, y + (cell_h - label.get_height()) // 2))

    def _draw_editor(self, screen: pygame.Surface) -> None:
        agent = self._editor_agent()
        if agent is None:
            return
        rows = len(_NEED_SPECS)
        h = 34 + rows * _ED_ROW_H + 24
        panel = pygame.Surface((_ED_W, h), pygame.SRCALPHA)
        panel.fill((*config.PALETTE.ui_bg, 235))
        screen.blit(panel, (_ED_X, _ED_Y))
        pygame.draw.rect(screen, config.PALETTE.ui_accent, (_ED_X, _ED_Y, _ED_W, h), 1)

        title = self.font.render(f"Set needs: {agent.name}", True, config.PALETTE.ui_accent)
        screen.blit(title, (_ED_X + 10, _ED_Y + 8))

        for i, (name, max_val) in enumerate(_NEED_SPECS):
            bar_y = _ED_Y + 34 + i * _ED_ROW_H
            label = self.small.render(name, True, config.PALETTE.ui_text)
            screen.blit(label, (_ED_X + 10, bar_y - 2))
            # Track
            pygame.draw.rect(screen, (50, 50, 60), (_ED_BAR_X, bar_y, _ED_BAR_W, 8))
            val = float(getattr(agent.needs, name, 0.0))
            frac = max(0.0, min(1.0, val / max_val))
            if name == "money":
                color = config.PALETTE.ui_accent
            elif frac < config.NEED_CRITICAL_THRESHOLD:
                color = config.PALETTE.ui_text_danger
            elif frac < config.NEED_LOW_THRESHOLD:
                color = (220, 180, 90)
            else:
                color = config.PALETTE.ui_text_good
            pygame.draw.rect(screen, color, (_ED_BAR_X, bar_y, int(_ED_BAR_W * frac), 8))
            # Value text
            vtxt = f"{val:.0f}" if name == "money" else f"{val:.2f}"
            vsurf = self.small.render(vtxt, True, config.PALETTE.ui_text_dim)
            screen.blit(vsurf, (_ED_BAR_X + _ED_BAR_W + 8, bar_y - 2))

        hint = self.small.render("drag sliders · ESC to close", True, config.PALETTE.ui_text_dim)
        screen.blit(hint, (_ED_X + 10, _ED_Y + h - 18))
