"""
agent.py — A single citizen. Owns personality, needs, relationships, memory,
current state, position, and the tick loop that translates utility-AI choices
into concrete actions in the world.
"""

from __future__ import annotations
from dataclasses import dataclass, field
from typing import List, Tuple, Optional, TYPE_CHECKING
import random

import config
from agents.personality import Personality
from agents.needs import Needs
from agents.relationships import RelationshipBook
from agents.memory import AgentMemory, MemoryEntry
from agents.utility_ai import UtilityAI
from agents.pathing import find_path
from world.buildings import BuildingType
from world.events import WorldEvent

if TYPE_CHECKING:
    from world.world import World
    from world.buildings import Building


FIRST_NAMES = (
    "Alex","Anna","Boris","Vera","Victor","Grace","Dmitri","Eugene",
    "Catherine","Ivan","Igor","Irene","Conrad","Laura","Leo","Linda",
    "Max","Marina","Natalie","Nicholas","Oleg","Olga","Paul","Polina",
    "Roman","Svetlana","Sergei","Tanya","Yuri","Julia","Andrew","Arthur",
    "Michael","Zoe","Stephen","Timothy","Rose","Fred","Tamara","Lydia",
)
LAST_NAMES = (
    "Ivanov","Petrov","Sidorov","Kuznetsov","Smirnov","Vasiliev","Popov",
    "Sokolov","Mikhailov","Novikov","Fedorov","Morozov","Volkov","Alexeev",
    "Lebedev","Semenov","Egorov","Pavlov","Kozlov","Stepanov","Nikolaev",
    "Orlov","Andreev","Makarov","Nikitin","Zakharov","Zaitsev","Soloviev",
)


