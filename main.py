"""
main.py — Emergent City entry point.

Bootstraps the world, agents, renderer, and UI, then runs the main loop.

Press Space to pause, 1/2/3 to change speed, F to follow an agent,
G for God Mode, Tab to toggle the event feed, Ctrl+S/L to save/load.
"""

from __future__ import annotations

import os
import sys
import random
import logging
from typing import Optional

import pygame
from dotenv import load_dotenv

import config
from world.world import World
from rendering.renderer import Renderer
from rendering.camera import Camera
from ui.hud import HUD
from ui.event_feed import EventFeed
from ui.agent_panel import AgentPanel
from ui.god_mode import GodMode
from ui.save_load import SaveLoad
from llm.openrouter_client import OpenRouterClient
from llm.decision_router import DecisionRouter
from utils.logger import setup_logging


def main() -> int:
    load_dotenv()
    setup_logging()
    log = logging.getLogger("emergent_city")

    if config.RANDOM_SEED is not None:
        random.seed(config.RANDOM_SEED)
        log.info("Seeded RNG with %d", config.RANDOM_SEED)

    pygame.init()
    pygame.display.set_caption(config.WINDOW_TITLE)
    screen = pygame.display.set_mode(
        (config.WINDOW_WIDTH, config.WINDOW_HEIGHT),
        pygame.DOUBLEBUF | pygame.RESIZABLE,
    )
    clock = pygame.time.Clock()

    # ── LLM
    api_key = os.getenv("OPENROUTER_API_KEY", "").strip()
    llm_client: Optional[OpenRouterClient] = None
    if api_key:
        llm_client = OpenRouterClient(
            api_key=api_key,
            model=os.getenv("OPENROUTER_MODEL", "google/gemini-2.5-flash"),
            site_url=os.getenv("OPENROUTER_SITE_URL", ""),
            app_name=os.getenv("OPENROUTER_APP_NAME", "EmergentCity"),
        )
        log.info("LLM enabled (model=%s)", llm_client.model)
    else:
        log.warning("No OPENROUTER_API_KEY set — running with rule-based fallback only")

    decision_router = DecisionRouter(client=llm_client)

    # ── World
    world = World(
        width=config.WORLD_WIDTH_TILES,
        height=config.WORLD_HEIGHT_TILES,
        decision_router=decision_router,
    )
    world.populate(config.INITIAL_POPULATION)

    # ── Rendering & UI
    camera = Camera(
        viewport_w=config.WINDOW_WIDTH,
        viewport_h=config.WINDOW_HEIGHT,
        world_w_tiles=world.width,
        world_h_tiles=world.height,
    )
    camera.center_on(world.width / 2, world.height / 2)

    renderer = Renderer(screen, world, camera)
    hud = HUD(screen, world)
    feed = EventFeed(screen, world)
    panel = AgentPanel(screen, world, camera)
    god = GodMode(world)
    saver = SaveLoad(world)

    # ── State
    running = True
    speed_index = 0  # 0 -> 1x, 1 -> 5x, 2 -> 20x
    paused = False
    followed_agent_id: Optional[int] = None

    log.info(
        "World ready: %dx%d tiles, %d citizens. Running.",
        world.width, world.height, len(world.agents),
    )

    while running:
        dt = clock.tick(config.TARGET_FPS) / 1000.0  # seconds since last frame

        # ── Events
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False

            elif event.type == pygame.VIDEORESIZE:
                w, h = event.size
                cw, ch = screen.get_size()
                # Ignore tiny OS decoration drift: WSLg nudges the size by a few
                # pixels each frame, which otherwise becomes a resize feedback
                # loop. Honor only real (user) resizes, then rebuild the display
                # and repoint every widget at the new surface so drawing follows.
                if abs(w - cw) > 24 or abs(h - ch) > 24:
                    screen = pygame.display.set_mode((w, h), pygame.DOUBLEBUF | pygame.RESIZABLE)
                    nw, nh = screen.get_size()
                    camera.resize(nw, nh)
                    for widget in (renderer, hud, feed, panel):
                        widget.screen = screen

            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_SPACE:
                    paused = not paused
                elif pygame.K_1 <= event.key <= pygame.K_9:
                    num = event.key - pygame.K_1  # 0..8
                    if god.active:
                        god.select_tool(num)       # pick a God Mode tool (1..9)
                    elif num < 3:
                        speed_index = num          # 1/2/3 = speed when not in God Mode
                elif event.key == pygame.K_f:
                    followed_agent_id = panel.selected_agent_id
                elif event.key == pygame.K_g:
                    god.toggle()
                elif event.key == pygame.K_TAB:
                    feed.toggle_visible()
                elif event.key == pygame.K_ESCAPE:
                    panel.close()
                    god.close_editor()
                elif event.key == pygame.K_s and (pygame.key.get_mods() & pygame.KMOD_CTRL):
                    saver.save()
                elif event.key == pygame.K_l and (pygame.key.get_mods() & pygame.KMOD_CTRL):
                    saver.load()

            elif event.type == pygame.MOUSEBUTTONDOWN:
                if event.button == 1:
                    if god.active:
                        god.on_mouse_down(event.pos, camera)
                    else:
                        panel.handle_click(event.pos)
                elif event.button == 4:
                    camera.zoom_at(event.pos, +0.1)
                elif event.button == 5:
                    camera.zoom_at(event.pos, -0.1)

            elif event.type == pygame.MOUSEBUTTONUP:
                if event.button == 1 and god.active:
                    god.on_mouse_up()

            elif event.type == pygame.MOUSEMOTION:
                if god.active:
                    god.on_mouse_motion(event.pos)
                elif event.buttons[0] and not panel.is_open:
                    camera.pan(-event.rel[0], -event.rel[1])

        # ── Camera follow
        if followed_agent_id is not None:
            ag = world.get_agent(followed_agent_id)
            if ag is not None:
                camera.center_on(ag.x, ag.y)

        # ── Simulation tick
        if not paused:
            speed = config.SPEED_MULTIPLIERS[speed_index]
            world.tick(dt * speed)

        # ── Render
        renderer.draw()
        hud.draw(
            paused=paused,
            speed=config.SPEED_MULTIPLIERS[speed_index],
            sim_time=world.time_system,
            population=len(world.agents),
            llm_calls=llm_client.total_calls if llm_client else 0,
        )
        feed.draw()
        panel.draw()
        god.draw_indicator(screen)

        pygame.display.flip()

    pygame.quit()
    return 0


if __name__ == "__main__":
    sys.exit(main())