@dataclass
class Agent:
    id: int
    name: str
    age: int
    home_id: int
    workplace_id: int
    personality: Personality
    needs: Needs
    relationships: RelationshipBook
    memory: AgentMemory
    x: int
    y: int
    target_x: int = -1
    target_y: int = -1
    path: List[Tuple[int, int]] = field(default_factory=list)
    current_action: str = "wander"
    action_progress: float = 0.0
    color: Tuple[int, int, int] = (200, 200, 200)
    faction_id: int = -1
    is_police: bool = False
    alive: bool = True
    arrested_ticks: int = 0
    wanted: bool = False
    wanted_ticks: int = 0      # lie-low countdown while evading police
    wanted_for: str = ""       # the crime they're wanted for
    jailed_for: str = ""       # crime they're currently serving time for
    sentence_total: int = 0    # full sentence length (ticks); served = total - arrested_ticks
    sleep_ticks: int = 0
    speech_cooldown: float = 0.0
    last_dialogue: str = ""
    last_thought: str = ""
    interior_building: int = -1   # building id if inside
    # ── Movement (not persisted) ─────────────────────────────────────────────
    # Accumulates dt × speed; an agent advances one tile every time this
    # crosses 1.0. Decoupled from tick rate so visual speed is consistent.
    _move_progress: float = 0.0
    # Facing direction: "S" (down), "N" (up), "E" (right), "W" (left).
    # Used by the renderer to flip/rotate the sprite.
    facing: str = "S"
    # Animation phase for stepping (renderer toggles between two leg poses).
    _step_phase: float = 0.0

    # ── Construction ──────────────────────────────────────────────────────────
    @classmethod
    def spawn(cls, aid: int, home: "Building", rng: random.Random, world: "World") -> "Agent":
        name = f"{rng.choice(FIRST_NAMES)} {rng.choice(LAST_NAMES)}"
        age = max(16, int(rng.gauss(35, 14)))
        # Position at one of the home's perimeter tiles
        x, y = cls._adjacent_walkable(home, world)
        pers = Personality.random(rng)
        col = (
            min(255, max(60, int(rng.gauss(160, 40)))),
            min(255, max(60, int(rng.gauss(160, 40)))),
            min(255, max(60, int(rng.gauss(160, 40)))),
        )
        return cls(
            id=aid, name=name, age=age,
            home_id=home.id, workplace_id=-1,
            personality=pers,
            needs=Needs(money=max(0.0, rng.gauss(config.STARTING_MONEY_MEAN,
                                                 config.STARTING_MONEY_STDDEV))),
            relationships=RelationshipBook(),
            memory=AgentMemory(),
            x=x, y=y, color=col,
        )

    @staticmethod
    def _adjacent_walkable(building, world: "World") -> Tuple[int, int]:
        for dx in range(-1, building.w + 1):
            for dy in range(-1, building.h + 1):
                tx, ty = building.x + dx, building.y + dy
                if not world.tile_map.in_bounds(tx, ty):
                    continue
                if world.tile_map.is_walkable(tx, ty):
                    return tx, ty
        return building.x, building.y

    # ── Tick ──────────────────────────────────────────────────────────────────
    def tick(self, dt: float, world: "World") -> None:
        if not self.alive:
            return

        if self.arrested_ticks > 0:
            self.arrested_ticks -= 1
            if self.arrested_ticks == 0:
                # Released: clear jail record.
                self.jailed_for = ""
                self.sentence_total = 0
            return

        # Convert dt seconds → in-game hours
        game_hours = dt * config.TIME_SCALE / 3600.0
        self.needs.decay(game_hours)
        if self.speech_cooldown > 0:
            self.speech_cooldown -= dt

        # Sleeping? regenerate
        if self.current_action == "sleep" and self.sleep_ticks > 0:
            self.needs.energy = min(1.0, self.needs.energy + 0.20 * game_hours * 4)
            self.sleep_ticks -= 1
            return

        # Resting at home restores energy slowly (half the sleep rate) — so
        # "go_home" is a real recovery action, not just standing around.
        if self.current_action == "go_home" and self.interior_building == self.home_id:
            self.needs.energy = min(1.0, self.needs.energy + 0.20 * game_hours * 2)

        # Pick a new action when current one expires
        if self.action_progress <= 0.0:
            self._choose_action(world)

        # Movement (rate-limited by AGENT_SPEED_TILES_PER_SECOND)
        self._move(dt, world)

        # On-arrival effects
        self._execute(dt, world)

        # Tick down current action timer
        self.action_progress -= dt

        if self.needs.is_dying():
            reason = "starvation" if self.needs.hunger <= 0.01 else "exhaustion"
            self.die(world, reason=reason)

    def daily_tick(self, world: "World") -> None:
        self.age += 1 / 365  # placeholder; aging is slow
        # Cost of living: flat rent + upkeep scaling with wealth. Keeps money
        # bounded (agents earn more than they spend) and keeps some economic
        # pressure so poverty — and thus occasional crime — stays a real dynamic.
        cost = config.RENT_PER_DAY + self.needs.money * config.UPKEEP_FRACTION
        self.needs.money = max(0.0, self.needs.money - cost)
        # Try to find a workplace if missing
        if self.workplace_id == -1:
            for b in world.buildings.workplaces():
                if len(b.workers) < b.capacity:
                    b.workers.append(self.id)
                    self.workplace_id = b.id
                    if b.type == BuildingType.POLICE_STATION:
                        self.is_police = True
                    break

    # ── Decision ──────────────────────────────────────────────────────────────
    def _choose_action(self, world: "World") -> None:
        action = UtilityAI.best_action(self, world)
        # Optionally consult LLM (rare, high-importance situations)
        if (world.decision_router is not None
                and config.LLM_ENABLED
                and world.decision_router.should_consult(self, world)):
            override = world.decision_router.consult(self, world, action)
            if override:
                action = override

        self.current_action = action
        self.action_progress = config.ACTION_BASE_DURATION_SECONDS

        # Set target tile based on action
        self.path = []
        self.target_x = -1
        self.target_y = -1

        if action == "eat" and self.needs.hunger < config.NEED_CRITICAL_THRESHOLD:
            # Emergency: a starving agent eats where it is (buys food on the spot)
            # instead of risking death walking to a distant shop. Target stays
            # unset so the eat effect fires immediately on the next tick.
            pass
        elif action == "eat" or action == "shop":
            self._target_building(world, [BuildingType.SHOP])
        elif action == "sleep" or action == "go_home":
            self._target_building_by_id(world, self.home_id)
        elif action == "work":
            if self.workplace_id != -1:
                self._target_building_by_id(world, self.workplace_id)
        elif action == "drink_at_bar":
            self._target_building(world, [BuildingType.BAR])
        elif action == "pray":
            self._target_building(world, [BuildingType.CHURCH])
        elif action == "patrol":
            self._wander_target(world)
        elif action == "socialize":
            self._wander_target(world)
        elif action == "wander" or action == "flee":
            self._wander_target(world)
        elif action == "commit_crime":
            self._wander_target(world)

    def _target_building(self, world: "World", types: List[BuildingType]) -> None:
        candidates = [b for b in world.buildings.buildings if b.type in types]
        if not candidates:
            self._wander_target(world)
            return
        target = min(candidates, key=lambda b: abs(b.x - self.x) + abs(b.y - self.y))
        self._target_building_by_id(world, target.id)

    def _target_building_by_id(self, world: "World", bid: int) -> None:
        b = world.buildings.get(bid)
        if b is None:
            self._wander_target(world)
            return
        tx, ty = Agent._adjacent_walkable(b, world)
        self.target_x, self.target_y = tx, ty
        path = find_path(world.tile_map, (self.x, self.y), (tx, ty))
        self.path = path[1:] if path else []

    def _wander_target(self, world: "World") -> None:
        for _ in range(10):
            tx = self.x + world.rng.randint(-8, 8)
            ty = self.y + world.rng.randint(-8, 8)
            if world.tile_map.in_bounds(tx, ty) and world.tile_map.is_walkable(tx, ty):
                path = find_path(world.tile_map, (self.x, self.y), (tx, ty))
                if path:
                    self.target_x, self.target_y = tx, ty
                    self.path = path[1:]
                    return

    # ── Movement (rate-limited) ───────────────────────────────────────────────
    def _move(self, dt: float, world: "World") -> None:
        """Advance along path at AGENT_SPEED_TILES_PER_SECOND.

        Independent of tick rate — accumulates dt × speed and steps only
        when ≥ 1 tile of progress has built up. Updates `facing` for sprite
        orientation and `_step_phase` for leg animation.
        """
        if not self.path:
            self._step_phase = 0.0
            return

        # Accumulate progress this tick.
        self._move_progress += dt * config.AGENT_SPEED_TILES_PER_SECOND

        # Step as many tiles as we've earned (usually 0 or 1 per tick).
        # Guard against runaway: at most 4 tiles per tick (huge dt spike).
        steps = 0
        while self._move_progress >= 1.0 and self.path and steps < 4:
            nx, ny = self.path[0]
            if world.tile_map.is_walkable(nx, ny) or (nx, ny) == (self.target_x, self.target_y):
                # Update facing before moving.
                if nx > self.x: self.facing = "E"
                elif nx < self.x: self.facing = "W"
                elif ny > self.y: self.facing = "S"
                elif ny < self.y: self.facing = "N"
                self.x, self.y = nx, ny
                self.path.pop(0)
                self._move_progress -= 1.0
                steps += 1
                self._step_phase += 0.5  # toggle leg pose
            else:
                # Blocked — re-path and bail this tick.
                p = find_path(world.tile_map, (self.x, self.y), (self.target_x, self.target_y))
                self.path = p[1:] if p else []
                self._move_progress = 0.0
                return

    # ── Execution (on-arrival effects only) ───────────────────────────────────
    def _execute(self, dt: float, world: "World") -> None:
        arrived = (self.x == self.target_x and self.y == self.target_y) or (
            self.target_x == -1 and not self.path
        )
        if not arrived:
            return

        # On arrival, execute action effect
        if self.current_action == "eat":
            if self.needs.money >= config.MEAL_PRICE:
                self.needs.money -= config.MEAL_PRICE
                self.needs.hunger = min(1.0, self.needs.hunger + 0.6)
                world.economy.record_purchase("food", 1.0)
            elif self.needs.hunger < config.NEED_CRITICAL_THRESHOLD:
                # Destitute and starving: a free charity meal (smaller than a
                # bought one) keeps them alive, so poverty leads to crime, not
                # death. They stay poor and hungry sooner, preserving pressure.
                self.needs.hunger = min(1.0, self.needs.hunger + 0.3)

        elif self.current_action == "sleep":
            self.sleep_ticks = config.SLEEP_TICKS
            self.interior_building = self.home_id

        elif self.current_action == "go_home":
            # Mark as home so the per-tick resting restore kicks in.
            self.interior_building = self.home_id

        elif self.current_action == "work":
            self.needs.money += config.WAGE_PER_SHIFT
            self.needs.energy = max(0.0, self.needs.energy - 0.1)
            self.needs.meaning = min(1.0, self.needs.meaning + 0.05)
            world.economy.record_production("tool", 0.5)

        elif self.current_action == "drink_at_bar":
            if self.needs.money >= config.DRINK_PRICE:
                self.needs.money -= config.DRINK_PRICE
                self.needs.social = min(1.0, self.needs.social + 0.3)
                self.needs.energy = max(0.0, self.needs.energy - 0.05)
                self._try_socialize(world)

        elif self.current_action == "socialize":
            self._try_socialize(world)

        elif self.current_action == "pray":
            self.needs.meaning = min(1.0, self.needs.meaning + 0.4)
            self.needs.safety = min(1.0, self.needs.safety + 0.1)

        elif self.current_action == "shop":
            if self.needs.money >= config.LUXURY_PRICE:
                self.needs.money -= config.LUXURY_PRICE
                self.needs.belonging = min(1.0, self.needs.belonging + 0.2)
                world.economy.record_purchase("luxury", 1.0)

        elif self.current_action == "commit_crime":
            target = self._pick_crime_target(world)
            kind = self._choose_crime_kind(world, target)
            world.crime.attempt_crime(self, world, kind=kind, target=target)

        elif self.current_action == "patrol":
            pass

        elif self.current_action == "flee":
            self.needs.safety = min(1.0, self.needs.safety + 0.05)

        # Force re-decision next tick
        self.action_progress = 0.0

    def _try_socialize(self, world: "World") -> None:
        nearby = [
            a for a in world.agents
            if a.alive and a.id != self.id
            and abs(a.x - self.x) <= 2 and abs(a.y - self.y) <= 2
        ]
        if not nearby:
            return
        other = world.rng.choice(nearby)
        delta = world.rng.uniform(-0.10, 0.25)
        # Compatibility nudge
        compat = 1.0 - abs(self.personality.agreeableness - other.personality.agreeableness)
        delta += (compat - 0.5) * 0.10
        self.relationships.adjust(other.id, delta)
        other.relationships.adjust(self.id, delta * 0.8)
        self.needs.social = min(1.0, self.needs.social + 0.15)
        other.needs.social = min(1.0, other.needs.social + 0.10)
        self.needs.belonging = min(1.0, self.needs.belonging + 0.05)

        rel = self.relationships
        if (rel.likes(other.id)
                and other.id not in rel.announced_friends
                and rel.familiarity.get(other.id, 0.0) >= 0.4):
            # Established friendship (high affinity AND enough shared history),
            # announced once per pair — not on every pleasant chat.
            rel.announced_friends.add(other.id)
            self.memory.remember(MemoryEntry(
                kind="positive_social", text=f"Became friends with {other.name}",
                importance=0.5, other_id=other.id,
            ))
            world.events.post(WorldEvent(
                kind="positive_social", actor_id=self.id, target_id=other.id,
                location=(self.x, self.y), importance=0.25,
                text=f"{self.name} and {other.name} became friends",
            ))
        elif delta > 0.20:
            # A pleasant chat — a private memory, not a city-wide announcement.
            self.memory.remember(MemoryEntry(
                kind="positive_social", text=f"Had a good talk with {other.name}",
                importance=0.3, other_id=other.id,
            ))
        elif delta < -0.05:
            self.memory.remember(MemoryEntry(
                kind="negative_social", text=f"Quarreled with {other.name}",
                importance=0.5, other_id=other.id,
            ))

    def _choose_crime_kind(self, world: "World", target) -> str:
        """Pick a crime type from personality & desperation. Violent traits lean
        toward assault; the desperate-and-broke toward (confrontational) robbery;
        otherwise petty theft. More serious kinds mean longer hunts/sentences."""
        p = self.personality
        if target is None:
            return "theft"
        if (p.has("cruel") or p.has("vengeful") or p.has("brave")) and world.rng.random() < 0.5:
            return "assault"
        if self.needs.money < config.LOW_MONEY_THRESHOLD * 0.5 and world.rng.random() < 0.4:
            return "robbery"
        return "theft"

    def _pick_crime_target(self, world: "World"):
        nearby = [
            a for a in world.agents
            if a.alive and a.id != self.id and not getattr(a, "is_police", False)
            and abs(a.x - self.x) <= 3 and abs(a.y - self.y) <= 3
        ]
        if not nearby:
            return None
        return world.rng.choice(nearby)

    # ── Lifecycle ─────────────────────────────────────────────────────────────
    def die(self, world: "World", reason: str) -> None:
        self.alive = False
        world.events.post(WorldEvent(
            kind="death", actor_id=self.id, location=(self.x, self.y),
            importance=0.9, text=f"{self.name} died ({reason})",
            payload={"reason": reason},
        ))
        # Free up housing/work slots
        home = world.buildings.get(self.home_id)
        if home and self.id in home.residents:
            home.residents.remove(self.id)
        if self.workplace_id != -1:
            wp = world.buildings.get(self.workplace_id)
            if wp and self.id in wp.workers:
                wp.workers.remove(self.id)

    # ── Persistence ───────────────────────────────────────────────────────────
    def to_dict(self) -> dict:
        return {
            "id": self.id, "name": self.name, "age": self.age,
            "home": self.home_id, "work": self.workplace_id,
            "personality": self.personality.to_dict(),
            "needs": self.needs.to_dict(),
            "rel": self.relationships.to_dict(),
            "memory": self.memory.to_dict(),
            "x": self.x, "y": self.y,
            "color": list(self.color),
            "faction": self.faction_id,
            "police": self.is_police,
            "alive": self.alive,
            "facing": self.facing,
        }

    @classmethod
    def from_dict(cls, d: dict, world: "World") -> "Agent":
        a = cls(
            id=d["id"], name=d["name"], age=d["age"],
            home_id=d["home"], workplace_id=d["work"],
            personality=Personality.from_dict(d["personality"]),
            needs=Needs.from_dict(d["needs"]),
            relationships=RelationshipBook.from_dict(d["rel"]),
            memory=AgentMemory.from_dict(d["memory"]),
            x=d["x"], y=d["y"], color=tuple(d["color"]),
            faction_id=d["faction"], is_police=d["police"], alive=d["alive"],
        )
        a.facing = d.get("facing", "S")
        return a
